package kestrodb

import (
	"bufio"
	"context"
	"crypto/tls"
	"crypto/x509"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"time"
)

// Version is this client's version (the KestroDB release it was written for).
const Version = "0.9.0"

// MaxFrame is the largest frame either way (64 MB), as on the server.
const MaxFrame = 64 * 1024 * 1024

// MaxValue is the largest JSON value of a key (256 KiB).
const MaxValue = 256 * 1024

// DefaultAddress is used when Connect is given an empty address.
const DefaultAddress = "tcp://127.0.0.1:7820"

// Options are a client's settings; the zero value is the defaults.
type Options struct {
	// Token is the access token (from the server's --token-file or --users); empty for none.
	Token string

	// Timeout limits each reply (default 30 s; negative: none). It is also the connect
	// timeout unless ConnectTimeout is set. A context deadline that comes first wins.
	Timeout time.Duration

	// ConnectTimeout limits opening a connection, TLS handshake included (default: Timeout).
	ConnectTimeout time.Duration

	// DisableReconnect turns off reopening a connection the server closed while it was idle
	// (by default it is reopened before the next request, never inside a transaction).
	DisableReconnect bool

	// FailoverTimeout is how long a cluster client keeps looking for the leader when a
	// member answers READONLY or cannot be reached (default 15 s; negative: no retries).
	FailoverTimeout time.Duration

	// TLS (tls://host:port addresses). The server certificate is verified against the
	// system pool unless CAFile or TLSConfig.RootCAs names other CAs.

	// TLSConfig is the base TLS configuration (cloned; client certificates, RootCAs, ...).
	TLSConfig *tls.Config
	// CAFile is a PEM file of the CA certificates to trust instead of the system pool.
	CAFile string
	// ServerName is the name to send (SNI) and verify, instead of the address's host.
	ServerName string
	// InsecureSkipVerify skips certificate and host name checks (testing only).
	InsecureSkipVerify bool
}

// address is one parsed member address.
type address struct {
	text    string // as written in the list
	network string // "tcp" or "unix"
	dial    string // host:port or the socket path
	host    string
	tls     bool
}

func parseAddress(text string) (*address, error) {
	if strings.HasPrefix(text, "unix://") {
		path := text[len("unix://"):]
		if path == "" {
			return nil, inputError("address must be tcp://host:port, tls://host:port or unix:///path")
		}
		return &address{text: text, network: "unix", dial: path}, nil
	}
	a := &address{text: text, network: "tcp"}
	hp := text
	switch {
	case strings.HasPrefix(text, "tcp://"):
		hp = text[6:]
	case strings.HasPrefix(text, "tls://"):
		hp, a.tls = text[6:], true
	}
	colon := strings.LastIndexByte(hp, ':')
	if colon < 0 {
		return nil, inputError("address must be tcp://host:port, tls://host:port or unix:///path: " + text)
	}
	host := hp[:colon]
	if strings.HasPrefix(host, "[") && strings.HasSuffix(host, "]") {
		host = host[1 : len(host)-1]
	}
	if host == "" {
		host = "127.0.0.1"
	}
	port, err := strconv.Atoi(hp[colon+1:])
	if err != nil || port < 1 || port > 65535 {
		return nil, inputError("invalid port in address " + text)
	}
	a.host = host
	a.dial = net.JoinHostPort(host, strconv.Itoa(port))
	return a, nil
}

// key is the same member however it is written.
func (a *address) key() string {
	if a.network == "unix" {
		return "unix://" + a.dial
	}
	scheme := "tcp://"
	if a.tls {
		scheme = "tls://"
	}
	return scheme + strings.ToLower(a.dial)
}

// splitAddresses splits a comma-separated list, dropping empty entries.
func splitAddresses(list string) []string {
	var out []string
	for _, a := range strings.Split(list, ",") {
		if a = strings.TrimSpace(a); a != "" {
			out = append(out, a)
		}
	}
	return out
}

// Client is a connection to kestro-server (or to the members of a cluster).
//
// A Client is safe for concurrent use: calls are serialized on its one connection. A
// transaction from Begin holds the client until Commit or Rollback, so other goroutines'
// calls wait for it (use the Tx it returns for the statements inside it). For parallel
// work, open one Client per goroutine, or use database/sql (driver "kestrodb") for a pool.
type Client struct {
	querier

	addresses []*address
	token     string
	opts      Options
	timeout   time.Duration // per reply; 0: none
	connectTO time.Duration
	failover  time.Duration
	reconnect bool
	tlsBase   *tls.Config
	tlsOnce   sync.Once
	tlsErr    error

	gate chan struct{} // held by each Client call, and by a Tx for its whole life
	sem  chan struct{} // held by each call of a Tx

	// Guarded by gate/sem (one call at a time).
	current      *address
	cn           *conn
	closed       bool
	lost         bool // the connection broke (as opposed to a move to another member)
	lastUse      time.Time
	inTx         bool
	implicitSave string // a SAVEPOINT that began the transaction: releasing it commits
	lastInsertID any
	opens        int // connections opened (tests)

	stateMu sync.Mutex // guards openTx and the copies read by the accessors below
	openTx  *Tx        // the Tx holding the gate, if any
	addrNow string
	txNow   bool
	idNow   any
}

// Connect opens a client. address is tcp://host:port, tls://host:port, unix:///path or
// host:port; several comma-separated addresses name a cluster's members (any order):
// writes go to the leader, which a follower's READONLY reply names, and the client moves
// there and sends the request again (nothing was applied), also during an election, for
// up to Options.FailoverTimeout. opts may be nil.
func Connect(ctx context.Context, address string, opts *Options) (*Client, error) {
	if strings.TrimSpace(address) == "" {
		address = DefaultAddress
	}
	return ConnectCluster(ctx, splitAddresses(address), opts)
}

// ConnectCluster opens a client to a cluster given as a list of member addresses.
func ConnectCluster(ctx context.Context, addresses []string, opts *Options) (*Client, error) {
	c, err := newClient(addresses, opts)
	if err != nil {
		return nil, err
	}
	if ctx == nil {
		ctx = context.Background()
	}
	if err := c.open(ctx); err != nil {
		return nil, err
	}
	return c, nil
}

func newClient(list []string, opts *Options) (*Client, error) {
	c := &Client{gate: make(chan struct{}, 1), sem: make(chan struct{}, 1)}
	c.querier = querier{c: c}
	if opts != nil {
		c.opts = *opts
	}
	for _, t := range list {
		if t = strings.TrimSpace(t); t == "" {
			continue
		}
		a, err := parseAddress(t)
		if err != nil {
			return nil, err
		}
		c.addresses = append(c.addresses, a)
	}
	if len(c.addresses) == 0 {
		return nil, inputError("no server address")
	}
	c.token = c.opts.Token
	switch {
	case c.opts.Timeout == 0:
		c.timeout = 30 * time.Second
	case c.opts.Timeout > 0:
		c.timeout = c.opts.Timeout
	}
	c.connectTO = c.timeout
	if c.opts.ConnectTimeout > 0 {
		c.connectTO = c.opts.ConnectTimeout
	}
	switch {
	case c.opts.FailoverTimeout == 0:
		c.failover = 15 * time.Second
	case c.opts.FailoverTimeout > 0:
		c.failover = c.opts.FailoverTimeout
	}
	c.reconnect = !c.opts.DisableReconnect
	c.current = c.addresses[0]
	c.publish()
	return c, nil
}

// Address is the member this client uses now, as written in the address list.
func (c *Client) Address() string {
	c.stateMu.Lock()
	defer c.stateMu.Unlock()
	return c.addrNow
}

// InTransaction reports whether a transaction is open: between BEGIN (or START
// TRANSACTION, or a SAVEPOINT outside one) and its COMMIT or ROLLBACK.
func (c *Client) InTransaction() bool {
	c.stateMu.Lock()
	defer c.stateMu.Unlock()
	return c.txNow
}

// LastInsertID is the id of the last row an INSERT on this connection created: int64,
// or string for text ids; nil before any.
func (c *Client) LastInsertID() any {
	c.stateMu.Lock()
	defer c.stateMu.Unlock()
	return c.idNow
}

// publish copies the state the accessors read (called with the call lock held).
func (c *Client) publish() {
	c.stateMu.Lock()
	c.addrNow, c.txNow, c.idNow = c.current.text, c.inTx, c.lastInsertID
	c.stateMu.Unlock()
}

// ---------------------------------------------------------------------- locking

// acquire takes the gate (a Client call) or the sem (a call inside a Tx).
func (c *Client) acquire(ctx context.Context, inTx bool) error {
	ch := c.gate
	if inTx {
		ch = c.sem
	}
	select {
	case ch <- struct{}{}:
		return nil
	default:
	}
	if ctx.Done() == nil {
		ch <- struct{}{}
		return nil
	}
	select {
	case ch <- struct{}{}:
		return nil
	case <-ctx.Done():
		return ioError("gave up waiting for the connection: "+ctx.Err().Error(), ctx.Err())
	}
}

func (c *Client) release(inTx bool) {
	c.publish()
	if inTx {
		<-c.sem
	} else {
		<-c.gate
	}
}

// ---------------------------------------------------------------------- connections

// conn is one open connection.
type conn struct {
	nc    net.Conn // what requests go through (a *tls.Conn for TLS)
	raw   net.Conn // the socket under it
	r     *bufio.Reader
	isTLS bool
	wbuf  []byte
}

func (cn *conn) close() { _ = cn.nc.Close() }

// open connects to the current member first, then the others.
func (c *Client) open(ctx context.Context) error {
	order := []*address{c.current}
	for _, a := range c.addresses {
		if a != c.current {
			order = append(order, a)
		}
	}
	var last error
	for _, a := range order {
		cn, err := c.openOne(ctx, a)
		if err == nil {
			c.cn, c.current, c.lost, c.lastUse = cn, a, false, time.Now()
			c.opens++
			return nil
		}
		if kindIs(err, "AUTH") || ctx.Err() != nil {
			return err
		}
		last = err
	}
	return last
}

func describe(err error) string {
	var op *net.OpError
	if errors.As(err, &op) && op.Err != nil {
		return op.Err.Error()
	}
	return err.Error()
}

func (c *Client) openOne(ctx context.Context, a *address) (*conn, error) {
	cannot := func(err error, extra string) error {
		if ctx.Err() != nil {
			err = ctx.Err()
		}
		return ioError("cannot connect to "+a.text+": "+describe(err)+extra, err)
	}
	dctx := ctx
	if c.connectTO > 0 {
		var cancel context.CancelFunc
		dctx, cancel = context.WithTimeout(ctx, c.connectTO)
		defer cancel()
	}
	d := net.Dialer{KeepAlive: 30 * time.Second}
	raw, err := d.DialContext(dctx, a.network, a.dial)
	if err != nil {
		return nil, cannot(err, "")
	}
	cn := &conn{nc: raw, raw: raw}
	if a.tls {
		cfg, err := c.tlsConfig(a)
		if err != nil {
			raw.Close()
			return nil, err
		}
		t := tls.Client(raw, cfg)
		if err := t.HandshakeContext(dctx); err != nil {
			raw.Close()
			return nil, cannot(fmt.Errorf("TLS handshake failed: %w", err), "")
		}
		cn.nc, cn.isTLS = t, true
	}
	cn.r = bufio.NewReaderSize(cn.nc, 64*1024)
	if c.token != "" {
		// BUSY already here (a server at its limit answers before reading): report it.
		if !cn.isTLS && peekRaw(cn.raw) == peekData {
			if early := cn.unsolicited(); early != nil && !kindIs(early, "IO") {
				cn.close()
				return nil, early
			}
			cn.close()
			return nil, cannot(errors.New("the server closed the connection before login"), "")
		}
		_, serverErr, err := cn.exchange(dctx, 'A', []byte(c.token), c.connectTO)
		if err != nil {
			cn.close()
			// A server at its connection limit sends BUSY and closes at once; the reset that
			// follows can discard the BUSY reply before it is read.
			return nil, cannot(err, " (the server closed the connection before login; it may be at its connection limit)")
		}
		if serverErr != nil {
			cn.close()
			return nil, serverErr
		}
	}
	return cn, nil
}

func (c *Client) tlsConfig(a *address) (*tls.Config, error) {
	c.tlsOnce.Do(func() {
		var cfg *tls.Config
		if c.opts.TLSConfig != nil {
			cfg = c.opts.TLSConfig.Clone()
		} else {
			cfg = &tls.Config{}
		}
		if cfg.MinVersion == 0 {
			cfg.MinVersion = tls.VersionTLS12
		}
		if c.opts.CAFile != "" {
			pem, err := os.ReadFile(c.opts.CAFile)
			if err != nil {
				c.tlsErr = ioError("TLS CA file: "+err.Error(), err)
				return
			}
			pool := x509.NewCertPool()
			if !pool.AppendCertsFromPEM(pem) {
				c.tlsErr = inputError("no certificate in " + c.opts.CAFile)
				return
			}
			cfg.RootCAs = pool
		}
		if c.opts.InsecureSkipVerify {
			cfg.InsecureSkipVerify = true
		}
		if c.opts.ServerName != "" {
			cfg.ServerName = c.opts.ServerName
		}
		c.tlsBase = cfg
	})
	if c.tlsErr != nil {
		return nil, c.tlsErr
	}
	cfg := c.tlsBase.Clone()
	if cfg.ServerName == "" {
		cfg.ServerName = a.host // Go sends no SNI for an IP address and checks its IP SANs
	}
	return cfg, nil
}

func (c *Client) drop() {
	if c.cn != nil {
		c.cn.close()
		c.cn = nil
	}
}

// ---------------------------------------------------------------------- frames

var longAgo = time.Unix(1, 0)

// exchange sends one request and reads its reply. serverErr is an error reply (the
// connection stays usable); err is a transport failure (the connection is not).
func (cn *conn) exchange(ctx context.Context, cmd byte, payload []byte, timeout time.Duration) (reply []byte, serverErr *Error, err error) {
	deadline := time.Time{}
	if timeout > 0 {
		deadline = time.Now().Add(timeout)
	}
	if d, ok := ctx.Deadline(); ok && (deadline.IsZero() || d.Before(deadline)) {
		deadline = d
	}
	_ = cn.nc.SetDeadline(deadline)
	if ctx.Done() != nil {
		stop := context.AfterFunc(ctx, func() { _ = cn.nc.SetDeadline(longAgo) })
		defer stop()
	}
	n := 5 + len(payload)
	if n <= 64*1024 {
		if cap(cn.wbuf) < n {
			cn.wbuf = make([]byte, n, 64*1024)
		}
		b := cn.wbuf[:n]
		binary.LittleEndian.PutUint32(b, uint32(1+len(payload)))
		b[4] = cmd
		copy(b[5:], payload)
		_, err = cn.nc.Write(b)
	} else {
		var h [5]byte
		binary.LittleEndian.PutUint32(h[:], uint32(1+len(payload)))
		h[4] = cmd
		bufs := net.Buffers{h[:], payload}
		_, err = bufs.WriteTo(cn.nc)
	}
	if err == nil {
		reply, err = cn.readFrame(MaxFrame + 4096)
	}
	if err != nil {
		if ctx.Err() != nil {
			err = ctx.Err()
		} else if d, ok := ctx.Deadline(); ok && !time.Now().Before(d) {
			err = context.DeadlineExceeded // the socket's deadline fired first
		}
		return nil, nil, err
	}
	if reply[0] == 255 {
		return nil, newError(string(reply[1:])), nil
	}
	return reply, nil, nil
}

func (cn *conn) readFrame(max uint32) ([]byte, error) {
	var h [4]byte
	if _, err := io.ReadFull(cn.r, h[:]); err != nil {
		if err == io.EOF {
			err = errors.New("connection closed")
		}
		return nil, err
	}
	length := binary.LittleEndian.Uint32(h[:])
	if length == 0 || length > max {
		return nil, fmt.Errorf("invalid response length %d", length)
	}
	reply := make([]byte, length)
	if _, err := io.ReadFull(cn.r, reply); err != nil {
		if err == io.EOF || err == io.ErrUnexpectedEOF {
			err = errors.New("connection closed")
		}
		return nil, err
	}
	return reply, nil
}

// unsolicited reads what the server sent without a request (BUSY before closing a
// connection it will not serve). The connection is unusable afterwards.
func (cn *conn) unsolicited() error {
	_ = cn.nc.SetReadDeadline(time.Now().Add(200 * time.Millisecond))
	frame, err := cn.readFrame(64 * 1024)
	if err != nil {
		return ioError("connection lost", err)
	}
	if frame[0] != 255 {
		return ioError("the server sent bytes outside a reply", nil)
	}
	return newError(string(frame[1:]))
}

// probe checks an idle connection before a request: alive false when the server closed it
// (early is then what it said, if anything). thorough also waits a moment for a reply
// where a free check is not possible (TLS, Windows); it is used after a second idle.
func (cn *conn) probe(thorough bool) (early error, alive bool) {
	if cn.r.Buffered() > 0 {
		return cn.unsolicited(), false
	}
	switch peekRaw(cn.raw) {
	case peekClosed:
		return nil, false
	case peekEmpty:
		return nil, true
	case peekData:
		if !cn.isTLS {
			return cn.unsolicited(), false
		}
		// TLS: a close_notify, or a record that is not a reply (a session ticket).
	}
	if !thorough {
		return nil, true
	}
	_ = cn.nc.SetReadDeadline(time.Now().Add(time.Millisecond))
	_, err := cn.r.Peek(1)
	if err == nil {
		return cn.unsolicited(), false
	}
	var ne net.Error
	if errors.As(err, &ne) && ne.Timeout() {
		return nil, true
	}
	return nil, false
}

// ---------------------------------------------------------------------- calls

var leaderPattern = regexp.MustCompile(`the leader is (\S+)`)

// call sends one request, following a cluster's leader. The caller holds the lock.
func (c *Client) call(ctx context.Context, cmd byte, payload []byte) ([]byte, error) {
	if len(c.addresses) < 2 || cmd == 'A' || cmd == 'Q' {
		return c.callOnce(ctx, cmd, payload)
	}
	deadline := time.Now().Add(c.failover)
	wait := 50 * time.Millisecond
	for {
		reply, err := c.callOnce(ctx, cmd, payload)
		if err == nil {
			return reply, nil
		}
		var e *Error
		if !errors.As(err, &e) {
			return nil, err
		}
		moved := e.Kind == "READONLY"
		unreachable := strings.HasPrefix(e.Message, "IO cannot connect")
		if (!moved && !unreachable) || c.inTx || c.closed || ctx.Err() != nil || time.Now().After(deadline) {
			return nil, err
		}
		var target *address
		if m := leaderPattern.FindStringSubmatch(e.Message); m != nil {
			target = c.member(m[1])
		}
		if target == nil {
			t := time.NewTimer(wait)
			select {
			case <-t.C:
			case <-ctx.Done():
				t.Stop()
				return nil, err
			}
			wait = min(wait*2, 500*time.Millisecond)
			next := 0
			for i, a := range c.addresses {
				if a == c.current {
					next = (i + 1) % len(c.addresses)
				}
			}
			target = c.addresses[next]
		}
		if target != c.current || c.cn == nil {
			c.drop()
			c.lost = false // a move, not a failure: connect even with reconnect off
			c.current = target
		}
	}
}

// member is the listed member a server named (as written in its --cluster list), or nil.
func (c *Client) member(name string) *address {
	for _, a := range c.addresses {
		if a.text == name {
			return a
		}
	}
	if n, err := parseAddress(name); err == nil {
		for _, a := range c.addresses {
			if a.key() == n.key() {
				return a
			}
		}
	}
	return nil
}

func (c *Client) callOnce(ctx context.Context, cmd byte, payload []byte) ([]byte, error) {
	if c.closed {
		return nil, closedError()
	}
	if err := ctx.Err(); err != nil {
		return nil, ioError("request not sent: "+err.Error(), err)
	}
	if len(payload) > MaxFrame-1 {
		return nil, limitError("request too large")
	}
	if c.cn != nil && cmd != 'A' {
		thorough := c.reconnect && !c.inTx && time.Since(c.lastUse) > time.Second
		early, alive := c.cn.probe(thorough)
		if !alive {
			c.drop()
			c.lost = true
			if early != nil && kindIs(early, "BUSY") && !c.inTx {
				return nil, early
			}
		}
	}
	if c.cn == nil {
		if c.inTx {
			c.inTx, c.implicitSave = false, ""
			return nil, ioError("connection lost inside a transaction; it was rolled back", nil)
		}
		if cmd == 'Q' {
			return []byte{0}, nil
		}
		if c.lost && !c.reconnect {
			return nil, ioError("the connection was lost (reconnect is off)", nil)
		}
		if err := c.open(ctx); err != nil {
			return nil, err
		}
	}
	reply, serverErr, err := c.cn.exchange(ctx, cmd, payload, c.timeout)
	c.lastUse = time.Now()
	if err != nil {
		c.drop()
		c.lost = true
		tx := c.inTx
		c.inTx, c.implicitSave = false, ""
		if tx {
			return nil, ioError("connection lost inside a transaction; it was rolled back ("+describe(err)+")", err)
		}
		return nil, ioError("connection lost; a write without a reply has an unknown outcome ("+describe(err)+")", err)
	}
	if serverErr != nil {
		return nil, serverErr
	}
	return reply, nil
}

// do runs one request under the lock.
func (c *Client) do(ctx context.Context, inTx bool, cmd byte, payload []byte) ([]byte, error) {
	if ctx == nil {
		ctx = context.Background()
	}
	if err := c.acquire(ctx, inTx); err != nil {
		return nil, err
	}
	defer c.release(inTx)
	return c.call(ctx, cmd, payload)
}

// ---------------------------------------------------------------------- server

// Stats returns the server's statistics: keys, bytes, txid, connections, tls, user, role,
// cluster, ...
func (c *Client) Stats(ctx context.Context) (map[string]any, error) {
	return c.jsonObject(c.do(ctx, false, 'S', nil))
}

func (c *Client) jsonObject(reply []byte, err error) (map[string]any, error) {
	if err != nil {
		return nil, err
	}
	v, err := parseJSON(reply[1:])
	if err != nil {
		return nil, ioError("the server's reply is not JSON: "+err.Error(), err)
	}
	m, ok := v.(map[string]any)
	if !ok {
		return nil, ioError("the server's reply is not a JSON object", nil)
	}
	return m, nil
}

// Verify checks every checksum of the database file (admin).
func (c *Client) Verify(ctx context.Context) error {
	_, err := c.do(ctx, false, 'V', nil)
	return err
}

// Backup writes an online backup as a plain file name in the server's --backup-dir (admin).
func (c *Client) Backup(ctx context.Context, name string) error {
	if name == "" {
		return inputError("a backup name is a plain file name")
	}
	_, err := c.do(ctx, false, 'P', []byte(name))
	return err
}

// CompactTo writes a compacted copy as a plain file name in the server's --backup-dir (admin).
func (c *Client) CompactTo(ctx context.Context, name string) error {
	if name == "" {
		return inputError("a backup name is a plain file name")
	}
	_, err := c.do(ctx, false, 'C', []byte(name))
	return err
}

// Compact compacts the server's file online (admin): {before, after, keys}.
func (c *Client) Compact(ctx context.Context) (map[string]any, error) {
	return c.jsonObject(c.do(ctx, false, 'K', nil))
}

// Promote turns a replica into a writable server (admin); it stops following its leader.
func (c *Client) Promote(ctx context.Context) error {
	_, err := c.do(ctx, false, 'X', nil)
	return err
}

// Close ends the connection. It waits for a call in progress; a Tx still open is ended
// (the server rolls it back) and its later calls fail with ErrClosed. Closing twice is fine.
func (c *Client) Close() error {
	for {
		select {
		case c.gate <- struct{}{}:
			c.closeLocked()
			c.publish()
			<-c.gate
			return nil
		default:
		}
		c.stateMu.Lock()
		tx := c.openTx
		c.stateMu.Unlock()
		if tx != nil {
			c.sem <- struct{}{} // between the Tx's calls
			c.stateMu.Lock()
			still := c.openTx == tx
			if still {
				c.openTx = nil
			}
			c.stateMu.Unlock()
			if still {
				tx.done = true
				c.closeLocked()
				c.publish()
				<-c.sem
				<-c.gate // the Tx's hold
				return nil
			}
			<-c.sem
		}
		time.Sleep(time.Millisecond) // a call is running
	}
}

func (c *Client) closeLocked() {
	if c.closed {
		return
	}
	if c.cn != nil {
		_, _, _ = c.cn.exchange(context.Background(), 'Q', nil, 2*time.Second)
		c.drop()
	}
	c.closed = true
	c.inTx, c.implicitSave = false, ""
}

func typeName(v any) string { return fmt.Sprintf("%T", v) }

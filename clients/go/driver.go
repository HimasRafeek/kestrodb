package kestrodb

import (
	"context"
	"database/sql"
	"database/sql/driver"
	"errors"
	"io"
	"net/url"
	"strconv"
	"strings"
	"time"
)

// The database/sql driver, registered as "kestrodb":
//
//	db, err := sql.Open("kestrodb", "kestrodb://127.0.0.1:7820?token="+url.QueryEscape(token))
//
// DSN forms: kestrodb://host:port[,host:port...][?options] (TCP), kestrodb+tls://...
// (TLS; or ?tls=true), kestrodb+unix:///path/to/socket, or an address as Connect takes it
// (tcp://, tls://, unix://) with optional ?options. A token may also be the URL's
// password (kestrodb://:TOKEN@host:port). Options: token, timeout, connectTimeout,
// failoverTimeout (Go durations like 5s, or seconds), reconnect=false, tls=true, caFile,
// serverName, insecureSkipVerify=true.
//
// Each pooled connection is one Client. sql.Named arguments fill :name placeholders.
// JSON arrays and objects come back as their JSON text. Transactions map to BEGIN, and
// statements outside one commit alone.

func init() { sql.Register("kestrodb", &Driver{}) }

// Driver is the database/sql driver.
type Driver struct{}

// Open opens a connection from a DSN (database/sql uses OpenConnector).
func (d *Driver) Open(dsn string) (driver.Conn, error) {
	c, err := d.OpenConnector(dsn)
	if err != nil {
		return nil, err
	}
	return c.Connect(context.Background())
}

// OpenConnector parses the DSN once for every connection of a pool.
func (d *Driver) OpenConnector(dsn string) (driver.Connector, error) {
	addresses, opts, err := ParseDSN(dsn)
	if err != nil {
		return nil, err
	}
	return &connector{addresses: addresses, opts: opts, driver: d}, nil
}

// NewConnector is a database/sql connector with Options (a TLS config, for example):
//
//	db := sql.OpenDB(kestrodb.NewConnector("tls://db.example:7820", &kestrodb.Options{Token: t}))
func NewConnector(address string, opts *Options) driver.Connector {
	c := &connector{addresses: splitAddresses(address), driver: &Driver{}}
	if opts != nil {
		c.opts = *opts
	}
	return c
}

// ParseDSN splits a DSN into member addresses and options.
func ParseDSN(dsn string) (addresses []string, opts Options, err error) {
	rest, query := dsn, ""
	if i := strings.IndexByte(rest, '?'); i >= 0 {
		rest, query = rest[:i], rest[i+1:]
	}
	scheme := "tcp://"
	switch {
	case strings.HasPrefix(rest, "kestrodb+tls://"):
		rest, scheme = rest[len("kestrodb+tls://"):], "tls://"
	case strings.HasPrefix(rest, "kestrodb+unix://"):
		rest, scheme = rest[len("kestrodb+unix://"):], "unix://"
	case strings.HasPrefix(rest, "kestrodb://"):
		rest = rest[len("kestrodb://"):]
	case strings.HasPrefix(rest, "kestrodb:"):
		rest, scheme = rest[len("kestrodb:"):], "" // kestrodb:tls://host:port
	default:
		scheme = ""
	}
	if scheme != "unix://" {
		if at := strings.LastIndexByte(rest, '@'); at >= 0 {
			user := rest[:at]
			rest = rest[at+1:]
			if i := strings.IndexByte(user, ':'); i >= 0 {
				tok, e := url.PathUnescape(user[i+1:])
				if e != nil {
					return nil, opts, inputError("DSN: invalid token in the user information")
				}
				opts.Token = tok
			}
		}
		rest = strings.TrimSuffix(rest, "/")
	}
	values, e := url.ParseQuery(query)
	if e != nil {
		return nil, opts, inputError("DSN: " + e.Error())
	}
	useTLS := false
	for k, vs := range values {
		v := vs[len(vs)-1]
		var perr error
		switch k {
		case "token", "password":
			opts.Token = v
		case "timeout":
			opts.Timeout, perr = dsnDuration(v)
		case "connectTimeout":
			opts.ConnectTimeout, perr = dsnDuration(v)
		case "failoverTimeout":
			opts.FailoverTimeout, perr = dsnDuration(v)
		case "reconnect":
			var b bool
			b, perr = strconv.ParseBool(v)
			opts.DisableReconnect = !b
		case "tls":
			useTLS, perr = strconv.ParseBool(v)
		case "caFile", "cafile":
			opts.CAFile, useTLS = v, true
		case "serverName":
			opts.ServerName = v
		case "insecureSkipVerify":
			opts.InsecureSkipVerify, perr = strconv.ParseBool(v)
		case "verify":
			var b bool
			b, perr = strconv.ParseBool(v)
			opts.InsecureSkipVerify = !b
		case "user":
			// ignored: the token selects the user
		default:
			return nil, opts, inputError("DSN: unknown option " + k)
		}
		if perr != nil {
			return nil, opts, inputError("DSN: invalid " + k + ": " + v)
		}
	}
	if scheme == "unix://" {
		addresses = []string{"unix://" + rest}
	} else {
		for _, a := range splitAddresses(rest) {
			if !strings.Contains(a, "://") {
				if useTLS || scheme == "tls://" {
					a = "tls://" + a
				} else {
					a = "tcp://" + a
				}
			} else if useTLS && strings.HasPrefix(a, "tcp://") {
				a = "tls://" + a[len("tcp://"):]
			}
			addresses = append(addresses, a)
		}
	}
	if len(addresses) == 0 {
		return nil, opts, inputError("DSN: no server address")
	}
	for _, a := range addresses {
		if _, err := parseAddress(a); err != nil {
			return nil, opts, err
		}
	}
	return addresses, opts, nil
}

func dsnDuration(v string) (time.Duration, error) {
	if f, err := strconv.ParseFloat(v, 64); err == nil {
		return time.Duration(f * float64(time.Second)), nil
	}
	return time.ParseDuration(v)
}

type connector struct {
	addresses []string
	opts      Options
	driver    *Driver
}

func (c *connector) Connect(ctx context.Context) (driver.Conn, error) {
	cl, err := ConnectCluster(ctx, c.addresses, &c.opts)
	if err != nil {
		return nil, err
	}
	return &sqlConn{c: cl}, nil
}

func (c *connector) Driver() driver.Driver { return c.driver }

// sqlConn is one pooled connection.
type sqlConn struct {
	c  *Client
	tx *Tx // the open database/sql transaction, if any
}

var (
	_ driver.Conn               = (*sqlConn)(nil)
	_ driver.ConnBeginTx        = (*sqlConn)(nil)
	_ driver.ConnPrepareContext = (*sqlConn)(nil)
	_ driver.ExecerContext      = (*sqlConn)(nil)
	_ driver.QueryerContext     = (*sqlConn)(nil)
	_ driver.Pinger             = (*sqlConn)(nil)
	_ driver.SessionResetter    = (*sqlConn)(nil)
	_ driver.Validator          = (*sqlConn)(nil)
	_ driver.NamedValueChecker  = (*sqlConn)(nil)
)

// Raw returns the Client under a database/sql connection (through sql.Conn.Raw):
//
//	conn.Raw(func(dc any) error { client := kestrodb.Raw(dc); ... })
func Raw(driverConn any) *Client {
	if s, ok := driverConn.(*sqlConn); ok {
		return s.c
	}
	return nil
}

func (s *sqlConn) q() querier {
	if s.tx != nil {
		return s.tx.querier
	}
	return s.c.querier
}

func mapErr(err error) error {
	if err != nil && kindIs(err, "CLOSED") {
		return driver.ErrBadConn
	}
	return err
}

func (s *sqlConn) Prepare(query string) (driver.Stmt, error) {
	return s.PrepareContext(context.Background(), query)
}

func (s *sqlConn) PrepareContext(_ context.Context, query string) (driver.Stmt, error) {
	return &sqlStmt{conn: s, query: query}, nil
}

func (s *sqlConn) Close() error {
	if s.tx != nil {
		_ = s.tx.Rollback(context.Background())
		s.tx = nil
	}
	return s.c.Close()
}

func (s *sqlConn) Begin() (driver.Tx, error) {
	return s.BeginTx(context.Background(), driver.TxOptions{})
}

func (s *sqlConn) BeginTx(ctx context.Context, opts driver.TxOptions) (driver.Tx, error) {
	switch sql.IsolationLevel(opts.Isolation) {
	case sql.LevelDefault, sql.LevelSerializable:
	default:
		return nil, inputError("isolation level " + sql.IsolationLevel(opts.Isolation).String() + " is not supported (transactions are serializable)")
	}
	tx, err := s.c.BeginTx(ctx, nil)
	if err != nil {
		return nil, mapErr(err)
	}
	s.tx = tx
	return &sqlTx{conn: s}, nil
}

// args are the parameters as SQL takes them (mixed() has checked they are all named or
// all positional).
func args(named []driver.NamedValue) []any {
	if len(named) == 0 {
		return nil
	}
	if named[0].Name != "" {
		m := make(Named, len(named))
		for _, nv := range named {
			m[nv.Name] = nv.Value
		}
		return []any{m}
	}
	out := make([]any, len(named))
	for i, nv := range named {
		out[i] = nv.Value
	}
	return out
}

func (s *sqlConn) ExecContext(ctx context.Context, query string, named []driver.NamedValue) (driver.Result, error) {
	if err := mixed(named); err != nil {
		return nil, err
	}
	r, err := s.q().SQL(ctx, query, args(named)...)
	if err != nil {
		return nil, mapErr(err)
	}
	return sqlResult{r}, nil
}

func (s *sqlConn) QueryContext(ctx context.Context, query string, named []driver.NamedValue) (driver.Rows, error) {
	if err := mixed(named); err != nil {
		return nil, err
	}
	r, err := s.q().SQL(ctx, query, args(named)...)
	if err != nil {
		return nil, mapErr(err)
	}
	return &sqlRows{r: r}, nil
}

func mixed(named []driver.NamedValue) error {
	n := 0
	for _, nv := range named {
		if nv.Name != "" {
			n++
		}
	}
	if n != 0 && n != len(named) {
		return inputError("SQL parameters: named and positional parameters cannot be mixed")
	}
	return nil
}

func (s *sqlConn) Ping(ctx context.Context) error {
	_, err := s.q().SQL(ctx, "SELECT 1")
	return mapErr(err)
}

// ResetSession runs before a pooled connection is reused: a transaction left open by
// SQL text is rolled back.
func (s *sqlConn) ResetSession(ctx context.Context) error {
	if s.tx != nil {
		_ = s.tx.Rollback(ctx)
		s.tx = nil
	}
	if s.c.InTransaction() {
		if _, err := s.c.SQL(ctx, "ROLLBACK"); err != nil && s.c.InTransaction() {
			return driver.ErrBadConn
		}
	}
	if !s.IsValid() {
		return driver.ErrBadConn
	}
	return nil
}

// IsValid is false once the client was closed, or lost its connection with reconnect off.
func (s *sqlConn) IsValid() bool {
	c := s.c
	if err := c.acquire(context.Background(), s.tx != nil); err != nil {
		return false
	}
	defer c.release(s.tx != nil)
	return !c.closed && !(c.lost && !c.reconnect)
}

// CheckNamedValue accepts every value the JSON encoder takes (maps, slices, structs, ...).
func (s *sqlConn) CheckNamedValue(nv *driver.NamedValue) error {
	if _, ok := nv.Value.(sql.Out); ok {
		return inputError("OUT parameters are not supported")
	}
	return nil
}

type sqlStmt struct {
	conn  *sqlConn
	query string
}

func (st *sqlStmt) Close() error  { return nil }
func (st *sqlStmt) NumInput() int { return -1 }

func (st *sqlStmt) Exec(vals []driver.Value) (driver.Result, error) {
	return st.ExecContext(context.Background(), toNamed(vals))
}

func (st *sqlStmt) Query(vals []driver.Value) (driver.Rows, error) {
	return st.QueryContext(context.Background(), toNamed(vals))
}

func (st *sqlStmt) ExecContext(ctx context.Context, named []driver.NamedValue) (driver.Result, error) {
	return st.conn.ExecContext(ctx, st.query, named)
}

func (st *sqlStmt) QueryContext(ctx context.Context, named []driver.NamedValue) (driver.Rows, error) {
	return st.conn.QueryContext(ctx, st.query, named)
}

func (st *sqlStmt) CheckNamedValue(nv *driver.NamedValue) error { return st.conn.CheckNamedValue(nv) }

func toNamed(vals []driver.Value) []driver.NamedValue {
	out := make([]driver.NamedValue, len(vals))
	for i, v := range vals {
		out[i] = driver.NamedValue{Ordinal: i + 1, Value: v}
	}
	return out
}

type sqlTx struct{ conn *sqlConn }

func (t *sqlTx) Commit() error {
	tx := t.conn.tx
	if tx == nil {
		return ErrTxDone
	}
	err := tx.Commit(context.Background())
	if err != nil && !errors.Is(err, ErrTxDone) {
		_ = tx.Rollback(context.Background()) // ends it if the COMMIT was never sent
	}
	t.conn.tx = nil
	return mapErr(err)
}

func (t *sqlTx) Rollback() error {
	tx := t.conn.tx
	if tx == nil {
		return ErrTxDone
	}
	t.conn.tx = nil
	err := tx.Rollback(context.Background())
	if errors.Is(err, ErrTxDone) {
		return nil
	}
	return mapErr(err)
}

type sqlResult struct{ r *Result }

func (r sqlResult) LastInsertId() (int64, error) {
	switch id := r.r.LastInsertID.(type) {
	case int64:
		return id, nil
	case nil:
		return 0, nil
	default:
		return 0, inputError("the row id is text, not an integer; read Result.LastInsertID through the Client")
	}
}

func (r sqlResult) RowsAffected() (int64, error) { return r.r.Changes, nil }

type sqlRows struct {
	r *Result
	i int
}

func (r *sqlRows) Columns() []string {
	if r.r.Columns == nil {
		return []string{}
	}
	return r.r.Columns
}

func (r *sqlRows) Close() error { return nil }

func (r *sqlRows) Next(dest []driver.Value) error {
	if r.i >= len(r.r.Rows) {
		return io.EOF
	}
	row := r.r.Rows[r.i]
	r.i++
	for j, col := range r.r.Columns {
		if j < len(dest) {
			dest[j] = driverValue(row[col])
		}
	}
	return nil
}

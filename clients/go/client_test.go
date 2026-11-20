package kestrodb

import (
	"context"
	"database/sql"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"net"
	"reflect"
	"strings"
	"testing"
	"time"
)

func sqlNamed(name string, v any) sql.NamedArg { return sql.Named(strings.TrimLeft(name, ":@"), v) }

func TestErrors(t *testing.T) {
	kinds := map[string]*Error{"CONFLICT": ErrConflict, "INPUT": ErrInput, "LIMIT": ErrLimit, "AUTH": ErrAuth, "IO": ErrIO,
		"CLOSED": ErrClosed, "BUSY": ErrBusy, "READONLY": ErrReadOnly, "UNAVAILABLE": ErrUnavailable, "CORRUPT": ErrCorrupt}
	for kind, s := range kinds {
		e := newError(kind + " something happened")
		if e.Kind != kind || !errors.Is(e, s) || e.Error() != kind+" something happened" {
			t.Errorf("%s: %+v", kind, e)
		}
		for other, s2 := range kinds {
			if other != kind && errors.Is(e, s2) {
				t.Errorf("%s matched %s", kind, other)
			}
		}
		wrapped := fmt.Errorf("context: %w", e)
		if !errors.Is(wrapped, s) {
			t.Errorf("wrapped %s", kind)
		}
	}
	if k := newError("STALE x").Kind; k != "STALE" {
		t.Errorf("other code: %s", k)
	}
	if k := newError("UNKNOWN_COMMIT x").Kind; k != "UNKNOWN_COMMIT" {
		t.Errorf("underscore code: %s", k)
	}
	for _, m := range []string{"weird message", "", "Conflict x"} {
		if k := newError(m).Kind; k != "ERROR" {
			t.Errorf("%q: kind %s", m, k)
		}
	}
	io := ioError("cannot connect", context.DeadlineExceeded)
	if !errors.Is(io, ErrIO) || !errors.Is(io, context.DeadlineExceeded) {
		t.Errorf("IO wraps its cause")
	}
	var e *Error
	if !errors.As(fmt.Errorf("x: %w", ErrConflict), &e) || e.Kind != "CONFLICT" {
		t.Errorf("errors.As")
	}
	if errors.Is(ErrIO, ErrConflict) || !errors.Is(ErrIO, ErrIO) {
		t.Errorf("sentinels")
	}
}

func TestAddresses(t *testing.T) {
	good := map[string][3]string{
		"tcp://127.0.0.1:7820": {"tcp", "127.0.0.1:7820", "tcp://127.0.0.1:7820"},
		"localhost:1":          {"tcp", "localhost:1", "tcp://localhost:1"},
		"tls://DB.example:443": {"tcp", "DB.example:443", "tls://db.example:443"},
		"tcp://[::1]:7820":     {"tcp", "[::1]:7820", "tcp://[::1]:7820"},
		"tcp://:7820":          {"tcp", "127.0.0.1:7820", "tcp://127.0.0.1:7820"},
		"unix:///tmp/k.sock":   {"unix", "/tmp/k.sock", "unix:///tmp/k.sock"},
	}
	for in, want := range good {
		a, err := parseAddress(in)
		if err != nil {
			t.Errorf("%s: %v", in, err)
			continue
		}
		if a.network != want[0] || a.dial != want[1] || a.key() != want[2] {
			t.Errorf("%s: %+v key %s", in, a, a.key())
		}
	}
	for _, bad := range []string{"tcp://127.0.0.1", "host", "tcp://h:0", "tcp://h:70000", "tcp://h:x", "unix://"} {
		if _, err := parseAddress(bad); !errors.Is(err, ErrInput) {
			t.Errorf("%s: %v", bad, err)
		}
	}
	if _, err := newClient([]string{" ", ""}, nil); !errors.Is(err, ErrInput) {
		t.Errorf("no address: %v", err)
	}
	c, err := newClient(splitAddresses("tcp://a:1, tcp://b:2 ,,tls://c:3"), &Options{FailoverTimeout: -1, Timeout: -1})
	if err != nil || len(c.addresses) != 3 || c.Address() != "tcp://a:1" || c.failover != 0 || c.timeout != 0 {
		t.Fatalf("cluster list: %v %+v", err, c)
	}
	if m := c.member("tcp://B:2"); m == nil || m.text != "tcp://b:2" {
		t.Errorf("member by key: %v", m)
	}
	if c.member("tcp://d:4") != nil || c.member("nonsense") != nil {
		t.Errorf("unknown member")
	}
	d, _ := newClient([]string{"tcp://a:1"}, nil)
	if d.timeout != 30*time.Second || d.failover != 15*time.Second || !d.reconnect || d.connectTO != 30*time.Second {
		t.Errorf("defaults %+v", d)
	}
}

func TestKeyValuePayloads(t *testing.T) {
	id, err := keyID("users", "kéy")
	if err != nil || string(id) != "users\x00kéy" {
		t.Errorf("id %q %v", id, err)
	}
	for _, c := range [][2]string{{"bad-name", "k"}, {"", "k"}, {"1abc", "k"}, {strings.Repeat("a", 65), "k"}, {"kv", ""}, {"kv", "a\x00b"}, {"kv", strings.Repeat("x", 513)}, {"kv", "\xff"}} {
		if _, err := keyID(c[0], c[1]); !errors.Is(err, ErrInput) {
			t.Errorf("keyID %q: %v", c, err)
		}
	}
	if _, err := keyID(strings.Repeat("a", 64), strings.Repeat("y", 512)); err != nil {
		t.Errorf("limits: %v", err)
	}

	b, err := batchPayload([]Op{SetOp("kv", "a", 1), DeleteOpIf("kv", "b", 7)})
	if err != nil {
		t.Fatal(err)
	}
	want := []byte{2, 0, 0, 0,
		1, 255, 255, 255, 255, 255, 255, 255, 255, 4, 0, 0, 0, 1, 0, 0, 0, 'k', 'v', 0, 'a', '1',
		0, 7, 0, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 'k', 'v', 0, 'b'}
	if !reflect.DeepEqual(b, want) {
		t.Errorf("batch payload\n got %v\nwant %v", b, want)
	}
	if _, err := batchPayload(nil); !errors.Is(err, ErrInput) {
		t.Errorf("empty batch: %v", err)
	}
	if _, err := batchPayload([]Op{SetOp("kv", "d", 1), DeleteOp("kv", "d")}); !errors.Is(err, ErrInput) {
		t.Errorf("duplicate: %v", err)
	}
	if _, err := batchPayload([]Op{{Kind: 9, Collection: "kv", Key: "x"}}); !errors.Is(err, ErrInput) {
		t.Errorf("kind: %v", err)
	}
	if _, err := batchPayload([]Op{SetOp("kv", "huge", strings.Repeat("z", 300*1024))}); !errors.Is(err, ErrLimit) {
		t.Errorf("value over 256 KiB: %v", err)
	}

	m, err := manyPayload("kv", []string{"a", "bc"})
	if err != nil || !reflect.DeepEqual(m, []byte{2, 0, 0, 0, 4, 0, 0, 0, 'k', 'v', 0, 'a', 5, 0, 0, 0, 'k', 'v', 0, 'b', 'c'}) {
		t.Errorf("getMany payload %v %v", m, err)
	}
	if _, err := manyPayload("kv", make([]string, 4097)); !errors.Is(err, ErrLimit) {
		t.Errorf("getMany over 4096: %v", err)
	}
	reply := []byte{0, 3, 0, 0, 0, 0, 1}
	reply = binary.LittleEndian.AppendUint64(reply, 9)
	reply = binary.LittleEndian.AppendUint32(reply, 7)
	reply = append(reply, `{"a":1}`...)
	reply = append(reply, 0)
	items, err := parseMany(reply, 3)
	if err != nil || len(items) != 3 || items[0] != nil || items[2] != nil || items[1].Version != 9 || !reflect.DeepEqual(items[1].Value, map[string]any{"a": int64(1)}) {
		t.Errorf("parseMany %v %v", items, err)
	}
	var into struct{ A int }
	if err := items[1].Decode(&into); err != nil || into.A != 1 {
		t.Errorf("Decode %v %v", into, err)
	}
	if _, err := parseMany(reply[:len(reply)-3], 3); !errors.Is(err, ErrIO) {
		t.Errorf("short getMany reply: %v", err)
	}
}

func TestTrack(t *testing.T) {
	c, _ := newClient([]string{"tcp://a:1"}, nil)
	steps := []struct {
		sql  string
		inTx bool
	}{
		{"BEGIN", true}, {"COMMIT", false}, {"start transaction", true}, {"  commit;", false},
		{"BEGIN CONCURRENT", true}, {"ROLLBACK TO SAVEPOINT a", true}, {"rollback to a", true}, {"ROLLBACK", false},
		{"SAVEPOINT outer_sp", true}, {"RELEASE SAVEPOINT other", true}, {"RELEASE SAVEPOINT outer_sp", false},
		{"SAVEPOINT \"q\";", true}, {"release q", false},
		{"BEGIN", true}, {"SAVEPOINT inner", true}, {"RELEASE inner", true}, {"END", false},
		{"SELECT 1", false}, {"", false},
	}
	for _, s := range steps {
		c.track(s.sql)
		if c.inTx != s.inTx {
			t.Errorf("after %q: inTx %v", s.sql, c.inTx)
		}
	}
	if !commitOrEnd.MatchString("  commit;") || !commitOrEnd.MatchString("END") || commitOrEnd.MatchString("ENDING") || commitOrEnd.MatchString("SELECT 1") {
		t.Errorf("commitOrEnd")
	}
}

func TestScan(t *testing.T) {
	row := &Row{columns: []string{"s", "i", "f", "b", "j", "d", "n", "big"}, values: map[string]any{
		"s": "text", "i": int64(42), "f": 2.5, "b": true, "j": map[string]any{"x": []any{int64(1), int64(2)}},
		"d": "2026-11-20", "n": nil, "big": "18446744073709551615"}}
	var (
		s   string
		i   int
		f   float64
		b   bool
		j   struct{ X []int }
		d   time.Time
		n   *string
		big uint64
	)
	if err := row.Scan(&s, &i, &f, &b, &j, &d, &n, &big); err != nil {
		t.Fatal(err)
	}
	if s != "text" || i != 42 || f != 2.5 || !b || !reflect.DeepEqual(j.X, []int{1, 2}) || !d.Equal(time.Date(2026, 11, 20, 0, 0, 0, 0, time.UTC)) || n != nil || big != 18446744073709551615 {
		t.Errorf("scanned %v %v %v %v %v %v %v %v", s, i, f, b, j, d, n, big)
	}
	var (
		a  any
		ns sql.NullString
		ni sql.NullInt64
		js string
		i8 int8
		ts time.Time
	)
	row2 := &Row{columns: []string{"a", "ns", "ni", "js", "ts"}, values: map[string]any{"a": int64(1), "ns": nil, "ni": int64(5), "js": []any{int64(1)}, "ts": "2026-11-20T10:15:30.5Z"}}
	if err := row2.Scan(&a, &ns, &ni, &js, &ts); err != nil {
		t.Fatal(err)
	}
	if a != int64(1) || ns.Valid || !ni.Valid || ni.Int64 != 5 || js != "[1]" || ts.Nanosecond() != 500000000 {
		t.Errorf("scanned %v %v %v %v %v", a, ns, ni, js, ts)
	}
	if err := (&Row{columns: []string{"n"}, values: map[string]any{"n": nil}}).Scan(&s); !errors.Is(err, ErrInput) {
		t.Errorf("NULL into string: %v", err)
	}
	if err := (&Row{columns: []string{"i"}, values: map[string]any{"i": int64(300)}}).Scan(&i8); !errors.Is(err, ErrInput) {
		t.Errorf("overflow: %v", err)
	}
	if err := (&Row{columns: []string{"i"}, values: map[string]any{"i": int64(1)}}).Scan(&s, &i); !errors.Is(err, ErrInput) {
		t.Errorf("count: %v", err)
	}
	if err := (&Row{err: ErrNoRows}).Scan(&s); !errors.Is(err, sql.ErrNoRows) {
		t.Errorf("no rows: %v", err)
	}
}

func TestParseDSN(t *testing.T) {
	addrs, o, err := ParseDSN("kestrodb://127.0.0.1:7820,127.0.0.1:7821?token=abc%2B1&timeout=3&failoverTimeout=25s&reconnect=false")
	if err != nil || !reflect.DeepEqual(addrs, []string{"tcp://127.0.0.1:7820", "tcp://127.0.0.1:7821"}) || o.Token != "abc+1" ||
		o.Timeout != 3*time.Second || o.FailoverTimeout != 25*time.Second || !o.DisableReconnect {
		t.Errorf("tcp DSN: %v %+v %v", addrs, o, err)
	}
	addrs, o, err = ParseDSN("kestrodb://:s3cret@localhost:9?caFile=/tmp/ca.pem&serverName=db")
	if err != nil || !reflect.DeepEqual(addrs, []string{"tls://localhost:9"}) || o.Token != "s3cret" || o.CAFile != "/tmp/ca.pem" || o.ServerName != "db" {
		t.Errorf("TLS by caFile: %v %+v %v", addrs, o, err)
	}
	for dsn, want := range map[string]string{
		"kestrodb+tls://h:1":          "tls://h:1",
		"kestrodb:tls://h:1?token=x":  "tls://h:1",
		"tcp://h:1":                   "tcp://h:1",
		"h:1?tls=true":                "tls://h:1",
		"kestrodb+unix:///run/k.sock": "unix:///run/k.sock",
		"kestrodb://h:1/":             "tcp://h:1",
	} {
		addrs, _, err := ParseDSN(dsn)
		if err != nil || len(addrs) != 1 || addrs[0] != want {
			t.Errorf("%s: %v %v", dsn, addrs, err)
		}
	}
	for _, bad := range []string{"kestrodb://", "kestrodb://h:1?bogus=1", "kestrodb://h:1?timeout=x", "kestrodb://h"} {
		if _, _, err := ParseDSN(bad); !errors.Is(err, ErrInput) {
			t.Errorf("%s: %v", bad, err)
		}
	}
}

// A fake server for the parts a real one cannot show on demand.
func fakeServer(t *testing.T, serve func(c net.Conn)) string {
	t.Helper()
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { l.Close() })
	go func() {
		for {
			c, err := l.Accept()
			if err != nil {
				return
			}
			go func() { defer c.Close(); serve(c) }()
		}
	}()
	return "tcp://" + l.Addr().String()
}

func readReq(c net.Conn) (byte, []byte, error) {
	var h [4]byte
	if _, err := io.ReadFull(c, h[:]); err != nil {
		return 0, nil, err
	}
	b := make([]byte, binary.LittleEndian.Uint32(h[:]))
	if _, err := io.ReadFull(c, b); err != nil {
		return 0, nil, err
	}
	return b[0], b[1:], nil
}

func writeFrame(c net.Conn, body []byte) {
	c.Write(binary.LittleEndian.AppendUint32(nil, uint32(len(body))))
	c.Write(body)
}

func TestFakeServer(t *testing.T) {
	ctx := context.Background()
	// Stats, errors, a context deadline, an unsolicited BUSY.
	addr := fakeServer(t, func(c net.Conn) {
		for {
			cmd, payload, err := readReq(c)
			if err != nil {
				return
			}
			switch cmd {
			case 'A':
				if string(payload) == "good" {
					writeFrame(c, []byte{0})
				} else {
					writeFrame(c, []byte("\xffAUTH invalid token"))
					return
				}
			case 'S':
				writeFrame(c, []byte("\x00{\"server\":true,\"keys\":3}"))
			case 'E':
				text := string(payload[4 : 4+binary.LittleEndian.Uint32(payload)])
				switch text {
				case "slow":
					time.Sleep(500 * time.Millisecond)
					writeFrame(c, []byte("\x00{\"rows\":[]}"))
				case "busy-then-close":
					writeFrame(c, []byte("\x00{\"changes\":0}"))
					writeFrame(c, []byte("\xffBUSY too many connections"))
					return
				default:
					writeFrame(c, []byte("\xffINPUT unknown statement "+text))
				}
			case 'Q':
				writeFrame(c, []byte{0})
				return
			}
		}
	})
	if _, err := Connect(ctx, addr, &Options{Token: "bad"}); !errors.Is(err, ErrAuth) {
		t.Fatalf("bad token: %v", err)
	}
	db, err := Connect(ctx, addr, &Options{Token: "good"})
	if err != nil {
		t.Fatal(err)
	}
	st, err := db.Stats(ctx)
	if err != nil || st["server"] != true || st["keys"] != int64(3) {
		t.Fatalf("stats %v %v", st, err)
	}
	if _, err := db.Exec(ctx, "nope"); !errors.Is(err, ErrInput) || err.Error() != "INPUT unknown statement nope" {
		t.Fatalf("server error: %v", err)
	}
	short, cancel := context.WithTimeout(ctx, 100*time.Millisecond)
	_, err = db.SQL(short, "slow")
	cancel()
	if !errors.Is(err, ErrIO) || !errors.Is(err, context.DeadlineExceeded) {
		t.Fatalf("deadline: %v", err)
	}
	if _, err := db.Stats(ctx); err != nil { // reconnected
		t.Fatalf("after a deadline: %v", err)
	}
	if db.opens != 2 {
		t.Errorf("opens %d", db.opens)
	}
	if _, err := db.SQL(ctx, "busy-then-close"); err != nil {
		t.Fatal(err)
	}
	time.Sleep(50 * time.Millisecond)
	if _, err := db.Stats(ctx); !errors.Is(err, ErrBusy) {
		t.Fatalf("unsolicited BUSY: %v", err)
	}
	if _, err := db.Stats(ctx); err != nil {
		t.Fatalf("the next call reconnects: %v", err)
	}
	canceled, cancel2 := context.WithCancel(ctx)
	cancel2()
	if _, err := db.Stats(canceled); !errors.Is(err, context.Canceled) {
		t.Errorf("canceled: %v", err)
	}
	db.Close()
	db.Close()
	if _, err := db.Stats(ctx); !errors.Is(err, ErrClosed) {
		t.Errorf("after close: %v", err)
	}
}

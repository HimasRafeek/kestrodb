package kestrodb

// TestServer runs against a real kestro-server; tests/go-client.test.mjs starts the
// servers and runs it as a compiled test binary:
//
//	go test -c -o kestro-go.test .
//	./kestro-go.test -test.run '^TestServer$' api ADDRESS TOKEN
//	... reconnect ADDRESS TOKEN          survives server restarts (waits for lines on stdin)
//	... busy ADDRESS TOKEN MAX            the server was started with --max-connections MAX
//	... tls ADDRESS TOKEN CA WRONG_CA     ADDRESS is tls://localhost:PORT (waits for a restart too)
//	... cluster LIST TOKEN FOLLOWER       follows the leader; waits for the leader to be killed
//	... cluster-new LIST TOKEN            a new client whose first member is dead
//
// Without arguments it is skipped.

import (
	"bufio"
	"context"
	"crypto/tls"
	"crypto/x509"
	"database/sql"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"math"
	"net/url"
	"os"
	"reflect"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"
)

type itest struct {
	*testing.T
	checks int
}

func (t *itest) eq(got, want any, what string) {
	t.Helper()
	t.checks++
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("%s: got %#v (%T), want %#v (%T)", what, got, got, want, want)
	}
}

func (t *itest) ok(cond bool, what string) {
	t.Helper()
	t.checks++
	if !cond {
		t.Fatalf("%s", what)
	}
}

func (t *itest) nil(err error, what string) {
	t.Helper()
	t.checks++
	if err != nil {
		t.Fatalf("%s: %v", what, err)
	}
}

// is checks err is of the sentinel's kind and returns it.
func (t *itest) is(err error, target error, what string) *Error {
	t.Helper()
	t.checks++
	if !errors.Is(err, target) {
		t.Fatalf("%s: got %v, want %v", what, err, target)
	}
	var e *Error
	errors.As(err, &e)
	return e
}

// must stops the test on an error (cur is the running test).
func must[T any](v T, err error) T {
	cur.Helper()
	cur.nil(err, "unexpected error")
	return v
}

var stdin = bufio.NewReader(os.Stdin)

var cur *itest

func waitLine(t *itest, ready string) {
	fmt.Println(ready)
	os.Stdout.Sync()
	if _, err := stdin.ReadString('\n'); err != nil {
		t.Fatalf("stdin: %v", err)
	}
}

func TestServer(t *testing.T) {
	args := flag.Args()
	if len(args) == 0 {
		t.Skip("run by tests/go-client.test.mjs against a kestro-server")
	}
	it := &itest{T: t}
	cur = it
	mode := args[0]
	switch mode {
	case "api":
		apiTest(it, args[1], args[2])
		databaseSQLTest(it, args[1], args[2])
	case "reconnect":
		reconnectTest(it, args[1], args[2])
	case "busy":
		n, _ := strconv.Atoi(args[3])
		busyTest(it, args[1], args[2], n)
	case "tls":
		tlsTest(it, args[1], args[2], args[3], args[4])
	case "cluster":
		clusterTest(it, args[1], args[2], args[3])
	case "cluster-new":
		clusterNewTest(it, args[1], args[2])
	default:
		t.Fatalf("unknown mode %s", mode)
	}
	fmt.Printf("ok go %s (%d checks)\n", mode, it.checks)
}

type uuidText string

func (u uuidText) MarshalText() ([]byte, error) { return []byte(u), nil }

type profile struct {
	Name string   `json:"name"`
	Tags []string `json:"tags"`
}

// ---------------------------------------------------------------------- client API

func apiTest(t *itest, address, token string) {
	ctx := context.Background()
	opts := &Options{Token: token}

	// Connecting: a wrong token, bad addresses.
	_, err := Connect(ctx, address, &Options{Token: "wrong-token-0123456789"})
	t.eq(t.is(err, ErrAuth, "wrong token").Kind, "AUTH", "AUTH kind")
	_, err = Connect(ctx, "tcp://127.0.0.1", opts)
	t.is(err, ErrInput, "address without a port")
	_, err = Connect(ctx, " , ", opts)
	t.is(err, ErrInput, "no address")
	_, err = Connect(ctx, "tcp://127.0.0.1:1", &Options{Token: token, Timeout: 5 * time.Second})
	refused := t.is(err, ErrIO, "nothing listening")
	t.ok(strings.HasPrefix(refused.Message, "IO cannot connect to tcp://127.0.0.1:1"), "IO message: "+refused.Message)
	short, cancel := context.WithTimeout(ctx, time.Nanosecond)
	time.Sleep(time.Millisecond)
	_, err = Connect(short, address, opts)
	cancel()
	t.ok(errors.Is(err, ErrIO) && errors.Is(err, context.DeadlineExceeded), fmt.Sprintf("connect with an expired context: %v", err))

	db := must(Connect(ctx, address, opts))
	t.eq(db.Address(), address, "Address()")
	st := must(db.Stats(ctx))
	t.eq(st["server"], true, "stats server")
	t.eq(st["tls"], false, "stats tls")
	_, isInt := st["keys"].(int64)
	t.ok(isInt, "stats keys is an int64")

	// ---- SQL with every parameter type.
	must(db.Exec(ctx, "CREATE TABLE IF NOT EXISTS typed (id INTEGER PRIMARY KEY, s TEXT, i INTEGER, r REAL, b BOOLEAN, j JSON, d DATE, u TEXT, n TEXT, t TIMESTAMP)"))
	must(db.Exec(ctx, "DELETE FROM typed"))
	text := "quote\" back\\slash\nnew\ttab \x01 é 日本 😀 ' -- ;"
	doc := map[string]any{"list": []any{int64(1), "two", 3.5, nil, true}, "obj": map[string]any{"k": "v"}, "empty": map[string]any{}}
	when := time.Date(2026, 11, 20, 10, 15, 30, 500000000, time.UTC)
	ins := must(db.SQL(ctx, "INSERT INTO typed (s, i, r, b, j, d, u, n, t) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
		text, 42, 3.25, true, doc, "2026-11-20", uuidText("123e4567-e89b-12d3-a456-426614174000"), nil, when))
	t.eq(ins.Changes, int64(1), "insert changes")
	t.ok(!ins.HasRows() && ins.Rows == nil, "a write has no rows")
	id1, isInt := ins.LastInsertID.(int64)
	t.ok(isInt && id1 > 0, "LastInsertID is an int64")
	t.eq(db.LastInsertID(), ins.LastInsertID, "client LastInsertID")
	row := must(db.QueryOne(ctx, "SELECT * FROM typed WHERE id = ?", id1))
	t.eq(row["s"], text, "text round trip")
	t.eq(row["i"], int64(42), "integer as int64")
	t.eq(row["r"], 3.25, "real as float64")
	t.eq(row["b"], true, "boolean")
	t.eq(row["j"], doc, "JSON column round trip")
	t.eq(row["d"], "2026-11-20", "date as ISO text")
	t.eq(row["u"], "123e4567-e89b-12d3-a456-426614174000", "TextMarshaler as text")
	t.eq(row["n"], nil, "null")
	t.eq(row["t"], "2026-11-20T10:15:30.5Z", "time.Time as RFC 3339 text")
	res := must(db.SQL(ctx, "SELECT * FROM typed WHERE id = ?", id1))
	t.eq(res.Columns[:3], []string{"id", "s", "i"}, "columns in table order")
	var gotTime time.Time
	var gotDate time.Time
	t.nil(db.QueryRow(ctx, "SELECT t, d FROM typed WHERE id = ?", id1).Scan(&gotTime, &gotDate), "scan times")
	t.ok(gotTime.Equal(when), "time.Time round trip")
	t.ok(gotDate.Equal(time.Date(2026, 11, 20, 0, 0, 0, 0, time.UTC)), "date into time.Time")
	// Other numeric Go types, structs, slices.
	must(db.Exec(ctx, "INSERT INTO typed (s, i, r, b) VALUES (?, ?, ?, ?)", "long", int64(9007199254740991), float32(1.5), false))
	must(db.Exec(ctx, "INSERT INTO typed (s, i, r) VALUES (?, ?, ?)", "short", int16(-7), json.Number("12.5")))
	must(db.Exec(ctx, "INSERT INTO typed (s, i, r) VALUES (?, ?, ?)", "byte", int8(8), 0.1))
	must(db.Exec(ctx, "INSERT INTO typed (s, i, j) VALUES (?, ?, ?)", "big", int64(-123456789012), []any{"a", color(0), []int{1, 2}}))
	must(db.Exec(ctx, "INSERT INTO typed (s, i, r, j) VALUES (?, ?, ?, ?)", "unsigned", uint32(4000000000), 2.0, profile{Name: "Ann", Tags: []string{"x", "y"}}))
	t.eq(must(db.QueryValue(ctx, "SELECT i FROM typed WHERE s = ?", "long")), int64(9007199254740991), "2^53-1 exact")
	t.eq(must(db.QueryValue(ctx, "SELECT r FROM typed WHERE s = ?", "long")), 1.5, "float32")
	t.eq(must(db.QueryValue(ctx, "SELECT b FROM typed WHERE s = ?", "long")), false, "false")
	t.eq(must(db.QueryValue(ctx, "SELECT i FROM typed WHERE s = ?", "short")), int64(-7), "int16")
	t.eq(must(db.QueryValue(ctx, "SELECT r FROM typed WHERE s = ?", "short")), 12.5, "json.Number")
	t.eq(must(db.QueryValue(ctx, "SELECT r FROM typed WHERE s = ?", "byte")), 0.1, "0.1 exactly")
	t.eq(must(db.QueryValue(ctx, "SELECT i FROM typed WHERE s = ?", "big")), int64(-123456789012), "negative int64")
	t.eq(must(db.QueryValue(ctx, "SELECT j FROM typed WHERE s = ?", "big")), []any{"a", "RED", []any{int64(1), int64(2)}}, "slice with a TextMarshaler and an []int")
	t.eq(must(db.QueryValue(ctx, "SELECT i FROM typed WHERE s = ?", "unsigned")), int64(4000000000), "uint32")
	r2 := must(db.QueryValue(ctx, "SELECT r FROM typed WHERE s = ?", "unsigned"))
	t.ok(r2 == 2.0 || r2 == int64(2), fmt.Sprintf("a REAL 2.0 (the server may write it as 2): %v", r2))
	var p profile
	t.nil(db.QueryRow(ctx, "SELECT j FROM typed WHERE s = ?", "unsigned").Scan(&p), "scan JSON into a struct")
	t.eq(p, profile{Name: "Ann", Tags: []string{"x", "y"}}, "struct round trip")
	t.eq(must(db.QueryValue(ctx, "SELECT COUNT(*) FROM typed WHERE b = ?", true)), int64(1), "boolean parameter")
	t.eq(must(db.QueryValue(ctx, "SELECT COUNT(*) FROM typed WHERE n IS NULL AND s = ?", text)), int64(1), "null column")
	_, err = db.Exec(ctx, "INSERT INTO typed (i) VALUES (?)", "not a number")
	t.is(err, ErrInput, "declared type enforced")
	_, err = db.Exec(ctx, "INSERT INTO typed (s) VALUES (?)", math.NaN())
	t.is(err, ErrInput, "NaN parameter")
	_, err = db.Exec(ctx, "INSERT INTO typed (s) VALUES (?)", "\xff")
	t.is(err, ErrInput, "invalid UTF-8 parameter")
	_, err = db.Exec(ctx, "INSERT INTO typed (s) VALUES (?)", make(chan int))
	t.is(err, ErrInput, "unknown parameter type")

	// ---- Placeholders: ?, ?N, $N, :name, @name; Named and sql.Named.
	must(db.Exec(ctx, "CREATE TABLE IF NOT EXISTS people (id INTEGER PRIMARY KEY, email TEXT UNIQUE, name TEXT, age INTEGER, city TEXT)"))
	must(db.Exec(ctx, "DELETE FROM people"))
	t.eq(must(db.Exec(ctx, "INSERT INTO people (email, name, age, city) VALUES (:email, :name, :age, @city)",
		Named{"email": "a@x.org", ":name": "Ann", "@age": 31, "city": "Kandy"})), int64(1), "named insert")
	ann := db.LastInsertID()
	must(db.Exec(ctx, "INSERT INTO people (email, name, age, city) VALUES (?, ?, ?, ?)", "b@x.org", "Bob", 25, "Galle"))
	must(db.Exec(ctx, "INSERT INTO people (email, name, age, city) VALUES ($2, $1, $3, $4)", "Cy", "c@x.org", 41, "Kandy"))
	must(db.Exec(ctx, "INSERT INTO people (email, name, age, city) VALUES (?2, ?1, ?3, ?3)", "Dee", "d@x.org", nil))
	t.eq(must(db.QueryValue(ctx, "SELECT name FROM people WHERE id = ?", ann)), "Ann", "LastInsertID row")
	older := must(db.Query(ctx, "SELECT name FROM people WHERE age > :min ORDER BY age", Named{"min": 26}))
	t.eq(older, []map[string]any{{"name": "Ann"}, {"name": "Cy"}}, "Query rows")
	t.eq(must(db.QueryOne(ctx, "SELECT name, age FROM people WHERE email = @e", sql.Named("e", "b@x.org"))), map[string]any{"name": "Bob", "age": int64(25)}, "QueryOne with sql.Named")
	none := must(db.QueryOne(ctx, "SELECT * FROM people WHERE id = ?", 999))
	t.ok(none == nil, "QueryOne none")
	t.eq(must(db.QueryValue(ctx, "SELECT COUNT(*) FROM people")), int64(4), "QueryValue")
	t.eq(must(db.QueryValue(ctx, "SELECT name FROM people WHERE id = -1")), nil, "QueryValue none")
	t.eq(must(db.QueryValue(ctx, "SELECT COUNT(*) FROM people WHERE city = $1 OR name = $1", "Kandy")), int64(2), "$1 twice")
	t.eq(len(must(db.Query(ctx, "SELECT * FROM people WHERE id = ?", -1))), 0, "empty result")
	empty := must(db.SQL(ctx, "SELECT * FROM people WHERE id = -1"))
	t.ok(empty.HasRows() && empty.Rows != nil, "an empty SELECT still has rows")
	t.eq(must(db.Exec(ctx, "UPDATE people SET age = age + 1 WHERE city = ?", "Kandy")), int64(2), "update changes")
	t.eq(must(db.Exec(ctx, "DELETE FROM people WHERE email = ?", "zzz")), int64(0), "delete none")
	_, err = db.Query(ctx, "UPDATE people SET age = 1 WHERE id = -1")
	t.is(err, ErrInput, "Query of a write")
	var name string
	var age int
	t.nil(db.QueryRow(ctx, "SELECT name, age FROM people WHERE email = ?", "a@x.org").Scan(&name, &age), "QueryRow")
	t.eq([]any{name, age}, []any{"Ann", 32}, "QueryRow values")
	t.is(db.QueryRow(ctx, "SELECT name FROM people WHERE id = -1").Scan(&name), sql.ErrNoRows, "QueryRow no rows")
	var nullable *int
	t.nil(db.QueryRow(ctx, "SELECT age FROM people WHERE email = ?", "d@x.org").Scan(&nullable), "NULL into **int")
	t.ok(nullable == nil, "NULL scanned as a nil pointer")

	// ---- Errors map to kinds.
	_, err = db.Exec(ctx, "INSERT INTO people (email) VALUES (?)", "a@x.org")
	t.eq(t.is(err, ErrConflict, "duplicate").Kind, "CONFLICT", "conflict kind")
	_, err = db.Query(ctx, "SELEC nonsense")
	t.eq(t.is(err, ErrInput, "bad SQL").Kind, "INPUT", "input kind")
	_, err = db.Query(ctx, "SELECT * FROM people WHERE id = :id", Named{})
	t.is(err, ErrInput, "missing named param")
	_, err = db.Query(ctx, "SELECT * FROM people WHERE id = ?")
	t.is(err, ErrInput, "missing positional param")
	_, err = db.Query(ctx, "SELECT * FROM people WHERE id = :id AND age = ?", 1, 2)
	t.is(err, ErrInput, "mixed placeholders")
	_, err = db.Query(ctx, "SELECT * FROM no_such_table")
	t.is(err, ErrInput, "missing table")
	t.eq(must(db.QueryValue(ctx, "SELECT COUNT(*) FROM people")), int64(4), "usable after errors")

	// ---- Transactions.
	must(db.Exec(ctx, "CREATE TABLE IF NOT EXISTS acct (id INTEGER PRIMARY KEY, bal INTEGER)"))
	must(db.Exec(ctx, "DELETE FROM acct"))
	must(db.Exec(ctx, "INSERT INTO acct (id, bal) VALUES (1, 100), (2, 0)"))
	other := must(Connect(ctx, address, opts))
	tx := must(db.Begin(ctx))
	t.ok(db.InTransaction(), "Begin opens")
	must(tx.Exec(ctx, "UPDATE acct SET bal = bal - 10 WHERE id = 1"))
	t.eq(must(tx.QueryValue(ctx, "SELECT bal FROM acct WHERE id = 1")), int64(90), "reads its own write")
	t.eq(must(other.QueryValue(ctx, "SELECT bal FROM acct WHERE id = 1")), int64(100), "others do not see it yet")
	// The Tx holds the client: another goroutine's call waits for the COMMIT.
	waited := make(chan any, 1)
	go func() {
		v, err := db.QueryValue(ctx, "SELECT bal FROM acct WHERE id = 1")
		if err != nil {
			v = err
		}
		waited <- v
	}()
	brief, cancel := context.WithTimeout(ctx, 100*time.Millisecond)
	_, err = db.Begin(brief)
	cancel()
	t.ok(errors.Is(err, ErrIO) && errors.Is(err, context.DeadlineExceeded), fmt.Sprintf("Begin waits for the open Tx: %v", err))
	select {
	case v := <-waited:
		t.Fatalf("a call ran inside another goroutine's transaction: %v", v)
	case <-time.After(150 * time.Millisecond):
	}
	t.nil(tx.Commit(ctx), "Commit")
	t.eq(<-waited, int64(90), "the waiting call ran after the COMMIT")
	t.ok(!db.InTransaction(), "Commit closes")
	t.eq(must(other.QueryValue(ctx, "SELECT bal FROM acct WHERE id = 1")), int64(90), "committed")
	t.is(tx.Commit(ctx), ErrTxDone, "Commit twice")
	_, err = tx.Exec(ctx, "SELECT 1")
	t.is(err, ErrTxDone, "a Tx after Commit")
	tx = must(db.Begin(ctx))
	must(tx.Exec(ctx, "UPDATE acct SET bal = 0 WHERE id = 1"))
	t.nil(tx.Rollback(ctx), "Rollback")
	t.ok(!db.InTransaction(), "Rollback closes")
	t.is(tx.Rollback(ctx), ErrTxDone, "Rollback twice")
	t.eq(must(db.QueryValue(ctx, "SELECT bal FROM acct WHERE id = 1")), int64(90), "rolled back")
	// As SQL text on the client.
	must(db.SQL(ctx, "BEGIN"))
	t.ok(db.InTransaction(), "BEGIN as SQL")
	_, err = db.Begin(ctx)
	t.is(err, ErrInput, "Begin inside a transaction")
	_, err = db.Set(ctx, "kv", "inside", 1)
	t.is(err, ErrInput, "key/value write inside a SQL transaction")
	t.ok(db.InTransaction(), "still open after a refused write")
	must(db.Exec(ctx, "UPDATE acct SET bal = 1 WHERE id = 2"))
	must(db.SQL(ctx, "ROLLBACK"))
	t.ok(!db.InTransaction(), "ROLLBACK as SQL")
	must(db.SQL(ctx, "start transaction"))
	t.ok(db.InTransaction(), "START TRANSACTION")
	must(db.SQL(ctx, "  commit;"))
	t.ok(!db.InTransaction(), "commit; with spaces")
	// Transaction(fn): COMMIT, or ROLLBACK and the error.
	t.nil(db.Transaction(ctx, func(tx *Tx) error {
		if _, err := tx.Exec(ctx, "UPDATE acct SET bal = bal - 30 WHERE id = 1"); err != nil {
			return err
		}
		_, err := tx.Exec(ctx, "UPDATE acct SET bal = bal + 30 WHERE id = 2")
		return err
	}), "Transaction")
	t.eq(must(db.QueryValue(ctx, "SELECT SUM(bal) FROM acct")), int64(90), "Transaction kept the total")
	t.eq(must(db.QueryValue(ctx, "SELECT bal FROM acct WHERE id = 2")), int64(30), "Transaction committed")
	boom := errors.New("boom")
	err = db.Transaction(ctx, func(tx *Tx) error {
		must(tx.Exec(ctx, "UPDATE acct SET bal = 0"))
		return boom
	})
	t.ok(err == boom, fmt.Sprintf("Transaction returns fn's error: %v", err))
	t.ok(!db.InTransaction(), "rolled back after the error")
	t.eq(must(db.QueryValue(ctx, "SELECT bal FROM acct WHERE id = 2")), int64(30), "Transaction rolled back")
	func() {
		defer func() { t.eq(recover(), "panic in a transaction", "the panic goes on") }()
		_ = db.Transaction(ctx, func(tx *Tx) error {
			must(tx.Exec(ctx, "UPDATE acct SET bal = 0"))
			panic("panic in a transaction")
		})
	}()
	t.ok(!db.InTransaction(), "rolled back after a panic")
	t.eq(must(db.QueryValue(ctx, "SELECT bal FROM acct WHERE id = 2")), int64(30), "the panic's writes rolled back")
	err = db.Transaction(ctx, func(tx *Tx) error {
		if _, err := tx.Exec(ctx, "INSERT INTO people (email) VALUES (?)", "zz@x.org"); err != nil {
			return err
		}
		_, err := tx.Exec(ctx, "INSERT INTO people (email) VALUES (?)", "zz@x.org")
		return err
	})
	t.is(err, ErrConflict, "conflict inside Transaction")
	t.ok(!db.InTransaction(), "closed after a conflict")
	t.eq(must(db.QueryValue(ctx, "SELECT COUNT(*) FROM people WHERE email = 'zz@x.org'")), int64(0), "nothing of it kept")
	var inside any
	t.nil(db.Transaction(ctx, func(tx *Tx) error {
		inside, err = tx.QueryValue(ctx, "SELECT bal FROM acct WHERE id = 1")
		return err
	}), "a read-only Transaction")
	t.eq(inside, int64(60), "a value from inside")
	// Savepoints: ROLLBACK TO keeps the transaction; a SAVEPOINT outside one begins it.
	tx = must(db.Begin(ctx))
	must(tx.Exec(ctx, "UPDATE acct SET bal = 1000 WHERE id = 1"))
	must(tx.SQL(ctx, "SAVEPOINT sp1"))
	must(tx.Exec(ctx, "UPDATE acct SET bal = 2000 WHERE id = 1"))
	must(tx.SQL(ctx, "ROLLBACK TO SAVEPOINT sp1"))
	t.ok(db.InTransaction(), "ROLLBACK TO keeps the transaction open")
	t.eq(must(tx.QueryValue(ctx, "SELECT bal FROM acct WHERE id = 1")), int64(1000), "ROLLBACK TO undid the later write")
	item, err := tx.Get(ctx, "kv", "nothing-here")
	t.ok(err == nil && item == nil, "a key/value read inside a Tx")
	t.nil(tx.Rollback(ctx), "whole rollback")
	t.eq(must(db.QueryValue(ctx, "SELECT bal FROM acct WHERE id = 1")), int64(60), "whole rollback undid it")
	must(db.SQL(ctx, "SAVEPOINT outer_sp"))
	t.ok(db.InTransaction(), "SAVEPOINT outside a transaction begins one")
	must(db.Exec(ctx, "UPDATE acct SET bal = 61 WHERE id = 1"))
	must(db.SQL(ctx, "RELEASE SAVEPOINT outer_sp"))
	t.ok(!db.InTransaction(), "releasing it commits")
	t.eq(must(other.QueryValue(ctx, "SELECT bal FROM acct WHERE id = 1")), int64(61), "released savepoint committed")
	// BEGIN CONCURRENT: the first committer wins; the loser's COMMIT fails and ends it.
	mine := must(db.BeginTx(ctx, &TxOptions{Concurrent: true}))
	theirs := must(other.BeginTx(ctx, &TxOptions{Concurrent: true}))
	must(mine.Exec(ctx, "UPDATE acct SET bal = 1 WHERE id = 2"))
	must(theirs.Exec(ctx, "UPDATE acct SET bal = 2 WHERE id = 2"))
	t.nil(theirs.Commit(ctx), "first committer")
	lost := t.is(mine.Commit(ctx), ErrConflict, "second committer loses")
	t.ok(strings.Contains(lost.Message, "CONFLICT"), "conflict message")
	t.ok(!db.InTransaction(), "a failed COMMIT ends the transaction")
	t.is(mine.Rollback(ctx), ErrTxDone, "nothing left to roll back")
	t.eq(must(db.QueryValue(ctx, "SELECT bal FROM acct WHERE id = 2")), int64(2), "the first committer's value")
	err = db.TransactionWith(ctx, &TxOptions{Concurrent: true}, func(tx *Tx) error {
		_, err := tx.Exec(ctx, "UPDATE acct SET bal = 3 WHERE id = 2")
		return err
	})
	t.nil(err, "TransactionWith concurrent")
	t.eq(must(other.QueryValue(ctx, "SELECT bal FROM acct WHERE id = 2")), int64(3), "concurrent transaction committed")

	// ---- Key/value with versions.
	must(db.Delete(ctx, "kv", "k1"))
	v1 := must(db.Set(ctx, "kv", "k1", map[string]any{"n": 1}))
	t.ok(v1 > 0, "Set returns a version")
	got := must(db.Get(ctx, "kv", "k1"))
	t.eq(got.Version, v1, "Get version")
	t.eq(got.Value, map[string]any{"n": int64(1)}, "Get value")
	t.eq(string(got.Raw), `{"n":1}`, "Get raw")
	var into struct{ N int }
	t.nil(got.Decode(&into), "Decode")
	t.eq(into.N, 1, "Decode value")
	missing := must(db.Get(ctx, "kv", "missing"))
	t.ok(missing == nil, "Get missing")
	v2 := must(db.SetIf(ctx, "kv", "k1", map[string]any{"n": 2}, v1))
	t.ok(v2 > v1, "SetIf at the right version")
	_, err = db.SetIf(ctx, "kv", "k1", map[string]any{"n": 3}, v1)
	t.eq(t.is(err, ErrConflict, "stale expected version").Kind, "CONFLICT", "stale kind")
	t.eq(must(db.Get(ctx, "kv", "k1")).Value, map[string]any{"n": int64(2)}, "not overwritten")
	_, err = db.SetIf(ctx, "kv", "k1", 1, 0)
	t.is(err, ErrConflict, "expected version 0 on an existing key")
	must(db.Delete(ctx, "kv", "fresh"))
	vf := must(db.SetIf(ctx, "kv", "fresh", "new", 0))
	t.eq(*must(db.Get(ctx, "kv", "fresh")), Item{Version: vf, Value: "new", Raw: json.RawMessage(`"new"`)}, "expected version 0 creates")
	_, err = db.DeleteIf(ctx, "kv", "fresh", vf+1000)
	t.is(err, ErrConflict, "conditional delete, wrong version")
	must(db.DeleteIf(ctx, "kv", "fresh", vf))
	t.ok(must(db.Get(ctx, "kv", "fresh")) == nil, "conditional delete")
	// Values of every JSON type.
	values := []any{nil, true, false, int64(0), int64(-1), int64(math.MaxInt64), int64(math.MinInt64), 3.5, -1.25e-10, "", "text 😀 \"q\" \\ \n",
		[]any{}, map[string]any{}, map[string]any{"deep": []any{map[string]any{"x": []any{int64(1), nil, "y"}}}}, json.Number("18446744073709551616")}
	for i, v := range values {
		key := "type" + strconv.Itoa(i)
		must(db.Set(ctx, "kv", key, v))
		t.eq(must(db.Get(ctx, "kv", key)).Value, v, "value round trip "+key)
	}
	// Unicode keys; key and collection rules.
	ukey := "ключ/日本/😀 with spaces"
	must(db.Set(ctx, "kv", ukey, "unicode key"))
	t.eq(must(db.Get(ctx, "kv", ukey)).Value, "unicode key", "unicode key")
	for _, bad := range [][2]string{{"bad-name", "k"}, {"kv", ""}, {"kv", "a\x00b"}, {"kv", strings.Repeat("x", 513)}, {"kv", "\xed\xa0\x80"}} {
		_, err = db.Get(ctx, bad[0], bad[1])
		t.is(err, ErrInput, fmt.Sprintf("invalid id %q", bad))
	}
	must(db.Set(ctx, "kv", strings.Repeat("y", 512), 1))
	_, err = db.Set(ctx, "kv", "huge", strings.Repeat("z", 300*1024))
	t.is(err, ErrLimit, "value over 256 KiB")
	big := strings.Repeat("b", 200*1024)
	must(db.Set(ctx, "kv", "big", big))
	t.eq(must(db.Get(ctx, "kv", "big")).Value, big, "200 KiB value")
	// GetMany: one snapshot, in order, nil where missing.
	many := must(db.GetMany(ctx, "kv", "k1", "missing", "type3", ukey))
	t.eq(len(many), 4, "GetMany size")
	t.eq(many[0].Value, map[string]any{"n": int64(2)}, "GetMany 0")
	t.ok(many[1] == nil, "GetMany missing")
	t.eq(many[2].Value, int64(0), "GetMany 2")
	t.eq(many[3].Value, "unicode key", "GetMany unicode")
	t.eq(len(must(db.GetMany(ctx, "kv"))), 0, "GetMany none")
	tooMany := make([]string, 4097)
	for i := range tooMany {
		tooMany[i] = "k" + strconv.Itoa(i)
	}
	_, err = db.GetMany(ctx, "kv", tooMany...)
	t.is(err, ErrLimit, "GetMany over 4096")
	// Batch: all or nothing, one version.
	vb := must(db.Batch(ctx, SetOp("kv", "b1", 1), SetOp("kv", "b2", 2), DeleteOp("kv", "type0")))
	t.eq(must(db.Get(ctx, "kv", "b1")).Version, vb, "batch version b1")
	t.eq(must(db.Get(ctx, "kv", "b2")).Version, vb, "batch version b2")
	t.ok(must(db.Get(ctx, "kv", "type0")) == nil, "batch delete")
	_, err = db.Batch(ctx, SetOp("kv", "b1", 10), SetOpIf("kv", "b2", 20, vb-1))
	t.is(err, ErrConflict, "batch with a stale version")
	t.eq(must(db.Get(ctx, "kv", "b1")).Value, int64(1), "nothing of a failed batch applied")
	_, err = db.Batch(ctx, SetOp("kv", "d", 1), DeleteOp("kv", "d"))
	t.is(err, ErrInput, "duplicate key in a batch")
	_, err = db.Batch(ctx)
	t.is(err, ErrInput, "empty batch")
	ops := make([]Op, 1000)
	for i := range ops {
		ops[i] = SetOp("bulk", "k"+strconv.Itoa(i), map[string]any{"i": i})
	}
	vbulk := must(db.Batch(ctx, ops...))
	k999 := must(db.Get(ctx, "bulk", "k999"))
	t.eq([]any{k999.Version, k999.Value}, []any{vbulk, map[string]any{"i": int64(999)}}, "1000-op batch")
	keys, _ := must(db.Stats(ctx))["keys"].(int64)
	t.ok(keys > 1000, "stats keys")
	t.nil(db.Verify(ctx), "Verify")
	t.is(db.Backup(ctx, "x.kdb"), ErrInput, "backup without --backup-dir")
	compacted := must(db.Compact(ctx))
	_, hasBefore := compacted["before"]
	_, hasAfter := compacted["after"]
	t.ok(hasBefore && hasAfter, "Compact")
	t.eq(must(db.Get(ctx, "bulk", "k5")).Value, map[string]any{"i": int64(5)}, "reads after compaction")

	// ---- Goroutines share one client (calls are serialized; a Tx holds it).
	must(db.Exec(ctx, "CREATE TABLE IF NOT EXISTS counter (id INTEGER PRIMARY KEY, n INTEGER)"))
	must(db.Exec(ctx, "DELETE FROM counter"))
	must(db.Exec(ctx, "INSERT INTO counter (id, n) VALUES (1, 0)"))
	var wg sync.WaitGroup
	errs := make(chan error, 16)
	for g := 0; g < 8; g++ {
		wg.Add(1)
		go func(g int) {
			defer wg.Done()
			for i := 0; i < 50; i++ {
				key := fmt.Sprintf("t%d_%d", g, i)
				if _, err := db.Set(ctx, "threads", key, i); err != nil {
					errs <- err
					return
				}
				it, err := db.Get(ctx, "threads", key)
				if err != nil || it.Value != int64(i) {
					errs <- fmt.Errorf("goroutine read %v %v", it, err)
					return
				}
				if g < 4 && i%5 == 0 {
					// Read, then write, inside one transaction: no other goroutine runs between.
					err := db.Transaction(ctx, func(tx *Tx) error {
						n, err := tx.QueryValue(ctx, "SELECT n FROM counter WHERE id = 1")
						if err != nil {
							return err
						}
						_, err = tx.Exec(ctx, "UPDATE counter SET n = ? WHERE id = 1", n.(int64)+1)
						return err
					})
					if err != nil {
						errs <- err
						return
					}
				}
			}
		}(g)
	}
	wg.Wait()
	close(errs)
	for err := range errs {
		t.Fatalf("goroutines: %v", err)
	}
	t.eq(must(db.QueryValue(ctx, "SELECT n FROM counter WHERE id = 1")), int64(40), "transactions from goroutines did not interleave")

	// ---- Contexts: a canceled call fails with the context's error; the client goes on.
	canceled, cancelNow := context.WithCancel(ctx)
	cancelNow()
	_, err = db.Stats(canceled)
	t.ok(errors.Is(err, ErrIO) && errors.Is(err, context.Canceled), fmt.Sprintf("canceled context: %v", err))
	t.eq(must(db.QueryValue(ctx, "SELECT COUNT(*) FROM people")), int64(4), "usable after a canceled context")

	// ---- An idle connection is kept (no reconnect when nothing happened).
	opened := db.opens
	time.Sleep(1200 * time.Millisecond)
	t.eq(must(db.QueryValue(ctx, "SELECT COUNT(*) FROM people")), int64(4), "after an idle second")
	t.eq(db.opens, opened, "the idle connection was kept, not reopened")

	// ---- Close ends an open Tx (the server rolls it back).
	third := must(Connect(ctx, address, opts))
	open := must(third.Begin(ctx))
	must(open.Exec(ctx, "UPDATE acct SET bal = 999 WHERE id = 1"))
	t.nil(third.Close(), "Close with an open Tx")
	_, err = open.Exec(ctx, "SELECT 1")
	t.is(err, ErrClosed, "a Tx after Close")
	t.is(open.Commit(ctx), ErrClosed, "Commit after Close")
	t.eq(must(db.QueryValue(ctx, "SELECT bal FROM acct WHERE id = 1")), int64(61), "Close rolled the Tx back")

	// ---- Closed.
	t.nil(other.Close(), "close other")
	t.nil(db.Close(), "Close")
	t.nil(db.Close(), "Close twice")
	_, err = db.Get(ctx, "kv", "k1")
	t.eq(t.is(err, ErrClosed, "use after Close").Kind, "CLOSED", "CLOSED kind")
}

// ---------------------------------------------------------------------- database/sql

func databaseSQLTest(t *itest, address, token string) {
	ctx := context.Background()
	hostPort := strings.TrimPrefix(address, "tcp://")
	bad := must(sql.Open("kestrodb", "kestrodb://"+hostPort+"?token=wrong-token-0123456789"))
	t.is(bad.PingContext(ctx), ErrAuth, "database/sql wrong token")
	bad.Close()
	_, err := sql.Open("kestrodb", "kestrodb://"+hostPort+"?nonsense=1")
	t.is(err, ErrInput, "unknown DSN option")

	db := must(sql.Open("kestrodb", "kestrodb://"+hostPort+"?token="+url.QueryEscape(token)))
	defer db.Close()
	db.SetMaxOpenConns(4)
	t.nil(db.PingContext(ctx), "Ping")
	must(db.ExecContext(ctx, "CREATE TABLE IF NOT EXISTS jt (id INTEGER PRIMARY KEY, name TEXT, n INTEGER, r REAL, ok BOOLEAN, d DATE, j JSON)"))
	must(db.ExecContext(ctx, "DELETE FROM jt"))
	res := must(db.ExecContext(ctx, "INSERT INTO jt (name, n, r, ok, d, j) VALUES (?, ?, ?, ?, ?, ?)", "prep 😀", 7, 2.5, true, "2026-11-20", map[string]any{"a": []int{1, 2}}))
	id := must(res.LastInsertId())
	t.ok(id > 0, "LastInsertId")
	t.eq(must(res.RowsAffected()), int64(1), "RowsAffected")
	var (
		name string
		n    int
		r    float64
		ok   bool
		d    string
		j    []byte
	)
	t.nil(db.QueryRowContext(ctx, "SELECT name, n, r, ok, d, j FROM jt WHERE id = ?", id).Scan(&name, &n, &r, &ok, &d, &j), "QueryRow Scan")
	t.eq([]any{name, n, r, ok, d, string(j)}, []any{"prep 😀", 7, 2.5, true, "2026-11-20", `{"a":[1,2]}`}, "scanned values")
	must(db.ExecContext(ctx, "INSERT INTO jt (name) VALUES (:name)", sql.Named("name", "nulls")))
	var nn sql.NullInt64
	var ns sql.NullString
	t.nil(db.QueryRowContext(ctx, "SELECT n, d FROM jt WHERE name = @name", sql.Named("name", "nulls")).Scan(&nn, &ns), "NULLs")
	t.ok(!nn.Valid && !ns.Valid, "NULL into sql.Null*")
	t.is(db.QueryRowContext(ctx, "SELECT n FROM jt WHERE id = -1").Scan(&n), sql.ErrNoRows, "ErrNoRows")
	// A prepared statement used many times; rows with columns.
	st := must(db.PrepareContext(ctx, "INSERT INTO jt (name, n) VALUES (?, ?)"))
	for i := 0; i < 5; i++ {
		must(st.ExecContext(ctx, "batch"+strconv.Itoa(i), i))
	}
	st.Close()
	rows := must(db.QueryContext(ctx, "SELECT name, n FROM jt WHERE name LIKE 'batch%' ORDER BY n"))
	t.eq(must(rows.Columns()), []string{"name", "n"}, "Columns")
	count := 0
	for rows.Next() {
		var s string
		var k int64
		t.nil(rows.Scan(&s, &k), "rows.Scan")
		t.eq(s, "batch"+strconv.Itoa(count), "row order")
		count++
	}
	t.nil(rows.Err(), "rows.Err")
	rows.Close()
	t.eq(count, 5, "row count")
	// Transactions.
	tx := must(db.BeginTx(ctx, nil))
	must(tx.ExecContext(ctx, "UPDATE jt SET n = 100 WHERE name = 'batch0'"))
	t.nil(tx.Rollback(), "Tx.Rollback")
	tx = must(db.BeginTx(ctx, nil))
	must(tx.ExecContext(ctx, "UPDATE jt SET n = 200 WHERE name = 'batch0'"))
	var inTx int
	t.nil(tx.QueryRowContext(ctx, "SELECT n FROM jt WHERE name = 'batch0'").Scan(&inTx), "read inside a Tx")
	t.eq(inTx, 200, "reads its own write")
	t.nil(tx.Commit(), "Tx.Commit")
	t.nil(db.QueryRowContext(ctx, "SELECT n FROM jt WHERE name = 'batch0'").Scan(&n), "after commit")
	t.eq(n, 200, "rollback then commit")
	_, err = db.BeginTx(ctx, &sql.TxOptions{Isolation: sql.LevelReadCommitted})
	t.is(err, ErrInput, "unsupported isolation level")
	// Errors keep their kinds.
	must(db.ExecContext(ctx, "CREATE TABLE IF NOT EXISTS ju (id INTEGER PRIMARY KEY, email TEXT UNIQUE)"))
	must(db.ExecContext(ctx, "DELETE FROM ju"))
	must(db.ExecContext(ctx, "INSERT INTO ju (email) VALUES ('x@y')"))
	_, err = db.ExecContext(ctx, "INSERT INTO ju (email) VALUES ('x@y')")
	t.is(err, ErrConflict, "database/sql duplicate")
	_, err = db.QueryContext(ctx, "SELEC 1")
	t.is(err, ErrInput, "database/sql bad SQL")
	tx = must(db.BeginTx(ctx, nil))
	must(tx.ExecContext(ctx, "INSERT INTO ju (email) VALUES ('in-tx')"))
	_, err = tx.ExecContext(ctx, "INSERT INTO ju (email) VALUES ('x@y')")
	t.is(err, ErrConflict, "conflict inside a Tx")
	t.nil(tx.Rollback(), "rollback after a conflict")
	t.nil(db.QueryRowContext(ctx, "SELECT COUNT(*) FROM ju").Scan(&n), "count")
	t.eq(n, 1, "the failed Tx left nothing")
	// The Client under a pooled connection.
	conn := must(db.Conn(ctx))
	t.nil(conn.Raw(func(dc any) error {
		c := Raw(dc)
		if c == nil || c.Address() != address {
			return fmt.Errorf("Raw: %v", c)
		}
		_, err := c.Set(ctx, "kv", "from-sql", true)
		return err
	}), "sql.Conn.Raw")
	conn.Close()
	// The pool from goroutines, with transactions.
	must(db.ExecContext(ctx, "CREATE TABLE IF NOT EXISTS pc (id INTEGER PRIMARY KEY, n INTEGER)"))
	must(db.ExecContext(ctx, "DELETE FROM pc"))
	must(db.ExecContext(ctx, "INSERT INTO pc (id, n) VALUES (1, 0)"))
	var wg sync.WaitGroup
	errs := make(chan error, 8)
	for g := 0; g < 8; g++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for i := 0; i < 10; i++ {
				tx, err := db.BeginTx(ctx, nil)
				if err != nil {
					errs <- err
					return
				}
				var v int
				if err := tx.QueryRowContext(ctx, "SELECT n FROM pc WHERE id = 1").Scan(&v); err != nil {
					tx.Rollback()
					errs <- err
					return
				}
				if _, err := tx.ExecContext(ctx, "UPDATE pc SET n = ? WHERE id = 1", v+1); err != nil {
					tx.Rollback()
					errs <- err
					return
				}
				if err := tx.Commit(); err != nil {
					errs <- err
					return
				}
			}
		}()
	}
	wg.Wait()
	close(errs)
	for err := range errs {
		t.Fatalf("pool goroutines: %v", err)
	}
	t.nil(db.QueryRowContext(ctx, "SELECT n FROM pc WHERE id = 1").Scan(&n), "pool count")
	t.eq(n, 80, "80 serialized increments")
	// NewConnector with Options.
	viaConnector := sql.OpenDB(NewConnector(address, &Options{Token: token}))
	var one int
	t.nil(viaConnector.QueryRowContext(ctx, "SELECT 1 AS one").Scan(&one), "NewConnector")
	t.eq(one, 1, "NewConnector value")
	viaConnector.Close()
}

// ---------------------------------------------------------------------- reconnect

func reconnectTest(t *itest, address, token string) {
	ctx := context.Background()
	opts := &Options{Token: token}
	db := must(Connect(ctx, address, opts))
	must(db.Exec(ctx, "CREATE TABLE IF NOT EXISTS rc (id INTEGER PRIMARY KEY, v TEXT)"))
	must(db.Exec(ctx, "INSERT INTO rc (v) VALUES (?)", "before"))
	must(db.Set(ctx, "rkv", "k", "before"))
	txc := must(Connect(ctx, address, opts))
	off := must(Connect(ctx, address, &Options{Token: token, DisableReconnect: true}))
	must(off.Stats(ctx))
	sdb := must(sql.Open("kestrodb", strings.Replace(address, "tcp://", "kestrodb://", 1)+"?token="+url.QueryEscape(token)))
	sdb.SetMaxOpenConns(1)
	t.nil(sdb.PingContext(ctx), "database/sql ping")
	waitLine(t, "READY") // the server restarts meanwhile
	time.Sleep(1500 * time.Millisecond)
	opened := db.opens
	must(db.Exec(ctx, "INSERT INTO rc (v) VALUES (?)", "after"))
	t.eq(db.opens, opened+1, "reopened once")
	t.eq(must(db.QueryValue(ctx, "SELECT COUNT(*) FROM rc")), int64(2), "rows after reconnect")
	t.eq(must(db.Get(ctx, "rkv", "k")).Value, "before", "key/value after reconnect")
	// A transaction cannot survive its connection: the next call says so, the one after reconnects.
	tx := must(txc.Begin(ctx))
	t.ok(txc.InTransaction(), "transaction on the new server")
	must(tx.Exec(ctx, "INSERT INTO rc (v) VALUES (?)", "in tx"))
	waitLine(t, "READY2") // restarted again, with the transaction open
	_, err := tx.Exec(ctx, "INSERT INTO rc (v) VALUES (?)", "x")
	lostTx := t.is(err, ErrIO, "a call inside a lost transaction")
	t.ok(strings.HasPrefix(lostTx.Message, "IO connection lost inside a transaction; it was rolled back"), "lost transaction message: "+lostTx.Message)
	t.ok(!txc.InTransaction(), "no longer in a transaction")
	_, err = tx.Exec(ctx, "SELECT 1")
	t.is(err, ErrIO, "the Tx stays failed (never runs outside the transaction)")
	t.is(tx.Commit(ctx), ErrIO, "Commit of a lost transaction")
	t.eq(must(txc.QueryValue(ctx, "SELECT COUNT(*) FROM rc")), int64(2), "the next call reconnects; the transaction's row is gone")
	// DisableReconnect: the dead connection fails with IO, and stays failed.
	_, err = off.Stats(ctx)
	t.eq(t.is(err, ErrIO, "reconnect off").Kind, "IO", "IO kind")
	_, err = off.Stats(ctx)
	t.is(err, ErrIO, "reconnect off, again")
	// database/sql reconnects the same way.
	var n int
	t.nil(sdb.QueryRowContext(ctx, "SELECT COUNT(*) FROM rc").Scan(&n), "database/sql after restarts")
	t.eq(n, 2, "database/sql count")
	sdb.Close()
	db.Close()
	txc.Close()
	off.Close()
}

// ---------------------------------------------------------------------- busy

func busyTest(t *itest, address, token string, max int) {
	ctx := context.Background()
	opts := &Options{Token: token}
	var held []*Client
	for i := 0; i < max; i++ {
		c := must(Connect(ctx, address, opts))
		must(c.Stats(ctx))
		held = append(held, c)
	}
	// The server answers BUSY and closes at once; the client's login frame, still unread,
	// can make that close a reset that discards the BUSY reply (then: IO). Every attempt
	// must fail, and BUSY must be seen.
	var busy *Error
	for i := 0; i < 20 && busy == nil; i++ {
		c, err := Connect(ctx, address, opts)
		if err == nil {
			_, err = c.Stats(ctx)
			c.Close()
		}
		if err == nil {
			t.Fatalf("a connection over --max-connections was accepted")
		}
		var e *Error
		if errors.As(err, &e) && e.Kind == "BUSY" {
			busy = e
			continue
		}
		t.ok(errors.Is(err, ErrIO) && (strings.Contains(err.Error(), "before login") || strings.Contains(err.Error(), "connection lost")), "over the limit: "+err.Error())
	}
	// Deterministic: a client that has sent nothing yet reads the server's BUSY before its
	// first request (a client without a token sends nothing when it connects).
	quiet := must(Connect(ctx, address, nil))
	time.Sleep(300 * time.Millisecond)
	_, err := quiet.Stats(ctx)
	waited := t.is(err, ErrBusy, "BUSY waiting before the first request")
	quiet.Close()
	if busy == nil {
		busy = waited
	}
	t.eq(busy.Kind, "BUSY", "BUSY kind")
	t.ok(strings.HasPrefix(busy.Message, "BUSY too many connections"), "BUSY message: "+busy.Message)
	held[0].Close()
	held = held[1:]
	var again *Client
	for i := 0; i < 50 && again == nil; i++ {
		c, err := Connect(ctx, address, opts)
		if err == nil {
			if _, err = c.Stats(ctx); err == nil {
				again = c
				break
			}
			c.Close()
		}
		time.Sleep(100 * time.Millisecond)
	}
	t.ok(again != nil, "a slot freed by Close")
	again.Close()
	for _, c := range held {
		c.Close()
	}
}

// ---------------------------------------------------------------------- TLS

func tlsTest(t *itest, address, token, ca, wrongCA string) {
	ctx := context.Background()
	port := address[strings.LastIndexByte(address, ':')+1:]
	opts := &Options{Token: token, CAFile: ca}
	db := must(Connect(ctx, address, opts))
	t.eq(must(db.Stats(ctx))["tls"], true, "stats tls")
	must(db.Exec(ctx, "CREATE TABLE IF NOT EXISTS tl (id INTEGER PRIMARY KEY, body TEXT)"))
	big := strings.Repeat("t", 300_000) // many TLS records each way
	must(db.Exec(ctx, "INSERT INTO tl (body) VALUES (?)", big))
	body := must(db.QueryValue(ctx, "SELECT body FROM tl WHERE id = ?", db.LastInsertID()))
	t.eq(len(body.(string)), 300_000, "300 KB over TLS")
	v := must(db.Set(ctx, "kv", "tls", map[string]any{"ok": true}))
	t.eq(must(db.Get(ctx, "kv", "tls")).Version, v, "key/value over TLS")
	opened := db.opens
	time.Sleep(1200 * time.Millisecond)
	t.eq(must(db.Get(ctx, "kv", "tls")).Version, v, "after an idle second")
	t.eq(db.opens, opened, "an idle TLS connection is kept")
	// The certificate names localhost and 127.0.0.1.
	ip := must(Connect(ctx, "tls://127.0.0.1:"+port, opts))
	t.eq(must(ip.Stats(ctx))["tls"], true, "IP address SAN")
	ip.Close()
	_, err := Connect(ctx, address, &Options{Token: token, CAFile: wrongCA})
	wrong := t.is(err, ErrIO, "wrong CA")
	t.ok(strings.Contains(wrong.Message, "TLS handshake failed"), "handshake message: "+wrong.Message)
	_, err = Connect(ctx, address, &Options{Token: token})
	t.is(err, ErrIO, "the system pool does not know the test CA")
	_, err = Connect(ctx, address, &Options{Token: token, CAFile: ca, ServerName: "other.example"})
	t.is(err, ErrIO, "wrong host name")
	insecure := must(Connect(ctx, address, &Options{Token: token, InsecureSkipVerify: true}))
	t.eq(must(insecure.Stats(ctx))["tls"], true, "InsecureSkipVerify")
	insecure.Close()
	pem := must(os.ReadFile(ca))
	pool := x509.NewCertPool()
	pool.AppendCertsFromPEM(pem)
	withConfig := must(Connect(ctx, address, &Options{Token: token, TLSConfig: &tls.Config{RootCAs: pool}}))
	t.eq(must(withConfig.Stats(ctx))["tls"], true, "TLSConfig with RootCAs")
	withConfig.Close()
	_, err = Connect(ctx, "tcp://localhost:"+port, &Options{Token: token, Timeout: 5 * time.Second})
	t.is(err, ErrIO, "plain TCP to a TLS port")
	sdb := must(sql.Open("kestrodb", "kestrodb+tls://localhost:"+port+"?token="+url.QueryEscape(token)+"&caFile="+url.QueryEscape(ca)))
	var n int
	t.nil(sdb.QueryRowContext(ctx, "SELECT COUNT(*) AS n FROM tl").Scan(&n), "database/sql over TLS")
	t.ok(n >= 1, "database/sql over TLS count")
	sdb.Close()
	waitLine(t, "READY") // the TLS server restarts
	time.Sleep(1500 * time.Millisecond)
	t.eq(must(db.Get(ctx, "kv", "tls")).Value, map[string]any{"ok": true}, "TLS reconnect after a restart")
	t.eq(db.opens, opened+1, "reopened once")
	db.Close()
}

// ---------------------------------------------------------------------- cluster

func clusterTest(t *itest, list, token, follower string) {
	ctx := context.Background()
	// Only the follower: a write is refused with READONLY naming the leader; reads work.
	f := must(Connect(ctx, follower, &Options{Token: token}))
	_, err := f.Exec(ctx, "INSERT INTO t (v) VALUES ('x')")
	ro := t.is(err, ErrReadOnly, "a follower refuses writes")
	t.eq(ro.Kind, "READONLY", "READONLY kind")
	t.ok(strings.Contains(ro.Message, "the leader is"), "names the leader: "+ro.Message)
	must(f.QueryValue(ctx, "SELECT COUNT(*) FROM t"))
	f.Close()
	opts := &Options{Token: token, Timeout: 3 * time.Second, FailoverTimeout: 25 * time.Second}
	db := must(Connect(ctx, list, opts))
	t.eq(db.Address(), follower, "starts at the first member")
	for i := 0; i < 5; i++ {
		must(db.Exec(ctx, "INSERT INTO t (v) VALUES (?)", "a"+strconv.Itoa(i)))
	}
	v := must(db.Set(ctx, "ckv", "k", "v1"))
	t.eq(must(db.Get(ctx, "ckv", "k")).Version, v, "key/value at the leader")
	fmt.Println("ADDRESS " + db.Address())
	sdb := must(sql.Open("kestrodb", "kestrodb://"+strings.ReplaceAll(list, "tcp://", "")+"?token="+url.QueryEscape(token)+"&timeout=3&failoverTimeout=25"))
	sdb.SetMaxOpenConns(1)
	res := must(sdb.ExecContext(ctx, "INSERT INTO t (v) VALUES ('sql')"))
	t.eq(must(res.RowsAffected()), int64(1), "database/sql follows the leader")
	conn := must(sdb.Conn(ctx))
	t.nil(conn.Raw(func(dc any) error {
		if a := Raw(dc).Address(); a != db.Address() {
			return fmt.Errorf("database/sql at %s, the client at %s", a, db.Address())
		}
		return nil
	}), "database/sql at the leader")
	conn.Close()
	sdb.Close()
	waitLine(t, "READY") // the leader is killed and a new one elected
	time.Sleep(1500 * time.Millisecond)
	for i := 0; i < 5; i++ {
		must(db.Exec(ctx, "INSERT INTO t (v) VALUES (?)", "b"+strconv.Itoa(i)))
	}
	t.eq(must(db.Get(ctx, "ckv", "k")).Value, "v1", "key/value survived the failover")
	fmt.Println("ADDRESS " + db.Address())
	db.Close()
}

func clusterNewTest(t *itest, list, token string) {
	ctx := context.Background()
	db := must(Connect(ctx, list, &Options{Token: token, Timeout: 3 * time.Second, FailoverTimeout: 25 * time.Second}))
	for i := 0; i < 5; i++ {
		must(db.Exec(ctx, "INSERT INTO t (v) VALUES (?)", "c"+strconv.Itoa(i)))
	}
	fmt.Println("ADDRESS " + db.Address())
	db.Close()
}

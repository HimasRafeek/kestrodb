package kestrodb

import (
	"context"
	"database/sql"
	"encoding/binary"
	"errors"
	"regexp"
	"strings"
)

// Named holds parameters for :name and @name placeholders; keys may include the leading
// : or @. Pass it as the only argument:
//
//	db.Exec(ctx, "INSERT INTO users (email, name) VALUES (:email, :name)", kestrodb.Named{"email": e, "name": n})
//
// sql.Named("email", e) arguments work too.
type Named map[string]any

// Result is the server's answer to one statement.
type Result struct {
	// Columns names the columns of Rows in SELECT order (in order of first appearance
	// when rows differ); empty when there are no rows.
	Columns []string
	// Rows holds a statement's rows; nil for a statement that returns none (INSERT,
	// UPDATE, DELETE, DDL), and empty, not nil, for a SELECT that matched nothing.
	Rows []map[string]any
	// Changes is the number of rows an INSERT, UPDATE or DELETE changed.
	Changes int64
	// LastInsertID is the id of the row an INSERT created: int64, or string for text
	// ids; nil when the statement created none.
	LastInsertID any
	// Extra holds any other members of the server's reply.
	Extra map[string]any

	hasID bool
}

// HasRows reports whether the statement returns rows (a SELECT, even one that matched none).
func (r *Result) HasRows() bool { return r.Rows != nil }

// Values returns the rows as slices in Columns order (nil where a row lacks a column).
func (r *Result) Values() [][]any {
	out := make([][]any, len(r.Rows))
	for i, row := range r.Rows {
		vals := make([]any, len(r.Columns))
		for j, col := range r.Columns {
			vals[j] = row[col]
		}
		out[i] = vals
	}
	return out
}

func (r *Result) extra(k string, v any) {
	switch k {
	case "changes":
		if n, ok := v.(int64); ok {
			r.Changes = n
			return
		}
	case "lastInsertId":
		r.LastInsertID, r.hasID = v, true
		return
	}
	if r.Extra == nil {
		r.Extra = map[string]any{}
	}
	r.Extra[k] = v
}

// sqlParams encodes the parameters as a JSON array (positional) or object (named).
func sqlParams(args []any) ([]byte, error) {
	if len(args) == 0 {
		return []byte("[]"), nil
	}
	if len(args) == 1 {
		if n, ok := args[0].(Named); ok {
			m := make(map[string]any, len(n))
			for k, v := range n {
				m[strings.TrimLeft(k, ":@")] = v
			}
			return encodeParams(m)
		}
	}
	named := 0
	for _, a := range args {
		switch a.(type) {
		case sql.NamedArg, *sql.NamedArg:
			named++
		case Named:
			return nil, inputError("SQL parameters: Named must be the only argument")
		}
	}
	if named > 0 {
		if named != len(args) {
			return nil, inputError("SQL parameters: named and positional parameters cannot be mixed")
		}
		m := make(map[string]any, len(args))
		for _, a := range args {
			na, ok := a.(sql.NamedArg)
			if !ok {
				na = *a.(*sql.NamedArg)
			}
			if na.Name == "" {
				return nil, inputError("SQL parameters: a sql.NamedArg without a name")
			}
			m[strings.TrimLeft(na.Name, ":@")] = na.Value
		}
		return encodeParams(m)
	}
	return encodeParams(args)
}

func encodeParams(v any) ([]byte, error) {
	b, err := encodeValue(v)
	if err != nil {
		var e *Error
		if errors.As(err, &e) {
			e.Message = "INPUT SQL parameters: " + strings.TrimPrefix(e.Message, "INPUT ")
		}
		return nil, err
	}
	return b, nil
}

func sqlPayload(text string, params []byte) []byte {
	p := make([]byte, 4+len(text)+len(params))
	binary.LittleEndian.PutUint32(p, uint32(len(text)))
	copy(p[4:], text)
	copy(p[4+len(text):], params)
	return p
}

var commitOrEnd = regexp.MustCompile(`(?i)^\s*(COMMIT|END)\b`)

// runSQL runs one statement; tx is nil outside a Tx.
func (c *Client) runSQL(ctx context.Context, tx *Tx, text string, args []any) (*Result, error) {
	params, err := sqlParams(args)
	if err != nil {
		return nil, err
	}
	payload := sqlPayload(text, params)
	if ctx == nil {
		ctx = context.Background()
	}
	if err := c.acquire(ctx, tx != nil); err != nil {
		return nil, err
	}
	defer c.release(tx != nil)
	if err := tx.check(); err != nil {
		return nil, err
	}
	reply, err := c.call(ctx, 'E', payload)
	if err != nil {
		// A COMMIT that fails (a conflict) has rolled the transaction back on the server.
		if !kindIs(err, "IO") && commitOrEnd.MatchString(text) {
			c.inTx, c.implicitSave = false, ""
		}
		tx.after(err)
		return nil, err
	}
	res, err := parseResult(reply[1:])
	if err != nil {
		return nil, ioError("malformed reply: "+err.Error(), err)
	}
	c.track(text)
	if res.hasID {
		c.lastInsertID = res.LastInsertID
	}
	return res, nil
}

// track keeps InTransaction right for BEGIN/COMMIT/ROLLBACK and savepoints sent as SQL.
func (c *Client) track(text string) {
	words := strings.Fields(text)
	if len(words) == 0 {
		return
	}
	if len(words) > 3 {
		words = words[:3]
	}
	w0 := strings.TrimSuffix(strings.ToUpper(words[0]), ";")
	switch w0 {
	case "BEGIN", "START":
		c.inTx, c.implicitSave = true, ""
	case "COMMIT", "END":
		c.inTx, c.implicitSave = false, ""
	case "ROLLBACK":
		// ROLLBACK TO [SAVEPOINT] name keeps the transaction open.
		if len(words) < 2 || !strings.EqualFold(words[1], "TO") {
			c.inTx, c.implicitSave = false, ""
		}
	case "SAVEPOINT":
		if !c.inTx && len(words) >= 2 {
			c.inTx, c.implicitSave = true, unquote(words[1])
		}
	case "RELEASE":
		name := ""
		if len(words) >= 3 && strings.EqualFold(words[1], "SAVEPOINT") {
			name = words[2]
		} else if len(words) >= 2 {
			name = words[1]
		}
		if name != "" && c.implicitSave != "" && strings.EqualFold(unquote(name), c.implicitSave) {
			c.inTx, c.implicitSave = false, ""
		}
	}
}

func unquote(w string) string {
	s := strings.TrimSuffix(w, ";")
	if len(s) >= 2 && (s[0] == '"' || s[0] == '`') && s[len(s)-1] == s[0] {
		s = s[1 : len(s)-1]
	}
	return s
}

// ---------------------------------------------------------------------- the SQL methods

// querier has the methods a Client and a Tx share.
type querier struct {
	c  *Client
	tx *Tx // nil on a Client
}

// SQL runs one statement (see SQL.md). args fill ?, ?N and $N placeholders in order, or
// :name and @name placeholders when the only argument is a Named map (or every argument
// is a sql.NamedArg). A parameter is nil, a bool, a number, a string, time.Time (RFC 3339
// text), a []any or map[string]any (stored as JSON), or anything encoding/json encodes.
func (q querier) SQL(ctx context.Context, query string, args ...any) (*Result, error) {
	return q.c.runSQL(ctx, q.tx, query, args)
}

// Exec runs an INSERT, UPDATE, DELETE or DDL statement and returns the number of rows
// it changed. LastInsertID (on the Client) has the id an INSERT created.
func (q querier) Exec(ctx context.Context, query string, args ...any) (int64, error) {
	r, err := q.SQL(ctx, query, args...)
	if err != nil {
		return 0, err
	}
	return r.Changes, nil
}

// Query returns all rows of a SELECT (or another statement that returns rows).
func (q querier) Query(ctx context.Context, query string, args ...any) ([]map[string]any, error) {
	r, err := q.SQL(ctx, query, args...)
	if err != nil {
		return nil, err
	}
	if !r.HasRows() {
		return nil, inputError("Query needs a statement that returns rows; use Exec")
	}
	return r.Rows, nil
}

// QueryOne returns the first row, or nil when there is none.
func (q querier) QueryOne(ctx context.Context, query string, args ...any) (map[string]any, error) {
	rows, err := q.Query(ctx, query, args...)
	if err != nil || len(rows) == 0 {
		return nil, err
	}
	return rows[0], nil
}

// QueryValue returns the first column of the first row, or nil when there is none.
func (q querier) QueryValue(ctx context.Context, query string, args ...any) (any, error) {
	r, err := q.SQL(ctx, query, args...)
	if err != nil {
		return nil, err
	}
	if !r.HasRows() {
		return nil, inputError("QueryValue needs a statement that returns rows; use Exec")
	}
	if len(r.Rows) == 0 || len(r.Columns) == 0 {
		return nil, nil
	}
	return r.Rows[0][r.Columns[0]], nil
}

// QueryRow runs a query for one row; Scan reports ErrNoRows when there is none.
//
//	var name string
//	err := db.QueryRow(ctx, "SELECT name FROM users WHERE id = ?", id).Scan(&name)
func (q querier) QueryRow(ctx context.Context, query string, args ...any) *Row {
	r, err := q.SQL(ctx, query, args...)
	if err != nil {
		return &Row{err: err}
	}
	if !r.HasRows() {
		return &Row{err: inputError("QueryRow needs a statement that returns rows; use Exec")}
	}
	if len(r.Rows) == 0 {
		return &Row{err: ErrNoRows}
	}
	return &Row{columns: r.Columns, values: r.Rows[0]}
}

// Get returns a key's value and version, or nil when the key does not exist.
func (q querier) Get(ctx context.Context, collection, key string) (*Item, error) {
	return q.c.get(ctx, q.tx, collection, key)
}

// GetMany reads up to 4096 keys in one consistent snapshot, in order; nil where a key is
// missing.
func (q querier) GetMany(ctx context.Context, collection string, keys ...string) ([]*Item, error) {
	return q.c.getMany(ctx, q.tx, collection, keys)
}

// Row is the result of QueryRow.
type Row struct {
	columns []string
	values  map[string]any
	err     error
}

// Err is the query's error, if any (ErrNoRows when it matched no row).
func (r *Row) Err() error { return r.err }

// Columns names the row's columns in SELECT order.
func (r *Row) Columns() []string { return r.columns }

// Map returns the row as a map.
func (r *Row) Map() (map[string]any, error) { return r.values, r.err }

// Scan copies the row's columns, in order, into dest (one per column). It converts as
// database/sql does: into *string, *[]byte, *int..., *uint..., *float64, *bool,
// *time.Time (from ISO text), *any, sql.Scanner (sql.NullString, ...), a pointer to a
// pointer for NULLs, and a struct, map or slice pointer for a JSON value.
func (r *Row) Scan(dest ...any) error {
	if r.err != nil {
		return r.err
	}
	if len(dest) != len(r.columns) {
		return inputError("Scan: " + itoa(len(dest)) + " destinations for " + itoa(len(r.columns)) + " columns")
	}
	for i, d := range dest {
		if err := assign(d, r.values[r.columns[i]]); err != nil {
			return inputError("Scan column " + r.columns[i] + ": " + strings.TrimPrefix(err.Error(), "INPUT "))
		}
	}
	return nil
}

// ---------------------------------------------------------------------- transactions

// TxOptions choose how a transaction begins.
type TxOptions struct {
	// Concurrent begins with BEGIN CONCURRENT: other transactions run alongside, and the
	// first to commit a change to the same rows wins (the other's Commit fails with
	// ErrConflict).
	Concurrent bool
}

// Tx is an open transaction (from Begin). It holds the Client: other goroutines' calls on
// the Client wait until Commit or Rollback. Run the transaction's statements through the
// Tx (calling the Client from inside it would wait for itself). A Tx is safe for
// concurrent use.
type Tx struct {
	querier
	done bool // guarded by the client's sem
	lost bool // the connection went with the transaction
}

// Begin starts a transaction (BEGIN). It holds the write gate on the server until Commit
// or Rollback, so keep it short.
func (c *Client) Begin(ctx context.Context) (*Tx, error) { return c.BeginTx(ctx, nil) }

// BeginTx starts a transaction with options (opts may be nil).
func (c *Client) BeginTx(ctx context.Context, opts *TxOptions) (*Tx, error) {
	if ctx == nil {
		ctx = context.Background()
	}
	if err := c.acquire(ctx, false); err != nil {
		return nil, err
	}
	ok := false
	defer func() {
		if !ok {
			c.release(false)
		}
	}()
	if c.inTx {
		return nil, inputError("a transaction is already open")
	}
	stmt := "BEGIN"
	if opts != nil && opts.Concurrent {
		stmt = "BEGIN CONCURRENT"
	}
	if _, err := c.call(ctx, 'E', sqlPayload(stmt, []byte("[]"))); err != nil {
		return nil, err
	}
	c.track(stmt)
	c.publish()
	tx := &Tx{}
	tx.querier = querier{c: c, tx: tx}
	c.stateMu.Lock()
	c.openTx = tx
	c.stateMu.Unlock()
	ok = true
	return tx, nil // the gate is released when the transaction ends
}

// check is called with the sem held, before a call inside the transaction.
func (tx *Tx) check() error {
	if tx == nil {
		return nil
	}
	if tx.c.closed {
		return closedError()
	}
	if tx.done || !tx.c.inTx {
		if tx.lost {
			return ioError("connection lost inside a transaction; it was rolled back", nil)
		}
		return ErrTxDone
	}
	return nil
}

// after notes a call's error: a lost connection took the transaction with it.
func (tx *Tx) after(err error) {
	if tx != nil && err != nil && kindIs(err, "IO") && !tx.c.inTx {
		tx.lost = true
	}
}

// end runs a Commit or Rollback step with the sem held. When the transaction is over, the
// Tx is marked done and, after the sem's release has published the client's state, the
// gate is freed for other calls.
func (tx *Tx) end(ctx context.Context, step func() error) error {
	c := tx.c
	if ctx == nil {
		ctx = context.Background()
	}
	c.sem <- struct{}{} // only calls of this Tx (and Close) hold it, each for one request
	if tx.done {
		closed := c.closed
		c.release(true)
		if closed {
			return closedError()
		}
		return ErrTxDone
	}
	err := step()
	over := !c.inTx
	if over {
		tx.done = true
		c.stateMu.Lock()
		c.openTx = nil
		c.stateMu.Unlock()
	}
	c.release(true)
	if over {
		<-c.gate
	}
	return err
}

// Commit commits the transaction. A failed COMMIT (a CONFLICT) has rolled it back.
func (tx *Tx) Commit(ctx context.Context) error {
	c := tx.c
	if ctx == nil {
		ctx = context.Background()
	}
	return tx.end(ctx, func() error {
		switch {
		case tx.lost:
			return ioError("connection lost inside a transaction; it was rolled back", nil)
		case c.inTx:
			_, err := c.call(ctx, 'E', sqlPayload("COMMIT", []byte("[]")))
			if err == nil {
				c.track("COMMIT")
			} else if !kindIs(err, "IO") {
				c.inTx, c.implicitSave = false, ""
			}
			return err
		}
		return nil
	})
}

// Rollback rolls the transaction back. After it, the transaction is over even when it
// returns an error (the connection is then closed, which rolls it back on the server).
func (tx *Tx) Rollback(ctx context.Context) error {
	c := tx.c
	if ctx == nil {
		ctx = context.Background()
	}
	return tx.end(ctx, func() error {
		if !c.inTx {
			return nil
		}
		_, err := c.call(ctx, 'E', sqlPayload("ROLLBACK", []byte("[]")))
		if err == nil {
			c.track("ROLLBACK")
		}
		if c.inTx {
			c.drop()
			c.inTx, c.implicitSave, c.lost = false, "", false
		}
		return err
	})
}

// Transaction runs fn inside BEGIN ... COMMIT. When fn returns an error or panics, the
// transaction is rolled back and the error (or panic) passed on.
//
//	err := db.Transaction(ctx, func(tx *kestrodb.Tx) error {
//	    if _, err := tx.Exec(ctx, "UPDATE accounts SET balance = balance - ? WHERE id = ?", 10, 1); err != nil {
//	        return err
//	    }
//	    _, err := tx.Exec(ctx, "UPDATE accounts SET balance = balance + ? WHERE id = ?", 10, 2)
//	    return err
//	})
func (c *Client) Transaction(ctx context.Context, fn func(tx *Tx) error) error {
	return c.TransactionWith(ctx, nil, fn)
}

// TransactionWith is Transaction with options (BEGIN CONCURRENT).
func (c *Client) TransactionWith(ctx context.Context, opts *TxOptions, fn func(tx *Tx) error) (err error) {
	if ctx == nil {
		ctx = context.Background()
	}
	tx, err := c.BeginTx(ctx, opts)
	if err != nil {
		return err
	}
	finished := false
	defer func() {
		if !finished {
			_ = tx.Rollback(context.WithoutCancel(ctx)) // a panic in fn
		}
	}()
	if err := fn(tx); err != nil {
		finished = true
		if rb := tx.Rollback(context.WithoutCancel(ctx)); rb != nil && !errors.Is(rb, ErrTxDone) && !kindIs(err, "IO") {
			return errors.Join(err, rb)
		}
		return err
	}
	finished = true
	if err := tx.Commit(ctx); err != nil {
		_ = tx.Rollback(context.WithoutCancel(ctx)) // ErrTxDone when the COMMIT ended it
		return err
	}
	return nil
}

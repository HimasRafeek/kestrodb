// Package kestrodb is the Go client for kestro-server, KestroDB's database server: SQL
// and versioned key/value over one TCP (or TLS, or Unix socket) connection, with
// reconnecting, cluster leader following, and a database/sql driver. Standard library only.
//
//	db, err := kestrodb.Connect(ctx, "tcp://127.0.0.1:7820", &kestrodb.Options{Token: os.Getenv("KESTRODB_TOKEN")})
//	if err != nil { ... }
//	defer db.Close()
//
//	_, err = db.Exec(ctx, "INSERT INTO users (email, name) VALUES (:email, :name)", kestrodb.Named{"email": "a@example.com", "name": "Ann"})
//	user, err := db.QueryOne(ctx, "SELECT * FROM users WHERE id = ?", db.LastInsertID())
//	var name string
//	err = db.QueryRow(ctx, "SELECT name FROM users WHERE id = ?", 1).Scan(&name)
//
//	err = db.Transaction(ctx, func(tx *kestrodb.Tx) error {
//	    if _, err := tx.Exec(ctx, "UPDATE accounts SET balance = balance - ? WHERE id = ?", 10, 1); err != nil {
//	        return err
//	    }
//	    _, err := tx.Exec(ctx, "UPDATE accounts SET balance = balance + ? WHERE id = ?", 10, 2)
//	    return err
//	})
//
//	v, err := db.Set(ctx, "users", "kestro", map[string]any{"name": "Kestro"})
//	_, err = db.SetIf(ctx, "users", "kestro", map[string]any{"name": "K"}, v) // only if still at version v
//	item, err := db.Get(ctx, "users", "kestro")                               // item.Version, item.Value, item.Decode(&u)
//	if errors.Is(err, kestrodb.ErrConflict) { ... }
//
// Addresses: tcp://host:port, tls://host:port, unix:///path, or host:port. Several
// comma-separated addresses name a cluster's members: writes go to the leader, which a
// follower's READONLY reply names, and the client moves there and sends the request again
// (nothing was applied), also during an election, for up to Options.FailoverTimeout.
// Never inside a transaction: its connection went with its leader.
//
// A connection the server closed while idle (a restart, the network) is reopened before
// the next request, except inside a transaction: that call fails with "IO ... connection
// lost inside a transaction; it was rolled back", and the one after reconnects. A request
// already sent when the connection broke fails with ErrIO (for a write: unknown outcome).
//
// JSON values come back as nil, bool, int64 (integers), float64 (numbers with a fraction
// or exponent), json.Number (integers beyond int64), string, []any and map[string]any.
//
// database/sql: import the package and sql.Open("kestrodb", "kestrodb://host:7820?token=...").
package kestrodb

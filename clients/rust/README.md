# kestrodb (Rust)

A synchronous Rust client for `kestro-server`: SQL and key/value over TCP, TLS (the `tls`
feature) or a Unix socket. It reconnects idle connections and follows a cluster's leader.
There is no embedded mode: the database runs in `kestro-server`.

```toml
[dependencies]
kestrodb = { path = "clients/rust" }                       # add features = ["tls"] for tls://
```

```rust
use kestrodb::serde_json::json;
use kestrodb::{named_params, params, Client, Error, Op, Options};

let mut db = Client::connect("tcp://127.0.0.1:7820",
    Options { token: std::env::var("KESTRODB_TOKEN").ok(), ..Options::default() })?;
db.execute("INSERT INTO users (email, name) VALUES (:email, :name)",
           named_params! { "email" => "a@example.com", "name" => "Ann" })?;
let id = db.last_insert_id().cloned();
let user = db.query_one("SELECT * FROM users WHERE id = ?", params![id])?;   // Option<Row>
db.transaction(|tx| {
    tx.execute("UPDATE accounts SET balance = balance - ? WHERE id = ?", params![10, 1])?;
    tx.execute("UPDATE accounts SET balance = balance + ? WHERE id = ?", params![10, 2])?;
    Ok::<_, Error>(())
})?;
let v = db.set("users", "kestro", json!({"name": "Kestro"}))?;
db.set_if("users", "kestro", json!({"name": "K"}), v)?;              // only if still at version v
db.batch(&[Op::set("kv", "a", 1), Op::delete("kv", "b").if_version(7)])?;
```

## API

| Method | Returns |
|---|---|
| `sql(text, params)` | `SqlResult { rows: Option<Vec<Row>>, changes, last_insert_id }`, `columns()` |
| `query` / `query_one` / `query_value` | `Vec<Row>` / `Option<Row>` / `Option<Value>` (first column) |
| `query_as::<T>(text, params)` | rows converted to any `Deserialize` type |
| `execute(text, params)` | rows changed (`u64`) |
| `last_insert_id()`, `in_transaction()`, `address()` | |
| `begin` / `commit` / `rollback`, `transaction(\|tx\| ...)` | COMMIT, or ROLLBACK and return the error (or resume the panic) |
| `get` / `get_as::<T>` / `get_many` | `Option<Versioned { version, value }>` |
| `set` / `set_if(.., expected_version)` / `delete` / `delete_if` / `batch(&[Op])` | the new version (`u64`) |
| `stats`, `verify`, `backup`, `compact_to`, `compact`, `promote` | server commands (admin) |
| `close()` (also on `Drop`) | |

`Row` is a `serde_json::Map<String, Value>` that keeps the columns in `SELECT` order.

**Parameters.** `params![a, b, c]` for `?`, `?N` and `$N`; `named_params! { "name" => v }`
for `:name` / `@name` (keys may include the `:` or `@`). Values are any `serde::Serialize`
type and are borrowed. Also accepted: `()`, a `Params`, `Vec<Value>`, `[Value; N]`, a
`serde_json` array or object. Non-finite floats become null (serde_json).

**Errors.** `kestrodb::Error` implements `std::error::Error`; one variant per code, each
holding the full message: `Conflict`, `Input`, `Limit`, `Auth`, `Io`, `Busy`, `ReadOnly`,
`Unavailable`, `Corrupt`, `Closed`, `Other` (`code()` gives e.g. `STALE`).
`is_unknown_outcome()` is true for `Io` and `Unavailable`.

**Options.** `token`, `timeout` (30 s; `Duration::ZERO`: none), `connect_timeout`,
`reconnect` (true), `failover_timeout` (15 s), and for `tls://`: `tls_ca_file` (default: the
system's CAs), `tls_verify` (false: testing only), `tls_server_name`, `tls_config` (your own
`rustls::ClientConfig`). Fields are public; builder methods have the same names.

**Connections.** Addresses are `tcp://host:port`, `tls://host:port`, `unix:///path` or
`host:port`; a cluster is several, comma-separated in one string or as a slice/`Vec`. A
connection the server closed while idle is reopened before the next request (never inside a
transaction: that call fails with `IO connection lost inside a transaction; it was rolled
back`). Writes that reach a follower get `READONLY` naming the leader; the client moves there
and sends them again, for up to `failover_timeout`. One `Client` is one connection: it is
`Send`; share it behind a `Mutex`, or open one per thread.

## Dependencies

- `serde` and `serde_json` (`preserve_order`): parameters, values and rows are JSON; the
  order keeps a row's columns in `SELECT` order.
- With `tls` only: `rustls` (ring provider, TLS 1.2/1.3) and `rustls-native-certs`
  (the system's CA store).

## Tests

```sh
cargo test                     # unit tests, with a fake server
cargo clippy --all-targets --all-features -- -D warnings
node --test tests/rust-client.test.mjs   # from the repository root: real servers
```

`tests/rust-client.test.mjs` builds `examples/client_test.rs` (into a directory in the temp
folder, or `KESTRO_CARGO_TARGET_DIR`) and runs it against `kestro-server`
(`KESTRO_SERVER_BINARY`): every parameter type, transactions, key/value versions, error
kinds, restarts, BUSY, TLS with a throwaway CA, and a 3-member cluster with a killed leader.

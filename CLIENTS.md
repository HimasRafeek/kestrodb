# Client libraries

KestroDB has the same SQL and key/value features in Node.js, PHP and Python, on Windows,
Linux and macOS. Each library can talk to `kestro-server` (many processes and machines)
or open a database file in its own process (embedded, one owning process at a time).
Other languages use the C API in `src/kestrodb.h`.

| | Node.js | PHP 7.4+ (64-bit) | Python 3.8+ |
|---|---|---|---|
| Server | `RemoteDB` (`kestrodb/remote`) | `KestroDB\Client` | `kestrodb.connect()` |
| Embedded | `Database` (`kestrodb`) | `KestroDB\Embedded` (FFI) | `kestrodb.open()` (ctypes) |
| Server + in-process reads | — | `['readFrom' => path]` | `read_from=path` |
| File | `index.mjs`, `remote.mjs` | `clients/php/KestroDB.php` (Composer: `kestrodb/kestrodb`) | `clients/python/kestrodb.py` |

## Parameters

Every SQL call takes parameters as a list for `?`, `?N` and `$N` placeholders, or as a
map (object, associative array, dict) for `:name` and `@name`. Map keys may include
the leading `:` or `@`. Named and unnamed placeholders cannot be mixed in one
statement. Values are JSON: strings, numbers, booleans, null; arrays and objects are
stored as JSON.

```sql
SELECT * FROM users WHERE city = ? AND age > ?        -- [ 'Kandy', 30 ]
SELECT * FROM users WHERE city = $1 OR home = $1      -- [ 'Kandy' ]
SELECT * FROM users WHERE city = :city AND age > :age -- { city: 'Kandy', age: 30 }
```

## Common API

| Node.js (`await` on RemoteDB) | PHP | Python | Returns |
|---|---|---|---|
| `query(sql, params)` | `query($sql, $params)` | `query(sql, params)` | all rows (objects / assoc arrays / dicts) |
| `queryOne(...)` | `queryOne(...)` | `query_one(...)` | first row or null |
| `queryValue(...)` | `queryValue(...)` | `query_value(...)` | first column of the first row, or null |
| `execute(...)` | `execute(...)` | `execute(...)` | rows changed (INSERT, UPDATE, DELETE, DDL) |
| `lastInsertId` | `lastInsertId()` | `last_insert_id` | id of the last row INSERT created |
| `transaction(fn)` | `transaction(fn)` | `with db.transaction():` | COMMIT, or ROLLBACK and rethrow on an error |
| `inTransaction` | `inTransaction()` | `in_transaction` | |
| `prepare(sql)` → `all/get/value/run` | `prepare($sql)` → PDO-style statement | `cursor()` (PEP 249) | |
| `sql(sql, params)` | `sql(...)` | `sql(...)` | the raw result: `{rows}` or `{changes, lastInsertId}` |
| `get / getMany / set / delete / batch` | same | `get / get_many / set / delete / batch` | key/value API |

Explicit transactions: `BEGIN`, `COMMIT` and `ROLLBACK` as SQL, or PHP
`beginTransaction()/commit()/rollBack()`, Python `begin()/commit()/rollback()`. A
transaction belongs to its connection (or embedded handle); it reads its own writes and
other connections do not see them until COMMIT. One transaction writes at a time; plain
statements from other connections wait for it, and are otherwise group-committed.

## Errors

Every error has a code (Node `err.code`, PHP `$e->kind`, Python `e.kind`):

| Code | Meaning | PHP class | Python class (PEP 249) |
|---|---|---|---|
| `CONFLICT` | unique constraint, duplicate id, failed `expectedVersion` | `ConflictException` | `IntegrityError` |
| `INPUT` | invalid SQL, parameters, names or values | `InputException` | `ProgrammingError` |
| `LIMIT` | a size or count limit | `LimitException` | `DataError` |
| `AUTH` | wrong token or not allowed | `AuthException` | `OperationalError` |
| `IO` | connection failed or lost; a write without a reply has an unknown outcome | `ConnectionException` | `OperationalError` |
| `CLOSED` | the connection or database was closed by the program | `ConnectionException` | `InterfaceError` |
| `BUSY` | the server is at its connection limit | `BusyException` | `OperationalError` |
| `READONLY` | a follower refused a write; in a cluster the client retries at the leader | `Exception` | `DatabaseError` |
| `UNAVAILABLE` | a cluster's majority did not confirm a write in time: unknown outcome | `Exception` | `DatabaseError` |
| `CORRUPT` | the file failed a checksum | `CorruptException` | `InternalError` |

All PHP classes extend `KestroDB\Exception`; all Python classes extend `kestrodb.KestroDBError`.

## Connections

- **Reconnect.** A connection the server closed while it was idle (restart, network) is
  reopened before the next request. Never inside a transaction: the next call then fails
  with `IO ... connection lost inside a transaction; it was rolled back`, and the one after
  reconnects. A request that was already sent when the connection broke fails with `IO`
  (for a write: unknown outcome). Turn it off with `reconnect: false` / `'reconnect' => false`.
- **Clusters.** Give every member's address (Node: an array or a comma-separated string;
  PHP: comma-separated; Python: a list or comma-separated). Writes go to the leader: a
  `READONLY` reply from a follower names it, and the client moves there and sends the
  request again (nothing was applied), also during an election, for up to `failoverTimeout`
  (Node, ms, and PHP, seconds; 15 s) or `failover_timeout` (Python, 15 s). Not inside a
  transaction: its connection went with its leader. `UNAVAILABLE` means a majority did not
  confirm a write in time: its outcome is unknown, as with `IO`. `db.address` (PHP
  `$db->address()`) is the member in use.
- **PHP-FPM.** `['persistent' => true]` keeps the connection open across requests in each
  worker. A transaction left open by a request that died is rolled back when the
  connection is reused.
- **TLS.** `tls://host:port`; see the README.
- **Threads.** One connection per thread (Python `threadsafety = 1`). Node's `RemoteDB`
  pipelines concurrent calls on one connection.

## Node.js

```js
import {Database} from 'kestrodb';          // embedded, synchronous
import {RemoteDB} from 'kestrodb/remote';   // server, async

const db = await RemoteDB.connect('tcp://127.0.0.1:7820', {token: process.env.KESTRODB_TOKEN});
await db.execute('INSERT INTO users (email, name) VALUES (:email, :name)', {email: 'a@example.com', name: 'Ann'});
const id = db.lastInsertId;
const user = await db.queryOne('SELECT * FROM users WHERE id = ?', [id]);
await db.transaction(async tx => {
  await tx.execute('UPDATE accounts SET balance = balance - ? WHERE id = ?', [10, 1]);
  await tx.execute('UPDATE accounts SET balance = balance + ? WHERE id = ?', [10, 2]);
});
const byCity = db.prepare('SELECT * FROM users WHERE city = ?');
const rows = await byCity.all(['Kandy']);
await db.close();
```

`Database` has the same methods, synchronous; its `transaction(fn)` also accepts an
async `fn`.

## PHP

```php
require 'clients/php/KestroDB.php';   // or Composer: kestrodb/kestrodb
use KestroDB\{Client, Embedded, ConflictException};

$db = new Client('tcp://127.0.0.1:7820', getenv('KESTRODB_TOKEN') ?: null, 5.0, ['persistent' => true]);
$db->execute('INSERT INTO users (email, name) VALUES (:email, :name)', ['email' => 'a@example.com', 'name' => 'Ann']);
$id = $db->lastInsertId();
$user = $db->queryOne('SELECT * FROM users WHERE id = ?', [$id]);
$db->transaction(function (Client $db) {
    $db->execute('UPDATE accounts SET balance = balance - ? WHERE id = ?', [10, 1]);
    $db->execute('UPDATE accounts SET balance = balance + ? WHERE id = ?', [10, 2]);
});
$st = $db->prepare('SELECT * FROM users WHERE city = :city');
$st->execute(['city' => 'Kandy']);
while ($row = $st->fetch()) { ... }          // also fetchAll(), fetchColumn(), rowCount(), foreach
try { $db->execute('INSERT INTO users (email) VALUES (?)', ['a@example.com']); }
catch (ConflictException $e) { ... }          // duplicate email

$local = Embedded::open('/data/app.kdb', true);   // no server; needs ext-ffi and the C library
```

`KestroDB\Embedded` and `['readFrom' => ...]` use PHP FFI (on by default for the CLI;
`ffi.enable=true` for PHP-FPM) and `kestrodb.dll` / `libkestrodb.so` (option `library`,
the `KESTRODB_LIBRARY` environment variable, or `../../bin`).

## Python (DB-API 2.0)

```python
import kestrodb

db = kestrodb.connect("tcp://127.0.0.1:7820", token=os.environ.get("KESTRODB_TOKEN"))
db.execute("INSERT INTO users (email, name) VALUES (:email, :name)", {"email": "a@example.com", "name": "Ann"})
user = db.query_one("SELECT * FROM users WHERE id = ?", [db.last_insert_id])
with db.transaction():
    db.execute("UPDATE accounts SET balance = balance - ? WHERE id = ?", [10, 1])
    db.execute("UPDATE accounts SET balance = balance + ? WHERE id = ?", [10, 2])

cur = db.cursor()                       # PEP 249
cur.execute("SELECT id, name FROM users WHERE city = ?", ["Kandy"])
for id_, name in cur: ...
cur.executemany("INSERT INTO users (email) VALUES (?)", [("b@example.com",), ("c@example.com",)])  # one transaction

local = kestrodb.open("app.kdb", create=True)   # embedded; the same methods
```

Connections start in autocommit mode: each statement commits alone, and transactions are
opened explicitly, because a KestroDB transaction holds the write lock until it ends. Set
`db.autocommit = False` for the PEP 249 behaviour where a cursor's first statement opens
a transaction that `commit()` or `rollback()` ends. `commit()` and `rollback()` without an
open transaction do nothing. Cursor rows are tuples in `SELECT` order; `description` names
the columns (it is `None` for a `SELECT` that returned no rows).

## Java

```java
import dev.kestrodb.*;   // clients/java: build.bat -> kestrodb.jar (Java 17+, no dependencies)

try (KestroClient db = KestroClient.connect("tcp://127.0.0.1:7820", System.getenv("KESTRODB_TOKEN"))) {
    db.execute("INSERT INTO users (email, name) VALUES (:email, :name)", Map.of("email", "a@example.com", "name", "Ann"));
    Map<String, Object> user = db.queryOne("SELECT * FROM users WHERE id = ?", db.lastInsertId());
    db.transaction(tx -> {
        tx.execute("UPDATE accounts SET balance = balance - ? WHERE id = ?", 10, 1);
        tx.execute("UPDATE accounts SET balance = balance + ? WHERE id = ?", 10, 2);
    });
    long v = db.set("users", "kestro", Map.of("name", "Kestro"));
    db.set("users", "kestro", Map.of("name", "K"), v);        // expectedVersion
}
Connection c = DriverManager.getConnection("jdbc:kestrodb://127.0.0.1:7820?token=" + token);  // minimal JDBC
```

`KestroClient` has the common API (`sql`, `query`, `queryOne`, `queryValue`, `execute`,
`lastInsertId()`, `transaction(fn)` / `transactionResult(fn)`, `begin/commit/rollback`,
`inTransaction()`, `prepare`, `get / getMany / set / delete / batch(Op...)`). Positional
parameters are varargs or a `List`; named ones a `Map`. Rows are ordered maps; JSON numbers are
`Long` when integral, else `Double`. Errors are unchecked `KestroException`s with `kind()`:
`ConflictException`, `InputException`, `LimitException`, `AuthException`,
`ConnectionException` (IO; `ClosedException` for CLOSED), `BusyException`, `ReadOnlyException`,
`UnavailableException`, `CorruptException`. Options: `new KestroClient.Options().timeout(...)
.reconnect(false).failoverTimeout(Duration.ofSeconds(15)).caFile("ca.pem")` (TLS: the JVM trust
store by default); a cluster is a comma-separated address list or a `List`, and `address()` is
the member in use. Calls are serialized, so threads may share a client, but use one per thread
for transactions. The JDBC driver (`jdbc:kestrodb://h1:7820,h2:7820?token=...`, a subset of
JDBC 4.2) maps errors to SQLExceptions with SQLStates and unwraps to `KestroClient`. No
embedded mode in Java. `tests/java-client.test.mjs` builds and runs it against kestro-server.

## .NET

```csharp
using KestroDB;   // clients/dotnet/KestroDB (.NET 8+, no dependencies); a minimal ADO.NET provider included

await using var db = await KestroClient.ConnectAsync("tcp://127.0.0.1:7820",
    new KestroOptions { Token = Environment.GetEnvironmentVariable("KESTRODB_TOKEN") });
await db.ExecuteAsync("INSERT INTO users (email, name) VALUES (:email, :name)", new { email = "a@example.com", name = "Ann" });
Row? user = await db.QueryOneAsync("SELECT * FROM users WHERE id = ?", db.LastInsertId);
await db.TransactionAsync(async tx =>
{
    await tx.ExecuteAsync("UPDATE accounts SET balance = balance - ? WHERE id = ?", 10, 1);
    await tx.ExecuteAsync("UPDATE accounts SET balance = balance + ? WHERE id = ?", 10, 2);
});
long v = await db.SetAsync("users", "kestro", new { name = "Kestro" });
await db.SetAsync("users", "kestro", new { name = "K" }, expectedVersion: v);
var list = conn.Query<User>("SELECT * FROM users WHERE city = @city", new { city = "Kandy" });   // Dapper on a KestroConnection
```

`KestroClient` has the common API, each call async (`SqlAsync`, `QueryAsync`, `QueryOneAsync`,
`QueryValueAsync`, `ExecuteAsync`, `TransactionAsync(fn)`, `BeginAsync(concurrent)/CommitAsync/RollbackAsync`,
`GetAsync / GetManyAsync / SetAsync / DeleteAsync / BatchAsync(Op...)`, `StatsAsync`) and blocking (`Query`,
`Execute`, `Transaction(fn)`, `KestroClient.Connect`, ...), plus `LastInsertId`, `InTransaction` and
`Address`. Positional parameters are arguments, an array or a list; named ones a dictionary or an
anonymous object. Rows are ordered `Row` maps; JSON numbers are `long` when integral, else `double`;
`QueryAsync<T>` and `row.Get<T>` / `As<T>` convert to your types. Errors are `KestroException`s
(`DbException`s) with `Kind`: `ConflictException`, `InputException`, `LimitException`, `AuthException`,
`KestroIOException` (`ClosedException` for CLOSED), `BusyException`, `ReadOnlyException`,
`UnavailableException`, `CorruptException`. Options: `new KestroOptions { Token, Timeout, Reconnect = false,
FailoverTimeout = TimeSpan.FromSeconds(15), CaFile = "ca.pem" }`; a cluster is a comma-separated address
list or a list. Calls are serialized, so tasks and threads may share a client; use one client per
concurrent unit of work for explicit transactions. `KestroConnection("Address=tcp://h1:7820,h2:7820;Token=...")`
is a minimal ADO.NET provider that Dapper uses. No embedded mode in .NET. `tests/dotnet-client.test.mjs`
builds and runs it against kestro-server.

## Rust

```rust
// clients/rust: crate kestrodb (std::net; features = ["tls"] for tls://)
use kestrodb::serde_json::json;
use kestrodb::{named_params, params, Client, Error, Op, Options};

let mut db = Client::connect("tcp://127.0.0.1:7820",
    Options { token: std::env::var("KESTRODB_TOKEN").ok(), ..Options::default() })?;
db.execute("INSERT INTO users (email, name) VALUES (:email, :name)",
           named_params! { "email" => "a@example.com", "name" => "Ann" })?;
let user = db.query_one("SELECT * FROM users WHERE id = ?", params![db.last_insert_id().cloned()])?;
db.transaction(|tx| {
    tx.execute("UPDATE accounts SET balance = balance - ? WHERE id = ?", params![10, 1])?;
    tx.execute("UPDATE accounts SET balance = balance + ? WHERE id = ?", params![10, 2])?;
    Ok::<_, Error>(())
})?;
let v = db.set("users", "kestro", json!({"name": "Kestro"}))?;
db.set_if("users", "kestro", json!({"name": "K"}), v)?;        // expected_version
```

`Client` has the common API: `sql` (`SqlResult { rows, changes, last_insert_id }`, `columns()`), `query`,
`query_one`, `query_value`, `query_as::<T>` (rows into any `Deserialize` type), `execute`,
`begin/commit/rollback`, `in_transaction()`, `transaction(|tx| ...)` (COMMIT, or ROLLBACK and return the
error or resume the panic), `get / get_as / get_many / set / set_if / delete / delete_if / batch(&[Op])`,
`stats`, `close()` (also on `Drop`). Rows are `serde_json::Map`s in `SELECT` order. Parameters are any
`Serialize` values: `params![..]` for `?`, `?N`, `$N`, `named_params!{..}` for `:name`/`@name`.
Non-finite floats are sent as null. Errors are `kestrodb::Error`, one variant per code: `Conflict`,
`Input`, `Limit`, `Auth`, `Io`, `Busy`, `ReadOnly`, `Unavailable`, `Corrupt`, `Closed`, `Other`.
`Options`: `token`, `timeout`, `connect_timeout`, `reconnect`, `failover_timeout`, and with the `tls`
feature (rustls, system CAs) `tls_ca_file`, `tls_verify`, `tls_server_name`, `tls_config`. A cluster is a
comma-separated string or a slice/`Vec`. One `Client` is one connection (`Send`): one per thread, or
behind a `Mutex`. Dependencies: `serde`, `serde_json` (with `tls`: `rustls`, `rustls-native-certs`).
`tests/rust-client.test.mjs` (cargo on `PATH`, in `~/.cargo/bin`, or `KESTRO_CARGO`) runs it against
kestro-server.

## Go

```go
import kestrodb "github.com/HimasRafeek/kestrodb/clients/go"   // Go 1.21+, standard library only

db, err := kestrodb.Connect(ctx, "tcp://127.0.0.1:7820", &kestrodb.Options{Token: os.Getenv("KESTRODB_TOKEN")})
defer db.Close()
_, err = db.Exec(ctx, "INSERT INTO users (email, name) VALUES (:email, :name)", kestrodb.Named{"email": "a@example.com", "name": "Ann"})
var name string
err = db.QueryRow(ctx, "SELECT name FROM users WHERE id = ?", 1).Scan(&name)   // kestrodb.ErrNoRows
err = db.Transaction(ctx, func(tx *kestrodb.Tx) error {
    _, err := tx.Exec(ctx, "UPDATE accounts SET balance = balance - ? WHERE id = ?", 10, 1)
    return err
})
v, err := db.Set(ctx, "users", "kestro", map[string]any{"name": "Kestro"})
_, err = db.SetIf(ctx, "users", "kestro", map[string]any{"name": "K"}, v)   // expected version
if errors.Is(err, kestrodb.ErrConflict) { /* ... */ }
sqlDB, err := sql.Open("kestrodb", "kestrodb://h1:7820,h2:7820?token="+url.QueryEscape(token))   // database/sql
```

`Client` has the common API with a `context.Context` first: `SQL` (a `*Result` with `Rows`, ordered
`Columns`, `Changes`, `LastInsertID`), `Query`, `QueryOne`, `QueryValue`, `Exec`, `QueryRow(...).Scan(...)`,
`Begin`/`BeginTx(ctx, &TxOptions{Concurrent: true})` → `*Tx`, `Transaction(ctx, fn)` (COMMIT, or ROLLBACK
on an error or panic), `Get / GetMany / Set / SetIf / Delete / DeleteIf / Batch(...)`. Named parameters are
a `kestrodb.Named` map or `sql.Named`. Errors are `*kestrodb.Error` with `Kind`; compare with `errors.Is`
and `ErrConflict`, `ErrInput`, `ErrLimit`, `ErrAuth`, `ErrIO`, `ErrClosed`, `ErrBusy`, `ErrReadOnly`,
`ErrUnavailable`, `ErrCorrupt`. Options: `Token`, `Timeout`, `ConnectTimeout`, `DisableReconnect`,
`FailoverTimeout`, `CAFile` / `TLSConfig` / `ServerName`. A `Client` is safe for concurrent use (calls are
serialized) and an open `Tx` holds it; for parallel work use database/sql (driver `kestrodb`), which pools
one `Client` per connection. SQL integers beyond 2^53 come back as floats (key/value values keep int64).
`tests/go-client.test.mjs` (`go` on `PATH` or `KESTRO_GO`) runs it against kestro-server.

## Tests

`tests/clients.test.mjs` runs the PHP and Python suites (`clients/php/test_api.php`,
`clients/python/test_dbapi.py`) against a real server and an embedded file, including a
server restart; `tests/sqlapi.test.mjs` covers Node. PHP and Python are found on `PATH`
or through `KESTRO_PHP` / `KESTRO_PYTHON`.

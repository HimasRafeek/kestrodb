# KestroDB client for .NET

A client for `kestro-server` for .NET 8+, with no dependencies outside the framework: SQL with
parameters, transactions, the key/value API with versions, error classes by code, reconnecting
idle connections, TLS (`SslStream`), and following a cluster's leader. Every call has an async
and a blocking form. A minimal ADO.NET provider (`KestroConnection`) makes Dapper work.

It talks to the server only (no embedded mode; use the C API in `src/kestrodb.h` through
P/Invoke if you need one).

## Build

```text
cd clients/dotnet/KestroDB
dotnet build -c Release          # bin/Release/net8.0/KestroDB.dll
dotnet pack -c Release           # or a NuGet package (KestroDB.0.9.0.nupkg)
```

Or reference `clients/dotnet/KestroDB/KestroDB.csproj` from your project, or copy its `.cs` files.

## Use

```csharp
using KestroDB;

await using var db = await KestroClient.ConnectAsync("tcp://127.0.0.1:7820",
    new KestroOptions { Token = Environment.GetEnvironmentVariable("KESTRODB_TOKEN") });

await db.ExecuteAsync("CREATE TABLE IF NOT EXISTS users (id INTEGER PRIMARY KEY, email TEXT UNIQUE, name TEXT, city TEXT)");
await db.ExecuteAsync("INSERT INTO users (email, name) VALUES (:email, :name)", new { email = "a@example.com", name = "Ann" });
object? id = db.LastInsertId;
Row? user = await db.QueryOneAsync("SELECT * FROM users WHERE id = ?", id);        // user["name"]
IReadOnlyList<Row> rows = await db.QueryAsync("SELECT * FROM users WHERE city = ?", "Kandy");
IReadOnlyList<User> typed = await db.QueryAsync<User>("SELECT * FROM users");      // by name, System.Text.Json
long n = await db.QueryValueAsync<long>("SELECT COUNT(*) FROM users");

await db.TransactionAsync(async tx =>
{
    await tx.ExecuteAsync("UPDATE accounts SET balance = balance - ? WHERE id = ?", 10, 1);
    await tx.ExecuteAsync("UPDATE accounts SET balance = balance + ? WHERE id = ?", 10, 2);
});                                                   // COMMIT, or ROLLBACK and rethrow

long v = await db.SetAsync("users", "kestro", new { name = "Kestro" });
Versioned? got = await db.GetAsync("users", "kestro");          // got.Version, got.Value; null if missing
await db.SetAsync("users", "kestro", new { name = "K" }, expectedVersion: v);   // only if still at v
await db.BatchAsync(Op.Set("kv", "a", 1), Op.Delete("kv", "b"));             // all or nothing
```

Drop the `Async` suffix (and `await`) for the blocking form: `db.Execute(...)`, `db.Query(...)`,
`db.Transaction(tx => ...)`, `KestroClient.Connect(...)`.

| Call | Returns |
|---|---|
| `SqlAsync(sql, params)` | `SqlResult`: `Rows` (null for a write), `Columns`, `Changes`, `LastInsertId`, `Raw` |
| `QueryAsync`, `QueryOneAsync`, `QueryValueAsync` (and `<T>`) | all rows, the first row or null, its first value or null |
| `ExecuteAsync` | rows changed |
| `BeginAsync(concurrent)`, `CommitAsync`, `RollbackAsync`, `InTransaction` | explicit transactions (`Commit`/`Rollback` with none open do nothing) |
| `TransactionAsync(fn)` / `TransactionAsync<T>(fn)` | COMMIT, or ROLLBACK and rethrow |
| `GetAsync`, `GetManyAsync`, `SetAsync`, `DeleteAsync`, `BatchAsync` | key/value with versions |
| `StatsAsync`, `VerifyAsync`, `BackupAsync`, `CompactToAsync`, `CompactAsync`, `PromoteAsync` | server calls |

Parameters: a list or array (or several arguments) for `?`, `?N` and `$N`; a dictionary or an
anonymous object for `:name` and `@name` (keys may include the leading `:` or `@`); one plain
value is the only positional parameter. Values: null, bool, every number type, string, Guid,
enums (by name), `DateTime` / `DateTimeOffset` / `DateOnly` / `TimeOnly` (ISO 8601 text), and
lists, dictionaries, anonymous objects and classes (JSON). NaN, infinities, `byte[]` and lone
surrogates are `INPUT` errors.

Rows are `Row`s: ordered maps (`IReadOnlyDictionary<string, object?>`) in SELECT order, also used
for JSON objects. Values are `long` (integral numbers), `double`, `string`, `bool`, null,
`List<object?>`, `Row`, or `BigInteger` beyond the long range. `row.Get<T>(name)` and
`Json.Convert<T>(value)` convert (long to int, text to Guid/DateTime/DateOnly/enum, rows to classes).

## Options

```csharp
new KestroOptions
{
    Token = "...",                               // null: none
    Timeout = TimeSpan.FromSeconds(30),          // per reply; also the connect timeout unless ConnectTimeout
    Reconnect = true,                            // reopen a connection closed while idle
    FailoverTimeout = TimeSpan.FromSeconds(15),  // how long a cluster client looks for the leader
    CaFile = "ca.pem",                           // tls://: trust these CAs (default: the system store)
    RemoteCertificateValidation = null,          // tls://: your own check instead
    ServerName = null, Verify = true, ClientCertificates = null,
}
```

A cluster is a comma-separated address list or a list of addresses; `Address` is the member in
use. Calls are serialized (a `SemaphoreSlim`), so tasks and threads may share a client; the body
of `TransactionAsync` has the client to itself until it ends. Use one client per concurrent unit
of work when they use explicit `BeginAsync`/`CommitAsync`. A cancelled call closes the
connection if the request was already sent (the next call reconnects).

## Errors

All errors are `KestroException` (a `System.Data.Common.DbException`) with `Kind`:
`ConflictException`, `InputException`, `LimitException`, `AuthException`, `KestroIOException`
(IO; `ClosedException` for CLOSED), `BusyException`, `ReadOnlyException`, `UnavailableException`,
`CorruptException`; other codes are plain `KestroException`s. `SqlState` and `IsTransient`
follow the kind.

## ADO.NET and Dapper

```csharp
using var conn = new KestroConnection("Address=tcp://127.0.0.1:7820;Token=...");
var users = conn.Query<User>("SELECT * FROM users WHERE city = @city", new { city = "Kandy" });   // Dapper
using var tx = conn.BeginTransaction();
conn.Execute("UPDATE users SET city = @c WHERE id = @id", new { c = "Galle", id = 1 }, tx);
tx.Commit();
```

Connection string keys: `Address` (or `Server`, `Host`, `Data Source`; comma-separated for a
cluster), `Token` (or `Password`), `Timeout`, `Connect Timeout`, `Failover Timeout` (seconds),
`Reconnect`, `CA File`, `Verify`, `Server Name`. Parameters are all named or all unnamed.
Readers hold the statement's rows; INTEGER columns read as `Int64`, REAL `Double`, BOOLEAN
`Boolean`, text `String`, JSON objects and arrays as their JSON text. `conn.Client` is the
`KestroClient`; `new KestroConnection(client)` wraps one. `KestroFactory.Instance` is the
`DbProviderFactory`.

## Tests

`tests/dotnet-client.test.mjs` builds `KestroDB.Tests` (a console program; it uses Dapper from
NuGet) into the temp directory and runs it against kestro-server: every parameter type,
transactions, BEGIN CONCURRENT, key/value versions, errors, ADO.NET and Dapper, restarts, BUSY,
TLS with a throwaway CA, and a 3-member cluster losing its leader.

```text
KESTRO_SERVER_BINARY=/path/to/kestro-server node --test tests/dotnet-client.test.mjs
```

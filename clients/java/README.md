# KestroDB client for Java

A client for `kestro-server` in plain Java 17+, with no dependencies: SQL with parameters,
transactions, the key/value API with versions, error classes by code, reconnecting idle
connections, TLS, and following a cluster's leader. A minimal JDBC driver is included.

It talks to the server only (there is no embedded mode in Java; use the C API in
`src/kestrodb.h` through JNI/Panama if you need one).

## Build

```text
clients\java\build.bat          (Windows; needs javac and jar from a JDK 17+)
```

This compiles `src/` into `build/classes` and makes `kestrodb.jar` (the JDBC driver is
registered in `META-INF/services`, so `DriverManager` finds it). On other systems:

```text
cd clients/java
javac --release 17 -d build/classes $(find src -name '*.java')
cp -r resources/* build/classes/
jar --create --file kestrodb.jar -C build/classes .
```

Or copy `src/dev/kestrodb` into your own source tree.

## Use

```java
import dev.kestrodb.*;
import java.util.*;

try (KestroClient db = KestroClient.connect("tcp://127.0.0.1:7820", System.getenv("KESTRODB_TOKEN"))) {
    db.execute("CREATE TABLE IF NOT EXISTS users (id INTEGER PRIMARY KEY, email TEXT UNIQUE, name TEXT, city TEXT)");
    db.execute("INSERT INTO users (email, name) VALUES (:email, :name)", Map.of("email", "a@example.com", "name", "Ann"));
    Object id = db.lastInsertId();
    Map<String, Object> user = db.queryOne("SELECT * FROM users WHERE id = ?", id);
    List<Map<String, Object>> rows = db.query("SELECT * FROM users WHERE city = ?", "Kandy");
    long n = (Long) db.queryValue("SELECT COUNT(*) FROM users");

    db.transaction(tx -> {
        tx.execute("UPDATE accounts SET balance = balance - ? WHERE id = ?", 10, 1);
        tx.execute("UPDATE accounts SET balance = balance + ? WHERE id = ?", 10, 2);
    });                                              // COMMIT, or ROLLBACK and rethrow

    long v = db.set("users", "kestro", Map.of("name", "Kestro"));
    Versioned got = db.get("users", "kestro");       // got.version(), got.value(); null if missing
    db.set("users", "kestro", Map.of("name", "K"), v);   // only if still at version v
    db.batch(Op.set("kv", "a", 1), Op.delete("kv", "b"));  // all or nothing
}
```

| Call | Returns |
|---|---|
| `sql(text, params...)` | `SqlResult`: `rows()` (null for a write), `changes()`, `lastInsertId()`, `raw()` |
| `query(...)`, `queryOne(...)`, `queryValue(...)` | all rows, the first row or null, its first value or null |
| `execute(...)` | rows changed |
| `prepare(text)` | `Prepared` with `all / one / value / run` |
| `begin()`, `commit()`, `rollback()`, `inTransaction()` | explicit transactions (`commit`/`rollback` with none open do nothing) |
| `transaction(tx -> ...)`, `transactionResult(tx -> value)` | COMMIT, or ROLLBACK and rethrow |
| `lastInsertId()` | id of the row the last INSERT created (`Long`, or `String` for a text id) |
| `get(collection, key)` | `Versioned` or null |
| `getMany(collection, keys)` | up to 4096, one snapshot, in order, null where missing |
| `set(collection, key, value[, expectedVersion])` | the new version (expectedVersion 0: the key must not exist) |
| `delete(collection, key[, expectedVersion])`, `batch(Op...)` | the transaction's version |
| `stats()`, `verify()`, `compact()`, `backup(name)`, `compactTo(name)`, `promote()` | server and admin calls |
| `address()`, `close()` | the member in use; end the connection |

**Parameters.** `sql`, `query`, `queryOne`, `queryValue` and `execute` take positional
parameters as varargs or a `List` (for `?`, `?N`, `$N`), or a `Map<String, ?>` for `:name`
and `@name` (keys may include the `:` or `@`). To pass one JSON array as the only
parameter, wrap it: `db.sql(text, List.of(myList))`. Values: `null`, `Boolean`, any
`Number` (`Float`/`Double` must be finite; `BigDecimal` and `BigInteger` are written
exactly), `String`/`CharSequence`/`Character`, `Map` and `Iterable`/arrays (stored as
JSON), and as text: `java.time` values, `java.sql.Date/Time/Timestamp`, `UUID`, enums
(their name). `byte[]` is refused (encode it, for example as Base64). A string with a lone
surrogate is refused (`INPUT`): it is not valid Unicode.

**Results.** Rows are `LinkedHashMap`s in `SELECT` order. JSON numbers are `Long` when
integral (`BigInteger` beyond the long range) and `Double` otherwise. JSON cannot tell
`2.0` from `2`, so a `REAL` column holding a whole number reads back as `Long`; use
`((Number) v).doubleValue()`. Arrays are `List`, objects `Map`. `Json.parse` and
`Json.stringify` are public if you need them.

## Errors

Every error is a `KestroException` (unchecked) with `kind()` (also `code()`), the
leading code of the message:

| Code | Class | Meaning |
|---|---|---|
| `CONFLICT` | `ConflictException` | unique constraint, duplicate id, failed `expectedVersion`; a failed COMMIT (`BEGIN CONCURRENT`) |
| `INPUT` | `InputException` | invalid SQL, parameters, names or values |
| `LIMIT` | `LimitException` | a size, count or time limit |
| `AUTH` | `AuthException` | wrong token or not allowed |
| `IO` | `ConnectionException` | connection failed or lost; a write without a reply has an unknown outcome |
| `CLOSED` | `ClosedException` (a `ConnectionException`) | the program closed the client |
| `BUSY` | `BusyException` | the server is at its connection limit |
| `READONLY` | `ReadOnlyException` | a follower refused a write; in a cluster the client retries at the leader first |
| `UNAVAILABLE` | `UnavailableException` | a cluster's majority did not confirm a write in time: unknown outcome |
| `CORRUPT` | `CorruptException` | the file failed a checksum |
| others (`STALE`, `UNSUPPORTED`, ...) | `KestroException` | |

## Connections

```java
KestroClient.Options o = new KestroClient.Options()
    .timeout(Duration.ofSeconds(30))          // reply timeout (and connect timeout unless set)
    .connectTimeout(Duration.ofSeconds(5))
    .reconnect(true)                          // reopen idle connections the server closed
    .failoverTimeout(Duration.ofSeconds(15))  // cluster: how long to look for the leader
    .caFile("ca.pem")                         // tls://: trust these CAs (default: the JVM's trust store)
    .serverName("db.example.com")             // tls://: name to send and verify, if not the address's host
    .verify(false);                           // tls://: no certificate checks (testing only)
KestroClient db = KestroClient.connect("tls://db.example.com:7820", token, o);
```

- **Addresses:** `tcp://host:port`, `tls://host:port`, `unix:///path` (Linux/macOS servers),
  or `host:port`. IPv6: `tcp://[::1]:7820`.
- **Reconnect.** A connection the server closed while it was idle (a restart, the network)
  is reopened before the next request (checked after a second of idleness). Never inside a
  transaction: that call fails with `IO ... connection lost inside a transaction; it was
  rolled back`, and the next one reconnects. A request already sent when the connection
  broke fails with `IO` (for a write: unknown outcome). `reconnect(false)` turns it off.
- **Clusters.** Give every member, comma-separated or as a `List`:
  `KestroClient.connect("tcp://h1:7820,tcp://h2:7820,tcp://h3:7820", token, o)`. Writes go to
  the leader: a follower's `READONLY` reply names it, and the client moves there and sends
  the request again (nothing was applied), also during an election, for up to
  `failoverTimeout`. Not inside a transaction: its connection went with its leader.
  `address()` is the member in use.
- **TLS.** TLS 1.2 or 1.3; the certificate and host name (or IP address) are verified. The
  default trust store is the JVM's (`cacerts`, or `-Djavax.net.ssl.trustStore=...`; on Windows
  `-Djavax.net.ssl.trustStoreType=Windows-ROOT` uses the Windows store). `caFile` trusts only
  the CA certificates in that PEM file; `sslContext(ctx)` uses your own context.
- **Threads.** Calls are serialized, so threads may share a client, but a transaction belongs
  to the connection: use one client per thread when threads open transactions.
- **BUSY.** A server at its limit sends `BUSY` and closes at once. If the client's login
  frame crosses it, the operating system may discard that reply; the client then fails with
  `IO cannot connect ... (the server closed the connection before login; it may be at its
  connection limit)`.

## JDBC

```java
Connection c = DriverManager.getConnection("jdbc:kestrodb://127.0.0.1:7820?token=" + token);
// or: jdbc:kestrodb:tls://db.example.com:7820?caFile=/etc/ca.pem
//     jdbc:kestrodb://h1:7820,h2:7820,h3:7820   (a cluster)
// Properties: token (or password), tls, caFile, verify, serverName, timeout,
//             connectTimeout, failoverTimeout (seconds), reconnect
try (PreparedStatement p = c.prepareStatement("SELECT id, name FROM users WHERE city = ?")) {
    p.setString(1, "Kandy");
    try (ResultSet rs = p.executeQuery()) { while (rs.next()) { long id = rs.getLong("id"); } }
}
KestroClient db = c.unwrap(KestroClient.class);   // the key/value API on the same connection
```

A subset of JDBC 4.2 (`jdbcCompliant()` is false): `Statement` and `PreparedStatement`
(positional parameters, `setX`/`setObject`, `executeQuery/Update/execute`, batches run in
one transaction, `getGeneratedKeys` with the last insert id, `setMaxRows`), scrollable
read-only `ResultSet`s held in memory with the usual getters and conversions (numbers,
booleans, ISO dates and times, JSON as text), `ResultSetMetaData` (types inferred from the
values), basic `DatabaseMetaData`, auto-commit on by default (off: the first statement
opens a transaction that `commit()`/`rollback()` ends; DDL never opens one). Errors are
`SQLException`s with a SQLState (`23000` CONFLICT, `42000` INPUT, `28000` AUTH, `08006` IO,
...) and the `KestroException` as cause. Other methods throw `SQLFeatureNotSupportedException`.

## Tests

`node --test tests/java-client.test.mjs` builds the client and runs
`test/dev/kestrodb/ClientTest.java` against real servers: SQL with every parameter type,
transactions, savepoints and `BEGIN CONCURRENT` conflicts, key/value versions and conflicts,
every error class, JDBC, threads, reconnecting after server restarts, `BUSY`, TLS with a
throwaway CA (needs `openssl`), and following a 3-member cluster's leader through a failover.
It is skipped when `javac` is not found (or set `KESTRO_JAVAC` / `KESTRO_JAVA`).

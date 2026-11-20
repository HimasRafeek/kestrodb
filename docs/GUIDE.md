# KestroDB guide

The full reference for KestroDB 0.9: the Node API, the server, users and TLS, monitoring,
backups, replication and failover, durability, and building from source. Start with the
[README](../README.md); SQL is in [SQL.md](../SQL.md) and the other languages' clients in
[CLIENTS.md](../CLIENTS.md).

## Contents

- [Getting started](#getting-started)
- [Windows release package](#windows-release-package)
- [SQL](../SQL.md)
- [API](#api)
- [Server mode and other languages](#server-mode-and-other-languages)
- [Replication](#replication) and [automatic failover](#cluster-automatic-failover)
- [What KestroDB does not do](#what-kestrodb-does-not-do)
- [Performance](#performance)
- [Platform status](#platform-status)
- [Durability and integrity](#durability-and-integrity)
- [Index snapshot](#index-snapshot)
- [Moving a database between systems](#moving-a-database-between-systems)
- [Advanced: the worker process](#advanced-the-worker-process)
- [Building and testing](#building-and-testing)

## Getting started

From a release package or npm (see [Install](#install)), or from source:

```text
npm run build
npm test
node example.mjs
```

### Windows release package

`kestrodb-0.9.0-windows-x64.zip` (from `node scripts/package-windows.mjs` after a Release
build) needs nothing installed besides Node.js 22+ for the Node API, or PHP 8 / Python 3
for those clients: the binaries depend only on Windows system DLLs.

| Path | What it is |
|---|---|
| `bin/kestro-server.exe` | The server (`kestro-server --db app.kdb --create --tcp 127.0.0.1:7820 --token-file token.txt`) |
| `bin/kestrodb.dll`, `include/kestrodb.h` | The C library (embedded use, in-process reads beside the server) |
| `bin/kestro.node`, `index.mjs`, `remote.mjs`, ... | The Node API: `import {Database} from './kestrodb-0.9.0-windows-x64/index.mjs'` |
| `clients/php/KestroDB.php`, `clients/python/kestrodb.py` | PHP and Python clients (they find `bin/kestrodb.dll` themselves) |
| `README.md`, `SQL.md`, `CLIENTS.md`, `FORMAT.md`, `CHANGELOG.md` | Documentation |
| `SHA256SUMS.txt` | Checksums of every file in the package |

Keep the folder layout as it is. The zip is not code-signed, so Windows SmartScreen may
warn the first time `kestro-server.exe` runs.

## Install

Prebuilt for **Windows x64**, **Linux x64** and **Linux arm64**. Linux needs glibc 2.34 or newer
(Ubuntu 22.04+, Debian 12+, RHEL/Rocky/Alma 9+, Fedora 35+, Amazon Linux 2023); on Linux,
`kestro-server` also needs OpenSSL 3 (`libssl3` / `openssl-libs`). Elsewhere (macOS, Alpine,
older Linux) build from source: `npm run build` (CMake 3.20+, a C++17 compiler, Linux: libssl-dev).

The packages below are built by `scripts/package.mjs` into `dist/` and attached to each
[GitHub release](https://github.com/HimasRafeek/kestrodb/releases); they are not on npm, PyPI,
Packagist or Docker Hub yet, so install them from the files. A checkout of the repository
has no binaries (`bin/` is build output): run `npm run build` there.

**Node.js 22+**

    npm install ./kestrodb-0.9.0.tgz

    import {Database} from 'kestrodb';
    const db = new Database('app.kdb', {create: true});

Server: `npx kestro-server --db app.kdb --create --tcp 127.0.0.1:7820 --token-file token.txt`;
clients `import {RemoteDB} from 'kestrodb/remote'`. Under systemd or another service manager run
`node_modules/.bin/kestro-server` (not `npx`, which does not pass SIGTERM on).

**Python 3.8+**

    pip install kestrodb-0.9.0-py3-none-<platform>.whl

    import kestrodb
    db = kestrodb.open("app.kdb", create=True)                          # embedded
    db = kestrodb.connect("tcp://127.0.0.1:7820", token="...")          # server

The wheel contains the C library; `KESTRODB_LIBRARY=/path/to/libkestrodb.so` overrides it.

**PHP 7.4+**: put `kestrodb-php-0.9.0.zip` in a folder and point Composer at it:

    {"repositories": [{"type": "artifact", "url": "path/to/folder"}],
     "require": {"kestrodb/kestrodb": "0.9.0"}}

Server connections need nothing else. Embedded mode and in-process reads need the FFI extension
(`extension=ffi`; with PHP-FPM also `ffi.enable=true`). The library is found in
`native/<os>-<arch>/` inside the package; `KESTRODB_LIBRARY` overrides it.

**Release archives**: `kestrodb-0.9.0-windows-x64.zip`, `kestrodb-0.9.0-linux-x64.tar.gz`,
`kestrodb-0.9.0-linux-arm64.tar.gz`: `bin/` (server, worker, Node addon, C library), the Node API,
the PHP and Python clients, `include/kestrodb.h` and `SHA256SUMS.txt`. Check a download with
`sha256sum -c kestrodb-0.9.0-linux-x64.tar.gz.sha256`.

**Linux service (systemd)**

    tar -xzf kestrodb-0.9.0-linux-x64.tar.gz
    sudo kestrodb-0.9.0-linux-x64/packaging/install.sh --dry-run    # see what it will do
    sudo kestrodb-0.9.0-linux-x64/packaging/install.sh

Installs to `/opt/kestrodb`, runs as user `kestrodb` with data in `/var/lib/kestrodb`, listens on
`127.0.0.1:7820`, settings in `/etc/kestrodb/kestrodb.env`, a new admin token in
`/etc/kestrodb/token`. `systemctl status kestrodb`, `journalctl -u kestrodb`.

**Docker**

    docker build -t kestrodb .
    openssl rand -hex 24 > token.txt && chmod 644 token.txt
    docker run -d --name kestrodb -p 127.0.0.1:7820:7820 -v kestrodb-data:/data \
      -v "$PWD/token.txt:/run/secrets/kestrodb_token:ro" -e KESTRODB_TOKEN_FILE=/run/secrets/kestrodb_token kestrodb

Extra server options go after the image name. `docker-compose.example.yml` has a single server
(`--profile single`) and a three-member cluster (`--profile cluster`). The image runs as a
non-root user with a health check. Kubernetes secrets are symlinks, which the server refuses
today: copy the token into the container's filesystem.

**Windows service**: `kestro-server.exe` runs as a service under NSSM or WinSW (see
[`packaging/windows/README.md`](../packaging/windows/README.md)); it does not talk to the Service Control Manager itself yet.

## API

```js
const db = new Database(path, {create: false, ...options});
```

| Call | What it does |
|---|---|
| `db.sql(text, params?)` | Runs one SQL statement ([SQL.md](../SQL.md)). `SELECT` returns `{rows}`; writes return `{changes, lastInsertId?}`. Params: an array for `?`/`$1`, or an object for `:name`. |
| `db.query`, `db.queryOne`, `db.queryValue`, `db.execute` | Rows; the first row; its first value; the number of rows changed (`db.lastInsertId` after an INSERT). |
| `db.transaction(fn)`, `db.prepare(sql)` | `fn` inside BEGIN ... COMMIT (ROLLBACK if it throws); a prepared statement with `all`, `get`, `value`, `run`. See [CLIENTS.md](../CLIENTS.md). |
| `db.set(collection, key, value, {expectedVersion}?)` | Saves a JSON value. Returns `{version, operations}`. |
| `db.get(collection, key)` | `{version, value}`, or `null` if missing. |
| `db.getMany(collection, keys)` | Up to 4,096 keys from one consistent snapshot; keeps order, duplicates and `null`s. |
| `db.delete(collection, key, {expectedVersion}?)` | Removes a key. |
| `db.batch([...operations])` | Several `set`/`delete` operations as one all-or-nothing transaction with one version. |
| `db.setAsync`, `db.deleteAsync`, `db.batchAsync` | The same writes, returning promises. Writes made at the same time share one disk flush (group commit), which is much faster for servers. |
| `db.stats()` | Key count, file size, last version, and how the database was opened. |
| `db.verify()` | Re-checks the whole file against its checksums. |
| `db.backup(path)` | Writes a verified copy to a new file, online: writes continue while it copies. |
| `db.archive(dir)`, `Database.restore(source, dest, {txid, time})` | Continuous archive and point-in-time restore; see [Backups and restore](#backups-and-restore). |
| `db.compactTo(path)` | Writes a copy with only the current values (drops old versions). |
| `db.compact()` | Online compaction: rewrites this file to hold only current values while reads and writes continue. |
| `db.close()` | Waits for queued async writes, then closes. `Database` also supports `using`. |

- **Collections** are names like `users` (letters, digits, `_`, up to 64 characters).
  **Keys** are any text up to 512 bytes. **Values** are JSON, up to 256 KiB each.
- **Versions** let you update safely: pass `{expectedVersion: v}` and the write fails with
  `CONFLICT` if someone else changed the key since you read version `v`. `0` means
  "must not exist yet".
- **Batches** look like `{type: 'set' | 'delete', collection, key, value?, expectedVersion?}`.
  [BULK.md](../BULK.md) and `bulk.mjs` stream large imports in batches.

Open options (all optional):

| Option | Default | Meaning |
|---|---|---|
| `create` | `false` | Create a new file. Refuses to overwrite an existing one. |
| `jsonValidation` | `'strict'` | `'strict'` rejects values that `JSON.stringify` would silently change (Dates, Maps, getters, sparse arrays, unsafe integers). `'basic'` stores what `JSON.stringify` returns, like most databases' Node bindings, and writes faster. |
| `verifyOnOpen` | `false` | Finish verifying the whole file before `new Database` returns, instead of in the background (see [Index snapshot](#index-snapshot)). |
| `readMode` | `'auto'` | `'auto'` reads through memory mapping; `'pread'` uses file reads. |
| `orderedSync` | `false` | Linux/macOS: flush data before metadata, then again before returning. |

Errors from the engine start their message with a code: `INPUT`, `LIMIT`, `CONFLICT`
(a failed `expectedVersion`), `LOCKED` (another process has the file open), `CORRUPT`
or `UNKNOWN_COMMIT` (an I/O failure left a write's outcome unknown). Invalid arguments
caught in JavaScript throw an error whose `code` property is `INPUT` or `LIMIT`.

One process owns a database file at a time. Inside that process, reads are fast enough
to call synchronously, and async writes are group-committed on a background thread.
To share a database between processes, see the next section.

## Server mode and other languages

`kestro-server` owns one database file and serves it to many clients at once over TCP or
a Unix socket, the way MySQL or PostgreSQL would:

```text
openssl rand -hex 24 > token.txt          # a long random token
kestro-server --db /var/lib/app/app.kdb --create --tcp 127.0.0.1:7820 --token-file token.txt
```

`--create` makes the file on the first start and opens it on later ones, so a service can keep
the flag. Options: `--tcp HOST:PORT` (repeatable), `--unix PATH` with `--socket-mode 600`
(Linux/macOS), `--token-file FILE` or the `KESTRODB_TOKEN` environment variable,
`--max-connections 1024`, `--verify-on-open`. It prints `KESTRODB READY ...` when
listening and stops cleanly (writing its index snapshot) on Ctrl+C or SIGTERM.

Limits that keep one client from stalling the others (all can be changed):

| Option | Default | What it does |
|---|---|---|
| `--statement-timeout` | `30s` | a statement that runs longer fails with `LIMIT` (long scans and joins hold up commits) |
| `--transaction-timeout` | `60s` | a connection idle inside `BEGIN` is closed and its transaction rolled back (it holds the write gate) |
| `--auth-timeout` | `10s` | a client must send its token this soon; before it, requests are at most 64 KB |
| `--idle-timeout` | none | closes connections idle this long outside transactions |
| `--max-connections-per-ip` | `256` | connections from one address (loopback clients are not limited) |

Ten failed logins from one address within a minute block logins from it for a minute. A reply
or result over 64 MB fails with `LIMIT` (ask for fewer rows), and a transaction may change at
most 1 GB (savepoints count their copies). Without `--users`/`--token-file` the server listens
only on loopback addresses unless `--no-auth` says otherwise. `backup(name)` and
`compactTo(name)` from a client write the plain file name `name` into the server's
`--backup-dir`, and are refused without one.

Writes from all connections that arrive together share one disk flush, so throughput
grows with the number of clients. Each write is still durable before its reply.

**Users and roles.** `--users FILE` lists one user per line: `name role token [tables=a,b]`.

```text
# name     role       token (or sha256:HEX)                         [tables=...]
admin      admin      sha256:<64 hex digits printed by kestro-server --hash-token>
app        readwrite  long-random-app-token-here
reporting  readonly   long-random-report-token      tables=orders,customers
```

| Role | May |
|---|---|
| `readonly` | read (`get`, `getMany`, `SELECT`), stats (no `BEGIN`: a transaction holds the write gate) |
| `readwrite` | also write (`set`, `delete`, `batch`, `INSERT`, `UPDATE`, `DELETE`) |
| `admin` | also `CREATE`/`DROP`/`ALTER`, `verify`, `backup`, `compactTo` |

A user with `tables=` may only use those tables (key/value collections count as tables);
every table a SQL statement touches is checked: joins, subqueries anywhere (`ORDER BY`,
`LIMIT`, `HAVING`, ...), `UNION`/`INTERSECT`/`EXCEPT` parts, and the tables a write reaches
through foreign keys (children its actions change or its checks read, parents it reads).
`SHOW TABLES` lists only its tables. `tables=` cannot limit an `admin` or `replica` (backups,
compaction and replication cover every table). Only an `admin` may change rows of a SQL table
with key/value writes; other users use SQL, where the table's constraints apply. Store
`sha256:` hashes rather than tokens: `echo TOKEN | kestro-server --hash-token` prints one.
Tokens must be at least 16 characters (32 or more random characters are recommended; shorter
ones get a warning); a wrong token is refused after a short delay. A
single `--token-file` (or `KESTRODB_TOKEN`) still works and makes that token an admin. Errors
for refused requests start with `AUTH`.

**TLS.** `--tls-cert cert.pem --tls-key key.pem` makes every `--tcp` listener TLS-only
(TLS 1.2 or newer; Unix sockets stay plain). Clients connect to `tls://host:port` and verify
the certificate: Node `RemoteDB.connect(addr, {tls: {caFile}})`, PHP
`['tls' => ['cafile' => ...]]`, Python `tls={"cafile": ...}` (system CAs by default).
Linux and macOS builds use OpenSSL; Windows builds use the system's Schannel (no extra
software). On Windows the certificate file may hold the chain (server certificate first),
the key may be PKCS#8 (`BEGIN PRIVATE KEY`), RSA (`BEGIN RSA PRIVATE KEY`) or EC
(`BEGIN EC PRIVATE KEY`), unencrypted; or give `--tls-pfx server.pfx
[--tls-pfx-password-file pw.txt]` instead. Because Schannel only uses keys held by the
Windows key store, the server keeps its key there (non-exportable, under the account
running the server) while it runs and deletes it when it stops (a server that was killed
has its key deleted at the next start); intermediate CA certificates are likewise placed in
the account's Intermediate Certification Authorities store while the server runs.
Followers and cluster members reach `tls://` leaders on every platform, checking the
certificate against the system CAs or only those in `--replica-ca ca.pem`.

**Client certificates (mutual TLS).** `--tls-client-ca ca.pem` makes every TLS client show
a certificate issued by those CAs (`--tls-client-cert required`, the default once a CA is
given); with `--tls-client-cert optional` a client may show none, but never an invalid one.
A missing, untrusted, expired or non-client certificate is refused before any request is
served: the client gets `AUTH TLS client certificate required/refused: <reason>` and the
server logs it. In the users file, `cert=cn:NAME` or `cert=sha256:FINGERPRINT` (from
`openssl x509 -noout -fingerprint -sha256`) binds a user to a certificate: `-` as its token
logs in by the certificate alone (`alice readonly - cert=cn:alice`), a token as well
requires both (`bob readwrite <token> cert=sha256:AB:CD:...`); token users keep working.
Followers and cluster members show a certificate with `--replica-cert cert.pem
--replica-key key.pem`, so a `replica` user bound to it needs no token file. Clients: Node
`{tls: {ca, cert, key}}`, Python `tls={'cafile', 'certfile', 'keyfile'}`, PHP
`['tls' => ['cafile', 'local_cert', 'local_pk']]` (Go, Rust, Java and .NET: not yet). Only
the CAs in `--tls-client-ca` are trusted (not the system store); revocation lists (CRL,
OCSP) are not checked, and the CA file must hold the root.

In-process readers (`readFrom`, `read_from`) read the database file directly, so file
permissions, not the users file, control them.

**Clients** (all with SQL, named parameters, transactions, prepared statements, error
classes, reconnect, and `get`, `getMany`, `set`, `delete`, `batch` with versions, so they
share one database; the full reference is [CLIENTS.md](../CLIENTS.md)):

| Language | File | Reads on the server's machine |
|---|---|---|
| PHP 7.4+ (64-bit) | `clients/php/KestroDB.php` (Composer `kestrodb/kestrodb`; `KestroDBEmbedded` without a server) | in-process with `readFrom` (PHP FFI) |
| Python 3.8+ | `clients/python/kestrodb.py` (DB-API 2.0) | in-process with `read_from` (ctypes) |
| Node.js | `remote.mjs` (`RemoteDB`, import `kestrodb/remote`) | use `Database` in-process when it is the only process |
| Any other (Go, Java, C#, Rust, Ruby, C/C++...) | `src/kestrodb.h` + `kestrodb.dll` / `libkestrodb.so` (a release archive's `bin/`, or `npm run build`) | C API: reader and embedded modes |

```php
require 'clients/php/KestroDB.php';
$db = new KestroDB\Client('tcp://127.0.0.1:7820', getenv('KESTRODB_TOKEN'), 5.0,
                          ['readFrom' => '/var/lib/app/app.kdb']);   // optional, fastest reads
$v = $db->set('users', 'kestro', ['name' => 'Kestro']);    // through the server, durable
$db->get('users', 'kestro');                              // in this PHP process, sees $v
$db->execute('INSERT INTO orders (user, total) VALUES (:u, :t)', ['u' => 'kestro', 't' => 12.5]);
$rows = $db->query('SELECT * FROM orders WHERE user = ?', ['kestro']);
$db->transaction(fn($db) => $db->execute('UPDATE orders SET total = 0 WHERE user = ?', ['kestro']));
```

```python
import kestrodb
db = kestrodb.connect("tcp://127.0.0.1:7820", token=TOKEN, read_from="/var/lib/app/app.kdb")
db.set("users", "kestro", {"name": "Kestro"})
db.get("users", "kestro")
rows = db.query("SELECT * FROM orders WHERE user = :u", {"u": "kestro"})
with db.transaction():
    db.execute("UPDATE orders SET total = 0 WHERE user = ?", ["kestro"])
cur = db.cursor(); cur.execute("SELECT id FROM orders"); cur.fetchall()   # DB-API 2.0

local = kestrodb.open("app.kdb", create=True)   # embedded: this process owns the file
```

**In-process reads** (`readFrom`, `read_from`, or `KESTRO_OPEN_READER` in the C API)
open the database file without taking ownership; any number of processes can do it while
the server writes. Before every read the reader looks at the file's commit slot (a few
memory loads) and, if it moved, checks and applies only the new transactions, so a read
always sees every write acknowledged before it, with no network round trip. Values are
checksum-verified as in every other mode. The C library stays loaded for the life of the
process and keeps readers open, so a PHP-FPM worker pays the open cost once, not per
request. PHP needs the FFI extension (on by default for the CLI; set `ffi.enable=true`
for PHP-FPM). If the database file is replaced (a restored backup), readers report
`STALE`; restart the workers.

### Monitoring and administration

**Metrics and health checks.** Start the server with a metrics endpoint beside the database port:

```
kestro-server --db app.kdb --users users --tcp 0.0.0.0:7820 \
  --metrics 127.0.0.1:9187                       # this machine only
# or, for a remote Prometheus:
  --metrics 0.0.0.0:9187 --metrics-token-file scrape.token
```

- `GET /metrics`: Prometheus text format, every name prefixed `kestrodb_`: connections, requests by
  type, SQL statements by kind, errors by code, statement and commit latency histograms, bytes in
  and out, database size, keys, index and snapshot memory, compactions, backups (last success
  timestamp), replication role and lag, sync replication, cluster term/leader/elections, and
  LISTEN/NOTIFY.
- `GET /health/live`: 200 while the process runs.
- `GET /health/ready`: 200 when the database is open and healthy; 503 when synchronous
  replication is degraded, a cluster has no leader, or a follower is disconnected or behind by more
  than `--ready-max-lag` (10 s by default). The reason is in the JSON body.

A non-loopback metrics address needs `--metrics-token-file` (every path then needs
`Authorization: Bearer TOKEN`) or an explicit `--metrics-public`. The endpoint is plain HTTP
(put a proxy in front for HTTPS).

```yaml
scrape_configs:
  - job_name: kestrodb
    authorization: {credentials_file: /etc/kestrodb/scrape.token}
    static_configs: [{targets: ['db1:9187']}]
```

**Slow-query log.** `--slow-query-ms 200` writes every statement slower than 200 ms as a JSON line
to `--slow-query-log FILE` (stderr by default):

```
{"time":"2026-11-20T08:43:56.882Z","ms":220.4,"user":"app","peer":"10.0.0.7","connection":12,"kind":"select","rows":1,"sql":"SELECT * FROM orders WHERE customer = ? AND total > ?"}
```

Parameter values and inline literals are never logged unless you add `--slow-query-params`.
Statements are cut at 4 KB; the file rotates to `FILE.1` past `--slow-query-log-max` MB (64).
A background thread writes it: if the disk falls behind, lines are dropped and counted
(`kestrodb_slow_query_log_dropped_total`); requests never wait for the disk.

**Admin command.** `kestro-server admin` connects like a client (`--server tcp://HOST:PORT`,
`unix://PATH` or `tls://HOST:PORT` with `--ca`, `--cert/--key`; the token from `--token-file` or
`KESTRODB_TOKEN`):

```
kestro-server admin --server tcp://db1:7820 --token-file admin.token status
kestro-server admin ... stats --json | verify | backup nightly.kdb | compact
kestro-server admin ... connections           # id, user, peer, since, in transaction, listening
kestro-server admin ... kill 42               # closes it; its open transaction is rolled back
kestro-server admin ... slow 20               # the newest slow statements
kestro-server admin --metrics-url http://db1:9187 --metrics-token-file scrape.token metrics
```

`status` and `stats` work for any user; every other command needs the admin role. Exit codes: 0 ok,
1 error, 2 usage.

### Capacity

Measured with `node bench/stress.mjs` (Windows, i5-14400): every connection sends one request
at a time like a PHP worker, 90% point reads and 10% durable writes, 100,000 keys.

| Connections | Operations/s | Durable writes/s | p99 latency | Errors | Server memory |
|---:|---:|---:|---:|---:|---:|
| 100 | 206,710 | 20,340 | 1.1 ms | 0 | 27 MB |
| 1,000 | 176,995 | 17,622 | 9.1 ms | 0 | 55 MB |
| 2,000 | 174,578 | 17,547 | 16.7 ms | 0 | 94 MB |
| 4,000 | 168,786 | 16,965 | 34.6 ms | 0 | 172 MB |

Throughput holds as connections grow; latency grows with the queue (requests wait
their turn). Each connection costs about 40 KB plus a thread; the default limit is
`--max-connections 1024`. The key index uses at most about 32 MB of RAM by default (`indexMemory` in Node, `--index-memory MB` for kestro-server; 0 keeps it all in RAM). Past that it moves to a memory-mapped file next to the database (`<file>.idx`), and RAM keeps only recent changes: 5,000,000 keys used 53 MB in kestro-server (was 327 MB), and 2,000,000 keys need about 16 MB of index RAM with random reads as fast as before (about 1.7 million per second in-process). The OS keeps the index pages in use in its file cache and reclaims them when other programs need memory, so the number of keys is limited by disk, not RAM. The index file is a cache: if it is missing, stale or damaged, it is rebuilt from the database log. Ordered keys and secondary indexes (used by SQL ORDER BY, ranges and indexed WHERE) have their own budget, 16 MB by default (`orderedMemory` in Node, `--ordered-memory MB` for kestro-server): past it their least recently changed pages move to a scratch file (`<file>.sets`, deleted on close) read through the OS cache, and they are built with an external sort, so even the first build never holds every key in RAM. A 1,000,000-row table with two indexes needs about 10 MB for them instead of 118 MB, at the same or better speed. Copy only the .kdb file for backups; stats() reports indexMode, indexMemory, indexFileBytes, orderedMemory and orderedFileBytes.

### SQL server against SQL server: kestro-server vs MariaDB

Windows 10, i5-14400, 2026-11-19. One Node client, both servers over TCP on the same
machine, the same table (unique email, indexes on city and age), the same statements and
parameters, 20,000 rows, median of 3 runs. MariaDB 11.4.4
runs InnoDB with `innodb_flush_log_at_trx_commit=1` (a flush per commit) and the binary
log off; KestroDB replies after its flush. Operations per second, higher is better.

| Test | KestroDB | MariaDB | |
|---|---:|---:|---|
| Bulk insert, 500 rows per statement | **227,500** rows | 123,500 | 1.8x |
| Durable single-row INSERT | **10,700** | 3,800 | 2.8x |
| SELECT by id | **29,800** | 14,100 | 2.1x |
| SELECT by unique column | **32,400** | 13,200 | 2.5x |
| Indexed WHERE + ORDER BY + LIMIT 10 | **4,400** | 1,100 | 4.0x |
| UPDATE by id (durable) | **11,000** | 3,700 | 3.0x |
| COUNT(*) with LIKE (full scan) | **1,100** | 487 | 2.3x |
| GROUP BY with COUNT/AVG (full scan) | **593** | 174 | 3.4x |
| SELECT by id, 16 connections | **75,800** | 30,400 | 2.5x |
| Durable INSERT, 16 connections | **34,500** | 29,600 | 1.2x |

Many connections committing single rows was the hard case (at first 6,200/s against
MariaDB's 32,000/s): KestroDB now group-commits SQL statements from all connections,
runs the next group's statements while the previous group is being written to disk,
and waits for one disk write per commit (see FORMAT.md, "The commit point"). Reproduce with
`node bench/server-sql-compare.mjs` (needs the mysql2 driver; see the file's header).

### SQL server against SQL server: kestro-server vs PostgreSQL

Windows 10, i5-14400, 2026-11-19, same method as above. PostgreSQL 18.7 (official EDB
binaries, default settings: `fsync=on`, `synchronous_commit=on`, `wal_sync_method=open_datasync`),
the `pg` driver with named prepared statements. Median of 3 runs, operations per second.

| Test | KestroDB | PostgreSQL | |
|---|---:|---:|---|
| Bulk insert, 500 rows per statement | **250,400** rows | 133,000 | 1.9x |
| Durable single-row INSERT | **11,700** | 11,200 | 1.05x |
| SELECT by id | **31,700** | 21,600 | 1.5x |
| SELECT by unique column | **33,700** | 24,300 | 1.4x |
| Indexed WHERE + ORDER BY + LIMIT 10 | **4,500** | 1,600 | 2.8x |
| UPDATE by id (durable) | **11,700** | 9,000 | 1.3x |
| COUNT(*) with LIKE (full scan) | **1,300** | 679 | 1.9x |
| GROUP BY with COUNT/AVG (full scan) | **638** | 404 | 1.6x |
| SELECT by id, 16 connections | **78,300** | 39,500 | 2.0x |
| Durable INSERT, 16 connections | **38,500** | 33,100 | 1.2x |

Single-connection durable writes remain the close case (1.05x here, 1.1x on 2026-11-17;
PostgreSQL first led 10.8K to 9.4K): a single commit now installs while its write is in flight (readers and the
touched shards stay locked until it is durable), the commit record is computed during
the write, and the committing thread spins briefly on the write instead of sleeping.
Reproduce with `KESTRO_PG_URL=postgres://... node bench/server-sql-compare.mjs`.

### Many processes: PHP and Python

Windows 10, i5-14400 (10 cores, 16 threads), 2026-11-15. N processes at once, as
PHP-FPM or gunicorn workers would run. 300 durable single writes and 20,000 random point
reads per process, ~230-byte JSON documents, 20,000 keys. Durability is matched: KestroDB
replies after its flush; SQLite (PDO / `sqlite3`) runs WAL with `synchronous=FULL`;
MySQL 8.0 runs InnoDB with `innodb_flush_log_at_trx_commit=1` (binary log off, which
favours MySQL). Every engine stores JSON text and decodes it on read. Higher is better.

PHP 8.2:

| Processes | Writes: KestroDB | SQLite | MySQL | Reads: KestroDB | SQLite | MySQL |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | **5,100/s** | 3,200/s | 2,600/s | **351,000/s** | 106,000/s | 19,000/s |
| 4 | **14,100/s** | 3,400/s | 4,400/s | **1,127,000/s** | 307,000/s | 57,000/s |
| 16 | **18,900/s** | 3,000/s | 11,600/s | **1,403,000/s** | 469,000/s | 160,000/s |

Python 3.13:

| Processes | Writes: KestroDB | SQLite | Reads: KestroDB | SQLite |
|---:|---:|---:|---:|---:|
| 1 | **4,500/s** (embedded **9,200/s**) | 3,800/s | **170,000/s** (embedded 173,000/s) | 96,000/s |
| 4 | **17,100/s** | 3,000/s | **671,000/s** | 270,000/s |
| 16 | **22,900/s** | 2,800/s | **874,000/s** | 416,000/s |

Python 3.12 on Linux (Ubuntu 24.04, shared 2-vCPU VPS, `results/python-linux.json`):

| Processes | Writes: KestroDB | SQLite | Reads: KestroDB | SQLite |
|---:|---:|---:|---:|---:|
| 1 | **3,700/s** (embedded **6,500/s**) | 2,300/s | **198,000/s** (embedded 201,000/s) | 132,000/s |
| 2 | **5,200/s** | 2,500/s | **322,000/s** | 218,000/s |
| 4 | **5,200/s** | 2,200/s | **310,000/s** | 202,000/s |

PHP 8.3 on the same Linux server (`results/php-linux.json`):

| Processes | Writes: KestroDB | SQLite | Reads: KestroDB | SQLite |
|---:|---:|---:|---:|---:|
| 1 | **4,000/s** | 2,400/s | **482,000/s** | 181,000/s |
| 2 | **5,400/s** | 2,700/s | **633,000/s** | 323,000/s |
| 4 | **7,000/s** | 2,400/s | **770,000/s** | 278,000/s |

Reading through the server instead (one loopback round trip per read) measured 38,000/s
(1 PHP process) to 248,000/s (16): faster than MySQL, slower than in-process SQLite,
which is why the clients read in-process when they can. Write numbers vary ±20% between
runs on this machine. Python was not compared with MySQL (no MySQL driver installed).
A re-run on 2026-11-17 against MariaDB 11.4.4 (PHP 8.3, same settings) gave durable writes
KestroDB 7,900 / 18,900 / 23,800 per second vs MariaDB 4,000 / 10,100 / 20,800 at 1 / 4 / 16
processes, and point reads 368,000 / 1,226,000 / 1,338,000 vs 24,000 / 71,000 / 167,000.
Reproduce with `node bench/lang-compare.mjs --lang php|python` (add
`--mysql "mysql:host=...;dbname=bench"`); raw results are in `results/php-windows.json`
and `results/python-windows.json`; `node scripts/lang-results.mjs` refreshes the
"PHP and Python, many processes" section of `index.html`.

## Backups and restore

KestroDB's log is append-only: committed bytes never change. Backups are therefore
**online**: the commit point is read under a short lock and the committed bytes are
copied while writes continue. Every copy is checked (SHA-256 and the chain of every
transaction) before it is published under its final name, in bounded memory.

**Scheduled backups** (kestro-server): `--backup-dir DIR --backup-every 1h --backup-keep 7`
writes `DIR/<name>-YYYYMMDD-HHMMSS.kdb` (UTC) on that interval and keeps the newest N. A
backup is a normal database file: open it, copy it elsewhere, or restore from it.

**Continuous archive** (point-in-time recovery): `--archive-dir DIR --archive-every 1s`
keeps `DIR/archive.kdb`, a valid database holding every committed transaction archived so
far, and `DIR/timeline` (`unix_ms txid end` per step). Each step appends only what was
committed since the last one, so it costs what was written, not the database size; the
server takes a last step when it stops. Put the directory on another disk or sync it to
another machine (it only grows at the end, so `rsync --append` works). If the database
file is replaced (a restore, a compacted copy), the next step keeps the old archive as
`archive-<ms>.kdb` and starts a new one.

**Restore** (offline, into a new file; the server never overwrites):

```text
kestro-server --restore DIR/archive.kdb --to restored.kdb --time 2026-11-17T14:32:10Z   # state then
kestro-server --restore DIR/archive.kdb --to restored.kdb --txid 48211                  # after that transaction
kestro-server --restore backups/app-20261117-140000.kdb --to restored.kdb               # a whole backup
```

`--time` takes ISO 8601 (UTC `Z` or `+HH:MM`) or unix milliseconds and picks the last
archived point at or before it (precision: the archive interval); `--txid` cuts exactly
after that transaction (`stats().txid` and every write's version are transaction ids).
Then start the server on the restored file. Embedded apps do the same in-process: Node
`db.archive(dir)` / `Database.restore(source, dest, {txid, time})`, PHP
`$db->archive($dir)` / `Embedded::restore(...)`, Python `db.archive(dir)` /
`kestrodb.restore(...)`, C `kestro_backup` / `kestro_archive` / `kestro_restore`.

### Reclaiming space (online compaction)

Updates and deletes leave old versions in the append-only log. `compact()` rewrites the
file to hold only current values **while reads and writes continue**: current values go
to a new file shard by shard (writers to a shard wait only while that shard is copied),
transactions committed meanwhile are appended, and writes pause only for the last few
transactions and the switch. The new file is checked in full (replayed) before it
replaces the old one; a crash at any point leaves the database whole. Readers in other
processes (`readFrom`) see `STALE` and reopen. Server: `RemoteDB.compact()`, PHP
`$db->compact()`, Python `db.compact()` (admin role). Embedded: `Database.compact()`,
`Embedded::compact()`, `Local.compact()`, C `kestro_compact`. It returns the committed
bytes before and after.

## Replication

A follower is a second kestro-server holding a byte-identical copy of the leader's log. It
serves reads (SQL, key/value, in-process `readFrom` readers on its machine) and refuses
writes with `READONLY` until it is promoted.

```text
# on the leader: a user with the replica role (it may only replicate)
repl  replica  long-random-replica-token
# on the follower machine:
kestro-server --db /var/lib/app/replica.kdb --replica-of tls://leader.example.com:7820 \
             --replica-token-file replica-token.txt [--replica-ca ca.pem] --tcp 0.0.0.0:7820 --users users.txt
```

- **Seeding**: a follower whose file does not exist yet copies the leader's log first
  (online; the leader keeps serving), then streams.
- **Streaming**: the follower asks for the groups after its own end, giving the hash of
  its last group; the leader answers at once or waits up to a second for the next commit,
  so changes arrive within milliseconds. The follower commits each transaction through its
  normal commit path with the same expectations: its log, hashes and chain come out
  byte-identical, and its key index, SQL tables, secondary indexes and column cache follow
  (catalog changes, such as `CREATE INDEX` or `ALTER TABLE`, included). Each transaction
  is durable on the follower before the next is requested.
- **Safety**: a follower that no longer continues the leader's log (the leader was
  compacted or restored) stops with `STALE`; a different chain after applying stops with
  `DIVERGED`. Delete the follower's file and restart it to reseed. Network errors and leader
  restarts only cause a reconnect (with back-off).
- **Status**: `stats().replica` on the follower: `state` (streaming, reconnecting,
  stopped, promoted), `leaderTxid`, `lagTransactions`, `lastContactMs`, `lastError`;
  `stats().readOnly`.
- **Synchronous replication**: by default a commit is acknowledged by the leader before
  followers have it, so a leader lost without warning can take its last transactions with
  it. Start the leader with `--sync-replicas N` and a write (an autocommit SQL write, a
  COMMIT, or a key/value write) is answered only once N followers have it durably: a leader
  that dies right after answering loses nothing (RPO 0). A follower's next replication
  request confirms everything before it, so this costs one network round trip per commit
  group (concurrent writers share it), not one per write. If fewer than N followers confirm
  within `--sync-timeout` (5 s), the leader logs `KESTRODB SYNC degraded` and goes on
  alone (`stats().sync.degraded`) until a follower catches up (`KESTRODB SYNC restored`).
  With `--sync-strict` it never goes on alone: writes wait for the followers however long it
  takes (availability traded for zero data loss). `stats().sync`: `followers`,
  `acknowledgedEnd`, `degraded`, `waits`, `timeouts`.
- **Failover (manual)**: stop writes to the old leader, wait until `lagTransactions` is 0
  (or, with `--sync-replicas`, at once), then `promote()` the follower (admin; Node
  `RemoteDB.promote()`, PHP `$db->promote()`, Python `db.promote()`) and point clients at
  it. For automatic failover, run a cluster (below).
- A follower's file must stay identical: compaction and writes are refused on it; back it up
  and archive it like any other database.

### Cluster: automatic failover

Three (or five) kestro-servers, each listing every member, elect one leader among
themselves. The leader takes the writes; the others follow it and serve reads. When the
leader fails, the others elect a new one within about two election timeouts (under a second
with `--election-timeout 400ms` on a local network; 1.5 s is the default) and clients carry on.

```text
# users.txt on every member: the cluster token has the replica role
admin    admin    long-random-admin-token
cluster  replica  long-random-cluster-token
# on each machine (the list is the same everywhere; --cluster-self names this machine):
kestro-server --db /var/lib/app/db.kdb --create --tcp 0.0.0.0:7820 --users users.txt \
  --cluster tcp://db1:7820,tcp://db2:7820,tcp://db3:7820 --cluster-self tcp://db1:7820 \
  --cluster-token-file cluster-token.txt
```

- **Clients** are given every member: Node
  `RemoteDB.connect('tcp://db1:7820,tcp://db2:7820,tcp://db3:7820', ...)` (or an array), PHP
  `new KestroDB\Client('tcp://db1:7820,tcp://db2:7820,tcp://db3:7820', ...)`, Python
  `kestrodb.connect(['tcp://db1:7820', ...])`. A member that follows answers a write with
  `READONLY ... the leader is tcp://db2:7820` (nothing was applied); the client moves there
  and sends it again, also while an election is under way (up to `failoverTimeout`, 15 s).
  Reads are served by whichever member the client is connected to; a follower's reads can
  trail the leader by its replication lag.
- **No acknowledged write is lost.** A write is answered once a majority of the members (the
  leader included) has it on disk. A member votes only for a candidate whose log is at least
  as up to date as its own (by the term of its last transaction, then its length), so any
  later leader has every answered write. A write that cannot reach a majority within
  `--sync-timeout` (5 s), or whose leader steps down first, fails with `UNAVAILABLE`: its
  outcome is unknown (as after a lost connection), so check before retrying.
- **One leader.** Terms and votes are flushed to `<db>.cluster` before a member answers. A
  leader that cannot hear from a majority for one election timeout steps down, so a leader cut
  off from the others stops taking writes. Elections start with a pre-vote, and members that
  hear from their leader refuse to vote, so a member that was cut off cannot depose a working
  leader when it returns.
- **Rejoining.** A member that was away catches up from the leader. A former leader holding
  writes no majority confirmed (it was cut off while they were in flight) finds where its log
  and the leader's part, cuts its log back to there (`KESTRODB CLUSTER log cut back to
  transaction N`), restarts in-process and follows. A member without its file (new, or a
  replaced disk) copies the leader's log first: start it without `--create`, which is for
  forming a new cluster (every member starts with an empty log).
- **Status**: `stats().cluster`: `self`, `term`, `role` (leader, follower, candidate),
  `leader`, `members`, `majority`, `elections`, `lastLeaderContactMs`; `stats().sync`.
- **Sizes**: three members survive the loss of one, five the loss of two. Two members survive
  neither one's loss (a majority of two is two).
- Compacting the leader makes each follower copy its log again (it no longer continues
  their copies): plan it for a quiet time. `promote()` is refused in a cluster.
- The cluster token is the members' secret: a client holding it could start elections.

## Viewer

A small web page to browse a database: tables with row counts, rows page by page, each
table's columns and indexes, and a SQL console.

```text
npm run viewer                                                    # pick a file or a server in the page
npm run viewer -- --db app.kdb                                    # a file nobody else has open
npm run viewer -- --server tls://db.example.com:7820 --token-file token.txt --ca ca.pem
```

Then open http://127.0.0.1:7821. **Open…** switches databases at any time: browse folders
and drives (KestroDB files are highlighted), paste a path, or connect to a server; recent
choices are remembered in the browser. A file that another program has open (for example
kestro-server) cannot be opened directly; connect to that server instead. It is read-only unless started with `--allow-writes`, listens
on 127.0.0.1 (another `--host` needs `--viewer-token-file`), and keeps itself small: no
dependencies, no work while idle, one query at a time, at most 1,000 rows and 2 MB per
answer, and a CPU governor (`--cpu-budget 2`, percent of the machine) that makes requests
wait when it is over budget. Measured on the i5-14400 (16 threads, 32 GB): 0.02% CPU and
about 66 MB while browsing; 1.95% CPU when flooded with JOIN queries. Node itself needs
about 60 MB, so on a small server run the viewer on your own computer and connect to
`kestro-server` over `tls://`.

## What KestroDB does not do

KestroDB has SQL tables, queries, aggregates, secondary and unique indexes, joins of every
kind, correlated subqueries, `UNION`, window functions, `CASE`, enforced foreign keys,
`CHECK` and `UNIQUE` constraints, transactions with savepoints, and `ALTER TABLE`. Today it
has **no**:

- **deferred constraints or `NATURAL` joins**; see [SQL.md](../SQL.md#not-supported-yet)
  (full-text search, `LISTEN`/`NOTIFY` and serializable `BEGIN CONCURRENT` arrived on
  2026-11-20);
- **a column cache for row-returning scans**: aggregates over a whole table use an
  in-memory column cache and beat SQLite, but a `SELECT` that returns whole rows without
  an index reads and parses every row;
- **several writing processes on one file**: one process owns the file (a second owner
  gets `LOCKED`); other processes write through `kestro-server` and can read the file
  directly;
- **read-then-write transactions in the key/value API**: there, batches are atomic writes
  and `expectedVersion` gives optimistic concurrency; use SQL `BEGIN ... COMMIT` for
  transactions that read and then write;
- **automatic space reclamation**: old versions stay in the file until you run an online
  `compact()` (or `compactTo` a new file); nothing compacts on a schedule yet;
- **native extensions for each language**: PHP and Python use FFI/ctypes over the C
  library, and other languages need a small binding over `src/kestrodb.h`;
- **clusters across regions**: a cluster answers a write once a majority has it, so members
  far apart add their round trip to every write; in-process reads need the database file
  on the same machine as the server;
- **years of production use**: it is fuzzed, power-loss tested, run under the address and
  thread sanitizers and soak-tested in CI and on Windows (see [VALIDATION.md](../VALIDATION.md)), but it has
  not yet run real production workloads for long.

## Performance

The current key/value and SQL results, on Windows and Linux, are in the
[README](../README.md#performance). How they are measured:

The engines alternate inside each phase and their order rotates with every phase and run:
an engine that runs right after another's background work (LevelDB compacts on its own
threads) pays for it, and with a fixed order that was always KestroDB (on Linux its mixed
row measured 248K against LMDB's 451K that way; 574K against 361K with the rotation, 523K against 405K in the latest run). Point
reads one at a time take about 22 ms per engine, so they move the most with machine load; an
earlier full run had LMDB 3% ahead there (743K against 722K/s). Since then a point read's
integrity check costs about 30 ns less (the checksum loads its input a word at a time and
computes only the half it compares), and KestroDB has led that row in every full run since (1.15–1.20×).

MVCC costs nothing measurable: the 0.9.0 build before it and this one, alternated on the
same machine (five rounds each), are within a few percent on every key/value row, the new
build ahead on most; point `SELECT`s became 60–75% faster with their direct path.

Since 0.8.0 (2026-11-15): durable writes got about twice as fast (one at a time 10.5K ->
18-24K across runs; Windows flush timing moves this row the most). The open row (31.7M -> 21.0M keys/s)
measures close + open + one read, ten times: since commits write only their group (the
commit slot is written without waiting, see FORMAT.md), the first close after writes
flushes the slot once (about 1 ms on Windows); a reopen without writes in between takes
about 0.67 ms (30M keys/s). Opening no longer lists the database's directory (it did since
online compaction, which made opens in busy directories slower: 1.55 ms -> 0.70 ms with
2,000 other files). The rows move by 10-15% between single runs on this machine when other programs run (the
mixed row measured 493K-578K today), so changes are judged with the builds alternated.

Compared: SQLite through `node:sqlite` and `better-sqlite3` (WAL, `synchronous=FULL`),
LMDB through `lmdb-js` (synced commits) and LevelDB through `classic-level`
(`sync: true`). Open `index.html` for the full leaderboard, ranked charts, progress
across versions and each engine's exact settings. These results come from one machine,
and single runs on it move by 10-15% when other programs run, more than some read
margins above; run the benchmark on your own hardware before relying on them.

How the comparison is set up:

- **JSON:** the benchmark opens KestroDB with `jsonValidation: 'basic'`, the same work the
  other engines do (`JSON.stringify`). The strict default costs more per write: in-process
  bulk load measured 302,000/s strict against 519,000/s basic.
- **Reads:** KestroDB checks every value against a checksum before returning it; LMDB
  does not.
- **Open:** each engine closes and reopens its database ten times and reads one key each
  time (one ~3 ms reopen was too noisy to rank). Runs recorded before run 6 in
  `index.html` timed a single reopen.

Reproduce:

```text
cd bench/competitors && npm install && cd ../..
node bench/compare.mjs --runs 5
node bench/sql-compare.mjs --runs 5
node bench/compare.mjs --runs 5 --record "what changed"   # adds a run to index.html
```

The other databases live in `bench/competitors` with their own `package.json`, so
KestroDB itself has no npm dependencies.

## Platform status

| Target | Validation of 0.9 (see [VALIDATION.md](../VALIDATION.md)) |
|---|---|
| Windows x64 | MSVC 2022, Windows 10: the portable C++ suites, the power-loss sweep, every Node suite (PHP, Python and Java clients included), the benchmarks, 3-hour and 1-hour soaks; GitHub's Windows runner on every push. |
| Linux x86-64 | GCC 13 and clang 18, Ubuntu 24.04: the portable, POSIX and power-loss suites, every Node suite (Node, PHP, Python, Java, Go, Rust and .NET clients), AddressSanitizer + UBSan, ThreadSanitizer, libFuzzer, the benchmarks; release binaries built on Ubuntu 22.04 (glibc 2.34) and tested on Ubuntu 24.04 and Rocky Linux 9. |
| Linux ARM64 | GitHub's ubuntu-24.04-arm runner on every push; the release binaries also run under QEMU (Debian 12, Node 24 for arm64). |
| macOS (Apple Silicon) | Passed on GitHub's macOS runner through 2026-11-19; not built for releases (build from source). |

Linux commits use `fdatasync`. `.github/workflows/platforms.yml` runs the suites on Linux x64,
Linux ARM64 and Windows, the sanitizers and fuzzing on every push and pull request;
`scripts/linux-check.sh` runs the Linux checks locally (for example under WSL2).

## Durability and integrity

Use a local file system with working locks and flushes: NTFS on Windows, local POSIX
file systems elsewhere. Network shares, cloud-sync folders and multi-host shared disks
are not supported.

- **Windows:** the database handle is opened write-through, so each write completes only
  after it reaches stable media. Commits overwrite preallocated space, and the engine
  calls `FlushFileBuffers` whenever it grows that space, so the file's size is durable
  before any commit relies on it.
- **Linux:** `fdatasync` before each acknowledgement (data, plus the file size when it
  changes; commits overwrite preallocated space). **macOS:** `F_FULLFSYNC`.
- Every transaction is stored with a SHA-256 hash chained to the previous one, and every
  value is checked against a checksum when read. Damaged data is reported as `CORRUPT`
  and never returned; the engine does not repair it, so keep backups.

Storage hardware must honor flush requests (write-through relies on forced-unit-access,
as SQL Server does). No physical power-loss testing has been performed. An I/O failure
can leave a write's outcome unknown (`UNKNOWN_COMMIT`): reopen and check before
retrying. [FORMAT.md](../FORMAT.md) and [PORTABILITY.md](../PORTABILITY.md) hold the on-disk contract and platform details.

## Index snapshot

On a clean close, and in the background once commits have been quiet for 50 ms,
KestroDB writes an **index snapshot** into the end of the file: every key with its
location, version and checksum, as one hash table per shard. The next open uses it
directly instead of re-reading the whole log.

- **Compatible.** It sits after the last committed byte, which every version since 0.4
  treats as leftover space and discards. Older versions just re-read the log; so does
  0.8 when the snapshot is missing, stale, damaged or from another file.
- **Lazy.** Lookups read the snapshot's tables straight from the mapped file. Each of its
  64 sections is checksummed the first time it is used; a damaged one makes KestroDB
  rebuild the index from the log and carry on. The first write builds the in-memory
  index; read-only sessions never pay for it.
- **Verified in the background.** Once the database has been open for 50 ms, a
  background thread performs the same checks a full re-read would (every transaction's
  SHA-256, the hash chain, every value's checksum). If it finds damage, every later
  operation fails with `CORRUPT`. A database closed within 50 ms skips this; reads still
  check every value they return. With `verifyOnOpen: true` the full check finishes before
  open returns, so a damaged file fails to open. `stats()` reports `openedFrom`, `verification` and `indexMode`.
- **Crash-friendly.** Snapshots are also written when writes go quiet, so the first open
  after a crash is still fast, provided nothing was committed after the last snapshot.
- **Checksum key.** Value checksums are keyed. After a snapshot open, the key is the one
  stored in the snapshot: checksums still detect damaged bytes, but no longer stop
  someone who can rewrite the file (the same level as the file's SHA-256 chain).

## Moving a database between systems

The file format (unchanged since 0.6 apart from the KDB spelling of its markers; files from
HimasDB, the earlier name, open and keep working) uses explicit little-endian integers and
no pointers or OS-specific layout. Close the database or use `backup()`, copy the file,
open it and call `verify()`. Copying a closed file copies its index snapshot too;
`backup()` and `compactTo()` copy committed data only, and the copy writes a snapshot
when it is next closed.

## Advanced: the worker process

`worker/client.mjs` (`import {NativeDB} from 'kestrodb/worker'`) runs the same engine in
a separate process (`bin/kestro-worker`) and talks to it over a pipe. Every call is async.
It is **slower** (each read is a round trip between processes) and most apps should use
`Database`. Reasons to use it anyway:

- a crash in native code ends only the worker, not your Node process;
- your environment cannot load a native addon (`kestro.node`);
- you want even disk flushes kept off Node's process entirely.

```js
import {NativeDB} from 'kestrodb/worker';

const db = await NativeDB.open('./app.kdb', {create: true});
await db.set('users', 'kestro', {name: 'Kestro'});
console.log(await db.get('users', 'kestro'));
await db.close();
```

It accepts the same options. Requests
issued together travel in one pipe write, and consecutive writes are group-committed.
The test suite also uses the worker to inject crashes at exact points of a commit,
which cannot be done to the test's own process.

## Building and testing

Required everywhere: CMake 3.20+, a C++17 compiler and Node.js 22+. BLAKE3 and the
Node-API headers are bundled.

| OS | Prerequisites |
|---|---|
| Windows x64 | Visual Studio 2022 Build Tools with "Desktop development with C++" and a Windows SDK; CMake on PATH. |
| Linux | GCC or Clang, Make or Ninja, OpenSSL 3 development headers (`libssl-dev`). |
| macOS | Xcode Command Line Tools and CMake. |

`npm run build` puts the addon (`kestro.node`), the worker and the native tests in `bin/`.
`npm test` runs the native C++ suite (through CTest) and the Node suite in `tests/`.
CMake options: `-DKESTRO_NODE_ADDON=OFF` skips the addon, `-DKESTRO_PORTABLE_HASH=ON`
selects portable BLAKE3. On Windows the addon's import library is generated from
`cmake/node_api.def`, and it loads only in a host executable named `node.exe`. Only
64-bit targets are supported.

# KestroDB

**A fast, durable SQL database in one file, embedded in your app or run as a server.**

KestroDB is a C++17 database engine that you open like SQLite: in-process from Node.js,
Python or PHP, with no server to run. When several processes or machines need the same
data, `kestro-server` serves it over TCP or TLS, with users and roles, replication and
automatic failover. Clients exist for Node.js, Python, PHP, Java, .NET, Go and Rust.

```js
import {Database} from 'kestrodb';

const db = new Database('app.kdb', {create: true});
db.sql('CREATE TABLE users (id INTEGER PRIMARY KEY, email TEXT UNIQUE, name TEXT, age INT)');
db.sql('INSERT INTO users (email, name, age) VALUES (?, ?, ?)', ['ada@example.com', 'Ada', 36]);
db.query('SELECT name, age FROM users WHERE age >= ? ORDER BY age DESC', [18]);
// → [{name: 'Ada', age: 36}]
db.close();
```

> **Status: 0.9, before 1.0.** The engine is tested hard (see [Validation](#validation)),
> but it has not yet run in production for long, and APIs may still change before 1.0.
> Every file format since 0.4 still opens, and keeping old files readable is a rule of the
> project. Test it with your workload, and keep backups.

## Why KestroDB

- **Every write is on disk before the call returns**, and concurrent writes share one disk
  flush (group commit), so durable throughput grows with load instead of collapsing.
- **Fast**: first on every row of its key/value and SQL benchmarks against SQLite, LMDB
  and LevelDB, on Windows and Linux ([Performance](#performance)).
- **Standard SQL**: the common core of PostgreSQL, MySQL and SQLite, with joins, subqueries,
  `WITH RECURSIVE`, window functions, composite and unique indexes, foreign keys, `CHECK`
  constraints, savepoints, `ALTER TABLE` and full-text search ([SQL.md](SQL.md)).
- **Readers never block writers**: long queries read a snapshot (MVCC), and
  `BEGIN CONCURRENT` transactions run side by side and are serializable.
- **Integrity checked everywhere**: transactions are chained with SHA-256, every value is
  checked when read, and damage is reported as `CORRUPT`, never returned. Fuzzed,
  power-loss tested and run under AddressSanitizer and ThreadSanitizer.
- **A server when you need one**: users and roles, TLS and client certificates, online
  backups and point-in-time restore, replication, a three-server cluster that fails over
  on its own, `LISTEN`/`NOTIFY`, Prometheus metrics and an admin command.
- **Small**: about 2 MB of binaries, about 5 MB of RAM for an idle server, and memory
  budgets that move the key index to disk when it outgrows RAM.

## Install

Prebuilt binaries for **Windows x64**, **Linux x64** and **Linux arm64** (glibc 2.34 or newer:
Ubuntu 22.04+, Debian 12+, RHEL/Rocky/Alma 9+, Fedora 35+, Amazon Linux 2023). Elsewhere,
[build from source](#building-from-source).

**Node.js 22+**

```text
npm install kestrodb
```

**Python 3.8+, PHP 7.4+, Docker, release archives**: download them from the
[latest release](https://github.com/HimasRafeek/kestrodb/releases/latest):

```text
pip install kestrodb-0.9.0-py3-none-<platform>.whl        # bundles the C library
docker build -t kestrodb .                                 # from a clone of this repository
```

The release archives (`kestrodb-0.9.0-<platform>.zip` / `.tar.gz`) hold the server, the C
library (`include/kestrodb.h`), the Node API and the PHP and Python clients; Linux archives
include a systemd unit and installer. The [guide](docs/GUIDE.md#install) covers each.

## Use it

### Embedded, in your process

```js
import {Database} from 'kestrodb';
const db = new Database('shop.kdb', {create: true});      // later opens: omit create

db.sql('CREATE TABLE orders (id INTEGER PRIMARY KEY, customer TEXT NOT NULL, total REAL)');
db.sql('CREATE INDEX ON orders (customer)');
db.transaction(() => {
  db.sql('INSERT INTO orders (customer, total) VALUES (?, ?)', ['ada', 42.5]);
  db.sql('INSERT INTO orders (customer, total) VALUES (?, ?)', ['ada', 17]);
});
db.queryOne('SELECT customer, COUNT(*) AS n, SUM(total) AS spent FROM orders GROUP BY customer');
// → {customer: 'ada', n: 2, spent: 59.5}

// Key/value, for the fastest access by key (values are JSON):
db.set('sessions', 'u42', {user: 42, expires: '2026-12-01'});
db.get('sessions', 'u42');                  // → {version, value: {user: 42, expires: '2026-12-01'}}
await db.setAsync('sessions', 'u43', {user: 43});   // concurrent writes share one flush
```

Python and PHP open files the same way, through the C library:

```python
import kestrodb
db = kestrodb.open("shop.kdb", create=True)
db.sql("SELECT * FROM orders WHERE customer = ?", ["ada"])
```

### As a server

```text
openssl rand -hex 24 > token.txt
kestro-server --db app.kdb --create --tcp 127.0.0.1:7820 --token-file token.txt
```

```js
import {RemoteDB} from 'kestrodb/remote';
const db = await RemoteDB.connect('tcp://127.0.0.1:7820', {token: process.env.KESTRODB_TOKEN});
await db.sql('SELECT COUNT(*) AS n FROM orders');
```

| Language | Client | Embedded (same file, in-process) |
|---|---|---|
| Node.js | `kestrodb` (`Database`, `RemoteDB`) | yes |
| Python | `kestrodb.py` (DB-API 2.0) | yes, through the C library |
| PHP | `KestroDB.php` (Composer) | yes, through FFI |
| Java | JDBC driver | server |
| .NET | ADO.NET provider | server |
| Go | `database/sql` driver | server |
| Rust | `kestrodb` crate | server |
| C, others | `include/kestrodb.h` | yes |

[CLIENTS.md](CLIENTS.md) documents every client. Many processes on one machine can write
through the server and read the file directly in their own process.

## Features

| Area | What you get |
|---|---|
| SQL | `SELECT` with every join kind, subqueries, `UNION`/`INTERSECT`/`EXCEPT`, `WITH` and `WITH RECURSIVE`, derived tables, `GROUP BY`/`HAVING`, window functions with frames, `CASE`, nested JSON fields (`address.city`); `INSERT ... ON CONFLICT`, `UPDATE`, `DELETE`; `CREATE`/`ALTER`/`DROP TABLE`, indexes on one or several columns ([SQL.md](SQL.md)) |
| Constraints | primary keys, `UNIQUE` (also over several columns), `NOT NULL`, `CHECK`, `DEFAULT`, enforced foreign keys with `CASCADE`/`SET NULL`, `VARCHAR(n)` lengths |
| Transactions | `BEGIN ... COMMIT` with savepoints; `BEGIN CONCURRENT` (optimistic, serializable by default); snapshot reads that never block commits |
| Full-text search | `CREATE FULLTEXT INDEX`, `MATCH ... AGAINST` in natural and boolean modes, phrases, prefixes, BM25 ranking |
| Change notifications | `LISTEN`/`NOTIFY`, `table_changes:<table>` channels, `db.listen()`/`db.watch()` |
| Server | TCP, Unix sockets, TLS 1.2+ (OpenSSL on Linux, Schannel on Windows), client certificates, users with roles and table limits, timeouts and connection limits |
| Operations | online backup, continuous archive and point-in-time restore, online compaction, Prometheus `/metrics`, `/health/ready`, slow-query log, `kestro-server admin` |
| High availability | asynchronous or synchronous replication; a three-member cluster that elects a new leader in about a second without losing an acknowledged write |
| Packaging | npm package with prebuilt binaries, Python wheels, a Composer package, a Docker image (non-root, health check), a hardened systemd unit |

What it does not do yet is listed honestly in the [guide](docs/GUIDE.md#what-kestrodb-does-not-do)
and in [SQL.md](SQL.md#not-supported-yet): for example `NATURAL` joins, deferred constraints,
several writing processes on one file without the server, and macOS release binaries.

## Performance

One machine (Intel i5-14400, 16 threads), 2026-11-20, KestroDB 0.9.0 in-process from Node 24
against the fastest of SQLite (node:sqlite and better-sqlite3), LMDB and LevelDB.
**Every engine acknowledges a write only after asking the OS to make it durable.** 20,000
keys, ~230-byte JSON documents, median of 5 runs, the engines' order rotating between runs.
Higher is better.

| Benchmark | Windows 10: KestroDB | fastest other | Linux: KestroDB | fastest other |
|---|---:|---:|---:|---:|
| Durable writes, one at a time | **24,700/s** | LMDB 7,300 (3.4×) | **2,800/s** | SQLite 1,800 (1.6×) |
| Durable writes, 256 in flight | **329,000/s** | LMDB 135,300 (2.4×) | **225,800/s** | LMDB 168,800 (1.3×) |
| Bulk load, 1,000 writes per transaction | **654,900/s** | SQLite 376,500 (1.7×) | **513,600/s** | SQLite 343,200 (1.5×) |
| Point reads, one at a time | **881,900/s** | LMDB 768,400 (1.15×) | **1,130,000/s** | LMDB 1,000,000 (1.13×) |
| Point reads, 256 in flight | **912,400/s** | LMDB 847,600 (1.08×) | **1,040,000/s** | LMDB 996,000 (1.05×) |
| Multi-key reads, 100 per call | **947,000/s** | LMDB 803,000 (1.18×) | **1,050,000/s** | LMDB 937,700 (1.12×) |
| Mixed: 95% reads, 5% durable writes | **546,400/s** | LMDB 178,700 (3.1×) | **654,600/s** | LMDB 368,000 (1.8×) |
| Open a 20,000-key database (×10) | **25.0M keys/s** | SQLite 15.4M (1.6×) | **87.1M keys/s** | LMDB 58.1M (1.5×) |

SQL against SQLite (prepared statements, WAL, `synchronous=FULL`), 20,000 rows, statements/s:

| Statement | Windows: KestroDB / SQLite | Linux: KestroDB / SQLite |
|---|---:|---:|
| `SELECT * WHERE id = ?` | 637,500 / 321,400 (1.98×) | 783,500 / 500,500 (1.57×) |
| `SELECT * WHERE email = ?` (unique index) | 453,100 / 280,300 (1.62×) | 599,900 / 421,900 (1.42×) |
| `WHERE city = ? ORDER BY age DESC LIMIT 10` (index) | 5,500 / 1,900 (2.86×) | 6,400 / 1,800 (3.48×) |
| `GROUP BY` with `COUNT`/`AVG` (whole table) | 1,400 / 385 (3.58×) | 1,700 / 382 (4.56×) |
| `COUNT(*) WHERE name LIKE ?` (whole table) | 2,600 / 1,600 (1.58×) | 2,800 / 1,700 (1.69×) |
| `INSERT` 500 rows per statement | 301,200 / 266,800 (1.13×) | 279,100 / 238,000 (1.17×) |
| `INSERT` one row, durable | 21,100 / 3,800 (5.49×) | 2,900 / 2,700 (1.10×) |
| `UPDATE ... WHERE id = ?`, durable | 21,500 / 4,200 (5.12×) | 2,800 / 2,700 (1.07×) |

Read these with their limits: one machine and small data sets (data larger than RAM and
millions of rows are not measured here); the Linux runs are Ubuntu 24.04 under WSL2, whose
virtual disk makes every flush slow, so durable rows there are close; single runs move by
10–15% on this machine. Bulk SQL `INSERT` on Windows ranged from 1.02× to 1.6× across runs.
Run them yourself: `npm install --prefix bench/competitors`, then
`node bench/compare.mjs --runs 5` and `node bench/sql-compare.mjs --runs 5`. The
[guide](docs/GUIDE.md#performance) has more results (server against MariaDB and PostgreSQL,
many PHP and Python processes).

## Validation

Every push and pull request runs the full suites on Linux x64, Linux arm64 and Windows,
AddressSanitizer + UndefinedBehaviorSanitizer, ThreadSanitizer and libFuzzer (SQL parser
and executor, wire protocol, file open). The suites include:

- a simulated power-loss sweep: at every point of a workload's file operations it rebuilds
  the disk states a power cut could leave (lost and torn writes) and checks each one opens
  with every acknowledged transaction intact;
- writers killed mid-commit, servers killed while clients commit, and online compaction and
  backup under load;
- random SQL compared with SQLite (3,000 queries on every push, 100,000 weekly);
- the seven clients against a live server, with TLS and a three-member cluster whose leader
  is killed;
- multi-hour soak runs (67 million operations without an error).

Results, platform by platform, are in [VALIDATION.md](VALIDATION.md). No physical power-cut
testing on real hardware has been done yet.

## Documentation

| Document | Contents |
|---|---|
| [docs/GUIDE.md](docs/GUIDE.md) | The full reference: Node API, server, users and TLS, monitoring, backups, replication, failover, durability |
| [SQL.md](SQL.md) | Every SQL statement, function and type, transactions, full-text search, notifications, limits |
| [CLIENTS.md](CLIENTS.md) | Node.js, Python, PHP, Java, .NET, Go and Rust clients |
| [FORMAT.md](FORMAT.md) | The on-disk format, byte by byte |
| [PORTABILITY.md](PORTABILITY.md), [BULK.md](BULK.md) | Platform details; streaming large imports |
| [VALIDATION.md](VALIDATION.md), [CHANGELOG.md](CHANGELOG.md) | What was tested, release by release; what changed |

## Building from source

CMake 3.20+, a C++17 compiler (tested: MSVC 2022, GCC 12 and 13, Clang 18) and Node.js 22+;
on Linux also the OpenSSL 3 headers (`libssl-dev`). BLAKE3 and the Node-API headers are bundled.

```text
git clone https://github.com/HimasRafeek/kestrodb && cd kestrodb
npm run build        # the Node addon, server, worker, C library and native tests, into bin/
npm test             # the C++ suites and every Node suite
node example.mjs
```

## Contributing and security

Issues and pull requests are welcome: see [CONTRIBUTING.md](CONTRIBUTING.md). Report
security problems privately as described in [SECURITY.md](SECURITY.md).

## License

[Apache License 2.0](LICENSE). Third-party components and their licenses are listed in
[THIRD-PARTY.md](THIRD-PARTY.md) and [NOTICE](NOTICE).

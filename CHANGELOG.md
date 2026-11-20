# 0.9.0 — 2026-11-19

Enterprise gaps (2026-11-20):
- `BEGIN CONCURRENT` is serializable: reads are recorded (rows, index ranges, whole tables)
  and checked at COMMIT, so write skew and phantoms fail with CONFLICT. `ISOLATION LEVEL
  SNAPSHOT` keeps the old behaviour.
- Mutual TLS on kestro-server: `--tls-client-ca`, `--tls-client-cert required|optional`,
  users bound to certificates (`cert=cn:`/`cert=sha256:`), followers and cluster members by
  certificate (`--replica-cert`/`--replica-key`); Node, Python and PHP client options.
- Full-text search: `CREATE FULLTEXT INDEX`, `MATCH ... AGAINST` (natural and boolean
  modes, phrases, prefixes, BM25); a selective word on 100,000 rows in under 0.1 ms.
- Change notifications: `LISTEN`/`NOTIFY`/`pg_notify`, `table_changes:<table>` channels,
  `db.listen`/`db.watch` in-process, notification frames on the server (`--notify-queue`).
- First builds of the ordered keys, secondary indexes and the column cache run without the
  writer lock: they read rows unchanged since a snapshot point, then apply the commits made
  meanwhile (from the MVCC undo records) under the lock and publish. During a first aggregate
  over 200,000 rows the slowest write took ~8 ms (was ~400 ms); rows are parsed in parallel,
  so the first query is faster too (~218 ms, was ~407 ms). FULLTEXT first builds keep the lock.
- Monitoring: `--metrics HOST:PORT` (Prometheus /metrics, /health/live, /health/ready),
  `--slow-query-ms` JSON-lines slow-query log (no parameter values by default), and
  `kestro-server admin` (status, stats, verify, backup, compact, metrics, connections, kill, slow).
- Packages (scripts/package.mjs, built locally, not published): Windows x64 zip, Linux x64 and
  arm64 tarballs (glibc 2.34+, libstdc++ and libcrypto static and hidden; the server uses the
  system OpenSSL 3), an npm package with prebuilds for the three platforms, Python wheels
  bundling the C library, a Composer zip, a Dockerfile (non-root, /data, health check) with
  single-node and cluster compose examples, a hardened systemd unit and installer.
- kestro-server reads --token-file, --users and the replica/cluster token files read-only (Docker
  secrets, root-owned files, systemd ProtectSystem=strict); still regular files only, no symlinks.
- ThreadSanitizer runs locally (WSL2) without its 64-lock deadlock detector.
- A request sent in the same TCP read as the login no longer waits for more bytes.

Benchmarks (2026-11-20; bench/compare.mjs rotates the engine order):
- The engines ran in a fixed order in every phase, so KestroDB always ran right after
  LevelDB's background compaction; on Linux that showed as LMDB 1.8× ahead on the mixed row.
  The order now rotates with every phase and run. Results with 5 runs: Linux first in all 8
  key/value rows (mixed 574K against LMDB 361K/s, 1.59×) and all 8 SQL rows; Windows first in
  all 8 key/value rows (point reads one at a time 924K against LMDB 768K/s, 1.20×; an earlier
  run had LMDB 3% ahead on that row) and all 8 SQL rows.
- Point reads: the read checksum (SipHash) loaded its input a byte at a time, which MSVC does
  not turn into word loads, and computed both halves of its 128-bit output while comparing
  one. A point read in the addon now costs ~258 ns instead of ~287 ns. The digest bytes are
  unchanged, so index snapshots stay valid.
- Linux rerun after the read-checksum change (Ubuntu 24.04 under WSL2, same i5-14400, Node
  24.11.1, 5 runs): still first in all 8 key/value and all 8 SQL rows. Point reads one at a
  time 1.05M against LMDB 941K/s (1.11×), multi-key reads 1.06M against 893K/s (1.18×), mixed
  523K against 405K/s (1.29×), open 88.4M against 60.8M keys/s (1.45×); SQL `SELECT` by id
  794K against SQLite 475K/s (1.67×), durable `INSERT`/`UPDATE` 1.13× and 1.06×.
- Go, Rust and .NET clients (with Java: seven languages tested against a server).

Clients and server edges (2026-11-20):
- Java client (clients/java, Java 17, no dependencies) and a minimal JDBC driver.
- A stop no longer waits for clients that keep their connection open (Windows: 120 s, now
  2 s); BUSY at a connection limit reaches clients that already sent their login (a reset
  could destroy it). Both found by the Java client's tests.
- Online compaction replays the new file before taking the writer lock (worst write during a
  compaction 134-177 ms, now 28-74 ms). WITH RECURSIVE stops at --statement-timeout (found by
  fuzzing). bench/soak.mjs no longer stalls its own requests.

SQL and TLS (2026-11-20; tests/cte, multiindex, altercolumn, windowgroup, tls-windows):
- `WITH` and `WITH RECURSIVE` (row limit 1,000,000), derived tables (`FROM (SELECT ...)`),
  `SELECT` without `FROM`; table permissions cover tables read inside them.
- Composite indexes (up to 8 columns, `UNIQUE` too): equality/IN on leading columns plus a
  range, `ORDER BY a, b ... LIMIT` streaming; multi-column foreign keys and `UNIQUE`
  constraints get one composite index.
- `ALTER COLUMN ... SET/DROP DEFAULT`, `SET/DROP NOT NULL`, `TYPE` (rows checked and converted
  first) and MySQL `MODIFY`; `VARCHAR(n)`/`CHAR(n)` lengths enforced; `RENAME COLUMN` keeps the
  column's place and an index's name.
- Window functions over `GROUP BY`/`HAVING` results; `ROWS`/`RANGE`/`GROUPS` frames with
  offsets (sliding aggregates); `NTH_VALUE`; named windows; `FILTER (WHERE ...)`. Fixed: an
  explicit `RANGE ... CURRENT ROW` frame acted as `ROWS`; qualified columns inside `OVER`
  read NULL; window sort keys were quadratic with many partitions.
- Native TLS on Windows (Schannel): `--tls-cert/--tls-key` (PEM: PKCS#8, RSA, EC) or
  `--tls-pfx`; `tls://` for followers and cluster members with `--replica-ca`.

Speed (2026-11-20; bench/sql-compare.mjs, bench/compare.mjs, interleaved A/B with the 0.9.0 build before MVCC):
- `SELECT ... WHERE id = ?` and `WHERE col = ?` on a UNIQUE column read their one row directly
  (no plan, no scan, no writer lock): +60-75%, 593K and 428K statements/s against SQLite's
  308K and 268K (tests/pointselect.test.mjs checks them against the general path).
- MVCC allocates nothing and reads no clock for statements that do not yield; every
  key/value benchmark is within noise of the build before it or ahead.
- First in all 16 benchmarks of the day (8 key/value against SQLite, better-sqlite3, LMDB
  and LevelDB; 8 SQL against SQLite).

Memory on long-running servers (found by the 3-hour soak):
- Each online compaction kept the replaced file open and mapped until the server closed, as
  <file>.compacted-old-N: after 33 compactions in the soak, about 4 GB of old files on disk
  and a 17 GB working set (private memory stayed at 89 MB). The switch now unmaps the old
  file, closes it and deletes it: point reads copy mapped values under the shard lock they
  already hold, so with every lock held no reader can be using the old views.

MVCC (tests/mvcc.test.mjs; portable tests under the thread sanitizer in CI):
- Snapshot reads: a long SQL read lets waiting commits through and keeps reading its
  snapshot (rows changed since are skipped where found and read from the references their
  changes replaced, merged in scan order). In the test a 260 ms query let 380 transactions
  commit, the slowest in 2 ms, and still saw exact totals. Short statements pay nothing. Undo
  records (about 100 bytes per changed row) exist only while a snapshot runs and are freed
  the moment the last one ends; no limit fails a query (`stats().snapshotMemory`).
- `BEGIN CONCURRENT`: optimistic transactions without the write gate until COMMIT;
  snapshot reads; first committer wins (`CONFLICT`, retry); constraints checked again at
  COMMIT; automatic ids reserved per transaction.
- Clients: a COMMIT that fails no longer leaves the client thinking a transaction is open.

Automatic failover (tests/cluster.test.mjs):
- `--cluster A,B,C --cluster-self A --cluster-token-file F [--election-timeout 1500ms]`: the
  members elect a leader (terms and votes flushed to `<db>.cluster`, pre-vote, members that
  hear from their leader refuse to vote); writes are answered once a majority has them; a
  leader without a majority for one election timeout steps down; a new leader is elected in
  about two timeouts (0.7-0.9 s with 400 ms in the tests). A former leader holding writes no
  majority confirmed cuts its log back and follows (in-process restart); a member without
  its file copies the leader's. `stats().cluster`.
- Clients (Node, PHP, Python) take a list of members and send writes to the leader,
  following READONLY redirects through elections (`failoverTimeout`, 15 s).
- A follower's replication request now counts as its confirmation once the leader has
  checked that its log continues the leader's, before waiting for new commits (it was
  counted after that wait, adding up to a second to synchronous writes).
- Connecting to a peer gives up after a timeout (a refused connection took two seconds on
  Windows).

Synchronous replication (tests/syncrep.test.mjs):
- `--sync-replicas N`: writes are answered once N followers have them durably (RPO 0);
  `--sync-timeout` (5 s) before the leader goes on alone (degraded, in `stats().sync`, until a
  follower catches up); `--sync-strict` to wait instead.

Power loss on Linux and macOS (found by the power-loss test in CI):
- There one flush covers a commit's group and its slot, so a power loss can keep the slot and
  lose or tear the group. The file then failed to open (`CORRUPT group magic`). Recovery now
  takes such a newer slot only when the groups after the older slot lead to it; otherwise it
  starts from the older slot (the commit was never acknowledged). A clean close now leaves
  both slots at the committed end, so damage to the last commit of a closed file is still
  reported, not set aside. The index snapshot is matched by txid, end and chain (not the slot
  generation), so that rewrite keeps it.

Security (2026-11-19, from a review of the server; tests/security.test.mjs):
- Table limits (`tables=`) now cover every part of a statement: UNION/INTERSECT/EXCEPT parts
  and subqueries in ORDER BY, GROUP BY, LIMIT, OFFSET and window clauses (before, a limited
  user could read other tables through them), and the tables a write reaches through foreign
  keys. SHOW TABLES lists only the user's tables; DROP INDEX must name its table; tables=
  cannot limit admin or replica users; non-admins cannot write SQL tables' rows with
  key/value writes (which skip constraints); an empty collection name is checked too.
- Read-only users and followers may not BEGIN (a transaction holds the write gate, which
  stalled every writer and a follower's replication); --transaction-timeout (60 s) rolls
  back a transaction whose connection is idle.
- --statement-timeout (30 s): long scans and joins stop with LIMIT (a three-way join of 400
  rows ran for minutes and held up every commit). Results and replies over 64 MB fail with
  LIMIT (they could exceed the 32-bit frame length).
- Before login: one frame of at most 64 KB at a time, a deadline (--auth-timeout, 10 s), and
  a receive buffer that grows with the bytes that arrive (a 5-byte header made the server
  allocate 64 MB per connection). --max-connections-per-ip (256, loopback exempt); a
  connection thread that cannot start no longer stops the server; ten failed logins from an
  address block it for a minute.
- Followers refuse key/value writes from every client (one without users accepted them,
  which broke its replication).
- Backups and compactTo from a client go only into --backup-dir, by plain file name (they
  could create files anywhere the server may write). The server refuses to listen on a
  non-loopback address without authentication unless --no-auth is given.
- Savepoints count their copies against a transaction's memory (1 GB at most).
- Seeding a follower copies the leader's log as byte ranges, so a compacted leader's large
  checkpoint group no longer stops it; a follower accepts replies with a whole maximum-size
  group. TLS: no renegotiation.

Hardening (2026-11-19):
- Fuzzing: fuzz/ has libFuzzer targets (with AddressSanitizer) for the SQL tokenizer and
  parser, JSON and the query language (fuzz_parse), SQL statements on a live database
  (fuzz_sql_exec), the binary request decoders (fuzz_protocol) and opening damaged files
  (fuzz_open); fuzz/seed.mjs seeds the corpora; fuzz/build.bat builds them with MSVC.
- Fix: deeply nested SQL exhausted the stack and crashed the process (about 1,000 nested
  parentheses or 400 nested subqueries; one such query could stop kestro-server). The
  parser now limits nesting (64 levels of parentheses, function calls, CASE, NOT, unary
  minus; 16 of subqueries) and the height of every expression (500 levels of operators,
  long OR/AND chains included), failing with INPUT.
- Fix: long operator chains copied their source text once per operator (memory quadratic
  in the chain's length); the text is now kept once per chain.
- SUM and AVG use compensated (Neumaier) summation, as SQLite does: results no longer
  depend on accumulated rounding error.
- kestro-server --create opens an existing file instead of failing (a service can keep the
  flag across restarts). Opening with create on an existing file now says so ("already
  exists") instead of reporting a Windows or POSIX error number.
- tests/sqlfuzz.test.mjs: random queries (filters, arithmetic, CASE, aggregates with GROUP
  BY/HAVING, all join kinds, correlated subqueries, DISTINCT, ORDER BY/LIMIT) compared with
  SQLite; 3,000 per run (KESTRO_SQLFUZZ_SEED / _COUNT for more).
- tests/crash.test.mjs: a writer killed at random moments, embedded and kestro-server with
  four connections: acknowledged transactions present, none torn, the file verifies.
- Fix: an indexed condition decided a row only after the row was read, and of several
  range conditions on one index only the first narrowed the scan: COUNT(*) WHERE age
  BETWEEN 20 AND 40 over 50,000 rows read every row from 20 up (20 ms, now 1.6 ms); WHERE
  city = ? counts now come from the index alone (2.5 ms -> 0.7 ms). Under the soak test
  (8 connections, 50,000 rows) the server went from about 1,000 to 4,800 statements/s,
  p99 latency from 45-120 ms to 8 ms: long reads had held commits back.
- Fix: every open listed the database's directory (looking for files an interrupted
  compaction might leave), slow in busy directories; a marker file now says when to look.
- bench/soak.mjs: kestro-server under steady mixed load (inserts, deletes, updates, point
  and range reads, aggregates, joins, transactions, online compactions) for N minutes,
  reporting throughput, p50/p99 latency and private memory per interval and per statement
  kind; fails on memory growth, errors or a file that does not verify. 30 minutes at 50,000
  rows: memory flat at 43-45 MB, no errors, file verified.
- AddressSanitizer: fuzz/asan-tests.bat (the portable suite) and fuzz/asan-addon.bat (the
  Node addon and kestro-server, for every Node suite); all pass (45 + 65 tests).
- CI (.github/workflows/platforms.yml): ASan+UBSan and TSan jobs for the C++ suites on
  Linux, libFuzzer runs of every fuzz target, and a scheduled job with 100,000 random queries
  against SQLite and 50 crash rounds.
- Simulated power loss (src/tests_powerloss.cpp, KESTRO_IO_RECORDER): every open, write,
  completion, flush and truncation is recorded; every disk state a power cut could leave
  (unflushed writes lost, written, or torn at 512-byte sectors) is rebuilt and must open,
  verify and keep every acknowledged transaction whole. Two fixes:
  - Commit slots are written without waiting (the log group is the commit point), so a power
    cut could tear both: the database then refused to open. A slot's fields now carry their
    own digest in the same sector (bytes 80..111), so a torn slot still reads as whole.
  - verify() rejected two slots with the same end, which the open-time rewrite after a
    recovery produces; it now applies open's rule.
- ThreadSanitizer fixes (found by the new CI job): rebuilding a damaged lazy index took
  the shard locks before readers_ (the reverse of commits and compactions); the paged
  index header read the database uuid from meta_ without the writer lock while a commit
  assigned it; readers read values through the file handle that an online compaction's
  switch replaces (now an atomic published at open and at the switch).
- CI: first full run green on Linux, Windows and macOS (first macOS build), ASan+UBSan,
  TSan and libFuzzer.
- KESTRO_COMMIT_SPIN_US (compile-time, default 200): how long a committing thread spins on
  its write before sleeping (Windows). 0, 50 and 200 measured within noise of each other
  on the mixed workload.

Renamed to KestroDB (2026-11-19):
- HimasDB is now KestroDB everywhere, with no aliases: kestro-server, libkestrodb /
  kestrodb.dll and kestrodb.h (C functions kestro_*), the kestrodb npm package
  (KestroDBError), the PHP namespace KestroDB (clients/php/KestroDB.php), the Python module
  kestrodb, KESTRO_* / KESTRODB_TOKEN environment variables, server messages (KESTRODB
  READY ...), and .kdb as the file extension in docs and examples (any extension opens).
- File markers: new files are written with KDBNAT02 / KDBG0002; files written before the
  rename (HDBNAT02 / HDBG0002) open, take new commits and may hold both spellings. The
  index snapshot (KDBIDX02) and the .idx cache (KDBXIDX1) of older files are rebuilt once.

SQL completeness (2026-11-18):
- Constraints, enforced at the end of each INSERT/UPDATE/DELETE against the rows as the
  statement leaves them: CHECK (column or table, named or not), FOREIGN KEY ... REFERENCES
  (id, a UNIQUE column or UNIQUE columns; ON DELETE / ON UPDATE NO ACTION, RESTRICT,
  CASCADE, SET NULL, SET DEFAULT, chained, self-references), UNIQUE over several columns.
  Kept in the catalog record ("rules"); the first column of each foreign key and
  multi-column UNIQUE is indexed. ALTER TABLE ADD CONSTRAINT / DROP CONSTRAINT (and MySQL's
  DROP FOREIGN KEY / DROP CHECK / DROP INDEX) check existing rows; DROP TABLE of a referenced
  table and DROP COLUMN of a constrained column are refused; RENAME carries them along.
- ALTER TABLE ... RENAME TO (rows, indexes, constraints and the id sequence move).
- SAVEPOINT, ROLLBACK TO [SAVEPOINT], RELEASE [SAVEPOINT] (outside a transaction SAVEPOINT
  begins one, as in SQLite).
- Correlated subqueries (EXISTS, IN, scalar) in SELECT, UPDATE and DELETE: run per outer
  row with the outer values, once per distinct value, inside the outer statement's locks.
- Window functions: ROW_NUMBER, RANK, DENSE_RANK, PERCENT_RANK, CUME_DIST, NTILE, LAG,
  LEAD, FIRST_VALUE, LAST_VALUE, and COUNT/SUM/AVG/MIN/MAX OVER (PARTITION BY ... ORDER BY
  ... [ROWS|RANGE BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW|UNBOUNDED FOLLOWING]).
- CASE WHEN ... THEN ... ELSE ... END and CASE x WHEN ...; UNION / INTERSECT / EXCEPT
  [ALL]; RIGHT and FULL [OUTER] JOIN; JOIN ... USING (...).
- Change: in a single-table SELECT an aliased table is known only by its alias (as in
  SQL), so "FROM users u WHERE users.id = 1" no longer means u.id.
- Fix: a commit writing rows of two or more indexed tables (a transaction over several
  tables, now also foreign key actions) could skip or garble index updates of the first
  table: the per-table index plans were held by pointers into a vector that moved. Indexes
  are rebuilt from the rows on open, so a restart repaired them.
- tests/sqldiff.test.mjs: 39 queries compared with SQLite (node:sqlite);
  tests/constraints.test.mjs: 61 statements with the same outcome and tables as SQLite
  (foreign_keys on), constraint DDL and reopen, multi-table transaction index check.

Replication:
- Followers: kestro-server --replica-of tcp://|tls://LEADER --replica-token-file FILE seeds a
  missing file from the leader, then streams; read-only (READONLY) until promoted. The
  follower commits each received transaction through its normal commit path: byte-identical
  log, indexes, SQL tables and column cache follow; catalog changes reload. STALE (leader
  compacted or restored) and DIVERGED stop it; other errors reconnect with back-off.
- Leader: command R (long-poll for commits, whole groups, at most 32 MB per reply), role
  "replica" (may only replicate); command X promotes (admin); stats().replica, readOnly.
- Node RemoteDB.promote(), PHP promote(), Python promote().
- tests/replication.test.mjs: seed, stream inserts/updates/deletes/DDL/indexes, identical
  queries and bytes, refused writes, follower and leader restarts, STALE after a leader
  compaction, promotion.

Online compaction:
- compact() rewrites the file to hold only current values while reads and writes continue:
  a checkpoint of values unchanged since a starting point (shard by shard), the
  transactions committed meanwhile re-chained after it, then a short pause for the last
  ones; the new file is replayed in full before it replaces the old one (POSIX: atomic
  replace; Windows: two renames, recovered on open). Point reads that straddle the switch
  read again. Server command K (admin); Node, PHP, Python, C APIs. Tested with writers
  and readers running (mapped, paged index, pread) and under AddressSanitizer.
- SortedSet and set paging unaffected (same rows); the paged key index restarts in RAM.

Backups:
- Online backups: backup() reads the commit point under a short lock and copies the
  committed bytes while writes continue (it held the writer lock for the whole copy and
  verified by rebuilding the full index in RAM); copies are verified by streaming the
  chain and SHA-256 of every transaction, in bounded memory.
- Continuous archive (archive.kdb + timeline, incremental) and point-in-time restore by
  transaction or time; a replaced source starts a new archive and keeps the old one.
- kestro-server: --backup-dir/--backup-every/--backup-keep (scheduled, retention),
  --archive-dir/--archive-every, --restore SOURCE --to NEW [--txid N | --time ISO].
- Node db.archive()/Database.restore(), PHP Embedded::archive/backup/restore, Python
  Local.archive/backup and kestrodb.restore(), C kestro_backup/kestro_archive/kestro_restore.
- tests/backup.test.mjs (scheduled backups with retention while writing, archive,
  restore by transaction and by time, refusals, embedded archive/restore).

Clients:
- Named (:name, @name) and numbered ($1, ?1) SQL parameters in the engine, for every client.
- PHP: query/queryOne/queryValue/execute/lastInsertId, PDO-style prepared statements,
  beginTransaction/commit/rollBack/transaction(fn), exception classes per error code,
  reconnect after an idle connection was closed, persistent connections for PHP-FPM
  (open transactions rolled back on reuse), KestroDBEmbedded (in-process SQL through FFI),
  composer.json.
- Python: DB-API 2.0 (PEP 249) cursors, exceptions and module attributes, query helpers,
  transaction() context manager, dict parameters, reconnect.
- Node: query/queryOne/queryValue/execute/transaction/prepare on Database and RemoteDB,
  object parameters, RemoteDB errors with codes and reconnect (a transaction lost with
  its connection is reported, never silently continued outside it); export kestrodb/remote.
- CLIENTS.md; tests/clients.test.mjs (PHP and Python against a server, embedded, and a
  server restart) and tests/sqlapi.test.mjs.

Versus PostgreSQL 18.7 (bench/server-sql-compare.mjs now takes KESTRO_PG_URL): #1 on all 10.
- Single commits on write-through platforms install while their write is in flight, with
  readers_ and the touched shards locked (in shard order) until it is durable; a failed
  write marks the store failed before any lock is released. The commit record (SHA-256)
  is computed during the write; the committing thread spins up to 200 us on the write
  before sleeping. Durable single-row SQL INSERT over TCP: 9.4K -> 12.6K/s.
- The maintainer is woken only when it has nothing pending (not on every commit).
- GROUP BY fast path on the column cache: values read in place, aggregate kinds decided
  once, string group keys found without copying: 1.5x -> 2.6x SQLite, 1.6x PostgreSQL.

Reads:
- Values that are ASCII (nearly all JSON) reach JavaScript as one-byte strings, built
  with a copy instead of UTF-8 decoding (get, getMany). Point reads with 256 in flight:
  833K/s vs LMDB 689-692K/s in two runs (it had dipped below LMDB in a run on a loaded machine).

Durability and commits:
- One disk write per commit on Windows: a group that is complete on disk and continues
  the chain (next transaction ID, previous hash, SHA-256) is committed; the metadata slot
  is written through the cache and reaches the disk lazily (flushed on close). Opening
  walks past a stale slot and tolerates one damaged slot. Durable single writes 12.4K ->
  16.9K/s (key/value), SQL durable INSERT 8.8K -> 14.3K/s (4.0x SQLite).
- SQL group commit pipelines: the next group's statements run while the previous group
  is written (overlapped I/O), and newly arriving statements join while the write is in
  flight; the slot write overlaps the install. 16 connections of durable single-row
  INSERTs: 6.2K -> 33-42K/s; MariaDB 28.5-30.8K/s.
- SortedSet::insert_sorted found each key's page by walking from the first page (O(pages)
  per commit per index); now a binary search. Commits on large tables were slowed by it.
- Tests: crash-injection tests now expect a group that reached the file to be committed;
  a damaged slot is recovered.

SQL:
- Group commit for SQL writes: autocommit INSERT, UPDATE and DELETE statements from many
  threads or server connections share one commit and one flush. A leader runs the
  waiting statements in order inside one implicit transaction (each sees the ones before
  it; a failing statement changes nothing and gets its own error) and keeps leading while
  statements wait. 16 connections of single-row durable INSERTs: 6,200 -> about 26,000-
  30,000 per second. Test: 16 threads, no lost updates, one winner per unique value.
- bench/server-sql-compare.mjs: kestro-server against MySQL/MariaDB over TCP.

Memory:
- Ordered keys and secondary indexes are paged: past Options::orderedMemory (default
  16 MB) their least recently changed pages move to a memory-mapped scratch file
  (<db>.sets, slots reused in place, adjacent writes combined), and they are built with an
  external merge sort in bounded RAM. 1,000,000 rows with two indexes: 118 MB -> 10 MB,
  inserts 138K -> 172K rows/s, indexed UPDATE 2.4K -> 6.3K/s, lookups unchanged or faster.
  New: --ordered-memory MB (server), orderedMemory (Node), stats orderedMemory and
  orderedFileBytes.
- Paged key index: once the index outgrows Options::indexMemory (default 32 MB), it moves
  to a memory-mapped sidecar file (<db>.idx) with per-shard hash tables, 64 KB block
  checksums and background merges; RAM holds only recent changes. kestro-server at
  5,000,000 keys: 327 MB -> 53 MB. 2,000,000 keys in-process: index RAM 124 MB -> 16 MB,
  random reads unchanged (~1.7M/s), reopen 18 ms. New: --index-memory MB (server),
  indexMemory (Node), stats indexMemory/indexFileBytes, index mode "paged", opened from
  "index-file". The sidecar is a cache bound to the log and rebuilt from it when damaged.
- Replace the key index with a compact one: 8-byte slots, 48-byte packed entries in
  growing blocks (keys up to 24 bytes stored inline, longer keys in a chunked arena), a
  64-bit read checksum per value (was 128), and no per-value mapped pointer (addresses come
  from a 4096-entry table of 64 MB file windows). Measured in kestro-server: 181 -> 68 bytes
  per key at 5,000,000 keys (2.7x less); in-process 66 bytes per key; 100,000 keys about
  8 MB in total. Speed unchanged within run-to-run noise (bench/compare.mjs,
  bench/sql-compare.mjs). File format unchanged.

SQL, queries and indexes:
- Enforce declared columns: types (integer, number, text, boolean, or any for JSON and
  undeclared), NOT NULL and DEFAULT on INSERT, UPDATE and upserts; schemas persist in the
  catalog. Add ALTER TABLE ADD COLUMN (backfills a DEFAULT), DROP COLUMN and RENAME COLUMN
  (rows rewritten; indexes dropped or moved with the column).
- Add transactions: BEGIN/COMMIT/ROLLBACK per connection (Node Database, RemoteDB, server
  connections, C library handles). Statements see the transaction's own changes; unique
  values are checked per statement; one writing transaction at a time through a write
  gate that every writer passes and readers never take; a dropped connection rolls back.
- Add joins (INNER, LEFT, CROSS, comma; aliases; alias.*), with index or id lookups for
  equality joins and WHERE push-down to the first table, and uncorrelated subqueries
  (IN, EXISTS, scalar), also in UPDATE and DELETE.
- Add a scan column cache for aggregates without a usable index, and parallel index
  maintenance (entries, unique checks, set updates) for large statements; SQL now leads
  SQLite in all eight bench/sql-compare.mjs rows on the test machine.
- Add SQL (SQL.md): CREATE/DROP TABLE, INSERT (many rows, automatic ids, upserts in
  PostgreSQL, MySQL and SQLite syntax), SELECT (WHERE with AND/OR/NOT, IN, BETWEEN,
  LIKE/ILIKE, IS NULL, arithmetic, functions; ORDER BY, LIMIT/OFFSET, DISTINCT, GROUP BY,
  HAVING, COUNT/SUM/AVG/MIN/MAX), UPDATE, DELETE, CREATE [UNIQUE] INDEX, DROP INDEX, with
  ? parameters. Every statement is atomic. Available as Database.sql, RemoteDB.sql, the
  server's E command, PHP and Python sql(), and kestro_sql in the C library.
- Add an ordered key set and secondary indexes (unique or not) on JSON fields, built on
  first use and maintained by every commit; index definitions persist in a reserved key.
  Unique checks see earlier transactions of the same group commit and allow swaps inside
  one transaction.
- Add a query engine (JSON specs; used by SQL): index selection, streaming in index or
  key order with early stop, consistent snapshots under concurrent commits, cursors.
- Add atomic increments (Op.incr), resolved inside the group commit and logged as plain
  writes, so the file format is unchanged.
- Native errors thrown to JavaScript now carry the error code in error.code.

# 0.8.0 — 2026-11-15

Server mode and other languages:
- Add users and roles to kestro-server (--users FILE: name, role admin/readwrite/readonly,
  token or sha256 hash, optional table list). Every request is checked: key/value reads
  and writes by collection, SQL by statement kind and every table it touches (joins and
  subqueries included); reserved keys and backup/verify/compaction need admin. Tokens are
  compared as SHA-256 in constant time across all users; failed logins are delayed;
  --hash-token prints a hash from standard input. --token-file still works (admin).
- Add TLS to kestro-server (--tls-cert/--tls-key, OpenSSL, TLS 1.2+; 10-second handshake
  limit) and tls:// addresses with certificate verification to the Node, PHP and Python
  clients. stats() now reports tls, user and role.
- Add `kestro-server`: one database shared by many clients over TCP or Unix sockets,
  token authentication, connection limit, pipelined requests with in-order replies, and
  one group commit for writes arriving together from all connections.
- Add clients for PHP (`clients/php/KestroDB.php`), Python (`clients/python/kestrodb.py`)
  and Node (`remote.mjs`, `RemoteDB`), sharing the server's keys.
- Add the C library `kestrodb` (`src/kestrodb.h`, `bin/kestrodb.dll` / `libkestrodb.so`)
  for any language with a C FFI: embedded owner mode, and a reader mode
  (`src/reader.hpp`) that reads a file another process is writing. Before each read the
  reader checks the commit slots through a memory map and applies only new groups after
  checking sequence, chain and frame hashes; bytes past the committed end are never
  used. The library pins itself in memory and shares readers per path, so PHP-FPM
  workers keep them across requests.
- PHP `readFrom` (FFI) and Python `read_from` (ctypes) read in-process while writing
  through the server; Python `kestrodb.open()` is embedded mode.
- Add `bench/lang-compare.mjs` (N PHP or Python processes against KestroDB, SQLite and
  MySQL) and results in `results/php-windows.json` and `results/python-windows.json`.

API layout:
- `Database` (`index.mjs`, the package's main export) is the one API to use. The
  worker-process client moved to `worker/client.mjs` (`kestrodb/worker`) as an advanced
  option; it is also what the crash-injection tests drive.

Performance:
- Add group commit: concurrent transactions share one metadata update and one durable
  commit, each keeping its own frame, version and atomicity. Compare-and-swap checks
  see the writes accepted before them in the same group. No file format change.
- Rewrite the worker loop around raw pipe I/O: process every buffered request, merge
  consecutive writes into one group commit, and answer all of them with one write.
  The Node client sends requests issued in the same tick as one pipe write.
- Add an in-process Node-API addon (`index.mjs`, `bin/kestro.node`): synchronous
  reads and writes, plus `setAsync`/`deleteAsync`/`batchAsync` that group-commit on a
  background writer thread. Node-API headers are vendored; the Windows import library
  is generated from `cmake/node_api.def`.
- Keep a zero-filled reserve past the committed end, so commits overwrite allocated
  bytes instead of growing the file on every commit. The format already defines bytes
  past the committed boundary as a tail every reader removes on open.
- Windows: memory-mapped reads (now the default) and write-through commits, with one
  `FlushFileBuffers` whenever the reserve grows. Remove a redundant `ResetEvent` call
  from every positioned read.
- Cheaper encoding and validation in JavaScript (same rules), and a direct multi-key read
  path in the addon.
- Replace the in-memory index (`std::unordered_map`) with an open-addressing table:
  one hash per key picks shard and slot, 16-byte slots, entries from pooled blocks.
- Point-read checksums are now keyed: SipHash-2-4-128 for values up to 2 KiB, keyed
  BLAKE3 (128-bit) above, with a secret drawn per process. They exist only in RAM and
  are rebuilt on every open, so the file format is unchanged; 85 ns instead of 230 ns
  for a 226-byte value.
- Faster open: the committed log is mapped once and prefetched; pool workers verify
  each group's SHA-256 while the replay applies groups in order (windows of 65,536
  groups bound memory); read checksums are computed only for surviving values, per
  shard in parallel. The same checks run in the same order as before; a bounded-read
  fallback is kept and tested.
- `jsonValidation: 'basic'` open option: `JSON.stringify` semantics instead of the strict
  default. The benchmark uses it, matching what the other engines do.
- Index snapshot: on clean close, and in the background once commits are quiet for
  50 ms, the index is written into the file's uncommitted tail as one hash table per
  shard, bound to the committed state and BLAKE3-protected per section. Opens use it
  without replaying the log: lookups probe the mapped tables until the first write
  builds the in-memory index. Missing, stale or damaged snapshots fall back to replay;
  older versions discard the tail as before.
- After a snapshot open, a background thread performs the full log verification
  (group SHA-256, hash chain, every value's checksum); a failure makes every later
  operation fail with CORRUPT. `verifyOnOpen` runs it before open returns.
  `stats()` adds `openedFrom`, `verification` and `indexMode`.
- Snapshot sections are checksummed when first used, not at open; a damaged section
  makes the store rebuild its index from the log and retry, even under concurrent
  readers. Background verification starts 50 ms after a snapshot open, on one shared
  thread (`verifyDelayMs`), so short sessions do not pay for it.
- Linux commits use `fdatasync` instead of `fsync`.
- Leaner group commit: transactions are encoded straight into one buffer, index
  references share one array, and compare-and-swap bookkeeping is skipped when no
  transaction in the group needs it.
- Read-checksum keys and the index hash seed are per store and stored in the snapshot
  (they were per process), so another process can use it.
- In-process async writes queued in one tick reach the writer thread as one call and one
  group commit. `getMany` reads through single gets when no async write is outstanding,
  which is still one consistent snapshot.

Fixes:
- MSVC could not compile the BLAKE3 vector test (ambiguous `std::string(0, 0)`).
- The portable test hung forever on MinGW builds while removing its long-path tree.
- Exiting Node without closing a database that had used async writes aborted the
  process (a joinable writer thread was destroyed during environment teardown).

Tooling:
- The open benchmark times ten consecutive reopens instead of one (too noisy to rank).
- `bench/compare.mjs` benchmarks KestroDB against SQLite (node:sqlite, better-sqlite3),
  LMDB and LevelDB with matched durability, across eight workloads; `--record` appends
  to `results/bench-history.json`, and `index.html` charts the leaderboard and history.
- MSVC builds link the C runtime statically. CI also runs the benchmark smoke test.

Validated natively on Windows 10 x64 (24 native + 33 Node tests) and Ubuntu 24.04 x86_64
(24 native + 30 POSIX + 33 Node tests). macOS remains untested. Linux and macOS
0.8 runtime validation is pending the CI workflow.

# 0.7.0 — 2026-11-15

- Add Windows, Linux and macOS platform backends while keeping HDBNAT02 unchanged.
- Add native Win32 file ownership, 64-bit positioned I/O, file flushing, shared
  locks, random IDs, Unicode/long paths, no-overwrite snapshot publication and
  binary stdin/stdout handling. Windows defaults to positioned reads.
- Add CommonCrypto SHA-256 and F_FULLFSYNC on macOS; retain OpenSSL 3 on Linux.
- Preserve BLAKE3 x86-64 acceleration and add architecture-aware ARM64/portable
  build selection; include MinGW and MSVC build paths.
- Make the Node adapter choose the native worker filename and automatic read
  policy; reject invalid Unicode/NUL paths before spawning the worker.
- Add CMake, platform-neutral build/test scripts, a native CI matrix, portable
  integrity/concurrency tests, process fault tests and older-format fixtures.
- Separate release and fault-injection workers and test that release hooks are absent.
- Fix the worker's return value on clean EOF after extracting its main function.
- Include Linux and Windows x64 binaries, test logs and an identical-policy Linux
  0.6/0.7 regression harness.

0.7 was validated natively on Linux. Windows was cross-compiled and macOS source
prepared; neither had native runtime validation in that release.

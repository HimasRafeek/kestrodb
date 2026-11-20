# KestroDB roadmap

What is done and what is planned next, in priority order. Last updated 2026-11-20 (0.9.0).
See README.md "What KestroDB does not do" and SQL.md "Not supported yet" for the current
limits.

## 0. Use as little RAM as possible, on demand, at the same speed (next)
Goal: a 1 GB server can hold far more rows than RAM allows (limited by disk), with hot data
as fast as today. RAM becomes a cache, like PostgreSQL's buffer pool or LMDB's mapped pages.
Today every key's location is held in RAM (about 130 bytes per key, plus ordered keys,
secondary indexes and the column cache when used).
- [x] Measure first (2026-11-16, 1M and 5M keys; 10M/50M still to run): server 132 B/key
      at 1M, 181 B/key at 5M before the compact index. RSS per row at 1M / 10M / 50M rows (key/value and SQL tables with
      indexes), open time, and read/write/query speed, so every change below is judged
      against these numbers.
- [x] Step 1, compact in-memory index (done 2026-11-16: 181 -> 68 B/key in the server at
      5M keys, 66 B/key in-process, small databases start at ~12 KB per shard; speed within
      noise; 64-bit read checksums in memory). Original plan: store only a short hash fingerprint and the file
      offset per key (about 16-24 bytes instead of ~130) and confirm the key on disk on a
      fingerprint match. Target 5-8x more rows per GB, point reads within 10% of today.
- [ ] Remove memory spikes: write the idle index snapshot in pieces instead of one buffer
      (~50 B/key while it runs), and verify after open without building a second full
      in-memory index.
- [x] Step 2 (done 2026-11-16): on-disk index paged on demand
      (sidecar <db>.idx, Options::indexMemory default 32 MB). Server at 5M keys: 327 MB -> 53 MB
      (11 B/key). --index-memory flag and stats done; speed unchanged (bench/compare.mjs, 2M-key paged
      reads ~1.7M/s). Validated on Linux 2026-11-16 (2M keys: 50 MB vs 130 MB;
      5M keys: 54 MB). Still to do: 10M/50M-key runs.
      Original plan: keep the main index in a memory-mapped file
      that commits update (extending today's index snapshot, which lookups already probe in
      the file after a clean open), so the OS keeps hot pages in RAM and the rest on disk.
      Size limited by disk; crash safety unchanged (the log stays the source of truth).
- [x] Ordered keys and secondary indexes on disk too (done 2026-11-17: paged sorted sets with
      a scratch file and external-sort builds; 1M rows + 2 indexes 118 MB -> 10 MB, same or
      better speed. Not yet persisted: rebuilt from the key index on the first query after
      each open; persisting them is a follow-up. Linux run still to do.) Original plan: (paged B+tree or sorted runs), loaded
      on demand instead of built fully in RAM on first query.
- [ ] Column cache and other caches with a RAM budget and eviction (least recently used),
      instead of a fixed cell cap.
- [ ] One setting for the total memory budget (e.g. --memory 256MB), honored by all caches.
      The default must be small (the database often shares a machine with other programs):
      it never grows its own memory without bound, and everything above the budget is
      reloaded from disk on demand.
- [ ] Report memory in stats() (index done: indexMemory/indexFileBytes; ordered keys, secondary indexes, column cache,
      connections) and show it in the viewer, so it is always visible what the database holds.
- [ ] Re-run every benchmark (bench/compare.mjs, bench/sql-compare.mjs, lang-compare,
      stress) and the 1 GB server scenario; publish before/after RAM and speed.

## 1. Replication and failover
- [x] Followers on other machines stream the leader's commit log over the server protocol
      (authenticated, TLS), byte-identical, with chain checks (2026-11-17).
- [x] Read-only queries on followers; replication lag in `stats()` (2026-11-17).
- [x] Manual promotion of a follower to leader (2026-11-17).
- [x] Automatic failover: `--cluster` (elections with terms, pre-vote and leader lease;
      majority-confirmed writes; rejoining by cutting back an unconfirmed tail) and clients
      that follow the leader (2026-11-20).
- [x] Optional synchronous replication: `--sync-replicas N` (2026-11-19).
- [x] MVCC: long reads let commits through (snapshot reads); `BEGIN CONCURRENT` optimistic
      transactions (2026-11-20). [x] Serializable concurrent transactions (read-set checks, 2026-11-20). [ ] `WHERE col IN (30,000 values)` on an index takes ~9 s (quadratic range intersection).

## 2. Backups and recovery
- [x] Scheduled backups from kestro-server (interval, keep N, target directory), online (2026-11-17).
- [x] Continuous archive (incremental) and point-in-time restore by transaction or time (2026-11-17).
- [x] A restore command (kestro-server --restore) and the procedure in the README (2026-11-17).
- [x] Online compaction (reclaim space without stopping the server) (2026-11-17).

## 3. Hardening
- [x] Fuzz the SQL parser, the JSON scanner, the wire protocol and the file format: fuzz/
      (libFuzzer + AddressSanitizer, MSVC), 2026-11-19. Found: stack exhaustion from nested
      SQL (fixed: nesting and expression-height limits). In CI: 3 minutes per target per push. [ ] Longer runs (OSS-Fuzz or scheduled).
- [x] Random differential SQL against SQLite (tests/sqlfuzz.test.mjs; 1.2M queries over
      61 seeds, 2026-11-19). Found: SUM/AVG rounding (fixed: compensated summation).
- [x] Random process-kill consistency, embedded and kestro-server (tests/crash.test.mjs).
- [x] Simulated power loss (src/tests_powerloss.cpp): every file operation recorded, every
      disk state a power cut could leave rebuilt and checked; 19,788 states consistent after two
      fixes (2026-11-19). [ ] On real hardware.
- [x] Soak test (bench/soak.mjs): 30 minutes at 50,000 rows, memory flat (2026-11-19); 3 hours,
      67 million operations, no errors (2026-11-20; found the compaction file leak). [ ] Days-long runs.
- [x] AddressSanitizer runs of every suite on Windows (2026-11-19); ASan+UBSan and TSan on
      Linux run in CI: all pass (2026-11-19, after three ThreadSanitizer fixes).
- [x] Security review of the server (2026-11-19): 14 findings, all fixed (tests/security.test.mjs).
- [x] First builds of the ordered key set, secondary indexes and column cache run without the writer
      lock (2026-11-20): 200,000 rows, slowest write during the build ~8 ms instead of ~400 ms.
      [ ] FULLTEXT first builds still hold it.
- [x] Linux: the file lock outlives a SIGKILLed server by 100-200 ms: opening retries for up to 1 s (2026-11-20).
- [ ] An external audit; rate limiting per user (today: per address and per statement).
- [x] Linux ARM64 builds and passes every suite in CI, and the PHP and Python suites run in CI on every platform (2026-11-19). macOS 26 on ARM64 passed every suite on GitHub's runner through 2026-11-19; it is not run in CI now. [ ] macOS in CI again.

## 4. SQL completeness
- [x] RIGHT / FULL joins, JOIN ... USING, correlated subqueries, UNION / INTERSECT / EXCEPT,
      CASE (2026-11-18).
- [x] Window functions (2026-11-18); with GROUP BY, ROWS/RANGE/GROUPS frames, named windows, FILTER (2026-11-20).
- [x] Foreign keys (all actions), CHECK, multi-column UNIQUE, enforced (2026-11-18).
- [x] Multi-column indexes; VARCHAR length limits (2026-11-20). [ ] Deferred constraints.
- [x] ALTER TABLE ... RENAME TO, ADD/DROP CONSTRAINT; savepoints (2026-11-18); ALTER COLUMN, MODIFY (2026-11-20).
- [x] Common table expressions (WITH, WITH RECURSIVE), derived tables (2026-11-20). [ ] NATURAL joins; WITH before INSERT/UPDATE/DELETE.
- [ ] SQL through in-process readers (readFrom / read_from).

## 5. Scale and speed
- [x] Concurrent writing transactions: `BEGIN CONCURRENT` (snapshot isolation, 2026-11-20).
- [x] Direct path for SELECT by id / UNIQUE column (2026-11-20).
- [x] Keys beyond RAM: the paged key index moves to a mapped file past indexMemory (2026-11-16).
- [ ] Faster row-returning full scans (binary row format); aggregates already use the
      column cache.
- [x] Group-commit SQL writes from many server connections (2026-11-17: 6.2K -> ~26-30K
      durable single-row INSERTs/s with 16 connections; MariaDB 30.4K).
- [x] Beat MariaDB on many-connection durable single-row writes (2026-11-17: 33.1K vs
      28.5K/s, pipelined group commit and one disk write per commit). Linux validated
      2026-11-17 (44/44 portable, 30/30 POSIX incl. crash injection). Still: power-loss hardware.
- [ ] An event-driven server for tens of thousands of connections (today one thread each).
- [x] Native TLS on the Windows server (Schannel, 2026-11-20). [x] Client certificates (mutual TLS), certificate-bound users, followers by certificate (2026-11-20). [ ] Revocation (CRL/OCSP); client certificates in the Go, Rust, Java and .NET clients.

## 6. Operations and ecosystem
- [x] Metrics endpoint (Prometheus) with health checks, slow-query log, `kestro-server admin` (2026-11-20). [ ] HTTPS for the endpoint; per-follower lag on the leader.
- [x] Release packages (2026-11-20): release archives for Windows x64, Linux x64 and arm64 (glibc 2.34+), an npm package with prebuilt binaries, Python wheels, a Composer zip, a Docker image, a systemd unit and installer. [ ] Publish (npm, PyPI, Packagist, Docker Hub); per-platform npm packages; native Windows service; Kubernetes secret symlinks; macOS and musl builds.
- [x] Clients for Java (JDBC), .NET (ADO.NET), Go (database/sql) and Rust (2026-11-20). [ ] Laravel / Django / WordPress drivers.
- [x] Change notifications: LISTEN/NOTIFY, table_changes channels, db.listen/db.watch (2026-11-20). [ ] Across processes; LISTEN over TLS.
- [x] Full-text search: FULLTEXT indexes, MATCH ... AGAINST, BM25 (2026-11-20). [ ] Stemming, stop words, word segmentation; MATCH in joins.
- [ ] Run one real (low-risk) project on KestroDB to find what tests miss.

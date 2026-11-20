# KestroDB validation

## 0.9.0 — 2026-11-20, serializable BEGIN CONCURRENT, mutual TLS, full-text search, LISTEN/NOTIFY, first builds without the writer lock (commit 1117a09)

| Check | Result |
|---|---|
| Windows 10 (MSVC 2022) | C++ suites 2/2; Node 171 tests: 154 passed, 0 failed, 17 skipped (Go, Rust, .NET toolchains not installed on Windows) |
| Linux, WSL2 (GCC 13, clang 18) | C++ suites 3/3; Node 164 tests: 162 passed, 0 failed, 2 skipped (Windows-only); AddressSanitizer + UBSan and ThreadSanitizer: all 3 suites passed, no reports |
| Found while merging | A FULLTEXT index built by the new lock-free first build got plain index entries; a cold UPDATE/DELETE by MATCH failed; a MATCH in a serializable transaction missed phantoms. All fixed, each with a test that fails without the fix |
| Benchmarks | First in all 8 key/value and all 8 SQL rows on Windows and Linux; old and new builds alternated on Windows (3 rounds): bulk INSERT, durable INSERT/UPDATE and SELECT by id within run-to-run variation |
| Open | One run of "before login ... deadline" (security.test.mjs) closed a silent connection after 370 ms instead of 1 s under full-suite load on Linux; 6/6 alone and in the next full run. The test now prints what it received |

## 0.9.0 — 2026-11-20, Linux under WSL2 (Ubuntu 24.04, same i5-14400, 16 threads, 15 GB)

Run with `scripts/linux-check.sh`.

| Check | Result |
|---|---|
| Release build (GCC 13.3, Ninja) | 47 s |
| Portable, power-loss and POSIX C++ suites | Passed |
| Node suites | 127 of 129 passed, 2 skipped (Windows-only); clients in Node, PHP 8.3, Python 3.12, Java 17, Go 1.23, Rust 1.100 and .NET 8 against the server, each with reconnect, BUSY, TLS and a 3-member cluster with a killed leader |
| AddressSanitizer + UndefinedBehaviorSanitizer | 50 portable tests, power loss and 30 POSIX tests passed |
| ThreadSanitizer | Portable, power-loss and POSIX suites passed, no reports (run without address randomization, which clang 18's runtime cannot handle on this kernel, and without its lock-order detector, which stops at 64 locks held at once: commits hold all 64 index shard locks) |
| Found and fixed by the new clients | A stop after any BUSY hung (the rejecter's condition variable destroyed at exit); a restart right after a kill saw LOCKED for 100-300 ms (the kernel releases a mapped file's lock late: retried for up to a second); a stop waited 120 s for silent clients; BUSY could be lost |
| Benchmarks | First in all 8 key/value and all 8 SQL rows (README) |

## 0.9.0 — 2026-11-20, replication, failover, MVCC (Windows and CI)

| Check | Result |
|---|---|
| CI run #8 (commit f904922: synchronous replication, automatic failover, POSIX power-loss fix) | All 7 jobs passed: Ubuntu x64 and ARM64, Windows, macOS, AddressSanitizer + UBSan, ThreadSanitizer, fuzzing |
| tests/cluster.test.mjs (Windows) | 8 runs in a row green; a new leader 0.74-0.85 s after the old one is killed (400 ms election timeout); no acknowledged write lost; a former leader with an unconfirmed write cuts it and follows; PHP and Python clients follow the leader |
| MVCC (portable C++, Windows; CI under ThreadSanitizer) | Long reads during 13-27K concurrent commits always see exact totals (1,600-1,900 yields per run); BEGIN CONCURRENT transfers from 4 threads with conflicts retried keep totals exact |
| tests/mvcc.test.mjs | A 245 ms query lets 330+ transactions commit (p90 1 ms, slowest 2 ms) and still reads one state; snapshot memory back to 0 when it ends |
| Soak, 3 hours (build before these changes) | 67 million operations, 0 errors, file verified; 6-7K ops/s, p99 about 6 ms; found the compaction file leak (33 old files, 4 GB, 17 GB working set; private memory 86-106 MB), fixed in e59e84c |
| Thread sanitizer finding | A compaction closing the old file while a point read used it with pread (78e3dc1): copies now happen under the shard lock |
| Benchmarks (Windows) | First in all 8 key/value and all 8 SQL benchmarks (see README) |
| Soak, 1 hour (fixed build, a4b479f) | 22 million operations, 0 errors, file verified; server memory flat at 40-45 MB (the old build grew 86 to 106 MB); no old file copies left; snapshot memory 0 at the end (3.67 million commits let through) |
| Worst-case latency | The soaks' 1.5-2.4 s maxima were mostly the harness: it started PowerShell synchronously and sorted millions of samples on its own event loop each report (fixed; reports now show the client's own stall). Online compaction also replayed the whole new file under the writer lock (worst write during a compaction 134-177 ms, now 28-74 ms, c8d174b). 15-minute soak after both: 6.6-6.9K ops/s, p99 5.1-5.6 ms, worst 134-243 ms after warm-up, 551 ms in the first interval (first builds of the ordered set, indexes and column cache hold the writer lock: open item) |
| Fuzzing (CI #19) | fuzz_sql_exec timed out on a recursive WITH growing toward its 1,000,000-row cap: the recursion did not check the statement deadline. Fixed (8ac96cf): a runaway recursion now stops at --statement-timeout (302 ms with 300 ms) |
| SQL additions (WITH, composite indexes, ALTER COLUMN, windows over GROUP BY) and Windows TLS | New suites: cte, multiindex, altercolumn, windowgroup (results compared with SQLite), tls-windows (Node, PHP and Python clients, replication and a 3-member cluster over TLS); 112 Node tests and 50 portable tests pass on the merged code |

## 0.9.0 — 2026-11-19, CI (GitHub Actions)

Run #4 of .github/workflows/platforms.yml (commit ae5bee0), all jobs passed:

| Job | Result |
|---|---|
| native (ubuntu-latest) | Build, portable + POSIX suites, every Node suite, benchmark smoke run, portable-hash build: passed (7.2 min) |
| native (windows-latest) | The same: passed (18.3 min) |
| native (macos-latest) | The same: passed (3.1 min), macOS 26 on ARM64 (Apple M2 Pro runner). **First build and test on macOS and on ARM64.** |
| sanitizers (address) | Portable and POSIX suites under AddressSanitizer + UndefinedBehaviorSanitizer (leak detection on): passed |
| sanitizers (thread) | Portable and POSIX suites under ThreadSanitizer: passed, after three fixes found by runs #1-#3: a lock-order inversion (index rebuild), a race on the database uuid (paged index header), a race on the file handle (online compaction switch) |
| fuzz | Four libFuzzer targets with ASan + UBSan, 3 minutes each: 3.1 million inputs (parse 1.2M, SQL execution 600K, protocol 886K, file open 448K), no findings |

## 0.9.0 — 2026-11-19, Linux round 11 (Ubuntu 24.04, a shared 2-vCPU, 1.8 GB cloud VM)

Built kestro-server, libkestrodb.so and the Node addon (GCC 13.3, `-O1`, one compile job)
from the renamed tree; tests ran with Node 24.22.3.

| Check | Result |
|---|---|
| Node suites: tests/sqldiff (39 queries vs SQLite), tests/constraints (61 statements vs SQLite, constraint DDL, multi-table index check), sqlapi, clients (PHP 8.3 server and embedded via FFI, Python 3.12 DB-API and embedded), server, backup, replication | 29/29 passed |
| Portable C++ suite | **Not run on this machine**: with its 1.8 GB of memory shared, GCC needed its most memory-frugal settings, and compiling the suite had not finished after 3 hours; it was stopped. The suite (and the POSIX suite, under ASan/UBSan and TSan) runs in the CI workflow on GitHub's Linux runners instead. |

## 0.9.0 — 2026-11-19, Windows

Windows 10, Intel i5-14400, MSVC 2022 (Release), Node 24.22.1, PHP 8.2 (no FFI), Python 3.12.

| Check | Result |
|---|---|
| Portable suite (CTest) | Passed |
| Node suites (62 tests, none skipped), including tests/sqldiff.test.mjs (39 queries compared with SQLite) and tests/constraints.test.mjs (61 statements with the same outcome and tables as SQLite with foreign keys on, constraint DDL and reopen, multi-table transaction index check) | 62/62 passed |
| Clang 23 syntax check of the engine and tests (-Wall -Wextra) | No errors |
| Files written before the rename (a copy of fixtures/native-0.6.hdb, HDBNAT02/HDBG0002): open, new commits (KDBG0002 groups), reopen | Passed |
| Release package `kestrodb-0.9.0-windows-x64.zip` unzipped into an empty folder: checksums, Node embedded (constraints, windows, CASE, cascade, reopen), kestro-server, Node RemoteDB (wrong token refused), Python server client and embedded (ctypes), PHP server client | Passed |
| Binary dependencies (dumpbin): kestro-server.exe, kestrodb.dll, kestro.node, kestro-worker.exe | Windows system DLLs only (KERNEL32, bcrypt, WS2_32) |
| Benchmarks (median of 3): SQL vs SQLite 8/8, key/value vs SQLite, better-sqlite3, LMDB, LevelDB 8/8 (run 8), kestro-server vs PostgreSQL 18.7 10/10, vs MariaDB 11.4.4 10/10 | #1 in all 36 |

## 0.8 — 2026-11-15, Windows

Machine: Windows 10 Pro 19045 x64, Intel Core i5-14400 (16 threads), NVMe SSD,
Node 24.22.1. Build: MSVC 2022 toolset 14.44 through CMake 4.1, Release, static CRT.

| Check | Result |
|---|---|
| `npm run build` (worker, fault worker, native tests, kestro.node) | Passed |
| Native portable suite (CTest) | 24/24 passed, including a damaged snapshot section rebuilt from the log under 8 concurrent readers, index-snapshot reopen, damaged/stale/absent snapshot fallback, background and on-open detection of corrupted log data, lazy-index reads/verify/backup/compaction and the switch to the in-memory index under 8 reader threads, idle checkpoints after a simulated crash, SipHash reference vectors, a 200,000-step randomized comparison of the index against `std::unordered_map`, and mapped vs bounded-read replay agreement on valid and corrupted files |
| Node suite | 33/33 passed: 18 from 0.7, 5 group commit, 10 in-process addon (sync, async, mapped reads, locking, interop, JSON modes, index snapshot across worker and addon) |
| Process faults (after_data, before_sync, after_meta, torn_meta, sync errors) | Passed with write-through commits |
| `process.exit` with 1,000 queued async writes | Clean exit; reopen found all 1,000 and verify passed |
| Binary imports | kestro-worker.exe: bcrypt.dll, KERNEL32.dll. kestro.node: node.exe, bcrypt.dll, KERNEL32.dll |
| Benchmark against 5 other engines | `results/bench-history.json` runs 4–6 (run 6: #1 in all 8), charted in `index.html` |
| Native portable suite after server mode | 25/25 passed: adds the multi-process reader (reads right after each commit and delete, remaps as the file grows, a concurrent thread never sees a torn or older two-key state over 3,000 commits, a new reader replays, a damaged committed value fails its checksum) |
| Server and language suite (`tests/server.test.mjs`) | 11/11 passed: 8 connections × 200 writes get versions 1..1600, CAS across 16 connections has one winner, pipelined read-your-writes, token auth, connection limit, clean shutdown with snapshot, 4 PHP 8.2 processes and 4 Python 3.13 processes through the server, 4 PHP processes reading in-process via FFI and 4 Python processes via ctypes (each checks it sees a write the server acknowledged just before), Python embedded mode (ownership lock, CONFLICT, reopen). Whole Node suite: 44/44 |
| PHP and Python multi-process benchmark | `bench/lang-compare.mjs`: KestroDB first in every row against SQLite and MySQL 8.0.35; `results/php-windows.json`, `results/python-windows.json` |
| Linux, macOS, ARM64 runtime of 0.8 | **Not run** at the time (Linux: see the next section; CI ran from 0.9.0) |
| Physical power loss | Not performed |

The shipped 0.7 MinGW Windows binaries were also run on this machine before any
change: all 16 native and 18 Node tests passed, which is the first Windows runtime pass
of 0.7. Its native test executable then hung in MinGW's `std::filesystem::remove_all`
on the test's own path of more than 260 characters, while removing its temporary
directory. 0.8 removes that tree through the platform layer's extended-length calls.
The MSVC build path also failed to compile the BLAKE3 vector test (`std::string(0, 0)`
is ambiguous under MSVC); fixed.

Write-through durability was chosen from a measurement, not assumed:
`FlushFileBuffers` after each small commit cost 333–390 µs; write-through writes into
preallocated space cost 67 µs, about 5× less. Write-through on a growing file cost
148–166 µs, because the file size changes on every write. Hence the preallocated
reserve, and one `FlushFileBuffers` whenever the reserve grows.

## 0.8 — 2026-11-15, Linux

Ubuntu 24.04.4 LTS x86_64 on a shared 2-vCPU, 1.8 GB cloud VM (AMD EPYC-Milan), GCC 13.3,
CMake 3.28, OpenSSL 3.0.13, Node 24.22.3.

| Check | Result |
|---|---|
| Release build (addon, workers, both native suites) | Passed, no warnings |
| Native portable suite | 23/23 passed (SHA-256 through OpenSSL 3) |
| POSIX suite (0.7's Linux-only tests) | 30/30 passed after two tests were updated: they damaged the file's last bytes, or committed data, and expected open to fail; the last bytes are now the index snapshot, and a snapshot open detects damaged data in the background, so they truncate at the committed boundary or open with `verifyOnOpen` |
| Node suite | 33/33 passed |
| Benchmark against 5 engines (median of 3) | #1 in 4 of 8, #2 behind LMDB in 4 (4–18%). This round's results file was later overwritten by round 2: `results/linux-ubuntu-2404-bench.json` now holds round 2 (#1 in 3 of 8, LMDB ahead in 5 by 1–13%) |

### Linux round 10: backups, compaction, replication (same server, 2026-11-17)

Built `kestro-server`, `libkestrodb.so`, the Node addon and the portable suite with one
compile job at `-O1` (lowest available memory 367 MB).

| Check | Result |
|---|---|
| Portable suite, including online compaction with writers and readers running (mapped, paged index, pread) and the POSIX atomic replace | 45/45 passed |
| Scheduled online backups with retention, continuous archive, point-in-time restore by transaction and by time, embedded archive/restore | Passed |
| Online compaction through the server while another connection writes | Passed |
| Replication: seed, stream inserts/updates/deletes/DDL/indexes, byte-identical log, READONLY, follower and leader restarts, STALE after a leader compaction, promotion | Passed |
| PHP 8.3 and Python 3.12 clients (server, embedded, backups, compaction) and the Node SQL API | Passed |

10/10 Node tests, none skipped.

### Linux round 9: client libraries (same server, 2026-11-17)

Built `kestro-server`, `libkestrodb.so` and the Node addon with one compile job at `-O1`
(lowest available memory 476 MB). PHP 8.3.6 (FFI), Python 3.12.3, Node 20.20.

| Check | Result |
|---|---|
| PHP SQL API: named/numbered parameters, prepared statements, transactions, exception classes, persistent connections, against the server and embedded (FFI) | Passed |
| PHP reconnects after the server restarts on the same port | Passed |
| Python DB-API 2.0: cursors, executemany, exceptions, transactions, autocommit off, against the server and embedded (ctypes) | Passed |
| Node `Database` and `RemoteDB` SQL API; RemoteDB reconnect and a transaction lost with its connection | Passed |

5/5 tests, none skipped.

### Linux round 8: one-write commits, pipelined SQL group commit, paged sets (same server, 2026-11-17)

Built `kestro-server`, the portable suite and the POSIX suite with the same settings
(lowest available memory 410 MB).

| Check | Result |
|---|---|
| Native portable suite | 44/44 passed (paged sorted sets, SQL group commit under 16 threads, damaged-slot recovery) |
| POSIX suite (fork-based crash injection) | 30/30 passed after one expectation was updated: a damaged newest slot is now recovered from the other slot and the chained group instead of refusing to open; truncated or zeroed data still refuses |
| Crash injection after_data, after_meta, torn_meta, sync_error, before_sync, final_sync_error | A group that reached the file is recovered; the acknowledged one is always present |
| Durable SQL INSERT over TCP (2 vCPUs, ~0.35 ms flush) | 1 connection 2,654/s, 4 connections 5,133/s, 16 connections 9,043/s (8,951/s on a 20,000-row table) |

### Linux round 7: paged key index (same server, 2026-11-16)

Built only `kestro-server` and the portable suite (one job, `-O1`; lowest available memory
444 MB). GCC 13.3, OpenSSL 3, Node 20 as the load client.

| Check | Result |
|---|---|
| Native portable suite | 40/40 passed, including the 4 paged-index tests (merges, reopen, crash replay, damaged files, concurrent readers and SQL) |
| kestro-server memory, 2,000,000 keys | 50 MB with the paged index (default 32 MB budget) vs 130 MB with `--index-memory 0`; load 6.7 s either way |
| kestro-server memory, 5,000,000 keys | 54 MB (11 bytes per key), loaded in 17.5 s |
| 1,000,000 keys, 10 / 100 connections, 10% durable writes | paged 23.9K / 26.1K ops/s, in-RAM index 24.3K / 23.9K ops/s; no errors; private memory 41-53 MB vs 67-69 MB |

### Linux round 6: TLS and users (same server, same day)

Built only `kestro-server` with one job at `-O1` (two parallel `-O3` compiles need about
1.1 GB, too much for this machine).
With a self-signed certificate and a users file, `kestro-server` listened TLS-only: a PHP
8.3 client (admin, CA file) created a table and inserted rows over `tls://` and refused a
server certificate from another CA; a Python client (readonly, limited to that table) read
over TLS and was refused an INSERT with AUTH; a Node 20 client read over TLS, and a plain
TCP connection to the TLS port failed.

### Linux round 5: transactions, joins, subqueries, schemas (same server, same day)

Same toolchain. CTest 2/2 and the portable suite 35/35 passed, including SQL transactions
(read-your-writes, isolation, rollback, one writer at a time), joins and subqueries with and
without indexes, and enforced schemas with ALTER TABLE. Through kestro-server: 4 PHP 8.3
processes (2 over TCP, 2 over the Unix socket reading in-process through FFI) each ran a
BEGIN/UPDATE/ROLLBACK transaction among their SQL checks, 2 Python processes and Python
embedded mode passed, and a PHP client committed a transaction, joined two tables written
by PHP and Python processes, and ran an IN (SELECT ...) subquery with the expected results.

### Linux round 4: SQL, indexes, column cache, PHP 8.3 (same server, same day)

Built with GCC 13.3 and CMake 3.28; PHP 8.3.6 (php-cli, php-sqlite3; FFI built in). CTest 2/2 and the portable suite 32/32
passed, including SQL, queries against brute force, unique indexes, increments and the
randomized column-cache test. kestro-server on TCP and a Unix socket with a token: 2 PHP
processes over TCP, 2 PHP processes over the Unix socket reading in-process through FFI,
2 Python processes reading in-process through ctypes, and Python embedded mode all passed;
their SQL inserts from 6 processes produced exactly 120 rows; SIGINT stopped the server
cleanly. `bench/lang-compare.mjs --lang php --workers 1,2,4`: KestroDB first in every row
against SQLite (`results/php-linux.json`). `bench/stress.mjs`: 100/500/1,000 connections
with no errors (about 22,000 operations/s on 2 shared vCPUs; 1,000
connections used 46 MB); 1,000,000 keys used 121 MB (126 bytes/key).

### Linux round 3: server mode, C library, Python (same server, same day)

Built with GCC 13.3, CMake 3.28 and OpenSSL 3. CTest 2/2 passed (portable and
POSIX suites); the portable suite passed 25/25 including the multi-process reader.
`kestro-server` listened on TCP and a Unix socket at once; 4 Python 3.12 processes ran
`test_kestrodb.py` over the Unix socket with in-process reads (`read_from`,
`libkestrodb.so` through ctypes), one more without; `test_embedded.py` passed;
SIGINT stopped the server with `KESTRODB STOPPED`. `bench/lang-compare.mjs --lang
python --workers 1,2,4`: KestroDB first in every row against SQLite
(`results/python-linux.json`). Node tests were not run there (Node 20; the tests need
Node 22+).

### Linux round 2 (same server, same day)

After `fdatasync`, lazy snapshot sections, delayed background verification and the
leaner group commit: 24/24 native, 30/30 POSIX and 33/33 Node tests passed. On this
shared 2-vCPU machine the benchmark (`results/linux-ubuntu-2404-bench.json`) shows clear leads in
durable single writes and open, and results within a few percent of LMDB, with the
leader changing from run to run, for reads, in-flight writes and the mixed workload.

# KestroDB 0.7 validation — 2026-11-15


## Executed checks

| Check | Result |
|---|---|
| Linux x86-64 GCC 14 release build | Passed |
| Portable C++ suite | 16/16 passed on Linux |
| Existing POSIX C++ suite | 30/30 passed on Linux |
| Node.js 24 suite | 18/18 passed on Linux |
| Portable BLAKE3 fallback build | Both native suites passed (46 tests) |
| AddressSanitizer + UndefinedBehaviorSanitizer | Both native suites passed (46 tests); ASAN_OPTIONS=detect_leaks=0 |
| ThreadSanitizer | Both native suites passed (46 tests) |
| Linux 0.4 → 0.7 append → 0.4 verification | Passed |
| Linux 0.6 → 0.7 append → 0.6 verification | Passed |
| Windows x64 MinGW cross-build | Worker, fault-test worker and portable test .exe built |
| Windows executable inspection | x64 PE console binary; bcrypt.dll, KERNEL32.dll, msvcrt.dll imports; no separately shipped MinGW DLL required |
| Windows native runtime | Not available; no pass claimed |
| Windows via Wine | Could not execute: wineserver socket creation denied by environment |
| macOS Intel/ARM64 build/runtime | Not available; no pass claimed |
| Native CI matrix | Included; not run for 0.7 |
| Physical power loss / hardware failure | Not performed |

The 64 Linux test cases consist of 46 C++ tests plus 18 Node tests. Re-running
those under another configuration does not create additional independent cases.
BLAKE3's 33 known-answer vectors are assertions inside one test case.

LeakSanitizer itself failed because the build container did not allow its required
process inspection. The rerun disables only leak detection; address and undefined-behavior instrumentation remain
active. No memory-leak coverage is claimed. TSan completed without a reported race.

The native tests cover atomic conflicts and CAS races, 8 reader + 8 writer threads,
consistent getMany snapshots, byte values including NUL/CR/LF/Ctrl-Z, hardlink owner
locks, Unicode and >260-character paths, >4 GiB positioned I/O, truncation,
corruption detection, metadata checks, tail recovery, backups, no-overwrite
publication, and streaming checkpoints larger than 64 MiB. Process tests cover
six failure boundaries, connection poisoning after flush errors, owner release
after forced termination, and exclusion of test hooks from the release worker.

Forced process termination leaves the OS cache alive. These tests do **not**
simulate a machine losing power. Injected sync errors do not reproduce all disk
failure patterns. Passing Linux tests does not establish Win32 or macOS behavior.

## Reproduction

```text
npm run build
npm test
```

`node scripts/test-binaries.mjs` runs the test binaries that `npm run build` leaves in
`bin/`. `.github/workflows/platforms.yml` builds and tests Linux x64, Linux ARM64 and
Windows on every push and pull request, including portable-hash builds.

Sanitizer builds on Linux:

```text
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DKESTRO_SANITIZER=address
cmake --build build-asan --parallel 2
ctest --test-dir build-asan --output-on-failure
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DKESTRO_SANITIZER=thread
cmake --build build-tsan --parallel 2
ctest --test-dir build-tsan --output-on-failure
```

Only if leak detection is unavailable in your own sandbox, use
`ASAN_OPTIONS=detect_leaks=0` for the ASan test command and record that limitation.
Do not disable it by default on a capable native machine.

## Linux performance regression spot check (0.7, historical)

This is a 0.6-vs-0.7 regression check, not a new database leaderboard and not a
Windows/macOS benchmark. Same compiler, optimization, BLAKE3 library, 20,000 keys,
512-byte values, 200,000 durable writes in batches of 1,000, then 1,000,000 point
reads, 1,000,000 values through getMany, and 1,000,000 reads across eight threads.
Every run verifies checksums and the final database. Both engines use the same
single-barrier policy, synchronizing before acknowledging each commit.

Five trials per version and read mode alternate version order. Final runs share
the same eight-CPU affinity and run after other build/test jobs finish. Results
include raw trials and medians in `results/linux-regression.json`. An earlier,
unpinned exploratory run is retained separately as `results/linux-regression-initial.json`;
its first trials overlapped the tail of build/test work and are not the final
comparison. A second pinned exploratory run is retained too
(`results/linux-regression-pinned-exploratory.json`); package tests may
have overlapped its final trial. The final reported run starts only after the
package test command completes. Shared-host noise still limits interpretation. No result here changes
the earlier 0.6 comparisons against SQLite or other databases.

The benchmark program, its driver script and the frozen 0.6 engine used for this check
were not carried into this repository, so it cannot be rerun from this tree; the raw trials
remain in `results/linux-regression.json`. Use native OS benchmarking before making
cross-platform performance claims.

## Final regression measurements

Median operations/second (getMany counts returned values). Five runs per version
and mode; ratio is 0.7 throughput divided by 0.6 throughput.

| Read mode | Workload | 0.6 | 0.7 | Change |
|---|---|---:|---:|---:|
| mapped | Batched writes | 362,706 | 352,950 | -2.7% |
| mapped | One-thread point reads | 540,835 | 507,632 | -6.1% |
| mapped | getMany values | 910,957 | 957,834 | +5.1% |
| mapped | Eight-thread point reads | 4,586,390 | 5,019,400 | +9.4% |
| pread | Batched writes | 394,081 | 424,231 | +7.7% |
| pread | One-thread point reads | 450,936 | 377,909 | -16.2% |
| pread | getMany values | 746,605 | 738,275 | -1.1% |
| pread | Eight-thread point reads | 2,411,760 | 2,168,320 | -10.1% |

Results are mixed: the final mapped point-read median is 6.1% lower, while
eight-thread mapped reads are 9.4% higher; positioned point reads are 16.2%
lower. Earlier runs varied materially. These observations warrant further
profiling on dedicated hardware; this release claims portability changes,
not a general speed improvement or verified performance parity.

## Results and fixtures

The `results/` directory holds the benchmark records cited here: `bench-history.json`
(charted in `index.html`), the PHP and Python multi-process sets (`php-*.json`,
`python-*.json`, gathered into `lang-results.js`), the 0.8 Linux benchmark
(`linux-ubuntu-2404-bench.json`) and the 0.7 regression trials (`linux-regression*.json`).
Build and test logs are not kept in the repository. Fixtures under `fixtures/` were
produced by the actual older Linux workers; their source versions and original statistics
are recorded in provenance.json.

# Contributing to KestroDB

Thanks for helping. Bug reports, test cases, benchmarks on your hardware, documentation
fixes and code are all welcome.

## Reporting bugs

Open an issue with the KestroDB version, your OS and CPU, how you use it (in-process from
Node, the server, which client), and the smallest steps that reproduce it. For data
problems, say whether `db.verify()` (or `kestro-server admin verify`) reports anything.
Security problems go through [SECURITY.md](SECURITY.md), not public issues.

## Building

You need CMake 3.20+, a C++17 compiler and Node.js 22+:

| OS | Prerequisites |
|---|---|
| Windows x64 | Visual Studio 2022 Build Tools ("Desktop development with C++", a Windows SDK) |
| Linux | GCC or Clang, Ninja or Make, OpenSSL 3 headers (`libssl-dev`) |
| macOS | Xcode Command Line Tools |

```text
npm run build        # bin/: the Node addon, server, worker, C library, native tests
npm test             # the C++ suites (CTest) and every Node suite in tests/
node example.mjs     # a tour of the API
```

The PHP, Python, Java, Go, Rust and .NET client suites run when those toolchains are
installed and are skipped otherwise. On Linux, `scripts/linux-check.sh` runs what CI runs:
the build, every suite, AddressSanitizer + UBSan, ThreadSanitizer and the fuzz targets.

## Pull requests

- Keep each pull request to one change, with tests that fail without it. Engine changes
  usually need a C++ test (`src/tests_portable.cpp`) or a Node test (`tests/*.test.mjs`).
- Durability, recovery and concurrency changes need extra care: run the power-loss suite
  (part of `npm test`), the sanitizers, and `tests/crash.test.mjs`.
- Performance claims need numbers: `node bench/compare.mjs --runs 5` and
  `node bench/sql-compare.mjs --runs 5`, before and after, on the same machine.
- Match the style of the code around your change (dense C++, small helpers, comments that
  say why). No new runtime dependencies for the engine or the Node package.
- Update the docs your change touches: [SQL.md](SQL.md) for SQL, [docs/GUIDE.md](docs/GUIDE.md)
  for the API and server, [CLIENTS.md](CLIENTS.md) for clients, [FORMAT.md](FORMAT.md) for
  anything on disk, and add a line to [CHANGELOG.md](CHANGELOG.md).
- The file format is a contract: files written by older versions must keep opening. Changes
  to it need a format test in `fixtures/` and a note in FORMAT.md.

## License

KestroDB is under the [Apache License 2.0](LICENSE). By submitting a contribution you agree
that it is licensed under the same terms (section 5 of the license), and that you have the
right to submit it.

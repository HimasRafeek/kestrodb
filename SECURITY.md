# Security policy

## Reporting a vulnerability

Please do **not** open a public issue for a security problem. Report it privately through
GitHub: open the repository's **Security** tab and choose **Report a vulnerability**
(GitHub private vulnerability reporting). Include:

- the KestroDB version (`kestro-server admin status`, or `version` in `stats()`) and platform;
- what an attacker can do, and what they need first (network access to the server port, a
  user with some role, write access to the database file, ...);
- steps or a small program that reproduces it.

You should get a first answer within a week. Fixes are released as a new patch version and
described in [CHANGELOG.md](CHANGELOG.md); reporters are credited unless they ask not to be.

## Supported versions

KestroDB is before 1.0: only the latest release gets security fixes.

| Version | Supported |
|---|---|
| 0.9.x | yes |
| older | no |

## Scope and threat model

In scope:

- `kestro-server`: anything a network client can do beyond its role (`readonly`,
  `readwrite`, `admin`, `replica`, `tables=` limits), before or after logging in;
  authentication, TLS and client certificates; denial of service that a single client can
  cause despite the limits in [docs/GUIDE.md](docs/GUIDE.md#server-mode-and-other-languages);
  the metrics endpoint;
- the database file: a crafted file that makes KestroDB crash, hang, read or write outside
  its buffers, or return damaged data without `CORRUPT` (files are checked with SHA-256 and
  keyed checksums);
- the clients' protocol handling, and SQL that crashes or hangs the engine.

Out of scope: anyone who can already write the database file or run code as the server's
user; a server started with `--no-auth` or `--metrics-public` and exposed to a network;
hardware that does not honor flush requests.

The fuzz targets in `fuzz/` (SQL parser and executor, wire protocol, file open) and the
server tests in `tests/security.test.mjs` are good starting points.

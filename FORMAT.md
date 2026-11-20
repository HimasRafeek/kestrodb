# Native binary format version 2

This format is incompatible with the earlier JavaScript version 1. All integers
are little-endian. Strings/values are length-delimited bytes; Node uses UTF-8 JSON.

## Metadata

The first 8192 bytes contain two 4096-byte slots. An entirely zero slot is unused.
Each populated slot has these fields:

| Offset | Length | Field |
| ---: | ---: | --- |
| 0 | 8 | `KDBNAT02` magic (`HDBNAT02` in files written before the rename to KestroDB; both are read) |
| 8 | 8 | Commit generation |
| 16 | 8 | Last transaction ID |
| 24 | 8 | Exclusive committed byte boundary |
| 32 | 16 | Database random ID |
| 48 | 32 | Last group SHA-256 |
| 80 | 32 | SHA-256 over bytes 0..79 (0.9; zero in older files) |
| 112 | 3952 | Zero padding |
| 4064 | 32 | SHA-256 over the preceding 4064 bytes |

Generation G is stored in slot G modulo 2. Initial generation 0, transaction 0
and boundary 8192 are stored in slot 0. Two nonempty slots must be consecutive
generations of the same database, with increasing boundaries and nondecreasing
transaction IDs. Any bad nonempty slot is rejected, including the older one.

Since 0.9 a slot is accepted when its whole-page digest fails but the digest at byte 80
holds: the fields and their digest share the first 512-byte sector, so a power loss that
tears the slot at a sector boundary leaves them whole. A clean close writes the older slot
again with the committed state, so both slots then agree.

## Transaction groups

| Offset | Length | Field |
| ---: | ---: | --- |
| 0 | 8 | `KDBG0002` magic (`HDBG0002` before the rename; both are read, and one file may hold both) |
| 8 | 8 | Payload length |
| 16 | 8 | Transaction ID |
| 24 | 4 | Flags: 0 normal, 1 initial checkpoint |
| 28 | 4 | Operation count |
| 32 | 32 | Previous group SHA-256 |
| 64 | Variable | Operations |
| After payload | 32 | SHA-256 of header and payload |

Each operation contains kind (1 byte: set=1, delete=0), expected version (8), key
length (4), value length (4), key bytes, then value bytes. `UINT64_MAX` means
unconditional; 0 means key absent. Delete has zero value length. Keys must be
unique in a group. A normal group advances the transaction ID by one.

A checkpoint is permitted only as the first group in a file. Its transaction
ID preserves the source's last transaction ID. Each operation is a set whose
expected-version field holds that entry's original version. This lets the new
file omit history while preserving compare-and-swap tokens. Later normal groups
continue incrementing from the preserved transaction ID. Normal transaction payloads remain bounded to 64 MiB. Version 0.6 streams checkpoint
payloads and can exceed that limit; checkpoint counts still fit uint32 and all
keys must fit the RAM index. This is a reader implementation extension, not a
change to byte layout. Older 0.4/0.5 readers reject checkpoints above 64 MiB or
their older count bound. Small checkpoints remain compatible. A large compacted
file and its backups require a 0.6 or newer reader. No live file is automatically replaced.

After verified replay, the in-memory hash index stores key, value offset, value
length, version and a 256-bit digest of the value. Point reads check that value digest.
The digest is reconstructed from a verified group rather than stored a second
time on disk. Version 0.4 uses SHA-256 for this RAM-only digest; 0.5/0.6/0.7 use BLAKE3-256;
0.8 uses a keyed digest: SipHash-2-4-128 for values up to 2 KiB and keyed BLAKE3
truncated to 128 bits above. The key is random per store after a replay, or the one
stored in the index snapshot the store was opened from. Since the compact key index
(0.9.0) RAM keeps the first 64 bits of each 128-bit checksum and compares those; the index
snapshot still stores all 16 bytes.
Stored metadata and groups retain SHA-256 in all these versions. Values stay on disk,
although the OS may cache their pages. Version 0.5 copies through read-only mappings
by default, with a selectable direct-pread path.

## Publication protocol

1. Under the writer mutex, validate all expected versions and encode the group.
   With group commit (0.8) this covers several queued transactions in order: each
   is validated against the transactions accepted before it and encoded as its own
   group with its own transaction ID, chained to the previous group. A transaction
   that fails validation is dropped from the commit and fails alone.
2. Make sure the zero-filled reserve covers the new groups (see below), then write
   the groups contiguously. If ordered-sync mode is enabled, flush the file now.
3. Write the next metadata slot and flush the file, covering every write (Windows: the
   groups are written through to the disk first, and the slot follows without waiting;
   see below).
4. Lock each transaction's index shards in turn, update references atomically, and
   return success to every transaction in the commit.

Several groups between two metadata updates were always valid: replay walks groups
from the data start to the committed boundary. Readers of 0.4 and later open
group-committed files.

### The commit point (0.8, from 2026-11-17)

A group that is complete on disk and continues the chain is committed: its header
magic, the next transaction ID, the previous group's hash, and its own SHA-256 all
check out. The metadata slot is a starting point for recovery, not the commit itself.
On Windows (write-through) a commit therefore waits for one disk write, its groups;
the slot is then written through the OS cache, where readers in other processes see it
at once, and it reaches the disk lazily (it is flushed on a clean close). Linux and
macOS keep one flush per commit covering groups and slot: a power loss before it returns
can leave the new slot on disk without all of its groups. Recovery (0.9) then takes the
newer slot only when the groups after the older slot lead to it, and otherwise starts from
the older slot (that commit was never acknowledged).

Opening as the owner: the newest valid slot is the start (one damaged slot is
tolerated: the other one and the chained groups recover the state), then every
complete group after its boundary that continues the chain is applied; if anything
was found, the slots are rewritten to match before the store opens. A group whose
write was interrupted fails its SHA-256 and ends the walk, as before. A transaction
that reached the disk but was never acknowledged (the process stopped first) may
therefore be present after recovery; an acknowledged one is always present. A damaged
group or a file truncated inside the committed groups still refuses to open.
Versions before this rule can lose groups committed after the last slot that reached
the disk, if they are the first to open a file after a power failure.

Group commit of SQL writes: autocommit INSERT, UPDATE and DELETE statements from many
threads or connections are run by one leader in arrival order inside one session and
committed together; while a group's groups are being written, the leader runs the next
statements (they see the in-flight group through the session; other readers see it
only once it is durable and installed).

### Reserve (0.8)

The engine keeps the file longer than the committed boundary: a zero-filled reserve,
grown in steps of 1/8 of the committed size (1 MiB to 64 MiB). Commits overwrite
reserved bytes instead of growing the file, so a commit does not also have to persist
a file-size change, and Windows can memory-map the bytes before they are written.
Bytes past the committed boundary are the uncommitted tail this format has always
defined, and every reader validates the committed prefix and truncates them on open.
0.8 also trims the reserve on a clean close.

### Windows write-through (0.8)

On Windows the owning handle is opened with FILE_FLAG_WRITE_THROUGH. Each write then
completes only after it reaches stable media, so a commit's groups are durable before
its metadata slot is written, and the slot is durable before the commit is
acknowledged: data-before-metadata ordering in both policies, without
FlushFileBuffers. Write-through may persist a file-size change lazily, so the engine
calls FlushFileBuffers after every reserve growth, before any commit that relies on it.

On Linux and macOS, the 0.5–0.8 default omits the optional first barrier. A successful platform file flush is required
before acknowledgement in both modes. Single-barrier mode can persist a new
metadata slot ahead of its data if the unacknowledged write is interrupted. Reopen
then starts from the other slot and recovers the chained groups that are intact.
Select orderedSync=true for the original data-before-metadata policy (on Windows it
also writes the slot through, waiting for two disk writes per commit). A torn slot is
recovered from the other slot and the chained groups. This is a runtime policy, not a file-format change.

Readers copy an index reference under its shard's shared lock and then read its immutable
file range outside that lock. They can overlap disk writes. There is no
multi-call snapshot isolation: separate gets may observe different commits.

Snapshots reconstruct metadata and copy the committed prefix. Compaction streams
a new checkpoint group through a 1 MiB buffer while hashing it incrementally.
Checkpoint replay uses bounded buffering and one value at a time; it still builds
a full key index. A checkpoint must validate completely before Store opens.
0.8 replays from one read-only mapping of the committed range, verifying each group's
SHA-256 on worker threads ahead of the in-order checks, in windows of 65,536 groups.
If the file cannot be mapped, the bounded-read replay above is used instead.
Snapshots and compaction use a temporary file in the destination filesystem,
flush and validate it, then publish without overwriting. POSIX backends also fsync the parent directory; Windows uses MoveFileExW WRITE_THROUGH and has no directory-fsync equivalent in this implementation.
Neither silently replaces an existing database.

getMany captures all requested references under shared locks on their shards,
then reads immutable value ranges and checks every value digest. It returns one
consistent set, including missing keys, but does not create a long-lived snapshot
handle for later calls.

## Index snapshot (0.8)

An index snapshot is an optional cache in the uncommitted tail. It starts exactly at the
committed boundary (`end` of the current metadata slot); the bytes after it, if any,
are ignored. All integers are little-endian.

| Offset | Length | Field |
| ---: | ---: | --- |
| 0 | 8 | `KDBIDX02` magic (an older `HDBIDX02` snapshot is ignored and rebuilt) |
| 8 | 8 | Commit generation it describes |
| 16 | 8 | Last transaction ID |
| 24 | 8 | Committed boundary (its own start offset) |
| 32 | 16 | Database random ID |
| 48 | 32 | Last group SHA-256 (the chain head) |
| 80 | 8 | Entry count |
| 88 | 8 | Snapshot length in bytes, trailer included |
| 96 | 48 | Read-checksum keys: SipHash (16), BLAKE3 (32) |
| 144 | 8 | Index hash seed |
| 152 | 8 | Zero |
| 160 | 64 × 48 | Per shard: section offset (8), section length (8), BLAKE3-256 of the section (32) |
| … | | 64 sections |
| length − 32 | 32 | BLAKE3-256 of the header and the table |

A section holds `u32 count`, `u32 slots` (a power of two, at least 4, load at most
2/3), `slots × u64`, then `count` entries: `u16` key length, key, `u64` value
offset, `u32` value length, `u64` version, 16-byte read checksum. A slot is 0 when
empty, otherwise the key hash's low 32 bits shifted left 32, OR the entry's offset from
the section start; lookups probe linearly from `hash & (slots − 1)`. The key hash is
the in-memory index hash under the stored seed, and the shard is its top 6 bits.

A reader uses a snapshot only if the magic, generation, transaction ID, boundary,
database ID and chain head equal the current metadata, the trailer matches and the
section counts add up. Otherwise it replays the log. 0.8 checks each section's hash when
the section is first used and, if one fails, rebuilds the index by replaying the log. Readers before
0.8 ignore the snapshot and remove it with the rest of the tail. 0.8 removes or replaces
it when it commits, and writes a new one on clean close or once commits are quiet.

Using a snapshot skips the replay's checks at open. 0.8 performs them afterwards in the
background (or before open returns, with `verifyOnOpen`) and refuses every operation
if they fail.

## Platform contract in 0.8

A durable commit means fsync on Linux, F_FULLFSYNC on macOS, and write-through writes
into the preallocated reserve on Windows (FlushFileBuffers when the reserve grows). The
on-disk layout and SHA-256 frames are unchanged since 0.6. Windows 0.8 is natively
tested; Linux and macOS 0.8 await the CI workflow. See PORTABILITY.md for namespace
durability limits.

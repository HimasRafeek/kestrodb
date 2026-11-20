# Platform implementation notes

The storage engine calls `src/platform.hpp` for handles, positioned I/O, file
ownership, file size/truncation, flushes, snapshot publication, random IDs,
shared/exclusive locks and mapping. `src/sha256.hpp` provides the same SHA-256
interface using each platform's crypto backend. Stored frames remain SHA-256;
BLAKE3 remains the RAM-index value checksum. No serialized checksum was changed.

| Operation | Linux | macOS | Windows |
|---|---|---|---|
| File I/O | pread/pwrite, 64-bit off_t | pread/pwrite | OVERLAPPED ReadFile/WriteFile; explicit 64-bit offset and per-thread event |
| Owner lock | flock, nonblocking exclusive | flock, nonblocking exclusive | LockFileEx, nonblocking exclusive byte at offset 2^62 |
| Thread locks | pthread rwlock | pthread rwlock | SRWLOCK |
| Durable commit | fsync | fcntl(F_FULLFSYNC) | FILE_FLAG_WRITE_THROUGH writes into a preallocated reserve; FlushFileBuffers when the reserve grows |
| New snapshot | link without overwrite, parent fsync, remove temporary name | same POSIX sequence | MoveFileExW WRITE_THROUGH without replacement or cross-volume copy |
| Directory flush | fsync on parent directory | fsync on parent directory | unavailable in this implementation |
| Point reads | mmap default, pread optional | mmap default, pread optional | MapViewOfFile default (views cover written bytes only, replaced as data grows), ReadFile optional |
| SHA-256 | OpenSSL 3 EVP | CommonCrypto | CNG reusable SHA-256 hash handles |
| Random UUID bytes | /dev/urandom | /dev/urandom | BCryptGenRandom |
| Worker paths | UTF-8 | UTF-8 | UTF-8 API → UTF-16 extended absolute paths; wmain for CLI |
| Protocol streams | byte streams | byte streams | explicitly switched to binary mode |

Each Windows operation waits for its own completion before stack OVERLAPPED
storage can expire. Concurrent reads use independent offsets, not a shared seek
pointer. The ownership byte lies outside valid database data offsets (<2^60), so
it does not block normal reads/writes or flushing. The file handle releases that
lock when the owning process exits. Tests exercise second opens and hardlink aliases.

Windows paths reject NUL, invalid UTF-8, device namespace paths and alternate data
streams. Core operations use Unicode APIs and extended absolute paths. Explicit
wide CLI arguments preserve non-ASCII filenames from Node. The worker manifest
opts into long paths. Final-component symlinks/reparse points are refused; this is
not a sandbox for untrusted parent directories. Database contents can hold all bytes;
Node's document API encodes JSON. CR/LF/Ctrl-Z must not alter binary framing.

CMake selects official BLAKE3 assembly for Unix, MinGW or MSVC on x86-64; GNU/Clang
ARM64 selects NEON. A portable C fallback is available and tested on Linux. MSVC
ARM64 selects portable hashing. Other 64-bit CPU targets require the portable
option and remain unvalidated. SIMD availability is checked by upstream dispatch.

## Evidence and remaining work

Linux tests cover the refactored platform calls, binary protocol, data integrity,
crash hooks, concurrency, aliases, long paths and >4 GiB positioned I/O (0.7).
0.8 was first built and tested natively on Windows 10 with MSVC 2022: 16 native and 31
Node tests, including mapped reads across view replacement, corruption detection through a
mapping, write-through commits under the process-fault tests, and the in-process addon.
Since 0.9.0, CI builds and tests Linux x64, Linux ARM64 and Windows on every push and pull
request; the macOS backend passed every suite on GitHub's runner through 2026-11-19 and is
not run in CI now.

Before treating a target as validated, run `npm run build` and `npm test` on that
native OS and keep the results. Then test Linux → Windows → macOS → Linux file
handoffs with continued writes and verification. The CI workflow
(`.github/workflows/platforms.yml`) performs native builds and tests, including the
Linux-created compatibility fixtures. Actual power-cut, disk-full and hardware failure
testing remain separate work.

Windows cannot map past end of file, so the engine maps only bytes it has already
written: committed data plus the zero-filled reserve. When committed data outgrows a
view, a longer view replaces it; the old view stays mapped until the store closes, so
readers holding references into it stay valid. Views of local files are coherent with
WriteFile. Use readMode 'pread' to read through ReadFile instead.

The addon's Windows import library is generated from cmake/node_api.def and names
node.exe, so the addon loads only in a host executable of that name (not, for example,
Electron without a delay-load hook). Linux commits use fdatasync; an O_DSYNC equivalent
of the Windows write-through path is neither implemented nor tested.

## Primary API references

- [CreateFileW](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew)
- [ReadFile / OVERLAPPED](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-readfile)
- [LockFileEx](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-lockfileex)
- [FlushFileBuffers](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-flushfilebuffers)
- [MoveFileExW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexw)
- [BCryptCreateHash reusable contexts](https://learn.microsoft.com/en-us/windows/win32/api/bcrypt/nf-bcrypt-bcryptcreatehash)
- [Apple fsync / F_FULLFSYNC discussion](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/fsync.2.html)

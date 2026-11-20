/* KestroDB C API: one shared library (kestrodb.dll / libkestrodb.so / libkestrodb.dylib) that
 * any language with a C foreign-function interface can load (PHP FFI, Python ctypes,
 * Go cgo, Java FFM/JNA, C# P/Invoke, Rust, Ruby FFI, ...).
 *
 * Two ways to open a database:
 *   KESTRO_OPEN_READER  Reads in this process while another process writes (usually
 *                      kestro-server). Any number of processes can do this at once; each
 *                      read sees every write acknowledged before it started. Writes go
 *                      through the server. Handles are shared per path within a process
 *                      and stay open for the process lifetime (like a persistent
 *                      connection), so opening again is free.
 *   KESTRO_OPEN_OWNER   This process is the only writer (an embedded database, like
 *                      SQLite in one process). Add KESTRO_OPEN_CREATE to create the file.
 *
 * Keys are bytes (1-1024). The language clients store collection + "\0" + key, the same
 * keys kestro-server uses, so a database is shared between all of them.
 * Every function that can fail takes err/errlen and writes a message that starts with an
 * error kind (INPUT, CONFLICT, CORRUPT, STALE, LOCKED, IO, ...). */
#ifndef KESTRODB_H
#define KESTRODB_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#if defined(_WIN32)
#define KESTRO_API __declspec(dllexport)
#else
#define KESTRO_API __attribute__((visibility("default")))
#endif

typedef struct kestro_db kestro_db;
enum { KESTRO_OPEN_READER = 0, KESTRO_OPEN_OWNER = 1, KESTRO_OPEN_CREATE = 2 };
/* expected_version for writes: KESTRO_ANY = unconditional, 0 = must not exist, n = must be at version n. */
#define KESTRO_ANY (-1)

KESTRO_API const char* kestro_version(void);
KESTRO_API kestro_db* kestro_open(const char* path, int mode, char* err, size_t errlen);
/* 1 found, 0 missing, -1 error. *value points to a per-thread buffer valid until this
 * thread's next kestro_get on any handle. */
KESTRO_API int kestro_get(kestro_db* db, const char* key, size_t klen, uint64_t* version, const char** value, size_t* vlen, char* err, size_t errlen);
/* Up to 4096 keys read from one committed state. payload: u32 count, then per key
 * u32 length + bytes. *reply (per-thread buffer): u8 0, u32 count, then per key u8 found
 * [, u64 version, u32 length, value]. 0 ok, -1 error. */
KESTRO_API int kestro_get_many(kestro_db* db, const unsigned char* payload, size_t n, const unsigned char** reply, size_t* rlen, char* err, size_t errlen);
/* Writes (owner handles only). Durable when they return. 0 ok, -1 error. */
KESTRO_API int kestro_set(kestro_db* db, const char* key, size_t klen, const char* value, size_t vlen, int64_t expected_version, uint64_t* version, char* err, size_t errlen);
KESTRO_API int kestro_delete(kestro_db* db, const char* key, size_t klen, int64_t expected_version, uint64_t* version, char* err, size_t errlen);
/* All-or-nothing transaction in the server's batch encoding (see src/protocol.hpp):
 * u32 count, then per operation: u8 kind (1 set, 0 delete), u64 expected (all 0xff = any),
 * u32 key length, u32 value length, key, value. */
KESTRO_API int kestro_batch(kestro_db* db, const unsigned char* payload, size_t n, uint64_t* version, char* err, size_t errlen);
/* One SQL statement (see SQL.md), owner handles. params: a JSON array for the ?
 * placeholders (NULL or "" for none). *result (per-thread buffer, valid until this
 * thread's next kestro_sql): {"rows":[...]} or {"changes":n,"lastInsertId":id}. 0 ok, -1 error. */
KESTRO_API int kestro_sql(kestro_db* db, const char* sql, size_t sqllen, const char* params, size_t plen, const char** result, size_t* rlen, char* err, size_t errlen);
KESTRO_API int64_t kestro_count(kestro_db* db);
/* Online compaction: rewrites the file to hold only current values while reads and writes
 * continue. before/after: committed bytes of the old file at the switch and of the new one. */
KESTRO_API int kestro_compact(kestro_db* db, uint64_t* before, uint64_t* after, char* err, size_t errlen);
/* Online backup to dest (a new file); commits continue meanwhile. Owner handles. 0 ok, -1 error. */
KESTRO_API int kestro_backup(kestro_db* db, const char* dest, uint64_t* txid, char* err, size_t errlen);
/* Continuous archive in dir (archive.kdb + timeline): appends what was committed since the last call. */
KESTRO_API int kestro_archive(kestro_db* db, const char* dir, uint64_t* txid, char* err, size_t errlen);
/* A new database at dest from source cut after txid (0: all), or at the last archived point at
 * or before at_ms (unix milliseconds; -1: unused). */
KESTRO_API int kestro_restore(const char* source, const char* dest, uint64_t txid, int64_t at_ms, uint64_t* restored_txid, char* err, size_t errlen);
/* Reader handles are shared and cached; closing one ends it for this process. */
KESTRO_API void kestro_close(kestro_db* db);

#ifdef __cplusplus
}
#endif
#endif

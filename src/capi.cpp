// C API (see kestrodb.h), built as the kestrodb shared library.
#include "kestrodb.h"
#include "reader.hpp"
#include "protocol.hpp"
#include <map>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

struct kestro_db {
  std::unique_ptr<kestro::Store> store;
  kestro::Store::SqlSession session;  // BEGIN ... COMMIT state of this handle
  std::unique_ptr<kestro::Reader> reader;
  std::string path;
  bool cached = false;
};

namespace {
std::mutex cache_m;
std::map<std::string, kestro_db*>& cache() { static auto* c = new std::map<std::string, kestro_db*>(); return *c; }

// The engine keeps worker threads and cached reader handles for the process lifetime, so
// the library must never be unloaded (PHP FFI, for one, unloads libraries per request).
void pin_library() {
  static bool pinned = [] {
#ifdef _WIN32
    HMODULE self;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&pin_library), &self);
#else
    Dl_info info;
    if (dladdr(reinterpret_cast<void*>(&pin_library), &info) && info.dli_fname) dlopen(info.dli_fname, RTLD_NOW | RTLD_NODELETE);
#endif
    return true;
  }();
  (void)pinned;
}
void fail(char* err, size_t errlen, const char* what) {
  if (!err || !errlen) return;
  size_t n = strlen(what);
  if (n >= errlen) n = errlen - 1;
  memcpy(err, what, n);
  err[n] = 0;
}
template <class F> int guard(char* err, size_t errlen, F&& f) {
  try { return f(); } catch (const std::exception& e) { fail(err, errlen, e.what()); } catch (...) { fail(err, errlen, "UNKNOWN error"); }
  return -1;
}
uint64_t expected_of(int64_t e) { return e < 0 ? kestro::ANY : uint64_t(e); }
kestro::Store& owner(kestro_db* db) { kestro::require(db && db->store, "INPUT this handle is a reader; write through kestro-server or open as owner"); return *db->store; }
}  // namespace

extern "C" {

KESTRO_API const char* kestro_version(void) { return "0.9.0"; }

KESTRO_API kestro_db* kestro_open(const char* path, int mode, char* err, size_t errlen) {
  pin_library();
  kestro_db* out = nullptr;
  guard(err, errlen, [&] {
    kestro::require(path && *path, "INPUT path");
    std::string p(path);
    if (mode == KESTRO_OPEN_READER) {
      std::lock_guard<std::mutex> lock(cache_m);
      auto& c = cache();
      auto it = c.find(p);
      if (it != c.end()) { out = it->second; return 0; }
      auto db = std::make_unique<kestro_db>();
      db->reader = std::make_unique<kestro::Reader>(p);
      db->path = p; db->cached = true;
      out = db.release();
      c[p] = out;
      return 0;
    }
    kestro::require(mode == KESTRO_OPEN_OWNER || mode == (KESTRO_OPEN_OWNER | KESTRO_OPEN_CREATE), "INPUT mode");
    auto db = std::make_unique<kestro_db>();
    db->store = std::make_unique<kestro::Store>(p, (mode & KESTRO_OPEN_CREATE) != 0);
    db->path = p;
    out = db.release();
    return 0;
  });
  return out;
}

KESTRO_API int kestro_get(kestro_db* db, const char* key, size_t klen, uint64_t* version, const char** value, size_t* vlen, char* err, size_t errlen) {
  return guard(err, errlen, [&] {
    kestro::require(db && key && klen && klen <= 1024, "INPUT key");
    thread_local std::string buffer;
    uint64_t v = 0;
    bool found = db->reader ? db->reader->read(std::string_view(key, klen), buffer, v) : db->store->read(std::string_view(key, klen), buffer, v);
    if (!found) return 0;
    if (version) *version = v;
    if (value) *value = buffer.data();
    if (vlen) *vlen = buffer.size();
    return 1;
  });
}

KESTRO_API int kestro_get_many(kestro_db* db, const unsigned char* payload, size_t n, const unsigned char** reply, size_t* rlen, char* err, size_t errlen) {
  return guard(err, errlen, [&] {
    kestro::require(db && payload && n >= 4 && reply && rlen, "INPUT getMany");
    thread_local kestro::Bytes framed, out;
    framed.assign(n + 1, 0); framed[0] = 'M'; memcpy(framed.data() + 1, payload, n);
    auto keys = kestro::protocol::parse_keys(framed.data(), framed.size());
    out = kestro::protocol::encode_values(db->reader ? db->reader->getMany(keys) : db->store->getMany(keys));
    *reply = out.data(); *rlen = out.size();
    return 0;
  });
}

KESTRO_API int kestro_set(kestro_db* db, const char* key, size_t klen, const char* value, size_t vlen, int64_t expected_version, uint64_t* version, char* err, size_t errlen) {
  return guard(err, errlen, [&] {
    kestro::require(key && klen && (value || !vlen), "INPUT key or value");
    std::vector<kestro::Op> ops(1);
    ops[0].key.assign(key, klen); ops[0].value.assign(value ? value : "", vlen); ops[0].expected = expected_of(expected_version);
    auto v = owner(db).commit(ops);
    if (version) *version = v;
    return 0;
  });
}

KESTRO_API int kestro_delete(kestro_db* db, const char* key, size_t klen, int64_t expected_version, uint64_t* version, char* err, size_t errlen) {
  return guard(err, errlen, [&] {
    kestro::require(key && klen, "INPUT key");
    std::vector<kestro::Op> ops(1);
    ops[0].key.assign(key, klen); ops[0].erase = true; ops[0].expected = expected_of(expected_version);
    auto v = owner(db).commit(ops);
    if (version) *version = v;
    return 0;
  });
}

KESTRO_API int kestro_batch(kestro_db* db, const unsigned char* payload, size_t n, uint64_t* version, char* err, size_t errlen) {
  return guard(err, errlen, [&] {
    kestro::require(payload && n >= 4, "INPUT batch");
    kestro::Bytes framed(n + 1);
    framed[0] = 'B';
    memcpy(framed.data() + 1, payload, n);
    auto ops = kestro::protocol::parse_batch(framed.data(), framed.size());
    auto v = owner(db).commit(ops);
    if (version) *version = v;
    return 0;
  });
}

KESTRO_API int kestro_compact(kestro_db* db, uint64_t* before, uint64_t* after, char* err, size_t errlen) {
  return guard(err, errlen, [&] { auto r = owner(db).compact(); if (before) *before = r.before; if (after) *after = r.after; return 0; });
}
KESTRO_API int kestro_backup(kestro_db* db, const char* dest, uint64_t* txid, char* err, size_t errlen) {
  return guard(err, errlen, [&] { kestro::require(dest && *dest, "INPUT path"); auto p = owner(db).backup(dest); if (txid) *txid = p.txid; return 0; });
}
KESTRO_API int kestro_archive(kestro_db* db, const char* dir, uint64_t* txid, char* err, size_t errlen) {
  return guard(err, errlen, [&] { kestro::require(dir && *dir, "INPUT directory"); auto p = owner(db).archive(dir); if (txid) *txid = p.txid; return 0; });
}
KESTRO_API int kestro_restore(const char* source, const char* dest, uint64_t txid, int64_t at_ms, uint64_t* restored_txid, char* err, size_t errlen) {
  return guard(err, errlen, [&] { kestro::require(source && *source && dest && *dest, "INPUT path"); auto p = kestro::Store::restore(source, dest, txid, at_ms); if (restored_txid) *restored_txid = p.txid; return 0; });
}
KESTRO_API int kestro_sql(kestro_db* db, const char* sql, size_t sqllen, const char* params, size_t plen, const char** result, size_t* rlen, char* err, size_t errlen) {
  return guard(err, errlen, [&] {
    kestro::require(sql && result && rlen, "INPUT sql");
    thread_local std::string out;
    out = owner(db).sql(&db->session, std::string_view(sql, sqllen), params && plen ? std::string_view(params, plen) : std::string_view("[]"));
    *result = out.data(); *rlen = out.size();
    return 0;
  });
}

KESTRO_API int64_t kestro_count(kestro_db* db) {
  try {
    if (!db) return -1;
    return db->reader ? int64_t(db->reader->size()) : int64_t(db->store->count());
  } catch (...) { return -1; }
}

KESTRO_API void kestro_close(kestro_db* db) {
  if (!db) return;
  if (db->store) db->store->end_session(db->session);
  if (db->cached) { std::lock_guard<std::mutex> lock(cache_m); cache().erase(db->path); }
  delete db;
}

}  // extern "C"

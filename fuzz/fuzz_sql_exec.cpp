// libFuzzer: SQL statements executed against a small database (three tables with indexes, a FULLTEXT one,
// constraints and a few rows). Input: one statement per line; a line starting with '[' or
// '{' gives the parameters for the next statement. The database is recreated every 500
// inputs so it stays small. Statements may fail with any error code; none may crash, hang
// or corrupt the store (every 50 inputs the file is verified).
#include "kestro.hpp"
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

using namespace kestro;
#ifdef _WIN32
static unsigned long process_id() { return GetCurrentProcessId(); }
#else
#include <unistd.h>
static unsigned long process_id() { return (unsigned long)getpid(); }
#endif
namespace fs = std::filesystem;

static std::string g_path;
// Never destroyed: libFuzzer ends with exit(), after OpenSSL's own cleanup has run, and closing
// the store then would hash its metadata with a torn-down libcrypto (a crash at exit).
static std::unique_ptr<Store>& g_db = *new std::unique_ptr<Store>;
static size_t g_runs = 0;

static void fresh() {
  g_db.reset();
  std::error_code ec;
  for (auto ext : {"", ".idx", ".sets", ".sort"}) fs::remove(g_path + ext, ec);
  Options o; o.indexMemory = 0; o.snapshotIdleMs = 0;
  g_db = std::make_unique<Store>(g_path, true, o);
  const char* setup[] = {
    "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT NOT NULL, email TEXT UNIQUE, age INTEGER CHECK (age >= 0), city TEXT DEFAULT 'x')",
    "CREATE TABLE orders (id INTEGER PRIMARY KEY, user INTEGER REFERENCES users ON DELETE CASCADE, total REAL, status TEXT, UNIQUE (user, status))",
    "CREATE TABLE tags (id INTEGER PRIMARY KEY, order_id INTEGER REFERENCES orders ON DELETE SET NULL, label TEXT)",
    "CREATE INDEX ON users (city)", "CREATE INDEX ON orders (total)", "CREATE FULLTEXT INDEX ON tags (label)",
    "INSERT INTO users (name, email, age, city) VALUES ('a','a@x',30,'k'),('b','b@x',40,'c'),('c',NULL,NULL,'k'),('d','d@x',7,NULL)",
    "INSERT INTO orders (user, total, status) VALUES (1, 5, 'open'), (1, 7.5, 'paid'), (2, 9, 'open'), (3, NULL, 'paid')",
    "INSERT INTO tags (order_id, label) VALUES (1, 'x'), (2, 'y'), (NULL, 'z')",
  };
  for (auto q : setup) g_db->sql(q);
}

extern "C" int LLVMFuzzerInitialize(int*, char***) {
  g_path = (fs::temp_directory_path() / ("kdb-fuzz-exec-" + std::to_string(process_id()) + ".kdb")).string();
  fresh();
  return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size > 2048) return 0;
  if (++g_runs % 500 == 0) fresh();
  std::string in(reinterpret_cast<const char*>(data), size), params = "[]";
  Store::SqlSession session;
  size_t at = 0;
  for (int n = 0; at <= in.size() && n < 8; n++) {
    size_t nl = in.find('\n', at); if (nl == std::string::npos) nl = in.size();
    std::string line = in.substr(at, nl - at); at = nl + 1;
    if (!line.empty() && (line[0] == '[' || line[0] == '{')) { params = line; continue; }
    // As on a server, a statement stops at a time limit (with LIMIT) instead of running long.
    try {
      Store::StatementDeadline limit(std::chrono::milliseconds(800));  // 8 statements stay under -timeout=10
      g_db->sql(&session, line, params);
    } catch (const std::exception&) {}
    params = "[]";
  }
  g_db->end_session(session);
  if (g_runs % 50 == 0) g_db->verify();  // throws (and the fuzzer reports it) if the file is damaged
  return 0;
}

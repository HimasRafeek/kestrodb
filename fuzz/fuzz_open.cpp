// libFuzzer: opening a database file with arbitrary (usually mutated) contents. Seed the
// corpus with real files (fuzz/corpus-open: fixtures and a generated database). A damaged
// file must fail to open with an error or open and serve verified data; it must never
// crash, hang, or read outside the file.
#include "kestro.hpp"
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
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

extern "C" int LLVMFuzzerInitialize(int*, char***) {
  g_path = (fs::temp_directory_path() / ("kdb-fuzz-open-" + std::to_string(process_id()) + ".kdb")).string();
  return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size > (1u << 20)) return 0;
  std::error_code ec;
  for (auto ext : {"", ".idx", ".sets", ".sort"}) fs::remove(g_path + ext, ec);
  { std::ofstream f(g_path, std::ios::binary); f.write(reinterpret_cast<const char*>(data), std::streamsize(size)); }
  for (int mode = 0; mode < 2; mode++) {
    try {
      Options o; o.indexMemory = 0; o.verifyOnOpen = true; o.snapshotIdleMs = 0; o.indexSnapshot = false;
      o.reads = mode ? ReadMode::Pread : ReadMode::Mapped;
      Store db(g_path, false, o);
      db.verify();
      auto n = db.count(); (void)n;
      try { db.sql("SHOW TABLES"); } catch (const std::exception&) {}
      try { db.sql("SELECT * FROM users"); } catch (const std::exception&) {}
      auto v = db.get("users"); (void)v;
    } catch (const std::exception&) {}
  }
  return 0;
}

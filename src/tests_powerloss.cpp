// Simulated power loss. Every file operation of a workload is recorded (KESTRO_IO_RECORDER:
// open, write, write completion, flush, truncation). For each point in that history the test
// rebuilds disk states a sudden power loss could leave, under the platform's contract:
//   - a write on a write-through handle (Windows owner handle) is durable once complete;
//   - any other write, and any truncation, is durable only after a later flush of the file;
//     until then it may be lost, written whole, or torn: any subset of its 512-byte sectors.
// Each state must open, verify, keep every transaction acknowledged before that point, hold
// every transaction whole or not at all, in commit order (a prefix 1..m), with "last" = m.
// Compactions, renames and directory metadata are outside this model.
#define KESTRO_IO_RECORDER 1
#include "kestro.hpp"
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <set>

using namespace kestro;
namespace fs = std::filesystem;
using platform::IoEvent;

struct Recording {
  std::mutex m;
  std::vector<IoEvent> events;
  size_t size() { std::lock_guard<std::mutex> l(m); return events.size(); }
};

static std::string value_for(int i) { return "{\"i\":" + std::to_string(i) + ",\"pad\":\"" + std::string(size_t(100 + (i * 397) % 3000), char('a' + i % 26)) + "\"}"; }

int main(int argc, char** argv) {
  const int variants = argc > 1 ? std::atoi(argv[1]) : 4;
  const fs::path dir = fs::temp_directory_path() / ("kestro-powerloss-" + std::to_string(platform::process_id()));
  fs::remove_all(dir); fs::create_directories(dir);
  const std::string db = (dir / "w.kdb").u8string();
  Recording rec;
  platform::io_recorder() = [&](const IoEvent& e) { std::lock_guard<std::mutex> l(rec.m); rec.events.push_back(e); };

  // The workload: transaction i sets a/i and b/i and "last" = i; acks[i] = the number of
  // recorded events when commit returned (a crash after that point must keep it).
  std::map<int, size_t> acks; int issued = 0; size_t created_at = 0;  // creation returned at this event count
  auto txn = [](int i) { return std::vector<Op>{{"a/" + std::to_string(i), value_for(i), ANY, false}, {"b/" + std::to_string(i), value_for(i), ANY, false}, {"last", std::to_string(i), ANY, false}}; };
  Options o; o.indexMemory = 0; o.snapshotIdleMs = 2; o.verifyDelayMs = 1;
  try {
    for (int phase = 0; phase < 2; phase++) {
      Store s(db, phase == 0, o);
      if (phase == 0) created_at = rec.size();
      for (int k = 0; k < 20; k++) {
        if (k % 6 == 5) {  // a group of three transactions in one commit
          std::vector<std::vector<Op>> g; for (int j = 0; j < 3; j++) g.push_back(txn(++issued));
          std::vector<const std::vector<Op>*> ptrs; for (auto& t : g) ptrs.push_back(&t);
          auto out = s.commit_group(ptrs); for (auto& x : out) if (x.error) std::rethrow_exception(x.error);
          for (int j = issued - 2; j <= issued; j++) acks[j] = rec.size();
        } else { s.commit(txn(++issued)); acks[issued] = rec.size(); }
        if (k % 7 == 3) std::this_thread::sleep_for(std::chrono::milliseconds(10));  // lets an idle checkpoint run
      }
    }  // phase 0 closes (snapshot, trim) and phase 1 reopens from it
  } catch (const std::exception& e) { std::cerr << "FAIL workload: " << e.what() << std::endl; return 1; }
  platform::io_recorder() = nullptr;
  const std::vector<IoEvent> ev = rec.events;

  // Only the database file's own operations count (index and scratch files are caches).
  std::map<uint64_t, std::string> path_of; std::map<uint64_t, bool> through_of;
  std::vector<int> relevant(ev.size(), 0); std::vector<char> through_at(ev.size(), 0);  // as of each event (handle values are reused after a close)
  for (size_t i = 0; i < ev.size(); i++) {
    const auto& e = ev[i];
    if (e.kind == platform::IoOpen) { path_of[e.file] = e.path; through_of[e.file] = e.through; }
    relevant[i] = path_of.count(e.file) && fs::u8path(path_of[e.file]) == fs::u8path(db);
    through_at[i] = through_of.count(e.file) && through_of[e.file];
  }
  struct Piece { size_t at; bool truncate; uint64_t off; std::string data; bool through; uint64_t id; };
  std::mt19937_64 rnd(20261119);
  const std::string probe = (dir / "crash.kdb").u8string();
  size_t states = 0, checked_points = 0;
  for (size_t c = 0; c <= ev.size(); c++) {
    if (c > 0 && c < ev.size() && !relevant[c - 1]) continue;  // only right after an operation on the file
    checked_points++;
    // Pieces up to the crash point, and which of them are durable at it.
    std::vector<Piece> pieces; std::map<uint64_t, size_t> piece_of; std::set<size_t> durable, completed;
    for (size_t i = 0; i < c; i++) {
      if (!relevant[i]) continue;
      const auto& e = ev[i];
      switch (e.kind) {
        case platform::IoWrite: case platform::IoWriteCached:
          piece_of[e.id] = pieces.size();
          pieces.push_back({i, false, e.off, e.data, e.kind == platform::IoWrite && through_at[i], e.id}); break;
        case platform::IoComplete: { auto it = piece_of.find(e.id); if (it == piece_of.end()) break; completed.insert(it->second); if (pieces[it->second].through) durable.insert(it->second); break; }
        case platform::IoTruncate: pieces.push_back({i, true, e.off, {}, false, 0}); completed.insert(pieces.size() - 1); break;
        case platform::IoSync: for (size_t p : completed) durable.insert(p); break;
        default: break;
      }
    }
    int acked = 0; for (auto& [i, at] : acks) if (at <= c) acked = std::max(acked, i);
    for (int v = 0; v < variants + 2; v++) {
      // v 0: every pending piece lost; v 1: every one written; others: random, with tearing.
      std::string img;
      for (size_t p = 0; p < pieces.size(); p++) {
        const auto& pc = pieces[p];
        int mode = durable.count(p) ? 1 : v == 0 ? 0 : v == 1 ? 1 : int(rnd() % 5) < 2 ? 0 : int(rnd() % 3) < 2 ? 1 : 2;  // 0 lost, 1 whole, 2 torn
        if (mode == 0) continue;
        if (pc.truncate) { img.resize(size_t(pc.off), '\0'); continue; }
        if (img.size() < pc.off + pc.data.size()) img.resize(size_t(pc.off + pc.data.size()), '\0');
        for (size_t s = 0; s < pc.data.size(); s += 512) {
          if (mode == 2 && rnd() % 2) continue;  // this sector did not reach the disk
          size_t n = std::min<size_t>(512, pc.data.size() - s);
          memcpy(&img[size_t(pc.off + s)], pc.data.data() + s, n);
        }
      }
      states++;
      std::error_code ec; for (auto ext : {"", ".idx", ".sets", ".sort", ".compacting", ".compacted-old"}) fs::remove(fs::u8path(probe + ext), ec);
      { std::ofstream f(fs::u8path(probe), std::ios::binary); f.write(img.data(), std::streamsize(img.size())); }
      auto fail = [&](const std::string& why) {
        std::cerr << "FAIL at event " << c << " of " << ev.size() << " (variant " << v << ", " << acked << " acknowledged): " << why << std::endl;
        fs::copy_file(fs::u8path(probe), dir / ("failed-" + std::to_string(c) + "-" + std::to_string(v) + ".kdb"), fs::copy_options::overwrite_existing, ec);
        std::exit(1);
      };
      if (c < created_at) continue;  // the database did not exist yet: any leftover is acceptable
      try {
        Options po; po.indexMemory = 0; po.verifyOnOpen = true;
        Store s(probe, false, po);
        s.verify();
        int m = 0;
        for (int i = 1; i <= issued; i++) {
          auto a = s.get("a/" + std::to_string(i)), b = s.get("b/" + std::to_string(i));
          if (a.found != b.found) fail("transaction " + std::to_string(i) + " is torn");
          if (!a.found) { for (int j = i + 1; j <= issued; j++) if (s.get("a/" + std::to_string(j)).found) fail("transaction " + std::to_string(j) + " present after missing " + std::to_string(i)); break; }
          if (a.value != value_for(i) || b.value != value_for(i)) fail("transaction " + std::to_string(i) + " has wrong values");
          m = i;
        }
        if (m < acked) fail("acknowledged transaction " + std::to_string(m + 1) + " is missing (" + std::to_string(acked) + " acknowledged)");
        auto last = s.get("last");
        if ((m == 0) != !last.found || (m && last.value != std::to_string(m))) fail("\"last\" is " + (last.found ? last.value : std::string("missing")) + ", " + std::to_string(m) + " transactions present");
      } catch (const std::exception& e) { fail(std::string("open/verify: ") + e.what()); }
    }
  }
  fs::remove_all(dir);
  std::cout << "power-loss: " << ev.size() << " file operations, " << checked_points << " crash points, " << states << " disk states: all consistent (" << issued << " transactions)" << std::endl;
  return 0;
}

#pragma once
// Reader: reads a database that another process (kestro-server, or any process holding
// a Store) is writing, from inside the reading process. Any number of processes can hold
// a Reader on the same file at once; none takes the ownership lock.
//
// How it stays current: the file is append-only below its committed end, and a commit
// becomes visible by rewriting one of the two 4 KiB commit slots. Before every read the
// Reader looks at the slots' generation and end fields through a memory map (a few
// memory loads). When they moved, it decodes the slots, then checks and applies only the
// new groups (magic, transaction sequence, SHA-256 chain, frame hash), exactly as replay
// does. So a read after another process's acknowledged write sees that write.
//
// What it never trusts: bytes past the committed end (reserve, index snapshot, a commit
// in progress). A group whose bytes are not visible yet fails its hash and is retried.
// Every value is checksum-verified on read with this Reader's own random key.
//
// A Reader follows the file it opened. If the file is replaced (a restored backup, a
// compacted copy moved over it), reads fail with STALE and the caller reopens.
#include "kestro.hpp"

namespace kestro {

class Reader {
  FD fd_;
  DigestKey key_{};
  Store::Sharded index_;
  Meta meta_;
  std::unique_ptr<Mapping> head_;  // the two commit slots
  std::unique_ptr<Mapping> view_;  // [0, mapped_) of the file, replaced as the file grows
  uint64_t mapped_ = 0;
  uint64_t seen_[4] = {};  // generation and end of both slots when last fully applied
  mutable std::mutex m_;
  uint64_t refreshes_ = 0;

  static uint64_t load64(const unsigned char* p) {
    const volatile unsigned char* v = p;
    uint64_t x = 0;
    for (int i = 7; i >= 0; i--) x = (x << 8) | v[i];
    return x;
  }
  // Bytes [off, off + n) of the committed range: from the map when it covers them.
  const unsigned char* bytes(uint64_t off, uint64_t n, Bytes& tmp) {
    if (off + n > mapped_) remap(off + n);
    if (view_ && view_->address && off + n <= mapped_) return static_cast<const unsigned char*>(view_->address) + off;
    tmp = read_at(fd_.n, size_t(n), off);
    return tmp.data();
  }
  void remap(uint64_t need) {
    uint64_t size = file_size(fd_.n);
    if (size < need) return;
    auto view = std::make_unique<Mapping>(fd_.n, 0, size_t(size));
    if (view->address) { view_ = std::move(view); mapped_ = size; }
  }
  // Applies committed groups from meta_.end up to target. Returns false when a group is
  // not fully visible yet (the caller retries); meta_ then stays at the last good group.
  bool catch_up(const Meta& target) {
    while (meta_.end < target.end) {
      uint64_t pos = meta_.end;
      if (target.end - pos < 96) return false;
      Bytes th, tw;
      const unsigned char* head = bytes(pos, 64, th);
      if (!is_group_magic(head)) return false;
      uint64_t len = get64(head + 8), seq = get64(head + 16);
      uint32_t flags = get32(head + 24), count = get32(head + 28);
      if (flags || seq != meta_.txid + 1 || len > MAX_GROUP || len > target.end - pos - 96 || count > 1000000 || memcmp(head + 32, meta_.chain.data(), 32)) return false;
      const unsigned char* whole = bytes(pos, 96 + len, tw);
      auto h = digest(whole, size_t(64 + len));
      if (memcmp(h.data(), whole + 64 + len, 32)) return false;
      Store::apply_group(whole, pos, len, seq, count, index_, true, key_);
      meta_.end = pos + 96 + len; meta_.txid = seq; meta_.chain = h;
    }
    require(meta_.txid == target.txid && meta_.chain == target.chain, "CORRUPT reader boundary");
    meta_.generation = target.generation;
    return true;
  }
  void refresh() {
    Bytes tmp;
    for (int attempt = 0;; attempt++) {
      const unsigned char* h = head_ && head_->address ? static_cast<const unsigned char*>(head_->address) : nullptr;
      if (!h) { tmp = read_at(fd_.n, START, 0); h = tmp.data(); }
      uint64_t now[4] = {load64(h + 8), load64(h + 24), load64(h + 4096 + 8), load64(h + 4096 + 24)};
      if (!memcmp(now, seen_, sizeof now)) return;
      // Something moved: decode a private copy of both slots (a slot being rewritten fails
      // its checksum and is skipped; the other one is still a valid commit).
      std::atomic_thread_fence(std::memory_order_acquire);
      Bytes copy(h, h + START);
      Meta slot[2]; bool valid[2] = {false, false}, torn = false;
      for (int i = 0; i < 2; i++) {
        try { valid[i] = decode_meta(copy.data() + 4096 * i, slot[i]); } catch (...) { torn = true; }
        if (valid[i] && slot[i].uuid != meta_.uuid) throw std::runtime_error("STALE database file was replaced; reopen it");
      }
      int best = valid[0] && (!valid[1] || slot[0].generation > slot[1].generation) ? 0 : valid[1] ? 1 : -1;
      bool done = true;
      if (best >= 0 && slot[best].generation > meta_.generation) {
        if (slot[best].end < meta_.end) throw std::runtime_error("STALE database file was rolled back; reopen it");
        done = catch_up(slot[best]);
      } else if (best >= 0 && slot[best].generation < meta_.generation && !torn) {
        throw std::runtime_error("STALE database file was rolled back; reopen it");
      }
      refreshes_++;
      if (done && !torn) { memcpy(seen_, now, sizeof now); return; }
      if (done && best >= 0 && slot[best].generation >= meta_.generation) return;  // current; recheck the torn slot next time
      require(attempt < 100000, "CORRUPT committed groups are not readable");
      std::this_thread::yield();
    }
  }

 public:
  explicit Reader(const std::string& path) {
    fd_.n = platform::open_file(path, false, false);
    platform::random_bytes(&key_, sizeof key_);
    for (int attempt = 0;; attempt++) {
      try {
        meta_ = read_meta(fd_.n);
        Store::replay(fd_.n, meta_, index_, key_);
        break;
      } catch (const std::exception& e) {
        // The writer may be rewriting a commit slot right now; anything else is real.
        if (attempt >= 1000 || strncmp(e.what(), "CORRUPT", 7)) throw;
        for (auto& s : index_) s.clear();
        std::this_thread::yield();
      }
    }
    head_ = std::make_unique<Mapping>(fd_.n, 0, size_t(START));
    remap(meta_.end);
  }
  Reader(const Reader&) = delete;
  Reader& operator=(const Reader&) = delete;

  // Point read of the latest committed value. Returns false when the key is absent.
  bool read(std::string_view key, std::string& out, uint64_t& version) {
    std::lock_guard<std::mutex> lock(m_);
    refresh();
    auto h = index_hash(key_, key);
    Ref fr;
    if (!index_[Store::shard_of(h)].find(key, h, fr)) return false;
    const Ref* f = &fr;
    Bytes tmp;
    const unsigned char* p = bytes(f->offset, f->length, tmp);
    out.assign(reinterpret_cast<const char*>(p), f->length);
    require(value_matches(key_,out.data(),out.size(),f->hash), "CORRUPT value on disk");
    version = f->version;
    return true;
  }
  // Several keys from one committed state (no write lands between them).
  std::vector<Value> getMany(const std::vector<std::string>& keys) {
    require(keys.size() <= 4096, "LIMIT getMany key count");
    std::lock_guard<std::mutex> lock(m_);
    refresh();
    std::vector<Value> out(keys.size());
    Bytes tmp;
    for (size_t i = 0; i < keys.size(); i++) {
      auto h = index_hash(key_, keys[i]);
      Ref fr;
      if (!index_[Store::shard_of(h)].find(keys[i], h, fr)) continue;
      const Ref* f = &fr;
      const unsigned char* p = bytes(f->offset, f->length, tmp);
      out[i].value.assign(reinterpret_cast<const char*>(p), f->length);
      require(value_matches(key_,out[i].value.data(),out[i].value.size(),f->hash), "CORRUPT value on disk");
      out[i].found = true; out[i].version = f->version;
    }
    return out;
  }
  size_t size() { std::lock_guard<std::mutex> lock(m_); refresh(); return Store::total(index_); }
  uint64_t txid() { std::lock_guard<std::mutex> lock(m_); refresh(); return meta_.txid; }
  uint64_t refreshes() const { std::lock_guard<std::mutex> lock(m_); return refreshes_; }
};

}  // namespace kestro

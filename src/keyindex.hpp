#pragma once
// In-memory key index: open addressing with linear probing over 16-byte slots
// (hash + entry pointer), backward-shift deletion (no tombstones), growth at 75%
// load. A lookup is usually one slot access plus one entry access, compared with
// several dependent pointer loads in a node-based std::unordered_map. Entries never
// move, so a pointer returned by find stays valid until that key is erased.
#include <cstdint>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#include <cstring>
#include <memory>
#include <stdexcept>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
namespace kestro {
inline uint64_t mix64(uint64_t x) { x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL; x ^= x >> 33; return x; }
// Seeded 64-bit key hash, used only in memory. The seed is a per-process secret.
inline uint64_t key_hash(const char* p, size_t n, uint64_t seed) {
  uint64_t h = seed ^ (uint64_t(n) * 0x9e3779b97f4a7c15ULL);
  for (; n >= 8; p += 8, n -= 8) { uint64_t w; memcpy(&w, p, 8); h = mix64(h ^ w); }
  uint64_t w = 0; memcpy(&w, p, n);
  return mix64(h ^ w ^ (uint64_t(n) << 59));
}
template <class V> class KeyIndex {
public:
  struct Entry { std::string key; V value; uint64_t hash; };
private:
  struct Slot { uint64_t hash = 0; Entry* e = nullptr; };
  std::vector<Slot> slots_;
  size_t size_ = 0, mask_ = 0;
  // Entries come from blocks of 256 with a free list: one allocation per 256 keys
  // instead of one per key. Entries still never move.
  union Cell { Entry e; Cell* next; Cell() {} ~Cell() {} };
  std::vector<std::unique_ptr<Cell[]>> blocks_;
  Cell* free_ = nullptr;
  size_t used_ = 256;  // cells taken in the newest block
  Entry* make(std::string_view key, uint64_t h) {
    Cell* c;
    if (free_) { c = free_; free_ = free_->next; }
    else { if (used_ == 256) { blocks_.emplace_back(new Cell[256]); used_ = 0; } c = &blocks_.back()[used_++]; }
    return new (&c->e) Entry{std::string(key), V{}, h};
  }
  void destroy(Entry* e) { e->~Entry(); auto* c = reinterpret_cast<Cell*>(e); c->next = free_; free_ = c; }
  static constexpr size_t npos = size_t(-1);
  void place(Slot s) { size_t i = s.hash & mask_; while (slots_[i].e) i = (i + 1) & mask_; slots_[i] = s; }
  void rehash(size_t capacity) {
    std::vector<Slot> old(capacity);
    old.swap(slots_); mask_ = capacity - 1;
    for (auto& s : old) if (s.e) place(s);
  }
  size_t locate(std::string_view key, uint64_t h) const {
    if (slots_.empty()) return npos;
    for (size_t i = h & mask_;; i = (i + 1) & mask_) {
      const Slot& s = slots_[i];
      if (!s.e) return npos;
      if (s.hash == h && s.e->key.size() == key.size() && !memcmp(s.e->key.data(), key.data(), key.size())) return i;
    }
  }
public:
  KeyIndex() = default;
  KeyIndex(const KeyIndex&) = delete;
  KeyIndex& operator=(const KeyIndex&) = delete;
  KeyIndex(KeyIndex&& o) noexcept { take(o); }
  KeyIndex& operator=(KeyIndex&& o) noexcept { if (this != &o) { clear(); take(o); } return *this; }
  ~KeyIndex() { clear(); }
  size_t size() const { return size_; }
  bool empty() const { return !size_; }
  void reserve(size_t n) { size_t cap = 16; while (cap * 3 < n * 4) cap *= 2; if (cap > slots_.size()) rehash(cap); }
  V* find(std::string_view key, uint64_t h) { auto i = locate(key, h); return i == npos ? nullptr : &slots_[i].e->value; }
  const V* find(std::string_view key, uint64_t h) const { auto i = locate(key, h); return i == npos ? nullptr : &slots_[i].e->value; }
  // Returns the value slot and whether the key was newly inserted (value-initialized).
  std::pair<V*, bool> try_emplace(std::string_view key, uint64_t h) {
    auto i = locate(key, h);
    if (i != npos) return {&slots_[i].e->value, false};
    if ((size_ + 1) * 4 > slots_.size() * 3) rehash(slots_.empty() ? 16 : slots_.size() * 2);
    auto* e = make(key, h);
    place({h, e}); size_++;
    return {&e->value, true};
  }
  bool insert_or_assign(std::string_view key, uint64_t h, const V& v) { auto r = try_emplace(key, h); *r.first = v; return r.second; }
  size_t erase(std::string_view key, uint64_t h) {
    auto i = locate(key, h);
    if (i == npos) return 0;
    destroy(slots_[i].e); size_--;
    // Backward shift: pull later entries of the probe run into the hole unless
    // their home slot lies cyclically within (hole, current].
    for (size_t j = (i + 1) & mask_; slots_[j].e; j = (j + 1) & mask_) {
      size_t home = slots_[j].hash & mask_;
      bool stays = i <= j ? (home > i && home <= j) : (home > i || home <= j);
      if (!stays) { slots_[i] = slots_[j]; i = j; }
    }
    slots_[i] = Slot{};
    return 1;
  }
  template <class F> void for_each(F&& f) const { for (const auto& s : slots_) if (s.e) f(std::string_view(s.e->key), s.e->value, s.hash); }
  template <class F> void for_each_mut(F&& f) { for (auto& s : slots_) if (s.e) f(s.e->value); }
  void clear() { for (auto& s : slots_) if (s.e) s.e->~Entry(); slots_.clear(); blocks_.clear(); free_ = nullptr; used_ = 256; size_ = mask_ = 0; }
private:
  void take(KeyIndex& o) {
    slots_ = std::move(o.slots_); blocks_ = std::move(o.blocks_); free_ = o.free_; used_ = o.used_; size_ = o.size_; mask_ = o.mask_;
    o.slots_.clear(); o.blocks_.clear(); o.free_ = nullptr; o.used_ = 256; o.size_ = o.mask_ = 0;
  }
public:
};

// CompactIndex: the store's key index, packed to save RAM (about 60 bytes per key instead of
// ~130). Same open addressing and backward-shift deletion as KeyIndex, but:
//  - slots are 8 bytes: the low 32 bits of the key hash and an entry number;
//  - entries are 48 bytes in blocks of 4096 (no per-entry allocation, no pointers): file
//    offset and key length, version and value length, the first 8 bytes of the read
//    checksum, and the key itself when it is at most 24 bytes (longer keys live in a
//    chunked arena that is compacted when half of it is dead);
//  - values are copied in and out (R: offset, version, length, hash; mapped is not kept).
// Limits enforced on insert: offset < 2^53, key length <= 2047, value length < 2^25,
// version < 2^39.
template <class R> class CompactIndex {
  struct Slot { uint32_t h = 0, e = 0; };  // e: entry number + 1, 0 = empty
  struct Entry {
    uint64_t off_klen;  // offset << 11 | key length
    uint64_t ver_len;   // version << 25 | value length
    uint64_t digest;    // first 8 bytes of the read checksum
    union { char inl[24]; struct { uint32_t arena, next_free; } ext; } k;
  };
  static_assert(sizeof(Entry) == 48, "packed entry");
  // Entry blocks grow 256, 512, ... 8192, then 16384 entries (768 KB: large enough to come
  // from the OS rather than the shared heap, where they would pin freed memory). A small
  // database therefore starts at ~12 KB per shard, a large one wastes nothing.
  static constexpr uint32_t BLOCK = 16384, FIRST = 256, SMALL_TOTAL = 256 * 63, INLINE = 24, ARENA_BLOCK = 1u << 20;
  static uint32_t block_size(size_t k) { return k < 6 ? FIRST << k : BLOCK; }
  static uint32_t block_start(size_t k) { return k <= 6 ? FIRST * ((1u << k) - 1) : SMALL_TOTAL + uint32_t(k - 6) * BLOCK; }
  static void locate_entry(uint32_t n, size_t& k, uint32_t& pos) {
    if (n >= SMALL_TOTAL) { k = 6 + (n - SMALL_TOTAL) / BLOCK; pos = (n - SMALL_TOTAL) % BLOCK; return; }
    uint32_t q = n / FIRST + 1;  // 1..63: block k holds q in [2^k, 2^(k+1))
#if defined(_MSC_VER)
    unsigned long b; _BitScanReverse(&b, q); k = b;
#else
    k = size_t(31 - __builtin_clz(q));
#endif
    pos = n - FIRST * ((1u << k) - 1);
  }
  // Key arena blocks hold 16 KB, 64 KB, 256 KB, then 1 MB; addresses keep a 1 MB stride.
  static uint32_t arena_cap(size_t k) { return k < 3 ? (16u << 10) << (2 * k) : ARENA_BLOCK; }
  std::vector<Slot> slots_;
  std::vector<std::unique_ptr<Entry[]>> blocks_;
  uint32_t entries_ = 0, free_ = 0;  // entries handed out; free list head (number + 1)
  std::vector<std::unique_ptr<char[]>> arena_;
  uint32_t arena_pos_ = ARENA_BLOCK;  // position in the newest arena block
  uint64_t arena_live_ = 0, arena_dead_ = 0;
  size_t size_ = 0, mask_ = 0;
  static constexpr size_t npos = size_t(-1);

  Entry& entry(uint32_t n) { size_t k; uint32_t p; locate_entry(n, k, p); return blocks_[k][p]; }
  const Entry& entry(uint32_t n) const { size_t k; uint32_t p; locate_entry(n, k, p); return blocks_[k][p]; }
  static size_t klen(const Entry& e) { return size_t(e.off_klen & 2047); }
  const char* arena_ptr(uint32_t a) const { return arena_[a >> 20].get() + (a & (ARENA_BLOCK - 1)); }
  std::string_view key_of(const Entry& e) const { size_t n = klen(e); return {n <= INLINE ? e.k.inl : arena_ptr(e.k.ext.arena), n}; }
  uint32_t arena_put(std::string_view key) {
    if (arena_.empty() || arena_pos_ + key.size() > arena_cap(arena_.size() - 1)) {
      if (arena_.size() >= 4096) throw std::runtime_error("LIMIT key arena");
      arena_.emplace_back(new char[arena_cap(arena_.size())]); arena_pos_ = 0;
    }
    uint32_t a = uint32_t((arena_.size() - 1) << 20 | arena_pos_);
    memcpy(arena_.back().get() + arena_pos_, key.data(), key.size());
    arena_pos_ += uint32_t(key.size()); arena_live_ += key.size();
    return a;
  }
  // Rewrites long keys into a fresh arena when most of it is dead (erased keys).
  void maybe_compact_arena() {
    if (arena_dead_ < (1u << 20) || arena_dead_ < arena_live_) return;
    std::vector<std::unique_ptr<char[]>> old; old.swap(arena_);
    auto old_ptr = [&](uint32_t a) { return old[a >> 20].get() + (a & (ARENA_BLOCK - 1)); };
    arena_pos_ = ARENA_BLOCK; arena_live_ = arena_dead_ = 0;
    for (auto& s : slots_) if (s.e) { Entry& e = entry(s.e - 1); size_t n = klen(e); if (n > INLINE) e.k.ext.arena = arena_put({old_ptr(e.k.ext.arena), n}); }
  }
  static void pack(Entry& e, const R& r, size_t key_len) {
    if (r.offset >= (uint64_t(1) << 53) || r.length >= (1u << 25) || r.version >= (uint64_t(1) << 39)) throw std::runtime_error("LIMIT index entry out of range");
    e.off_klen = r.offset << 11 | uint64_t(key_len);
    e.ver_len = r.version << 25 | uint64_t(r.length);
    memcpy(&e.digest, r.hash.data(), 8);
  }
  static void unpack(const Entry& e, R& r) {
    r = R{};
    r.offset = e.off_klen >> 11; r.version = e.ver_len >> 25; r.length = uint32_t(e.ver_len & ((1u << 25) - 1));
    memcpy(r.hash.data(), &e.digest, 8);
  }
  uint32_t make(std::string_view key) {
    uint32_t n;
    if (free_) { n = free_ - 1; free_ = entry(n).k.ext.next_free; }
    else {
      if (entries_ == UINT32_MAX - 1) throw std::runtime_error("LIMIT index entries");
      if (entries_ == block_start(blocks_.size())) blocks_.emplace_back(new Entry[block_size(blocks_.size())]);
      n = entries_++;
    }
    Entry& e = entry(n);
    if (key.size() > 2047) throw std::runtime_error("LIMIT key");
    if (key.size() <= INLINE) memcpy(e.k.inl, key.data(), key.size());
    else e.k.ext.arena = arena_put(key);
    e.off_klen = key.size();
    return n;
  }
  void release(uint32_t n) {
    Entry& e = entry(n); size_t len = klen(e);
    if (len > INLINE) { arena_dead_ += len; arena_live_ -= len; }
    e.k.ext.next_free = free_; free_ = n + 1;
  }
  void place(Slot s) { size_t i = s.h & mask_; while (slots_[i].e) i = (i + 1) & mask_; slots_[i] = s; }
  void rehash(size_t capacity) { std::vector<Slot> old(capacity); old.swap(slots_); mask_ = capacity - 1; for (auto& s : old) if (s.e) place(s); }
  size_t locate(std::string_view key, uint64_t h) const {
    if (slots_.empty()) return npos;
    uint32_t hl = uint32_t(h);
    for (size_t i = hl & mask_;; i = (i + 1) & mask_) {
      const Slot& s = slots_[i];
      if (!s.e) return npos;
      if (s.h == hl) { const Entry& e = entry(s.e - 1); if (klen(e) == key.size() && !memcmp(key_of(e).data(), key.data(), key.size())) return i; }
    }
  }
  void take(CompactIndex& o) {
    slots_ = std::move(o.slots_); blocks_ = std::move(o.blocks_); arena_ = std::move(o.arena_);
    entries_ = o.entries_; free_ = o.free_; arena_pos_ = o.arena_pos_; arena_live_ = o.arena_live_; arena_dead_ = o.arena_dead_; size_ = o.size_; mask_ = o.mask_;
    o.slots_.clear(); o.blocks_.clear(); o.arena_.clear(); o.entries_ = o.free_ = 0; o.arena_pos_ = ARENA_BLOCK; o.arena_live_ = o.arena_dead_ = 0; o.size_ = o.mask_ = 0;
  }
public:
  CompactIndex() = default;
  CompactIndex(const CompactIndex&) = delete;
  CompactIndex& operator=(const CompactIndex&) = delete;
  CompactIndex(CompactIndex&& o) noexcept { take(o); }
  CompactIndex& operator=(CompactIndex&& o) noexcept { if (this != &o) { clear(); take(o); } return *this; }
  size_t size() const { return size_; }
  bool empty() const { return !size_; }
  void reserve(size_t n) { size_t cap = 16; while (cap * 3 < n * 4) cap *= 2; if (cap > slots_.size()) rehash(cap); }
  // Bytes held (slots, entry blocks, key arena), for memory reporting.
  size_t memory() const {
    size_t n = slots_.capacity() * sizeof(Slot);
    for (size_t k = 0; k < blocks_.size(); k++) n += size_t(block_size(k)) * sizeof(Entry);
    for (size_t k = 0; k < arena_.size(); k++) n += arena_cap(k);
    return n;
  }
  // The same, in O(1) (no walk over blocks): checked after every commit.
  size_t approx_memory() const { return slots_.capacity() * sizeof(Slot) + size_t(entries_) * sizeof(Entry) + size_t(arena_live_ + arena_dead_); }
  bool find(std::string_view key, uint64_t h, R& out) const { auto i = locate(key, h); if (i == npos) return false; unpack(entry(slots_[i].e - 1), out); return true; }
  // Inserts or replaces; returns true when the key is new. old receives the previous value.
  bool put(std::string_view key, uint64_t h, const R& r, R* old = nullptr) {
    auto i = locate(key, h);
    if (i != npos) { Entry& e = entry(slots_[i].e - 1); if (old) unpack(e, *old); pack(e, r, klen(e)); return false; }
    if ((size_ + 1) * 4 > slots_.size() * 3) rehash(slots_.empty() ? 16 : slots_.size() * 2);
    uint32_t n = make(key);
    pack(entry(n), r, key.size());
    place({uint32_t(h), n + 1}); size_++;
    return true;
  }
  size_t erase(std::string_view key, uint64_t h) {
    auto i = locate(key, h);
    if (i == npos) return 0;
    release(slots_[i].e - 1); size_--;
    for (size_t j = (i + 1) & mask_; slots_[j].e; j = (j + 1) & mask_) {
      size_t home = slots_[j].h & mask_;
      bool stays = i <= j ? (home > i && home <= j) : (home > i || home <= j);
      if (!stays) { slots_[i] = slots_[j]; i = j; }
    }
    slots_[i] = Slot{};
    maybe_compact_arena();
    return 1;
  }
  // f(key, value). The key view is valid until the index changes.
  template <class F> void for_each(F&& f) const { R r; for (const auto& s : slots_) if (s.e) { const Entry& e = entry(s.e - 1); unpack(e, r); f(key_of(e), static_cast<const R&>(r)); } }
  // f(key, value&): changes to the value are stored back.
  template <class F> void update_each(F&& f) { R r; for (auto& s : slots_) if (s.e) { Entry& e = entry(s.e - 1); unpack(e, r); f(key_of(e), r); pack(e, r, klen(e)); } }
  void clear() { std::vector<Slot>().swap(slots_); blocks_.clear(); arena_.clear(); entries_ = free_ = 0; arena_pos_ = ARENA_BLOCK; arena_live_ = arena_dead_ = 0; size_ = mask_ = 0; }
};
}

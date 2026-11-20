#pragma once
// SortedSet: a set of byte strings kept in order, for range and prefix scans.
// Strings live packed in pages of at most MAX entries (one buffer plus offsets), so an
// entry costs its length plus 4 bytes, and inserts move at most one page's bytes.
//
// Paging: with a PageFile attached, pages not changed recently can be moved to a scratch
// file (spill_page) that is read through memory-mapped windows, so the OS keeps the pages
// in use in RAM and the rest on disk. A page that changes is copied back into RAM. RAM then
// holds the page directory (the last entry of each page) and recently changed pages.
// The scratch file holds nothing of record: sets are rebuilt from the key index after a
// restart. Pages read from the file are valid until the next spill or release_retired(),
// which callers run only while no reader can be looking at a set.
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace kestro {

// Scratch file for SortedSet pages: slots of 512 B to 16 MB (powers of two), reused through
// free lists, read through 64 MB windows mapped with 16 MB overlap (a slot never needs two).
// Created on first use, never flushed, removed when closed.
class PageFile {
 public:
  static constexpr uint64_t W = uint64_t(64) << 20, OVER = uint64_t(16) << 20;
  explicit PageFile(std::string path) : path_(std::move(path)) {}
  ~PageFile() {
    maps_.clear(); retired_.clear();
    if (platform::valid(fd_.n)) { platform::close(fd_.n); fd_.n = platform::invalid_file(); platform::remove_noexcept(path_); }
  }
  PageFile(const PageFile&) = delete; PageFile& operator=(const PageFile&) = delete;
  static size_t cls(size_t n) { size_t c = 0; while ((size_t(512) << c) < n) c++; return c; }
  uint64_t alloc(size_t n, uint32_t& cap) {
    if (n > OVER) throw std::runtime_error("LIMIT sorted set page over 16 MB");
    std::lock_guard<std::mutex> g(m_);
    size_t c = cls(n); cap = uint32_t(size_t(512) << c);
    if (c < free_.size() && !free_[c].empty()) { auto o = free_[c].back(); free_[c].pop_back(); return o; }
    if (!platform::valid(fd_.n)) { platform::remove_noexcept(path_); fd_.n = platform::open_file(path_, true, false); }
    if (end_ / W >= 4096) throw std::runtime_error("LIMIT sorted set file");
    uint64_t o = end_; end_ += cap; return o;
  }
  void free_slot(uint64_t o, uint32_t cap) {
    std::lock_guard<std::mutex> g(m_);
    size_t c = cls(cap); if (free_.size() <= c) free_.resize(c + 1); free_[c].push_back(o);
  }
  // Writes are combined while they land back to back (new slots at the end of the file):
  // one large write instead of one per page. publish() flushes them.
  void write(uint64_t o, const void* p, size_t n, size_t cap) {
    std::lock_guard<std::mutex> g(m_);
    if (pending_.empty() || o != pending_at_ + pending_.size()) { flush_locked(); pending_at_ = o; }
    size_t at = pending_.size(); pending_.resize(at + std::max(n, cap)); memcpy(&pending_[at], p, n);
    if (pending_.size() >= (size_t(8) << 20)) flush_locked();
  }
  // Maps every window up to what was written. Replaced views are kept until release_retired().
  void publish() {
    std::lock_guard<std::mutex> g(m_);
    flush_locked();
    for (uint64_t b = 0; b < written_; b += W) {
      size_t c = size_t(b / W);
      uint64_t need = std::min<uint64_t>(W + OVER, written_ - b);
      const Mapping* v = views_[c].load(std::memory_order_acquire);
      if (v && v->length >= need) continue;
      uint64_t len = platform::mapping_beyond_eof ? W + OVER : need;
      auto m = std::make_unique<Mapping>(fd_.n, b, size_t(len));
      if (!m->address) throw std::runtime_error("IO cannot map the sorted set file");
      views_[c].store(m.get(), std::memory_order_release);
      if (maps_.size() <= c) maps_.resize(c + 1);
      if (maps_[c]) retired_.push_back(std::move(maps_[c]));
      maps_[c] = std::move(m);
    }
  }
  void release_retired() { std::lock_guard<std::mutex> g(m_); retired_.clear(); }
  const unsigned char* at(uint64_t o) const { const Mapping* v = views_[size_t(o / W)].load(std::memory_order_acquire); return static_cast<const unsigned char*>(v->address) + (o % W); }
  uint64_t bytes() const { std::lock_guard<std::mutex> g(m_); return end_; }
 private:
  using Mapping = platform::Mapping;
  std::string path_; platform::FD fd_; mutable std::mutex m_;
  uint64_t end_ = 0, written_ = 0, pending_at_ = 0;
  std::string pending_;
  void flush_locked() {
    if (pending_.empty()) return;
    platform::write_exact(fd_.n, pending_.data(), pending_.size(), pending_at_);
    written_ = std::max<uint64_t>(written_, pending_at_ + pending_.size()); pending_.clear();
  }
  std::vector<std::vector<uint64_t>> free_;
  std::array<std::atomic<const Mapping*>, 4096> views_{};
  std::vector<std::unique_ptr<Mapping>> maps_, retired_;
};

// Shared by every set of a store: RAM held by in-memory pages, and a clock for recency.
struct SetMemory { std::atomic<int64_t> bytes{0}; std::atomic<uint64_t> clock{0}; };

class SortedSet {
  static constexpr size_t MAX = 64;
  struct Chunk {
    std::string data;
    std::vector<uint32_t> off{0};  // off[i]..off[i+1] is entry i
    size_t size() const { return off.size() - 1; }
    std::string_view at(size_t i) const { return {data.data() + off[i], size_t(off[i + 1] - off[i])}; }
    std::string_view back() const { return at(size() - 1); }
    int64_t bytes() const { return int64_t(data.capacity() + 4 * off.capacity() + sizeof(Chunk)); }
    void insert(size_t i, std::string_view s) {
      data.insert(off[i], s.data(), s.size());
      off.insert(off.begin() + std::ptrdiff_t(i) + 1, off[i]);
      for (size_t j = i + 1; j < off.size(); j++) off[j] += uint32_t(s.size());
    }
    void erase(size_t i) {
      uint32_t n = off[i + 1] - off[i];
      data.erase(off[i], n);
      off.erase(off.begin() + std::ptrdiff_t(i) + 1);
      for (size_t j = i + 1; j < off.size(); j++) off[j] -= n;
    }
  };
  struct Page {
    std::unique_ptr<Chunk> mem;          // in RAM; null: in the file
    std::string last;                    // last entry while in the file
    uint64_t off = 0, touched = 0;       // file slot (cap 0: none)
    uint32_t cap = 0, n = 0;             // slot size; entries while in the file
  };
  struct View {
    const uint32_t* off; const char* data; size_t n;
    size_t size() const { return n; }
    std::string_view at(size_t i) const { return {data + off[i], size_t(off[i + 1] - off[i])}; }
    size_t lower(std::string_view s) const { size_t lo = 0, hi = n; while (lo < hi) { size_t m = (lo + hi) / 2; if (at(m) < s) lo = m + 1; else hi = m; } return lo; }
  };
  std::vector<Page> pages_;
  size_t count_ = 0;
  PageFile* file_ = nullptr;
  SetMemory* acct_ = nullptr;

  // RAM changes and the recency tick are batched per call: the shared counters are touched
  // once per insert/erase/insert_sorted, not once per page (sets change in parallel).
  int64_t delta_ = 0; uint64_t tick_ = 0;
  void account(int64_t d) { delta_ += d; }
  void begin() { if (acct_) tick_ = acct_->clock.fetch_add(1, std::memory_order_relaxed); }
  void settle() { if (acct_ && delta_) acct_->bytes.fetch_add(delta_, std::memory_order_relaxed); delta_ = 0; }
  struct Batch { SortedSet& s; explicit Batch(SortedSet& x) : s(x) { s.begin(); } ~Batch() { s.settle(); } };
  View view(size_t c) const {
    const Page& p = pages_[c];
    if (p.mem) return {p.mem->off.data(), p.mem->data.data(), p.mem->size()};
    const unsigned char* b = file_->at(p.off);
    return {reinterpret_cast<const uint32_t*>(b), reinterpret_cast<const char*>(b + 4 * (size_t(p.n) + 1)), p.n};
  }
  std::string_view last(size_t c) const { const Page& p = pages_[c]; return p.mem ? p.mem->back() : std::string_view(p.last); }
  size_t page_size(size_t c) const { const Page& p = pages_[c]; return p.mem ? p.mem->size() : p.n; }
  // The page in RAM, ready to change (copied from the file if needed).
  Chunk& load(size_t c) {
    Page& p = pages_[c];
    p.touched = tick_;
    if (p.mem) return *p.mem;
    View v = view(c);
    auto ch = std::make_unique<Chunk>();
    ch->data.assign(v.data, v.off[v.n]); ch->off.assign(v.off, v.off + v.n + 1);
    account(ch->bytes());
    p.mem = std::move(ch); std::string().swap(p.last);
    return *p.mem;
  }
  Page fresh() { Page p; p.touched = tick_; p.mem = std::make_unique<Chunk>(); account(p.mem->bytes()); return p; }
  void drop(Page& p) {
    if (p.mem) { account(-p.mem->bytes()); p.mem.reset(); }
    if (p.cap && file_) { file_->free_slot(p.off, p.cap); p.cap = 0; }
  }
  void release() { for (auto& p : pages_) drop(p); pages_.clear(); count_ = 0; settle(); }
  // First page whose last entry is >= s (the last page when s is beyond every entry).
  size_t chunk_for(std::string_view s) const {
    size_t lo = 0, hi = pages_.size();
    while (lo < hi) { size_t m = (lo + hi) / 2; if (last(m) < s) lo = m + 1; else hi = m; }
    return lo == pages_.size() ? lo - 1 : lo;
  }
  void split(size_t c) {
    Chunk& ch = *pages_[c].mem;
    Page right = fresh(); int64_t before = ch.bytes() + right.mem->bytes();
    size_t half = ch.size() / 2;
    for (size_t j = half; j < ch.size(); j++) right.mem->insert(right.mem->size(), ch.at(j));
    ch.data.resize(ch.off[half]); ch.off.resize(half + 1); ch.data.shrink_to_fit();
    account(ch.bytes() + right.mem->bytes() - before);
    pages_.insert(pages_.begin() + std::ptrdiff_t(c) + 1, std::move(right));
  }

 public:
  struct Pos { size_t c = 0, i = 0; };
  SortedSet() = default;
  SortedSet(const SortedSet&) = delete; SortedSet& operator=(const SortedSet&) = delete;
  SortedSet(SortedSet&& o) noexcept : pages_(std::move(o.pages_)), count_(o.count_), file_(o.file_), acct_(o.acct_) { o.pages_.clear(); o.count_ = 0; }
  SortedSet& operator=(SortedSet&& o) noexcept {
    if (this != &o) { release(); pages_ = std::move(o.pages_); count_ = o.count_; file_ = o.file_; acct_ = o.acct_; o.pages_.clear(); o.count_ = 0; }
    return *this;
  }
  ~SortedSet() { release(); }
  // Paging and memory accounting (both optional). Attach before the set holds entries.
  void attach(PageFile* f, SetMemory* m) { file_ = f; acct_ = m; }
  size_t size() const { return count_; }
  bool empty() const { return !count_; }
  void clear() { release(); }
  size_t memory() const { size_t n = 0; for (const auto& p : pages_) n += sizeof(Page) + p.last.capacity() + (p.mem ? size_t(p.mem->bytes()) : 0); return n; }
  bool insert(std::string_view s) {
    Batch b_(*this);
    if (pages_.empty()) { pages_.push_back(fresh()); auto& ch = *pages_[0].mem; int64_t b = ch.bytes(); ch.insert(0, s); account(ch.bytes() - b); count_ = 1; return true; }
    size_t c = chunk_for(s);
    if (!pages_[c].mem) { View v = view(c); size_t i = v.lower(s); if (i < v.size() && v.at(i) == s) return false; }
    Chunk& ch = load(c);
    size_t i = View{ch.off.data(), ch.data.data(), ch.size()}.lower(s);
    if (i < ch.size() && ch.at(i) == s) return false;
    int64_t b = ch.bytes(); ch.insert(i, s); account(ch.bytes() - b);
    count_++;
    if (ch.size() > MAX) split(c);
    return true;
  }
  // Inserts strings given in ascending order in one forward pass (no search from the top
  // for each). Strings already present are skipped. Returns how many were added.
  template <class It> size_t insert_sorted(It first, It last_) {
    Batch b_(*this);
    size_t added = 0, c = 0;
    for (; first != last_; ++first) {
      std::string_view s = *first;
      if (pages_.empty()) { insert(s); added++; continue; }
      if (c >= pages_.size()) c = pages_.size() - 1;
      // Binary search from the current page on (the strings come in ascending order), not a
      // walk from the first page: a commit costs O(log pages), whatever the table size.
      if (last(c) < s) { size_t lo = c + 1, hi = pages_.size(); while (lo < hi) { size_t m = (lo + hi) / 2; if (last(m) < s) lo = m + 1; else hi = m; } c = lo == pages_.size() ? lo - 1 : lo; }
      if (!pages_[c].mem) { View v = view(c); size_t i = v.lower(s); if (i < v.size() && v.at(i) == s) continue; }
      Chunk& ch = load(c);
      size_t i = View{ch.off.data(), ch.data.data(), ch.size()}.lower(s);
      if (i < ch.size() && ch.at(i) == s) continue;
      int64_t b = ch.bytes(); ch.insert(i, s); account(ch.bytes() - b); count_++; added++;
      if (ch.size() > MAX) split(c);
    }
    return added;
  }
  bool erase(std::string_view s) {
    Batch b_(*this);
    if (pages_.empty()) return false;
    size_t c = chunk_for(s);
    if (!pages_[c].mem) { View v = view(c); size_t i = v.lower(s); if (i >= v.size() || v.at(i) != s) return false; }
    Chunk& ch = load(c);
    size_t i = View{ch.off.data(), ch.data.data(), ch.size()}.lower(s);
    if (i >= ch.size() || ch.at(i) != s) return false;
    int64_t b = ch.bytes(); ch.erase(i); account(ch.bytes() - b);
    count_--;
    if (!ch.size()) { drop(pages_[c]); pages_.erase(pages_.begin() + std::ptrdiff_t(c)); }
    return true;
  }
  bool contains(std::string_view s) const {
    if (pages_.empty()) return false;
    View v = view(chunk_for(s));
    size_t i = v.lower(s);
    return i < v.size() && v.at(i) == s;
  }
  // Builds from strings already sorted and unique (bulk load), in RAM.
  template <class It> void assign_sorted(It first, It last_) {
    clear();
    for (; first != last_; ++first) build_add(*first, SIZE_MAX);
  }
  // Bulk load one string at a time, in ascending order. Pages go straight to the file
  // once the store's set memory passes keep_in_ram (needs a file); finish with build_end.
  void build_add(std::string_view s, size_t keep_in_ram) {
    Batch b_(*this);
    if (pages_.empty() || page_size(pages_.size() - 1) >= MAX * 3 / 4) {
      if (!pages_.empty() && file_ && acct_ && size_t(std::max<int64_t>(0, acct_->bytes.load())) > keep_in_ram) spill_page(pages_.size() - 1, false);
      pages_.push_back(fresh());
    }
    Chunk& ch = *pages_.back().mem; int64_t b = ch.bytes(); ch.insert(ch.size(), s); account(ch.bytes() - b);
    count_++;
  }
  void build_end() { if (file_) file_->publish(); }
  // Pages in RAM, for choosing what to spill: f(last change, page number).
  template <class F> void each_ram_page(F&& f) const { for (size_t c = 0; c < pages_.size(); c++) if (pages_[c].mem) f(pages_[c].touched, c); }
  // Writes page c to the file and frees its RAM (its old slot is reused when it fits).
  // The caller publishes the file afterwards (publish defaults to doing it here).
  void spill_page(size_t c, bool publish = true) {
    Page& p = pages_[c]; if (!p.mem || !file_) return;
    Batch b_(*this);
    const Chunk& ch = *p.mem; size_t n = ch.size(), bytes = 4 * (n + 1) + ch.data.size();
    if (bytes > p.cap) { if (p.cap) file_->free_slot(p.off, p.cap); p.off = file_->alloc(bytes, p.cap); }
    thread_local std::string buf; buf.resize(bytes);
    memcpy(&buf[0], ch.off.data(), 4 * (n + 1)); memcpy(&buf[4 * (n + 1)], ch.data.data(), ch.data.size());
    file_->write(p.off, buf.data(), bytes, p.cap);
    p.last.assign(ch.back()); p.n = uint32_t(n);
    account(-ch.bytes()); p.mem.reset();
    if (publish) file_->publish();
  }
  // Positions: end() is {pages, 0}.
  Pos end() const { return {pages_.size(), 0}; }
  bool valid(Pos p) const { return p.c < pages_.size(); }
  std::string_view get(Pos p) const { return view(p.c).at(p.i); }
  Pos seek(std::string_view s) const {  // first entry >= s
    if (pages_.empty()) return end();
    size_t c = chunk_for(s);
    View v = view(c); size_t i = v.lower(s);
    if (i == v.size()) return {c + 1, 0};
    return {c, i};
  }
  Pos next(Pos p) const { if (++p.i == page_size(p.c)) { p.c++; p.i = 0; } return p; }
  // Previous entry; returns end() before the first.
  Pos prev(Pos p) const {
    if (p.c >= pages_.size()) { if (pages_.empty()) return end(); size_t c = pages_.size() - 1; return {c, page_size(c) - 1}; }
    if (p.i > 0) return {p.c, p.i - 1};
    if (p.c == 0) return end();
    return {p.c - 1, page_size(p.c - 1) - 1};
  }
};

// Sorts any number of strings with bounded RAM: runs of up to `limit` bytes are sorted in
// RAM and, when there is more than one, written to a temporary file and merged.
class ExternalSorter {
  std::string path_; size_t limit_, used_ = 0;
  std::vector<std::unique_ptr<char[]>> blocks_; size_t block_pos_ = 0;
  std::vector<std::string_view> items_;
  platform::FD fd_; uint64_t end_ = 0;
  struct Run { uint64_t pos, end; };
  std::vector<Run> runs_;
  static constexpr size_t BLOCK = 1 << 20;
  std::string_view keep(std::string_view s) {
    if (s.size() > BLOCK) { blocks_.emplace_back(new char[s.size()]); memcpy(blocks_.back().get(), s.data(), s.size()); block_pos_ = BLOCK; return {blocks_.back().get(), s.size()}; }
    if (blocks_.empty() || block_pos_ + s.size() > BLOCK) { blocks_.emplace_back(new char[BLOCK]); block_pos_ = 0; }
    char* p = blocks_.back().get() + block_pos_; memcpy(p, s.data(), s.size()); block_pos_ += s.size(); return {p, s.size()};
  }
  void flush_run() {
    std::sort(items_.begin(), items_.end());
    if (!platform::valid(fd_.n)) { platform::remove_noexcept(path_); fd_.n = platform::open_file(path_, true, false); }
    std::string buf; Run r{end_, end_};
    auto out = [&] { platform::write_exact(fd_.n, buf.data(), buf.size(), r.end); r.end += buf.size(); buf.clear(); };
    for (auto s : items_) { uint32_t n = uint32_t(s.size()); buf.append(reinterpret_cast<const char*>(&n), 4); buf.append(s.data(), s.size()); if (buf.size() >= BLOCK) out(); }
    out(); end_ = r.end; runs_.push_back(r);
    items_.clear(); blocks_.clear(); block_pos_ = 0; used_ = 0;
  }
  struct Reader {
    platform::File f; uint64_t pos, end; std::string buf; size_t at = 0;
    bool need(size_t n) {
      if (buf.size() - at >= n) return true;
      buf.erase(0, at); at = 0;
      size_t get = size_t(std::min<uint64_t>(end - pos, std::max<size_t>(n - buf.size(), 256 << 10)));
      size_t old = buf.size(); buf.resize(old + get); if (get) platform::read_exact(f, &buf[old], get, pos); pos += get;
      return buf.size() >= n;
    }
    bool next(std::string_view& s) {
      if (!need(4)) return false;
      uint32_t n; memcpy(&n, buf.data() + at, 4);
      if (!need(4 + size_t(n))) throw std::runtime_error("CORRUPT sort run");
      s = std::string_view(buf.data() + at + 4, n); at += 4 + n; return true;
    }
  };
 public:
  ExternalSorter(std::string temp_path, size_t limit) : path_(std::move(temp_path)), limit_(std::max<size_t>(limit, 1 << 16)) {}
  ~ExternalSorter() { if (platform::valid(fd_.n)) { platform::close(fd_.n); fd_.n = platform::invalid_file(); platform::remove_noexcept(path_); } }
  void add(std::string_view s) { items_.push_back(keep(s)); used_ += s.size() + sizeof(std::string_view); if (used_ >= limit_) flush_run(); }
  // emit(string_view) for every string in ascending order (duplicates kept).
  template <class F> void finish(F&& emit) {
    if (runs_.empty()) { std::sort(items_.begin(), items_.end()); for (auto s : items_) emit(s); items_.clear(); blocks_.clear(); return; }
    if (!items_.empty()) flush_run();
    std::vector<Reader> rd; rd.reserve(runs_.size());
    for (auto& r : runs_) rd.push_back(Reader{fd_.n, r.pos, r.end, {}, 0});
    std::vector<std::string_view> cur(rd.size()); std::vector<size_t> heap;
    auto greater = [&](size_t a, size_t b) { return cur[a] > cur[b]; };
    for (size_t i = 0; i < rd.size(); i++) if (rd[i].next(cur[i])) heap.push_back(i);
    std::make_heap(heap.begin(), heap.end(), greater);
    while (!heap.empty()) {
      std::pop_heap(heap.begin(), heap.end(), greater); size_t i = heap.back(); heap.pop_back();
      emit(cur[i]);
      if (rd[i].next(cur[i])) { heap.push_back(i); std::push_heap(heap.begin(), heap.end(), greater); }
    }
  }
};

}  // namespace kestro

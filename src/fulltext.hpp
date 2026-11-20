#pragma once
// Full-text search (see SQL.md): the tokenizer shared by FULLTEXT indexes and MATCH ... AGAINST,
// query parsing (natural language and boolean mode) and BM25 relevance.
//
// Tokens: runs of letters and digits in UTF-8 text, lower-cased. ASCII, Latin-1, Latin
// Extended-A, Greek and Cyrillic letters are folded to lower case; accents are kept (é is not
// e). Separators: ASCII and Latin-1 punctuation, symbols and spaces, general punctuation
// (U+2000..U+206F), ideographic space and punctuation (U+3000..U+3003), U+FEFF and invalid
// UTF-8. Every other code point is a word character, so text without spaces (Chinese,
// Japanese, Thai) is one long token: there is no word segmentation, no stemming and no stop
// word list. Tokens shorter than the index's minimum length (characters) or longer than
// MAX_TOKEN bytes are left out, in documents and queries alike.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace kestro::fts {
constexpr size_t MAX_TOKEN = 64, MAX_ITEMS = 64;
[[noreturn]] inline void fail(const std::string& m) { throw std::runtime_error("INPUT SQL: " + m); }

// One UTF-8 code point at s[i]: its length, 0 when the bytes are not valid UTF-8.
inline size_t decode(std::string_view s, size_t i, uint32_t& cp) {
  auto b = [&](size_t k) { return uint8_t(s[i + k]); };
  uint8_t c = b(0);
  if (c < 0x80) { cp = c; return 1; }
  size_t n = c >= 0xF0 && c <= 0xF4 ? 4 : c >= 0xE0 ? (c < 0xF0 ? 3 : 0) : c >= 0xC2 ? 2 : 0;
  if (!n || i + n > s.size()) return 0;
  cp = c & (0x7F >> n);
  for (size_t k = 1; k < n; k++) { if ((b(k) & 0xC0) != 0x80) return 0; cp = (cp << 6) | (b(k) & 0x3F); }
  if ((n == 3 && cp < 0x800) || (n == 4 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
  return n;
}
inline void encode(std::string& out, uint32_t c) {
  if (c < 0x80) out += char(c);
  else if (c < 0x800) { out += char(0xC0 | (c >> 6)); out += char(0x80 | (c & 0x3F)); }
  else if (c < 0x10000) { out += char(0xE0 | (c >> 12)); out += char(0x80 | ((c >> 6) & 0x3F)); out += char(0x80 | (c & 0x3F)); }
  else { out += char(0xF0 | (c >> 18)); out += char(0x80 | ((c >> 12) & 0x3F)); out += char(0x80 | ((c >> 6) & 0x3F)); out += char(0x80 | (c & 0x3F)); }
}
inline uint32_t lower(uint32_t c) {
  if (c < 0x80) return c >= 'A' && c <= 'Z' ? c + 32 : c;
  if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;                     // Latin-1
  if (c >= 0x100 && c <= 0x17F) {                                              // Latin Extended-A
    if (c == 0x130) return 'i';
    if (c == 0x178) return 0xFF;
    if (c <= 0x137 || (c >= 0x14A && c <= 0x177)) return c | 1;                // pairs: even upper, odd lower
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return c & 1 ? c + 1 : c;  // odd upper, even lower
    return c;
  }
  if (c >= 0x386 && c <= 0x3AB) {                                             // Greek
    if (c == 0x386) return 0x3AC;
    if (c >= 0x388 && c <= 0x38A) return c + 37;
    if (c == 0x38C) return 0x3CC;
    if (c == 0x38E || c == 0x38F) return c + 63;
    if (c >= 0x391 && c != 0x3A2) return c + 32;
    return c;
  }
  if (c == 0x3C2) return 0x3C3;                                                // final sigma
  if (c >= 0x410 && c <= 0x42F) return c + 32;                                 // Cyrillic
  if (c >= 0x400 && c <= 0x40F) return c + 80;
  return c;
}
inline bool word_char(uint32_t c) {
  if (c < 0x80) return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
  if (c < 0xC0) return c == 0xAA || c == 0xB5 || c == 0xBA;
  if (c == 0xD7 || c == 0xF7 || c == 0xFEFF) return false;
  if ((c >= 0x2000 && c <= 0x206F) || (c >= 0x3000 && c <= 0x3003)) return false;
  return true;
}
// Calls cb(token) for each token of text, in order (the view is valid during the call).
template <class F> void tokens(std::string_view text, size_t min_len, F&& cb) {
  std::string tok; size_t chars = 0;
  auto flush = [&] { if (!tok.empty() && chars >= min_len && tok.size() <= MAX_TOKEN) cb(std::string_view(tok)); tok.clear(); chars = 0; };
  for (size_t i = 0; i < text.size();) {
    uint32_t c; size_t n = decode(text, i, c);
    if (!n) { flush(); i++; continue; }
    i += n;
    if (!word_char(c)) { flush(); continue; }
    if (c < 0x80) tok += char(c >= 'A' && c <= 'Z' ? c + 32 : c); else encode(tok, lower(c));
    chars++;
  }
  flush();
}

// ---------- Queries ----------
// An item: one word, a phrase (several words in a row) or a prefix (the last word ends in *).
struct Item { std::vector<std::string> words; bool prefix = false; enum Kind { Optional, Required, Excluded } kind = Optional; };
struct Query { std::vector<Item> items; bool boolean = false; };
// Natural language mode: every distinct word of the text is an optional item (quotes and
// operators are separators). Boolean mode: items separated by spaces, each optionally
// starting with + (required) or - (excluded): word, word* (prefix) or "a phrase". Words of
// one item separated by punctuation (e-mail, x-ray) form a phrase.
inline Query parse(std::string_view q, bool boolean, size_t min_len) {
  Query out; out.boolean = boolean;
  auto add = [&](Item it) {
    if (it.words.empty()) return;
    for (const auto& x : out.items) if (x.words == it.words && x.prefix == it.prefix && x.kind == it.kind) return;
    if (out.items.size() >= MAX_ITEMS) fail("a full-text query has at most " + std::to_string(MAX_ITEMS) + " words or phrases");
    out.items.push_back(std::move(it));
  };
  if (!boolean) { tokens(q, min_len, [&](std::string_view t) { Item it; it.words.emplace_back(t); add(std::move(it)); }); return out; }
  auto space = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
  for (size_t i = 0; i < q.size();) {
    if (space(q[i])) { i++; continue; }
    Item it;
    if (q[i] == '+' || q[i] == '-') { it.kind = q[i] == '+' ? Item::Required : Item::Excluded; i++; }
    std::string_view text;
    if (i < q.size() && q[i] == '"') {
      size_t j = q.find('"', i + 1);
      if (j == std::string_view::npos) fail("unterminated phrase in the full-text query");
      text = q.substr(i + 1, j - i - 1); i = j + 1;
    } else {
      size_t j = i; while (j < q.size() && !space(q[j]) && q[j] != '"') j++;
      text = q.substr(i, j - i); i = j;
      for (char c : text) if (c == '(' || c == ')' || c == '<' || c == '>' || c == '~' || c == '@')
        fail(std::string("IN BOOLEAN MODE supports +word, -word, word* and \"phrases\", not '") + c + "'");
      while (!text.empty() && text.back() == '*') { it.prefix = true; text.remove_suffix(1); }
    }
    // A prefix's last word may be shorter than the minimum length (it stands for longer words).
    std::vector<std::string> all; tokens(text, 1, [&](std::string_view t) { all.emplace_back(t); });
    for (size_t k = 0; k < all.size(); k++) {
      size_t chars = 0; for (unsigned char c : all[k]) chars += (c & 0xC0) != 0x80;
      if (chars >= min_len || (it.prefix && k + 1 == all.size())) it.words.push_back(std::move(all[k]));
    }
    add(std::move(it));
  }
  return out;
}

// ---------- Matching and relevance ----------
// Index statistics for a query: documents with tokens, their average length, and per item
// the number of documents containing it.
struct Stats { double docs = 0, avgdl = 1; std::vector<double> df; };
// What a MATCH ... AGAINST needs per statement: the query, its statistics and the rows the
// index says can match (sorted row keys, for narrowing a WHERE).
struct Prepared { Query query; Stats stats; size_t min_len = 1; std::string table; std::shared_ptr<const std::vector<std::string>> keys; };
// A document's tokens in one buffer (reused from row to row: no allocation per token). An
// empty token separates columns: no phrase crosses one.
struct Tokens {
  std::string text; std::vector<uint32_t> ends;
  void clear() { text.clear(); ends.clear(); }
  void add(std::string_view t) { text.append(t.data(), t.size()); ends.push_back(uint32_t(text.size())); }
  void separate() { if (!ends.empty()) ends.push_back(uint32_t(text.size())); }
  size_t size() const { return ends.size(); }
  std::string_view operator[](size_t i) const { uint32_t a = i ? ends[i - 1] : 0; return std::string_view(text).substr(a, ends[i] - a); }
};
inline bool word_hit(std::string_view tok, const std::string& w, bool prefix) { return prefix ? tok.compare(0, w.size(), w) == 0 : tok == w; }
inline size_t occurrences(const Item& it, const Tokens& toks) {
  size_t n = 0, k = it.words.size();
  for (size_t i = 0; i + k <= toks.size(); i++) {
    bool hit = true;
    for (size_t j = 0; j < k && hit; j++) hit = !toks[i + j].empty() && word_hit(toks[i + j], it.words[j], it.prefix && j + 1 == k);
    n += hit;
  }
  return n;
}
// BM25 (k1 = 1.2, b = 0.75) summed over the items found; 0 when the document does not match.
// A matching document always scores above 0.
inline double score(const Query& q, const Stats& st, const Tokens& toks) {
  if (q.items.empty()) return 0;
  double dl = 0; for (size_t i = 0; i < toks.size(); i++) dl += !toks[i].empty();
  double sum = 0; bool any = false;
  for (size_t i = 0; i < q.items.size(); i++) {
    const auto& it = q.items[i];
    double tf = double(occurrences(it, toks));
    if (it.kind == Item::Excluded) { if (tf > 0) return 0; continue; }
    if (it.kind == Item::Required && tf == 0) return 0;
    if (tf == 0) continue;
    any = true;
    double df = i < st.df.size() ? st.df[i] : 1, n = std::max(st.docs, df);
    double idf = std::log(1 + (n - df + 0.5) / (df + 0.5));
    sum += idf * tf * 2.2 / (tf + 1.2 * (0.25 + 0.75 * dl / std::max(st.avgdl, 1.0)));
  }
  if (!any) return 0;
  return std::max(sum, 1e-9);
}
}  // namespace kestro::fts

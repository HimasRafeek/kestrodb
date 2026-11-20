#pragma once
// Minimal JSON support for queries and indexes.
//
//  - find_path: locates a field's raw value text inside a document without allocating.
//  - Scalar / scalar_of: one JSON value reduced to what comparisons need.
//  - encode: an order-preserving byte encoding of a value, so index entries sort by value
//    and a byte range answers =, <, >, prefix. Order: missing < null < false < true <
//    numbers < strings < arrays/objects (compared as raw text).
//  - Json / parse: a small DOM for query specs and catalog records.
// Documents are untrusted: every function bounds-checks and treats malformed text as missing.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kestro::json {

inline void skip_ws(const char*& p, const char* e) { while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++; }

// Skips one string starting at '"'. Returns false if unterminated.
inline bool skip_string(const char*& p, const char* e) {
  if (p >= e || *p != '"') return false;
  // memchr to the next quote (vectorized); it ends the string unless an odd number of
  // backslashes precede it.
  const char* q = p + 1;
  for (;;) {
    q = static_cast<const char*>(memchr(q, '"', size_t(e - q)));
    if (!q) return false;
    size_t slashes = 0;
    for (const char* b = q; b > p + 1 && b[-1] == '\\'; b--) slashes++;
    if (!(slashes & 1)) { p = q + 1; return true; }
    q++;
  }
}
// Skips one value of any kind. depth guards against hostile nesting.
inline bool skip_value(const char*& p, const char* e, int depth = 0) {
  skip_ws(p, e);
  if (p >= e || depth > 256) return false;
  char c = *p;
  if (c == '"') return skip_string(p, e);
  if (c == '{' || c == '[') {
    char close = c == '{' ? '}' : ']';
    p++; skip_ws(p, e);
    if (p < e && *p == close) { p++; return true; }
    for (;;) {
      if (c == '{') { skip_ws(p, e); if (!skip_string(p, e)) return false; skip_ws(p, e); if (p >= e || *p != ':') return false; p++; }
      if (!skip_value(p, e, depth + 1)) return false;
      skip_ws(p, e);
      if (p >= e) return false;
      if (*p == ',') { p++; continue; }
      if (*p == close) { p++; return true; }
      return false;
    }
  }
  const char* s = p;
  while (p < e && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
  return p > s;
}
// Compares a raw JSON string token (with quotes) to a plain name, handling escapes.
inline bool string_equals(const char* p, const char* e, std::string_view name);
// Finds the raw text of doc.path[0].path[1]... Returns false when absent or malformed.
inline bool find_path(std::string_view doc, const std::vector<std::string>& path, std::string_view& out) {
  const char *p = doc.data(), *e = doc.data() + doc.size();
  for (const auto& name : path) {
    skip_ws(p, e);
    if (p >= e || *p != '{') return false;
    p++;
    bool found = false;
    for (;;) {
      skip_ws(p, e);
      if (p < e && *p == '}') return false;
      const char* ks = p;
      if (!skip_string(p, e)) return false;
      bool match = string_equals(ks, p, name);
      skip_ws(p, e);
      if (p >= e || *p != ':') return false;
      p++;
      skip_ws(p, e);
      if (match) { found = true; break; }
      if (!skip_value(p, e)) return false;
      skip_ws(p, e);
      if (p < e && *p == ',') { p++; continue; }
      return false;
    }
    if (!found) return false;
  }
  skip_ws(p, e);
  const char* s = p;
  if (!skip_value(p, e)) return false;
  out = std::string_view(s, size_t(p - s));
  return true;
}

inline void put_utf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) out += char(cp);
  else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
  else if (cp < 0x10000) { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
  else { out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 0x3F)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
}
inline int hex4(const char* p) {
  int v = 0;
  for (int i = 0; i < 4; i++) {
    char c = p[i]; v <<= 4;
    if (c >= '0' && c <= '9') v |= c - '0'; else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10; else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10; else return -1;
  }
  return v;
}
// Decodes a string token (with quotes) into UTF-8. Returns false when malformed.
inline bool unescape(const char* p, const char* e, std::string& out) {
  out.clear();
  if (p >= e || *p != '"') return false;
  for (p++; p < e; p++) {
    char c = *p;
    if (c == '"') return p + 1 == e;
    if (c != '\\') { out += c; continue; }
    if (++p >= e) return false;
    switch (*p) {
      case '"': out += '"'; break; case '\\': out += '\\'; break; case '/': out += '/'; break;
      case 'b': out += '\b'; break; case 'f': out += '\f'; break; case 'n': out += '\n'; break;
      case 'r': out += '\r'; break; case 't': out += '\t'; break;
      case 'u': {
        if (e - p < 5) return false;
        int cp = hex4(p + 1); if (cp < 0) return false; p += 4;
        if (cp >= 0xD800 && cp < 0xDC00 && e - p >= 7 && p[1] == '\\' && p[2] == 'u') {
          int lo = hex4(p + 3);
          if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); p += 6; }
        }
        put_utf8(out, uint32_t(cp)); break;
      }
      default: return false;
    }
  }
  return false;
}
inline bool string_equals(const char* p, const char* e, std::string_view name) {
  // Fast path: no escapes in the token.
  size_t n = size_t(e - p);
  if (n == name.size() + 2 && !memchr(p + 1, '\\', n - 2)) return !memcmp(p + 1, name.data(), name.size());
  if (!memchr(p + 1, '\\', n >= 2 ? n - 2 : 0)) return false;
  std::string s;
  return unescape(p, e, s) && s == name;
}

// ---------- Scalars ----------
enum Tag : uint8_t { MISSING = 1, NUL = 2, FALSE_ = 3, TRUE_ = 4, NUMBER = 5, STRING = 6, COMPOSITE = 7 };
struct Scalar {
  Tag tag = MISSING;
  double num = 0;
  std::string str;  // decoded string, or raw text for arrays/objects
};
// Reduces raw value text to a Scalar. Malformed text counts as missing.
inline Scalar scalar_of(std::string_view raw) {
  Scalar s;
  if (raw.empty()) return s;
  char c = raw[0];
  if (c == '"') { if (unescape(raw.data(), raw.data() + raw.size(), s.str)) s.tag = STRING; return s; }
  if (c == '{' || c == '[') { s.tag = COMPOSITE; s.str.assign(raw); return s; }
  if (raw == "null") { s.tag = NUL; return s; }
  if (raw == "true") { s.tag = TRUE_; return s; }
  if (raw == "false") { s.tag = FALSE_; return s; }
  std::string tmp(raw);
  char* end = nullptr;
  double v = strtod(tmp.c_str(), &end);
  if (end && *end == 0 && std::isfinite(v)) { s.tag = NUMBER; s.num = v == 0 ? 0.0 : v; }  // -0 == 0
  return s;
}
// Several top-level fields in one pass: out[i] is names[i] (missing when absent).
inline void top_fields(std::string_view doc, const std::vector<std::string>& names, std::vector<Scalar>& out) {
  out.assign(names.size(), Scalar{});
  const char *p = doc.data(), *e = doc.data() + doc.size();
  skip_ws(p, e);
  if (p >= e || *p != '{') return;
  p++; size_t found = 0;
  for (;;) {
    skip_ws(p, e);
    if (p >= e || *p == '}') return;
    const char* ks = p;
    if (!skip_string(p, e)) return;
    const char* ke = p;
    skip_ws(p, e);
    if (p >= e || *p != ':') return;
    p++; skip_ws(p, e);
    const char* vs = p;
    if (!skip_value(p, e)) return;
    for (size_t i = 0; i < names.size(); i++)
      if (out[i].tag == MISSING && string_equals(ks, ke, names[i])) { out[i] = scalar_of(std::string_view(vs, size_t(p - vs))); if (++found == names.size()) return; }
    skip_ws(p, e);
    if (p < e && *p == ',') { p++; continue; }
    return;
  }
}
inline Scalar field(std::string_view doc, const std::vector<std::string>& path) {
  std::string_view raw;
  if (!find_path(doc, path, raw)) return Scalar{};
  return scalar_of(raw);
}

// ---------- Order-preserving encoding ----------
// Strings: bytes with 0x00 -> 0x00 0xFF, terminated by 0x00 0x01, so a shorter string sorts
// first and the encoding is self-delimiting (an index entry is encode(value) + primary key).
inline void escape_into(std::string& out, std::string_view s) {
  for (char c : s) { out += c; if (c == 0) out += char(0xFF); }
}
inline void encode_into(std::string& out, const Scalar& s) {
  out += char(s.tag);
  if (s.tag == NUMBER) {
    uint64_t b; memcpy(&b, &s.num, 8);
    b = (b >> 63) ? ~b : (b | (uint64_t(1) << 63));
    for (int i = 7; i >= 0; i--) out += char((b >> (8 * i)) & 0xFF);
  } else if (s.tag == STRING || s.tag == COMPOSITE) {
    escape_into(out, s.str);
    out += char(0); out += char(1);
  }
}
inline std::string encode(const Scalar& s) { std::string out; encode_into(out, s); return out; }
// Smallest string greater than every string that starts with p ("" means no upper bound).
inline std::string prefix_end(std::string p) {
  while (!p.empty()) {
    unsigned char last = static_cast<unsigned char>(p.back());
    if (last < 0xFF) { p.back() = char(last + 1); return p; }
    p.pop_back();
  }
  return p;
}
// Length of the encoded value at the start of an index entry (the primary key follows).
inline size_t encoded_length(std::string_view e) {
  if (e.empty()) return 0;
  uint8_t t = uint8_t(e[0]);
  if (t == NUMBER) return e.size() >= 9 ? 9 : e.size();
  if (t == STRING || t == COMPOSITE) {
    for (size_t i = 1; i + 1 < e.size(); i++) {
      if (e[i] == 0) { if (uint8_t(e[i + 1]) == 1) return i + 2; i++; }
    }
    return e.size();
  }
  return 1;
}
// Total order matching encode(): returns <0, 0, >0.
inline int compare(const Scalar& a, const Scalar& b) {
  if (a.tag != b.tag) return a.tag < b.tag ? -1 : 1;
  if (a.tag == NUMBER) return a.num < b.num ? -1 : a.num > b.num ? 1 : 0;
  if (a.tag == STRING || a.tag == COMPOSITE) { int c = a.str.compare(b.str); return c < 0 ? -1 : c > 0 ? 1 : 0; }
  return 0;
}

// ---------- Small DOM for query specs ----------
struct Json {
  enum Kind : uint8_t { Null, False, True, Number, String, Array, Object } kind = Null;
  double num = 0;
  std::string str;  // string value; for any value, raw holds its source text
  std::string raw;
  std::vector<Json> items;
  std::vector<std::pair<std::string, Json>> fields;
  const Json* get(std::string_view k) const { for (const auto& f : fields) if (f.first == k) return &f.second; return nullptr; }
  bool is_string() const { return kind == String; }
  bool is_number() const { return kind == Number; }
  Scalar scalar() const { return scalar_of(raw); }
};
inline void parse_value(const char*& p, const char* e, Json& out, int depth) {
  skip_ws(p, e);
  if (p >= e || depth > 64) throw std::runtime_error("INPUT malformed JSON");
  const char* start = p;
  char c = *p;
  if (c == '{' || c == '[') {
    out.kind = c == '{' ? Json::Object : Json::Array;
    char close = c == '{' ? '}' : ']';
    p++; skip_ws(p, e);
    if (p < e && *p == close) { p++; }
    else for (;;) {
      std::string key;
      if (c == '{') {
        skip_ws(p, e); const char* ks = p;
        if (!skip_string(p, e) || !unescape(ks, p, key)) throw std::runtime_error("INPUT malformed JSON key");
        skip_ws(p, e); if (p >= e || *p != ':') throw std::runtime_error("INPUT malformed JSON");
        p++;
      }
      Json v; parse_value(p, e, v, depth + 1);
      if (c == '{') out.fields.emplace_back(std::move(key), std::move(v)); else out.items.push_back(std::move(v));
      skip_ws(p, e);
      if (p < e && *p == ',') { p++; continue; }
      if (p < e && *p == close) { p++; break; }
      throw std::runtime_error("INPUT malformed JSON");
    }
  } else {
    if (!skip_value(p, e)) throw std::runtime_error("INPUT malformed JSON");
    std::string_view raw(start, size_t(p - start));
    Scalar s = scalar_of(raw);
    switch (s.tag) {
      case STRING: out.kind = Json::String; out.str = std::move(s.str); break;
      case NUMBER: out.kind = Json::Number; out.num = s.num; break;
      case TRUE_: out.kind = Json::True; break;
      case FALSE_: out.kind = Json::False; break;
      case NUL: out.kind = Json::Null; break;
      default: throw std::runtime_error("INPUT malformed JSON value");
    }
  }
  out.raw.assign(start, size_t(p - start));
}
inline Json parse(std::string_view text) {
  const char *p = text.data(), *e = text.data() + text.size();
  Json j; parse_value(p, e, j, 0);
  skip_ws(p, e);
  if (p != e) throw std::runtime_error("INPUT trailing bytes after JSON");
  return j;
}
// A JSON array of parameters straight to scalars (objects/arrays keep their checked text).
inline std::vector<Scalar> parse_scalars(std::string_view text) {
  const char *p = text.data(), *e = text.data() + text.size();
  std::vector<Scalar> out;
  skip_ws(p, e);
  if (p >= e || *p != '[') throw std::runtime_error("INPUT parameters must be a JSON array");
  p++; skip_ws(p, e);
  if (p < e && *p == ']') p++;
  else for (;;) {
    skip_ws(p, e);
    const char* s = p;
    if (!skip_value(p, e)) throw std::runtime_error("INPUT malformed parameter JSON");
    std::string_view raw(s, size_t(p - s));
    Scalar v = scalar_of(raw);
    if (v.tag == MISSING) throw std::runtime_error("INPUT malformed parameter JSON");
    if (v.tag == COMPOSITE) { Json check; const char* q = raw.data(); parse_value(q, raw.data() + raw.size(), check, 0); }
    out.push_back(std::move(v));
    skip_ws(p, e);
    if (p < e && *p == ',') { p++; continue; }
    if (p < e && *p == ']') { p++; break; }
    throw std::runtime_error("INPUT malformed parameter JSON");
  }
  skip_ws(p, e);
  if (p != e) throw std::runtime_error("INPUT trailing bytes after parameters");
  return out;
}
inline std::string quote(std::string_view s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
    else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); out += b; }
    else out += char(c);
  }
  return out + "\"";
}
// Sets obj.path = value, creating objects along the way (replacing non-objects).
inline void set_path(Json& obj, const std::vector<std::string>& path, Json value) {
  Json* cur = &obj;
  for (size_t i = 0; i < path.size(); i++) {
    if (cur->kind != Json::Object) { *cur = Json{}; cur->kind = Json::Object; }
    Json* next = nullptr;
    for (auto& f : cur->fields) if (f.first == path[i]) { next = &f.second; break; }
    if (!next) { cur->fields.emplace_back(path[i], Json{}); next = &cur->fields.back().second; }
    if (i + 1 == path.size()) *next = std::move(value);
    else cur = next;
  }
}
inline const Json* find_dom(const Json& j, const std::vector<std::string>& path) {
  const Json* cur = &j;
  for (const auto& p : path) { if (cur->kind != Json::Object) return nullptr; cur = cur->get(p); if (!cur) return nullptr; }
  return cur;
}
// Removes obj.path; returns whether it was there.
inline bool erase_path(Json& obj, const std::vector<std::string>& path) {
  Json* cur = &obj;
  for (size_t i = 0; i < path.size(); i++) {
    if (cur->kind != Json::Object) return false;
    auto it = std::find_if(cur->fields.begin(), cur->fields.end(), [&](const auto& f) { return f.first == path[i]; });
    if (it == cur->fields.end()) return false;
    if (i + 1 == path.size()) { cur->fields.erase(it); return true; }
    cur = &it->second;
  }
  return false;
}
inline void dump_into(std::string& out, const Json& j) {
  if (j.kind == Json::Object) {
    out += '{';
    for (size_t i = 0; i < j.fields.size(); i++) { if (i) out += ','; out += quote(j.fields[i].first); out += ':'; dump_into(out, j.fields[i].second); }
    out += '}';
  } else if (j.kind == Json::Array) {
    out += '[';
    for (size_t i = 0; i < j.items.size(); i++) { if (i) out += ','; dump_into(out, j.items[i]); }
    out += ']';
  } else out += j.raw.empty() ? std::string("null") : j.raw;
}
inline std::string dump(const Json& j) { std::string s; dump_into(s, j); return s; }
// Splits "a.b.c" into a path; field names cannot be empty.
inline std::vector<std::string> split_path(std::string_view f) {
  std::vector<std::string> path;
  size_t s = 0;
  for (size_t i = 0; i <= f.size(); i++) if (i == f.size() || f[i] == '.') { if (i == s) throw std::runtime_error("INPUT field path"); path.emplace_back(f.substr(s, i - s)); s = i + 1; }
  if (path.empty() || path.size() > 16) throw std::runtime_error("INPUT field path");
  return path;
}

}  // namespace kestro::json

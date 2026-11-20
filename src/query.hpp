#pragma once
// Query specs (JSON) and filter evaluation. See QUERY.md for the language.
//
//   {"collection": "users",
//    "where": {"age": {"$gte": 18}, "city": "Colombo", "_key": {"$prefix": "u:"}},
//    "orderBy": "-age",          // field, "-field" for descending, "_key" (default)
//    "limit": 100,               // 1..10000
//    "after": "<cursor>",        // from the previous page's "next"
//    "count": true,              // only count matches
//    "keysOnly": true}           // return keys and versions without values
#include "json.hpp"
#include <algorithm>
#include <functional>
#include <memory>

namespace kestro::query {

using json::Scalar;
struct Cond {
  enum Op { Eq, Ne, Gt, Gte, Lt, Lte, In, Prefix, Exists } op = Eq;
  std::string field;
  std::vector<std::string> path;
  bool is_key = false;  // the document key itself ("_key")
  Scalar v;
  std::vector<Scalar> list;
  bool exists = true;
};
struct Spec {
  std::string collection;
  std::vector<Cond> where;
  bool order_key = true, desc = false;
  std::string order_field;
  std::vector<std::string> order_path;
  size_t limit = 100;
  bool has_after = false;
  std::string after;  // decoded cursor
  bool count = false, keys_only = false;
  // Engine-internal (SQL): an extra filter over (key, document), and no limit cap.
  std::function<bool(std::string_view, std::string_view)> residual;
  bool unlimited = false;
  // Engine-internal: receives every match (key, version, document) instead of results.
  std::function<void(std::string_view, uint64_t, std::string_view)> sink;
  // Engine-internal (SQL): further sort fields after order_field, same direction (ORDER BY
  // a, b, ...); NULL and missing values sort together there.
  std::vector<std::string> order_more;
  std::vector<std::vector<std::string>> order_more_paths;
  bool order_more_key = false;  // and then the key (ORDER BY a, b, id)
  // Engine-internal (SQL MATCH): visit only these row keys (sorted), found by a FULLTEXT index.
  std::shared_ptr<const std::vector<std::string>> keys;
};

inline std::string hex(std::string_view s) {
  static const char* d = "0123456789abcdef";
  std::string out; out.reserve(s.size() * 2);
  for (unsigned char c : s) { out += d[c >> 4]; out += d[c & 15]; }
  return out;
}
inline std::string unhex(std::string_view s) {
  if (s.size() % 2) throw std::runtime_error("INPUT invalid cursor");
  auto v = [](char c) -> int { if (c >= '0' && c <= '9') return c - '0'; if (c >= 'a' && c <= 'f') return c - 'a' + 10; throw std::runtime_error("INPUT invalid cursor"); };
  std::string out;
  for (size_t i = 0; i < s.size(); i += 2) out += char(v(s[i]) * 16 + v(s[i + 1]));
  return out;
}
inline Cond::Op op_of(std::string_view name) {
  if (name == "$eq") return Cond::Eq; if (name == "$ne") return Cond::Ne;
  if (name == "$gt") return Cond::Gt; if (name == "$gte") return Cond::Gte;
  if (name == "$lt") return Cond::Lt; if (name == "$lte") return Cond::Lte;
  if (name == "$in") return Cond::In; if (name == "$prefix") return Cond::Prefix;
  if (name == "$exists") return Cond::Exists;
  throw std::runtime_error("INPUT unknown operator " + std::string(name));
}
inline bool valid_collection(std::string_view c) { return !c.empty() && c.size() <= 64 && c.find('\0') == std::string_view::npos && uint8_t(c[0]) > 1; }

inline Spec parse(std::string_view text) {
  json::Json j = json::parse(text);
  if (j.kind != json::Json::Object) throw std::runtime_error("INPUT query must be a JSON object");
  Spec q;
  for (const auto& [k, v] : j.fields) {
    if (k == "collection") { if (!v.is_string() || !valid_collection(v.str)) throw std::runtime_error("INPUT query collection"); q.collection = v.str; }
    else if (k == "where") {
      if (v.kind != json::Json::Object) throw std::runtime_error("INPUT where must be an object");
      for (const auto& [field, cond] : v.fields) {
        auto add = [&](Cond c) {
          c.field = field;
          if (field == "_key") c.is_key = true; else c.path = json::split_path(field);
          q.where.push_back(std::move(c));
        };
        bool ops = cond.kind == json::Json::Object && !cond.fields.empty() && std::all_of(cond.fields.begin(), cond.fields.end(), [](const auto& f) { return !f.first.empty() && f.first[0] == '$'; });
        if (!ops) { Cond c; c.op = Cond::Eq; c.v = cond.scalar(); add(std::move(c)); continue; }
        for (const auto& [name, arg] : cond.fields) {
          Cond c; c.op = op_of(name);
          if (c.op == Cond::In) {
            if (arg.kind != json::Json::Array || arg.items.size() > 1000) throw std::runtime_error("INPUT $in needs an array of up to 1000 values");
            for (const auto& it : arg.items) c.list.push_back(it.scalar());
          } else if (c.op == Cond::Exists) {
            if (arg.kind != json::Json::True && arg.kind != json::Json::False) throw std::runtime_error("INPUT $exists needs true or false");
            c.exists = arg.kind == json::Json::True;
          } else if (c.op == Cond::Prefix) {
            if (!arg.is_string()) throw std::runtime_error("INPUT $prefix needs a string");
            c.v = arg.scalar();
          } else c.v = arg.scalar();
          add(std::move(c));
        }
      }
      if (q.where.size() > 32) throw std::runtime_error("LIMIT at most 32 conditions");
    } else if (k == "orderBy") {
      if (!v.is_string() || v.str.empty()) throw std::runtime_error("INPUT orderBy must be a field name");
      std::string f = v.str;
      if (f[0] == '-') { q.desc = true; f.erase(0, 1); }
      if (f == "_key") q.order_key = true;
      else { q.order_key = false; q.order_field = f; q.order_path = json::split_path(f); }
    } else if (k == "limit") {
      if (!v.is_number() || v.num < 1 || v.num > 10000 || v.num != double(size_t(v.num))) throw std::runtime_error("INPUT limit must be 1..10000");
      q.limit = size_t(v.num);
    } else if (k == "after") {
      if (v.kind == json::Json::Null) continue;
      if (!v.is_string()) throw std::runtime_error("INPUT after must be a cursor string");
      q.has_after = true; q.after = unhex(v.str);
    } else if (k == "count") q.count = v.kind == json::Json::True;
    else if (k == "keysOnly") q.keys_only = v.kind == json::Json::True;
    else throw std::runtime_error("INPUT unknown query field " + k);
  }
  if (q.collection.empty()) throw std::runtime_error("INPUT query needs a collection");
  return q;
}

// Comparisons other than = and != apply within one type (numbers with numbers, strings
// with strings, booleans with booleans), as in MongoDB; other types never match them.
inline int type_class(json::Tag t) { return t == json::FALSE_ || t == json::TRUE_ ? json::FALSE_ : t; }
inline bool match(const Cond& c, const Scalar& d) {
  switch (c.op) {
    case Cond::Exists: return (d.tag != json::MISSING) == c.exists;
    case Cond::Eq: return json::compare(d, c.v) == 0;
    case Cond::Ne: return json::compare(d, c.v) != 0;
    case Cond::In: for (const auto& x : c.list) if (json::compare(d, x) == 0) return true; return false;
    case Cond::Prefix: return d.tag == json::STRING && d.str.compare(0, c.v.str.size(), c.v.str) == 0;
    default: break;
  }
  if (d.tag == json::MISSING || type_class(d.tag) != type_class(c.v.tag)) return false;
  int r = json::compare(d, c.v);
  switch (c.op) {
    case Cond::Gt: return r > 0; case Cond::Gte: return r >= 0;
    case Cond::Lt: return r < 0; case Cond::Lte: return r <= 0;
    default: return false;
  }
}

// Byte ranges [lo, hi) of index entries (encode(value) + key) that can satisfy c; hi ""
// means unbounded. Empty result: c cannot narrow the scan (!=, $exists).
using Ranges = std::vector<std::pair<std::string, std::string>>;
inline Ranges ranges_for(const Cond& c) {
  auto enc = [](const Scalar& s) { return json::encode(s); };
  auto class_lo = [](json::Tag t) { return std::string(1, char(type_class(t))); };
  auto class_hi = [](json::Tag t) { return std::string(1, char((t == json::FALSE_ ? json::TRUE_ : t) + 1)); };
  Ranges r;
  switch (c.op) {
    case Cond::Eq: { auto e = enc(c.v); r.push_back({e, json::prefix_end(e)}); break; }
    case Cond::In: {
      std::vector<std::string> es;
      for (const auto& x : c.list) es.push_back(enc(x));
      std::sort(es.begin(), es.end()); es.erase(std::unique(es.begin(), es.end()), es.end());
      for (auto& e : es) r.push_back({e, json::prefix_end(e)});
      if (r.empty()) r.push_back({std::string(1, '\x01'), std::string(1, '\x01')});  // matches nothing
      break;
    }
    case Cond::Gt: r.push_back({json::prefix_end(enc(c.v)), class_hi(c.v.tag)}); break;
    case Cond::Gte: r.push_back({enc(c.v), class_hi(c.v.tag)}); break;
    case Cond::Lt: r.push_back({class_lo(c.v.tag), enc(c.v)}); break;
    case Cond::Lte: r.push_back({class_lo(c.v.tag), json::prefix_end(enc(c.v))}); break;
    case Cond::Prefix: { std::string p(1, char(json::STRING)); json::escape_into(p, c.v.str); r.push_back({p, json::prefix_end(p)}); break; }
    default: break;
  }
  return r;
}
// Narrowing for the document key: the primary key range inside "collection\0".
inline bool key_range(const Cond& c, const std::string& prefix, std::string& lo, std::string& hi) {
  if (c.op != Cond::Eq && c.op != Cond::Prefix && c.op != Cond::Gt && c.op != Cond::Gte && c.op != Cond::Lt && c.op != Cond::Lte) return false;
  if (c.v.tag != json::STRING) { lo = hi = prefix; return true; }  // keys are strings: nothing matches
  std::string k = prefix + c.v.str;
  std::string end = json::prefix_end(prefix);
  switch (c.op) {
    case Cond::Eq: lo = k; hi = k + '\0'; break;
    case Cond::Prefix: lo = k; hi = json::prefix_end(k); break;
    case Cond::Gt: lo = k + '\0'; hi = end; break;
    case Cond::Gte: lo = k; hi = end; break;
    case Cond::Lt: lo = prefix; hi = k; break;
    case Cond::Lte: lo = prefix; hi = k + '\0'; break;
    default: return false;
  }
  return true;
}

}  // namespace kestro::query

// libFuzzer: the SQL tokenizer and parser, expression parsing and identifier renaming, the
// JSON parser and field access, the key/value query language. Any input may be rejected
// with an exception; none may crash, hang or touch memory it does not own.
#include "kestro.hpp"
#include <cstdint>
#include <cstdlib>
#include <string>

using namespace kestro;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size > 4096) return 0;
  std::string s(reinterpret_cast<const char*>(data), size);
  try { auto p = sql::parse_named(s); (void)p; } catch (const std::exception&) {}
  try { auto e = sql::parse_expression(s); (void)e; } catch (const std::exception&) {}
  try { auto t = sql::rename_identifier(s, "a", "b", false); t = sql::rename_identifier(s, "t", "u", true); (void)t; } catch (const std::exception&) {}
  try {
    auto j = json::parse(s);
    auto d = json::dump(j);
    auto again = json::parse(d);                      // what dump writes must parse again
    if (json::dump(again) != d) std::abort();     // and dump the same
    auto f = json::field(s, {"a"}); auto g = json::field(s, {"a", "b"});
    std::string enc; json::encode_into(enc, f); json::encode_into(enc, g);
  } catch (const std::exception&) {}
  try { auto sc = json::scalar_of(s); std::string enc; json::encode_into(enc, sc); } catch (const std::exception&) {}
  try { auto q = query::parse(s); (void)q; } catch (const std::exception&) {}
  return 0;
}

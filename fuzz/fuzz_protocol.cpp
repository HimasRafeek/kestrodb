// libFuzzer: the binary request decoders used by the worker and kestro-server (batches of
// writes, key lists). Malformed requests must be rejected, never misread.
#include "protocol.hpp"
#include <cstdint>
#include <cstdlib>

using namespace kestro;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  try { auto ops = protocol::parse_batch(data, size); for (const auto& o : ops) if (o.key.size() > size) std::abort(); } catch (const std::exception&) {}
  try { auto keys = protocol::parse_keys(data, size); size_t total = 0; for (const auto& k : keys) total += k.size(); if (total > size) std::abort(); } catch (const std::exception&) {}
  return 0;
}

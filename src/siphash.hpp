#pragma once
// SipHash-2-4 (Aumasson & Bernstein, "SipHash: a fast short-input PRF", 2012),
// 64- and 128-bit outputs. A keyed pseudorandom function: without the 128-bit key,
// nobody can produce an input with a chosen output.
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace kestro::sip {
inline uint64_t rotl(uint64_t x, int b) { return (x << b) | (x >> (64 - b)); }
// Little-endian word. One load on little-endian targets (every supported one); MSVC does
// not fold the byte loop into a load, which made the point-read checksum twice as slow.
inline uint64_t load64(const unsigned char* p) {
#if defined(_M_X64) || defined(_M_ARM64) || defined(_M_IX86) || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
  uint64_t v; memcpy(&v, p, 8); return v;
#else
  uint64_t v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | p[i]; return v;
#endif
}
struct State {
  uint64_t v0, v1, v2, v3;
  void round() {
    v0 += v1; v1 = rotl(v1, 13); v1 ^= v0; v0 = rotl(v0, 32);
    v2 += v3; v3 = rotl(v3, 16); v3 ^= v2;
    v0 += v3; v3 = rotl(v3, 21); v3 ^= v0;
    v2 += v1; v1 = rotl(v1, 17); v1 ^= v2; v2 = rotl(v2, 32);
  }
  void absorb(uint64_t m) { v3 ^= m; round(); round(); v0 ^= m; }
};
// Compresses the message; out128 selects the 128-bit variant's initialization.
inline State compress(uint64_t k0, uint64_t k1, const void* data, size_t n, bool out128) {
  State s{k0 ^ 0x736f6d6570736575ULL, k1 ^ 0x646f72616e646f6dULL, k0 ^ 0x6c7967656e657261ULL, k1 ^ 0x7465646279746573ULL};
  if (out128) s.v1 ^= 0xee;
  auto p = static_cast<const unsigned char*>(data);
  size_t blocks = n / 8;
  for (size_t i = 0; i < blocks; i++) s.absorb(load64(p + 8 * i));
  uint64_t last = uint64_t(n) << 56;
  for (size_t i = 0, tail = n % 8; i < tail; i++) last |= uint64_t(p[8 * blocks + i]) << (8 * i);
  s.absorb(last);
  return s;
}
inline uint64_t hash64(uint64_t k0, uint64_t k1, const void* data, size_t n) {
  auto s = compress(k0, k1, data, n, false);
  s.v2 ^= 0xff; s.round(); s.round(); s.round(); s.round();
  return s.v0 ^ s.v1 ^ s.v2 ^ s.v3;
}
// The first 8 bytes of hash128 as a number (little-endian), without the second finalization:
// what a point read compares (digest_eq looks at 8 bytes).
inline uint64_t hash128_low(uint64_t k0, uint64_t k1, const void* data, size_t n) {
  auto s = compress(k0, k1, data, n, true);
  s.v2 ^= 0xee; s.round(); s.round(); s.round(); s.round();
  return s.v0 ^ s.v1 ^ s.v2 ^ s.v3;
}
// Writes 16 bytes, little-endian, low word first (the reference output order).
inline void hash128(uint64_t k0, uint64_t k1, const void* data, size_t n, unsigned char out[16]) {
  auto s = compress(k0, k1, data, n, true);
  s.v2 ^= 0xee; s.round(); s.round(); s.round(); s.round();
  uint64_t a = s.v0 ^ s.v1 ^ s.v2 ^ s.v3;
  s.v1 ^= 0xdd; s.round(); s.round(); s.round(); s.round();
  uint64_t b = s.v0 ^ s.v1 ^ s.v2 ^ s.v3;
  for (int i = 0; i < 8; i++) { out[i] = (unsigned char)(a >> (8 * i)); out[8 + i] = (unsigned char)(b >> (8 * i)); }
}
}

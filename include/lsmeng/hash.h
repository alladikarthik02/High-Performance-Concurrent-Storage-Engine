#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace lsmeng {

// A 64-bit hash for the Bloom filter (SPEC 3.6) and for cache shard selection (3.11).
//
// Not a cryptographic hash and not trying to be: nothing here defends against an adversary
// choosing keys. What it must do is spread ordinary keys -- which in a real workload share
// long prefixes (`user:1001:name`, `user:1001:email`) -- across the whole 64-bit range,
// because a Bloom filter whose hash ignores the tail of the key has an FPR far worse than
// theory and no test of the filter itself would notice.
//
// This is the xxHash64-style mixing pattern: multiply by a large odd constant, xor-shift,
// multiply again. Cheap, and it passes the avalanche property that matters here.
inline uint64_t Hash64(const char* data, size_t n, uint64_t seed = 0x9E3779B97F4A7C15ull) {
  constexpr uint64_t kM = 0xC6A4A7935BD1E995ull;
  uint64_t h = seed ^ (static_cast<uint64_t>(n) * kM);
  while (n >= 8) {
    uint64_t k;
    std::memcpy(&k, data, 8);   // memcpy, not a cast: an unaligned 8-byte load is UB, and
    k *= kM;                    // UBSan will say so. The compiler emits the same ldr.
    k ^= k >> 47;
    k *= kM;
    h ^= k;
    h *= kM;
    data += 8;
    n -= 8;
  }
  if (n) {
    uint64_t k = 0;
    std::memcpy(&k, data, n);   // the tail must contribute; dropping it is a real FPR bug
    h ^= k;
    h *= kM;
  }
  h ^= h >> 47;
  h *= kM;
  h ^= h >> 47;
  return h;
}

}  // namespace lsmeng

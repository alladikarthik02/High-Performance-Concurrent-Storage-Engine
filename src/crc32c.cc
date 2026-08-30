#include "lsmeng/crc32c.h"

#include <cstring>

#if defined(__aarch64__)
#  include <sys/auxv.h>
#  include <asm/hwcap.h>
#endif
#if defined(__x86_64__)
#  include <nmmintrin.h>
#  include <cpuid.h>
#endif

namespace lsmeng {
namespace crc32c {
namespace {

// Reflected Castagnoli polynomial. The table is built once at startup rather than being a
// 1 KiB literal: it is four lines, and a hand-pasted table is a place for a typo that no
// review catches and that only shows up as a checksum mismatch on someone else's machine.
struct Table {
  uint32_t t[8][256];
  Table() {
    constexpr uint32_t kPoly = 0x82F63B78u;
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? (kPoly ^ (c >> 1)) : (c >> 1);
      t[0][i] = c;
    }
    // Slicing-by-8: each extra table folds one more byte per iteration.
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = t[0][i];
      for (int s = 1; s < 8; ++s) { c = t[0][c & 0xFF] ^ (c >> 8); t[s][i] = c; }
    }
  }
};
const Table& table() { static const Table t; return t; }

bool DetectHardware() {
#if defined(__aarch64__)
  return (getauxval(AT_HWCAP) & HWCAP_CRC32) != 0;
#elif defined(__x86_64__)
  unsigned a, b, c, d;
  if (!__get_cpuid(1, &a, &b, &c, &d)) return false;
  return (c & (1u << 20)) != 0;   // SSE4.2
#else
  return false;
#endif
}
const bool kHasHardware = DetectHardware();

#if defined(__aarch64__)
// The +crc target attribute applies to this function only, so the rest of the translation
// unit stays baseline and the binary still loads on a CPU without the extension.
__attribute__((target("+crc")))
uint32_t ExtendArm(uint32_t crc, const char* data, size_t n) {
  uint32_t c = ~crc;
  // memcpy, not a reinterpret_cast. A Slice points into the middle of a block, so `data`
  // is arbitrarily aligned, and an unaligned uint64_t load is undefined behaviour even on
  // architectures that tolerate it -- UBSan says so, and the compiler is entitled to act
  // on it. The memcpy compiles to the same single `ldr`. See CHALLENGES B7.
  while (n >= 8) {
    uint64_t w;
    std::memcpy(&w, data, 8);
    c = __builtin_aarch64_crc32cx(c, w);
    data += 8;
    n -= 8;
  }
  while (n--) c = __builtin_aarch64_crc32cb(c, static_cast<uint8_t>(*data++));
  return ~c;
}
#endif

#if defined(__x86_64__)
__attribute__((target("sse4.2")))
uint32_t ExtendX86(uint32_t crc, const char* data, size_t n) {
  uint64_t c = ~crc;
  while (n >= 8) {
    uint64_t w;
    std::memcpy(&w, data, 8);   // see the aarch64 path above -- same reason
    c = _mm_crc32_u64(c, w);
    data += 8;
    n -= 8;
  }
  while (n--) c = _mm_crc32_u8(static_cast<uint32_t>(c), static_cast<uint8_t>(*data++));
  return ~static_cast<uint32_t>(c);
}
#endif

}  // namespace

uint32_t ExtendPortable(uint32_t crc, const char* data, size_t n) {
  const Table& tb = table();
  uint32_t c = ~crc;
  // Byte-at-a-time until 8-byte aligned, then slicing-by-8, then the tail.
  while (n && (reinterpret_cast<uintptr_t>(data) & 7)) {
    c = tb.t[0][(c ^ static_cast<uint8_t>(*data++)) & 0xFF] ^ (c >> 8);
    --n;
  }
  while (n >= 8) {
    uint32_t lo;
    std::memcpy(&lo, data, 4);
    uint32_t hi;
    std::memcpy(&hi, data + 4, 4);
    c ^= lo;
    c = tb.t[7][c & 0xFF] ^ tb.t[6][(c >> 8) & 0xFF] ^ tb.t[5][(c >> 16) & 0xFF] ^
        tb.t[4][(c >> 24) & 0xFF] ^ tb.t[3][hi & 0xFF] ^ tb.t[2][(hi >> 8) & 0xFF] ^
        tb.t[1][(hi >> 16) & 0xFF] ^ tb.t[0][(hi >> 24) & 0xFF];
    data += 8;
    n -= 8;
  }
  while (n--) c = tb.t[0][(c ^ static_cast<uint8_t>(*data++)) & 0xFF] ^ (c >> 8);
  return ~c;
}

uint32_t Extend(uint32_t crc, const char* data, size_t n) {
#if defined(__aarch64__)
  if (kHasHardware) return ExtendArm(crc, data, n);
#elif defined(__x86_64__)
  if (kHasHardware) return ExtendX86(crc, data, n);
#endif
  return ExtendPortable(crc, data, n);
}

const char* Implementation() {
#if defined(__aarch64__)
  return kHasHardware ? "aarch64 __crc32c (hardware)" : "portable table (aarch64, no HWCAP_CRC32)";
#elif defined(__x86_64__)
  return kHasHardware ? "x86-64 SSE4.2 _mm_crc32 (hardware)" : "portable table (x86-64, no SSE4.2)";
#else
  return "portable table";
#endif
}

}  // namespace crc32c
}  // namespace lsmeng

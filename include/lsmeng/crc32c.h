#pragma once
#include <cstddef>
#include <cstdint>

namespace lsmeng {
namespace crc32c {

// CRC32C (Castagnoli, reflected polynomial 0x82F63B78) -- the checksum on every WAL
// record, every MANIFEST record, and every SST block (SPEC S4, S7).
//
// WHY CASTAGNOLI AND NOT THE ZIP CRC32: it has better error-detection properties for the
// short records we write, and both x86-64 and ARMv8 implement it as a single instruction.
// It is the checksum a storage engine is expected to use, and being able to say why is
// part of the point.
//
// THREE IMPLEMENTATIONS, chosen at RUNTIME (see CHALLENGES B3):
//   * aarch64: __crc32cd, behind a getauxval(AT_HWCAP) check
//   * x86-64:  _mm_crc32_u64, behind a CPUID check
//   * everywhere: a table-driven fallback, always compiled, always correct
// The check is at runtime and not compile time on purpose: a binary built with the
// extension enabled and called unconditionally runs fine on the build machine and takes
// SIGILL on a slightly older CPU.

// Continue a CRC over more data. `crc` is a running value in the same representation this
// header returns -- i.e. already finalised -- so Extend(Extend(0, a), b) == Value(a||b).
uint32_t Extend(uint32_t crc, const char* data, size_t n);

inline uint32_t Value(const char* data, size_t n) { return Extend(0, data, n); }

// Which path is live. Reported by tests and by tools/env_facts so a machine's actual
// behaviour is recorded rather than assumed.
const char* Implementation();

// Force the portable path, for the differential test that proves the fast and slow paths
// agree. "The two implementations disagree" is a silent corruption bug, not a perf bug.
uint32_t ExtendPortable(uint32_t crc, const char* data, size_t n);

}  // namespace crc32c
}  // namespace lsmeng

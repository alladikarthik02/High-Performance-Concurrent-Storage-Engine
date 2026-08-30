// T1: CRC32C, against the RFC 3720 (iSCSI) test vectors.
//
// WHY THE PUBLISHED VECTORS MATTER MORE THAN A ROUNDTRIP TEST: a roundtrip only proves the
// implementation agrees with itself. Every checksum in this engine is written once and
// verified later, possibly by a different build with a different code path selected at
// runtime (CHALLENGES B3). If our CRC were self-consistent but not actually CRC32C, every
// test would pass and the format would be undocumentable.
#include "tests/test.h"

#include <string>
#include <vector>

#include "lsmeng/crc32c.h"

using namespace lsmeng;

TEST(rfc3720_test_vectors) {
  std::fprintf(stderr, "   crc32c implementation: %s\n", crc32c::Implementation());

  std::string zeros(32, '\x00');
  CHECK_EQ(crc32c::Value(zeros.data(), 32), 0x8a9136aau);

  std::string ones(32, '\xff');
  CHECK_EQ(crc32c::Value(ones.data(), 32), 0x62a8ab43u);

  std::string inc(32, '\0');
  for (int i = 0; i < 32; ++i) inc[i] = static_cast<char>(i);
  CHECK_EQ(crc32c::Value(inc.data(), 32), 0x46dd794eu);

  std::string dec(32, '\0');
  for (int i = 0; i < 32; ++i) dec[i] = static_cast<char>(31 - i);
  CHECK_EQ(crc32c::Value(dec.data(), 32), 0x113fdb5cu);

  // The standard "123456789" check value for CRC-32C.
  CHECK_EQ(crc32c::Value("123456789", 9), 0xe3069283u);

  // Empty input must be 0, which is exactly why SPEC E-5 bans a zero-length WAL record:
  // crc=0,len=0 would otherwise be a self-consistent record, and a zero-filled region of
  // a file would parse as infinitely many valid empty records.
  CHECK_EQ(crc32c::Value("", 0), 0u);
}

TEST(hardware_and_portable_paths_agree_on_random_data) {
  // "The fast path and the slow path disagree" is a silent data-corruption bug: a file
  // written on a CPU with the CRC extension would fail verification on one without it,
  // and vice versa. This is the test that makes the runtime dispatch safe.
  testing::Rng rng(testing::seed());
  std::string buf;
  for (int trial = 0; trial < 300; ++trial) {
    size_t n = rng.below(600);
    buf.resize(n);
    for (size_t i = 0; i < n; ++i) buf[i] = static_cast<char>(rng.next());
    TCTX("n=" << n);
    CHECK_EQ(crc32c::Value(buf.data(), n), crc32c::ExtendPortable(0, buf.data(), n));
  }
}

TEST(extend_is_associative_over_split_points) {
  // The WAL and the SST both CRC a header and then a payload without concatenating them
  // into one buffer, so Extend must compose. Every split point is tested because the
  // 8-byte-at-a-time loops have alignment-dependent tails.
  const std::string data = "the quick brown fox jumps over the lazy dog, 0123456789ABCDEF";
  const uint32_t whole = crc32c::Value(data.data(), data.size());
  for (size_t split = 0; split <= data.size(); ++split) {
    TCTX("split=" << split);
    uint32_t c = crc32c::Value(data.data(), split);
    c = crc32c::Extend(c, data.data() + split, data.size() - split);
    CHECK_EQ(c, whole);
  }
}

TEST(unaligned_starts_produce_the_same_value) {
  // The portable path aligns to 8 bytes before slicing-by-8; the hardware path does not.
  // A bug in that alignment prologue shows up only for buffers that start off-alignment,
  // which is the normal case for a Slice pointing into the middle of a block.
  std::string buf(200, '\0');
  testing::Rng rng(testing::seed());
  for (auto& c : buf) c = static_cast<char>(rng.next());
  for (size_t off = 0; off < 16; ++off) {
    TCTX("off=" << off);
    CHECK_EQ(crc32c::Value(buf.data() + off, 100),
             crc32c::ExtendPortable(0, buf.data() + off, 100));
  }
}

TEST(a_single_bit_flip_always_changes_the_checksum) {
  // The property the whole design leans on (SPEC S4/S7): a corrupted block must not
  // validate. A 32-bit CRC cannot promise this for arbitrary corruption, but it does for
  // any single-bit flip, and that is the case worth asserting.
  std::string buf(64, 'x');
  const uint32_t base = crc32c::Value(buf.data(), buf.size());
  for (size_t i = 0; i < buf.size(); ++i) {
    for (int bit = 0; bit < 8; ++bit) {
      buf[i] = static_cast<char>(buf[i] ^ (1 << bit));
      CHECK_NE(crc32c::Value(buf.data(), buf.size()), base);
      buf[i] = static_cast<char>(buf[i] ^ (1 << bit));
    }
  }
}

RUN_ALL()

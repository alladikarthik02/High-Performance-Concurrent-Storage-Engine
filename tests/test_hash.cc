// T1: the 64-bit hash used by the Bloom filter and by cache shard selection.
//
// A hash that ignores part of the key produces a Bloom filter whose false-positive rate is
// far worse than theory -- and no test OF THE FILTER would notice, because the filter is
// still correct, just useless. So the properties are asserted here, at the source.
#include "tests/test.h"

#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "lsmeng/hash.h"

using namespace lsmeng;

TEST(hash_depends_on_every_byte_including_the_tail) {
  // Real keys share long prefixes (`user:1001:name` vs `user:1001:email`). A hash whose
  // tail handling is broken maps them together, which is invisible until the FPR is
  // measured -- and by then it looks like a Bloom bug.
  const std::string base(64, 'x');
  const uint64_t h0 = Hash64(base.data(), base.size());
  for (size_t i = 0; i < base.size(); ++i) {
    TCTX("byte=" << i);
    std::string m = base;
    m[i] = 'y';
    CHECK_NE(Hash64(m.data(), m.size()), h0);
  }
  // Lengths 1..16 exercise the sub-8-byte tail path specifically.
  for (size_t n = 1; n <= 16; ++n) {
    std::string a(n, 'a'), b(n, 'a');
    b[n - 1] = 'b';
    TCTX("n=" << n);
    CHECK_NE(Hash64(a.data(), n), Hash64(b.data(), n));
  }
}

TEST(length_is_part_of_the_hash) {
  // "ab" and "ab\0" must not collide: they are different keys (SPEC 3.1 allows NULs).
  CHECK_NE(Hash64("ab", 2), Hash64("ab\0", 3));
  CHECK_NE(Hash64("", 0), Hash64("\0", 1));
}

TEST(no_collisions_on_a_large_realistic_key_set) {
  // 200k structured keys, which is the shape that actually breaks weak hashes: long shared
  // prefixes and a varying tail.
  std::set<uint64_t> seen;
  size_t collisions = 0;
  const int kN = 200000;
  for (int i = 0; i < kN; ++i) {
    std::string k = "user:" + std::to_string(1000000 + i) + ":name";
    if (!seen.insert(Hash64(k.data(), k.size())).second) ++collisions;
  }
  // Birthday bound: 200k keys in a 64-bit space expects ~1e-9 collisions. Any at all
  // means the effective output space is far smaller than 64 bits.
  std::fprintf(stderr, "   MEASURED: %zu collisions over %d structured keys\n", collisions, kN);
  CHECK_EQ(collisions, 0u);
}

TEST(bits_are_distributed_across_the_whole_word) {
  // The Bloom filter splits the hash into two 32-bit halves (SPEC 3.6). If the high half
  // were poorly mixed, the double-hashing step degenerates and the FPR collapses toward
  // that of a single hash. Check every bit position is set roughly half the time.
  const int kN = 20000;
  int ones[64] = {0};
  for (int i = 0; i < kN; ++i) {
    std::string k = "key" + std::to_string(i);
    uint64_t h = Hash64(k.data(), k.size());
    for (int b = 0; b < 64; ++b) if ((h >> b) & 1) ++ones[b];
  }
  for (int b = 0; b < 64; ++b) {
    TCTX("bit=" << b << " ones=" << ones[b] << "/" << kN);
    CHECK_GT(ones[b], kN * 0.45);
    CHECK_LT(ones[b], kN * 0.55);
  }
}

TEST(shard_selection_is_balanced) {
  // SPEC 3.11 picks a cache shard from this hash. A skewed shard map would put most of the
  // traffic on one mutex and make the 16-shard experiment (R12) measure nothing.
  const int kShards = 16;
  std::vector<int> counts(kShards, 0);
  const int kN = 160000;
  for (int i = 0; i < kN; ++i) {
    uint64_t file = static_cast<uint64_t>(i % 500);
    uint64_t off = static_cast<uint64_t>((i / 500) * 4096);
    char buf[16];
    std::memcpy(buf, &file, 8);
    std::memcpy(buf + 8, &off, 8);
    counts[Hash64(buf, 16) % kShards]++;
  }
  const int expected = kN / kShards;
  for (int s = 0; s < kShards; ++s) {
    TCTX("shard=" << s << " count=" << counts[s] << " expected=" << expected);
    CHECK_GT(counts[s], expected * 0.9);
    CHECK_LT(counts[s], expected * 1.1);
  }
}

TEST(seed_changes_the_result) {
  CHECK_NE(Hash64("abc", 3, 1), Hash64("abc", 3, 2));
}

RUN_ALL()

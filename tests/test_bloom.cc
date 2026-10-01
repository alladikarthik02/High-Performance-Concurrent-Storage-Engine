// T2: the Bloom filter. SPEC 3.6, R6.
//
// The headline test MEASURES the false-positive rate against theory. R6's claim
// ("keeping reads fast through Bloom filters") is only earned by a number, and a filter
// whose FPR is 10x theory is still *correct* -- it just does nothing. Correctness tests
// alone cannot tell those apart, which is why the measurement is the point.
#include "tests/test.h"

#include <cmath>
#include <string>
#include <vector>

#include "lsmeng/bloom.h"

using namespace lsmeng;

namespace {
std::string Key(int i) { return "key:" + std::to_string(i); }

// Build a filter over keys [0,n), return the encoded block.
std::string Build(int n, int bits_per_key) {
  BloomFilterBuilder b(bits_per_key);
  for (int i = 0; i < n; ++i) { std::string k = Key(i); b.AddKey(Slice(k)); }
  std::string out;
  b.Finish(&out);
  return out;
}
}  // namespace

TEST(no_false_negatives_ever) {
  // The one property the read path's correctness depends on. A false negative would make
  // Get skip a file that holds the key and return stale data or NotFound.
  for (int n : {1, 2, 10, 100, 1000, 10000}) {
    TCTX("n=" << n);
    const std::string block = Build(n, 10);
    BloomFilter f{Slice(block)};
    for (int i = 0; i < n; ++i) {
      std::string k = Key(i);
      if (!f.MayContain(Slice(k))) {
        TCTX("missing key=" << k);
        CHECK(false);
      }
    }
  }
}

TEST(measured_false_positive_rate_matches_theory) {
  // Theory: FPR = (1 - e^(-k*n/m))^k with k = round(m*ln2).
  // At m=10 bits/key, k=7 -> ~0.0082.
  struct Case { int bits; double expected; };
  const Case cases[] = {{4, 0.16}, {10, 0.0082}, {16, 0.00048}};
  const int n = 20000;

  for (const auto& c : cases) {
    const std::string block = Build(n, c.bits);
    BloomFilter f{Slice(block)};
    int fp = 0;
    const int trials = 100000;
    for (int i = 0; i < trials; ++i) {
      std::string k = "absent:" + std::to_string(i);   // disjoint from the added keys
      if (f.MayContain(Slice(k))) ++fp;
    }
    const double measured = static_cast<double>(fp) / trials;
    const double bits_per_key = static_cast<double>((block.size() - 1) * 8) / n;
    std::fprintf(stderr,
                 "   MEASURED bits/key=%d  k=%d  FPR=%.5f  (theory %.5f)  %.2f bytes/key\n",
                 c.bits, static_cast<int>(static_cast<uint8_t>(block.back())), measured,
                 c.expected, static_cast<double>(block.size()) / n);
    CHECK_LT(bits_per_key, c.bits + 0.1);
    // Within 2x of theory in either direction. Loose enough not to be flaky, tight enough
    // that a broken hash or a degenerate probe sequence fails it -- both of those produce
    // an FPR off by 10x or more, not 2x.
    CHECK_LT(measured, c.expected * 2.0 + 0.002);
    CHECK_GT(measured, c.expected * 0.4);
  }
}

TEST(empty_filter_answers_not_present_and_does_not_crash) {
  // SPEC E-3. n = 0 gives a zero-byte array without the minimum-size floor: `% nbits`
  // divides by zero and the probe reads out of bounds. ASan and UBSan both catch the
  // alternative, which is why this test is cheap insurance.
  BloomFilterBuilder b(10);
  std::string out;
  b.Finish(&out);
  CHECK_GE(out.size(), 9u);            // 8 bytes of array + the k byte
  BloomFilter f{Slice(out)};
  for (int i = 0; i < 100; ++i) {
    std::string k = Key(i);
    CHECK(!f.MayContain(Slice(k)));    // correct for an empty set
  }
}

TEST(missing_or_corrupt_filter_fails_open_never_closed) {
  // SPEC S8. Failing open costs a wasted read; failing closed loses data. Every
  // degenerate input must answer "maybe".
  const std::string k = "anything";
  CHECK(BloomFilter{}.MayContain(Slice(k)));                      // never constructed
  CHECK(BloomFilter{Slice("", 0)}.MayContain(Slice(k)));          // empty block
  CHECK(BloomFilter{Slice("\x07", 1)}.MayContain(Slice(k)));      // 1 byte: no array
  std::string bad_k(9, '\xff');
  bad_k[8] = static_cast<char>(99);                               // k out of range
  CHECK(BloomFilter{Slice(bad_k)}.MayContain(Slice(k)));
  std::string zero_k(9, '\xff');
  zero_k[8] = 0;                                                  // k = 0 probes nothing
  CHECK(BloomFilter{Slice(zero_k)}.MayContain(Slice(k)));
}

TEST(k_is_stored_in_the_block_so_a_reader_needs_no_configuration) {
  // A file written with bits_per_key=16 must read correctly in a process configured for
  // 10. If k came from the reader's Options instead of the file, every lookup in that file
  // would probe the wrong bits -- and produce FALSE NEGATIVES, the one failure the design
  // says is impossible.
  const std::string block16 = Build(500, 16);
  const std::string block4 = Build(500, 4);
  CHECK_EQ(static_cast<int>(static_cast<uint8_t>(block16.back())), 11);  // 16*0.69
  CHECK_EQ(static_cast<int>(static_cast<uint8_t>(block4.back())), 2);    // 4*0.69
  for (const auto* b : {&block16, &block4}) {
    BloomFilter f{Slice(*b)};
    for (int i = 0; i < 500; ++i) { std::string k = Key(i); CHECK(f.MayContain(Slice(k))); }
  }
}

TEST(k_is_clamped_to_at_least_one) {
  // bits_per_key = 1 gives k = 0 unclamped, and a filter that probes zero bits answers
  // "not present" for EVERYTHING -- a false-negative machine, and the worst possible
  // failure. The clamp is not defensive programming; it is load-bearing.
  BloomFilterBuilder b(1);
  CHECK_GE(b.k(), 1);
  std::string k = Key(1);
  b.AddKey(Slice(k));
  std::string out;
  b.Finish(&out);
  BloomFilter f{Slice(out)};
  CHECK(f.MayContain(Slice(k)));
}

TEST(keys_with_arbitrary_bytes_are_handled) {
  // SPEC 3.1: embedded NULs and 0xFF are legal. A hash that used strlen would truncate
  // them together and inflate the FPR invisibly.
  std::vector<std::string> keys = {
      std::string("a\0b", 3), std::string("\0", 1), "\xff\xff", std::string(1, '\0') + "\xff",
      "", "normal",
  };
  BloomFilterBuilder b(10);
  for (const auto& k : keys) b.AddKey(Slice(k));
  std::string out;
  b.Finish(&out);
  BloomFilter f{Slice(out)};
  for (const auto& k : keys) { TCTX("len=" << k.size()); CHECK(f.MayContain(Slice(k))); }
}

TEST(filter_size_is_proportional_to_key_count) {
  // Feeds SPEC E-34: a whole-file filter grows with the file, so a deep-tier file's filter
  // is megabytes. That is why the table cache must be bounded in BYTES and not only by
  // file count -- this test is where the number comes from.
  for (int n : {1000, 10000, 100000}) {
    const std::string block = Build(n, 10);
    const double bytes_per_key = static_cast<double>(block.size()) / n;
    std::fprintf(stderr, "   MEASURED n=%6d -> %8zu bytes (%.3f bytes/key)\n", n,
                 block.size(), bytes_per_key);
    CHECK_GT(bytes_per_key, 1.2);
    CHECK_LT(bytes_per_key, 1.3);
  }
}

RUN_ALL()

// T11: the latency histogram. If this is wrong, every number backing R10/R13 is wrong --
// and wrong in a way that looks like a result rather than like a bug (the B8/B15 lesson).
#include "tests/test.h"

#include <algorithm>
#include <cmath>
#include <thread>
#include <vector>

#include "lsmeng/histogram.h"

using namespace lsmeng;

TEST(percentiles_match_a_sorted_array_within_bucket_error) {
  // The reference is the definition: sort the samples and index. The histogram may differ
  // by at most one bucket width (0.4% above 256), never more.
  testing::Rng rng(testing::seed());
  std::vector<uint64_t> samples;
  Histogram h;
  for (int i = 0; i < 200000; ++i) {
    // A realistic latency shape: a tight body plus a heavy tail, which is exactly where a
    // naive histogram goes wrong.
    //
    // The tail is 2% rather than 0.1% ON PURPOSE. With a 0.1% tail, p99.9 lands EXACTLY on
    // the discontinuity between body and tail, where "the 99.9th percentile" is genuinely
    // ambiguous -- the last body sample and the first tail sample are both defensible
    // answers, and they differ by 20x. That is a property of the data, not of the
    // histogram, and a test that trips on it is testing the wrong thing. At 2%, p99 and
    // p99.9 both sit well inside the tail and p50/p90 well inside the body.
    uint64_t v = rng.one_in(50) ? 5000 + rng.below(200000) : 20 + rng.below(300);
    samples.push_back(v);
    h.Add(v);
  }
  std::sort(samples.begin(), samples.end());

  // WHAT THE HISTOGRAM GUARANTEES, stated as the test: the value it returns is a bucket's
  // LOWER BOUND, so it may understate the true percentile by at most one bucket width
  // (0.4% above 256 us, exact below), and must NEVER overstate it. Understating is the
  // conservative direction for a latency claim -- it cannot flatter the result.
  //
  // Two earlier formulations of this assertion were wrong, and both failed on correct
  // code, which is worth recording:
  //   * comparing VALUES at index p*n/100 is unstable at p99.9, where only ~200 of 200,000
  //     samples live and adjacent ones differ by hundreds of microseconds, so a one-rank
  //     disagreement reads as a 10% "error";
  //   * comparing RANKS is defeated by ties -- with ~667 samples per distinct value, the
  //     true percentile of the p90 value is 90.26%, and no implementation could do better.
  for (double p : {50.0, 90.0, 99.0, 99.9, 99.99}) {
    const uint64_t got = h.Percentile(p);
    const uint64_t truth = samples[static_cast<size_t>(p * samples.size() / 100.0)];
    const double understatement =
        truth ? (static_cast<double>(truth) - static_cast<double>(got)) / truth : 0.0;
    std::fprintf(stderr, "   p%-6.2f -> %8llu us   (exact %8llu, understated by %.4f%%)\n",
                 p, (unsigned long long)got, (unsigned long long)truth,
                 understatement * 100);
    TCTX("p=" << p);
    CHECK_LE(got, truth);              // never overstates
    CHECK_LT(understatement, 0.005);   // and understates by at most one bucket width
  }
}

TEST(exact_below_the_linear_threshold_and_the_boundary_convention) {
  // Under 256 us every value has its own bucket, so percentiles there are EXACT -- which
  // matters because that is where the overwhelming majority of operations land.
  //
  // It also pins down the boundary convention, which percentile definitions disagree
  // about: Percentile(p) returns the SMALLEST value v such that at least p% of samples are
  // <= v. With 100 copies each of 0..255 (25,600 samples), 50% is 12,800 samples, and
  // values 0..127 account for exactly 12,800 -- so p50 is 127, not 128. Writing 128 here
  // first was my arithmetic being wrong, not the histogram; stating the rule is what stops
  // the next person "fixing" the code to match a different convention.
  Histogram h;
  for (uint64_t v = 0; v < 256; ++v) for (int i = 0; i < 100; ++i) h.Add(v);
  CHECK_EQ(h.Percentile(50), 127u);
  CHECK_EQ(h.Percentile(99), 253u);
  CHECK_EQ(h.Percentile(100), 255u);
  CHECK_EQ(h.Max(), 255u);
}

TEST(merging_per_thread_histograms_is_equivalent_to_one) {
  // Per-thread histograms merged at the end is what avoids E-24's false sharing. The merge
  // must be exactly equivalent to a single shared histogram, or the tail is a fiction.
  testing::Rng rng(testing::seed());
  Histogram single;
  std::vector<Histogram> per_thread(8);
  for (int i = 0; i < 80000; ++i) {
    const uint64_t v = 1 + rng.below(100000);
    single.Add(v);
    per_thread[i % 8].Add(v);
  }
  Histogram merged;
  for (const auto& h : per_thread) merged.Merge(h);
  CHECK_EQ(merged.Count(), single.Count());
  CHECK_EQ(merged.Max(), single.Max());
  CHECK_EQ(merged.Min(), single.Min());
  for (double p : {50.0, 90.0, 99.0, 99.9}) {
    TCTX("p=" << p);
    CHECK_EQ(merged.Percentile(p), single.Percentile(p));
  }
}

TEST(edge_values_do_not_break_it) {
  Histogram h;
  CHECK_EQ(h.Percentile(50), 0u);        // empty
  CHECK_EQ(h.Count(), 0u);
  h.Add(0);
  CHECK_EQ(h.Percentile(50), 0u);
  h.Add(UINT64_MAX / 2);                 // absurdly large, must not index out of range
  CHECK_GT(h.Max(), 0u);
  CHECK_EQ(h.Count(), 2u);
}

RUN_ALL()

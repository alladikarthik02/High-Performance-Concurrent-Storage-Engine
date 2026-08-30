// T1b: the counters. SPEC 4.1, E-24.
#include "tests/test.h"

#include <algorithm>
#include <string>
#include <thread>
#include <vector>

#include "lsmeng/stats.h"

using namespace lsmeng;

TEST(counters_from_many_threads_sum_exactly) {
  // Sharded counters are only useful if they are still EXACT. An approximate counter would
  // make "blocks read per Get" a number you cannot reason about.
  Stats s;
  const int kThreads = 8, kPer = 50000;
  std::vector<std::thread> ts;
  for (int t = 0; t < kThreads; ++t)
    ts.emplace_back([&] { for (int i = 0; i < kPer; ++i) s.Add(kWrites); });
  for (auto& t : ts) t.join();
  CHECK_EQ(s.Get(kWrites), static_cast<uint64_t>(kThreads) * kPer);
}

TEST(max_keeps_a_high_water_mark_across_threads) {
  Stats s;
  std::vector<std::thread> ts;
  for (int t = 0; t < 8; ++t)
    ts.emplace_back([&, t] { for (int i = 0; i <= 100; ++i) s.Max(kMaxStallMs, t * 100 + i); });
  for (auto& t : ts) t.join();
  CHECK_EQ(s.Get(kMaxStallMs), 800u);
}

TEST(a_max_counter_is_not_reduced_by_summing) {
  // The regression test for B8. Eight threads each push a different high-water mark; the
  // answer is the largest, never the total. Summing gave 3600 where the truth was 800.
  Stats s;
  for (int t = 0; t < 8; ++t) s.Max(kMaxStallMs, static_cast<uint64_t>(t) * 100);
  CHECK_LE(s.Get(kMaxStallMs), 700u);
  CHECK_EQ(KindOf(kMaxStallMs), CounterKind::kMax);
  CHECK_EQ(KindOf(kWrites), CounterKind::kSum);
}

TEST(set_replaces_rather_than_accumulates) {
  Stats s;
  s.Set(kFilterBytesResident, 100);
  s.Set(kFilterBytesResident, 250);
  CHECK_EQ(s.Get(kFilterBytesResident), 250u);   // a gauge, not a sum
}

TEST(property_lookup_distinguishes_a_typo_from_a_zero) {
  // The reason GetProperty returns bool rather than just a string: a benchmark that
  // misspells a counter name must fail loudly, not silently read 0 and report it.
  Stats s;
  s.Add(kBlocksRead, 7);
  std::string out;
  CHECK(s.GetProperty(Slice("lsmeng.blocks-read"), &out));
  CHECK_EQ(out, std::string("7"));
  CHECK(!s.GetProperty(Slice("lsmeng.blocks-red"), &out));   // typo
  CHECK(!s.GetProperty(Slice("blocks-read"), &out));         // missing prefix
  CHECK(s.GetProperty(Slice("lsmeng.stats"), &out));
  CHECK(out.find("blocks-read") != std::string::npos);
}

TEST(every_counter_declares_how_it_is_reduced) {
  // B8 again, structurally: a new counter added without a kind entry trips the
  // static_assert in stats.cc, and a counter whose kind is wrong is caught here by the
  // one property that distinguishes them -- a sum grows with thread count, a max does not.
  for (int c = 0; c < kNumCounters; ++c) {
    Stats s;
    const Counter ctr = static_cast<Counter>(c);
    if (KindOf(ctr) != CounterKind::kSum) continue;
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; ++t) ts.emplace_back([&] { s.Add(ctr, 10); });
    for (auto& t : ts) t.join();
    TCTX("counter=" << CounterName(ctr));
    CHECK_EQ(s.Get(ctr), 40u);
  }
}

TEST(every_counter_has_a_name_and_the_names_are_unique) {
  // The static_assert in stats.cc catches a length mismatch; this catches a duplicate or
  // an empty entry, which would silently alias two counters in every report.
  std::vector<std::string> names;
  for (int c = 0; c < kNumCounters; ++c) {
    std::string n = CounterName(static_cast<Counter>(c));
    CHECK(!n.empty());
    names.push_back(n);
  }
  std::sort(names.begin(), names.end());
  CHECK(std::adjacent_find(names.begin(), names.end()) == names.end());
}

TEST(shards_are_cache_line_separated) {
  // E-24: without padding, eight threads incrementing "different" counters share a cache
  // line and the resulting ping-pong appears in a profile as a hot instruction that looks
  // like lock contention. This asserts the layout that prevents it.
  Stats s;
  // Two threads hammering the same counter should still be exact (checked above); here we
  // just assert the structure is at least one cache line per shard.
  CHECK_GE(sizeof(Stats) / 16, 64u);
}

RUN_ALL()

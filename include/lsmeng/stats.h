#pragma once
#include <atomic>
#include <cstdint>
#include <string>

#include "lsmeng/slice.h"

namespace lsmeng {

// SPEC 4.1 / T1b. The counters through which tests and benchmarks observe the engine.
//
// WHY THEY ARE PART OF THE CONTRACT, not a debug nicety: "p99 got worse" is a symptom;
// "data blocks read per Get went from 1.2 to 9" is a diagnosis. Every claim in SPEC 2 that
// is about behaviour rather than latency is measured through these.
//
// SHARDED, AND CACHE-LINE PADDED, ON PURPOSE (SPEC E-24). A single counter block
// incremented by every thread on every operation is textbook false sharing: the line
// ping-pongs between cores and shows up in a profile as a hot instruction that looks like
// real contention. Diagnosing that as a lock problem would send T12 chasing the wrong
// thing entirely. Sixteen padded shards, summed on read, makes the counters honest.

enum Counter : int {
  kWrites = 0,
  kWalSyncs,
  kBloomChecked,
  kBloomRejected,
  kBlocksRead,
  kFilterBlocksRead,
  kFilterBytesResident,
  kCacheHits,
  kCacheMisses,
  kMemtableBytes,
  kCompactions,
  kBytesCompacted,
  kPendingCompactionBytes,
  kStalls,
  kMaxStallMs,
  kOrphanFiles,
  kOrphanBytes,
  kNumCounters,
};

// Sharding is NOT a uniform transformation. It is correct for a SUM -- add anywhere, add
// the shards up -- and WRONG for a maximum, because summing eight per-shard maxima is not
// the maximum. Getting that wrong reported a 3600 ms stall where the real worst case was
// 800 ms, i.e. a tail-latency number inflated by 4.5x, in the exact metric R13 claims to
// improve. So how a counter is REDUCED is part of its declaration. See CHALLENGES B8.
enum class CounterKind : uint8_t {
  kSum,     // accumulate across shards
  kMax,     // high-water mark; reduce with max, not +
  kGauge,   // a current value, written to shard 0 only so the sum is still correct
};

const char* CounterName(Counter c);
CounterKind KindOf(Counter c);

class Stats {
 public:
  Stats() { Reset(); }

  void Add(Counter c, uint64_t n = 1) {
    // Relaxed: counters are observations, never a correctness boundary. Any stronger
    // ordering would put a barrier on the hot path to make a statistic slightly less
    // stale, which is a bad trade.
    shards_[ShardIndex()].v[c].fetch_add(n, std::memory_order_relaxed);
  }

  // For a high-water mark (kMaxStallMs) rather than a sum.
  void Max(Counter c, uint64_t n) {
    auto& a = shards_[ShardIndex()].v[c];
    uint64_t cur = a.load(std::memory_order_relaxed);
    while (n > cur && !a.compare_exchange_weak(cur, n, std::memory_order_relaxed)) {}
  }

  // A gauge is a current value, not an accumulation. It goes to shard 0 ONLY, so summing
  // the shards still yields it and there is nothing to reconcile.
  void Set(Counter c, uint64_t n) { shards_[0].v[c].store(n, std::memory_order_relaxed); }

  uint64_t Get(Counter c) const {
    if (KindOf(c) == CounterKind::kMax) {
      uint64_t best = 0;
      for (int s = 0; s < kShards; ++s) {
        const uint64_t v = shards_[s].v[c].load(std::memory_order_relaxed);
        if (v > best) best = v;
      }
      return best;
    }
    uint64_t total = 0;
    for (int s = 0; s < kShards; ++s) total += shards_[s].v[c].load(std::memory_order_relaxed);
    return total;
  }

  void Reset() {
    for (int s = 0; s < kShards; ++s)
      for (int c = 0; c < kNumCounters; ++c) shards_[s].v[c].store(0, std::memory_order_relaxed);
  }

  // "lsmeng.blocks-read" -> the value, as a decimal string. Returns false for an unknown
  // name, which is how DB::GetProperty distinguishes a typo from a zero.
  bool GetProperty(const Slice& name, std::string* out) const;

  // A multi-line human summary for "lsmeng.stats".
  std::string Summary() const;

 private:
  static constexpr int kShards = 16;
  static constexpr int kCacheLine = 64;

  struct alignas(kCacheLine) Shard {
    std::atomic<uint64_t> v[kNumCounters];
    // Pad the tail so the last counter of one shard cannot share a line with the first of
    // the next. alignas alone aligns the START; without this the END still collides.
    char pad[kCacheLine - ((sizeof(std::atomic<uint64_t>) * kNumCounters) % kCacheLine)];
  };

  static int ShardIndex() {
    // One stable index per thread, assigned on first use. A thread-id hash would work too,
    // but a monotonic counter guarantees the first 16 threads land on 16 distinct shards
    // instead of colliding by luck.
    static std::atomic<int> next{0};
    static thread_local int idx = next.fetch_add(1, std::memory_order_relaxed) % kShards;
    return idx;
  }

  Shard shards_[kShards];
};

}  // namespace lsmeng

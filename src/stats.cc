#include "lsmeng/stats.h"

#include <cstdio>

namespace lsmeng {

namespace {
// Order must match the Counter enum exactly. A mismatch would silently report one
// counter's value under another's name, so the static_assert below is the guard.
const char* kNames[] = {
    "writes", "wal-syncs", "bloom-checked", "bloom-rejected", "blocks-read",
    "filter-blocks-read", "filter-bytes-resident", "cache-hits", "cache-misses",
    "memtable-bytes", "compactions", "bytes-compacted", "pending-compaction-bytes",
    "stalls", "max-stall-ms", "orphan-files", "orphan-bytes",
};
static_assert(sizeof(kNames) / sizeof(kNames[0]) == kNumCounters,
              "kNames and the Counter enum have drifted apart");

// How each counter is reduced across shards. Order must match the Counter enum.
const CounterKind kKinds[] = {
    CounterKind::kSum,    // writes
    CounterKind::kSum,    // wal-syncs
    CounterKind::kSum,    // bloom-checked
    CounterKind::kSum,    // bloom-rejected
    CounterKind::kSum,    // blocks-read
    CounterKind::kSum,    // filter-blocks-read
    CounterKind::kGauge,  // filter-bytes-resident
    CounterKind::kSum,    // cache-hits
    CounterKind::kSum,    // cache-misses
    CounterKind::kGauge,  // memtable-bytes
    CounterKind::kSum,    // compactions
    CounterKind::kSum,    // bytes-compacted
    CounterKind::kGauge,  // pending-compaction-bytes
    CounterKind::kSum,    // stalls
    CounterKind::kMax,    // max-stall-ms   <-- the one that made B8 visible
    CounterKind::kGauge,  // orphan-files
    CounterKind::kGauge,  // orphan-bytes
};
static_assert(sizeof(kKinds) / sizeof(kKinds[0]) == kNumCounters,
              "kKinds and the Counter enum have drifted apart");
}  // namespace

const char* CounterName(Counter c) { return kNames[c]; }
CounterKind KindOf(Counter c) { return kKinds[c]; }

bool Stats::GetProperty(const Slice& name, std::string* out) const {
  const std::string kPrefix = "lsmeng.";
  const std::string n = name.ToString();
  if (n.compare(0, kPrefix.size(), kPrefix) != 0) return false;
  const std::string bare = n.substr(kPrefix.size());
  if (bare == "stats") { *out = Summary(); return true; }
  for (int c = 0; c < kNumCounters; ++c) {
    if (bare == kNames[c]) {
      *out = std::to_string(Get(static_cast<Counter>(c)));
      return true;
    }
  }
  return false;
}

std::string Stats::Summary() const {
  std::string s;
  char line[128];
  for (int c = 0; c < kNumCounters; ++c) {
    std::snprintf(line, sizeof(line), "%-26s %llu\n", kNames[c],
                  static_cast<unsigned long long>(Get(static_cast<Counter>(c))));
    s += line;
  }
  return s;
}

}  // namespace lsmeng

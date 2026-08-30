#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace lsmeng {

// SPEC 10.1. A logarithmic-bucket latency histogram.
//
// WHY NOT A MEAN. R13 claims a p99.9 improvement. A mean cannot express that at all: a
// workload where 999 operations take 10 us and one takes 200 ms has a mean of 210 us,
// which describes neither population. Storing every sample would be exact but unbounded;
// buckets give constant memory with bounded relative error, which is the standard trade
// and the one HdrHistogram makes.
//
// LAYOUT: values below 256 us get their own bucket (1 us resolution -- the region where
// nearly every operation lands). Above that, 256 sub-buckets per power of two, so relative
// error is at most 1/256 = 0.4% anywhere in the range. ~15k buckets, 117 KiB per
// histogram, which is why each thread can afford its own and merge at the end -- avoiding
// exactly the false sharing E-24 warns about.
class Histogram {
 public:
  Histogram() : counts_(kBuckets, 0) {}

  void Add(uint64_t micros) {
    ++counts_[BucketFor(micros)];
    ++count_;
    sum_ += micros;
    if (micros > max_) max_ = micros;
    if (count_ == 1 || micros < min_) min_ = micros;
  }

  void Merge(const Histogram& other) {
    for (size_t i = 0; i < counts_.size(); ++i) counts_[i] += other.counts_[i];
    count_ += other.count_;
    sum_ += other.sum_;
    if (other.count_ > 0) {
      max_ = std::max(max_, other.max_);
      min_ = (count_ == other.count_) ? other.min_ : std::min(min_, other.min_);
    }
  }

  uint64_t Count() const { return count_; }
  uint64_t Max() const { return max_; }
  uint64_t Min() const { return count_ ? min_ : 0; }
  double Mean() const { return count_ ? static_cast<double>(sum_) / count_ : 0.0; }

  // The value below which `p` of the population falls. Interpolation is deliberately NOT
  // done: with 0.4% buckets it would add precision the data does not have, and reporting a
  // bucket's lower bound is the conservative direction for a latency claim.
  uint64_t Percentile(double p) const {
    if (count_ == 0) return 0;
    const uint64_t target = static_cast<uint64_t>(p * static_cast<double>(count_) / 100.0);
    uint64_t seen = 0;
    for (size_t i = 0; i < counts_.size(); ++i) {
      seen += counts_[i];
      if (seen >= target) return ValueOf(i);
    }
    return max_;
  }

  std::string Report() const {
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "n=%llu  min=%llu  mean=%.1f  p50=%llu  p90=%llu  p99=%llu  "
                  "p99.9=%llu  max=%llu  (us)",
                  (unsigned long long)count_, (unsigned long long)Min(), Mean(),
                  (unsigned long long)Percentile(50), (unsigned long long)Percentile(90),
                  (unsigned long long)Percentile(99), (unsigned long long)Percentile(99.9),
                  (unsigned long long)max_);
    return buf;
  }

 private:
  static constexpr int kSubBits = 8;
  static constexpr uint64_t kSub = 1ull << kSubBits;   // 256
  static constexpr int kBuckets = static_cast<int>(kSub) * 57;

  static size_t BucketFor(uint64_t v) {
    if (v < kSub) return static_cast<size_t>(v);
    const int e = 63 - __builtin_clzll(v);             // floor(log2 v), >= 8
    const int shift = e - kSubBits;
    const uint64_t sub = (v >> shift) & (kSub - 1);
    const size_t idx = static_cast<size_t>((e - kSubBits + 1)) * kSub + sub;
    return idx < static_cast<size_t>(kBuckets) ? idx : static_cast<size_t>(kBuckets - 1);
  }

  // The inverse of BucketFor: the smallest value that lands in this bucket. Reporting the
  // lower bound rather than the midpoint keeps a latency figure conservative -- it can
  // understate by at most one bucket width (0.4%), never overstate.
  static uint64_t ValueOf(size_t bucket) {
    if (bucket < kSub) return bucket;
    const size_t octave = bucket / kSub;               // = e - kSubBits + 1
    const size_t sub = bucket % kSub;
    const int e = static_cast<int>(octave) + kSubBits - 1;
    const int shift = e - kSubBits;
    return (sub | kSub) << shift;
  }

  std::vector<uint64_t> counts_;
  uint64_t count_ = 0;
  uint64_t sum_ = 0;
  uint64_t max_ = 0;
  uint64_t min_ = 0;
};

}  // namespace lsmeng

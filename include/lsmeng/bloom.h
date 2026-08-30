#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "lsmeng/hash.h"
#include "lsmeng/slice.h"

namespace lsmeng {

// SPEC 3.6. One filter per SST file, answering "is this key definitely NOT in this file?"
//
// THE SAFETY ARGUMENT, in one sentence: the filter can be wrong in only one direction. A
// false positive costs one wasted block read -- exactly the situation we were in without
// a filter. A false NEGATIVE would return wrong data, and cannot happen, because a key
// that was added set every bit it probes. That asymmetry is the whole reason this is safe
// to trust on a read path.
//
// THREE RULES THAT ARE EASY TO GET WRONG, all of them silent:
//   1. TOMBSTONES GO IN THE FILTER. A Delete writes a kTypeDeletion entry and its key must
//      be added. Omitting it means the filter says "not present", the file holding the
//      tombstone is skipped, and an older value in a deeper tier RESURRECTS (SPEC E-2a).
//      The builder adds every entry regardless of type, and asserts it.
//   2. Nothing ever clears a bit. Bloom filters cannot support removal; deletion is
//      represented by a tombstone record, never by un-setting bits.
//   3. A missing or corrupt filter must mean "MAYBE present", never "not present"
//      (SPEC S8). Failing open is the only safe direction. See BloomFilter::MayContain
//      on an empty slice.

class BloomFilterBuilder {
 public:
  explicit BloomFilterBuilder(int bits_per_key) : bits_per_key_(bits_per_key) {
    // k = m * ln2, clamped. Below 1 the filter would probe nothing and answer "not
    // present" for everything -- a false-negative machine. Above 30 the cost per probe
    // dominates and the FPR stops improving.
    k_ = static_cast<int>(static_cast<double>(bits_per_key) * 0.69);
    if (k_ < 1) k_ = 1;
    if (k_ > 30) k_ = 30;
  }

  // Called with the USER key, never the internal key: the filter is queried with a user
  // key and must not depend on a sequence number. Adding internal keys would make every
  // lookup a guaranteed miss.
  void AddKey(const Slice& user_key) { hashes_.push_back(Hash64(user_key.data(), user_key.size())); }

  size_t num_keys() const { return hashes_.size(); }
  int k() const { return k_; }

  // Appends the filter block to `dst`: [ bit array ][ k : 1 byte ].
  // Storing k in the file means a file written with different settings still reads
  // correctly -- a reader never has to be configured to match its writer.
  void Finish(std::string* dst) {
    size_t bits = hashes_.size() * static_cast<size_t>(bits_per_key_);
    // SPEC E-3: n = 0 would give a zero-byte array, so `% nbits` divides by zero and the
    // probe reads out of bounds. A minimum of 64 bits, all zero, makes every probe hit a
    // clear bit and answer "not present" -- which is the CORRECT answer for an empty set.
    if (bits < 64) bits = 64;
    const size_t bytes = (bits + 7) / 8;
    bits = bytes * 8;

    const size_t init = dst->size();
    dst->resize(init + bytes, 0);
    char* array = &(*dst)[init];

    for (uint64_t h : hashes_) {
      // Kirsch-Mitzenmacher double hashing: one real hash, k derived probes, with an FPR
      // indistinguishable from k independent hashes at these parameters. h2 is forced odd
      // so the probe sequence cannot degenerate to a single bit when h2 shares a factor
      // with the array size.
      uint32_t h1 = static_cast<uint32_t>(h);
      uint32_t h2 = static_cast<uint32_t>(h >> 32) | 1u;
      for (int i = 0; i < k_; ++i) {
        const size_t bitpos = (static_cast<size_t>(h1) + static_cast<size_t>(i) * h2) % bits;
        array[bitpos / 8] |= static_cast<char>(1 << (bitpos % 8));
      }
    }
    dst->push_back(static_cast<char>(k_));
  }

  void Reset() { hashes_.clear(); }

 private:
  int bits_per_key_;
  int k_;
  std::vector<uint64_t> hashes_;
};

// Reads a filter block produced by the builder. Holds no ownership: the bytes belong to
// the SstReader that read them (SPEC 3.5 -- filters are pinned at table-open, not fetched
// through the block cache, because a whole-file filter is far larger than a block and
// would evict the data blocks it exists to protect).
class BloomFilter {
 public:
  BloomFilter() = default;
  explicit BloomFilter(const Slice& contents) : contents_(contents) {}

  bool MayContain(const Slice& user_key) const {
    const size_t n = contents_.size();
    // S8: fail OPEN. An absent filter block, a truncated one, or one whose CRC failed all
    // land here, and every one of them must answer "maybe" so the caller reads the file.
    // Answering "no" would silently lose data.
    if (n < 2) return true;

    const size_t bytes = n - 1;
    const int k = static_cast<uint8_t>(contents_[n - 1]);
    // k > 30 is not a filter we wrote. Treat it as a future format we do not understand
    // and fail open rather than guess.
    if (k < 1 || k > 30) return true;

    const size_t bits = bytes * 8;
    const uint64_t h = Hash64(user_key.data(), user_key.size());
    uint32_t h1 = static_cast<uint32_t>(h);
    uint32_t h2 = static_cast<uint32_t>(h >> 32) | 1u;
    const char* array = contents_.data();
    for (int i = 0; i < k; ++i) {
      const size_t bitpos = (static_cast<size_t>(h1) + static_cast<size_t>(i) * h2) % bits;
      if ((array[bitpos / 8] & (1 << (bitpos % 8))) == 0) return false;   // certain
    }
    return true;   // probable
  }

  bool empty() const { return contents_.size() < 2; }
  size_t size_bytes() const { return contents_.size(); }

 private:
  Slice contents_;
};

}  // namespace lsmeng

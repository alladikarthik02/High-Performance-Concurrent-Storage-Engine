#pragma once
#include <cstdint>
#include <string>

#include "lsmeng/coding.h"
#include "lsmeng/slice.h"

namespace lsmeng {

// SPEC 3.1. The internal key encoding and THE comparator.
//
// S10: exactly one comparator implementation exists, and the memtable, the block builder,
// the merging iterator and compaction all use it. If any two of them disagreed, the
// database would return WRONG ANSWERS rather than crash -- the worst failure mode
// available -- which is why this lives in one header and is brute-forced for strict weak
// ordering in test_dbformat.

using SequenceNumber = uint64_t;

// 56 bits of sequence, 8 bits of type. Sequence numbers are allocated from 1: zero is
// reserved so that an accidentally-zero oldest_snapshot_seq is detectable rather than
// silently turning compaction into a no-op that reclaims nothing while every correctness
// test still passes (SPEC E-33).
constexpr SequenceNumber kMaxSequenceNumber = (1ull << 56) - 1;

enum ValueType : unsigned char {
  kTypeDeletion = 0x00,
  kTypeValue = 0x01,
};

// For a Seek, we want the NEWEST version of a user key at or below a snapshot. Because
// equal user keys sort by descending packed value, the largest possible packed value for
// a given sequence is the seek target -- hence kValueTypeForSeek is the LARGER type.
constexpr ValueType kValueTypeForSeek = kTypeValue;

inline uint64_t PackSequenceAndType(SequenceNumber seq, ValueType t) {
  return (seq << 8) | static_cast<uint8_t>(t);
}

struct ParsedInternalKey {
  Slice user_key;
  SequenceNumber sequence = 0;
  ValueType type = kTypeValue;
};

inline void AppendInternalKey(std::string* result, const ParsedInternalKey& k) {
  result->append(k.user_key.data(), k.user_key.size());
  PutFixed64(result, PackSequenceAndType(k.sequence, k.type));
}

// E-17: an internal key is split by LENGTH, never by scanning for a delimiter. A user key
// may end in bytes that look exactly like a packed sequence number, and any parser that
// searched for a separator would mis-split it.
inline Slice ExtractUserKey(const Slice& internal_key) {
  return Slice(internal_key.data(), internal_key.size() - 8);
}

inline bool ParseInternalKey(const Slice& internal_key, ParsedInternalKey* out) {
  const size_t n = internal_key.size();
  if (n < 8) return false;   // cannot be a valid internal key; caller reports Corruption
  const uint64_t packed = DecodeFixed64(internal_key.data() + n - 8);
  const unsigned char c = static_cast<unsigned char>(packed & 0xFF);
  if (c > static_cast<unsigned char>(kTypeValue)) return false;
  out->user_key = Slice(internal_key.data(), n - 8);
  out->sequence = packed >> 8;
  out->type = static_cast<ValueType>(c);
  return true;
}

// THE comparator (SPEC 3.1).
//
//   user keys ascending; for the SAME user key, larger packed value FIRST.
//
// Two consequences worth being able to say out loud:
//   1. All versions of one user key are adjacent, newest first. So a Get is a single Seek
//      plus "take the first entry whose user key matches" -- no backwards scan.
//   2. For an identical sequence, kTypeValue would sort before kTypeDeletion. That case
//      CANNOT arise, because sequence numbers are unique per mutation. The code must not
//      depend on the accident, so the test suite asserts uniqueness rather than assuming
//      it (S9).
struct InternalKeyComparator {
  int operator()(const Slice& a, const Slice& b) const { return Compare(a, b); }

  static int Compare(const Slice& a, const Slice& b) {
    int r = ExtractUserKey(a).compare(ExtractUserKey(b));
    if (r != 0) return r;
    const uint64_t an = DecodeFixed64(a.data() + a.size() - 8);
    const uint64_t bn = DecodeFixed64(b.data() + b.size() - 8);
    // Descending. Written as two comparisons rather than (bn - an) because the subtraction
    // of two uint64 sequence numbers overflows an int and is a classic sorting bug.
    if (an > bn) return -1;
    if (an < bn) return +1;
    return 0;
  }
};

// The three encodings of a lookup, built once. Get needs the memtable form (length-
// prefixed) and the SST form (bare internal key) from the same buffer; building them
// separately is how they drift apart.
class LookupKey {
 public:
  LookupKey(const Slice& user_key, SequenceNumber seq) {
    const size_t usize = user_key.size();
    const size_t needed = usize + 13;   // varint32 (<=5) + key + 8
    char* dst = (needed <= sizeof(space_)) ? space_ : new char[needed];
    start_ = dst;
    dst = EncodeVarint32(dst, static_cast<uint32_t>(usize + 8));
    kstart_ = dst;
    std::memcpy(dst, user_key.data(), usize);
    dst += usize;
    EncodeFixed64(dst, PackSequenceAndType(seq, kValueTypeForSeek));
    dst += 8;
    end_ = dst;
  }
  ~LookupKey() { if (start_ != space_) delete[] start_; }
  LookupKey(const LookupKey&) = delete;
  LookupKey& operator=(const LookupKey&) = delete;

  Slice memtable_key() const { return Slice(start_, static_cast<size_t>(end_ - start_)); }
  Slice internal_key() const { return Slice(kstart_, static_cast<size_t>(end_ - kstart_)); }
  Slice user_key() const { return Slice(kstart_, static_cast<size_t>(end_ - kstart_) - 8); }

 private:
  char* start_;
  const char* kstart_;
  const char* end_;
  char space_[200];   // avoids a heap allocation for the overwhelmingly common short key
};

}  // namespace lsmeng

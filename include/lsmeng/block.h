#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "lsmeng/dbformat.h"
#include "lsmeng/iterator.h"
#include "lsmeng/slice.h"

namespace lsmeng {

// SPEC 3.5. A data or index block, with prefix compression and restart points.
//
// WHY PREFIX COMPRESSION. Keys in an SST are sorted, so adjacent keys usually share a
// prefix (`user:1001:name`, `user:1001:email`). Storing the shared part once is nearly
// free to implement and materially shrinks the file.
//
//   entry := [ shared : varint ][ non_shared : varint ][ vlen : varint ]
//            [ key_delta : non_shared bytes ][ value : vlen bytes ]
//   ...
//   [ restart[0] : u32 ] ... [ restart[R-1] : u32 ][ R : u32 ]
//
// WHY RESTART POINTS. Prefix compression alone would force every lookup to decode the
// block from byte zero, because entry N's key is only defined relative to entry N-1. Every
// Nth entry (default 16) instead stores its key in FULL, and the trailing array of their
// offsets makes the block BINARY SEARCHABLE: find the last restart whose key <= target,
// then scan at most 16 entries.
//
// THE INVARIANT THAT IS EASY TO BREAK SILENTLY (SPEC E-8): an entry may only share a
// prefix with the IMMEDIATELY PRECEDING entry, and the first entry after a restart must
// set shared = 0. Get this wrong and the block reads back with subtly wrong keys and NO
// CHECKSUM FAILURE, because the bytes are internally consistent. That is why T5's test
// compares a full scan against the exact input vector rather than spot-checking lookups.

class BlockBuilder {
 public:
  explicit BlockBuilder(int restart_interval) : restart_interval_(restart_interval) { Reset(); }

  void Add(const Slice& key, const Slice& value);
  Slice Finish();
  void Reset();

  bool empty() const { return buffer_.empty(); }
  size_t CurrentSizeEstimate() const {
    return buffer_.size() + restarts_.size() * sizeof(uint32_t) + sizeof(uint32_t);
  }
  const std::string& last_key() const { return last_key_; }

 private:
  const int restart_interval_;
  std::string buffer_;
  std::vector<uint32_t> restarts_;
  int counter_ = 0;
  bool finished_ = false;
  std::string last_key_;
};

// Reads a block. Does NOT own the bytes: they belong to the block cache entry or to the
// caller's buffer, and outlive the iterator by contract.
class Block {
 public:
  explicit Block(const Slice& contents);

  bool ok() const { return !corrupt_; }
  size_t size() const { return data_.size(); }
  uint32_t NumRestarts() const { return num_restarts_; }

  Iterator* NewIterator() const;

 private:
  class Iter;
  Slice data_;
  uint32_t restart_offset_ = 0;
  uint32_t num_restarts_ = 0;
  bool corrupt_ = false;
};

}  // namespace lsmeng

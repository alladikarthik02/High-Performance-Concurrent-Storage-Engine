#pragma once
#include <cstdint>
#include <memory>
#include <string>

#include "lsmeng/block.h"
#include "lsmeng/bloom.h"
#include "lsmeng/cache.h"
#include "lsmeng/env.h"
#include "lsmeng/iterator.h"
#include "lsmeng/options.h"
#include "lsmeng/stats.h"

namespace lsmeng {

// SPEC 3.5. The sorted string table: written once, sequentially, never modified.
//
//   +-------------------------------+
//   |  data block 0 ...             |   ~4 KiB target each
//   +-------------------------------+
//   |  bloom filter block           |   one filter for the whole file
//   +-------------------------------+
//   |  index block                  |   one entry per data block
//   +-------------------------------+
//   |  footer (48 bytes, fixed)     |
//   +-------------------------------+
//
// Every block carries a 5-byte trailer: [ compression_type : 1 ][ crc32c : 4 ], with the
// CRC computed over (contents || type). Including the type byte prevents a "valid block,
// wrong interpretation" case where a flipped type byte routes good bytes into a
// decompressor (S7).
//
// The footer is FIXED SIZE with the magic LAST, so it can be read by seeking to
// file_size - 48, and so an interrupted write cannot produce a footer that both parses and
// points at garbage.

constexpr size_t kFooterSize = 48;
constexpr size_t kBlockTrailerSize = 5;
constexpr uint64_t kSstMagic = 0x6C736D656E673031ull;   // "lsmeng01"
constexpr uint8_t kNoCompression = 0x00;

struct BlockHandle {
  uint64_t offset = 0;
  uint64_t size = 0;
  void EncodeTo(std::string* dst) const;
  Status DecodeFrom(Slice* input);
  bool empty() const { return size == 0; }
};

struct Footer {
  BlockHandle bloom_handle;
  BlockHandle index_handle;
  void EncodeTo(std::string* dst) const;
  Status DecodeFrom(const Slice& input);
};

// Per-file metadata that lives in the MANIFEST, not in the SST -- because the version set
// needs "can this file possibly contain key k?" answered WITHOUT opening the file at all
// (SPEC 3.5). It is the cheapest read-path filter and runs before the Bloom filter.
struct SstInfo {
  std::string smallest_key;   // internal keys
  std::string largest_key;
  uint64_t num_entries = 0;
  uint64_t num_deletions = 0;
  uint64_t file_size = 0;
};

class SstBuilder {
 public:
  SstBuilder(const Options& options, WritableFile* file);
  ~SstBuilder();

  // Keys must arrive in ascending internal-key order.
  void Add(const Slice& internal_key, const Slice& value);
  Status Finish();
  void Abandon() { abandoned_ = true; }

  uint64_t NumEntries() const { return info_.num_entries; }
  uint64_t FileSize() const { return offset_; }
  const SstInfo& info() const { return info_; }
  Status status() const { return status_; }

 private:
  void WriteBlock(BlockBuilder* b, BlockHandle* handle);
  void WriteRawBlock(const Slice& contents, BlockHandle* handle);
  void FlushDataBlock();

  const Options options_;
  WritableFile* const file_;
  uint64_t offset_ = 0;
  Status status_;
  BlockBuilder data_block_;
  BlockBuilder index_block_;
  BloomFilterBuilder filter_;
  std::string last_key_;
  SstInfo info_;
  bool finished_ = false;
  bool abandoned_ = false;
  // An index entry is only emitted once the FOLLOWING block's first key is known, so the
  // separator can be short and still satisfy last_key(i) <= S < first_key(i+1).
  bool pending_index_entry_ = false;
  BlockHandle pending_handle_;
};

class SstReader {
 public:
  // Opens the file and reads footer, index, and filter. The FILTER IS READ HERE, ONCE, and
  // held for this reader's lifetime -- not fetched through the block cache, because a
  // whole-file filter is far larger than a block and would evict the data blocks it exists
  // to protect (SPEC 3.5, E-34).
  // `block_cache` and `file_number` may be null/0, in which case every block read goes to
  // disk. That is the configuration the T12 experiment turns off to measure what the cache
  // is worth.
  static Status Open(const Options& options, std::unique_ptr<RandomAccessFile> file,
                     uint64_t file_size, Stats* stats, std::unique_ptr<SstReader>* out,
                     Cache* block_cache = nullptr, uint64_t file_number = 0);

  // Returns NotFound if the key is absent. `stats` records bloom-checked/bloom-rejected
  // and blocks-read, which is how R6's "measured reduction in data blocks read" is
  // obtained.
  Status Get(const ReadOptions& options, const Slice& internal_key, std::string* value,
             bool* found_tombstone) const;

  Iterator* NewIterator(const ReadOptions& options) const;

  // Bytes of resident filter, for S11's byte-bounded table cache.
  size_t FilterMemoryBytes() const { return filter_data_.size(); }
  const Footer& footer() const { return footer_; }
  uint32_t NumDataBlocks() const;

 private:
  SstReader(const Options& o, std::unique_ptr<RandomAccessFile> f, Stats* s, Cache* c,
            uint64_t n)
      : options_(o), file_(std::move(f)), stats_(s), block_cache_(c), file_number_(n) {}

  Status ReadBlock(const BlockHandle& handle, bool verify, std::string* out) const;
  Iterator* BlockIterator(const Slice& index_value) const;

  const Options options_;
  std::unique_ptr<RandomAccessFile> file_;
  Stats* stats_;
  Cache* block_cache_ = nullptr;
  uint64_t file_number_ = 0;
  Footer footer_;
  std::string index_data_;
  std::string filter_data_;
  std::unique_ptr<Block> index_block_;
  BloomFilter filter_;
};

}  // namespace lsmeng

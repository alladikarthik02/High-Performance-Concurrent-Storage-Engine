#pragma once
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "lsmeng/dbformat.h"
#include "lsmeng/slice.h"
#include "lsmeng/status.h"

namespace lsmeng {

// SPEC 3.9. Per-file metadata lives HERE, in the manifest -- not inside the SST -- because
// the version set must answer "can this file possibly contain key k?" WITHOUT opening the
// file at all. It is the cheapest read-path filter and runs before the Bloom filter.
struct FileMetaData {
  uint64_t number = 0;
  uint64_t file_size = 0;
  std::string smallest;      // smallest internal key in the file
  std::string largest;       // largest internal key
  uint64_t num_entries = 0;
  uint64_t num_deletions = 0;
  int refs = 0;              // in-memory only; never serialised

  Slice smallest_user_key() const { return ExtractUserKey(Slice(smallest)); }
  Slice largest_user_key() const { return ExtractUserKey(Slice(largest)); }
};

// A delta on the file set. The MANIFEST is an append-only log of these, using the EXACT
// SAME record framing as the WAL (SPEC 3.3) -- so the manifest's crash behaviour is tested
// by the WAL's tests rather than by a second, separately-trusted implementation.
class VersionEdit {
 public:
  void Clear();

  void SetComparatorName(const Slice& name) { has_comparator_ = true; comparator_ = name.ToString(); }
  void SetLogNumber(uint64_t n) { has_log_number_ = true; log_number_ = n; }
  // SPEC 3.3.1 -- the field SPEC v1 did not have. From the moment a memtable is frozen
  // until its flush has a durable manifest edit, TWO logs hold acknowledged data. Without
  // prev_log_number, recovery replays one and silently discards the other.
  void SetPrevLogNumber(uint64_t n) { has_prev_log_number_ = true; prev_log_number_ = n; }
  void SetNextFile(uint64_t n) { has_next_file_number_ = true; next_file_number_ = n; }
  void SetLastSequence(SequenceNumber s) { has_last_sequence_ = true; last_sequence_ = s; }

  void AddFile(int tier, const FileMetaData& f) { new_files_.emplace_back(tier, f); }
  void DeleteFile(int tier, uint64_t number) { deleted_files_.insert({tier, number}); }

  void EncodeTo(std::string* dst) const;
  Status DecodeFrom(const Slice& src);
  std::string DebugString() const;

  bool has_comparator_ = false;
  bool has_log_number_ = false;
  bool has_prev_log_number_ = false;
  bool has_next_file_number_ = false;
  bool has_last_sequence_ = false;

  std::string comparator_;
  uint64_t log_number_ = 0;
  uint64_t prev_log_number_ = 0;
  uint64_t next_file_number_ = 0;
  SequenceNumber last_sequence_ = 0;

  std::set<std::pair<int, uint64_t>> deleted_files_;
  std::vector<std::pair<int, FileMetaData>> new_files_;
};

}  // namespace lsmeng

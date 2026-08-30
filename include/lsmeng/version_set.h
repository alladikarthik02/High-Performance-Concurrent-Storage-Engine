#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "lsmeng/env.h"
#include "lsmeng/options.h"
#include "lsmeng/table_cache.h"
#include "lsmeng/tracked_mutex.h"
#include "lsmeng/version_edit.h"
#include "lsmeng/wal.h"

namespace lsmeng {

class VersionSet;

// SPEC 3.9. An IMMUTABLE, refcounted snapshot of the file set.
//
// THE SAFETY ARGUMENT FOR THE WHOLE CONCURRENT READ PATH, in two sentences: a reader takes
// version->Ref() under db_mutex_ and then does all its I/O with no lock; a compaction
// installs a NEW Version and the old one lives until its refcount hits zero. Because files
// are immutable and unlinking an open fd on POSIX leaves the fd usable, even a badly-timed
// unlink cannot corrupt an in-flight read (S14).
class Version {
 public:
  void Ref() { ++refs_; }
  void Unref();

  int NumTiers() const { return static_cast<int>(files_.size()); }
  const std::vector<FileMetaData*>& FilesAtTier(int tier) const { return files_[tier]; }
  int NumFilesAtTier(int tier) const {
    return tier < NumTiers() ? static_cast<int>(files_[tier].size()) : 0;
  }
  uint64_t TotalBytes() const;

  // SPEC 3.7 step 5. Tiers shallow to deep; within a tier, newest file number first.
  //
  // The recency argument, which is load-bearing and easy to get wrong (E-1): file number
  // is a valid recency key only WITHIN a tier, because a compaction's output gets a higher
  // number than files holding newer data in shallower tiers. Across tiers the guarantee is
  // the SPEC 3.8.1 invariant -- the shallowest tier containing a key holds its newest
  // version -- which holds only because compaction inputs are an oldest-first prefix.
  Status Get(const ReadOptions& options, const LookupKey& key, std::string* value,
             TableCache* table_cache, Stats* stats);

  // Every file that could contain `user_key`, in the search order above.
  void ForEachOverlapping(const Slice& user_key,
                          const std::function<bool(int tier, FileMetaData*)>& fn) const;

  void AddIterators(const ReadOptions& options, TableCache* table_cache,
                    std::vector<Iterator*>* out) const;

  std::string DebugString() const;

 private:
  friend class VersionSet;
  friend class VersionBuilder;
  explicit Version(VersionSet* vset) : vset_(vset) {}
  ~Version();

  VersionSet* vset_;
  Version* next_ = nullptr;
  Version* prev_ = nullptr;
  int refs_ = 0;
  std::vector<std::vector<FileMetaData*>> files_;
};

class VersionSet {
 public:
  VersionSet(const std::string& dbname, const Options& options, TableCache* table_cache,
             Stats* stats);
  ~VersionSet();

  // Recover from CURRENT + MANIFEST. Reports why on failure, naming the file (E-18).
  Status Recover();

  // Create a brand-new database: MANIFEST-1 with a snapshot of the empty version, then
  // CURRENT pointing at it.
  Status CreateNew();

  // Append an edit to the MANIFEST, fsync it, and only then install the new Version
  // (SPEC 3.9 -- data file -> fsync -> manifest edit -> fsync -> in-memory install).
  // Never the reverse: recording a file before it is durable produces a manifest that
  // references a file which does not exist, which is unrecoverable rather than untidy.
  //
  // `mu`, when non-null, is db_mutex_ HELD BY THE CALLER. It is RELEASED across the append
  // and the fsync and re-acquired before the install -- because S13 forbids holding it
  // across I/O, and a manifest fsync is exactly the kind of multi-millisecond stall that
  // would block every queued writer and every Get. Releasing it is safe only because
  // exactly one thread is ever inside this function (SPEC 3.8.4's single background
  // thread), which is asserted rather than assumed (S23). See CHALLENGES B14.
  Status LogAndApply(VersionEdit* edit, TrackedMutex* mu);

  Version* current() const { return current_; }

  uint64_t NewFileNumber() { return next_file_number_++; }
  uint64_t ManifestFileNumber() const { return manifest_file_number_; }
  uint64_t LogNumber() const { return log_number_; }
  uint64_t PrevLogNumber() const { return prev_log_number_; }
  SequenceNumber LastSequence() const { return last_sequence_; }
  void SetLastSequence(SequenceNumber s) { last_sequence_ = s; }
  void SetLogNumber(uint64_t n) { log_number_ = n; }
  void SetPrevLogNumber(uint64_t n) { prev_log_number_ = n; }
  void MarkFileNumberUsed(uint64_t n) { if (next_file_number_ <= n) next_file_number_ = n + 1; }

  // SPEC 3.3.1. Every log with number >= this is live and must be replayed and retained.
  uint64_t MinLiveLog() const {
    return prev_log_number_ != 0 ? std::min(log_number_, prev_log_number_) : log_number_;
  }

  // Files referenced by ANY live version -- the set that must not be unlinked (S14).
  void AddLiveFiles(std::set<uint64_t>* live) const;

  // The comparator name recorded in the manifest, so a database written by an
  // incompatible build is refused rather than silently misread.
  static const char* kComparatorName;

 private:
  friend class Version;
  void AppendVersion(Version* v);
  Status WriteSnapshot(WalWriter* writer);

  Env* const env_;
  const std::string dbname_;
  const Options options_;
  TableCache* const table_cache_;
  Stats* const stats_;

  uint64_t next_file_number_ = 2;   // 1 is the first manifest
  uint64_t manifest_file_number_ = 0;
  uint64_t log_number_ = 0;
  uint64_t prev_log_number_ = 0;
  SequenceNumber last_sequence_ = 0;   // sequences are allocated from 1 (E-33)

  std::unique_ptr<WritableFile> manifest_file_;
  std::unique_ptr<WalWriter> manifest_log_;

  Version dummy_versions_{this};   // head of a circular doubly-linked list of live versions
  Version* current_ = nullptr;

  // S23: exactly one thread appends to the MANIFEST, so on-disk record order equals
  // in-memory install order by construction. Asserted, not assumed.
  std::atomic<bool> appending_{false};
};

}  // namespace lsmeng

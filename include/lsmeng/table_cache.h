#pragma once
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "lsmeng/cache.h"
#include "lsmeng/env.h"
#include "lsmeng/options.h"
#include "lsmeng/sst.h"
#include "lsmeng/stats.h"

namespace lsmeng {

// SPEC 3.11 / S11 / E-26 / E-34. Open SstReaders, bounded by TWO limits.
//
// WHY TWO. One fd per open SST means a long run hits `ulimit -n` (E-26), so a COUNT bound
// is needed. But each reader also pins its whole-file Bloom filter in memory (SPEC 3.5),
// and a deep-tier file holding ~10^6 keys carries a ~1.25 MiB filter -- T2 measured 1.25
// bytes/key. So `max_open_files` alone does not bound memory at all: 500 deep-tier readers
// would pin over 600 MiB of filters while the count limit reports everything is fine
// (E-34). Whichever of `max_open_files` or `filter_memory_bytes` binds first wins.
//
// This is one of the two places SPEC v1 was wrong about resource bounds, and the fix is
// visible here rather than buried in a comment.

class TableCache {
 public:
  TableCache(const std::string& dbname, const Options& options, Cache* block_cache,
             Stats* stats);
  ~TableCache();

  // Opens the file if it is not already cached. `*reader` is valid until Release().
  Status Get(uint64_t file_number, uint64_t file_size, SstReader** reader,
             Cache::Handle** handle);
  void Release(Cache::Handle* handle);

  // Convenience: a Get through the table cache, releasing the handle before returning.
  Status Lookup(const ReadOptions& options, uint64_t file_number, uint64_t file_size,
                const Slice& internal_key, std::string* value, bool* found_tombstone);

  // An iterator over the whole file, which keeps the reader alive for its lifetime.
  Iterator* NewIterator(const ReadOptions& options, uint64_t file_number, uint64_t file_size);

  void Evict(uint64_t file_number);

  size_t ResidentFilterBytes() const;
  size_t NumOpen() const { return cache_.NumEntries(); }

 private:
  const std::string dbname_;
  const Options options_;
  Cache* const block_cache_;
  Stats* const stats_;
  Cache cache_;   // file_number -> SstReader
};

std::string SstFileName(const std::string& dbname, uint64_t number);
std::string LogFileName(const std::string& dbname, uint64_t number);
std::string ManifestFileName(const std::string& dbname, uint64_t number);
std::string CurrentFileName(const std::string& dbname);
std::string LockFileName(const std::string& dbname);
std::string TempFileName(const std::string& dbname, uint64_t number);
// Parses "000123.sst" etc. Returns false for anything it does not recognise, which is how
// orphan reporting (E-6) tells a stray file from one of ours.
bool ParseFileName(const std::string& filename, uint64_t* number, std::string* kind);

}  // namespace lsmeng

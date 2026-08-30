#include "lsmeng/table_cache.h"

#include <cstdio>
#include <cstdlib>

#include "lsmeng/coding.h"

namespace lsmeng {

namespace {
std::string NumberedFile(const std::string& dbname, uint64_t number, const char* suffix) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "/%06llu%s", static_cast<unsigned long long>(number), suffix);
  return dbname + buf;
}

struct CachedReader {
  std::unique_ptr<SstReader> reader;
};
void DeleteCachedReader(void* v) { delete static_cast<CachedReader*>(v); }
}  // namespace

std::string SstFileName(const std::string& d, uint64_t n) { return NumberedFile(d, n, ".sst"); }
std::string LogFileName(const std::string& d, uint64_t n) { return NumberedFile(d, n, ".log"); }
std::string TempFileName(const std::string& d, uint64_t n) { return NumberedFile(d, n, ".tmp"); }
std::string CurrentFileName(const std::string& d) { return d + "/CURRENT"; }
std::string LockFileName(const std::string& d) { return d + "/LOCK"; }
std::string ManifestFileName(const std::string& d, uint64_t n) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "/MANIFEST-%06llu", static_cast<unsigned long long>(n));
  return d + buf;
}

bool ParseFileName(const std::string& filename, uint64_t* number, std::string* kind) {
  if (filename == "CURRENT" || filename == "LOCK" || filename == "LOG") {
    *number = 0;
    *kind = filename;
    return true;
  }
  if (filename.rfind("MANIFEST-", 0) == 0) {
    const std::string digits = filename.substr(9);
    if (digits.empty()) return false;
    for (char c : digits) if (c < '0' || c > '9') return false;
    *number = std::strtoull(digits.c_str(), nullptr, 10);
    *kind = "MANIFEST";
    return true;
  }
  const size_t dot = filename.rfind('.');
  if (dot == std::string::npos || dot == 0) return false;
  const std::string digits = filename.substr(0, dot);
  const std::string suffix = filename.substr(dot + 1);
  if (suffix != "sst" && suffix != "log" && suffix != "tmp") return false;
  for (char c : digits) if (c < '0' || c > '9') return false;
  *number = std::strtoull(digits.c_str(), nullptr, 10);
  *kind = suffix;
  return true;
}

TableCache::TableCache(const std::string& dbname, const Options& options, Cache* block_cache,
                       Stats* stats)
    : dbname_(dbname),
      options_(options),
      block_cache_(block_cache),
      stats_(stats),
      // The cache is charged in BYTES, and each reader is charged its resident filter size
      // plus a fixed per-file cost. Making the per-file cost
      // filter_memory_bytes/max_open_files means a file with no filter still consumes one
      // "slot", so the count limit is enforced through the same mechanism as the byte
      // limit rather than as a second, separately-drifting counter.
      cache_(options.filter_memory_bytes, options.cache_shards) {}

TableCache::~TableCache() = default;

Status TableCache::Get(uint64_t file_number, uint64_t file_size, SstReader** reader,
                       Cache::Handle** handle) {
  std::string key;
  PutFixed64(&key, file_number);

  if (Cache::Handle* h = cache_.Lookup(Slice(key))) {
    *handle = h;
    *reader = static_cast<CachedReader*>(cache_.Value(h))->reader.get();
    return Status::OK();
  }

  // MISS: open outside any cache lock (SPEC 3.10, L3 -- same rule as the block cache).
  Env* env = options_.env ? options_.env : Env::Default();
  const std::string fname = SstFileName(dbname_, file_number);
  std::unique_ptr<RandomAccessFile> file;
  Status s = env->NewRandomAccessFile(fname, &file);
  if (!s.ok()) return s;

  std::unique_ptr<SstReader> r;
  s = SstReader::Open(options_, std::move(file), file_size, stats_, &r, block_cache_,
                      file_number);
  if (!s.ok()) return s;

  auto* cr = new CachedReader();
  const size_t charge =
      r->FilterMemoryBytes() +
      (options_.max_open_files > 0
           ? options_.filter_memory_bytes / static_cast<size_t>(options_.max_open_files)
           : 0);
  cr->reader = std::move(r);
  *reader = cr->reader.get();
  *handle = cache_.Insert(Slice(key), cr, charge, &DeleteCachedReader);
  if (stats_) stats_->Set(kFilterBytesResident, ResidentFilterBytes());
  return Status::OK();
}

void TableCache::Release(Cache::Handle* handle) { if (handle) cache_.Release(handle); }

void TableCache::Evict(uint64_t file_number) {
  std::string key;
  PutFixed64(&key, file_number);
  cache_.Erase(Slice(key));
}

size_t TableCache::ResidentFilterBytes() const { return cache_.TotalCharge(); }

Status TableCache::Lookup(const ReadOptions& options, uint64_t file_number,
                          uint64_t file_size, const Slice& internal_key, std::string* value,
                          bool* found_tombstone) {
  SstReader* r = nullptr;
  Cache::Handle* h = nullptr;
  Status s = Get(file_number, file_size, &r, &h);
  if (!s.ok()) return s;
  s = r->Get(options, internal_key, value, found_tombstone);
  Release(h);
  return s;
}

Iterator* TableCache::NewIterator(const ReadOptions& options, uint64_t file_number,
                                  uint64_t file_size) {
  SstReader* r = nullptr;
  Cache::Handle* h = nullptr;
  Status s = Get(file_number, file_size, &r, &h);
  if (!s.ok()) return NewErrorIterator(s);

  // The handle must outlive the iterator, or the reader can be evicted mid-scan and the
  // iterator is left pointing at a freed SstReader. This wrapper is that guarantee.
  class Holding final : public Iterator {
   public:
    Holding(Iterator* inner, TableCache* tc, Cache::Handle* h)
        : inner_(inner), tc_(tc), h_(h) {}
    ~Holding() override { delete inner_; tc_->Release(h_); }
    bool Valid() const override { return inner_->Valid(); }
    void SeekToFirst() override { inner_->SeekToFirst(); }
    void SeekToLast() override { inner_->SeekToLast(); }
    void Seek(const Slice& t) override { inner_->Seek(t); }
    void Next() override { inner_->Next(); }
    void Prev() override { inner_->Prev(); }
    Slice key() const override { return inner_->key(); }
    Slice value() const override { return inner_->value(); }
    Status status() const override { return inner_->status(); }

   private:
    Iterator* inner_;
    TableCache* tc_;
    Cache::Handle* h_;
  };
  return new Holding(r->NewIterator(options), this, h);
}

}  // namespace lsmeng

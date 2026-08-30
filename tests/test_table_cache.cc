// T6b: the table cache. SPEC 3.11, S11, E-26, E-34.
#include "tests/test.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "lsmeng/dbformat.h"
#include "lsmeng/table_cache.h"
#include "tests/tmpdir.h"

using namespace lsmeng;

namespace {
std::string IKey(const std::string& u, SequenceNumber s, ValueType t = kTypeValue) {
  std::string out;
  AppendInternalKey(&out, ParsedInternalKey{Slice(u), s, t});
  return out;
}

// Write file `number` containing `n` keys, returning its size.
uint64_t MakeFile(Env* env, const std::string& dbname, uint64_t number, int n,
                  const Options& opt, int key_base = 0) {
  std::vector<std::pair<std::string, std::string>> kvs;
  for (int i = 0; i < n; ++i) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "key%08d", key_base + i);
    kvs.emplace_back(IKey(buf, static_cast<SequenceNumber>(i + 1)), "value" + std::to_string(i));
  }
  std::sort(kvs.begin(), kvs.end(), [](const auto& a, const auto& b) {
    return InternalKeyComparator::Compare(Slice(a.first), Slice(b.first)) < 0;
  });
  std::unique_ptr<WritableFile> f;
  env->NewWritableFile(SstFileName(dbname, number), &f);
  SstBuilder b(opt, f.get());
  for (const auto& kv : kvs) b.Add(Slice(kv.first), Slice(kv.second));
  b.Finish();
  f->Sync();
  f->Close();
  uint64_t size = 0;
  env->GetFileSize(SstFileName(dbname, number), &size);
  return size;
}
}  // namespace

TEST(file_name_parsing_round_trips_and_rejects_strangers) {
  // E-6: orphan REPORTING has to tell one of our files from a stray. A parser that
  // accepted anything would report the user's notes.txt as a database file, and a parser
  // that rejected our own files would report live SSTs as orphans.
  uint64_t n = 0;
  std::string kind;
  CHECK(ParseFileName("000123.sst", &n, &kind)); CHECK_EQ(n, 123u); CHECK_EQ(kind, std::string("sst"));
  CHECK(ParseFileName("000007.log", &n, &kind)); CHECK_EQ(n, 7u);   CHECK_EQ(kind, std::string("log"));
  CHECK(ParseFileName("MANIFEST-000042", &n, &kind)); CHECK_EQ(n, 42u); CHECK_EQ(kind, std::string("MANIFEST"));
  CHECK(ParseFileName("CURRENT", &n, &kind)); CHECK_EQ(kind, std::string("CURRENT"));
  CHECK(ParseFileName("LOCK", &n, &kind));
  CHECK(!ParseFileName("notes.txt", &n, &kind));
  CHECK(!ParseFileName("abc.sst", &n, &kind));        // non-numeric stem
  CHECK(!ParseFileName(".sst", &n, &kind));
  CHECK(!ParseFileName("MANIFEST-", &n, &kind));
  CHECK(!ParseFileName("000123.sstx", &n, &kind));

  CHECK_EQ(SstFileName("/db", 123), std::string("/db/000123.sst"));
  CHECK_EQ(ManifestFileName("/db", 42), std::string("/db/MANIFEST-000042"));
}

TEST(a_reader_is_opened_once_and_reused) {
  testing::TmpDir d("tc");
  Env* env = Env::Default();
  Options opt;
  Stats stats;
  const uint64_t size = MakeFile(env, d.path(), 1, 500, opt);

  Cache block_cache(1 << 20, 4);
  TableCache tc(d.path(), opt, &block_cache, &stats);

  SstReader* r1 = nullptr; Cache::Handle* h1 = nullptr;
  REQUIRE_OK(tc.Get(1, size, &r1, &h1));
  SstReader* r2 = nullptr; Cache::Handle* h2 = nullptr;
  REQUIRE_OK(tc.Get(1, size, &r2, &h2));
  CHECK_EQ(r1, r2);            // the same open reader, not a second fd
  CHECK_EQ(tc.NumOpen(), 1u);
  tc.Release(h1);
  tc.Release(h2);
}

TEST(open_files_are_bounded_so_fds_cannot_be_exhausted) {
  // E-26: one fd per open SST. With max_open_files=4 and 50 files, the cache must evict.
  testing::TmpDir d("tc");
  Env* env = Env::Default();
  Options opt;
  opt.max_open_files = 4;
  opt.filter_memory_bytes = 64 * 1024;
  opt.cache_shards = 1;      // deterministic eviction for the assertion below
  Stats stats;

  std::vector<uint64_t> sizes;
  for (int i = 0; i < 50; ++i) sizes.push_back(MakeFile(env, d.path(), i + 1, 50, opt, i * 100));

  Cache block_cache(1 << 20, 1);
  TableCache tc(d.path(), opt, &block_cache, &stats);

  std::string value;
  bool tombstone = false;
  for (int i = 0; i < 50; ++i) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "key%08d", i * 100);
    TCTX("file=" << (i + 1));
    CHECK_OK(tc.Lookup(ReadOptions(), i + 1, sizes[i], Slice(IKey(buf, 1000000)), &value,
                       &tombstone));
  }
  std::fprintf(stderr, "   %zu readers open after touching 50 files (max_open_files=%d)\n",
               tc.NumOpen(), opt.max_open_files);
  CHECK_LE(tc.NumOpen(), 8u);   // bounded; the exact number depends on charge rounding
}

TEST(resident_filter_bytes_are_bounded_not_just_the_file_count) {
  // E-34, the bound SPEC v1 did not have. max_open_files is a COUNT; a deep-tier file's
  // whole-file Bloom filter is ~1.25 bytes per key (T2), so 500 large files would pin
  // hundreds of MiB while a count-only limit reports everything is fine.
  testing::TmpDir d("tc");
  Env* env = Env::Default();
  Options opt;
  opt.max_open_files = 1000;          // deliberately NOT the binding constraint
  opt.filter_memory_bytes = 32 * 1024;
  opt.cache_shards = 1;
  Stats stats;

  std::vector<uint64_t> sizes;
  const int kFiles = 30;
  for (int i = 0; i < kFiles; ++i)
    sizes.push_back(MakeFile(env, d.path(), i + 1, 2000, opt, i * 10000));

  Cache block_cache(1 << 20, 1);
  TableCache tc(d.path(), opt, &block_cache, &stats);
  std::string value;
  bool tombstone = false;
  for (int i = 0; i < kFiles; ++i) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "key%08d", i * 10000);
    tc.Lookup(ReadOptions(), i + 1, sizes[i], Slice(IKey(buf, 1000000)), &value, &tombstone);
  }
  std::fprintf(stderr,
               "   MEASURED %zu readers resident, %zu bytes charged (limit %zu, "
               "max_open_files=%d)\n",
               tc.NumOpen(), tc.ResidentFilterBytes(), opt.filter_memory_bytes,
               opt.max_open_files);
  CHECK_LT(tc.NumOpen(), static_cast<size_t>(kFiles));   // the BYTE limit bound it, not the count
  CHECK_LE(tc.ResidentFilterBytes(), opt.filter_memory_bytes * 2);
}

TEST(an_iterator_keeps_its_reader_alive_under_eviction_pressure) {
  // The use-after-free this wrapper exists to prevent: a long scan holds an iterator while
  // other lookups evict readers. Without the held handle the SstReader is freed mid-scan.
  // ASan is what turns this from "usually fine" into a hard failure.
  testing::TmpDir d("tc");
  Env* env = Env::Default();
  Options opt;
  opt.max_open_files = 2;
  opt.filter_memory_bytes = 8 * 1024;
  opt.cache_shards = 1;
  Stats stats;

  std::vector<uint64_t> sizes;
  for (int i = 0; i < 20; ++i) sizes.push_back(MakeFile(env, d.path(), i + 1, 200, opt, i * 1000));

  Cache block_cache(1 << 20, 1);
  TableCache tc(d.path(), opt, &block_cache, &stats);

  std::unique_ptr<Iterator> it(tc.NewIterator(ReadOptions(), 1, sizes[0]));
  it->SeekToFirst();
  REQUIRE(it->Valid());
  const std::string first_key = it->key().ToString();

  // Evict aggressively while the iterator is live.
  std::string value;
  bool tombstone = false;
  for (int i = 1; i < 20; ++i) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "key%08d", i * 1000);
    tc.Lookup(ReadOptions(), i + 1, sizes[i], Slice(IKey(buf, 1000000)), &value, &tombstone);
  }

  int n = 0;
  for (; it->Valid(); it->Next()) ++n;
  CHECK_OK(it->status());
  CHECK_EQ(n, 200);
  CHECK_EQ(first_key.substr(0, 3), std::string("key"));
}

TEST(block_cache_hits_avoid_repeated_disk_reads) {
  testing::TmpDir d("tc");
  Env* env = Env::Default();
  Options opt;
  opt.block_size = 512;
  Stats stats;
  const uint64_t size = MakeFile(env, d.path(), 1, 2000, opt);

  Cache block_cache(1 << 20, 4);
  TableCache tc(d.path(), opt, &block_cache, &stats);

  std::string value;
  bool tombstone = false;
  const std::string key = IKey("key00000100", 1000000);
  for (int i = 0; i < 100; ++i) tc.Lookup(ReadOptions(), 1, size, Slice(key), &value, &tombstone);

  std::fprintf(stderr, "   MEASURED 100 identical lookups: %llu blocks read, %llu cache hits, "
               "%llu misses\n",
               (unsigned long long)stats.Get(kBlocksRead),
               (unsigned long long)stats.Get(kCacheHits),
               (unsigned long long)stats.Get(kCacheMisses));
  CHECK_LE(stats.Get(kBlocksRead), 2u);       // essentially one read, then all hits
  CHECK_GE(stats.Get(kCacheHits), 90u);
}

RUN_ALL()

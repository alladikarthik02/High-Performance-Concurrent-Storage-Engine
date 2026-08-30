// T5b: the SST file. SPEC 3.5, 3.6, R6, E-2a, E-9, E-28, S7, S8.
#include "tests/test.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "lsmeng/coding.h"
#include "lsmeng/dbformat.h"
#include "lsmeng/sst.h"
#include "tests/tmpdir.h"

using namespace lsmeng;

namespace {
std::string IKey(const std::string& user, SequenceNumber seq, ValueType t = kTypeValue) {
  std::string s;
  AppendInternalKey(&s, ParsedInternalKey{Slice(user), seq, t});
  return s;
}

struct Built {
  std::string path;
  SstInfo info;
};

// Write an SST from already-sorted internal-key entries.
Built WriteSst(Env* env, const std::string& path,
               const std::vector<std::pair<std::string, std::string>>& kvs,
               const Options& opt) {
  std::unique_ptr<WritableFile> f;
  Status s = env->NewWritableFile(path, &f);
  Built b;
  b.path = path;
  if (!s.ok()) return b;
  SstBuilder builder(opt, f.get());
  for (const auto& kv : kvs) builder.Add(Slice(kv.first), Slice(kv.second));
  builder.Finish();
  b.info = builder.info();
  f->Sync();
  f->Close();
  return b;
}

Status OpenSst(Env* env, const Built& b, const Options& opt, Stats* stats,
               std::unique_ptr<SstReader>* out) {
  std::unique_ptr<RandomAccessFile> rf;
  Status s = env->NewRandomAccessFile(b.path, &rf);
  if (!s.ok()) return s;
  uint64_t size = 0;
  s = env->GetFileSize(b.path, &size);
  if (!s.ok()) return s;
  return SstReader::Open(opt, std::move(rf), size, stats, out);
}

std::vector<std::pair<std::string, std::string>> MakeEntries(int n, size_t value_size = 20) {
  std::vector<std::pair<std::string, std::string>> kvs;
  for (int i = 0; i < n; ++i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "user:%08d:name", i);
    kvs.emplace_back(IKey(buf, static_cast<SequenceNumber>(i + 1)),
                     std::string(value_size, static_cast<char>('a' + (i % 26))));
  }
  std::sort(kvs.begin(), kvs.end(), [](const auto& a, const auto& b) {
    return InternalKeyComparator::Compare(Slice(a.first), Slice(b.first)) < 0;
  });
  return kvs;
}
}  // namespace

TEST(round_trip_reproduces_every_entry_exactly) {
  testing::TmpDir d("sst");
  Env* env = Env::Default();
  Options opt;
  Stats stats;
  // A small block size on purpose, so the file has MANY blocks and every index boundary is
  // exercised. With one big block, E-9's separator bug is invisible.
  opt.block_size = 512;

  const auto kvs = MakeEntries(5000, 40);
  Built b = WriteSst(env, d.file("1.sst"), kvs, opt);
  std::unique_ptr<SstReader> r;
  REQUIRE_OK(OpenSst(env, b, opt, &stats, &r));
  std::fprintf(stderr, "   %llu entries in %llu bytes across %u data blocks\n",
               (unsigned long long)b.info.num_entries, (unsigned long long)b.info.file_size,
               r->NumDataBlocks());
  CHECK_GT(r->NumDataBlocks(), 50u);

  std::unique_ptr<Iterator> it(r->NewIterator(ReadOptions()));
  size_t i = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next(), ++i) {
    REQUIRE(i < kvs.size());
    TCTX("entry=" << i);
    CHECK_EQ(it->key().ToString(), kvs[i].first);
    CHECK_EQ(it->value().ToString(), kvs[i].second);
  }
  CHECK_EQ(i, kvs.size());
  CHECK_OK(it->status());
}

TEST(every_key_is_findable_across_every_block_boundary) {
  // SPEC E-9. The index separator must satisfy last_key(i) <= S < first_key(i+1). Get it
  // wrong by one and exactly the keys sitting on a block boundary go missing -- a handful
  // out of thousands, with no error anywhere. Tiny blocks make boundaries dense.
  testing::TmpDir d("sst");
  Env* env = Env::Default();
  Options opt;
  opt.block_size = 200;      // roughly 2-3 entries per block
  Stats stats;

  const auto kvs = MakeEntries(2000, 30);
  Built b = WriteSst(env, d.file("b.sst"), kvs, opt);
  std::unique_ptr<SstReader> r;
  REQUIRE_OK(OpenSst(env, b, opt, &stats, &r));
  std::fprintf(stderr, "   %u data blocks for %zu entries\n", r->NumDataBlocks(), kvs.size());
  CHECK_GT(r->NumDataBlocks(), 400u);

  std::string value;
  bool tombstone = false;
  for (size_t i = 0; i < kvs.size(); ++i) {
    TCTX("entry=" << i);
    Status s = r->Get(ReadOptions(), Slice(kvs[i].first), &value, &tombstone);
    CHECK_OK(s);
    CHECK_EQ(value, kvs[i].second);
  }
}

TEST(a_tombstone_is_in_the_filter_and_reported_as_a_hit) {
  // SPEC E-2a -- the single worst bug available in this design, and it is one missing
  // line in the builder. If a deletion's key were not added to the Bloom filter, the
  // filter would say "not present", this file would be skipped, and an older value in a
  // deeper tier would resurrect.
  testing::TmpDir d("sst");
  Env* env = Env::Default();
  Options opt;
  Stats stats;
  std::vector<std::pair<std::string, std::string>> kvs = {
      {IKey("alive", 10), "v"},
      {IKey("deleted", 20, kTypeDeletion), ""},
  };
  std::sort(kvs.begin(), kvs.end(), [](const auto& a, const auto& b) {
    return InternalKeyComparator::Compare(Slice(a.first), Slice(b.first)) < 0;
  });
  Built b = WriteSst(env, d.file("t.sst"), kvs, opt);
  CHECK_EQ(b.info.num_deletions, 1u);

  std::unique_ptr<SstReader> r;
  REQUIRE_OK(OpenSst(env, b, opt, &stats, &r));

  std::string value;
  bool tombstone = false;
  Status s = r->Get(ReadOptions(), Slice(IKey("deleted", 100)), &value, &tombstone);
  CHECK(s.IsNotFound());
  CHECK(tombstone);          // a HIT that happens to be a delete -- the search must stop
  CHECK_EQ(stats.Get(kBloomRejected), 0u);   // the filter must NOT have rejected it

  tombstone = false;
  s = r->Get(ReadOptions(), Slice(IKey("never-written", 100)), &value, &tombstone);
  CHECK(s.IsNotFound());
  CHECK(!tombstone);         // genuinely absent -- the search must continue
}

TEST(measured_bloom_rejection_on_absent_keys) {
  // R6: "measured reduction in SST data blocks read per Get". This is where that number
  // comes from -- with filters on, an absent-key lookup reads ~0.8% of the blocks it would
  // otherwise. With them off, every candidate file is read.
  testing::TmpDir d("sst");
  Env* env = Env::Default();
  const auto kvs = MakeEntries(20000, 20);

  for (int bits : {0, 10}) {
    Options opt;
    opt.bloom_bits_per_key = bits;
    opt.block_size = 4096;
    Stats stats;
    Built b = WriteSst(env, d.file("f" + std::to_string(bits) + ".sst"), kvs, opt);
    std::unique_ptr<SstReader> r;
    REQUIRE_OK(OpenSst(env, b, opt, &stats, &r));

    const uint64_t blocks_before = stats.Get(kBlocksRead);
    std::string value;
    bool tombstone = false;
    const int kProbes = 20000;
    for (int i = 0; i < kProbes; ++i) {
      char buf[40];
      std::snprintf(buf, sizeof(buf), "absent:%08d", i);
      r->Get(ReadOptions(), Slice(IKey(buf, 1000000)), &value, &tombstone);
    }
    const uint64_t blocks = stats.Get(kBlocksRead) - blocks_before;
    std::fprintf(stderr,
                 "   MEASURED bloom_bits=%2d -> %llu data blocks read for %d absent-key "
                 "lookups (%.4f per lookup), %llu rejected by filter\n",
                 bits, (unsigned long long)blocks, kProbes,
                 static_cast<double>(blocks) / kProbes,
                 (unsigned long long)stats.Get(kBloomRejected));
    if (bits == 0) CHECK_GE(blocks, static_cast<uint64_t>(kProbes));   // one block each
    else CHECK_LT(blocks, static_cast<uint64_t>(kProbes) / 20);        // ~1% of them
  }
}

TEST(smallest_and_largest_keys_are_recorded_for_the_manifest) {
  // These live in the MANIFEST, not the SST, because the version set uses them to answer
  // "can this file possibly contain key k?" WITHOUT opening the file (SPEC 3.5). It is the
  // cheapest read-path filter and runs before the Bloom filter.
  testing::TmpDir d("sst");
  Env* env = Env::Default();
  Options opt;
  const auto kvs = MakeEntries(100);
  Built b = WriteSst(env, d.file("m.sst"), kvs, opt);
  CHECK_EQ(b.info.smallest_key, kvs.front().first);
  CHECK_EQ(b.info.largest_key, kvs.back().first);
  CHECK_EQ(b.info.num_entries, kvs.size());
  CHECK_GT(b.info.file_size, 0u);
}

TEST(a_corrupt_block_is_caught_by_its_checksum) {
  // S7. Flip a bit in the data region and the reader must report Corruption rather than
  // hand back wrong bytes.
  testing::TmpDir d("sst");
  Env* env = Env::Default();
  Options opt;
  opt.block_size = 256;
  Stats stats;
  const auto kvs = MakeEntries(200);
  Built b = WriteSst(env, d.file("c.sst"), kvs, opt);

  std::string bytes;
  REQUIRE_OK(ReadFileToString(env, b.path, &bytes));
  bytes[50] = static_cast<char>(bytes[50] ^ 0x40);   // inside the first data block
  REQUIRE_OK(WriteStringToFileSync(env, bytes, b.path));

  std::unique_ptr<SstReader> r;
  REQUIRE_OK(OpenSst(env, b, opt, &stats, &r));      // footer and index are still fine
  std::unique_ptr<Iterator> it(r->NewIterator(ReadOptions()));
  it->SeekToFirst();
  // Either the scan reports corruption, or it stops -- but it must never return a key that
  // was not written.
  bool saw_corruption = false;
  for (; it->Valid(); it->Next()) {}
  if (!it->status().ok()) saw_corruption = true;
  CHECK(saw_corruption);
  CHECK_CODE(it->status(), Status::Code::kCorruption);
}

TEST(a_file_without_the_magic_is_refused) {
  testing::TmpDir d("sst");
  Env* env = Env::Default();
  Options opt;
  Stats stats;
  REQUIRE_OK(WriteStringToFileSync(env, std::string(200, 'x'), d.file("junk.sst")));
  Built b; b.path = d.file("junk.sst");
  std::unique_ptr<SstReader> r;
  Status s = OpenSst(env, b, opt, &stats, &r);
  CHECK_CODE(s, Status::Code::kCorruption);
  // A file too short even for a footer is a different, also-named failure.
  REQUIRE_OK(WriteStringToFileSync(env, "tiny", d.file("tiny.sst")));
  Built t; t.path = d.file("tiny.sst");
  CHECK_CODE(OpenSst(env, t, opt, &stats, &r), Status::Code::kCorruption);
}

TEST(a_one_megabyte_value_round_trips) {
  // E-28: one entry per block, legal, must not corrupt anything.
  testing::TmpDir d("sst");
  Env* env = Env::Default();
  Options opt;
  opt.block_size = 4096;
  Stats stats;
  const std::string big(1 << 20, 'B');
  std::vector<std::pair<std::string, std::string>> kvs = {
      {IKey("a", 1), "small"}, {IKey("b", 2), big}, {IKey("c", 3), "small again"},
  };
  Built b = WriteSst(env, d.file("big.sst"), kvs, opt);
  std::unique_ptr<SstReader> r;
  REQUIRE_OK(OpenSst(env, b, opt, &stats, &r));
  std::string value;
  bool tombstone = false;
  CHECK_OK(r->Get(ReadOptions(), Slice(IKey("b", 100)), &value, &tombstone));
  CHECK_EQ(value.size(), big.size());
  CHECK_EQ(value, big);
}

TEST(forward_and_reverse_scans_agree_over_a_multi_block_file) {
  testing::TmpDir d("sst");
  Env* env = Env::Default();
  Options opt;
  opt.block_size = 300;
  Stats stats;
  const auto kvs = MakeEntries(1000, 25);
  Built b = WriteSst(env, d.file("r.sst"), kvs, opt);
  std::unique_ptr<SstReader> r;
  REQUIRE_OK(OpenSst(env, b, opt, &stats, &r));

  std::vector<std::string> backwards;
  std::unique_ptr<Iterator> it(r->NewIterator(ReadOptions()));
  for (it->SeekToLast(); it->Valid(); it->Prev()) backwards.push_back(it->key().ToString());
  std::reverse(backwards.begin(), backwards.end());
  REQUIRE(backwards.size() == kvs.size());
  for (size_t i = 0; i < kvs.size(); ++i) { TCTX("i=" << i); CHECK_EQ(backwards[i], kvs[i].first); }
}

TEST(an_empty_sst_is_well_formed) {
  testing::TmpDir d("sst");
  Env* env = Env::Default();
  Options opt;
  Stats stats;
  Built b = WriteSst(env, d.file("e.sst"), {}, opt);
  std::unique_ptr<SstReader> r;
  REQUIRE_OK(OpenSst(env, b, opt, &stats, &r));
  std::unique_ptr<Iterator> it(r->NewIterator(ReadOptions()));
  it->SeekToFirst();
  CHECK(!it->Valid());
  CHECK_OK(it->status());
  std::string value;
  bool tombstone = false;
  CHECK(r->Get(ReadOptions(), Slice(IKey("x", 1)), &value, &tombstone).IsNotFound());
}

RUN_ALL()

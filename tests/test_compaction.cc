// T10: tiered compaction. SPEC 3.8, R7, S15, E-1, E-2 (all five), E-11, E-30, E-32, E-33.
//
// Four of the tests here exist because SPEC v1 was WRONG in ways that silently lose or
// resurrect data, and the spec review caught them on paper (SPEC 11.1-11.4). Each one
// reconstructs the exact scenario that would have broken.
#include "tests/test.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <thread>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "lsmeng/db.h"
#include "tests/tmpdir.h"

using namespace lsmeng;

namespace {
#define REQUIRE_OK_VOID(st) do { const auto s_ = (st); if (!s_.ok()) { ::testing::fail(__FILE__, __LINE__, s_.ToString()); return; } } while (0)

struct Db {
  testing::TmpDir dir{"compact"};
  Options opt;
  DB* db = nullptr;

  Db(size_t write_buffer = 16 * 1024, int tier_trigger = 4, int max_tiers = 7) {
    opt.write_buffer_size = write_buffer;
    opt.tier_trigger = tier_trigger;
    opt.max_tiers = max_tiers;
    Open();
  }
  ~Db() { delete db; }
  Status Open() { delete db; db = nullptr; return DB::Open(opt, dir.path(), &db); }
  DB* operator->() { return db; }

  std::string Get(const std::string& k) {
    std::string v;
    Status s = db->Get(ReadOptions(), Slice(k), &v);
    if (s.IsNotFound()) return "<absent>";
    if (!s.ok()) return "<error:" + s.ToString() + ">";
    return v;
  }
  uint64_t Prop(const std::string& n) {
    std::string v;
    if (!db->GetProperty(Slice(n), &v)) return UINT64_MAX;
    return std::strtoull(v.c_str(), nullptr, 10);
  }
  int TotalFiles() {
    int n = 0;
    for (int t = 0; t < 12; ++t) n += static_cast<int>(Prop("lsmeng.num-files-at-tier" + std::to_string(t)));
    return n;
  }
  std::string TierShape() {
    std::string s;
    for (int t = 0; t < 12; ++t) {
      const uint64_t n = Prop("lsmeng.num-files-at-tier" + std::to_string(t));
      if (n) s += "t" + std::to_string(t) + "=" + std::to_string(n) + " ";
    }
    return s;
  }
  // Write enough distinct filler to force `flushes` memtable flushes.
  void Filler(int flushes, int tag) {
    for (int i = 0; i < flushes * 200; ++i)
      REQUIRE_OK_VOID(db->Put(WriteOptions(),
                              "zfill" + std::to_string(tag) + "_" + std::to_string(i),
                              std::string(100, 'f')));
  }
};

std::vector<std::pair<std::string, std::string>> ScanAll(DB* db) {
  std::vector<std::pair<std::string, std::string>> out;
  std::unique_ptr<Iterator> it(db->NewIterator(ReadOptions()));
  for (it->SeekToFirst(); it->Valid(); it->Next())
    out.emplace_back(it->key().ToString(), it->value().ToString());
  return out;
}
}  // namespace

TEST(compaction_reduces_the_file_count_and_preserves_every_live_key) {
  Db d;
  std::map<std::string, std::string> model;
  for (int i = 0; i < 6000; ++i) {
    const std::string k = "k" + std::to_string(i % 500);
    const std::string v = "v" + std::to_string(i);
    REQUIRE_OK(d->Put(WriteOptions(), k, v));
    model[k] = v;
  }
  REQUIRE_OK(d->CompactRange(nullptr, nullptr));
  std::fprintf(stderr, "   after compaction: %s (%llu compactions, %llu bytes)\n",
               d.TierShape().c_str(), (unsigned long long)d.Prop("lsmeng.compactions"),
               (unsigned long long)d.Prop("lsmeng.bytes-compacted"));
  CHECK_GT(d.Prop("lsmeng.compactions"), 0u);
  const auto got = ScanAll(d.db);
  CHECK_EQ(got.size(), model.size());
  for (const auto& kv : model) { TCTX("k=" << kv.first); CHECK_EQ(d.Get(kv.first), kv.second); }
}

TEST(the_newest_version_wins_across_tiers) {
  // SPEC E-1 / 3.8.1. The tier-ordering invariant: for every user key, the SHALLOWEST tier
  // containing it holds its NEWEST version. It holds only because compaction inputs are an
  // oldest-first prefix -- a size-chosen subset would break it (11.4).
  Db d;
  REQUIRE_OK(d->Put(WriteOptions(), "target", "v1"));
  d.Filler(2, 1);
  REQUIRE_OK(d->Flush());
  REQUIRE_OK(d->Put(WriteOptions(), "target", "v2"));
  d.Filler(2, 2);
  REQUIRE_OK(d->Flush());
  REQUIRE_OK(d->CompactRange(nullptr, nullptr));   // pushes older data downward
  REQUIRE_OK(d->Put(WriteOptions(), "target", "v3"));
  REQUIRE_OK(d->Flush());
  std::fprintf(stderr, "   tiers: %s\n", d.TierShape().c_str());
  CHECK_EQ(d.Get("target"), std::string("v3"));
  REQUIRE_OK(d.Open());                            // and after recovery
  CHECK_EQ(d.Get("target"), std::string("v3"));
}

TEST(a_partial_tier_compaction_does_not_reorder_recency) {
  // SPEC E-30. Tier 0 holds MORE than tier_trigger files (the normal loaded state), so the
  // picker must take the OLDEST T, never a size-chosen or newest-chosen subset. If it took
  // the newest, a newer version would be promoted to tier 1 while an older one stayed in
  // tier 0 -- which is searched first -- and Get would return the stale value forever, with
  // no crash and no checksum failure.
  Db d(8 * 1024, /*tier_trigger=*/4);
  for (int gen = 0; gen < 8; ++gen) {
    REQUIRE_OK(d->Put(WriteOptions(), "hot", "gen" + std::to_string(gen)));
    d.Filler(1, gen);
    REQUIRE_OK(d->Flush());
  }
  std::fprintf(stderr, "   tiers before drain: %s\n", d.TierShape().c_str());
  CHECK_EQ(d.Get("hot"), std::string("gen7"));
  REQUIRE_OK(d->CompactRange(nullptr, nullptr));
  std::fprintf(stderr, "   tiers after drain:  %s\n", d.TierShape().c_str());
  CHECK_EQ(d.Get("hot"), std::string("gen7"));
}

TEST(e2a_a_tombstone_in_a_deeper_file_is_not_skipped_by_the_bloom_filter) {
  // E-2(a). Every entry goes into the filter, tombstones included. If a deletion's key were
  // omitted, the filter would say "not present", its file would be skipped, and an older
  // value in a deeper tier would resurrect.
  Db d;
  REQUIRE_OK(d->Put(WriteOptions(), "victim", "alive"));
  REQUIRE_OK(d->Flush());
  REQUIRE_OK(d->Delete(WriteOptions(), "victim"));
  REQUIRE_OK(d->Flush());
  CHECK_EQ(d.Get("victim"), std::string("<absent>"));
  REQUIRE_OK(d.Open());
  CHECK_EQ(d.Get("victim"), std::string("<absent>"));
}

TEST(e2d_a_version_visible_only_through_an_old_snapshot_is_not_dropped) {
  // ***SPEC 11.2***, the critical bug in v1's merge rule. v1 dropped a non-newest version
  // when `entry.seq <= oldest_snapshot_seq` -- which is exactly the version the snapshot
  // needs, every time, because the first version at or below the boundary always satisfies
  // it. The correct predicate is about the PREVIOUSLY EMITTED version.
  Db d;
  REQUIRE_OK(d->Put(WriteOptions(), "k", "v1"));
  SnapshotHandle snap(d.db);                 // pins v1
  REQUIRE_OK(d->Put(WriteOptions(), "k", "v2"));
  REQUIRE_OK(d->Put(WriteOptions(), "k", "v3"));
  REQUIRE_OK(d->Flush());
  REQUIRE_OK(d->CompactRange(nullptr, nullptr));

  ReadOptions ro;
  ro.snapshot = snap.get();
  std::string v;
  Status s = d->Get(ro, "k", &v);
  CHECK_OK(s);
  CHECK_EQ(v, std::string("v1"));            // v1 must have survived the merge
  CHECK_EQ(d.Get("k"), std::string("v3"));   // and the live read still sees v3
}

TEST(e2e_a_tombstone_is_not_dropped_while_an_older_sibling_holds_the_key) {
  // ***SPEC 11.1***, the other critical v1 bug. v1 allowed dropping a tombstone whenever
  // "the output tier is the last tier that exists" -- reasoning only about DEEPER tiers.
  // But tiered compaction writes its output INTO a tier that already holds up to T-1 older,
  // non-input files that the merge never reads. Dropping the tombstone then resurrects the
  // value one of those files still holds.
  //
  // Built directly: drive a value down into a deep tier, then delete it and force the
  // tombstone through a compaction whose inputs exclude the file holding the value.
  Db d(8 * 1024, /*tier_trigger=*/4, /*max_tiers=*/7);
  REQUIRE_OK(d->Put(WriteOptions(), "buried", "SHOULD_NOT_COME_BACK"));
  for (int gen = 0; gen < 6; ++gen) { d.Filler(1, gen); REQUIRE_OK(d->Flush()); }
  REQUIRE_OK(d->CompactRange(nullptr, nullptr));
  std::fprintf(stderr, "   after burying: %s\n", d.TierShape().c_str());
  CHECK_EQ(d.Get("buried"), std::string("SHOULD_NOT_COME_BACK"));

  REQUIRE_OK(d->Delete(WriteOptions(), "buried"));
  for (int gen = 10; gen < 18; ++gen) { d.Filler(1, gen); REQUIRE_OK(d->Flush()); }
  REQUIRE_OK(d->CompactRange(nullptr, nullptr));
  std::fprintf(stderr, "   after deleting: %s\n", d.TierShape().c_str());

  CHECK_EQ(d.Get("buried"), std::string("<absent>"));
  REQUIRE_OK(d.Open());
  CHECK_EQ(d.Get("buried"), std::string("<absent>"));
  const auto got = ScanAll(d.db);
  for (const auto& kv : got) CHECK_NE(kv.first, std::string("buried"));
}

TEST(e33_compaction_actually_reclaims_space_with_no_live_snapshot) {
  // SPEC E-33. With no live snapshot, oldest_snapshot_seq is last_sequence_, NOT 0. A zero
  // there turns compaction into a pure merge that reclaims NOTHING -- and every correctness
  // test still passes, because nothing wrong is ever returned. This is the test that fails
  // when that happens.
  Db d;
  for (int round = 0; round < 12; ++round)
    for (int i = 0; i < 500; ++i)
      REQUIRE_OK(d->Put(WriteOptions(), "k" + std::to_string(i), "round" + std::to_string(round)));
  REQUIRE_OK(d->Flush());
  REQUIRE_OK(d->CompactRange(nullptr, nullptr));

  // THE ASSERTION THAT MATTERS: how many ENTRIES remain on disk. 6,000 writes over 500
  // distinct keys leaves 6,000 entries if the merge drops nothing, and ~500 if it works.
  //
  // Neither the file COUNT nor the byte total can express this, and both mislead here:
  // compaction runs in the background throughout the writes, so by the time a test can
  // read a "before" figure the reclamation has already happened and before == after. That
  // is why the first two versions of this assertion failed on a correct engine.
  const uint64_t entries = d.Prop("lsmeng.live-entries");
  const uint64_t bytes = d.Prop("lsmeng.live-bytes");
  const auto got = ScanAll(d.db);
  std::fprintf(stderr,
               "   MEASURED %llu entries and %llu bytes on disk for %zu live keys, after "
               "6000 writes over 500 keys (%d files)\n",
               (unsigned long long)entries, (unsigned long long)bytes, got.size(),
               d.TotalFiles());
  CHECK_EQ(got.size(), 500u);
  // Some superseded versions legitimately remain in shallow tiers that have not been
  // merged yet; what must NOT happen is all 6,000 surviving, which is what an
  // oldest_snapshot of 0 produces (E-33).
  CHECK_LT(entries, 2000u);
  for (int i = 0; i < 500; ++i) {
    TCTX("i=" << i);
    CHECK_EQ(d.Get("k" + std::to_string(i)), std::string("round11"));
  }
}

TEST(deleted_keys_stop_costing_space_once_their_tombstones_are_droppable) {
  Db d;
  for (int i = 0; i < 2000; ++i) REQUIRE_OK(d->Put(WriteOptions(), "d" + std::to_string(i), std::string(80, 'x')));
  REQUIRE_OK(d->Flush());
  for (int i = 0; i < 2000; ++i) REQUIRE_OK(d->Delete(WriteOptions(), "d" + std::to_string(i)));
  REQUIRE_OK(d->CompactRange(nullptr, nullptr));
  const auto got = ScanAll(d.db);
  std::fprintf(stderr, "   %s -> %zu live keys, %llu bytes compacted\n", d.TierShape().c_str(),
               got.size(), (unsigned long long)d.Prop("lsmeng.bytes-compacted"));
  CHECK_EQ(got.size(), 0u);
}

TEST(tier_count_is_bounded_by_max_tiers) {
  // SPEC E-32 / 3.8.2. Under an overwrite workload every merge output is about the size of
  // its inputs, so a naive count-triggered scheme grows tiers with TOTAL bytes written
  // rather than live data. max_tiers folds the deepest tier instead of creating a new one.
  Db d(8 * 1024, /*tier_trigger=*/2, /*max_tiers=*/4);
  for (int round = 0; round < 40; ++round) {
    for (int i = 0; i < 200; ++i)
      REQUIRE_OK(d->Put(WriteOptions(), "k" + std::to_string(i), "r" + std::to_string(round)));
    REQUIRE_OK(d->Flush());
  }
  REQUIRE_OK(d->CompactRange(nullptr, nullptr));
  std::fprintf(stderr, "   MEASURED tiers after 40 overwrite rounds: %s\n", d.TierShape().c_str());
  for (int t = 4; t < 12; ++t) {
    TCTX("tier=" << t);
    CHECK_EQ(d.Prop("lsmeng.num-files-at-tier" + std::to_string(t)), 0u);
  }
  for (int i = 0; i < 200; ++i) {
    TCTX("i=" << i);
    CHECK_EQ(d.Get("k" + std::to_string(i)), std::string("r39"));
  }
}

TEST(writes_stall_rather_than_letting_tier_zero_grow_without_bound) {
  // SPEC E-11 / 3.8.4. The stall is DESIGNED behaviour, not a bug: without it, reads
  // degrade without limit. With compaction running, the stall must resolve rather than
  // become the permanent outage T9 saw when no compactor existed.
  Db d(8 * 1024, /*tier_trigger=*/4);
  for (int i = 0; i < 8000; ++i)
    REQUIRE_OK(d->Put(WriteOptions(), "k" + std::to_string(i % 2000), std::string(60, 'v')));
  // Whether a stall was actually NEEDED depends on how fast compaction keeps up on this
  // machine; what must hold either way is that tier 0 never exceeds the stop trigger. The
  // stall counter is reported for information, not asserted.
  std::fprintf(stderr, "   MEASURED %llu stalls, max stall %llu ms, tiers: %s\n",
               (unsigned long long)d.Prop("lsmeng.stalls"),
               (unsigned long long)d.Prop("lsmeng.max-stall-ms"), d.TierShape().c_str());
  CHECK_LE(d.Prop("lsmeng.num-files-at-tier0"), static_cast<uint64_t>(d.opt.tier0_stop_trigger));
  for (int i = 0; i < 2000; i += 97) {
    TCTX("i=" << i);
    CHECK_EQ(d.Get("k" + std::to_string(i)).size(), 60u);
  }
}

TEST(compaction_runs_concurrently_with_readers_and_writers) {
  // R8: readers, writers, a live flush and a live compaction, all at once. TSan is the
  // verdict; this supplies the interleaving.
  Db d(16 * 1024);
  std::atomic<bool> stop{false};
  std::atomic<int> bad{0}, reads{0};

  std::vector<std::thread> writers;
  for (int t = 0; t < 3; ++t)
    writers.emplace_back([&, t] {
      for (int i = 0; i < 2500; ++i) {
        const std::string k = "k" + std::to_string(t * 10000 + i);
        d->Put(WriteOptions(), k, "v:" + k);
      }
    });
  std::vector<std::thread> readers;
  for (int r = 0; r < 3; ++r)
    readers.emplace_back([&, r] {
      testing::Rng rng(static_cast<uint64_t>(r) + 1);
      while (!stop.load(std::memory_order_acquire)) {
        const std::string k = "k" + std::to_string(rng.below(30000));
        std::string v;
        Status s = d->Get(ReadOptions(), Slice(k), &v);
        reads.fetch_add(1, std::memory_order_relaxed);
        if (s.ok() && v != "v:" + k) bad.fetch_add(1);
        else if (!s.ok() && !s.IsNotFound()) bad.fetch_add(1);
      }
    });
  for (auto& t : writers) t.join();
  stop.store(true, std::memory_order_release);
  for (auto& t : readers) t.join();
  std::fprintf(stderr, "   %d reads, %d inconsistent, %llu compactions, tiers: %s\n",
               reads.load(), bad.load(), (unsigned long long)d.Prop("lsmeng.compactions"),
               d.TierShape().c_str());
  CHECK_EQ(bad.load(), 0);
  CHECK_GT(d.Prop("lsmeng.compactions"), 0u);
}

RUN_ALL()

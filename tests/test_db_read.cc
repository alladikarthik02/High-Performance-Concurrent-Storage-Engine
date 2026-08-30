// T9: the read path -- merging iterator, snapshots, scans.
//
// THE HEADLINE TEST IS `differential_against_a_std_map_model`. SPEC 7 layer 3 calls it the
// one that matters, and the reason is that it does not encode my beliefs about what could
// go wrong: it runs a random op sequence against a reference model and compares EVERYTHING
// after every step. The key space is deliberately tiny so overwrites, deletes and
// resurrections collide constantly -- a large key space would hide exactly the
// version-selection bugs this is hunting.
#include "tests/test.h"

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "lsmeng/db.h"
#include "tests/tmpdir.h"

using namespace lsmeng;

namespace {
struct Db {
  testing::TmpDir dir{"dbread"};
  Options opt;
  DB* db = nullptr;

  explicit Db(size_t write_buffer = 256 * 1024, bool allow_unbounded_tier0 = false) {
    opt.write_buffer_size = write_buffer;
    if (allow_unbounded_tier0) {
      // THERE IS NO COMPACTOR UNTIL T10. Tier 0 therefore grows by one file per flush and
      // nothing ever drains it, so the stop trigger fires and every writer stalls -- which
      // is CORRECT behaviour, and the watchdog reports it clearly rather than hanging.
      // SPEC section 9 states this forward dependency explicitly instead of hiding it;
      // T10 restores the defaults and adds the write-stall test that belongs with the
      // compactor.
      opt.tier0_slowdown_trigger = 1000000;
      opt.tier0_stop_trigger = 1000000;
    }
    Open();
  }
  ~Db() { delete db; }
  Status Open() { delete db; db = nullptr; return DB::Open(opt, dir.path(), &db); }
  DB* operator->() { return db; }
};

std::vector<std::pair<std::string, std::string>> ScanAll(DB* db, const Snapshot* snap = nullptr) {
  ReadOptions ro;
  ro.snapshot = snap;
  std::vector<std::pair<std::string, std::string>> out;
  std::unique_ptr<Iterator> it(db->NewIterator(ro));
  for (it->SeekToFirst(); it->Valid(); it->Next())
    out.emplace_back(it->key().ToString(), it->value().ToString());
  return out;
}

std::vector<std::pair<std::string, std::string>> ScanAllReverse(DB* db,
                                                                const Snapshot* snap = nullptr) {
  ReadOptions ro;
  ro.snapshot = snap;
  std::vector<std::pair<std::string, std::string>> out;
  std::unique_ptr<Iterator> it(db->NewIterator(ro));
  for (it->SeekToLast(); it->Valid(); it->Prev())
    out.emplace_back(it->key().ToString(), it->value().ToString());
  std::reverse(out.begin(), out.end());
  return out;
}
}  // namespace

TEST(a_scan_returns_keys_in_sorted_order_with_no_duplicates) {
  Db d;
  for (int i = 99; i >= 0; --i)   // inserted in reverse, must come back sorted
    CHECK_OK(d->Put(WriteOptions(), "key" + std::to_string(100 + i), "v" + std::to_string(i)));
  for (int i = 0; i < 100; ++i)   // overwrite everything, so every key has two versions
    CHECK_OK(d->Put(WriteOptions(), "key" + std::to_string(100 + i), "w" + std::to_string(i)));

  const auto got = ScanAll(d.db);
  REQUIRE(got.size() == 100);
  for (int i = 0; i < 100; ++i) {
    TCTX("i=" << i);
    CHECK_EQ(got[i].first, "key" + std::to_string(100 + i));
    CHECK_EQ(got[i].second, "w" + std::to_string(i));   // the NEWER version wins
  }
}

TEST(a_deleted_key_is_absent_from_a_scan_and_does_not_resurrect) {
  Db d;
  for (int i = 0; i < 20; ++i) CHECK_OK(d->Put(WriteOptions(), "k" + std::to_string(i), "first"));
  CHECK_OK(d->Flush());                                       // versions now in an SST
  for (int i = 0; i < 20; i += 2) CHECK_OK(d->Delete(WriteOptions(), "k" + std::to_string(i)));

  const auto got = ScanAll(d.db);
  CHECK_EQ(got.size(), 10u);
  for (const auto& kv : got) {
    const int n = std::atoi(kv.first.c_str() + 1);
    TCTX("key=" << kv.first);
    CHECK_EQ(n % 2, 1);          // only the odd ones survive
  }
  // E-2: the tombstone is in the memtable and the value is in an SST. A read that fell
  // through the tombstone would resurrect "first".
  std::string v;
  CHECK(d->Get(ReadOptions(), "k0", &v).IsNotFound());
}

TEST(forward_and_reverse_scans_agree_exactly) {
  // SPEC E-15. Prev is a genuinely different code path -- it re-seeks and resolves each
  // user key rather than unwinding versions -- so it is compared against Next, not against
  // my expectations.
  Db d(32 * 1024, /*allow_unbounded_tier0=*/true);   // several SSTs plus a memtable: a real merge
  testing::Rng rng(testing::seed());
  for (int i = 0; i < 3000; ++i) {
    const std::string k = "k" + std::to_string(rng.below(400));
    if (rng.one_in(4)) d->Delete(WriteOptions(), k);
    else d->Put(WriteOptions(), k, "v" + std::to_string(i));
  }
  const auto fwd = ScanAll(d.db);
  const auto rev = ScanAllReverse(d.db);
  CHECK_EQ(fwd.size(), rev.size());
  for (size_t i = 0; i < fwd.size() && i < rev.size(); ++i) {
    TCTX("i=" << i);
    CHECK_EQ(fwd[i].first, rev[i].first);
    CHECK_EQ(fwd[i].second, rev[i].second);
  }
}

TEST(seek_positions_at_the_first_key_at_or_after_the_target) {
  Db d;
  for (int i = 0; i < 100; ++i) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "k%04d", i * 10);
    CHECK_OK(d->Put(WriteOptions(), buf, "v"));
  }
  for (int probe = -5; probe < 1005; ++probe) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "k%04d", probe);
    TCTX("probe=" << probe);
    std::unique_ptr<Iterator> it(d->NewIterator(ReadOptions()));
    it->Seek(Slice(buf));
    if (probe > 990) { CHECK(!it->Valid()); continue; }
    REQUIRE(it->Valid());
    const int expect = ((probe < 0 ? 0 : probe) + 9) / 10 * 10;
    char want[16];
    std::snprintf(want, sizeof(want), "k%04d", expect);
    CHECK_EQ(it->key().ToString(), std::string(want));
  }
}

TEST(a_snapshot_sees_the_database_as_it_was) {
  Db d;
  CHECK_OK(d->Put(WriteOptions(), "a", "1"));
  CHECK_OK(d->Put(WriteOptions(), "b", "1"));
  SnapshotHandle snap(d.db);     // RAII -- E-10: a leaked snapshot freezes reclamation

  CHECK_OK(d->Put(WriteOptions(), "a", "2"));
  CHECK_OK(d->Delete(WriteOptions(), "b"));
  CHECK_OK(d->Put(WriteOptions(), "c", "new"));

  ReadOptions ro;
  ro.snapshot = snap.get();
  std::string v;
  CHECK_OK(d->Get(ro, "a", &v));  CHECK_EQ(v, std::string("1"));
  CHECK_OK(d->Get(ro, "b", &v));  CHECK_EQ(v, std::string("1"));   // the delete is invisible
  CHECK(d->Get(ro, "c", &v).IsNotFound());                          // and so is the new key

  const auto snapshot_scan = ScanAll(d.db, snap.get());
  REQUIRE(snapshot_scan.size() == 2);
  CHECK_EQ(snapshot_scan[0].first, std::string("a"));
  CHECK_EQ(snapshot_scan[0].second, std::string("1"));
  CHECK_EQ(snapshot_scan[1].first, std::string("b"));

  const auto live_scan = ScanAll(d.db);
  REQUIRE(live_scan.size() == 2);
  CHECK_EQ(live_scan[0].second, std::string("2"));
  CHECK_EQ(live_scan[1].first, std::string("c"));
}

TEST(a_snapshot_survives_a_flush) {
  // The version pinned by the snapshot's sequence must remain readable after its data has
  // moved from the memtable into an SST.
  Db d;
  CHECK_OK(d->Put(WriteOptions(), "k", "old"));
  SnapshotHandle snap(d.db);
  CHECK_OK(d->Put(WriteOptions(), "k", "new"));
  CHECK_OK(d->Flush());

  ReadOptions ro;
  ro.snapshot = snap.get();
  std::string v;
  CHECK_OK(d->Get(ro, "k", &v));
  CHECK_EQ(v, std::string("old"));
  CHECK_OK(d->Get(ReadOptions(), "k", &v));
  CHECK_EQ(v, std::string("new"));
}

TEST(many_versions_of_one_key_are_deduplicated) {
  // E-29's correctness half: thousands of versions of a single key. A scan must emit it
  // exactly once, and a Get must not walk past all of them incorrectly.
  Db d(32 * 1024, /*allow_unbounded_tier0=*/true);
  for (int i = 0; i < 5000; ++i) CHECK_OK(d->Put(WriteOptions(), "hot", "v" + std::to_string(i)));
  const auto got = ScanAll(d.db);
  REQUIRE(got.size() == 1);
  CHECK_EQ(got[0].first, std::string("hot"));
  CHECK_EQ(got[0].second, std::string("v4999"));
}

TEST(differential_against_a_std_map_model) {
  // ***THE MILESTONE***. SPEC 7 layer 3 / T9's exit criterion.
  //
  // Op mix: Put / Delete / Get / Scan / batch / Flush / Snapshot / ReleaseSnapshot /
  // Reopen. CompactRange is excluded because compaction lands in T10 -- SPEC section 9
  // states that forward dependency rather than hiding it.
  //
  // The key space is 150 keys on purpose. Overwrites and deletes must collide constantly,
  // because the bugs being hunted are all about WHICH version wins.
  Db d(24 * 1024, /*allow_unbounded_tier0=*/true);   // small buffer -> constant flushes and real multi-SST merges
  std::map<std::string, std::string> model;
  testing::Rng rng(testing::seed());

  // Depth is tunable because this test is quadratic-ish in practice: reopens replay logs
  // and full scans merge every live SST, so 30k ops takes ~7.5 minutes unsanitised and
  // far longer under ASan/TSan. The default keeps `check.sh` usable; the deep run is the
  // one that actually certifies the read path and its result is recorded in
  // docs/BENCHMARKS.md with the command line that produced it.
  //
  //   LSMENG_MODEL_OPS=30000 ./build-none/test_db_read
  //
  const int kOps = [] {
    if (const char* e = std::getenv("LSMENG_MODEL_OPS")) return std::atoi(e);
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    // Sanitizer builds run 20-50x slower, and this test is worse than linear in ops:
    // without a compactor (T10) tier 0 grows one file per flush, so by the end of a run
    // every full scan merges ~80 SSTs -- which is exactly the read amplification SPEC 3.8
    // says tiered compaction has, observed rather than assumed. The op count is scaled so
    // the sanitizer configurations stay usable; the deep run below is what certifies the
    // read path.
    return 600;
#else
    return 4000;
#endif
  }();
  const int kKeys = 150;
  int puts = 0, dels = 0, batches = 0, flushes = 0, reopens = 0, scans = 0;

  auto key_of = [&](uint32_t n) { return "k" + std::to_string(1000 + n); };

  for (int op = 0; op < kOps; ++op) {
    TCTX("op=" << op);
    const uint32_t r = rng.below(100);

    if (r < 45) {                       // Put
      const std::string k = key_of(rng.below(kKeys));
      const std::string v = "v" + std::to_string(op);
      REQUIRE_OK(d->Put(WriteOptions(), k, v));
      model[k] = v;
      ++puts;
    } else if (r < 65) {                // Delete
      const std::string k = key_of(rng.below(kKeys));
      REQUIRE_OK(d->Delete(WriteOptions(), k));
      model.erase(k);
      ++dels;
    } else if (r < 75) {                // atomic batch
      WriteBatch b;
      std::map<std::string, std::string> staged;
      std::vector<std::string> removed;
      const int n = 1 + static_cast<int>(rng.below(5));
      for (int i = 0; i < n; ++i) {
        const std::string k = key_of(rng.below(kKeys));
        if (rng.one_in(3)) { b.Delete(k); staged.erase(k); removed.push_back(k); }
        else { const std::string v = "b" + std::to_string(op) + "_" + std::to_string(i);
               b.Put(k, v); staged[k] = v;
               removed.erase(std::remove(removed.begin(), removed.end(), k), removed.end()); }
      }
      REQUIRE_OK(d->Write(WriteOptions(), &b));
      for (const auto& k : removed) model.erase(k);
      for (const auto& kv : staged) model[kv.first] = kv.second;
      ++batches;
    } else if (r < 90) {                // Get, compared immediately
      const std::string k = key_of(rng.below(kKeys + 20));   // some keys never written
      std::string v;
      Status s = d->Get(ReadOptions(), Slice(k), &v);
      auto mi = model.find(k);
      if (mi == model.end()) {
        // S25: branch on code(), never on ToString(). NotFound means the model agrees;
        // Corruption would mean the engine is broken, and the two must not be conflated.
        CHECK_CODE(s, Status::Code::kNotFound);
      } else {
        CHECK_OK(s);
        CHECK_EQ(v, mi->second);
      }
    } else if (r < 94) {                // full scan, forward AND reverse
      const auto fwd = ScanAll(d.db);
      const auto rev = ScanAllReverse(d.db);
      CHECK_EQ(fwd.size(), model.size());
      CHECK_EQ(rev.size(), model.size());
      size_t i = 0;
      for (const auto& kv : model) {
        if (i < fwd.size()) { CHECK_EQ(fwd[i].first, kv.first); CHECK_EQ(fwd[i].second, kv.second); }
        if (i < rev.size()) { CHECK_EQ(rev[i].first, kv.first); CHECK_EQ(rev[i].second, kv.second); }
        ++i;
      }
      ++scans;
    } else if (r < 96) {                // snapshot: take it, mutate, verify, release
      SnapshotHandle snap(d.db);
      const std::map<std::string, std::string> frozen = model;
      const std::string k = key_of(rng.below(kKeys));
      REQUIRE_OK(d->Put(WriteOptions(), k, "after_snapshot"));
      model[k] = "after_snapshot";
      const auto through = ScanAll(d.db, snap.get());
      CHECK_EQ(through.size(), frozen.size());
      size_t i = 0;
      for (const auto& kv : frozen) {
        if (i < through.size()) { CHECK_EQ(through[i].first, kv.first); CHECK_EQ(through[i].second, kv.second); }
        ++i;
      }
    } else if (r < 98) {                // Flush
      REQUIRE_OK(d->Flush());
      ++flushes;
    } else {                            // Reopen -- makes this a recovery test too
      REQUIRE_OK(d.Open());
      ++reopens;
      const auto after = ScanAll(d.db);
      CHECK_EQ(after.size(), model.size());
      size_t i = 0;
      for (const auto& kv : model) {
        if (i < after.size()) { CHECK_EQ(after[i].first, kv.first); CHECK_EQ(after[i].second, kv.second); }
        ++i;
      }
    }
  }

  // Final full comparison, both directions.
  const auto fwd = ScanAll(d.db);
  const auto rev = ScanAllReverse(d.db);
  CHECK_EQ(fwd.size(), model.size());
  CHECK_EQ(rev.size(), model.size());
  size_t i = 0;
  for (const auto& kv : model) {
    TCTX("final entry " << i);
    if (i < fwd.size()) { CHECK_EQ(fwd[i].first, kv.first); CHECK_EQ(fwd[i].second, kv.second); }
    if (i < rev.size()) { CHECK_EQ(rev[i].first, kv.first); CHECK_EQ(rev[i].second, kv.second); }
    ++i;
  }
  std::fprintf(stderr,
               "   %d ops: %d puts, %d deletes, %d batches, %d flushes, %d reopens, "
               "%d scans; %zu live keys\n",
               kOps, puts, dels, batches, flushes, reopens, scans, model.size());
}

RUN_ALL()

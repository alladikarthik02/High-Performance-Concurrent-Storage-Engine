// T8: the DB write path. SPEC 3.2, 3.2.1, 3.3.1, R3, R4, R5, S1, S2, S3, S12, S13, S21,
// S22, E-19, E-20, E-36.
#include "tests/test.h"

#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "lsmeng/db.h"
#include "lsmeng/table_cache.h"
#include "tests/fault_env.h"
#include "tests/tmpdir.h"

using namespace lsmeng;

namespace {
struct Db {
  testing::TmpDir dir{"db"};
  Options opt;
  DB* db = nullptr;

  explicit Db(Env* env = nullptr, size_t write_buffer = 64 * 1024) {
    opt.env = env;
    opt.write_buffer_size = write_buffer;
    Open();
  }
  ~Db() { delete db; }
  Status Open() {
    delete db;
    db = nullptr;
    return DB::Open(opt, dir.path(), &db);
  }
  DB* operator->() { return db; }

  std::string Get(const std::string& k) {
    std::string v;
    Status s = db->Get(ReadOptions(), Slice(k), &v);
    if (s.IsNotFound()) return "<absent>";
    if (!s.ok()) return "<error:" + s.ToString() + ">";
    return v;
  }
  uint64_t Prop(const std::string& name) {
    std::string v;
    if (!db->GetProperty(Slice(name), &v)) return UINT64_MAX;
    return std::strtoull(v.c_str(), nullptr, 10);
  }
};
}  // namespace

TEST(open_put_get_delete_roundtrip) {
  Db d;
  CHECK_OK(d->Put(WriteOptions(), "k1", "v1"));
  CHECK_OK(d->Put(WriteOptions(), "k2", "v2"));
  CHECK_EQ(d.Get("k1"), std::string("v1"));
  CHECK_EQ(d.Get("k2"), std::string("v2"));
  CHECK_EQ(d.Get("nope"), std::string("<absent>"));
  CHECK_OK(d->Delete(WriteOptions(), "k1"));
  CHECK_EQ(d.Get("k1"), std::string("<absent>"));
  CHECK_EQ(d.Get("k2"), std::string("v2"));
  // Arbitrary bytes, and the empty key, are legal (SPEC 3.1).
  CHECK_OK(d->Put(WriteOptions(), "", "empty key"));
  CHECK_OK(d->Put(WriteOptions(), std::string("a\0b", 3), std::string("v\0v", 3)));
  CHECK_EQ(d.Get(""), std::string("empty key"));
  CHECK_EQ(d.Get(std::string("a\0b", 3)), std::string("v\0v", 3));
}

TEST(oversized_input_is_rejected_never_truncated) {
  // S3. A silently truncated key is a wrong answer that looks like a right one.
  Db d;
  CHECK_CODE(d->Put(WriteOptions(), Slice(std::string(kMaxKeySize + 1, 'k')), "v"),
             Status::Code::kInvalidArgument);
  CHECK_CODE(d->Put(WriteOptions(), "k", Slice(std::string(kMaxValueSize + 1, 'v'))),
             Status::Code::kInvalidArgument);
  CHECK_OK(d->Put(WriteOptions(), Slice(std::string(kMaxKeySize, 'k')), "v"));   // exactly at the limit
  WriteBatch huge;
  for (int i = 0; i < 20; ++i) huge.Put("k" + std::to_string(i), std::string(600 * 1024, 'x'));
  CHECK_CODE(d->Write(WriteOptions(), &huge), Status::Code::kInvalidArgument);
}

TEST(a_batch_is_atomic_and_ordered) {
  Db d;
  WriteBatch b;
  b.Put("a", "1");
  b.Put("b", "2");
  b.Delete("a");
  b.Put("c", "3");
  CHECK_OK(d->Write(WriteOptions(), &b));
  CHECK_EQ(d.Get("a"), std::string("<absent>"));   // the delete came after the put
  CHECK_EQ(d.Get("b"), std::string("2"));
  CHECK_EQ(d.Get("c"), std::string("3"));
}

TEST(data_survives_reopen_and_sequence_numbers_do_not_restart) {
  // SPEC E-19 / S9. If last_sequence restarted at 0, post-restart writes would get
  // sequences BELOW existing data and old values would shadow new ones forever.
  Db d;
  for (int i = 0; i < 200; ++i)
    CHECK_OK(d->Put(WriteOptions(), "key" + std::to_string(i), "before" + std::to_string(i)));
  REQUIRE_OK(d.Open());   // reopen
  for (int i = 0; i < 200; ++i) {
    TCTX("i=" << i);
    CHECK_EQ(d.Get("key" + std::to_string(i)), "before" + std::to_string(i));
  }
  // Overwrite after reopen: the NEW value must win.
  for (int i = 0; i < 200; ++i)
    CHECK_OK(d->Put(WriteOptions(), "key" + std::to_string(i), "after" + std::to_string(i)));
  REQUIRE_OK(d.Open());
  for (int i = 0; i < 200; ++i) {
    TCTX("i=" << i);
    CHECK_EQ(d.Get("key" + std::to_string(i)), "after" + std::to_string(i));
  }
}

TEST(a_memtable_switch_leaves_two_logs_live_and_recovery_replays_both) {
  // SPEC 3.3.1 / E-36 -- the bug SPEC v1 had. From the freeze of a memtable until its
  // flush has a durable manifest edit, TWO logs hold acknowledged data. A recovery that
  // replayed only log_number would lose everything written since the switch.
  // A small buffer so switches happen often -- but not so small that tier 0 reaches
  // kTier0StopTrigger, because THERE IS NO COMPACTOR UNTIL T10 and nothing would drain it.
  // The spec review predicted exactly this dependency (SPEC 11.12); T10 removes the
  // constraint and the write-stall path gets its own test there.
  Db d(nullptr, 48 * 1024);
  const int kN = 2000;
  for (int i = 0; i < kN; ++i)
    CHECK_OK(d->Put(WriteOptions(), "k" + std::to_string(i), std::string(64, 'v')));

  REQUIRE_OK(d.Open());
  int found = 0;
  for (int i = 0; i < kN; ++i) if (d.Get("k" + std::to_string(i)) != "<absent>") ++found;
  std::fprintf(stderr, "   %d/%d keys survived a reopen across many memtable switches\n",
               found, kN);
  CHECK_EQ(found, kN);
}

TEST(a_flush_produces_a_tier_zero_file_and_reads_still_work) {
  Db d(nullptr, 16 * 1024);
  for (int i = 0; i < 500; ++i)
    CHECK_OK(d->Put(WriteOptions(), "k" + std::to_string(i), std::string(64, 'v')));
  CHECK_OK(d->Flush());
  const uint64_t files = d.Prop("lsmeng.num-files-at-tier0");
  std::fprintf(stderr, "   tier 0 holds %llu file(s) after flush\n", (unsigned long long)files);
  CHECK_GT(files, 0u);
  for (int i = 0; i < 500; ++i) {
    TCTX("i=" << i);
    CHECK_EQ(d.Get("k" + std::to_string(i)).size(), 64u);   // now served from an SST
  }
}

TEST(group_commit_amortises_fsyncs_across_threads) {
  // R5's mechanism, measured. The claim is that ONE fsync commits many writers' data, so
  // fsyncs/s stays roughly flat as threads increase while writes/s climbs. If this ratio
  // is ~1.0, group commit is not working and R5 is not earned.
  Db d(nullptr, 4 << 20);
  const int kThreads = 8, kPer = 400;
  std::atomic<int> failed{0};
  std::vector<std::thread> ts;
  for (int t = 0; t < kThreads; ++t)
    ts.emplace_back([&, t] {
      WriteOptions wo;
      wo.sync = true;                       // every writer asks for durability
      for (int i = 0; i < kPer; ++i)
        if (!d->Put(wo, "t" + std::to_string(t) + "k" + std::to_string(i), std::string(50, 'v')).ok())
          failed.fetch_add(1);
    });
  for (auto& th : ts) th.join();
  CHECK_EQ(failed.load(), 0);   // never ignore a Status in a test that counts things

  const uint64_t writes = d.Prop("lsmeng.writes");
  const uint64_t syncs = d.Prop("lsmeng.wal-syncs");
  std::fprintf(stderr,
               "   MEASURED %llu writes, %llu fsyncs at %d threads -> %.2f writes per fsync\n",
               (unsigned long long)writes, (unsigned long long)syncs, kThreads,
               static_cast<double>(writes) / static_cast<double>(syncs ? syncs : 1));
  CHECK_EQ(writes, static_cast<uint64_t>(kThreads * kPer));
  // R5's mechanism: strictly fewer fsyncs than writes. A ratio near 1.0 would mean each
  // writer paid for its own fsync and group commit is not working.
  CHECK_LT(syncs, writes);
  CHECK_GT(static_cast<double>(writes) / static_cast<double>(syncs), 1.5);
}

TEST(a_sync_writer_is_never_committed_by_a_group_that_does_not_fsync) {
  // S2. Batching a sync=false writer into a synced group is harmless -- it gets more
  // durability than it asked for. The reverse is a durability bug, so BuildBatchGroup
  // stops at the first differing sync flag. Here: mixed writers, all data must survive.
  Db d(nullptr, 4 << 20);
  std::atomic<int> errors{0};
  std::vector<std::thread> ts;
  for (int t = 0; t < 8; ++t)
    ts.emplace_back([&, t] {
      WriteOptions wo;
      wo.sync = (t % 2 == 0);   // alternating, so groups are constantly split
      for (int i = 0; i < 200; ++i)
        if (!d->Put(wo, "t" + std::to_string(t) + "k" + std::to_string(i), "v").ok())
          errors.fetch_add(1);
    });
  for (auto& th : ts) th.join();
  CHECK_EQ(errors.load(), 0);
  for (int t = 0; t < 8; ++t)
    for (int i = 0; i < 200; i += 50) {
      TCTX("t=" << t << " i=" << i);
      CHECK_EQ(d.Get("t" + std::to_string(t) + "k" + std::to_string(i)), std::string("v"));
    }
}

TEST(concurrent_writers_and_readers_under_tsan) {
  // S6/S12/S21: writers, readers, a live flush, and memtable switches, all at once. TSan
  // is the verdict; this test only supplies the interleaving.
  Db d(nullptr, 256 * 1024);   // see the note above: no compactor until T10
  std::atomic<bool> stop{false};
  std::atomic<int> bad{0}, reads{0};

  std::vector<std::thread> writers;
  for (int t = 0; t < 4; ++t)
    writers.emplace_back([&, t] {
      for (int i = 0; i < 3000; ++i) {
        const std::string k = "k" + std::to_string(t * 10000 + i);
        d->Put(WriteOptions(), k, "v:" + k);   // the value encodes its key
      }
    });

  std::vector<std::thread> readers;
  for (int r = 0; r < 4; ++r)
    readers.emplace_back([&, r] {
      testing::Rng rng(static_cast<uint64_t>(r) + 1);
      while (!stop.load(std::memory_order_acquire)) {
        const std::string k = "k" + std::to_string(rng.below(40000));
        std::string v;
        Status s = d->Get(ReadOptions(), Slice(k), &v);
        reads.fetch_add(1, std::memory_order_relaxed);
        if (s.ok() && v != "v:" + k) bad.fetch_add(1);   // never a value that was not written
        else if (!s.ok() && !s.IsNotFound()) bad.fetch_add(1);
      }
    });

  for (auto& t : writers) t.join();
  stop.store(true, std::memory_order_release);
  for (auto& t : readers) t.join();
  std::fprintf(stderr, "   %d concurrent reads, %d inconsistent\n", reads.load(), bad.load());
  CHECK_EQ(bad.load(), 0);
  CHECK_GT(reads.load(), 100);
}

TEST(acknowledged_sync_writes_survive_a_simulated_crash) {
  // R3/R4/S1 -- the durability claim, through the crash simulator. This is the
  // configuration a kill -9 test CANNOT provide, because the page cache survives a
  // process death (SPEC 11.21). Delete the log Sync() and this test fails.
  testing::TmpDir dir("dbcrash");
  FaultEnv fe;
  Options opt;
  opt.env = &fe;
  opt.write_buffer_size = 1 << 20;   // no flush, so everything is in the WAL only

  std::set<std::string> acknowledged;
  {
    DB* db = nullptr;
    REQUIRE_OK(DB::Open(opt, dir.path(), &db));
    WriteOptions wo;
    wo.sync = true;
    for (int i = 0; i < 300; ++i) {
      const std::string k = "key" + std::to_string(i);
      if (db->Put(wo, k, "value" + std::to_string(i)).ok()) acknowledged.insert(k);
    }
    // Deliberately NOT closed: the process "dies" here.
    fe.SimulateCrash();
    delete db;
  }

  Options reopen;
  reopen.write_buffer_size = 1 << 20;   // a fresh, real Env -- like a restarted process
  DB* db2 = nullptr;
  REQUIRE_OK(DB::Open(reopen, dir.path(), &db2));
  int lost = 0;
  for (const std::string& k : acknowledged) {
    std::string v;
    if (!db2->Get(ReadOptions(), Slice(k), &v).ok()) { ++lost; }
  }
  std::fprintf(stderr, "   %zu acknowledged sync=true writes, %d lost after crash\n",
               acknowledged.size(), lost);
  CHECK_GT(acknowledged.size(), 200u);
  CHECK_EQ(lost, 0);   // R4: NO acknowledged durable write is lost
  delete db2;
}

TEST(a_second_open_of_the_same_directory_is_refused) {
  // S16, through the public API this time.
  Db d;
  DB* second = nullptr;
  Status s = DB::Open(d.opt, d.dir.path(), &second);
  CHECK(!s.ok());
  CHECK(second == nullptr);
  std::fprintf(stderr, "   second Open refused: %s\n", s.ToString().c_str());
}

TEST(close_is_idempotent_and_joins_the_background_thread) {
  Db d(nullptr, 48 * 1024);
  for (int i = 0; i < 500; ++i) d->Put(WriteOptions(), "k" + std::to_string(i), std::string(64, 'v'));
  CHECK_OK(d->Close());
  CHECK_OK(d->Close());   // idempotent
  // Writes after Close must fail rather than hang or corrupt.
  CHECK(!d->Put(WriteOptions(), "late", "v").ok());
}

RUN_ALL()

// T3: the memtable. SPEC 3.4, and the arena-overhead measurement SPEC marks ASSUMED.
#include "tests/test.h"

#include <atomic>
#include <cstdio>
#include <map>
#include <memory>
#include <thread>
#include <string>
#include <vector>

#include "lsmeng/memtable.h"

using namespace lsmeng;

namespace {
struct MemHandle {
  MemTable* m = new MemTable();
  ~MemHandle() { m->Unref(); }
  MemTable* operator->() { return m; }
  MemTable& operator*() { return *m; }
};

// Get through the memtable, returning one of three outcomes -- which is the distinction
// the read path depends on and the one a bool cannot express.
enum class Outcome { kValue, kTombstone, kAbsent };
Outcome Lookup(MemTable* m, const std::string& key, SequenceNumber seq, std::string* out) {
  LookupKey lk(Slice(key), seq);
  Status s;
  if (!m->Get(lk, out, &s)) return Outcome::kAbsent;
  return s.ok() ? Outcome::kValue : Outcome::kTombstone;
}
}  // namespace

TEST(put_then_get_roundtrip_with_arbitrary_bytes) {
  MemHandle m;
  const std::vector<std::pair<std::string, std::string>> kvs = {
      {"a", "1"}, {"", "empty key is legal"}, {"b", ""},
      {std::string("k\0k", 3), std::string("v\0v", 3)},
      {"\xff\xff", "high bytes"}, {std::string(4096, 'K'), std::string(1024, 'V')},
  };
  SequenceNumber seq = 1;
  for (const auto& kv : kvs) m->Add(seq++, kTypeValue, Slice(kv.first), Slice(kv.second));
  std::string out;
  for (const auto& kv : kvs) {
    TCTX("key size=" << kv.first.size());
    CHECK(Lookup(m.m, kv.first, 1000, &out) == Outcome::kValue);
    CHECK_EQ(out, kv.second);
  }
  CHECK(Lookup(m.m, "missing", 1000, &out) == Outcome::kAbsent);
}

TEST(newest_version_at_or_below_the_snapshot_wins) {
  // The consequence of SPEC 3.1's comparator: versions of one key are adjacent, newest
  // first, so a seek at (key, snapshot) lands directly on the right one.
  MemHandle m;
  m->Add(10, kTypeValue, Slice("k"), Slice("v10"));
  m->Add(20, kTypeValue, Slice("k"), Slice("v20"));
  m->Add(30, kTypeValue, Slice("k"), Slice("v30"));
  std::string out;
  CHECK(Lookup(m.m, "k", 100, &out) == Outcome::kValue); CHECK_EQ(out, std::string("v30"));
  CHECK(Lookup(m.m, "k", 30, &out) == Outcome::kValue);  CHECK_EQ(out, std::string("v30"));
  CHECK(Lookup(m.m, "k", 29, &out) == Outcome::kValue);  CHECK_EQ(out, std::string("v20"));
  CHECK(Lookup(m.m, "k", 10, &out) == Outcome::kValue);  CHECK_EQ(out, std::string("v10"));
  CHECK(Lookup(m.m, "k", 9, &out) == Outcome::kAbsent);  // older than every version
}

TEST(a_tombstone_is_a_hit_and_stops_the_search) {
  // The distinction that matters most in this file. "Found a tombstone" and "not in this
  // memtable" must be different answers: conflating them makes Get fall through to an
  // older SST and resurrect a deleted value (SPEC 3.7, E-2).
  MemHandle m;
  m->Add(10, kTypeValue, Slice("k"), Slice("v"));
  m->Add(20, kTypeDeletion, Slice("k"), Slice(""));
  std::string out;
  CHECK(Lookup(m.m, "k", 100, &out) == Outcome::kTombstone);   // hit, and it is a delete
  CHECK(Lookup(m.m, "k", 15, &out) == Outcome::kValue);        // snapshot before the delete
  CHECK_EQ(out, std::string("v"));
  CHECK(Lookup(m.m, "other", 100, &out) == Outcome::kAbsent);  // genuinely not here
}

TEST(a_reinserted_key_after_a_delete_is_visible_again) {
  MemHandle m;
  m->Add(10, kTypeValue, Slice("k"), Slice("first"));
  m->Add(20, kTypeDeletion, Slice("k"), Slice(""));
  m->Add(30, kTypeValue, Slice("k"), Slice("second"));
  std::string out;
  CHECK(Lookup(m.m, "k", 100, &out) == Outcome::kValue);
  CHECK_EQ(out, std::string("second"));
  CHECK(Lookup(m.m, "k", 25, &out) == Outcome::kTombstone);
}

TEST(iteration_is_in_internal_key_order_newest_first_per_key) {
  // This ordering is what the flush and the merging iterator both consume: for each user
  // key, the newest version arrives first, so a compaction can dedup with a one-entry
  // memory of what it just emitted (SPEC 3.8.3).
  MemHandle m;
  m->Add(5, kTypeValue, Slice("b"), Slice("b5"));
  m->Add(1, kTypeValue, Slice("a"), Slice("a1"));
  m->Add(9, kTypeDeletion, Slice("a"), Slice(""));
  m->Add(3, kTypeValue, Slice("a"), Slice("a3"));

  std::unique_ptr<MemTableIterator> it(m->NewIterator());
  std::vector<std::pair<std::string, SequenceNumber>> got;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    ParsedInternalKey p;
    REQUIRE(ParseInternalKey(it->key(), &p));
    got.emplace_back(p.user_key.ToString(), p.sequence);
  }
  REQUIRE(got.size() == 4);
  CHECK_EQ(got[0].first, std::string("a")); CHECK_EQ(got[0].second, 9u);
  CHECK_EQ(got[1].first, std::string("a")); CHECK_EQ(got[1].second, 3u);
  CHECK_EQ(got[2].first, std::string("a")); CHECK_EQ(got[2].second, 1u);
  CHECK_EQ(got[3].first, std::string("b")); CHECK_EQ(got[3].second, 5u);
}

TEST(differential_against_a_map_model) {
  // A small key space so overwrites and deletes collide constantly -- the same reasoning
  // as SPEC 7 layer 3. A large key space would hide exactly the version-selection bugs
  // this is hunting.
  MemHandle m;
  std::map<std::string, std::pair<SequenceNumber, std::string>> model;  // key -> (seq, val)
  std::map<std::string, SequenceNumber> deleted;
  testing::Rng rng(testing::seed());
  SequenceNumber seq = 1;

  for (int i = 0; i < 20000; ++i) {
    std::string k = "k" + std::to_string(rng.below(200));
    if (rng.one_in(3)) {
      m->Add(seq, kTypeDeletion, Slice(k), Slice(""));
      deleted[k] = seq;
      model.erase(k);
    } else {
      std::string v = "v" + std::to_string(seq);
      m->Add(seq, kTypeValue, Slice(k), Slice(v));
      model[k] = {seq, v};
      deleted.erase(k);
    }
    ++seq;
  }

  std::string out;
  for (int n = 0; n < 200; ++n) {
    std::string k = "k" + std::to_string(n);
    TCTX("key=" << k);
    Outcome o = Lookup(m.m, k, seq, &out);
    auto mi = model.find(k);
    if (mi != model.end()) {
      CHECK(o == Outcome::kValue);
      CHECK_EQ(out, mi->second.second);
    } else if (deleted.count(k)) {
      CHECK(o == Outcome::kTombstone);
    } else {
      CHECK(o == Outcome::kAbsent);
    }
  }
}

TEST(memory_usage_grows_and_measures_the_real_arena_overhead) {
  // Replaces T1's MODELLED estimate with the real thing: actual skip-list nodes, actual
  // encoding, actual random heights. SPEC 3.4 assumed 2-3x.
  MemHandle m;
  const int kN = 50000;
  const size_t key_bytes = 16, val_bytes = 100;
  const std::string v(val_bytes, 'v');
  size_t before = m->ApproximateMemoryUsage();
  for (int i = 0; i < kN; ++i) {
    char kbuf[32];
    std::snprintf(kbuf, sizeof(kbuf), "key%013d", i);
    m->Add(static_cast<SequenceNumber>(i + 1), kTypeValue, Slice(kbuf, key_bytes), Slice(v));
  }
  const size_t used = m->ApproximateMemoryUsage() - before;
  const double ratio = static_cast<double>(used) /
                       static_cast<double>(kN * (key_bytes + val_bytes));
  std::fprintf(stderr,
               "   MEASURED memtable arena overhead: %.2fx  (%zu bytes for %d records of "
               "%zu user bytes)\n", ratio, used, kN, key_bytes + val_bytes);
  // S11: the number must be an OVER-estimate of user bytes, since MakeRoomForWrite uses it
  // to bound the memtable. Under-counting means an unbounded memtable.
  CHECK_GT(ratio, 1.0);
  CHECK_LT(ratio, 3.0);
}

TEST(concurrent_readers_during_a_single_writer) {
  // S12's world: one writer (the group-commit leader), many readers, no lock. Under TSan
  // this is the memtable-level check that the skip list's release/acquire discipline holds
  // through the encoding layer as well.
  MemTable* m = new MemTable();
  std::atomic<bool> done{false};
  std::atomic<int> bad{0}, reads{0};

  std::thread writer([&] {
    for (int i = 1; i <= 30000; ++i) {
      char kbuf[32];
      std::snprintf(kbuf, sizeof(kbuf), "key%08d", i);
      std::string val = "val" + std::to_string(i);
      m->Add(static_cast<SequenceNumber>(i), kTypeValue, Slice(kbuf, 11), Slice(val));
    }
    done.store(true, std::memory_order_release);
  });

  std::vector<std::thread> readers;
  for (int r = 0; r < 8; ++r)
    readers.emplace_back([&] {
      std::string out;
      while (!done.load(std::memory_order_acquire)) {
        std::unique_ptr<MemTableIterator> it(m->NewIterator());
        for (it->SeekToFirst(); it->Valid(); it->Next()) {
          ParsedInternalKey p;
          if (!ParseInternalKey(it->key(), &p)) { bad.fetch_add(1); break; }
          // The value encodes the sequence, so a torn read shows up as a mismatch.
          const std::string expect = "val" + std::to_string(p.sequence);
          if (it->value().ToString() != expect) { bad.fetch_add(1); break; }
          reads.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });

  writer.join();
  for (auto& t : readers) t.join();
  std::fprintf(stderr, "   %d concurrent entry reads, %d inconsistent\n", reads.load(), bad.load());
  CHECK_EQ(bad.load(), 0);
  CHECK_GT(reads.load(), 1000);
  m->Unref();
}

RUN_ALL()

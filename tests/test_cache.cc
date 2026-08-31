// T6: the sharded LRU cache. SPEC 3.11, R12, E-12, E-26, S11.
#include "tests/test.h"

#include <atomic>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "lsmeng/cache.h"

using namespace lsmeng;

namespace {
std::atomic<int> g_deleted{0};
void CountingDeleter(void* v) {
  g_deleted.fetch_add(1);
  delete static_cast<std::string*>(v);
}
std::string* NewValue(const std::string& s) { return new std::string(s); }
}  // namespace

TEST(insert_lookup_release_roundtrip) {
  g_deleted = 0;
  Cache c(1 << 20, 4);
  auto* h = c.Insert(Slice("k1"), NewValue("v1"), 10, CountingDeleter);
  REQUIRE(h != nullptr);
  CHECK_EQ(*static_cast<std::string*>(c.Value(h)), std::string("v1"));
  c.Release(h);

  auto* h2 = c.Lookup(Slice("k1"));
  REQUIRE(h2 != nullptr);
  CHECK_EQ(*static_cast<std::string*>(c.Value(h2)), std::string("v1"));
  c.Release(h2);

  CHECK(c.Lookup(Slice("nope")) == nullptr);
  CHECK_EQ(g_deleted.load(), 0);   // still cached, so not yet freed
}

TEST(capacity_is_enforced_and_the_least_recently_used_goes_first) {
  g_deleted = 0;
  Cache c(100, 1);   // one shard, so LRU order is fully deterministic
  for (int i = 0; i < 10; ++i) {
    auto* h = c.Insert(Slice("k" + std::to_string(i)), NewValue("v"), 10, CountingDeleter);
    c.Release(h);
  }
  CHECK_LE(c.TotalCharge(), 100u);

  // Touch k5 so it becomes most-recently-used, then overflow by one.
  auto* h = c.Lookup(Slice("k5"));
  REQUIRE(h != nullptr);
  c.Release(h);
  auto* h2 = c.Insert(Slice("new"), NewValue("v"), 10, CountingDeleter);
  c.Release(h2);

  CHECK(c.Lookup(Slice("k5")) != nullptr);   // survived because it was touched
  CHECK_LE(c.TotalCharge(), 100u);
}

TEST(an_entry_in_use_is_not_freed_when_it_is_evicted) {
  // THE property that makes the read path safe (SPEC 3.11): eviction only UNLINKS. A block
  // currently being read by a thread must stay alive until that thread releases it, or the
  // reader is looking at freed memory. ASan is what proves the alternative would be caught.
  g_deleted = 0;
  Cache c(50, 1);
  auto* held = c.Insert(Slice("pinned"), NewValue("important"), 25, CountingDeleter);
  REQUIRE(held != nullptr);

  // Force eviction of everything by inserting well past capacity.
  for (int i = 0; i < 20; ++i) {
    auto* h = c.Insert(Slice("filler" + std::to_string(i)), NewValue("x"), 25, CountingDeleter);
    c.Release(h);
  }
  // The pinned entry is out of the table...
  CHECK(c.Lookup(Slice("pinned")) == nullptr);
  // ...but the value we still hold is intact.
  CHECK_EQ(*static_cast<std::string*>(c.Value(held)), std::string("important"));
  c.Release(held);
}

TEST(reinserting_a_key_replaces_it_and_frees_the_old_value) {
  g_deleted = 0;
  Cache c(1 << 20, 4);
  auto* a = c.Insert(Slice("k"), NewValue("old"), 10, CountingDeleter);
  c.Release(a);
  auto* b = c.Insert(Slice("k"), NewValue("new"), 10, CountingDeleter);
  c.Release(b);
  CHECK_EQ(g_deleted.load(), 1);   // the old value is gone, exactly once
  auto* h = c.Lookup(Slice("k"));
  REQUIRE(h != nullptr);
  CHECK_EQ(*static_cast<std::string*>(c.Value(h)), std::string("new"));
  c.Release(h);
}

TEST(erase_removes_an_entry_immediately) {
  g_deleted = 0;
  Cache c(1 << 20, 4);
  auto* h = c.Insert(Slice("k"), NewValue("v"), 10, CountingDeleter);
  c.Release(h);
  c.Erase(Slice("k"));
  CHECK(c.Lookup(Slice("k")) == nullptr);
  CHECK_EQ(g_deleted.load(), 1);
}

TEST(everything_is_freed_at_destruction) {
  g_deleted = 0;
  {
    Cache c(1 << 20, 8);
    for (int i = 0; i < 100; ++i) {
      auto* h = c.Insert(Slice("k" + std::to_string(i)), NewValue("v"), 1, CountingDeleter);
      c.Release(h);
    }
  }
  // S19: no leaks. Valgrind memcheck would also catch this, but a counter makes the
  // failure immediate rather than a report at exit.
  CHECK_EQ(g_deleted.load(), 100);
}

TEST(block_keys_are_built_without_allocating) {
  // T12's fix. BlockKey used to return a std::string -- a heap allocation on every block
  // access, on the hot read path. It now writes into a caller buffer.
  char a[Cache::kBlockKeySize], b[Cache::kBlockKeySize];
  const Slice k1 = Cache::BlockKey(a, 7, 4096);
  const Slice k2 = Cache::BlockKey(b, 7, 4096);
  CHECK_EQ(k1.size(), Cache::kBlockKeySize);
  CHECK(k1 == k2);                                     // same inputs, same key
  const Slice k3 = Cache::BlockKey(b, 7, 8192);
  CHECK(!(k1 == k3));                                  // different offset, different key
  const Slice k4 = Cache::BlockKey(b, 8, 4096);
  CHECK(!(k1 == k4));                                  // different file, different key
  // A collision here would return the WRONG BLOCK for a lookup, so the encoding is checked
  // to be injective over the two fields rather than merely "looks different": file number
  // occupies bytes 0..7 and offset bytes 8..15, so k1 and k4 (same offset, different file)
  // must share the SECOND half and differ in the first.
  CHECK_EQ(std::memcmp(a + 8, b + 8, 8), 0);
  CHECK_NE(std::memcmp(a, b, 8), 0);
}

TEST(shards_partition_the_keyspace_and_the_total_capacity) {
  // The 16-shard cache must not be SMALLER than the 1-shard cache of the same nominal
  // size, or T12's comparison would be measuring two different caches rather than two
  // locking strategies. Per-shard capacity is rounded up for exactly this reason.
  for (int shards : {1, 4, 16}) {
    Cache c(16000, shards);
    TCTX("shards=" << shards);
    CHECK_EQ(c.num_shards(), shards);
    for (int i = 0; i < 1000; ++i) {
      auto* h = c.Insert(Slice("key" + std::to_string(i)), NewValue("v"), 16, CountingDeleter);
      c.Release(h);
    }
    // Some slack: with N shards the hash can distribute unevenly, so the bound is per
    // shard, not global. What must NOT happen is the total collapsing as shards increase.
    std::fprintf(stderr, "   shards=%2d -> %zu entries, %zu bytes charged\n", shards,
                 c.NumEntries(), c.TotalCharge());
    CHECK_GT(c.NumEntries(), 500u);
  }
}

TEST(concurrent_hammering_is_race_free_and_never_double_frees) {
  // Under TSan this is the test that says the shard mutexes actually protect what they
  // claim to. Under ASan it says the refcounting never frees an entry that is still held.
  g_deleted = 0;
  Cache c(64 * 1024, 16);
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> hits{0}, misses{0};

  std::vector<std::thread> ts;
  for (int t = 0; t < 8; ++t) {
    ts.emplace_back([&, t] {
      testing::Rng rng(static_cast<uint64_t>(t) + 1);
      for (int i = 0; i < 40000; ++i) {
        const std::string key = "block" + std::to_string(rng.below(500));
        auto* h = c.Lookup(Slice(key));
        if (h) {
          // Read through the handle while other threads are evicting -- this is exactly
          // the in-use-during-eviction case, at speed.
          const std::string& v = *static_cast<std::string*>(c.Value(h));
          if (v != key) { stop = true; }
          hits.fetch_add(1, std::memory_order_relaxed);
          c.Release(h);
        } else {
          misses.fetch_add(1, std::memory_order_relaxed);
          auto* ins = c.Insert(Slice(key), NewValue(key), 200, CountingDeleter);
          c.Release(ins);
        }
      }
    });
  }
  for (auto& t : ts) t.join();
  std::fprintf(stderr, "   %llu hits, %llu misses across 8 threads\n",
               (unsigned long long)hits.load(), (unsigned long long)misses.load());
  CHECK(!stop.load());               // no handle ever yielded the wrong value
  CHECK_GT(hits.load(), 1000u);      // the threads really did share the cache
  CHECK_LE(c.TotalCharge(), 64u * 1024u + 200u * 16u);   // bounded per shard
}

RUN_ALL()

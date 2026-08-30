// T3: the skip list. SPEC 3.4, S6, S12.
//
// Two kinds of test here, and they answer different questions:
//   * against std::set -- is the ORDERING and the SEARCH correct?
//   * one writer + eight readers under TSan -- is the MEMORY ORDERING correct?
// The second cannot be answered by checking values, because a missing release/acquire pair
// produces correct results on x86 almost always and on aarch64 usually. Only an
// instrumented build sees it (the same lesson as CHALLENGES B7).
#include "tests/test.h"

#include <atomic>
#include <set>
#include <thread>
#include <vector>

#include "lsmeng/arena.h"
#include "lsmeng/skiplist.h"

using namespace lsmeng;

namespace {
struct U64Comparator {
  int operator()(const uint64_t& a, const uint64_t& b) const {
    return a < b ? -1 : (a > b ? 1 : 0);
  }
};
using TestList = SkipList<uint64_t, U64Comparator>;
}  // namespace

TEST(empty_list_finds_nothing) {
  Arena arena;
  TestList list(U64Comparator(), &arena);
  CHECK(!list.Contains(10));
  TestList::Iterator it(&list);
  CHECK(!it.Valid());
  it.SeekToFirst();
  CHECK(!it.Valid());
  it.SeekToLast();
  CHECK(!it.Valid());
  it.Seek(100);
  CHECK(!it.Valid());
}

TEST(matches_std_set_over_random_inserts_and_seeks) {
  Arena arena;
  TestList list(U64Comparator(), &arena);
  std::set<uint64_t> model;
  testing::Rng rng(testing::seed());

  const int kN = 20000;
  for (int i = 0; i < kN; ++i) {
    // A small key space on purpose, so collisions are frequent -- but the skip list
    // requires uniqueness, so the model decides.
    uint64_t k = rng.below(kN * 4);
    if (model.insert(k).second) list.Insert(k);
  }

  for (uint64_t k : model) { TCTX("k=" << k); CHECK(list.Contains(k)); }

  // Forward iteration must reproduce the model exactly, in order.
  {
    TestList::Iterator it(&list);
    auto mi = model.begin();
    for (it.SeekToFirst(); it.Valid(); it.Next(), ++mi) {
      REQUIRE(mi != model.end());
      CHECK_EQ(it.key(), *mi);
    }
    CHECK(mi == model.end());
  }
  // Reverse iteration must reproduce it backwards. SPEC E-15: forward and reverse
  // disagreeing is a real class of bug, and Prev() here walks from the head rather than
  // following back pointers, so it is a genuinely different code path.
  {
    TestList::Iterator it(&list);
    auto mi = model.rbegin();
    for (it.SeekToLast(); it.Valid(); it.Prev(), ++mi) {
      REQUIRE(mi != model.rend());
      CHECK_EQ(it.key(), *mi);
    }
    CHECK(mi == model.rend());
  }
  // Seek must land on the first key >= target, for targets both in and out of the set.
  for (int i = 0; i < 2000; ++i) {
    uint64_t target = rng.below(kN * 5);
    TCTX("target=" << target);
    TestList::Iterator it(&list);
    it.Seek(target);
    auto mi = model.lower_bound(target);
    if (mi == model.end()) CHECK(!it.Valid());
    else { REQUIRE(it.Valid()); CHECK_EQ(it.key(), *mi); }
  }
}

TEST(sequential_and_reverse_insertion_orders_both_work) {
  // Degenerate insertion orders are where a search that fails to descend properly shows
  // up: ascending inserts always append at the tail, descending always prepend at the head.
  for (bool ascending : {true, false}) {
    Arena arena;
    TestList list(U64Comparator(), &arena);
    const int kN = 5000;
    for (int i = 0; i < kN; ++i) list.Insert(ascending ? i : (kN - 1 - i));
    TCTX("ascending=" << ascending);
    for (int i = 0; i < kN; ++i) CHECK(list.Contains(i));
    TestList::Iterator it(&list);
    int expect = 0;
    for (it.SeekToFirst(); it.Valid(); it.Next()) CHECK_EQ(it.key(), static_cast<uint64_t>(expect++));
    CHECK_EQ(expect, kN);
  }
}

TEST(one_writer_and_eight_readers_never_observe_a_partial_node) {
  // SPEC 3.4 / S6. THE test for the release/acquire pair.
  //
  // The writer inserts keys whose two halves are redundant: key = (n << 32) | n. A reader
  // that observes a node before its key is fully written would see the halves disagree --
  // which is the visible symptom of the missing barrier. Under TSan, the race itself is
  // reported whether or not the symptom appears, which is the real point: this test
  // supplies the interleaving, TSan supplies the verdict (the CHALLENGES B7 lesson).
  Arena arena;
  TestList list(U64Comparator(), &arena);
  std::atomic<bool> done{false};
  std::atomic<uint64_t> highest{0};
  std::atomic<int> torn{0};        // a node observed with a half-written key
  std::atomic<int> from_future{0}; // a key newer than anything published
  std::atomic<int> reads{0};

  std::thread writer([&] {
    for (uint64_t n = 1; n <= 40000; ++n) {
      list.Insert((n << 32) | n);
      highest.store(n, std::memory_order_release);
    }
    done.store(true, std::memory_order_release);
  });

  std::vector<std::thread> readers;
  for (int r = 0; r < 8; ++r) {
    readers.emplace_back([&] {
      while (!done.load(std::memory_order_acquire)) {
        TestList::Iterator it(&list);
        for (it.SeekToFirst(); it.Valid(); it.Next()) {
          const uint64_t k = it.key();
          reads.fetch_add(1, std::memory_order_relaxed);

          // (a) The real invariant: a node's two halves are written together, so a reader
          //     that can see the node must see both. A mismatch is the visible symptom of
          //     a missing release/acquire pair.
          if ((k >> 32) != (k & 0xFFFFFFFF)) { torn.fetch_add(1, std::memory_order_relaxed); break; }

          // (b) No key from the future. `highest` is loaded AFTER observing k, which is
          //     the only correct order: the writer does Insert(n) and THEN stores
          //     highest=n, so a reader can legitimately see n while highest is still n-1.
          //     Loading `highest` once BEFORE a long traversal -- which is what this test
          //     did originally -- compares late observations against a stale bound and
          //     reports failures that are the TEST's bug, not the list's. See CHALLENGES B9.
          const uint64_t hi_now = highest.load(std::memory_order_acquire);
          if ((k & 0xFFFFFFFF) > hi_now + 1) {
            from_future.fetch_add(1, std::memory_order_relaxed);
            break;
          }
        }
      }
    });
  }

  writer.join();
  for (auto& t : readers) t.join();
  std::fprintf(stderr, "   %d reads across 8 readers, %d torn, %d from-future\n",
               reads.load(), torn.load(), from_future.load());
  CHECK_EQ(torn.load(), 0);
  CHECK_EQ(from_future.load(), 0);
  CHECK_GT(reads.load(), 1000);   // the readers must actually have raced, not idled
}

TEST(readers_see_every_key_the_writer_finished_publishing) {
  // The completeness half of the previous test: after the writer is done, a reader must
  // find all of it. A lost update here would mean a write that was acknowledged and then
  // silently vanished.
  Arena arena;
  TestList list(U64Comparator(), &arena);
  const int kN = 30000;
  std::atomic<int> found{0};
  std::thread writer([&] { for (int i = 0; i < kN; ++i) list.Insert(static_cast<uint64_t>(i)); });
  writer.join();
  std::vector<std::thread> readers;
  for (int r = 0; r < 4; ++r)
    readers.emplace_back([&] {
      int c = 0;
      TestList::Iterator it(&list);
      for (it.SeekToFirst(); it.Valid(); it.Next()) ++c;
      found.fetch_add(c == kN ? 1 : 0);
    });
  for (auto& t : readers) t.join();
  CHECK_EQ(found.load(), 4);
}

TEST(height_growth_does_not_lose_nodes) {
  // max_height_ is stored with relaxed ordering, justified in skiplist.h by the argument
  // that a reader seeing a stale height starts lower and still finds everything. This
  // exercises the transition: many inserts, each possibly raising the height.
  Arena arena;
  TestList list(U64Comparator(), &arena);
  testing::Rng rng(testing::seed());
  std::set<uint64_t> model;
  for (int i = 0; i < 50000; ++i) {
    uint64_t k = rng.next();
    if (model.insert(k).second) list.Insert(k);
  }
  size_t n = 0;
  TestList::Iterator it(&list);
  for (it.SeekToFirst(); it.Valid(); it.Next()) ++n;
  CHECK_EQ(n, model.size());
}

RUN_ALL()

// T1: the arena. SPEC 3.4 -- and it measures the arena-vs-user byte ratio that SPEC
// marks ASSUMED, which is the number that decides how much a "4 MiB memtable" holds.
#include "tests/test.h"

#include <atomic>
#include <set>
#include <thread>
#include <vector>

#include "lsmeng/arena.h"

using namespace lsmeng;

TEST(allocations_are_distinct_non_overlapping_and_writable) {
  Arena a;
  testing::Rng rng(testing::seed());
  std::vector<std::pair<char*, size_t>> blocks;
  for (int i = 0; i < 2000; ++i) {
    size_t n = 1 + rng.below(300);
    char* p = a.Allocate(n);
    REQUIRE(p != nullptr);
    std::memset(p, static_cast<int>(i & 0xFF), n);   // ASan catches an overlap here
    blocks.emplace_back(p, n);
  }
  for (size_t i = 0; i < blocks.size(); ++i) {
    TCTX("block=" << i);
    for (size_t k = 0; k < blocks[i].second; ++k)
      CHECK_EQ(static_cast<int>(static_cast<unsigned char>(blocks[i].first[k])),
               static_cast<int>(i & 0xFF));
  }
}

TEST(zero_byte_allocation_still_returns_a_distinct_pointer) {
  // Returning the same pointer twice would make two different nodes compare equal, which
  // is a subtle aliasing bug in anything that uses pointer identity.
  Arena a;
  std::set<char*> seen;
  for (int i = 0; i < 100; ++i) CHECK(seen.insert(a.Allocate(0)).second);
}

TEST(aligned_allocation_is_actually_aligned) {
  Arena a;
  testing::Rng rng(testing::seed());
  for (int i = 0; i < 500; ++i) {
    a.Allocate(1 + rng.below(7));                     // deliberately misalign the bump ptr
    char* p = a.AllocateAligned(1 + rng.below(64));
    CHECK_EQ(reinterpret_cast<uintptr_t>(p) % 8, 0u); // UBSan catches the alternative
  }
}

TEST(an_allocation_larger_than_a_block_succeeds) {
  // The fallback path: a request bigger than kBlockSize must get its own block rather
  // than loop or return a short buffer. SPEC 3.1 allows 1 MiB values.
  Arena a;
  for (size_t n : {size_t{4096}, size_t{4097}, size_t{64 * 1024}, size_t{1 << 20}}) {
    TCTX("n=" << n);
    char* p = a.Allocate(n);
    REQUIRE(p != nullptr);
    std::memset(p, 0xAB, n);
    CHECK_EQ(static_cast<int>(static_cast<unsigned char>(p[n - 1])), 0xAB);
  }
}

TEST(memory_usage_grows_monotonically_and_bounds_the_bytes_handed_out) {
  Arena a;
  size_t last = a.memory_usage();
  size_t handed_out = 0;
  for (int i = 0; i < 5000; ++i) {
    size_t n = 1 + (i % 97);
    a.Allocate(n);
    handed_out += n;
    size_t now = a.memory_usage();
    CHECK_GE(now, last);
    last = now;
  }
  // S11 depends on this being an over-estimate, never an under-estimate: MakeRoomForWrite
  // uses it to decide when to freeze, and an under-count means an unbounded memtable.
  CHECK_GE(a.memory_usage(), handed_out);
}

TEST(measure_arena_overhead_ratio_for_a_realistic_record) {
  // The number SPEC 3.4 marks ASSUMED at "roughly 2-3x for small records". A memtable
  // entry is a skip-list node: height pointers + the encoded internal key + the value.
  // This models it directly rather than guessing.
  struct FakeNode { std::atomic<void*> next[4]; };   // average height at p=1/4
  const size_t key_bytes = 16 + 8;    // user key + packed seq/type
  const size_t val_bytes = 100;
  const size_t user_bytes_per_record = 16 + 100;

  Arena a;
  const int kRecords = 20000;
  for (int i = 0; i < kRecords; ++i)
    a.AllocateAligned(sizeof(FakeNode) + key_bytes + val_bytes + 8 /* varint prefixes */);

  const double ratio = static_cast<double>(a.memory_usage()) /
                       static_cast<double>(kRecords * user_bytes_per_record);
  std::fprintf(stderr,
               "   MEASURED arena overhead: %.2fx  (%zu arena bytes for %d records of "
               "%zu user bytes)\n",
               ratio, a.memory_usage(), kRecords, user_bytes_per_record);
  // A loose assertion on purpose: the point is to RECORD the number, not to freeze it.
  // A ratio outside this range means the node layout changed enough that the memtable
  // sizing needs rethinking, which is worth failing for.
  CHECK_GT(ratio, 1.0);
  CHECK_LT(ratio, 4.0);
}

RUN_ALL()

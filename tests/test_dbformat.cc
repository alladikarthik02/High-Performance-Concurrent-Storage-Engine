// T1: the internal key encoding and THE comparator (SPEC 3.1, S10).
//
// If the comparator is wrong the database returns WRONG ANSWERS rather than crashing, and
// four independent components (memtable, block builder, merging iterator, compaction) all
// depend on it agreeing with itself. So it is brute-forced rather than spot-checked.
#include "tests/test.h"

#include <algorithm>
#include <string>
#include <vector>

#include "lsmeng/dbformat.h"

using namespace lsmeng;

namespace {
std::string IKey(const std::string& user, SequenceNumber seq, ValueType t) {
  std::string s;
  AppendInternalKey(&s, ParsedInternalKey{Slice(user), seq, t});
  return s;
}

// The corpus deliberately includes everything that has ever broken a key comparator:
// the empty key, prefixes of each other, embedded NULs, and 0xFF bytes (which break any
// implementation that treats char as signed).
std::vector<std::string> UserKeyCorpus() {
  return {
      "", "a", "ab", "abc", "abcd", "b",
      std::string("a\0", 2), std::string("a\0b", 3), std::string("\0", 1),
      "\xff", "\xff\xff", "\xff\x00", std::string("a\xff", 2),
      "user:1001:email", "user:1001:name", "user:1002:name", "user:10", "user:2",
  };
}
}  // namespace

TEST(pack_and_parse_roundtrip) {
  for (const auto& u : UserKeyCorpus()) {
    for (SequenceNumber seq : {SequenceNumber{1}, SequenceNumber{2}, SequenceNumber{1ull << 30},
                               kMaxSequenceNumber}) {
      for (ValueType t : {kTypeValue, kTypeDeletion}) {
        TCTX("user=" << u.size() << "B seq=" << seq << " type=" << int(t));
        std::string ik = IKey(u, seq, t);
        ParsedInternalKey p;
        REQUIRE(ParseInternalKey(Slice(ik), &p));
        CHECK_EQ(p.user_key.ToString(), u);
        CHECK_EQ(p.sequence, seq);
        CHECK_EQ(int(p.type), int(t));
        // E-17: split by LENGTH, never by scanning. A user key ending in bytes that look
        // like a packed sequence number must still round-trip.
        CHECK_EQ(ExtractUserKey(Slice(ik)).ToString(), u);
      }
    }
  }
}

TEST(malformed_internal_keys_are_rejected) {
  ParsedInternalKey p;
  CHECK(!ParseInternalKey(Slice("", 0), &p));
  CHECK(!ParseInternalKey(Slice("1234567", 7), &p));   // one byte short of the trailer
  std::string bad = IKey("k", 5, kTypeValue);
  bad[bad.size() - 8] = static_cast<char>(0x7F);       // an undefined value type
  CHECK(!ParseInternalKey(Slice(bad), &p));
}

TEST(user_key_ordering_is_bytewise_with_shorter_first_on_a_prefix_tie) {
  // E-16. "abc" < "abcd". strcmp would stop at a NUL; memcmp without the length tiebreak
  // would call them equal. Both mistakes lose exactly the keys that are prefixes.
  CHECK_LT(Slice("abc").compare(Slice("abcd")), 0);
  CHECK_GT(Slice("abcd").compare(Slice("abc")), 0);
  CHECK_EQ(Slice("abc").compare(Slice("abc")), 0);
  CHECK_LT(Slice("", 0).compare(Slice("a")), 0);
  // 0xFF must compare as the LARGEST byte. If char is signed and the code uses it
  // directly, this is the assertion that fails.
  CHECK_GT(Slice("\xff").compare(Slice("\x01")), 0);
  CHECK_LT(Slice(std::string("a\0", 2)).compare(Slice("ab")), 0);
}

TEST(same_user_key_sorts_newest_first) {
  // Consequence 1 of SPEC 3.1: all versions of a key are adjacent, newest first, so a Get
  // is one Seek plus "take the first match".
  const std::string older = IKey("k", 10, kTypeValue);
  const std::string newer = IKey("k", 20, kTypeValue);
  CHECK_LT(InternalKeyComparator::Compare(Slice(newer), Slice(older)), 0);
  CHECK_GT(InternalKeyComparator::Compare(Slice(older), Slice(newer)), 0);
  // A tombstone at a higher sequence still sorts before an older value -- which is what
  // makes a delete win over the value it hides.
  const std::string tomb = IKey("k", 30, kTypeDeletion);
  CHECK_LT(InternalKeyComparator::Compare(Slice(tomb), Slice(newer)), 0);
}

TEST(sequence_comparison_does_not_overflow) {
  // Written as two comparisons rather than (b - a) precisely because subtracting two
  // uint64 sequence numbers and returning it as an int is a classic sorting bug that only
  // appears once sequences get large.
  const std::string lo = IKey("k", 1, kTypeValue);
  const std::string hi = IKey("k", kMaxSequenceNumber, kTypeValue);
  CHECK_LT(InternalKeyComparator::Compare(Slice(hi), Slice(lo)), 0);
  CHECK_GT(InternalKeyComparator::Compare(Slice(lo), Slice(hi)), 0);
}

TEST(comparator_is_a_strict_weak_ordering_by_brute_force) {
  // S10. Build every (user key, seq, type) combination from the adversarial corpus and
  // verify the three axioms exhaustively. std::sort has undefined behaviour -- not a wrong
  // answer, undefined behaviour -- if the comparator is not a strict weak ordering, so
  // this is checked directly rather than inferred from "sorting seemed to work".
  std::vector<std::string> keys;
  for (const auto& u : UserKeyCorpus())
    for (SequenceNumber s : {SequenceNumber{1}, SequenceNumber{7}, SequenceNumber{1000}})
      for (ValueType t : {kTypeValue, kTypeDeletion}) keys.push_back(IKey(u, s, t));

  auto less = [](const std::string& a, const std::string& b) {
    return InternalKeyComparator::Compare(Slice(a), Slice(b)) < 0;
  };

  for (const auto& a : keys) CHECK(!less(a, a));                       // irreflexive
  for (const auto& a : keys)
    for (const auto& b : keys) {
      if (less(a, b)) CHECK(!less(b, a));                              // asymmetric
    }
  for (const auto& a : keys)
    for (const auto& b : keys) {
      if (!less(a, b) && !less(b, a))                                  // equivalence...
        for (const auto& c : keys)
          CHECK_EQ(less(a, c), less(b, c));                            // ...is transitive
    }
  std::fprintf(stderr, "   brute-forced %zu keys (%zu^3 transitivity checks bounded)\n",
               keys.size(), keys.size());
}

TEST(sorting_puts_versions_of_a_key_together_newest_first) {
  std::vector<std::string> keys = {
      IKey("b", 5, kTypeValue), IKey("a", 1, kTypeValue), IKey("a", 9, kTypeDeletion),
      IKey("a", 3, kTypeValue), IKey("ab", 2, kTypeValue),
  };
  std::sort(keys.begin(), keys.end(), [](const std::string& x, const std::string& y) {
    return InternalKeyComparator::Compare(Slice(x), Slice(y)) < 0;
  });
  std::vector<std::pair<std::string, SequenceNumber>> got;
  for (const auto& k : keys) {
    ParsedInternalKey p;
    REQUIRE(ParseInternalKey(Slice(k), &p));
    got.emplace_back(p.user_key.ToString(), p.sequence);
  }
  CHECK_EQ(got[0].first, std::string("a")); CHECK_EQ(got[0].second, 9u);
  CHECK_EQ(got[1].first, std::string("a")); CHECK_EQ(got[1].second, 3u);
  CHECK_EQ(got[2].first, std::string("a")); CHECK_EQ(got[2].second, 1u);
  CHECK_EQ(got[3].first, std::string("ab"));
  CHECK_EQ(got[4].first, std::string("b"));
}

TEST(lookup_key_builds_all_three_views_consistently) {
  for (const auto& u : UserKeyCorpus()) {
    TCTX("user size=" << u.size());
    LookupKey lk(Slice(u), 42);
    CHECK_EQ(lk.user_key().ToString(), u);
    CHECK_EQ(lk.internal_key().size(), u.size() + 8);
    ParsedInternalKey p;
    REQUIRE(ParseInternalKey(lk.internal_key(), &p));
    CHECK_EQ(p.sequence, 42u);
    CHECK_EQ(int(p.type), int(kValueTypeForSeek));
    // memtable_key is the internal key with a varint length in front.
    Slice mk = lk.memtable_key();
    uint32_t len = 0;
    const char* after = GetVarint32Ptr(mk.data(), mk.data() + mk.size(), &len);
    REQUIRE(after != nullptr);
    CHECK_EQ(len, static_cast<uint32_t>(u.size() + 8));
    CHECK_EQ(after, lk.internal_key().data());
  }
}

TEST(lookup_key_handles_a_key_larger_than_its_inline_buffer) {
  // The 200-byte inline buffer exists to avoid a heap allocation for ordinary keys. The
  // heap path is the one that never runs in casual testing, so it is exercised on purpose
  // -- SPEC 3.1 allows keys up to 4 KiB.
  const std::string big(4096, 'k');
  LookupKey lk(Slice(big), 7);
  CHECK_EQ(lk.user_key().ToString(), big);
  ParsedInternalKey p;
  REQUIRE(ParseInternalKey(lk.internal_key(), &p));
  CHECK_EQ(p.sequence, 7u);
}

RUN_ALL()

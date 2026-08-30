// T5a: the block builder and reader. SPEC 3.5, E-8, E-9.
//
// The central risk in this file is SILENT: a prefix-compression bug produces a block whose
// bytes are internally consistent, so every CRC passes and the corruption only appears as
// a key that comes back wrong. That is why the main test compares a FULL SCAN against the
// exact input vector rather than spot-checking lookups.
#include "tests/test.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "lsmeng/block.h"
#include "lsmeng/coding.h"
#include "lsmeng/dbformat.h"

using namespace lsmeng;

namespace {
std::string IKey(const std::string& user, SequenceNumber seq = 100) {
  std::string s;
  AppendInternalKey(&s, ParsedInternalKey{Slice(user), seq, kTypeValue});
  return s;
}

// Build a block from sorted (key,value) pairs and hand back the encoded bytes.
std::string BuildBlock(const std::vector<std::pair<std::string, std::string>>& kvs,
                       int restart_interval = 16) {
  BlockBuilder b(restart_interval);
  for (const auto& kv : kvs) b.Add(Slice(kv.first), Slice(kv.second));
  return b.Finish().ToString();
}

// A full forward scan of a block.
std::vector<std::pair<std::string, std::string>> ScanBlock(const std::string& contents) {
  Block block{Slice(contents)};
  std::vector<std::pair<std::string, std::string>> out;
  std::unique_ptr<Iterator> it(block.NewIterator());
  for (it->SeekToFirst(); it->Valid(); it->Next())
    out.emplace_back(it->key().ToString(), it->value().ToString());
  return out;
}
}  // namespace

TEST(full_scan_reproduces_the_exact_input_for_adversarial_key_sets) {
  // The key sets are chosen to break prefix compression specifically:
  //  - all identical prefixes (maximum sharing)
  //  - no shared prefix at all (zero sharing)
  //  - keys that are prefixes of each other
  //  - 0xFF bytes, embedded NULs, the empty key
  std::vector<std::vector<std::string>> corpora = {
      {"", "a", "aa", "aaa", "aaaa", "b"},
      {"user:1001:email", "user:1001:name", "user:1002:email", "user:1002:name"},
      {"aaaaaaaaaaaaaaaaaaaa", "aaaaaaaaaaaaaaaaaaab", "aaaaaaaaaaaaaaaaaaac"},
      {"a", "b", "c", "d", "e", "f", "g", "h"},
      {std::string("\0", 1), std::string("\0\0", 2), std::string("\0a", 2), "\xff", "\xff\xff"},
  };
  // Enough keys to cross several restart intervals in every configuration.
  std::vector<std::string> many;
  for (int i = 0; i < 200; ++i) many.push_back("prefix:" + std::string(3 - std::to_string(i % 100).size(), '0') + std::to_string(i));
  corpora.push_back(many);

  for (size_t c = 0; c < corpora.size(); ++c) {
    for (int ri : {1, 2, 16, 1000}) {   // ri=1 makes every entry a restart; ri=1000 none
      TCTX("corpus=" << c << " restart_interval=" << ri);
      std::vector<std::pair<std::string, std::string>> kvs;
      for (size_t i = 0; i < corpora[c].size(); ++i)
        kvs.emplace_back(IKey(corpora[c][i]), "value-" + std::to_string(i));
      // Internal keys must be added in comparator order.
      std::sort(kvs.begin(), kvs.end(), [](const auto& a, const auto& b) {
        return InternalKeyComparator::Compare(Slice(a.first), Slice(b.first)) < 0;
      });

      const std::string block = BuildBlock(kvs, ri);
      const auto got = ScanBlock(block);
      REQUIRE(got.size() == kvs.size());
      for (size_t i = 0; i < got.size(); ++i) {
        TCTX("entry=" << i);
        CHECK_EQ(got[i].first, kvs[i].first);     // E-8: exact bytes, not just "looks right"
        CHECK_EQ(got[i].second, kvs[i].second);
      }
    }
  }
}

TEST(seek_lands_on_the_first_key_at_or_after_the_target) {
  // The binary search over restart points. An off-by-one here makes exactly the keys on a
  // restart boundary unreachable, which a full scan would NOT catch (E-9's cousin).
  std::vector<std::pair<std::string, std::string>> kvs;
  for (int i = 0; i < 500; ++i) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "k%04d", i * 2);   // even keys only
    kvs.emplace_back(IKey(buf), "v" + std::to_string(i));
  }
  const std::string contents = BuildBlock(kvs, 16);
  Block block{Slice(contents)};

  for (int probe = -2; probe < 1002; ++probe) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "k%04d", probe);
    const std::string target = IKey(buf);
    TCTX("probe=" << probe);

    std::unique_ptr<Iterator> it(block.NewIterator());
    it->Seek(Slice(target));

    // Reference answer, computed independently of the block.
    size_t expect = kvs.size();
    for (size_t i = 0; i < kvs.size(); ++i)
      if (InternalKeyComparator::Compare(Slice(kvs[i].first), Slice(target)) >= 0) { expect = i; break; }

    if (expect == kvs.size()) {
      CHECK(!it->Valid());
    } else {
      REQUIRE(it->Valid());
      CHECK_EQ(it->key().ToString(), kvs[expect].first);
    }
  }
}

TEST(forward_and_reverse_iteration_agree) {
  // E-15's block-level cousin. Prev() restarts at the previous restart point and walks
  // forward, which is a genuinely different code path from Next().
  std::vector<std::pair<std::string, std::string>> kvs;
  for (int i = 0; i < 300; ++i) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "key%05d", i);
    kvs.emplace_back(IKey(buf), std::string(i % 40, 'v'));
  }
  const std::string contents = BuildBlock(kvs, 16);
  Block block{Slice(contents)};

  std::vector<std::pair<std::string, std::string>> backwards;
  std::unique_ptr<Iterator> it(block.NewIterator());
  for (it->SeekToLast(); it->Valid(); it->Prev())
    backwards.emplace_back(it->key().ToString(), it->value().ToString());
  std::reverse(backwards.begin(), backwards.end());

  REQUIRE(backwards.size() == kvs.size());
  for (size_t i = 0; i < kvs.size(); ++i) {
    TCTX("entry=" << i);
    CHECK_EQ(backwards[i].first, kvs[i].first);
    CHECK_EQ(backwards[i].second, kvs[i].second);
  }
}

TEST(seek_then_prev_and_seek_then_next_are_consistent) {
  std::vector<std::pair<std::string, std::string>> kvs;
  for (int i = 0; i < 100; ++i) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "k%03d", i);
    kvs.emplace_back(IKey(buf), "v" + std::to_string(i));
  }
  const std::string contents = BuildBlock(kvs, 8);
  Block block{Slice(contents)};
  for (size_t i = 1; i + 1 < kvs.size(); ++i) {
    TCTX("i=" << i);
    std::unique_ptr<Iterator> it(block.NewIterator());
    it->Seek(Slice(kvs[i].first));
    REQUIRE(it->Valid());
    CHECK_EQ(it->key().ToString(), kvs[i].first);
    it->Prev();
    REQUIRE(it->Valid());
    CHECK_EQ(it->key().ToString(), kvs[i - 1].first);
    it->Next();
    REQUIRE(it->Valid());
    CHECK_EQ(it->key().ToString(), kvs[i].first);
  }
}

TEST(an_empty_block_and_a_single_entry_block_both_work) {
  {
    BlockBuilder b(16);
    const std::string contents = b.Finish().ToString();
    Block block{Slice(contents)};
    CHECK(block.ok());
    std::unique_ptr<Iterator> it(block.NewIterator());
    it->SeekToFirst();
    CHECK(!it->Valid());
    it->Seek(Slice(IKey("anything")));
    CHECK(!it->Valid());
  }
  {
    const std::string contents = BuildBlock({{IKey("only"), "one"}});
    const auto got = ScanBlock(contents);
    REQUIRE(got.size() == 1);
    CHECK_EQ(got[0].second, std::string("one"));
  }
}

TEST(a_malformed_trailer_is_rejected_rather_than_walked_off_the_end) {
  // A block that failed its CRC never reaches the Block class -- but a truncated cache
  // entry or a builder bug could, and the response must be an error iterator, not a wild
  // read. ASan is what proves the alternative would be caught.
  const std::string good = BuildBlock({{IKey("a"), "1"}, {IKey("b"), "2"}});
  CHECK(!Block(Slice("", 0)).ok());
  CHECK(!Block(Slice(good.data(), 3)).ok());
  std::string bad_count = good;
  EncodeFixed32(&bad_count[bad_count.size() - 4], 0xFFFFFFFFu);   // absurd restart count
  Block b{Slice(bad_count)};
  CHECK(!b.ok());
  std::unique_ptr<Iterator> it(b.NewIterator());
  it->SeekToFirst();
  CHECK(!it->Valid());
  CHECK_CODE(it->status(), Status::Code::kCorruption);
}

TEST(a_value_larger_than_a_block_is_stored_whole) {
  // E-28: a 1 MiB value in a 4 KiB block. The block builder must produce one oversized
  // entry rather than looping or truncating.
  const std::string big(1 << 20, 'V');
  const std::string contents = BuildBlock({{IKey("k"), big}});
  const auto got = ScanBlock(contents);
  REQUIRE(got.size() == 1);
  CHECK_EQ(got[0].second.size(), big.size());
  CHECK_EQ(got[0].second, big);
}

RUN_ALL()

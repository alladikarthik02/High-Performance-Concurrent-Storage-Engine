// T7: VersionEdit, VersionSet, MANIFEST, CURRENT, recovery.
// SPEC 3.9, 3.3.1, S5, S9, S14, S23, E-6, E-7, E-18, E-19, E-35, E-36.
#include "tests/test.h"

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "lsmeng/coding.h"
#include "lsmeng/version_set.h"
#include "tests/fault_env.h"
#include "tests/tmpdir.h"

using namespace lsmeng;

namespace {
std::string IKey(const std::string& u, SequenceNumber s, ValueType t = kTypeValue) {
  std::string out;
  AppendInternalKey(&out, ParsedInternalKey{Slice(u), s, t});
  return out;
}

FileMetaData MakeFile(uint64_t number, const std::string& lo, const std::string& hi,
                      uint64_t size = 1000) {
  FileMetaData f;
  f.number = number;
  f.file_size = size;
  f.smallest = IKey(lo, 1);
  f.largest = IKey(hi, 100);
  f.num_entries = 10;
  f.num_deletions = 1;
  return f;
}

struct Fixture {
  testing::TmpDir dir{"vset"};
  Options opt;
  Stats stats;
  std::unique_ptr<Cache> block_cache;
  std::unique_ptr<TableCache> table_cache;
  std::unique_ptr<VersionSet> vset;

  explicit Fixture(Env* env = nullptr) {
    opt.env = env;
    block_cache.reset(new Cache(1 << 20, 4));
    table_cache.reset(new TableCache(dir.path(), opt, block_cache.get(), &stats));
    vset.reset(new VersionSet(dir.path(), opt, table_cache.get(), &stats));
  }
  void Reopen() {
    vset.reset();
    table_cache.reset(new TableCache(dir.path(), opt, block_cache.get(), &stats));
    vset.reset(new VersionSet(dir.path(), opt, table_cache.get(), &stats));
  }
};
}  // namespace

TEST(version_edit_round_trips_every_field) {
  VersionEdit e;
  e.SetComparatorName(Slice("cmp"));
  e.SetLogNumber(11);
  e.SetPrevLogNumber(10);      // SPEC 3.3.1 -- the field v1 did not have
  e.SetNextFile(42);
  e.SetLastSequence(9999);
  e.AddFile(0, MakeFile(7, "a", "m"));
  e.AddFile(2, MakeFile(8, std::string("\0k", 2), "\xff"));   // arbitrary bytes in keys
  e.DeleteFile(1, 3);
  e.DeleteFile(1, 4);

  std::string enc;
  e.EncodeTo(&enc);
  VersionEdit back;
  REQUIRE_OK(back.DecodeFrom(Slice(enc)));

  CHECK_EQ(back.comparator_, std::string("cmp"));
  CHECK_EQ(back.log_number_, 11u);
  CHECK_EQ(back.prev_log_number_, 10u);
  CHECK_EQ(back.next_file_number_, 42u);
  CHECK_EQ(back.last_sequence_, 9999u);
  REQUIRE(back.new_files_.size() == 2);
  CHECK_EQ(back.new_files_[0].first, 0);
  CHECK_EQ(back.new_files_[0].second.number, 7u);
  CHECK_EQ(back.new_files_[1].second.smallest, e.new_files_[1].second.smallest);
  CHECK_EQ(back.deleted_files_.size(), 2u);
}

TEST(version_edit_rejects_malformed_input) {
  // A manifest record whose CRC passed can still be malformed if an older or newer build
  // wrote it. "Assume it parses" turns a version mismatch into a wild read.
  VersionEdit e;
  e.SetLogNumber(5);
  e.AddFile(0, MakeFile(1, "a", "b"));
  std::string enc;
  e.EncodeTo(&enc);

  for (size_t keep = 0; keep < enc.size(); ++keep) {
    TCTX("truncate to " << keep);
    VersionEdit back;
    Status s = back.DecodeFrom(Slice(enc.data(), keep));
    // Either it decodes a valid prefix of fields, or it says Corruption. It must never
    // read past the buffer -- ASan is what proves the alternative would be caught.
    if (!s.ok()) CHECK_CODE(s, Status::Code::kCorruption);
  }
  VersionEdit bad;
  std::string unknown_tag;
  PutVarint32(&unknown_tag, 99);
  CHECK_CODE(bad.DecodeFrom(Slice(unknown_tag)), Status::Code::kCorruption);
}

TEST(create_then_recover_reproduces_the_file_set) {
  Fixture f;
  REQUIRE_OK(f.vset->CreateNew());

  VersionEdit e;
  e.SetLogNumber(3);
  e.AddFile(0, MakeFile(10, "a", "c"));
  e.AddFile(0, MakeFile(11, "d", "f"));
  e.AddFile(1, MakeFile(12, "a", "z", 5000));
  f.vset->MarkFileNumberUsed(12);
  f.vset->SetLastSequence(777);
  REQUIRE_OK(f.vset->LogAndApply(&e));

  CHECK_EQ(f.vset->current()->NumFilesAtTier(0), 2);
  CHECK_EQ(f.vset->current()->NumFilesAtTier(1), 1);

  f.Reopen();
  REQUIRE_OK(f.vset->Recover());
  CHECK_EQ(f.vset->current()->NumFilesAtTier(0), 2);
  CHECK_EQ(f.vset->current()->NumFilesAtTier(1), 1);
  CHECK_EQ(f.vset->current()->TotalBytes(), 7000u);
  CHECK_EQ(f.vset->LogNumber(), 3u);
}

TEST(last_sequence_survives_reopen_and_never_restarts_at_zero) {
  // SPEC E-19 / S9. If last_sequence were not recovered, post-restart writes would get
  // sequence numbers BELOW existing data, so old values would shadow new ones forever --
  // silently, and only for keys that existed before the restart.
  Fixture f;
  REQUIRE_OK(f.vset->CreateNew());
  f.vset->SetLastSequence(123456);
  VersionEdit e;
  e.AddFile(0, MakeFile(5, "a", "b"));
  REQUIRE_OK(f.vset->LogAndApply(&e));

  f.Reopen();
  REQUIRE_OK(f.vset->Recover());
  CHECK_EQ(f.vset->LastSequence(), 123456u);
  CHECK_GT(f.vset->NewFileNumber(), 5u);   // file numbers must not be reissued either
}

TEST(prev_log_number_makes_two_logs_live_at_once) {
  // SPEC 3.3.1 / E-36. From the freeze of a memtable until its flush has a durable
  // manifest edit, TWO logs hold acknowledged data. MinLiveLog is what recovery replays
  // from and what GC must retain.
  Fixture f;
  REQUIRE_OK(f.vset->CreateNew());

  VersionEdit e;
  e.SetLogNumber(9);        // the mutable memtable's log
  e.SetPrevLogNumber(8);    // the immutable memtable's log -- still live
  REQUIRE_OK(f.vset->LogAndApply(&e));
  CHECK_EQ(f.vset->MinLiveLog(), 8u);

  VersionEdit done;
  done.SetLogNumber(9);
  done.SetPrevLogNumber(0);   // the flush landed; log 8 may now be deleted
  REQUIRE_OK(f.vset->LogAndApply(&done));
  CHECK_EQ(f.vset->MinLiveLog(), 9u);

  f.Reopen();
  REQUIRE_OK(f.vset->Recover());
  CHECK_EQ(f.vset->MinLiveLog(), 9u);
}

TEST(recover_reports_why_when_the_directory_is_broken) {
  // SPEC E-18. Each failure is a DISTINCT Status naming the file. A database that fails to
  // open must say why -- "Corruption" with no filename is how debugging sessions get long.
  {   // no CURRENT at all: NotFound, which Open uses to mean "no database here"
    Fixture f;
    Status s = f.vset->Recover();
    CHECK_CODE(s, Status::Code::kNotFound);
  }
  {   // CURRENT names a manifest that does not exist
    Fixture f;
    REQUIRE_OK(f.vset->CreateNew());
    REQUIRE_OK(WriteStringToFileSync(Env::Default(), "MANIFEST-999999\n",
                                     CurrentFileName(f.dir.path())));
    f.Reopen();
    Status s = f.vset->Recover();
    CHECK_CODE(s, Status::Code::kCorruption);
    CHECK(s.ToString().find("MANIFEST-999999") != std::string::npos);
  }
  {   // CURRENT without its trailing newline
    Fixture f;
    REQUIRE_OK(f.vset->CreateNew());
    REQUIRE_OK(WriteStringToFileSync(Env::Default(), "MANIFEST-000001",
                                     CurrentFileName(f.dir.path())));
    f.Reopen();
    Status s = f.vset->Recover();
    CHECK_CODE(s, Status::Code::kCorruption);
    CHECK(s.ToString().find("CURRENT") != std::string::npos);
  }
  {   // a manifest that is not a log file at all
    Fixture f;
    REQUIRE_OK(f.vset->CreateNew());
    std::string cur;
    REQUIRE_OK(ReadFileToString(Env::Default(), CurrentFileName(f.dir.path()), &cur));
    cur.pop_back();
    REQUIRE_OK(WriteStringToFileSync(Env::Default(), "garbage garbage garbage",
                                     f.dir.path() + "/" + cur));
    f.Reopen();
    CHECK_CODE(f.vset->Recover(), Status::Code::kCorruption);
  }
}

TEST(a_manifest_written_by_another_comparator_is_refused) {
  // The database is refused rather than silently misread. A different key ordering would
  // make every binary search wrong while every checksum still passed.
  Fixture f;
  REQUIRE_OK(f.vset->CreateNew());
  std::string cur;
  REQUIRE_OK(ReadFileToString(Env::Default(), CurrentFileName(f.dir.path()), &cur));
  cur.pop_back();

  // Hand-build a manifest whose first record names a different comparator.
  VersionEdit e;
  e.SetComparatorName(Slice("someone.elses.comparator"));
  e.SetNextFile(5);
  std::string rec;
  e.EncodeTo(&rec);
  {
    std::unique_ptr<WritableFile> file;
    REQUIRE_OK(Env::Default()->NewWritableFile(f.dir.path() + "/" + cur, &file));
    std::unique_ptr<WalWriter> w;
    REQUIRE_OK(WalWriter::Create(std::move(file), 1, &w));
    CHECK_OK(w->AddRecord(Slice(rec)));
    CHECK_OK(w->Sync());
    CHECK_OK(w->Close());
  }
  f.Reopen();
  Status s = f.vset->Recover();
  CHECK_CODE(s, Status::Code::kInvalidArgument);
  CHECK(s.ToString().find("comparator") != std::string::npos);
}

TEST(a_torn_manifest_tail_means_the_last_edit_simply_did_not_happen) {
  // SPEC 3.9 / E-35. A crash between the manifest append and its fsync leaves exactly
  // this. The edit is discarded and its SST becomes an orphan -- untidy, not fatal. What
  // must NOT happen is the manifest referencing a file that does not exist.
  Fixture f;
  REQUIRE_OK(f.vset->CreateNew());
  VersionEdit good;
  good.AddFile(0, MakeFile(10, "a", "c"));
  REQUIRE_OK(f.vset->LogAndApply(&good));
  VersionEdit lost;
  lost.AddFile(0, MakeFile(11, "d", "f"));
  REQUIRE_OK(f.vset->LogAndApply(&lost));

  std::string cur;
  REQUIRE_OK(ReadFileToString(Env::Default(), CurrentFileName(f.dir.path()), &cur));
  cur.pop_back();
  const std::string manifest = f.dir.path() + "/" + cur;

  std::string bytes;
  REQUIRE_OK(ReadFileToString(Env::Default(), manifest, &bytes));
  // Chop the last few bytes: the final record is now torn.
  REQUIRE_OK(WriteStringToFileSync(Env::Default(), Slice(bytes.data(), bytes.size() - 3),
                                   manifest));

  f.Reopen();
  REQUIRE_OK(f.vset->Recover());
  CHECK_EQ(f.vset->current()->NumFilesAtTier(0), 1);   // file 11's edit is gone
}

TEST(a_crash_before_the_directory_fsync_leaves_the_old_current) {
  // SPEC E-7. write CURRENT.tmp -> fsync -> rename -> fsync(DIR). Without the directory
  // fsync the rename can be lost, leaving CURRENT naming a manifest that is gone: an
  // unopenable database. The crash simulator is what makes this falsifiable at all.
  FaultEnv fe;
  Fixture f(&fe);
  REQUIRE_OK(f.vset->CreateNew());
  REQUIRE_OK(fe.SyncDir(f.dir.path()));   // the created database is durable

  std::string before;
  REQUIRE_OK(ReadFileToString(Env::Default(), CurrentFileName(f.dir.path()), &before));

  // Now perform a CURRENT rotation WITHOUT syncing the directory afterwards.
  const std::string tmp = TempFileName(f.dir.path(), 999);
  REQUIRE_OK(WriteStringToFileSync(&fe, Slice("MANIFEST-999999\n"), tmp));
  REQUIRE_OK(fe.RenameFile(tmp, CurrentFileName(f.dir.path())));
  std::string mid;
  REQUIRE_OK(ReadFileToString(Env::Default(), CurrentFileName(f.dir.path()), &mid));
  CHECK_EQ(mid, std::string("MANIFEST-999999\n"));

  fe.SimulateCrash();

  std::string after;
  REQUIRE_OK(ReadFileToString(Env::Default(), CurrentFileName(f.dir.path()), &after));
  CHECK_EQ(after, before);   // the un-durable rename never happened
}

TEST(live_files_are_the_union_over_all_live_versions) {
  // S14. RemoveObsoleteFiles must never unlink a file some older, still-referenced Version
  // still points at -- that is the guarantee that lets a reader do all its I/O with no
  // lock held.
  Fixture f;
  REQUIRE_OK(f.vset->CreateNew());
  VersionEdit e1;
  e1.AddFile(0, MakeFile(10, "a", "c"));
  REQUIRE_OK(f.vset->LogAndApply(&e1));

  Version* pinned = f.vset->current();
  pinned->Ref();   // a reader holds this version

  VersionEdit e2;
  e2.DeleteFile(0, 10);
  e2.AddFile(1, MakeFile(11, "a", "c"));
  REQUIRE_OK(f.vset->LogAndApply(&e2));

  std::set<uint64_t> live;
  f.vset->AddLiveFiles(&live);
  CHECK(live.count(10) == 1);   // still live: the pinned version references it
  CHECK(live.count(11) == 1);

  pinned->Unref();
  live.clear();
  f.vset->AddLiveFiles(&live);
  CHECK(live.count(10) == 0);   // now genuinely obsolete
  CHECK(live.count(11) == 1);
}

TEST(files_are_searched_shallow_tier_first_and_newest_within_a_tier) {
  // SPEC E-1's ordering, at the Version level. File number is a valid recency key only
  // WITHIN a tier; across tiers the guarantee is the 3.8.1 invariant.
  Fixture f;
  REQUIRE_OK(f.vset->CreateNew());
  VersionEdit e;
  e.AddFile(0, MakeFile(20, "a", "z"));
  e.AddFile(0, MakeFile(21, "a", "z"));
  e.AddFile(1, MakeFile(5, "a", "z"));
  REQUIRE_OK(f.vset->LogAndApply(&e));

  std::vector<std::pair<int, uint64_t>> order;
  f.vset->current()->ForEachOverlapping(Slice("m"), [&](int tier, FileMetaData* fm) {
    order.emplace_back(tier, fm->number);
    return true;
  });
  REQUIRE(order.size() == 3);
  CHECK_EQ(order[0].first, 0); CHECK_EQ(order[0].second, 21u);   // tier 0, newest first
  CHECK_EQ(order[1].first, 0); CHECK_EQ(order[1].second, 20u);
  CHECK_EQ(order[2].first, 1); CHECK_EQ(order[2].second, 5u);    // then tier 1
}

TEST(the_key_range_filter_skips_files_without_touching_them) {
  // The cheapest read-path filter, and it runs before the Bloom filter because it costs no
  // I/O at all -- the range is in the manifest, not the file.
  Fixture f;
  REQUIRE_OK(f.vset->CreateNew());
  VersionEdit e;
  e.AddFile(0, MakeFile(1, "a", "c"));
  e.AddFile(0, MakeFile(2, "m", "p"));
  e.AddFile(0, MakeFile(3, "x", "z"));
  REQUIRE_OK(f.vset->LogAndApply(&e));

  int visited = 0;
  f.vset->current()->ForEachOverlapping(Slice("n"), [&](int, FileMetaData*) { ++visited; return true; });
  CHECK_EQ(visited, 1);
  visited = 0;
  f.vset->current()->ForEachOverlapping(Slice("q"), [&](int, FileMetaData*) { ++visited; return true; });
  CHECK_EQ(visited, 0);
}

RUN_ALL()

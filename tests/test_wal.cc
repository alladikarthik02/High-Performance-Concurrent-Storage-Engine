// T4: the write-ahead log. SPEC 3.3, R3, R4, S1, S4, E-4, E-5.
//
// The headline test truncates the log at EVERY byte offset and asserts that recovery
// always yields a prefix of what was written -- never a partial record, never garbage,
// never a crash. That is the whole durability contract, checked exhaustively rather than
// at a few hand-picked points.
#include "tests/test.h"

#include <memory>
#include <string>
#include <vector>

#include "lsmeng/coding.h"
#include "lsmeng/env.h"
#include "lsmeng/wal.h"
#include "lsmeng/write_batch.h"
#include "tests/fault_env.h"
#include "tests/tmpdir.h"

using namespace lsmeng;

namespace {
constexpr uint64_t kLogNum = 7;

// Write `records` to a fresh log, return the file's bytes.
std::string WriteLog(Env* env, const std::string& path,
                     const std::vector<std::string>& records) {
  std::unique_ptr<WritableFile> f;
  if (!env->NewWritableFile(path, &f).ok()) return "";
  std::unique_ptr<WalWriter> w;
  if (!WalWriter::Create(std::move(f), kLogNum, &w).ok()) return "";
  for (const auto& r : records) if (!w->AddRecord(Slice(r)).ok()) return "";
  if (!w->Sync().ok()) return "";
  if (!w->Close().ok()) return "";
  std::string bytes;
  ReadFileToString(env, path, &bytes);
  return bytes;
}

// Read everything a log yields, plus why it stopped.
std::vector<std::string> ReadLog(Env* env, const std::string& path, WalReader::Stop* stop,
                                 Status* open_status = nullptr) {
  std::vector<std::string> out;
  std::unique_ptr<WalReader> r;
  Status s = WalReader::Open(env, path, &r);
  if (open_status) *open_status = s;
  if (!s.ok()) return out;
  Slice rec;
  std::string scratch;
  while (r->ReadRecord(&rec, &scratch)) out.push_back(rec.ToString());
  *stop = r->stop_reason();
  return out;
}
}  // namespace

TEST(roundtrip_of_many_records_including_odd_sizes) {
  testing::TmpDir d("wal");
  Env* env = Env::Default();
  std::vector<std::string> recs;
  testing::Rng rng(testing::seed());
  for (int i = 0; i < 500; ++i) {
    size_t n = 1 + rng.below(3000);
    std::string r(n, '\0');
    for (auto& c : r) c = static_cast<char>(rng.next());
    recs.push_back(r);
  }
  recs.push_back(std::string(1, '\0'));             // a 1-byte record of NUL
  recs.push_back(std::string(100000, 'x'));         // larger than the reader's 32 KiB buffer
  WriteLog(env, d.file("1.log"), recs);

  WalReader::Stop stop;
  auto got = ReadLog(env, d.file("1.log"), &stop);
  CHECK_EQ(got.size(), recs.size());
  for (size_t i = 0; i < got.size() && i < recs.size(); ++i) {
    TCTX("record=" << i << " size=" << recs[i].size());
    CHECK_EQ(got[i], recs[i]);
  }
  CHECK(stop == WalReader::Stop::kEof);
}

TEST(a_zero_length_record_is_refused_by_the_writer) {
  // E-5, at the source: the writer must never create the thing the reader is defending
  // against.
  testing::TmpDir d("wal");
  std::unique_ptr<WritableFile> f;
  REQUIRE_OK(Env::Default()->NewWritableFile(d.file("z.log"), &f));
  std::unique_ptr<WalWriter> w;
  REQUIRE_OK(WalWriter::Create(std::move(f), kLogNum, &w));
  CHECK_CODE(w->AddRecord(Slice("", 0)), Status::Code::kInvalidArgument);
}

TEST(a_zero_filled_region_does_not_parse_as_valid_empty_records) {
  // THE E-5 TEST. CRC32C("") is 0, so crc=0,len=0,type=0 is self-consistent -- and a
  // preallocated-but-unwritten extent is exactly that. Without the zero-length ban and the
  // seeded CRC, recovery would read an unbounded stream of "valid" empty records here.
  testing::TmpDir d("wal");
  Env* env = Env::Default();
  const std::string path = d.file("holes.log");
  WriteLog(env, path, {"first", "second"});

  std::string bytes;
  ReadFileToString(env, path, &bytes);
  bytes.append(4096, '\0');                       // the hole
  REQUIRE_OK(WriteStringToFileSync(env, bytes, path));

  WalReader::Stop stop;
  auto got = ReadLog(env, path, &stop);
  REQUIRE(got.size() == 2);
  CHECK_EQ(got[0], std::string("first"));
  CHECK_EQ(got[1], std::string("second"));
  CHECK(stop == WalReader::Stop::kZeroLength);    // stopped for the right reason
}

TEST(truncation_at_every_byte_offset_yields_a_clean_prefix) {
  // The exhaustive durability test. For every possible truncation point, recovery must
  // return a PREFIX of what was written -- never a partial record, never a wrong one.
  testing::TmpDir d("wal");
  Env* env = Env::Default();
  const std::vector<std::string> recs = {"alpha", "bravo", "charlie", "delta", "echo"};
  const std::string full = WriteLog(env, d.file("t.log"), recs);
  REQUIRE(!full.empty());

  for (size_t keep = 0; keep <= full.size(); ++keep) {
    TCTX("keep=" << keep << "/" << full.size());
    const std::string path = d.file("trunc.log");
    REQUIRE_OK(WriteStringToFileSync(env, Slice(full.data(), keep), path));

    Status open_status;
    WalReader::Stop stop = WalReader::Stop::kEof;
    auto got = ReadLog(env, path, &stop, &open_status);

    if (keep < kWalHeaderSize) {
      // Not even a complete file header: this is not a log, and Open must say so rather
      // than silently return an empty log (which would look like "no data was written").
      CHECK(!open_status.ok());
      continue;
    }
    CHECK_OK(open_status);
    CHECK_LE(got.size(), recs.size());
    for (size_t i = 0; i < got.size(); ++i) CHECK_EQ(got[i], recs[i]);   // a genuine prefix

    // A truncation that lands exactly ON a record boundary is a CLEAN EOF, not a torn
    // tail -- the file is byte-for-byte identical to a log that simply had fewer records
    // written to it, and no reader could tell the difference. That is the correct
    // behaviour, and an earlier version of this assertion wrongly demanded a non-kEof
    // reason at every truncation point. Only a truncation INSIDE a record must report why.
    const size_t consumed = kWalHeaderSize + [&] {
      size_t n = 0;
      for (size_t i = 0; i < got.size(); ++i) n += kWalRecordHeaderSize + recs[i].size();
      return n;
    }();
    if (keep > consumed) CHECK(stop != WalReader::Stop::kEof);
    else CHECK(stop == WalReader::Stop::kEof);
    env->DeleteFile(path);
  }
}

TEST(a_single_flipped_bit_anywhere_stops_recovery_at_that_record) {
  // S4/S7: every record is checksummed and verified before use. This walks a bit through
  // the whole file and asserts the reader never hands out a record it should not.
  testing::TmpDir d("wal");
  Env* env = Env::Default();
  const std::vector<std::string> recs = {"one", "two", "three"};
  const std::string full = WriteLog(env, d.file("c.log"), recs);

  for (size_t byte = kWalHeaderSize; byte < full.size(); ++byte) {
    TCTX("corrupt byte=" << byte);
    std::string bad = full;
    bad[byte] = static_cast<char>(bad[byte] ^ 0x01);
    const std::string path = d.file("corrupt.log");
    REQUIRE_OK(WriteStringToFileSync(env, bad, path));

    WalReader::Stop stop = WalReader::Stop::kEof;
    auto got = ReadLog(env, path, &stop);
    // Whatever came back must be a correct prefix. The flipped bit may land in a length
    // field and produce a torn-payload stop, or in a payload and produce a CRC stop --
    // both are fine. What must NOT happen is a record being returned with wrong contents.
    for (size_t i = 0; i < got.size(); ++i) CHECK_EQ(got[i], recs[i]);
    CHECK_LT(got.size(), recs.size() + 1);
    env->DeleteFile(path);
  }
}

TEST(a_log_written_under_one_number_does_not_validate_under_another) {
  // The seeded CRC's second job: a record from log 7 must not validate if it turns up in
  // log 8. That is the shape of a stale-file-reuse bug -- an old log left behind and a new
  // one created with the same name.
  testing::TmpDir d("wal");
  Env* env = Env::Default();
  const std::string path = d.file("n.log");
  WriteLog(env, path, {"payload"});

  std::string bytes;
  ReadFileToString(env, path, &bytes);
  EncodeFixed64(&bytes[8], 8);                    // rewrite the header's log_number
  REQUIRE_OK(WriteStringToFileSync(env, bytes, path));

  WalReader::Stop stop = WalReader::Stop::kEof;
  auto got = ReadLog(env, path, &stop);
  CHECK_EQ(got.size(), 0u);
  CHECK(stop == WalReader::Stop::kBadCrc);
}

TEST(open_rejects_a_file_that_is_not_a_log) {
  testing::TmpDir d("wal");
  Env* env = Env::Default();
  REQUIRE_OK(WriteStringToFileSync(env, "this is not a log file at all", d.file("x.log")));
  std::unique_ptr<WalReader> r;
  // E-18: a database that fails to open must say WHY, naming the file.
  Status s = WalReader::Open(env, d.file("x.log"), &r);
  CHECK_CODE(s, Status::Code::kCorruption);
  CHECK(s.ToString().find("x.log") != std::string::npos);
}

TEST(an_injected_short_write_produces_a_torn_tail_that_recovery_survives) {
  // The realistic failure, through the Env seam rather than by editing bytes: a partial
  // write that REPORTS SUCCESS. This is what actually happens when a process dies
  // mid-append, and it is why the four recovery checks exist.
  testing::TmpDir d("wal");
  FaultEnv fe;
  const std::string path = d.file("f.log");
  {
    std::unique_ptr<WritableFile> f;
    REQUIRE_OK(fe.NewWritableFile(path, &f));
    std::unique_ptr<WalWriter> w;
    REQUIRE_OK(WalWriter::Create(std::move(f), kLogNum, &w));
    CHECK_OK(w->AddRecord(Slice("good-record-one")));
    CHECK_OK(w->AddRecord(Slice("good-record-two")));
    fe.SetAppendFault({FaultSpec::Kind::kShortWrite, fe.append_calls() + 1, 0});
    CHECK_OK(w->AddRecord(Slice("this-payload-gets-cut-in-half")));
    CHECK_OK(w->Sync());
    CHECK_OK(w->Close());
  }
  WalReader::Stop stop = WalReader::Stop::kEof;
  auto got = ReadLog(Env::Default(), path, &stop);
  REQUIRE(got.size() == 2);
  CHECK_EQ(got[0], std::string("good-record-one"));
  CHECK_EQ(got[1], std::string("good-record-two"));
  CHECK(stop != WalReader::Stop::kEof);
}

TEST(unsynced_records_are_lost_after_a_simulated_crash_and_synced_ones_are_not) {
  // R3/R4/S1 through the crash simulator. This is the configuration a kill -9 test CANNOT
  // provide, because the page cache survives a process death (SPEC 11.21): delete the
  // Sync() call and this test fails, while a kill -9 test would still pass.
  testing::TmpDir d("wal");
  FaultEnv fe;
  const std::string path = d.file("d.log");
  {
    std::unique_ptr<WritableFile> f;
    REQUIRE_OK(fe.NewWritableFile(path, &f));
    CHECK_OK(fe.SyncDir(d.path()));       // make the file's existence durable first
    std::unique_ptr<WalWriter> w;
    REQUIRE_OK(WalWriter::Create(std::move(f), kLogNum, &w));
    CHECK_OK(w->AddRecord(Slice("acknowledged")));
    CHECK_OK(w->Sync());                  // <-- the durability point (S1)
    CHECK_OK(w->AddRecord(Slice("never-acknowledged")));
    CHECK_OK(w->Close());
  }
  fe.SimulateCrash();

  WalReader::Stop stop = WalReader::Stop::kEof;
  auto got = ReadLog(Env::Default(), path, &stop);
  REQUIRE(got.size() == 1);
  CHECK_EQ(got[0], std::string("acknowledged"));
}

TEST(write_batch_is_all_or_nothing_through_the_log) {
  // E-4: one batch is one record, so a batch is either wholly replayed or wholly absent.
  testing::TmpDir d("wal");
  Env* env = Env::Default();
  WriteBatch b;
  b.Put("k1", "v1");
  b.Delete("k2");
  b.Put("k3", std::string("v\0 3", 4));
  b.SetSequence(100);
  CHECK_EQ(b.Count(), 3);

  const std::string path = d.file("b.log");
  WriteLog(env, path, {b.Contents()});

  WalReader::Stop stop;
  auto got = ReadLog(env, path, &stop);
  REQUIRE(got.size() == 1);
  WriteBatch back;
  CHECK_OK(back.SetContents(Slice(got[0])));
  CHECK_EQ(back.Count(), 3);
  CHECK_EQ(back.Sequence(), 100u);

  struct Collect : WriteBatch::Handler {
    std::vector<std::string> ops;
    void Put(const Slice& k, const Slice& v) override { ops.push_back("P:" + k.ToString() + "=" + v.ToString()); }
    void Delete(const Slice& k) override { ops.push_back("D:" + k.ToString()); }
  } c;
  CHECK_OK(back.Iterate(&c));
  REQUIRE(c.ops.size() == 3);
  CHECK_EQ(c.ops[0], std::string("P:k1=v1"));
  CHECK_EQ(c.ops[1], std::string("D:k2"));
  CHECK_EQ(c.ops[2], std::string("P:k3=v\0 3", 9));   // 9 bytes: the NUL is data
}

TEST(a_corrupt_batch_payload_is_reported_not_partially_applied) {
  // The CRC covers the payload, so this cannot happen from disk corruption -- but an
  // encoder bug could produce it, and a batch whose declared count disagrees with its
  // records would otherwise apply a truncated prefix silently.
  WriteBatch b;
  b.Put("a", "1");
  b.Put("b", "2");
  std::string rep = b.Contents();
  rep.resize(rep.size() - 1);           // chop the last value byte
  WriteBatch bad;
  CHECK_OK(bad.SetContents(Slice(rep)));
  struct Noop : WriteBatch::Handler {
    void Put(const Slice&, const Slice&) override {}
    void Delete(const Slice&) override {}
  } n;
  CHECK_CODE(bad.Iterate(&n), Status::Code::kCorruption);
}

RUN_ALL()

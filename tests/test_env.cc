// T0: the Env seam, the S13 enforcement mechanism, and the crash simulator.
//
// These are the tests that make every later durability test meaningful. If SimulateCrash
// does not actually drop un-fsynced bytes, then the whole of SPEC 7 layer 4 is theatre.
#include "tests/test.h"

#include <thread>
#include <vector>

#include "lsmeng/env.h"
#include "lsmeng/tracked_mutex.h"
#include "tests/fault_env.h"
#include "tests/tmpdir.h"

using namespace lsmeng;

namespace {
std::string ReadAll(Env* env, const std::string& p) {
  std::string s;
  ReadFileToString(env, p, &s);
  return s;
}
}  // namespace

TEST(write_then_read_roundtrip) {
  testing::TmpDir d("env");
  Env* env = Env::Default();
  const std::string p = d.file("a.txt");
  // Embedded NUL on purpose: SPEC 3.1 says arbitrary bytes are legal, and a length-based
  // Slice is the whole reason that can be true.
  const std::string payload("hello\0world", 11);
  REQUIRE_OK(WriteStringToFileSync(env, payload, p));
  CHECK(env->FileExists(p));
  uint64_t sz = 0;
  CHECK_OK(env->GetFileSize(p, &sz));
  CHECK_EQ(sz, 11u);
  CHECK_EQ(ReadAll(env, p), payload);
}

TEST(missing_file_is_not_found_not_io_error) {
  testing::TmpDir d("env");
  Env* env = Env::Default();
  std::unique_ptr<SequentialFile> f;
  Status s = env->NewSequentialFile(d.file("nope"), &f);
  // SPEC E-18 / S25: "no database here" and "a database here that will not open" must be
  // distinguishable by code(), because Open branches on exactly that difference.
  CHECK_CODE(s, Status::Code::kNotFound);
  CHECK(!s.ok());
}

TEST(pread_is_positional_and_safe_from_many_threads) {
  testing::TmpDir d("env");
  Env* env = Env::Default();
  const std::string p = d.file("big");
  std::string data;
  for (int i = 0; i < 4096; ++i) data.push_back(static_cast<char>(i & 0xFF));
  REQUIRE_OK(WriteStringToFileSync(env, data, p));

  std::unique_ptr<RandomAccessFile> f;
  REQUIRE_OK(env->NewRandomAccessFile(p, &f));

  // SPEC E-25: with read(2) on a shared descriptor this test fails with interleaved
  // garbage that looks exactly like a checksum bug one layer up. With pread it cannot.
  std::atomic<int> bad{0};
  std::vector<std::thread> ts;
  for (int t = 0; t < 8; ++t) {
    ts.emplace_back([&, t] {
      char scratch[64];
      for (int i = 0; i < 500; ++i) {
        uint64_t off = static_cast<uint64_t>((t * 137 + i * 7) % 4000);
        Slice got;
        if (!f->Read(off, 64, &got, scratch).ok() || got.size() != 64) { ++bad; return; }
        if (std::memcmp(got.data(), data.data() + off, 64) != 0) ++bad;
      }
    });
  }
  for (auto& th : ts) th.join();
  CHECK_EQ(bad.load(), 0);
}

TEST(read_past_eof_is_a_short_read_not_an_error) {
  testing::TmpDir d("env");
  Env* env = Env::Default();
  const std::string p = d.file("small");
  REQUIRE_OK(WriteStringToFileSync(env, "abc", p));
  std::unique_ptr<RandomAccessFile> f;
  REQUIRE_OK(env->NewRandomAccessFile(p, &f));
  char scratch[16];
  Slice got;
  CHECK_OK(f->Read(1, 100, &got, scratch));
  CHECK_EQ(got.size(), 2u);   // recovery relies on this to detect a torn tail (SPEC 3.3)
}

TEST(rename_and_syncdir_and_children) {
  testing::TmpDir d("env");
  Env* env = Env::Default();
  REQUIRE_OK(WriteStringToFileSync(env, "x", d.file("one")));
  CHECK_OK(env->RenameFile(d.file("one"), d.file("two")));
  CHECK(!env->FileExists(d.file("one")));
  CHECK(env->FileExists(d.file("two")));
  // SPEC E-7: this is what makes the rename durable. A container filesystem that answers
  // EINVAL is tolerated (and recorded by tools/env_facts), but anything else is a real
  // failure and must surface here rather than silently at 3am after a power cut.
  CHECK_OK(env->SyncDir(d.path()));
  std::vector<std::string> kids;
  CHECK_OK(env->GetChildren(d.path(), &kids));
  CHECK_EQ(kids.size(), 1u);
  CHECK_EQ(kids[0], std::string("two"));
}

TEST(lock_file_excludes_a_second_acquisition) {
  testing::TmpDir d("env");
  Env* env = Env::Default();
  FileLock* a = nullptr;
  REQUIRE_OK(env->LockFile(d.file("LOCK"), &a));
  REQUIRE(a != nullptr);
  FileLock* b = nullptr;
  Status s = env->LockFile(d.file("LOCK"), &b);
  // SPEC S16. This is the in-process half of the check; the two-PROCESS test that
  // actually gates orphan collection lands in T7, because that is the one wanrep B2
  // showed can fail on a bind mount.
  CHECK(!s.ok());
  CHECK(b == nullptr);
  CHECK_OK(env->UnlockFile(a));
  // After release it must be acquirable again -- otherwise a clean restart is impossible.
  REQUIRE_OK(env->LockFile(d.file("LOCK"), &b));
  CHECK_OK(env->UnlockFile(b));
}

TEST(tracked_mutex_maintains_the_thread_local_mask) {
  // SPEC S13's mechanism. The assertion itself aborts, so it cannot be exercised from a
  // passing test; what is testable -- and what actually breaks if the wrapper is wrong --
  // is that the bit is set exactly while the lock is held, including across a scope exit.
  TrackedMutex m;
  CHECK_EQ(held_locks() & kDbMutex, 0u);
  {
    std::lock_guard<TrackedMutex> g(m);
    CHECK_NE(held_locks() & kDbMutex, 0u);
  }
  CHECK_EQ(held_locks() & kDbMutex, 0u);
  CHECK(m.try_lock());
  CHECK_NE(held_locks() & kDbMutex, 0u);
  m.unlock();
  CHECK_EQ(held_locks() & kDbMutex, 0u);
}

TEST(fault_env_injects_short_write_io_error_and_enospc) {
  testing::TmpDir d("env");
  FaultEnv fe;
  {
    fe.SetAppendFault({FaultSpec::Kind::kIOError, 0, 0});
    std::unique_ptr<WritableFile> f;
    REQUIRE_OK(fe.NewWritableFile(d.file("e1"), &f));
    CHECK_CODE(f->Append("hello"), Status::Code::kIOError);
  }
  {
    fe.ResetCounters();
    fe.SetAppendFault({FaultSpec::Kind::kNoSpace, 1, 0});
    std::unique_ptr<WritableFile> f;
    REQUIRE_OK(fe.NewWritableFile(d.file("e2"), &f));
    CHECK_OK(f->Append("first"));                                  // call 0: fine
    CHECK_CODE(f->Append("second"), Status::Code::kIOError);       // call 1: ENOSPC
  }
  {
    fe.ResetCounters();
    fe.SetAppendFault({FaultSpec::Kind::kShortWrite, 0, 0});
    std::unique_ptr<WritableFile> f;
    REQUIRE_OK(fe.NewWritableFile(d.file("e3"), &f));
    CHECK_OK(f->Append("0123456789"));   // reports success, writes half
    CHECK_OK(f->Sync());
    CHECK_OK(f->Close());
    // A partial write that claims success is the nastiest realistic failure, and it is
    // exactly what produces the torn WAL record T4's recovery has to survive.
    CHECK_EQ(ReadAll(Env::Default(), d.file("e3")).size(), 5u);
  }
}

TEST(simulate_crash_drops_bytes_that_were_never_synced) {
  testing::TmpDir d("env");
  FaultEnv fe;
  const std::string p = d.file("wal");
  {
    std::unique_ptr<WritableFile> f;
    REQUIRE_OK(fe.NewWritableFile(p, &f));
    // Make the file's own existence durable first, so this test isolates ONE variable:
    // un-synced BYTES. Without the SyncDir the file itself is not durable either and the
    // crash correctly removes it entirely -- a different failure, tested separately
    // below. (This is the mistake that made the first run of this test fail; the model
    // was right and the test was asking two questions at once. CHALLENGES B2.)
    CHECK_OK(fe.SyncDir(d.path()));
    CHECK_OK(f->Append("DURABLE"));
    CHECK_OK(f->Sync());              // 7 bytes are now durable
    CHECK_OK(f->Append("VOLATILE"));  // never synced
    CHECK_OK(f->Flush());             // in the page cache -- a kill -9 would KEEP this
  }
  CHECK_EQ(ReadAll(Env::Default(), p).size(), 15u);
  fe.SimulateCrash();
  // SPEC 11.21: this is the assertion a kill -9 test can never make. Delete the Sync()
  // from the engine and this line fails; delete it and rely only on kill -9 and nothing
  // fails at all.
  CHECK_EQ(ReadAll(Env::Default(), p), std::string("DURABLE"));
}

TEST(simulate_crash_removes_a_create_whose_directory_was_never_synced) {
  testing::TmpDir d("env");
  FaultEnv fe;
  {
    std::unique_ptr<WritableFile> f;
    REQUIRE_OK(fe.NewWritableFile(d.file("orphan"), &f));
    CHECK_OK(f->Append("data"));
    CHECK_OK(f->Sync());     // the FILE is durable...
    CHECK_OK(f->Close());
  }                          // ...but its DIRECTORY ENTRY never was.
  CHECK(Env::Default()->FileExists(d.file("orphan")));
  fe.SimulateCrash();
  // SPEC E-7. This is the failure mode that leaves CURRENT naming a manifest that is
  // gone, and it is invisible without a crash model that understands directories.
  CHECK(!Env::Default()->FileExists(d.file("orphan")));
}

TEST(simulate_crash_keeps_a_create_whose_directory_was_synced) {
  testing::TmpDir d("env");
  FaultEnv fe;
  {
    std::unique_ptr<WritableFile> f;
    REQUIRE_OK(fe.NewWritableFile(d.file("kept"), &f));
    CHECK_OK(f->Append("data"));
    CHECK_OK(f->Sync());
    CHECK_OK(f->Close());
  }
  CHECK_OK(fe.SyncDir(d.path()));
  fe.SimulateCrash();
  CHECK(Env::Default()->FileExists(d.file("kept")));
  CHECK_EQ(ReadAll(Env::Default(), d.file("kept")), std::string("data"));
}

TEST(simulate_crash_rolls_back_an_unsynced_rename) {
  testing::TmpDir d("env");
  FaultEnv fe;
  REQUIRE_OK(WriteStringToFileSync(Env::Default(), "v1", d.file("CURRENT")));
  REQUIRE_OK(WriteStringToFileSync(Env::Default(), "v2", d.file("CURRENT.tmp")));
  CHECK_OK(fe.RenameFile(d.file("CURRENT.tmp"), d.file("CURRENT")));
  CHECK_EQ(ReadAll(Env::Default(), d.file("CURRENT")), std::string("v2"));
  fe.SimulateCrash();
  // The rename happened but was never made durable, so after a crash it must be as if it
  // never happened. This is the exact scenario SPEC 3.9's CURRENT rotation is built to
  // survive, and the reason SyncDir is a first-class Env operation.
  CHECK_EQ(ReadAll(Env::Default(), d.file("CURRENT")), std::string("v1"));
}

RUN_ALL()

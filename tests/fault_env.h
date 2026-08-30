// FaultEnv -- the fault-injection seam. SPEC 7 layer 2, T0 deliverable.
//
// WHY IT IS BUILT NOW, BEFORE ANY ENGINE CODE. Retrofitted fault injection tests the
// wrapper, not the engine: by the time you add it, the code has already been shaped
// around I/O that cannot fail, and the interesting paths do not exist to be injected
// into. It is also the only reason SPEC S24 ("all I/O through Options::env") is a rule
// worth enforcing.
//
// WHY PER-INSTANCE, NEVER GLOBAL. Two tests running in parallel under ctest -j must not
// see each other's injected failures. Every knob here lives on the instance.
//
// THE CRASH MODEL, and its honest limits.
//   SimulateCrash() drops everything that was never made durable:
//     * each written file is truncated back to its size at the last successful Sync();
//     * a file created, or a rename performed, whose PARENT DIRECTORY was never SyncDir'd
//       afterwards is undone -- which is what makes SPEC E-7 (the missing directory
//       fsync) an actually falsifiable claim rather than a comment.
//   It deliberately does NOT model a lost unlink. Losing an unlink only leaves an orphan
//   file, which is the benign direction and is what E-6 already covers; modelling it
//   would require un-deleting content we no longer have.
//
//   This matters because a `kill -9` test CANNOT catch a missing fsync: the page cache
//   survives a process death, so a build with every Sync() deleted still passes it
//   (SPEC 11.21). The two crash configurations test different things and both are
//   required.
#pragma once

#include <algorithm>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

#include "lsmeng/env.h"

namespace lsmeng {

class FaultEnv;

// What to inject, and when. `at_call` is a 0-based index into the calls of that kind made
// through this FaultEnv; -1 disables. Counting calls rather than matching filenames means
// a test can say "fail the third write to anything", which is how you sweep a failure
// across every step of a sequence.
struct FaultSpec {
  enum class Kind {
    kNone,
    kShortWrite,    // Append writes only part of the data, returns OK
    kIOError,       // return IOError (EIO)
    kNoSpace,       // return IOError (ENOSPC)
    kTruncate,      // truncate the file to `param` bytes, then return OK
    kCorruptByte,   // flip a bit at offset `param` on the next Sync
  };
  Kind kind = Kind::kNone;
  long at_call = -1;
  uint64_t param = 0;
};

namespace fault_detail {

struct SharedState {
  std::mutex mu;
  // Per file: bytes known durable (size at last successful Sync).
  std::map<std::string, uint64_t> durable_size;
  // Files created whose parent directory has not been SyncDir'd since.
  std::set<std::string> unsynced_creates;
  // Renames whose parent directory has not been SyncDir'd since.
  //   from, to, and -- crucially -- whatever `to` contained BEFORE the rename.
  //   rename(2) atomically REPLACES its target, destroying the old contents, so undoing
  //   a rename by renaming back is not enough: the file that was overwritten has to be
  //   put back too. Modelling this wrong makes the CURRENT-rotation test (SPEC 3.9)
  //   silently pass with no CURRENT at all. See CHALLENGES B2.
  struct PendingRename { std::string from, to; bool had_target; std::string target_bytes; };
  std::vector<PendingRename> unsynced_renames;

  long append_calls = 0;
  long sync_calls = 0;
  FaultSpec append_fault;
  FaultSpec sync_fault;

  // Set once SimulateCrash runs: every later operation fails, like a dead process would.
  bool crashed = false;
};

inline std::string ParentDir(const std::string& path) {
  size_t p = path.rfind('/');
  return p == std::string::npos ? std::string(".") : path.substr(0, p);
}

}  // namespace fault_detail

class FaultWritableFile final : public WritableFile {
 public:
  FaultWritableFile(std::string name, std::unique_ptr<WritableFile> target,
                    std::shared_ptr<fault_detail::SharedState> st)
      : name_(std::move(name)), target_(std::move(target)), st_(std::move(st)) {}

  uint64_t Size() const override { return target_->Size(); }

 protected:
  Status AppendImpl(const Slice& data) override {
    FaultSpec f;
    {
      std::lock_guard<std::mutex> g(st_->mu);
      if (st_->crashed) return Status::IOError("simulated crash: process is dead");
      long n = st_->append_calls++;
      if (st_->append_fault.kind != FaultSpec::Kind::kNone && st_->append_fault.at_call == n)
        f = st_->append_fault;
    }
    switch (f.kind) {
      case FaultSpec::Kind::kIOError:
        return Status::IOError("injected EIO on append to " + name_);
      case FaultSpec::Kind::kNoSpace:
        return Status::IOError("injected ENOSPC on append to " + name_);
      case FaultSpec::Kind::kShortWrite: {
        // The nastiest realistic failure: a partial write that reports success. This is
        // what produces a torn record, and the WAL's four recovery checks (SPEC 3.3) exist
        // for exactly this.
        size_t keep = data.size() / 2;
        return target_->Append(Slice(data.data(), keep));
      }
      default:
        return target_->Append(data);
    }
  }

  Status FlushImpl() override { return target_->Flush(); }

  Status SyncImpl() override {
    FaultSpec f;
    {
      std::lock_guard<std::mutex> g(st_->mu);
      if (st_->crashed) return Status::IOError("simulated crash: process is dead");
      long n = st_->sync_calls++;
      if (st_->sync_fault.kind != FaultSpec::Kind::kNone && st_->sync_fault.at_call == n)
        f = st_->sync_fault;
    }
    if (f.kind == FaultSpec::Kind::kIOError)
      return Status::IOError("injected EIO on sync of " + name_);
    if (f.kind == FaultSpec::Kind::kNoSpace)
      return Status::IOError("injected ENOSPC on sync of " + name_);

    Status s = target_->Sync();
    if (s.ok()) {
      std::lock_guard<std::mutex> g(st_->mu);
      st_->durable_size[name_] = target_->Size();
    }
    return s;
  }

  Status CloseImpl() override { return target_->Close(); }

 private:
  std::string name_;
  std::unique_ptr<WritableFile> target_;
  std::shared_ptr<fault_detail::SharedState> st_;
};

class FaultEnv final : public Env {
 public:
  explicit FaultEnv(Env* target = Env::Default())
      : target_(target), st_(std::make_shared<fault_detail::SharedState>()) {}

  void SetAppendFault(const FaultSpec& f) {
    std::lock_guard<std::mutex> g(st_->mu); st_->append_fault = f;
  }
  void SetSyncFault(const FaultSpec& f) {
    std::lock_guard<std::mutex> g(st_->mu); st_->sync_fault = f;
  }
  void ResetCounters() {
    std::lock_guard<std::mutex> g(st_->mu); st_->append_calls = 0; st_->sync_calls = 0;
  }
  long append_calls() const { std::lock_guard<std::mutex> g(st_->mu); return st_->append_calls; }
  long sync_calls() const { std::lock_guard<std::mutex> g(st_->mu); return st_->sync_calls; }

  // Drop everything that was never made durable, then mark the env dead. Recovery is then
  // tested by opening the directory with a FRESH env, exactly as a restarted process would.
  void SimulateCrash();

  bool crashed() const { std::lock_guard<std::mutex> g(st_->mu); return st_->crashed; }

  uint64_t NowMicros() override { return target_->NowMicros(); }
  void SleepForMicros(uint64_t m) override { target_->SleepForMicros(m); }

 protected:
  Status NewSequentialFileImpl(const std::string& f, std::unique_ptr<SequentialFile>* r) override {
    return target_->NewSequentialFile(f, r);
  }
  Status NewRandomAccessFileImpl(const std::string& f, std::unique_ptr<RandomAccessFile>* r) override {
    return target_->NewRandomAccessFile(f, r);
  }
  Status NewWritableFileImpl(const std::string& f, std::unique_ptr<WritableFile>* r) override {
    bool existed = target_->FileExists(f);
    std::unique_ptr<WritableFile> t;
    Status s = target_->NewWritableFile(f, &t);
    if (!s.ok()) return s;
    {
      std::lock_guard<std::mutex> g(st_->mu);
      st_->durable_size[f] = 0;   // O_TRUNC: nothing in it is durable yet
      if (!existed) st_->unsynced_creates.insert(f);
    }
    r->reset(new FaultWritableFile(f, std::move(t), st_));
    return Status::OK();
  }
  Status NewAppendableFileImpl(const std::string& f, std::unique_ptr<WritableFile>* r) override {
    bool existed = target_->FileExists(f);
    std::unique_ptr<WritableFile> t;
    Status s = target_->NewAppendableFile(f, &t);
    if (!s.ok()) return s;
    {
      std::lock_guard<std::mutex> g(st_->mu);
      if (!existed) { st_->durable_size[f] = 0; st_->unsynced_creates.insert(f); }
      else if (!st_->durable_size.count(f)) st_->durable_size[f] = t->Size();
    }
    r->reset(new FaultWritableFile(f, std::move(t), st_));
    return Status::OK();
  }
  bool FileExistsImpl(const std::string& f) override { return target_->FileExists(f); }
  Status GetChildrenImpl(const std::string& d, std::vector<std::string>* r) override {
    return target_->GetChildren(d, r);
  }
  Status DeleteFileImpl(const std::string& f) override {
    {
      std::lock_guard<std::mutex> g(st_->mu);
      st_->durable_size.erase(f);
      st_->unsynced_creates.erase(f);
    }
    return target_->DeleteFile(f);
  }
  Status CreateDirImpl(const std::string& d) override { return target_->CreateDir(d); }
  Status DeleteDirImpl(const std::string& d) override { return target_->DeleteDir(d); }
  Status GetFileSizeImpl(const std::string& f, uint64_t* s) override {
    return target_->GetFileSize(f, s);
  }
  Status RenameFileImpl(const std::string& s, const std::string& t) override {
    // Capture the target's contents BEFORE the rename destroys them.
    bool had_target = target_->FileExists(t);
    std::string target_bytes;
    if (had_target) ReadFileToString(target_, t, &target_bytes);

    Status r = target_->RenameFile(s, t);
    if (r.ok()) {
      std::lock_guard<std::mutex> g(st_->mu);
      st_->unsynced_renames.push_back({s, t, had_target, std::move(target_bytes)});
      auto it = st_->durable_size.find(s);
      if (it != st_->durable_size.end()) { st_->durable_size[t] = it->second; st_->durable_size.erase(it); }
    }
    return r;
  }
  Status SyncDirImpl(const std::string& d) override {
    Status s = target_->SyncDir(d);
    if (s.ok()) {
      // Everything in this directory is now durable, so nothing here is rolled back.
      std::lock_guard<std::mutex> g(st_->mu);
      for (auto it = st_->unsynced_creates.begin(); it != st_->unsynced_creates.end();)
        it = (fault_detail::ParentDir(*it) == d) ? st_->unsynced_creates.erase(it) : std::next(it);
      auto& v = st_->unsynced_renames;
      v.erase(std::remove_if(v.begin(), v.end(),
                             [&](const fault_detail::SharedState::PendingRename& p) {
                               return fault_detail::ParentDir(p.to) == d;
                             }),
              v.end());
    }
    return s;
  }
  Status LockFileImpl(const std::string& f, FileLock** l) override { return target_->LockFile(f, l); }
  Status UnlockFileImpl(FileLock* l) override { return target_->UnlockFile(l); }

 private:
  Env* target_;
  std::shared_ptr<fault_detail::SharedState> st_;
};

// Defined out-of-line-ish (still header-only) because it needs the POSIX truncate, which
// is deliberately absent from the Env interface -- Env models what the ENGINE may do, and
// the engine has no business truncating a file. A crash simulator does.
inline void FaultEnv::SimulateCrash() {
  std::lock_guard<std::mutex> g(st_->mu);
  if (st_->crashed) return;

  // 1. Roll back renames whose directory entry was never made durable (SPEC E-7).
  //    Newest first, so a chain of renames unwinds in the right order.
  for (auto it = st_->unsynced_renames.rbegin(); it != st_->unsynced_renames.rend(); ++it) {
    if (!target_->FileExists(it->from)) target_->RenameFile(it->to, it->from);
    // Put back whatever the rename overwrote. Without this the target simply vanishes,
    // which is a DIFFERENT failure from the one being modelled -- and one that would
    // make a broken CURRENT rotation look like a passing test.
    if (it->had_target) WriteStringToFileSync(target_, it->target_bytes, it->to);
  }
  st_->unsynced_renames.clear();

  // 2. Remove files whose creation was never made durable.
  for (const auto& f : st_->unsynced_creates) {
    target_->DeleteFile(f);
    st_->durable_size.erase(f);
  }
  st_->unsynced_creates.clear();

  // 3. Truncate every surviving file back to what was actually Sync()ed. This is the step
  //    a kill -9 test can never perform, because the page cache outlives the process --
  //    and therefore the only step that can fail a build with a missing fsync.
  for (const auto& kv : st_->durable_size) {
    uint64_t actual = 0;
    if (target_->GetFileSize(kv.first, &actual).ok() && actual > kv.second) {
      (void)!::truncate(kv.first.c_str(), static_cast<off_t>(kv.second));
    }
  }
  st_->crashed = true;
}

}  // namespace lsmeng

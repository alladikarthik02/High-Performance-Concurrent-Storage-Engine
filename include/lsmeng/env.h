#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "lsmeng/slice.h"
#include "lsmeng/status.h"
#include "lsmeng/tracked_mutex.h"

namespace lsmeng {

// SPEC 4.2 / S24. Every byte of I/O in the engine goes through this interface.
//
// TWO REASONS, both load-bearing:
//   1. It is the seam fault injection is wired into (SPEC 7 layer 2). SPEC v1 had no
//      Env* in Options and no Env parameter on DB::Open, which made the entire
//      fault-injection and crash-simulation strategy unreachable -- the tests were
//      specified but could not be written (SPEC 11.11).
//   2. It is what makes E-25 enforceable. Reads are POSITIONAL ONLY: there is no API here
//      that advances a shared file offset. `read` on an fd shared by many threads is a
//      race on that offset, and the resulting interleaved garbage looks exactly like an
//      SST checksum bug -- a day of debugging the wrong layer. You cannot make that
//      mistake through this interface, which is better than being careful.
//
// NVI (non-virtual interface) on purpose: the public methods are non-virtual, assert
// S13, and then dispatch to a protected virtual. A subclass therefore CANNOT bypass the
// "no lock held across I/O" check, not even by accident.

class SequentialFile {
 public:
  virtual ~SequentialFile() = default;
  // Reads up to n bytes, advancing this handle's own private offset. Single-threaded by
  // contract -- used only for WAL and MANIFEST replay, which happen on one thread.
  Status Read(size_t n, Slice* result, char* scratch) {
    LSMENG_ASSERT_NO_LOCK_FOR_IO();
    return ReadImpl(n, result, scratch);
  }
  Status Skip(uint64_t n) { LSMENG_ASSERT_NO_LOCK_FOR_IO(); return SkipImpl(n); }
 protected:
  virtual Status ReadImpl(size_t n, Slice* result, char* scratch) = 0;
  virtual Status SkipImpl(uint64_t n) = 0;
};

class RandomAccessFile {
 public:
  virtual ~RandomAccessFile() = default;
  // Positional. Safe to call concurrently from many threads on one instance (E-25).
  Status Read(uint64_t offset, size_t n, Slice* result, char* scratch) const {
    LSMENG_ASSERT_NO_LOCK_FOR_IO();
    return ReadImpl(offset, n, result, scratch);
  }
 protected:
  virtual Status ReadImpl(uint64_t offset, size_t n, Slice* result, char* scratch) const = 0;
};

class WritableFile {
 public:
  virtual ~WritableFile() = default;
  Status Append(const Slice& data) { LSMENG_ASSERT_NO_LOCK_FOR_IO(); return AppendImpl(data); }
  Status Flush() { LSMENG_ASSERT_NO_LOCK_FOR_IO(); return FlushImpl(); }
  // Sync() must make everything appended so far durable. SPEC S1 depends on it: for an
  // acknowledged sync=true write the bytes are in the WAL and Sync()ed BEFORE the ack.
  Status Sync() { LSMENG_ASSERT_NO_LOCK_FOR_IO(); return SyncImpl(); }
  Status Close() { LSMENG_ASSERT_NO_LOCK_FOR_IO(); return CloseImpl(); }
  virtual uint64_t Size() const = 0;
 protected:
  virtual Status AppendImpl(const Slice& data) = 0;
  virtual Status FlushImpl() = 0;
  virtual Status SyncImpl() = 0;
  virtual Status CloseImpl() = 0;
};

// An acquired directory lock. Destroying it releases the lock.
class FileLock {
 public:
  virtual ~FileLock() = default;
};

class Env {
 public:
  virtual ~Env() = default;

  // The process-wide POSIX implementation. Never owned by the caller.
  static Env* Default();

#define LSMENG_ENV_FWD(sig, call)                 \
  sig { LSMENG_ASSERT_NO_LOCK_FOR_IO(); return call; }

  LSMENG_ENV_FWD(Status NewSequentialFile(const std::string& f, std::unique_ptr<SequentialFile>* r),
                 NewSequentialFileImpl(f, r))
  LSMENG_ENV_FWD(Status NewRandomAccessFile(const std::string& f, std::unique_ptr<RandomAccessFile>* r),
                 NewRandomAccessFileImpl(f, r))
  LSMENG_ENV_FWD(Status NewWritableFile(const std::string& f, std::unique_ptr<WritableFile>* r),
                 NewWritableFileImpl(f, r))
  LSMENG_ENV_FWD(Status NewAppendableFile(const std::string& f, std::unique_ptr<WritableFile>* r),
                 NewAppendableFileImpl(f, r))
  LSMENG_ENV_FWD(bool FileExists(const std::string& f), FileExistsImpl(f))
  LSMENG_ENV_FWD(Status GetChildren(const std::string& d, std::vector<std::string>* r),
                 GetChildrenImpl(d, r))
  LSMENG_ENV_FWD(Status DeleteFile(const std::string& f), DeleteFileImpl(f))
  LSMENG_ENV_FWD(Status CreateDir(const std::string& d), CreateDirImpl(d))
  LSMENG_ENV_FWD(Status DeleteDir(const std::string& d), DeleteDirImpl(d))
  LSMENG_ENV_FWD(Status GetFileSize(const std::string& f, uint64_t* s), GetFileSizeImpl(f, s))
  LSMENG_ENV_FWD(Status RenameFile(const std::string& s, const std::string& t),
                 RenameFileImpl(s, t))
  // SPEC E-7: rename is atomic on POSIX, but the rename itself is not durable until the
  // CONTAINING DIRECTORY is fsynced. Omitting this leaves CURRENT naming a manifest that
  // is gone -- an unopenable database. It is the classic omission in this pattern, which
  // is why it is a first-class Env operation rather than a detail inside one call site.
  LSMENG_ENV_FWD(Status SyncDir(const std::string& d), SyncDirImpl(d))
  LSMENG_ENV_FWD(Status LockFile(const std::string& f, FileLock** l), LockFileImpl(f, l))
  LSMENG_ENV_FWD(Status UnlockFile(FileLock* l), UnlockFileImpl(l))
#undef LSMENG_ENV_FWD

  // Not I/O; no S13 assertion. Nothing in the engine may depend on wall-clock time for
  // correctness (SPEC E-39) -- this exists for stats and for the stall brake only.
  virtual uint64_t NowMicros() = 0;
  virtual void SleepForMicros(uint64_t micros) = 0;

 protected:
  virtual Status NewSequentialFileImpl(const std::string&, std::unique_ptr<SequentialFile>*) = 0;
  virtual Status NewRandomAccessFileImpl(const std::string&, std::unique_ptr<RandomAccessFile>*) = 0;
  virtual Status NewWritableFileImpl(const std::string&, std::unique_ptr<WritableFile>*) = 0;
  virtual Status NewAppendableFileImpl(const std::string&, std::unique_ptr<WritableFile>*) = 0;
  virtual bool FileExistsImpl(const std::string&) = 0;
  virtual Status GetChildrenImpl(const std::string&, std::vector<std::string>*) = 0;
  virtual Status DeleteFileImpl(const std::string&) = 0;
  virtual Status CreateDirImpl(const std::string&) = 0;
  virtual Status DeleteDirImpl(const std::string&) = 0;
  virtual Status GetFileSizeImpl(const std::string&, uint64_t*) = 0;
  virtual Status RenameFileImpl(const std::string&, const std::string&) = 0;
  virtual Status SyncDirImpl(const std::string&) = 0;
  virtual Status LockFileImpl(const std::string&, FileLock**) = 0;
  virtual Status UnlockFileImpl(FileLock*) = 0;
};

// Convenience helpers used by tests and by recovery.
Status ReadFileToString(Env* env, const std::string& fname, std::string* data);
Status WriteStringToFileSync(Env* env, const Slice& data, const std::string& fname);

}  // namespace lsmeng

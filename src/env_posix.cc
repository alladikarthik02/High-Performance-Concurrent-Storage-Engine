// The one file in the engine permitted to call libc I/O directly (SPEC S24).
// scripts/check.sh greps for violations everywhere else and fails the build.

#include "lsmeng/env.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

namespace lsmeng {
namespace {

Status PosixError(const std::string& ctx, int err) {
  std::string m = ctx + ": " + std::strerror(err);
  // ENOENT is the one errno that routinely means "absent", not "broken". Mapping it to
  // NotFound rather than IOError is what lets Open distinguish "no database here" from
  // "a database here that will not open" -- SPEC E-18 requires those to be different
  // Statuses, and callers must be able to branch on code() (S25).
  if (err == ENOENT) return Status::NotFound(m);
  return Status::IOError(m);
}

constexpr size_t kWriteBufferBytes = 64 * 1024;

class PosixSequentialFile final : public SequentialFile {
 public:
  PosixSequentialFile(std::string name, int fd) : name_(std::move(name)), fd_(fd) {}
  ~PosixSequentialFile() override { ::close(fd_); }

 protected:
  Status ReadImpl(size_t n, Slice* result, char* scratch) override {
    while (true) {
      ssize_t r = ::read(fd_, scratch, n);
      if (r < 0) {
        if (errno == EINTR) continue;   // retry: a signal is not an I/O failure
        return PosixError(name_, errno);
      }
      *result = Slice(scratch, static_cast<size_t>(r));
      return Status::OK();
    }
  }
  Status SkipImpl(uint64_t n) override {
    if (::lseek(fd_, static_cast<off_t>(n), SEEK_CUR) == static_cast<off_t>(-1))
      return PosixError(name_, errno);
    return Status::OK();
  }

 private:
  std::string name_;
  int fd_;
};

class PosixRandomAccessFile final : public RandomAccessFile {
 public:
  PosixRandomAccessFile(std::string name, int fd) : name_(std::move(name)), fd_(fd) {}
  ~PosixRandomAccessFile() override { ::close(fd_); }

 protected:
  // pread, never read. SPEC E-25: `read` advances an offset shared by every thread using
  // this descriptor, so two concurrent readers would interleave and produce garbage that
  // looks exactly like an SST checksum failure. pread takes the offset as an argument and
  // touches no shared state, which is what makes one SstReader safe to share across all
  // reader threads with no lock at all (SPEC 3.10).
  Status ReadImpl(uint64_t offset, size_t n, Slice* result, char* scratch) const override {
    size_t done = 0;
    while (done < n) {
      ssize_t r = ::pread(fd_, scratch + done, n - done,
                          static_cast<off_t>(offset + done));
      if (r < 0) {
        if (errno == EINTR) continue;
        *result = Slice(scratch, done);
        return PosixError(name_, errno);
      }
      if (r == 0) break;  // EOF: a short read is reported, not an error
      done += static_cast<size_t>(r);
    }
    *result = Slice(scratch, done);
    return Status::OK();
  }

 private:
  std::string name_;
  int fd_;
};

class PosixWritableFile final : public WritableFile {
 public:
  PosixWritableFile(std::string name, int fd, uint64_t initial_size)
      : name_(std::move(name)), fd_(fd), size_(initial_size) {
    buf_.reserve(kWriteBufferBytes);
  }
  ~PosixWritableFile() override {
    if (fd_ >= 0) { FlushImpl(); ::close(fd_); }
  }
  uint64_t Size() const override { return size_; }

 protected:
  Status AppendImpl(const Slice& data) override {
    size_ += data.size();
    // Small appends accumulate in a user-space buffer; large ones bypass it. A WAL record
    // is typically tens of bytes, and one write(2) per record would put a syscall on the
    // hot write path for no benefit -- Sync() is what provides durability, not write().
    if (buf_.size() + data.size() <= kWriteBufferBytes) {
      buf_.append(data.data(), data.size());
      return Status::OK();
    }
    Status s = FlushImpl();
    if (!s.ok()) return s;
    if (data.size() >= kWriteBufferBytes) return WriteAll(data.data(), data.size());
    buf_.append(data.data(), data.size());
    return Status::OK();
  }

  Status FlushImpl() override {
    if (buf_.empty()) return Status::OK();
    Status s = WriteAll(buf_.data(), buf_.size());
    buf_.clear();
    return s;
  }

  Status SyncImpl() override {
    Status s = FlushImpl();
    if (!s.ok()) return s;
    // fdatasync, not fsync: we do not care about mtime, only about the data and the size.
    // On this container both go through the same virtio path anyway -- T0 measures it.
    if (::fdatasync(fd_) != 0) return PosixError("fdatasync " + name_, errno);
    return Status::OK();
  }

  Status CloseImpl() override {
    Status s = FlushImpl();
    if (fd_ >= 0 && ::close(fd_) != 0 && s.ok()) s = PosixError("close " + name_, errno);
    fd_ = -1;
    return s;
  }

 private:
  Status WriteAll(const char* p, size_t n) {
    while (n > 0) {
      ssize_t w = ::write(fd_, p, n);
      if (w < 0) {
        if (errno == EINTR) continue;
        return PosixError("write " + name_, errno);
      }
      p += w;
      n -= static_cast<size_t>(w);
    }
    return Status::OK();
  }

  std::string name_;
  int fd_;
  uint64_t size_;
  std::string buf_;
};

class PosixFileLock final : public FileLock {
 public:
  PosixFileLock(std::string name, int fd) : name_(std::move(name)), fd_(fd) {}
  ~PosixFileLock() override = default;
  const std::string& name() const { return name_; }
  int fd() const { return fd_; }

 private:
  std::string name_;
  int fd_;
};

// SPEC S16. Read a process's start time from /proc/<pid>/stat field 22.
//
// WHY THIS EXISTS AT ALL. flock() is the primary mechanism, but wanrep's bug B2 measured
// flock SUCCEEDING TWICE on a Docker Desktop bind mount -- i.e. not excluding. SPEC v1
// recorded that candidly and then specified nothing for the likely repeat, while wiring
// an unconditional unlink loop (orphan GC) to it. So the pid file is a second, always-
// active mechanism that does not depend on the filesystem implementing advisory locks.
//
// The start time is what makes it safe against pid reuse: a recycled pid belongs to a
// different process with a different start time, so a stale lock is detectable rather
// than permanent.
bool ProcessStartTime(long pid, unsigned long long* out) {
  char path[64];
  std::snprintf(path, sizeof(path), "/proc/%ld/stat", pid);
  FILE* f = std::fopen(path, "r");
  if (!f) return false;
  // Field 2 (comm) may contain spaces and parentheses, so parse from the LAST ')'.
  std::string content;
  char chunk[512];
  size_t n;
  while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) content.append(chunk, n);
  std::fclose(f);
  size_t rp = content.rfind(')');
  if (rp == std::string::npos) return false;
  const char* p = content.c_str() + rp + 1;
  int field = 2;
  while (*p) {
    while (*p == ' ') ++p;
    if (!*p) break;
    ++field;
    if (field == 22) { *out = std::strtoull(p, nullptr, 10); return true; }
    while (*p && *p != ' ') ++p;
  }
  return false;
}

class PosixEnv final : public Env {
 public:
  uint64_t NowMicros() override {
    struct timeval tv;
    ::gettimeofday(&tv, nullptr);
    return static_cast<uint64_t>(tv.tv_sec) * 1000000 + tv.tv_usec;
  }
  void SleepForMicros(uint64_t micros) override { ::usleep(micros); }

 protected:
  Status NewSequentialFileImpl(const std::string& f, std::unique_ptr<SequentialFile>* r) override {
    int fd = ::open(f.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return PosixError(f, errno);
    r->reset(new PosixSequentialFile(f, fd));
    return Status::OK();
  }
  Status NewRandomAccessFileImpl(const std::string& f, std::unique_ptr<RandomAccessFile>* r) override {
    int fd = ::open(f.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return PosixError(f, errno);
    r->reset(new PosixRandomAccessFile(f, fd));
    return Status::OK();
  }
  Status NewWritableFileImpl(const std::string& f, std::unique_ptr<WritableFile>* r) override {
    int fd = ::open(f.c_str(), O_TRUNC | O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return PosixError(f, errno);
    r->reset(new PosixWritableFile(f, fd, 0));
    return Status::OK();
  }
  Status NewAppendableFileImpl(const std::string& f, std::unique_ptr<WritableFile>* r) override {
    int fd = ::open(f.c_str(), O_APPEND | O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return PosixError(f, errno);
    struct stat st;
    uint64_t sz = (::fstat(fd, &st) == 0) ? static_cast<uint64_t>(st.st_size) : 0;
    r->reset(new PosixWritableFile(f, fd, sz));
    return Status::OK();
  }
  bool FileExistsImpl(const std::string& f) override { return ::access(f.c_str(), F_OK) == 0; }

  Status GetChildrenImpl(const std::string& d, std::vector<std::string>* r) override {
    r->clear();
    DIR* dir = ::opendir(d.c_str());
    if (!dir) return PosixError(d, errno);
    struct dirent* e;
    while ((e = ::readdir(dir)) != nullptr) {
      std::string n(e->d_name);
      if (n != "." && n != "..") r->push_back(n);
    }
    ::closedir(dir);
    return Status::OK();
  }
  Status DeleteFileImpl(const std::string& f) override {
    if (::unlink(f.c_str()) != 0) return PosixError("unlink " + f, errno);
    return Status::OK();
  }
  Status CreateDirImpl(const std::string& d) override {
    if (::mkdir(d.c_str(), 0755) != 0 && errno != EEXIST)
      return PosixError("mkdir " + d, errno);
    return Status::OK();
  }
  Status DeleteDirImpl(const std::string& d) override {
    if (::rmdir(d.c_str()) != 0) return PosixError("rmdir " + d, errno);
    return Status::OK();
  }
  Status GetFileSizeImpl(const std::string& f, uint64_t* s) override {
    struct stat st;
    if (::stat(f.c_str(), &st) != 0) { *s = 0; return PosixError(f, errno); }
    *s = static_cast<uint64_t>(st.st_size);
    return Status::OK();
  }
  Status RenameFileImpl(const std::string& s, const std::string& t) override {
    if (::rename(s.c_str(), t.c_str()) != 0)
      return PosixError("rename " + s + " -> " + t, errno);
    return Status::OK();
  }
  // SPEC E-7. Opening a directory O_RDONLY and fsyncing it is how a rename or a file
  // creation becomes durable. Without this the directory entry can be lost on a crash
  // even though the file's own data was synced.
  Status SyncDirImpl(const std::string& d) override {
    int fd = ::open(d.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return PosixError("open dir " + d, errno);
    int rc = ::fsync(fd);
    int err = errno;
    ::close(fd);
    // Some filesystems refuse fsync on a directory fd with EINVAL. That is not a failure
    // to make anything durable -- it means the filesystem does not implement it -- so it
    // is tolerated rather than turned into a spurious IOError. T0 records which behaviour
    // this container actually has, because the answer decides whether E-7's guarantee
    // holds here at all.
    if (rc != 0 && err != EINVAL) return PosixError("fsync dir " + d, err);
    return Status::OK();
  }

  Status LockFileImpl(const std::string& f, FileLock** l) override {
    *l = nullptr;
    int fd = ::open(f.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return PosixError("open " + f, errno);

    // Mechanism 1: advisory lock. Cheap, and correct wherever it is implemented.
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
      int err = errno;
      ::close(fd);
      return Status::IOError("database is locked (flock): " + f + ": " + std::strerror(err));
    }

    // Mechanism 2: the pid file, always active because mechanism 1 has been observed to
    // silently do nothing on this exact bind mount (wanrep B2).
    char buf[128] = {0};
    ssize_t n = ::pread(fd, buf, sizeof(buf) - 1, 0);
    if (n > 0) {
      long other_pid = 0;
      unsigned long long other_start = 0;
      if (std::sscanf(buf, "%ld %llu", &other_pid, &other_start) == 2 && other_pid > 0 &&
          other_pid != static_cast<long>(::getpid())) {
        unsigned long long live_start = 0;
        if (ProcessStartTime(other_pid, &live_start) && live_start == other_start) {
          ::close(fd);
          char msg[192];
          std::snprintf(msg, sizeof(msg),
                        "database is locked by pid %ld (LOCK file %s)", other_pid, f.c_str());
          return Status::IOError(msg);
        }
        // Otherwise the pid is gone, or was recycled by a different process: stale lock,
        // safe to reclaim. Falling through is the reclaim.
      }
    }

    unsigned long long my_start = 0;
    ProcessStartTime(static_cast<long>(::getpid()), &my_start);
    char rec[64];
    int len = std::snprintf(rec, sizeof(rec), "%ld %llu\n", static_cast<long>(::getpid()), my_start);
    if (::ftruncate(fd, 0) != 0 || ::pwrite(fd, rec, static_cast<size_t>(len), 0) != len) {
      int err = errno;
      ::close(fd);
      return PosixError("write " + f, err);
    }
    ::fdatasync(fd);
    *l = new PosixFileLock(f, fd);
    return Status::OK();
  }

  Status UnlockFileImpl(FileLock* l) override {
    auto* p = static_cast<PosixFileLock*>(l);
    if (!p) return Status::OK();
    // Truncate before releasing so a crashed-then-restarted process does not read its own
    // stale record and mistake it for a live owner.
    (void)!::ftruncate(p->fd(), 0);
    ::flock(p->fd(), LOCK_UN);
    ::close(p->fd());
    delete p;
    return Status::OK();
  }
};

}  // namespace

Env* Env::Default() {
  static PosixEnv env;
  return &env;
}

Status ReadFileToString(Env* env, const std::string& fname, std::string* data) {
  data->clear();
  std::unique_ptr<SequentialFile> f;
  Status s = env->NewSequentialFile(fname, &f);
  if (!s.ok()) return s;
  char space[8192];
  while (true) {
    Slice frag;
    s = f->Read(sizeof(space), &frag, space);
    if (!s.ok()) return s;
    if (frag.empty()) break;
    data->append(frag.data(), frag.size());
  }
  return Status::OK();
}

Status WriteStringToFileSync(Env* env, const Slice& data, const std::string& fname) {
  std::unique_ptr<WritableFile> f;
  Status s = env->NewWritableFile(fname, &f);
  if (!s.ok()) return s;
  s = f->Append(data);
  if (s.ok()) s = f->Sync();
  Status c = f->Close();
  return s.ok() ? c : s;
}

}  // namespace lsmeng

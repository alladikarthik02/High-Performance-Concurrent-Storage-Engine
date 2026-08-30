// T0: measure the environment instead of assuming it. SPEC 8.
//
// Every number this prints goes into docs/BENCHMARKS.md as a MEASURED line. The wanrep
// precedent is the reason this tool exists at all: that project ASSUMED flock excluded on
// a Docker bind mount, and it did not (its bug B2), which silently invalidated a safety
// requirement. Facts that decide correctness get measured on the machine that will run
// the code.
#include <fcntl.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "lsmeng/env.h"

using namespace lsmeng;
using Clock = std::chrono::steady_clock;

static double MedianFsyncMicros(Env* env, const std::string& dir, int iters) {
  std::vector<double> samples;
  const std::string p = dir + "/.fsync_probe";
  std::unique_ptr<WritableFile> f;
  if (!env->NewWritableFile(p, &f).ok()) return -1;
  for (int i = 0; i < iters; ++i) {
    char rec[64];
    std::snprintf(rec, sizeof(rec), "%040d\n", i);
    if (!f->Append(rec).ok()) return -1;
    auto t0 = Clock::now();
    if (!f->Sync().ok()) return -1;
    auto t1 = Clock::now();
    samples.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
  }
  f->Close();
  env->DeleteFile(p);
  std::sort(samples.begin(), samples.end());
  return samples.empty() ? -1 : samples[samples.size() / 2];
}

// SPEC S16 / wanrep B2. The only honest way to ask "does flock exclude here?" is with two
// real processes: flock is per open-file-description, so a same-process second acquisition
// proves nothing about the filesystem.
static const char* FlockExcludesAcrossProcesses(const std::string& dir) {
  const std::string p = dir + "/.flock_probe";
  int fd = ::open(p.c_str(), O_RDWR | O_CREAT, 0644);
  if (fd < 0) return "ERROR (cannot create probe file)";
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) { ::close(fd); return "ERROR (parent could not lock)"; }

  pid_t pid = ::fork();
  if (pid == 0) {
    int cfd = ::open(p.c_str(), O_RDWR, 0644);
    int rc = (cfd >= 0) ? ::flock(cfd, LOCK_EX | LOCK_NB) : -1;
    ::_exit(rc == 0 ? 1 : 0);   // exit 1 == child ALSO got the lock == flock does NOT exclude
  }
  int status = 0;
  ::waitpid(pid, &status, 0);
  ::flock(fd, LOCK_UN);
  ::close(fd);
  ::unlink(p.c_str());
  if (!WIFEXITED(status)) return "ERROR (child did not exit normally)";
  return WEXITSTATUS(status) == 1 ? "NO  -- flock does NOT exclude (wanrep B2 repeats here)"
                                  : "yes -- flock excludes correctly";
}

static const char* SyncDirBehaviour(Env* env, const std::string& dir) {
  Status s = env->SyncDir(dir);
  if (s.ok()) return "supported (or tolerated EINVAL -- see env_posix.cc)";
  return "FAILED";
}

int main(int argc, char** argv) {
  Env* env = Env::Default();
  const std::string bind_dir = (argc > 1) ? argv[1] : "/work/scratch";
  const std::string local_dir = (argc > 2) ? argv[2] : "/data";
  env->CreateDir(bind_dir);
  env->CreateDir(local_dir);

  std::printf("## T0 environment facts (MEASURED)\n\n");

  std::printf("### Machine\n");
#if defined(__aarch64__)
  std::printf("- arch: aarch64 (arm64)\n");
#elif defined(__x86_64__)
  std::printf("- arch: x86_64\n");
#else
  std::printf("- arch: other\n");
#endif
  std::printf("- nproc (hardware_concurrency): %u\n", std::thread::hardware_concurrency());
  std::printf("- page size: %ld bytes\n", ::sysconf(_SC_PAGESIZE));

  std::printf("\n### Hardware CRC32C\n");
  // SPEC T1 needs a hardware CRC32C path. WHICH intrinsic is available is not a detail:
  // _mm_crc32_u64 is SSE4.2 and exists only on x86-64. On Apple Silicon the container is
  // aarch64, where the equivalent is the ARMv8-A CRC extension (__crc32cd). Assuming the
  // x86 one is how you get a build that fails only on the machine you actually have.
#if defined(__x86_64__) && defined(__SSE4_2__)
  std::printf("- x86 SSE4.2 _mm_crc32_u64: available at compile time\n");
#elif defined(__x86_64__)
  std::printf("- x86_64 but SSE4.2 NOT enabled at compile time (needs -msse4.2)\n");
#endif
#if defined(__aarch64__)
#  if defined(__ARM_FEATURE_CRC32)
  std::printf("- ARMv8 CRC extension (__crc32cd): available at compile time\n");
#  else
  std::printf("- aarch64, ARM CRC extension NOT enabled at compile time (needs +crc)\n");
#  endif
#endif

  std::printf("\n### Filesystems\n");
  std::printf("- fsync median, bind mount   (%s): %.1f us\n", bind_dir.c_str(),
              MedianFsyncMicros(env, bind_dir, 200));
  std::printf("- fsync median, container fs (%s): %.1f us\n", local_dir.c_str(),
              MedianFsyncMicros(env, local_dir, 200));
  std::printf("- SyncDir, bind mount:   %s\n", SyncDirBehaviour(env, bind_dir));
  std::printf("- SyncDir, container fs: %s\n", SyncDirBehaviour(env, local_dir));

  std::printf("\n### flock exclusion across two processes (SPEC S16)\n");
  std::printf("- bind mount   (%s): %s\n", bind_dir.c_str(), FlockExcludesAcrossProcesses(bind_dir));
  std::printf("- container fs (%s): %s\n", local_dir.c_str(), FlockExcludesAcrossProcesses(local_dir));

  return 0;
}

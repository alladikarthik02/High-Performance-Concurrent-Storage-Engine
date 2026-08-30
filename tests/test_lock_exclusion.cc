// T7: two-process exclusion. SPEC S16, E-6, and CHALLENGES B4.
//
// WHY THIS NEEDS A REAL SECOND PROCESS. flock() is per open-file-description, so a
// same-process second acquisition proves nothing about the filesystem. Every other layer
// of SPEC 7 is in-process, and layer 4's second process is a kill -9'ed child, not a
// concurrent opener -- so without this file the exclusion property is asserted nowhere.
//
// WHY IT MATTERS MORE HERE THAN ELSEWHERE. E-6's orphan collection UNLINKS FILES, and its
// safety argument is "only because S16 guarantees a single process owns the directory."
// T0 measured flock FAILING to exclude on the Docker bind mount -- exactly as wanrep's bug
// B2 predicted. So this test is the gate: orphan collection is refused unless it passes,
// and the pid-file fallback is what makes it pass at all.
#include "tests/test.h"

#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <string>

#include "lsmeng/env.h"
#include "lsmeng/table_cache.h"
#include "tests/tmpdir.h"

using namespace lsmeng;

namespace {
// Runs in a forked child: try to take the lock, exit 1 if it SUCCEEDED (which means
// exclusion failed), 0 if it was correctly refused.
int ChildTriesToLock(const std::string& path) {
  Env* env = Env::Default();
  FileLock* l = nullptr;
  Status s = env->LockFile(path, &l);
  if (s.ok()) { env->UnlockFile(l); return 1; }
  return 0;
}
}  // namespace

TEST(a_second_process_cannot_take_a_held_lock) {
  testing::TmpDir d("lock");
  Env* env = Env::Default();
  const std::string path = d.file("LOCK");

  FileLock* held = nullptr;
  REQUIRE_OK(env->LockFile(path, &held));

  pid_t pid = ::fork();
  REQUIRE(pid >= 0);
  if (pid == 0) ::_exit(ChildTriesToLock(path));

  int status = 0;
  ::waitpid(pid, &status, 0);
  REQUIRE(WIFEXITED(status));
  // exit 1 == the child ALSO got the lock == exclusion is broken.
  CHECK_EQ(WEXITSTATUS(status), 0);
  std::fprintf(stderr, "   exclusion on %s: %s\n", d.path().c_str(),
               WEXITSTATUS(status) == 0 ? "ENFORCED" : "*** NOT ENFORCED ***");

  CHECK_OK(env->UnlockFile(held));
}

TEST(a_released_lock_can_be_retaken_by_another_process) {
  // Without this, a clean shutdown followed by a restart would be impossible -- and the
  // symptom would be "the database will not open after a normal exit", which is worse
  // than no locking at all.
  testing::TmpDir d("lock");
  Env* env = Env::Default();
  const std::string path = d.file("LOCK");

  FileLock* l = nullptr;
  REQUIRE_OK(env->LockFile(path, &l));
  REQUIRE_OK(env->UnlockFile(l));

  pid_t pid = ::fork();
  REQUIRE(pid >= 0);
  if (pid == 0) ::_exit(ChildTriesToLock(path));
  int status = 0;
  ::waitpid(pid, &status, 0);
  REQUIRE(WIFEXITED(status));
  CHECK_EQ(WEXITSTATUS(status), 1);   // 1 == the child got it, which is correct here
}

TEST(a_lock_left_by_a_dead_process_is_reclaimed) {
  // The pid-file fallback's other half. A process that is kill -9'ed leaves its pid in the
  // LOCK file; without a staleness check the database would be permanently unopenable
  // after any crash -- which is exactly the failure mode a naive pid file introduces.
  testing::TmpDir d("lock");
  Env* env = Env::Default();
  const std::string path = d.file("LOCK");

  pid_t pid = ::fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    FileLock* l = nullptr;
    Env::Default()->LockFile(path, &l);
    ::_exit(0);   // exits WITHOUT unlocking -- the pid record stays behind
  }
  int status = 0;
  ::waitpid(pid, &status, 0);

  FileLock* l = nullptr;
  Status s = env->LockFile(path, &l);
  CHECK_OK(s);   // the dead owner's lock must be reclaimable
  if (s.ok()) CHECK_OK(env->UnlockFile(l));
}

TEST(the_refusal_names_the_holder_so_the_error_is_actionable) {
  // E-18: a database that fails to open must say WHY. "Corruption" with no detail is how
  // debugging sessions get long.
  testing::TmpDir d("lock");
  Env* env = Env::Default();
  const std::string path = d.file("LOCK");
  FileLock* a = nullptr;
  REQUIRE_OK(env->LockFile(path, &a));
  FileLock* b = nullptr;
  Status s = env->LockFile(path, &b);
  CHECK(!s.ok());
  CHECK_CODE(s, Status::Code::kIOError);
  const std::string msg = s.ToString();
  CHECK(msg.find("lock") != std::string::npos || msg.find("LOCK") != std::string::npos);
  std::fprintf(stderr, "   refusal message: %s\n", msg.c_str());
  CHECK_OK(env->UnlockFile(a));
}

RUN_ALL()

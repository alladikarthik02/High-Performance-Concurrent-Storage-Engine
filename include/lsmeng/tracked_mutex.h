#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace lsmeng {

// SPEC S13 enforcement.
//
// THE PROBLEM THIS SOLVES. S13 says db_mutex_ is never held across a read/write/fsync/
// rename/open/unlink. Holding a mutex across I/O is neither a data race nor a lock-order
// inversion, so TSan reports nothing, helgrind reports nothing, DRD reports nothing, and
// a lock-order tracker is structurally the wrong instrument. SPEC v1's only enforcement
// mechanism was the word "inspection" -- and v1's own write path violated the rule, by
// creating a log file inside MakeRoomForWrite while holding the mutex (SPEC 11.10). The
// failure is silent: it costs throughput and tail latency, never correctness, so no
// correctness test can ever catch it.
//
// THE MECHANISM. A TrackedMutex sets a bit in a thread-local mask while held. Every Env
// entry point that can block on the filesystem asserts the bit is clear. A violation
// therefore fails loudly at the first offending call, in every configuration, instead of
// showing up months later as an unexplained p99.9 spike.
//
// COST. One thread-local OR on lock and one AND on unlock. Compiled out entirely when
// LSMENG_ASSERTIONS is off.

enum LockBit : uint32_t {
  kDbMutex = 1u << 0,
  kCacheShard = 1u << 1,
  kTableCacheShard = 1u << 2,
};

inline uint32_t& held_locks() {
  static thread_local uint32_t mask = 0;
  return mask;
}

// LSMENG_ASSERT, not assert().
//
// CMake's RelWithDebInfo (and Release) add -DNDEBUG, which turns every assert() into
// nothing at all. S13's enforcement mechanism would then be compiled out of the exact
// configuration we benchmark and ship -- silently, with the build still printing
// "LSMENG_ASSERTIONS=1" on the command line. An assertion that disappears under the
// build flag most likely to be used is worse than no assertion, because it looks like
// coverage. So this macro does its own check and its own abort, and NDEBUG cannot reach
// it. See CHALLENGES B1.
#if LSMENG_ASSERTIONS
#define LSMENG_ASSERT(cond, msg)                                              \
  do {                                                                        \
    if (!(cond)) {                                                            \
      std::fprintf(stderr, "\nLSMENG ASSERT FAILED %s:%d\n  %s\n  (%s)\n",     \
                   __FILE__, __LINE__, (msg), #cond);                         \
      std::abort();                                                           \
    }                                                                         \
  } while (0)
#else
#define LSMENG_ASSERT(cond, msg) ((void)0)
#endif

#define LSMENG_ASSERT_NO_LOCK_FOR_IO()                                        \
  LSMENG_ASSERT((::lsmeng::held_locks() & ::lsmeng::kDbMutex) == 0,            \
                "SPEC S13 violated: db_mutex_ held across a blocking Env call")

// A std::mutex that records itself in the thread-local mask. Usable with
// std::unique_lock / std::condition_variable_any exactly like a std::mutex, which is why
// it exposes the BasicLockable + Lockable surface rather than wrapping a scoped guard.
template <uint32_t Bit>
class TrackedMutexT {
 public:
  void lock() {
    m_.lock();
#if LSMENG_ASSERTIONS
    held_locks() |= Bit;
#endif
  }
  void unlock() {
#if LSMENG_ASSERTIONS
    held_locks() &= ~Bit;
#endif
    m_.unlock();
  }
  bool try_lock() {
    if (!m_.try_lock()) return false;
#if LSMENG_ASSERTIONS
    held_locks() |= Bit;
#endif
    return true;
  }

  // condition_variable_any calls unlock()/lock() around the wait, so the mask is
  // maintained correctly across a wait with no extra work. That is the reason this type
  // models Lockable rather than hiding a std::mutex behind a guard.
  bool held_by_this_thread() const {
#if LSMENG_ASSERTIONS
    return (held_locks() & Bit) != 0;
#else
    return true;  // cannot know; assertions using this are compiled out anyway
#endif
  }

 private:
  std::mutex m_;
};

using TrackedMutex = TrackedMutexT<kDbMutex>;

}  // namespace lsmeng

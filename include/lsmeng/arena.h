#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace lsmeng {

// Bump allocator for memtable nodes. SPEC 3.4.
//
// WHY: a memtable is filled, frozen, flushed, and destroyed WHOLE. Nothing is ever freed
// individually, so per-node free() and its bookkeeping are pure waste -- and, more
// importantly, the absence of individual frees is what makes the skip list safe for
// lock-free readers with no hazard pointers and no RCU (SPEC 3.4). The allocator is not
// an optimisation; it is part of the concurrency argument.
//
// WHAT IS MEASURED: memory_usage() counts ARENA bytes, not user bytes. The difference --
// node headers, height pointers, alignment -- is what makes a "4 MiB memtable" hold
// considerably less than 4 MiB of user data. T3 measures the real ratio; SPEC assumes
// 2-3x for small records and that number is a hypothesis until it is not.
class Arena {
 public:
  Arena() = default;
  ~Arena() { for (char* b : blocks_) delete[] b; }
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  char* Allocate(size_t bytes) {
    // A zero-byte allocation must still return a distinct usable pointer rather than
    // aliasing the next one, so callers can compare pointers meaningfully.
    if (bytes == 0) bytes = 1;
    if (bytes <= remaining_) {
      char* r = ptr_;
      ptr_ += bytes;
      remaining_ -= bytes;
      return r;
    }
    return AllocateFallback(bytes);
  }

  char* AllocateAligned(size_t bytes) {
    constexpr size_t kAlign = sizeof(void*) > 8 ? sizeof(void*) : 8;
    static_assert((kAlign & (kAlign - 1)) == 0, "alignment must be a power of two");
    size_t mod = reinterpret_cast<uintptr_t>(ptr_) & (kAlign - 1);
    size_t slop = mod ? (kAlign - mod) : 0;
    size_t needed = bytes + slop;
    char* result;
    if (needed <= remaining_) {
      result = ptr_ + slop;
      ptr_ += needed;
      remaining_ -= needed;
    } else {
      result = AllocateFallback(bytes);   // a fresh block is already aligned by new[]
    }
    return result;
  }

  // Read by MakeRoomForWrite from another thread, hence atomic. Relaxed is right: it is a
  // heuristic for "is this memtable full", never a correctness boundary.
  size_t memory_usage() const { return usage_.load(std::memory_order_relaxed); }

 private:
  static constexpr size_t kBlockSize = 4096;

  char* AllocateFallback(size_t bytes) {
    // A request larger than a quarter of a block gets its own exact-sized block, so one
    // oversized value cannot waste most of a shared block -- and, more importantly, so a
    // request bigger than kBlockSize works at all rather than looping forever.
    if (bytes > kBlockSize / 4) return AllocateNewBlock(bytes);
    char* b = AllocateNewBlock(kBlockSize);
    ptr_ = b + bytes;
    remaining_ = kBlockSize - bytes;
    return b;
  }

  char* AllocateNewBlock(size_t size) {
    char* b = new char[size];
    blocks_.push_back(b);
    // Charge the pointer we now own as well as the bytes: with 4 KiB blocks the vector is
    // a rounding error, but "memory usage" that ignores its own bookkeeping is the kind
    // of number that drifts from reality exactly when it matters (S11).
    usage_.store(usage_.load(std::memory_order_relaxed) + size + sizeof(char*),
                 std::memory_order_relaxed);
    return b;
  }

  char* ptr_ = nullptr;
  size_t remaining_ = 0;
  std::vector<char*> blocks_;
  std::atomic<size_t> usage_{0};
};

}  // namespace lsmeng

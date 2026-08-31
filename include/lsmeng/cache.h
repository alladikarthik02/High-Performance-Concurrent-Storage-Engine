#pragma once
#include <cstddef>
#include <cstdint>
#include <list>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "lsmeng/hash.h"
#include "lsmeng/slice.h"

namespace lsmeng {

// SPEC 3.11. A sharded LRU cache.
//
// WHY SHARDING IS THE POINT, and not a micro-optimisation: an LRU is MUTATED ON EVERY HIT
// -- the entry moves to the head of the recency list -- so even a pure-read workload
// serialises every block access in the process behind one mutex. That is the concrete,
// measurable contention story for R12, and T12 benchmarks 1 vs 4 vs 16 shards to put a
// number on it. If the number turns out to be small, that negative result gets reported
// rather than buried (the wanrep precedent: its "lock-free beats a mutex" hypothesis was
// measured and killed).
//
// THE LOCK IS NEVER HELD ACROSS I/O (SPEC 3.10, L2). A miss releases the shard lock,
// reads the block, and then inserts. The cost is that two threads missing on the same
// block both read it -- the accepted "cache-miss stampede" of E-12 -- and the benefit is
// that no reader ever waits on another reader's disk I/O.
//
// Entries are REFCOUNTED, so a block currently being read cannot be evicted out from under
// its reader. Eviction only unlinks; the last Release() frees.

class Cache {
 public:
  struct Handle;

  Cache(size_t capacity_bytes, int num_shards);
  ~Cache();
  Cache(const Cache&) = delete;
  Cache& operator=(const Cache&) = delete;

  // Takes ownership of `value` via `deleter`, called when the entry is finally freed.
  Handle* Insert(const Slice& key, void* value, size_t charge, void (*deleter)(void*));
  Handle* Lookup(const Slice& key);
  void Release(Handle* h);
  void* Value(Handle* h);
  void Erase(const Slice& key);

  size_t TotalCharge() const;
  size_t NumEntries() const;
  int num_shards() const { return static_cast<int>(shards_.size()); }

  // A stable key for a (file, offset) block, written into a caller-supplied buffer.
  //
  // It used to return a std::string, which meant a heap allocation on EVERY block access --
  // and T12's profile showed allocation costing more cycles than lock contention. The
  // buffer form costs nothing. `buf` must live as long as the returned Slice is used.
  static constexpr size_t kBlockKeySize = 16;
  static Slice BlockKey(char* buf, uint64_t file_number, uint64_t offset);

 private:
  struct Entry;
  static Entry* ToEntry(Handle* h);
  static Handle* ToHandle(Entry* e);

  struct Entry {
    std::string key;
    void* value = nullptr;
    size_t charge = 0;
    void (*deleter)(void*) = nullptr;
    uint32_t refs = 0;      // 1 for the cache's own reference, plus one per live Handle
    bool in_cache = false;
    int shard = 0;          // so Release() need not re-hash the key
    std::list<Entry*>::iterator lru_pos{};
  };

  // HETEROGENEOUS LOOKUP. Without `is_transparent` on both the hash and the equality,
  // `table_.find(some_view)` does not compile and the only option is
  // `table_.find(key.ToString())` -- a heap allocation on every lookup, constructed WHILE
  // HOLDING THE SHARD MUTEX. That is both a cost in itself and a lengthening of the
  // critical section, which is why it showed up in the profile beside the lock rows rather
  // than instead of them. See CHALLENGES B21.
  struct TransparentHash {
    using is_transparent = void;
    size_t operator()(std::string_view s) const {
      return static_cast<size_t>(Hash64(s.data(), s.size()));
    }
  };
  struct TransparentEq {
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const { return a == b; }
  };
  using Table = std::unordered_map<std::string, Entry*, TransparentHash, TransparentEq>;

  class Shard {
   public:
    void SetCapacity(size_t c) { capacity_ = c; }
    Handle* Insert(Entry* prepared);   // the Entry is BUILT OUTSIDE the lock
    Handle* Lookup(const Slice& key);
    void Release(Entry* e);
    void Erase(const Slice& key);
    size_t charge() const { std::lock_guard<std::mutex> g(mu_); return usage_; }
    size_t entries() const { std::lock_guard<std::mutex> g(mu_); return table_.size(); }
    ~Shard();

   private:
    void Unref(Entry* e);              // caller holds mu_
    void EvictIfNeeded();              // caller holds mu_
    void RemoveFromTable(Entry* e);    // caller holds mu_

    mutable std::mutex mu_;
    size_t capacity_ = 0;
    size_t usage_ = 0;
    Table table_;
    std::list<Entry*> lru_;            // front = least recently used
  };

  size_t ShardIndexFor(const Slice& key) const {
    return Hash64(key.data(), key.size()) % shards_.size();
  }
  Shard* ShardFor(const Slice& key) { return &shards_[ShardIndexFor(key)]; }

  std::vector<Shard> shards_;
};

}  // namespace lsmeng

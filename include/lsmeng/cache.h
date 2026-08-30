#pragma once
#include <cstddef>
#include <cstdint>
#include <list>
#include <mutex>
#include <string>
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

  // A stable 64-bit key for a (file, offset) block, so callers do not each invent one.
  static std::string BlockKey(uint64_t file_number, uint64_t offset);

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
    std::list<Entry*>::iterator lru_pos{};
  };

  class Shard {
   public:
    void SetCapacity(size_t c) { capacity_ = c; }
    Handle* Insert(const Slice& key, void* value, size_t charge, void (*deleter)(void*));
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
    std::unordered_map<std::string, Entry*> table_;
    std::list<Entry*> lru_;            // front = least recently used
  };

  Shard* ShardFor(const Slice& key) {
    return &shards_[Hash64(key.data(), key.size()) % shards_.size()];
  }

  std::vector<Shard> shards_;
};

}  // namespace lsmeng

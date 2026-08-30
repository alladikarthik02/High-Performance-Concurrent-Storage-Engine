#include "lsmeng/cache.h"

#include "lsmeng/coding.h"

namespace lsmeng {

// A Handle is just an opaque view of an Entry. Keeping them distinct types stops callers
// from reaching into the entry and, more usefully, makes "you must Release() this" visible
// at the call site.
struct Cache::Handle {};

// Handle <-> Entry is a pure reinterpretation: a Handle* IS an Entry*, typed opaquely so
// callers cannot reach inside. These are member functions rather than free ones because
// Entry is private -- keeping it private is the point.
Cache::Entry* Cache::ToEntry(Cache::Handle* h) { return reinterpret_cast<Entry*>(h); }
Cache::Handle* Cache::ToHandle(Cache::Entry* e) { return reinterpret_cast<Handle*>(e); }

Cache::Shard::~Shard() {
  for (auto& kv : table_) {
    Entry* e = kv.second;
    if (e->deleter) e->deleter(e->value);
    delete e;
  }
}

void Cache::Shard::RemoveFromTable(Entry* e) {
  if (!e->in_cache) return;
  e->in_cache = false;
  usage_ -= e->charge;
  lru_.erase(e->lru_pos);
  table_.erase(e->key);
  --e->refs;   // drop the cache's own reference
}

void Cache::Shard::Unref(Entry* e) {
  if (--e->refs == 0) {
    // Only reachable once the entry has left the table AND no handle remains. This is what
    // makes it safe for a compaction to evict a block a reader is still holding: eviction
    // unlinks, the last Release frees.
    if (e->deleter) e->deleter(e->value);
    delete e;
  }
}

void Cache::Shard::EvictIfNeeded() {
  while (usage_ > capacity_ && !lru_.empty()) {
    Entry* victim = lru_.front();
    RemoveFromTable(victim);
    if (victim->refs == 0) {
      if (victim->deleter) victim->deleter(victim->value);
      delete victim;
    }
    // If refs > 0 the entry is in use; it is out of the table and will be freed by its
    // last Release. It no longer counts against usage_.
  }
}

Cache::Handle* Cache::Shard::Insert(const Slice& key, void* value, size_t charge,
                                    void (*deleter)(void*)) {
  std::lock_guard<std::mutex> g(mu_);

  // An existing entry for this key is evicted first. Two threads can legitimately race to
  // insert the same block -- the accepted stampede of E-12 -- and the later one wins.
  auto it = table_.find(key.ToString());
  if (it != table_.end()) {
    Entry* old = it->second;
    RemoveFromTable(old);
    if (old->refs == 0) { if (old->deleter) old->deleter(old->value); delete old; }
  }

  Entry* e = new Entry();
  e->key = key.ToString();
  e->value = value;
  e->charge = charge;
  e->deleter = deleter;
  e->refs = 2;          // one for the cache, one for the handle we return
  e->in_cache = true;

  lru_.push_back(e);
  e->lru_pos = std::prev(lru_.end());
  table_[e->key] = e;
  usage_ += charge;

  EvictIfNeeded();
  return Cache::ToHandle(e);
}

Cache::Handle* Cache::Shard::Lookup(const Slice& key) {
  std::lock_guard<std::mutex> g(mu_);
  auto it = table_.find(key.ToString());
  if (it == table_.end()) return nullptr;
  Entry* e = it->second;
  ++e->refs;
  // The mutation that makes even a read-only workload contend: touch moves the entry to
  // the most-recently-used end.
  lru_.erase(e->lru_pos);
  lru_.push_back(e);
  e->lru_pos = std::prev(lru_.end());
  return Cache::ToHandle(e);
}

void Cache::Shard::Release(Entry* e) {
  std::lock_guard<std::mutex> g(mu_);
  Unref(e);
}

void Cache::Shard::Erase(const Slice& key) {
  std::lock_guard<std::mutex> g(mu_);
  auto it = table_.find(key.ToString());
  if (it == table_.end()) return;
  Entry* e = it->second;
  RemoveFromTable(e);
  if (e->refs == 0) { if (e->deleter) e->deleter(e->value); delete e; }
}

Cache::Cache(size_t capacity_bytes, int num_shards) {
  if (num_shards < 1) num_shards = 1;
  shards_ = std::vector<Shard>(static_cast<size_t>(num_shards));
  // Round the per-shard capacity UP, so the total is never less than requested. Rounding
  // down would make a 16-shard cache smaller than a 1-shard cache of the same nominal
  // size, and the T12 comparison would then be measuring two different caches.
  const size_t per = (capacity_bytes + num_shards - 1) / static_cast<size_t>(num_shards);
  for (auto& s : shards_) s.SetCapacity(per);
}

Cache::~Cache() = default;

Cache::Handle* Cache::Insert(const Slice& key, void* value, size_t charge,
                             void (*deleter)(void*)) {
  return ShardFor(key)->Insert(key, value, charge, deleter);
}
Cache::Handle* Cache::Lookup(const Slice& key) { return ShardFor(key)->Lookup(key); }
void Cache::Release(Handle* h) {
  Entry* e = ToEntry(h);
  ShardFor(Slice(e->key))->Release(e);
}
void* Cache::Value(Handle* h) { return ToEntry(h)->value; }
void Cache::Erase(const Slice& key) { ShardFor(key)->Erase(key); }

size_t Cache::TotalCharge() const {
  size_t n = 0;
  for (const auto& s : shards_) n += s.charge();
  return n;
}
size_t Cache::NumEntries() const {
  size_t n = 0;
  for (const auto& s : shards_) n += s.entries();
  return n;
}

std::string Cache::BlockKey(uint64_t file_number, uint64_t offset) {
  std::string k;
  PutFixed64(&k, file_number);
  PutFixed64(&k, offset);
  return k;
}

}  // namespace lsmeng

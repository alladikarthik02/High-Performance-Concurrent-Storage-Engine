#pragma once
#include <atomic>
#include <string>

#include "lsmeng/arena.h"
#include "lsmeng/coding.h"
#include "lsmeng/dbformat.h"
#include "lsmeng/iterator.h"
#include "lsmeng/skiplist.h"
#include "lsmeng/status.h"

namespace lsmeng {

// SPEC 3.4. The in-memory write buffer: a skip list over encoded internal keys, backed by
// an arena, with one writer and lock-free readers.
//
// ENTRY ENCODING (one contiguous arena allocation per entry, so a node is one pointer
// chase, not two):
//
//   [ varint32 : internal_key_size ]   == user_key.size() + 8
//   [ internal key                 ]   user_key || (seq << 8 | type)
//   [ varint32 : value_size        ]
//   [ value                        ]   absent-but-length-prefixed-zero for a deletion
//
// The skip list's Key is `const char*` pointing at the start of that record, and the
// comparator decodes the length prefix. Storing the key inline rather than as a pointer to
// a separate allocation is what keeps a lookup to one cache-line chain per level.

class MemTableIterator;

class MemTable {
 public:
  MemTable() : table_(KeyComparator(), &arena_) {}
  MemTable(const MemTable&) = delete;
  MemTable& operator=(const MemTable&) = delete;

  // Refcounted: a reader takes a reference under db_mutex_ and then does all its work with
  // no lock held (SPEC 3.7 step 1). The memtable outlives the mutex critical section, not
  // the other way round.
  //
  // THE CONSTRUCTOR ALREADY GRANTS ONE REFERENCE to whoever created it. So `new MemTable()`
  // must NOT be followed by Ref() -- doing that made the count go 2 -> 1 on flush and never
  // reach zero, leaking the whole arena every time (CHALLENGES B16). Ref() is for
  // ADDITIONAL holders: a reader pinning it, or the flush pinning it while it works.
  void Ref() { refs_.fetch_add(1, std::memory_order_relaxed); }
  void Unref() {
    if (refs_.fetch_sub(1, std::memory_order_acq_rel) == 1) delete this;
  }

  size_t ApproximateMemoryUsage() const { return arena_.memory_usage(); }

  // SINGLE WRITER ONLY (S12). The caller must be the group-commit leader.
  void Add(SequenceNumber seq, ValueType type, const Slice& key, const Slice& value);

  // Returns true if `key` is present in this memtable -- INCLUDING as a tombstone, in
  // which case *s is set to NotFound. The distinction matters: "found a tombstone" must
  // stop the search, while "not in this memtable" must continue to the next level. A Get
  // that conflated them would read through a delete to an older value (SPEC 3.7).
  bool Get(const LookupKey& key, std::string* value, Status* s);

  MemTableIterator* NewIterator();

  // The same traversal, wrapped in the common Iterator interface so the merging iterator
  // can treat a memtable and an SST identically. Caller owns it; it does NOT own the
  // memtable, so the caller must keep a reference for its lifetime.
  Iterator* NewInternalIterator();

 private:
  friend class MemTableIterator;
  ~MemTable() = default;   // only Unref() may destroy it

  struct KeyComparator {
    int operator()(const char* a, const char* b) const {
      // Both are length-prefixed encoded internal keys.
      uint32_t alen = 0, blen = 0;
      const char* ap = GetVarint32Ptr(a, a + 5, &alen);
      const char* bp = GetVarint32Ptr(b, b + 5, &blen);
      return InternalKeyComparator::Compare(Slice(ap, alen), Slice(bp, blen));
    }
  };

  using Table = SkipList<const char*, KeyComparator>;

  Arena arena_;
  Table table_;
  std::atomic<int> refs_{1};
};

class MemTableIterator {
 public:
  explicit MemTableIterator(MemTable* mem) : iter_(&mem->table_) {}

  bool Valid() const { return iter_.Valid(); }
  void SeekToFirst() { iter_.SeekToFirst(); }
  void SeekToLast() { iter_.SeekToLast(); }
  void Next() { iter_.Next(); }
  void Prev() { iter_.Prev(); }

  void Seek(const Slice& internal_key) {
    // The skip list is keyed by the length-prefixed encoding, so a seek target must be
    // encoded the same way. Building it in a member buffer avoids an allocation per seek.
    tmp_.clear();
    PutLengthPrefixedSlice(&tmp_, internal_key);
    iter_.Seek(tmp_.data());
  }

  Slice key() const {
    uint32_t len = 0;
    const char* p = GetVarint32Ptr(iter_.key(), iter_.key() + 5, &len);
    return Slice(p, len);
  }

  Slice value() const {
    uint32_t klen = 0;
    const char* kp = GetVarint32Ptr(iter_.key(), iter_.key() + 5, &klen);
    uint32_t vlen = 0;
    const char* vp = GetVarint32Ptr(kp + klen, kp + klen + 5, &vlen);
    return Slice(vp, vlen);
  }

 private:
  MemTable::Table::Iterator iter_;
  std::string tmp_;
};

}  // namespace lsmeng

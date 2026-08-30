#pragma once
#include "lsmeng/slice.h"
#include "lsmeng/status.h"

namespace lsmeng {

// The one iteration interface, implemented by the block reader, the SST reader, the
// memtable, and the merging iterator -- so that a compaction merging four SSTs and a flush
// draining a memtable are the same code path over different sources.
//
// CONTRACT:
//   * key() and value() are only valid while Valid(), and only until the next movement.
//     They point INTO the underlying block or arena; they are not copies. That is what
//     makes a scan allocation-free, and it is why callers that need to keep a key must
//     copy it.
//   * status() reports a read or checksum failure. An iterator that hits corruption
//     becomes !Valid() and records why -- it never returns garbage and never throws.
class Iterator {
 public:
  Iterator() = default;
  virtual ~Iterator() = default;
  Iterator(const Iterator&) = delete;
  Iterator& operator=(const Iterator&) = delete;

  virtual bool Valid() const = 0;
  virtual void SeekToFirst() = 0;
  virtual void SeekToLast() = 0;
  virtual void Seek(const Slice& target) = 0;
  virtual void Next() = 0;
  virtual void Prev() = 0;
  virtual Slice key() const = 0;
  virtual Slice value() const = 0;
  virtual Status status() const = 0;
};

// An iterator that is permanently empty, carrying a Status. Returned instead of nullptr when a
// table cannot be opened, so every caller handles failure through the same path it
// already has for "no more entries" -- there is no separate null check to forget.
Iterator* NewErrorIterator(const Status& status);
Iterator* NewEmptyIterator();

}  // namespace lsmeng

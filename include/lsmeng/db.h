#pragma once
#include <cstdint>
#include <string>

#include "lsmeng/iterator.h"
#include "lsmeng/options.h"
#include "lsmeng/slice.h"
#include "lsmeng/status.h"
#include "lsmeng/write_batch.h"

namespace lsmeng {

// Opaque to callers, but a real base class so DBImpl can downcast. Declaring it as a bare
// forward reference (which is what the spec's API sketch implied) makes it impossible to
// implement without a reinterpret_cast, which is exactly the kind of thing that compiles
// on one build and not another.
class Snapshot {
 public:
  virtual ~Snapshot() = default;
};

// SPEC 4.1. The public surface.
class DB {
 public:
  static Status Open(const Options& options, const std::string& dir, DB** out);

  DB() = default;
  virtual ~DB();
  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;

  virtual Status Put(const WriteOptions&, const Slice& key, const Slice& value) = 0;
  virtual Status Delete(const WriteOptions&, const Slice& key) = 0;
  virtual Status Write(const WriteOptions&, WriteBatch* updates) = 0;
  virtual Status Get(const ReadOptions&, const Slice& key, std::string* value) = 0;
  virtual Iterator* NewIterator(const ReadOptions&) = 0;

  virtual const Snapshot* GetSnapshot() = 0;
  virtual void ReleaseSnapshot(const Snapshot*) = 0;

  virtual bool GetProperty(const Slice& name, std::string* out) = 0;
  // nullptr means unbounded in that direction. An empty Slice cannot serve as a sentinel
  // because the EMPTY KEY IS LEGAL (SPEC 3.1) -- a real distinction, not pedantry.
  virtual Status CompactRange(const Slice* begin, const Slice* end) = 0;
  virtual Status Flush() = 0;

  // Idempotent. Stops accepting writes, joins the background thread, and returns any
  // sticky bg_error_ (S17). ~DB() calls it and discards the result, which is why an
  // application that cares about the last error must call Close() itself.
  virtual Status Close() = 0;
};

// SPEC 3.12 / E-10. A leaked snapshot stops space reclamation FOREVER -- compaction cannot
// drop anything newer than the oldest live snapshot -- while the database looks perfectly
// healthy. So this RAII wrapper is what tests and benchmarks must use; the raw
// GetSnapshot/ReleaseSnapshot pair remains for C-style embedders.
class SnapshotHandle {
 public:
  explicit SnapshotHandle(DB* db) : db_(db), snap_(db->GetSnapshot()) {}
  ~SnapshotHandle() { if (db_ && snap_) db_->ReleaseSnapshot(snap_); }
  SnapshotHandle(const SnapshotHandle&) = delete;
  SnapshotHandle& operator=(const SnapshotHandle&) = delete;
  const Snapshot* get() const { return snap_; }

 private:
  DB* db_;
  const Snapshot* snap_;
};

// Removes a database directory entirely. Used by tests; refuses if the database is locked.
Status DestroyDB(const std::string& dir, const Options& options);

}  // namespace lsmeng

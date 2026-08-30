#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <set>
#include <string>
#include <thread>

#include "lsmeng/db.h"
#include "lsmeng/memtable.h"
#include "lsmeng/stats.h"
#include "lsmeng/table_cache.h"
#include "lsmeng/tracked_mutex.h"
#include "lsmeng/version_set.h"
#include "lsmeng/wal.h"

namespace lsmeng {

// A snapshot is a sequence number plus a registration in a sorted list held under
// db_mutex_. Compaction reads the front of that list, ONCE, at schedule time (SPEC 3.8.3).
class SnapshotImpl : public Snapshot {
 public:
  explicit SnapshotImpl(SequenceNumber s) : sequence(s) {}
  SequenceNumber sequence;
};

Iterator* NewDBIterator(Iterator* internal, SequenceNumber sequence);

class DBImpl : public DB {
 public:
  DBImpl(const Options& raw_options, const std::string& dbname);
  ~DBImpl() override;

  Status Put(const WriteOptions&, const Slice& key, const Slice& value) override;
  Status Delete(const WriteOptions&, const Slice& key) override;
  Status Write(const WriteOptions&, WriteBatch* updates) override;
  Status Get(const ReadOptions&, const Slice& key, std::string* value) override;
  Iterator* NewIterator(const ReadOptions&) override;
  const Snapshot* GetSnapshot() override;
  void ReleaseSnapshot(const Snapshot*) override;
  bool GetProperty(const Slice& name, std::string* out) override;
  Status CompactRange(const Slice* begin, const Slice* end) override;
  Status Flush() override;
  Status Close() override;

  Status Recover();
  Status NewDB();

 private:
  friend class DB;

  // One queued writer. SPEC 3.2: a thread performs a WAL append or a memtable insert ONLY
  // while it is writers_.front(). There is exactly one front, so the log has one writer
  // and the memtable has one writer, with neither needing its own lock.
  struct Writer {
    Status status;
    WriteBatch* batch = nullptr;
    bool sync = false;
    bool done = false;
    std::condition_variable_any cv;
  };

  WriteBatch* BuildBatchGroup(Writer** last_writer);
  Status MakeRoomForWrite(bool force);      // mutex held on entry and exit; performs NO I/O
  Status SwitchMemtableLocked();            // releases the mutex to create the log
  void MaybeScheduleBackground();
  void BackgroundLoop();
  Status FlushImmutableMemtable();
  Status WriteLevel0Table(MemTable* mem, VersionEdit* edit);
  Status RecoverLogFile(uint64_t log_number, SequenceNumber* max_sequence,
                        VersionEdit* edit, bool* saved_a_table);
  void RemoveObsoleteFiles();
  void ReportOrphans();
  Status ValidateKeyValue(const Slice& key, const Slice& value) const;
  SequenceNumber OldestSnapshotLocked() const;

  Env* const env_;
  const std::string dbname_;
  const Options options_;
  Stats stats_;

  std::unique_ptr<Cache> block_cache_;
  std::unique_ptr<TableCache> table_cache_;

  TrackedMutex mutex_;                       // L1 -- never held across I/O (S13)
  std::condition_variable_any bg_cv_;
  FileLock* db_lock_ = nullptr;

  std::deque<Writer*> writers_;
  WriteBatch* tmp_batch_ = nullptr;

  MemTable* mem_ = nullptr;
  MemTable* imm_ = nullptr;                  // being flushed
  std::atomic<bool> has_imm_{false};

  std::unique_ptr<WritableFile> logfile_;
  std::unique_ptr<WalWriter> log_;
  uint64_t logfile_number_ = 0;

  std::unique_ptr<VersionSet> versions_;

  std::set<uint64_t> pending_outputs_;       // files being written; never GC'd
  std::vector<SnapshotImpl*> snapshots_;     // ascending by sequence

  std::thread bg_thread_;
  bool bg_running_ = false;
  bool bg_work_scheduled_ = false;
  bool shutting_down_ = false;
  bool closed_ = false;
  Status bg_error_;                          // sticky (S17)
};

}  // namespace lsmeng

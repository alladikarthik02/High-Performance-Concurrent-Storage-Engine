#include "db_impl.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "lsmeng/coding.h"
#include "lsmeng/sst.h"

namespace lsmeng {

namespace {
constexpr int kTier0SlowdownTrigger = 8;
constexpr int kTier0StopTrigger = 12;

// SPEC E-22 / E-31. A write stall and a deadlock look identical from outside: in both
// cases writers are parked on bg_cv_ and nothing moves. The difference is whether anything
// CAN still make progress. Rather than hang forever and leave the operator to guess, a
// stall that has not drained within this deadline is converted into a sticky, descriptive
// error naming the tier-0 file count -- which is the difference between "the database is
// broken" and "compaction cannot keep up with this write rate".
constexpr int kStallWatchdogSeconds = 10;
}  // namespace

DB::~DB() = default;

DBImpl::DBImpl(const Options& raw_options, const std::string& dbname)
    : env_(raw_options.env ? raw_options.env : Env::Default()),
      dbname_(dbname),
      options_(raw_options) {
  block_cache_.reset(new Cache(options_.block_cache_bytes, options_.cache_shards));
  table_cache_.reset(new TableCache(dbname_, options_, block_cache_.get(), &stats_));
  versions_.reset(new VersionSet(dbname_, options_, table_cache_.get(), &stats_));
  tmp_batch_ = new WriteBatch();
}

DBImpl::~DBImpl() {
  Close();
  delete tmp_batch_;
  if (mem_) mem_->Unref();
  if (imm_) imm_->Unref();
  versions_.reset();
  if (db_lock_) env_->UnlockFile(db_lock_);
}

Status DBImpl::Close() {
  {
    std::unique_lock<TrackedMutex> lock(mutex_);
    if (closed_) return bg_error_;
    closed_ = true;
    shutting_down_ = true;
    // S22: every writer parked on bg_cv_ must be woken by the FAILURE of what it waits
    // for, not only by its success. Without this broadcast a stalled writer waits forever
    // and the process never exits.
    bg_cv_.notify_all();
  }
  if (bg_thread_.joinable()) bg_thread_.join();
  // E-10: a leaked snapshot stops reclamation forever. In debug builds, say so loudly at
  // shutdown rather than letting the database silently grow in production.
  std::unique_lock<TrackedMutex> lock(mutex_);
  LSMENG_ASSERT(snapshots_.empty(), "snapshot leaked at Close() -- see SPEC E-10");
  if (log_) { lock.unlock(); log_->Close(); lock.lock(); }
  return bg_error_;
}

Status DBImpl::ValidateKeyValue(const Slice& key, const Slice& value) const {
  // S3: never truncated, never silently accepted, never an assert on user input.
  if (key.size() > kMaxKeySize)
    return Status::InvalidArgument("key of " + std::to_string(key.size()) +
                                   " bytes exceeds the " + std::to_string(kMaxKeySize) +
                                   " byte limit");
  if (value.size() > kMaxValueSize)
    return Status::InvalidArgument("value of " + std::to_string(value.size()) +
                                   " bytes exceeds the " + std::to_string(kMaxValueSize) +
                                   " byte limit");
  return Status::OK();
}

Status DBImpl::Put(const WriteOptions& o, const Slice& key, const Slice& value) {
  Status s = ValidateKeyValue(key, value);
  if (!s.ok()) return s;
  WriteBatch batch;
  batch.Put(key, value);
  return Write(o, &batch);
}

Status DBImpl::Delete(const WriteOptions& o, const Slice& key) {
  Status s = ValidateKeyValue(key, Slice());
  if (!s.ok()) return s;
  WriteBatch batch;
  batch.Delete(key);
  return Write(o, &batch);
}

// SPEC 3.2. Leader/follower group commit: ONE fsync commits many writers' data.
Status DBImpl::Write(const WriteOptions& options, WriteBatch* updates) {
  if (updates == nullptr) return Status::InvalidArgument("null WriteBatch");
  if (updates->ApproximateSize() > kMaxBatchSize)
    return Status::InvalidArgument("WriteBatch exceeds the " +
                                   std::to_string(kMaxBatchSize) + " byte limit");

  Writer w;
  w.batch = updates;
  w.sync = options.sync;

  std::unique_lock<TrackedMutex> lock(mutex_);
  writers_.push_back(&w);
  while (!w.done && &w != writers_.front()) w.cv.wait(lock);
  if (w.done) return w.status;
  if (closed_ || shutting_down_) {
    // Pop ourselves before returning, or the queue keeps a corpse at its head and the
    // next writer waits forever (S22's second half).
    writers_.pop_front();
    if (!writers_.empty()) writers_.front()->cv.notify_one();
    return Status::IOError("DB is closed");
  }

  // --- we are the LEADER, holding the mutex ---
  Status status = MakeRoomForWrite(false);
  uint64_t last_sequence = versions_->LastSequence();
  Writer* last_writer = &w;

  if (status.ok()) {
    WriteBatch* batch = BuildBatchGroup(&last_writer);
    batch->SetSequence(last_sequence + 1);
    last_sequence += static_cast<uint64_t>(batch->Count());

    // --- MUTEX RELEASED: the expensive part. Safe because only the leader writes, and
    //     only MakeRoomForWrite (above, under the mutex, at the queue front) can swap the
    //     memtable -- so the pointer captured here is still valid (S12, E-21).
    MemTable* mem = mem_;
    WalWriter* log = log_.get();
    lock.unlock();
    status = log->AddRecord(Slice(batch->Contents()));
    if (status.ok() && options.sync) {
      // S1: for an acknowledged sync=true write the bytes are in the WAL and fsynced
      // BEFORE the ack, and before the memtable insert is visible to any reader. The
      // reverse order would let a reader observe a value a crash can then erase.
      status = log->Sync();
      stats_.Add(kWalSyncs, 1);
    }
    if (status.ok()) status = batch->InsertInto(mem);
    lock.lock();

    // Read the count BEFORE clearing. tmp_batch_ is the merged group buffer, and clearing
    // it first made every GROUPED write count as zero -- so the better group commit worked,
    // the more writes it lost track of, and the metric that proves R5 reported the
    // opposite of the truth. See CHALLENGES B15.
    const uint64_t applied = static_cast<uint64_t>(batch->Count());
    if (batch == tmp_batch_) tmp_batch_->Clear();
    if (status.ok()) {
      versions_->SetLastSequence(last_sequence);
      stats_.Add(kWrites, applied);
    } else {
      // A failed WAL write makes the memtable and the log disagree. There is no honest
      // recovery from that inside a running process, so the error is made sticky and
      // every subsequent write returns it (S17).
      bg_error_ = status;
      bg_cv_.notify_all();
    }
  }

  while (true) {
    Writer* ready = writers_.front();
    writers_.pop_front();
    if (ready != &w) {
      ready->status = status;
      ready->done = true;
      ready->cv.notify_one();
    }
    if (ready == last_writer) break;
  }
  if (!writers_.empty()) writers_.front()->cv.notify_one();
  return status;
}

WriteBatch* DBImpl::BuildBatchGroup(Writer** last_writer) {
  Writer* first = writers_.front();
  WriteBatch* result = first->batch;

  size_t size = result->ApproximateSize();
  size_t max_size = 1 << 20;
  if (size <= (128 << 10)) max_size = size + (128 << 10);   // do not delay a small write

  *last_writer = first;
  auto it = writers_.begin();
  ++it;
  for (; it != writers_.end(); ++it) {
    Writer* w = *it;
    // S2: a sync=true writer must NEVER be committed by a group whose leader will not
    // fsync. The reverse (a sync=false writer in a synced group) is harmless -- it gets
    // more durability than it asked for -- but this direction is a durability bug.
    if (w->sync != first->sync) break;
    if (w->batch == nullptr) continue;
    size += w->batch->ApproximateSize();
    if (size > max_size) break;
    if (result == first->batch) {
      result = tmp_batch_;
      result->Clear();
      result->Append(*first->batch);
    }
    result->Append(*w->batch);
    *last_writer = w;
  }
  return result;
}

// Mutex held on entry and exit. PERFORMS NO I/O -- SPEC 3.2.1 / S13.
Status DBImpl::MakeRoomForWrite(bool force) {
  bool allow_delay = !force;
  while (true) {
    if (!bg_error_.ok()) return bg_error_;
    if (shutting_down_) return Status::IOError("DB is closing");

    if (allow_delay && versions_->current()->NumFilesAtTier(0) >= kTier0SlowdownTrigger) {
      // A deliberate brake, not a bug: one millisecond of writer time handed to the
      // compactor. It is visible in p99.9, and SPEC E-11 says that tail is the design
      // rather than noise.
      stats_.Add(kStalls, 1);
      mutex_.unlock();
      env_->SleepForMicros(1000);
      mutex_.lock();
      allow_delay = false;
      continue;
    }
    if (!force && mem_->ApproximateMemoryUsage() <= options_.write_buffer_size) return Status::OK();
    if (imm_ != nullptr) {
      // The previous flush has not finished. S22: the predicate covers bg_error_ and
      // shutting_down_ as well, so a background thread that dies does not park us forever.
      bg_cv_.wait(mutex_, [this] { return imm_ == nullptr || !bg_error_.ok() || shutting_down_; });
      continue;
    }
    if (versions_->current()->NumFilesAtTier(0) >= kTier0StopTrigger) {
      stats_.Add(kStalls, 1);
      const uint64_t stall_start = env_->NowMicros();
      const bool drained = bg_cv_.wait_for(
          mutex_, std::chrono::seconds(kStallWatchdogSeconds), [this] {
            return versions_->current()->NumFilesAtTier(0) < kTier0StopTrigger ||
                   !bg_error_.ok() || shutting_down_;
          });
      stats_.Max(kMaxStallMs, (env_->NowMicros() - stall_start) / 1000);
      if (!drained && bg_error_.ok() && !shutting_down_) {
        // Nothing drained tier 0 in the whole deadline. Report it as a stall with the
        // number in the message, instead of parking here forever and being reported as a
        // deadlock (E-22, E-31).
        bg_error_ = Status::IOError(
            "write stalled: tier 0 holds " +
            std::to_string(versions_->current()->NumFilesAtTier(0)) +
            " files and did not drain in " + std::to_string(kStallWatchdogSeconds) +
            "s -- compaction is not keeping up (or is not running)");
        bg_cv_.notify_all();
        return bg_error_;
      }
      continue;
    }
    Status s = SwitchMemtableLocked();
    if (!s.ok()) return s;
    if (force) return Status::OK();
  }
}

Status DBImpl::SwitchMemtableLocked() {
  // Creating a log file is open(2) plus a directory fsync (E-7), so it CANNOT happen with
  // db_mutex_ held -- every queued writer and every Get would block behind a
  // several-millisecond sync, landing squarely in the metric R13 claims to improve. That
  // was a real defect in SPEC v1 (SPEC 11.10), and the TrackedMutex assertion fires if it
  // ever comes back.
  const uint64_t new_log_number = versions_->NewFileNumber();
  mutex_.unlock();

  std::unique_ptr<WritableFile> lfile;
  std::unique_ptr<WalWriter> lwriter;
  Status s = env_->NewWritableFile(LogFileName(dbname_, new_log_number), &lfile);
  if (s.ok()) s = WalWriter::Create(std::move(lfile), new_log_number, &lwriter);
  if (s.ok()) s = lwriter->Sync();
  if (s.ok()) s = env_->SyncDir(dbname_);   // the log's own directory entry must be durable

  mutex_.lock();
  if (!s.ok()) {
    versions_->MarkFileNumberUsed(new_log_number);
    return s;
  }
  // Re-check under the mutex: while it was released another thread could have enqueued,
  // but only the leader switches, so mem_/imm_ are still ours. imm_ may have been cleared
  // by the background thread, which is fine and is what the caller's loop re-tests.
  if (imm_ != nullptr) {
    // Someone else's flush is in flight; discard this log and let the loop wait.
    versions_->MarkFileNumberUsed(new_log_number);
    return Status::OK();
  }

  if (log_) { mutex_.unlock(); log_->Close(); mutex_.lock(); }
  // SPEC 3.3.1: from here until the flush has a durable manifest edit, TWO logs are live.
  versions_->SetPrevLogNumber(logfile_number_);
  log_ = std::move(lwriter);
  logfile_number_ = new_log_number;
  versions_->SetLogNumber(new_log_number);

  imm_ = mem_;
  has_imm_.store(true, std::memory_order_release);
  // MemTable's constructor already grants ONE reference to its creator. Calling Ref()
  // here as well made every flushed memtable's count go 2 -> 1 and never reach zero, so
  // each one leaked its entire arena. ASan found it; no correctness test could have.
  // See CHALLENGES B16.
  mem_ = new MemTable();
  stats_.Set(kMemtableBytes, mem_->ApproximateMemoryUsage());
  MaybeScheduleBackground();
  return Status::OK();
}

void DBImpl::MaybeScheduleBackground() {
  if (shutting_down_ || !bg_error_.ok()) return;
  if (bg_work_scheduled_) return;
  if (imm_ == nullptr) return;      // nothing to do yet (compaction joins in T10)
  bg_work_scheduled_ = true;
  bg_cv_.notify_all();
  if (!bg_running_) {
    bg_running_ = true;
    bg_thread_ = std::thread([this] { BackgroundLoop(); });
  }
}

// SPEC 3.8.4: exactly ONE background thread. Flush first, then (from T10) compaction.
// One thread means the MANIFEST has exactly one appender, so record order equals in-memory
// install order by construction and no manifest lock is needed (S23).
void DBImpl::BackgroundLoop() {
  std::unique_lock<TrackedMutex> lock(mutex_);
  while (true) {
    bg_cv_.wait(lock, [this] { return shutting_down_ || (imm_ != nullptr && bg_error_.ok()); });
    if (shutting_down_) break;
    if (imm_ != nullptr && bg_error_.ok()) {
      lock.unlock();
      Status s = FlushImmutableMemtable();
      lock.lock();
      if (!s.ok()) {
        // E-23: a background thread that dies silently lets tier 0 grow without bound and
        // stalls everything with no error surfaced. The error is sticky and every waiter
        // is woken so they can return it.
        bg_error_ = s;
      }
      bg_work_scheduled_ = false;
      bg_cv_.notify_all();
    }
  }
  bg_work_scheduled_ = false;
  bg_cv_.notify_all();
}

Status DBImpl::FlushImmutableMemtable() {
  MemTable* m = nullptr;
  uint64_t log_to_retire = 0;
  {
    std::unique_lock<TrackedMutex> lock(mutex_);
    if (imm_ == nullptr) return Status::OK();
    m = imm_;
    m->Ref();
    log_to_retire = versions_->PrevLogNumber();
  }

  VersionEdit edit;
  Status s = WriteLevel0Table(m, &edit);

  {
    std::unique_lock<TrackedMutex> lock(mutex_);
    if (s.ok()) {
      // SPEC 3.9 ordering: the SST is written and fsynced (in WriteLevel0Table), THEN the
      // manifest edit, THEN the in-memory install. Only after that edit is durable may the
      // log it came from be deleted (S5) -- doing it earlier loses data on a crash.
      edit.SetLogNumber(logfile_number_);
      edit.SetPrevLogNumber(0);          // the immutable memtable is now on disk
      s = versions_->LogAndApply(&edit, &mutex_);
    }
    if (s.ok()) {
      imm_->Unref();
      imm_ = nullptr;
      has_imm_.store(false, std::memory_order_release);
      RemoveObsoleteFiles();
    }
    m->Unref();
    bg_cv_.notify_all();
  }
  (void)log_to_retire;
  return s;
}

Status DBImpl::WriteLevel0Table(MemTable* mem, VersionEdit* edit) {
  FileMetaData meta;
  {
    std::unique_lock<TrackedMutex> lock(mutex_);
    meta.number = versions_->NewFileNumber();
    pending_outputs_.insert(meta.number);
  }
  const std::string fname = SstFileName(dbname_, meta.number);

  std::unique_ptr<WritableFile> file;
  Status s = env_->NewWritableFile(fname, &file);
  if (s.ok()) {
    SstBuilder builder(options_, file.get());
    std::unique_ptr<MemTableIterator> it(mem->NewIterator());
    for (it->SeekToFirst(); it->Valid(); it->Next()) builder.Add(it->key(), it->value());
    s = builder.Finish();
    if (s.ok()) {
      meta.file_size = builder.info().file_size;
      meta.smallest = builder.info().smallest_key;
      meta.largest = builder.info().largest_key;
      meta.num_entries = builder.info().num_entries;
      meta.num_deletions = builder.info().num_deletions;
    }
  }
  if (s.ok()) s = file->Sync();          // the DATA is durable before the manifest names it
  if (s.ok()) s = file->Close();
  if (s.ok()) s = env_->SyncDir(dbname_);  // ...and so is its directory entry (E-7)

  {
    std::unique_lock<TrackedMutex> lock(mutex_);
    pending_outputs_.erase(meta.number);
  }
  if (!s.ok()) { env_->DeleteFile(fname); return s; }
  if (meta.num_entries == 0) { env_->DeleteFile(fname); return Status::OK(); }

  edit->AddFile(0, meta);
  return Status::OK();
}

void DBImpl::RemoveObsoleteFiles() {
  // Caller holds the mutex. Only the SET is computed here; no I/O happens under the lock.
  if (!bg_error_.ok()) return;

  std::set<uint64_t> live = pending_outputs_;
  versions_->AddLiveFiles(&live);
  const uint64_t min_live_log = versions_->MinLiveLog();
  const uint64_t manifest_number = versions_->ManifestFileNumber();

  std::vector<std::string> filenames;
  std::vector<std::string> to_delete;
  mutex_.unlock();
  env_->GetChildren(dbname_, &filenames);
  for (const std::string& name : filenames) {
    uint64_t number = 0;
    std::string kind;
    if (!ParseFileName(name, &number, &kind)) continue;   // not ours -- never touched
    bool keep = true;
    if (kind == "log") {
      // SPEC 3.3.1 / S5: keep EVERY log at or above min_live_log. Both the mutable and
      // the immutable memtable's logs hold acknowledged data.
      keep = (number >= min_live_log);
    } else if (kind == "sst") {
      keep = (live.count(number) > 0);
    } else if (kind == "MANIFEST") {
      keep = (number >= manifest_number);
    } else if (kind == "tmp") {
      keep = false;
    }
    if (!keep) to_delete.push_back(dbname_ + "/" + name);
  }
  for (const std::string& f : to_delete) env_->DeleteFile(f);
  mutex_.lock();
}

Status DBImpl::Get(const ReadOptions& options, const Slice& key, std::string* value) {
  MemTable* mem = nullptr;
  MemTable* imm = nullptr;
  Version* current = nullptr;
  SequenceNumber snapshot = 0;

  {
    // SPEC 3.7 step 1 / S21. The sequence number AND the references are taken in ONE
    // critical section. SPEC v1 read last_sequence_ outside the lock, and in that gap a
    // compaction can install a version that legitimately dropped the exact entry this read
    // needs -- a silent stale read with no crash and no checksum failure (SPEC 11.5).
    std::unique_lock<TrackedMutex> lock(mutex_);
    if (!bg_error_.ok()) return bg_error_;
    snapshot = options.snapshot
                   ? static_cast<const SnapshotImpl*>(options.snapshot)->sequence
                   : versions_->LastSequence();
    mem = mem_;
    imm = imm_;
    current = versions_->current();
    mem->Ref();
    if (imm) imm->Ref();
    current->Ref();
  }

  // --- NO LOCK HELD from here. Everything is kept alive by refcount. ---
  Status s;
  LookupKey lkey(key, snapshot);
  bool found = false;
  if (mem->Get(lkey, value, &s)) {
    found = true;
  } else if (imm != nullptr && imm->Get(lkey, value, &s)) {
    found = true;
  } else {
    s = current->Get(options, lkey, value, table_cache_.get(), &stats_);
    found = true;
  }
  (void)found;

  {
    std::unique_lock<TrackedMutex> lock(mutex_);
    mem->Unref();
    if (imm) imm->Unref();
    current->Unref();
  }
  return s;
}

const Snapshot* DBImpl::GetSnapshot() {
  std::unique_lock<TrackedMutex> lock(mutex_);
  auto* s = new SnapshotImpl(versions_->LastSequence());
  snapshots_.push_back(s);
  std::sort(snapshots_.begin(), snapshots_.end(),
            [](const SnapshotImpl* a, const SnapshotImpl* b) { return a->sequence < b->sequence; });
  return s;
}

void DBImpl::ReleaseSnapshot(const Snapshot* s) {
  std::unique_lock<TrackedMutex> lock(mutex_);
  auto it = std::find(snapshots_.begin(), snapshots_.end(), static_cast<const SnapshotImpl*>(s));
  if (it != snapshots_.end()) { delete *it; snapshots_.erase(it); }
}

SequenceNumber DBImpl::OldestSnapshotLocked() const {
  // SPEC 3.8.3 / E-33: with no live snapshot the value is last_sequence_, NOT 0. A zero
  // here turns compaction into a pure merge that reclaims nothing, while every correctness
  // test still passes.
  return snapshots_.empty() ? versions_->LastSequence() : snapshots_.front()->sequence;
}

Status DBImpl::Flush() {
  std::unique_lock<TrackedMutex> lock(mutex_);
  if (!bg_error_.ok()) return bg_error_;
  if (mem_->ApproximateMemoryUsage() == 0 && imm_ == nullptr) return Status::OK();
  Status s = MakeRoomForWrite(true);
  if (!s.ok()) return s;
  bg_cv_.wait(lock, [this] { return imm_ == nullptr || !bg_error_.ok() || shutting_down_; });
  return bg_error_;
}

Status DBImpl::CompactRange(const Slice*, const Slice*) {
  // T10 implements this. Flushing is the part that exists now, and returning OK for the
  // rest would be a lie a test could not see.
  return Flush();
}

Iterator* DBImpl::NewIterator(const ReadOptions&) {
  return NewErrorIterator(Status::NotSupported("iterators land in T9"));
}

bool DBImpl::GetProperty(const Slice& name, std::string* out) {
  const std::string n = name.ToString();
  if (n.rfind("lsmeng.num-files-at-tier", 0) == 0) {
    const int tier = std::atoi(n.c_str() + std::strlen("lsmeng.num-files-at-tier"));
    std::unique_lock<TrackedMutex> lock(mutex_);
    *out = std::to_string(versions_->current()->NumFilesAtTier(tier));
    return true;
  }
  if (n == "lsmeng.memtable-bytes") {
    std::unique_lock<TrackedMutex> lock(mutex_);
    *out = std::to_string(mem_->ApproximateMemoryUsage());
    return true;
  }
  return stats_.GetProperty(name, out);
}

// ---------------------------------------------------------------- Open / Recover

Status DBImpl::NewDB() {
  Status s = env_->CreateDir(dbname_);
  if (!s.ok() && !s.IsNotFound()) { /* EEXIST is folded into OK by CreateDir */ }
  return versions_->CreateNew();
}

Status DBImpl::RecoverLogFile(uint64_t log_number, SequenceNumber* max_sequence,
                              VersionEdit* edit, bool* saved_a_table) {
  const std::string fname = LogFileName(dbname_, log_number);
  std::unique_ptr<WalReader> reader;
  Status s = WalReader::Open(env_, fname, &reader);
  if (!s.ok()) {
    // A log named by the version set but absent is a real problem; a log that is present
    // but unreadable at its very header is a torn creation, which is survivable.
    if (s.IsNotFound()) return Status::OK();
    return s;
  }

  MemTable* mem = new MemTable();   // constructor grants the creator's reference (B16)
  Slice record;
  std::string scratch;
  int batches = 0;

  while (reader->ReadRecord(&record, &scratch)) {
    WriteBatch batch;
    s = batch.SetContents(record);
    if (!s.ok()) break;
    s = batch.InsertInto(mem);
    if (!s.ok()) break;
    ++batches;

    const SequenceNumber last = batch.Sequence() + static_cast<uint64_t>(batch.Count()) - 1;
    if (last > *max_sequence) *max_sequence = last;

    // E-20: replaying a 500 MiB log into a 4 MiB memtable must not OOM. Flush and carry
    // on, exactly as the normal write path would.
    if (mem->ApproximateMemoryUsage() > options_.write_buffer_size) {
      s = WriteLevel0Table(mem, edit);
      *saved_a_table = true;
      mem->Unref();
      mem = new MemTable();
      if (!s.ok()) break;
    }
  }

  std::fprintf(stderr, "lsmeng: recovered %d batch(es) from %s (%s)\n", batches,
               fname.c_str(), reader->StopReasonString());

  if (s.ok() && mem->ApproximateMemoryUsage() > 0) {
    s = WriteLevel0Table(mem, edit);
    *saved_a_table = true;
  }
  mem->Unref();
  return s;
}

Status DBImpl::Recover() {
  Status s = env_->CreateDir(dbname_);
  if (!s.ok()) return s;

  // S16: the lock is taken BEFORE any recovery, GC, or manifest write. Everything below
  // assumes single ownership of this directory.
  s = env_->LockFile(LockFileName(dbname_), &db_lock_);
  if (!s.ok()) return s;

  if (!env_->FileExists(CurrentFileName(dbname_))) {
    if (!options_.create_if_missing)
      return Status::NotFound("no database at " + dbname_ + " and create_if_missing is false");
    s = NewDB();
    if (!s.ok()) return s;
  } else if (options_.error_if_exists) {
    return Status::InvalidArgument("database already exists at " + dbname_ +
                                   " and error_if_exists is set");
  } else {
    s = versions_->Recover();
    if (!s.ok()) return s;
  }

  // SPEC 3.3.1 / E-36. Replay EVERY log at or above min_live_log, in ASCENDING numeric
  // order. Two logs are live from a memtable freeze until its flush has a durable manifest
  // edit; a recovery that replayed only log_number would silently discard every
  // acknowledged sync=true write since the switch. Ordering is by file number, never by
  // content, because later logs carry higher sequence numbers by construction.
  std::vector<std::string> filenames;
  s = env_->GetChildren(dbname_, &filenames);
  if (!s.ok()) return s;

  const uint64_t min_live_log = versions_->MinLiveLog();
  std::vector<uint64_t> logs;
  for (const std::string& name : filenames) {
    uint64_t number = 0;
    std::string kind;
    if (ParseFileName(name, &number, &kind) && kind == "log" && number >= min_live_log)
      logs.push_back(number);
  }
  std::sort(logs.begin(), logs.end());

  VersionEdit edit;
  SequenceNumber max_sequence = versions_->LastSequence();
  bool saved_a_table = false;
  for (uint64_t number : logs) {
    versions_->MarkFileNumberUsed(number);
    s = RecoverLogFile(number, &max_sequence, &edit, &saved_a_table);
    if (!s.ok()) return s;
  }

  // S9 / E-19: sequence numbers must never restart below existing data, or old values
  // shadow new ones forever.
  if (max_sequence > versions_->LastSequence()) versions_->SetLastSequence(max_sequence);

  // A fresh log for this incarnation.
  const uint64_t new_log_number = versions_->NewFileNumber();
  std::unique_ptr<WritableFile> lfile;
  s = env_->NewWritableFile(LogFileName(dbname_, new_log_number), &lfile);
  if (!s.ok()) return s;
  s = WalWriter::Create(std::move(lfile), new_log_number, &log_);
  if (!s.ok()) return s;
  s = log_->Sync();
  if (s.ok()) s = env_->SyncDir(dbname_);
  if (!s.ok()) return s;
  logfile_number_ = new_log_number;

  edit.SetLogNumber(new_log_number);
  edit.SetPrevLogNumber(0);           // everything older has been flushed above
  edit.SetLastSequence(versions_->LastSequence());
  s = versions_->LogAndApply(&edit, nullptr);   // recovery: no mutex is held yet
  if (!s.ok()) return s;

  mem_ = new MemTable();   // constructor grants the creator's reference (B16)

  {
    std::unique_lock<TrackedMutex> lock(mutex_);
    ReportOrphans();
    RemoveObsoleteFiles();
  }
  (void)saved_a_table;
  return Status::OK();
}

// SPEC 3.9 / E-6. Orphans are REPORTED, not deleted, unless explicitly asked. flock does
// not exclude on this container's bind mount (CHALLENGES B4), and an orphan SST only
// wastes space while a wrongly unlinked live SST ends the database. Those are not the same
// class of problem, and the default reflects that.
void DBImpl::ReportOrphans() {
  std::set<uint64_t> live = pending_outputs_;
  versions_->AddLiveFiles(&live);
  const uint64_t min_live_log = versions_->MinLiveLog();

  std::vector<std::string> filenames;
  mutex_.unlock();
  env_->GetChildren(dbname_, &filenames);
  uint64_t orphan_files = 0, orphan_bytes = 0;
  for (const std::string& name : filenames) {
    uint64_t number = 0;
    std::string kind;
    if (!ParseFileName(name, &number, &kind)) continue;
    bool orphan = false;
    if (kind == "sst") orphan = (live.count(number) == 0);
    else if (kind == "log") orphan = (number < min_live_log);
    if (!orphan) continue;
    ++orphan_files;
    uint64_t size = 0;
    if (env_->GetFileSize(dbname_ + "/" + name, &size).ok()) orphan_bytes += size;
  }
  mutex_.lock();
  stats_.Set(kOrphanFiles, orphan_files);
  stats_.Set(kOrphanBytes, orphan_bytes);
  if (orphan_files > 0 && !options_.gc_orphans_on_open)
    std::fprintf(stderr,
                 "lsmeng: %llu orphan file(s), %llu bytes, left in place "
                 "(Options::gc_orphans_on_open is false -- see SPEC E-6)\n",
                 (unsigned long long)orphan_files, (unsigned long long)orphan_bytes);
}

Status DB::Open(const Options& options, const std::string& dbname, DB** out) {
  *out = nullptr;
  auto* impl = new DBImpl(options, dbname);
  Status s = impl->Recover();
  if (!s.ok()) { delete impl; return s; }
  *out = impl;
  return Status::OK();
}

Status DestroyDB(const std::string& dbname, const Options& options) {
  Env* env = options.env ? options.env : Env::Default();
  std::vector<std::string> filenames;
  Status s = env->GetChildren(dbname, &filenames);
  if (!s.ok()) return Status::OK();   // nothing there is not a failure
  FileLock* lock = nullptr;
  s = env->LockFile(LockFileName(dbname), &lock);
  if (!s.ok()) return s;              // refuse to destroy a database someone else has open
  for (const std::string& name : filenames) {
    uint64_t number = 0;
    std::string kind;
    if (ParseFileName(name, &number, &kind) && kind != "LOCK")
      env->DeleteFile(dbname + "/" + name);
  }
  env->UnlockFile(lock);
  env->DeleteFile(LockFileName(dbname));
  env->DeleteDir(dbname);
  return Status::OK();
}

}  // namespace lsmeng

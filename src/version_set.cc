#include "lsmeng/version_set.h"

#include <algorithm>
#include <cstdio>

#include "lsmeng/coding.h"

namespace lsmeng {

const char* VersionSet::kComparatorName = "lsmeng.InternalKeyComparator.v1";

// ---------------------------------------------------------------- Version

Version::~Version() {
  for (auto& tier : files_)
    for (FileMetaData* f : tier) {
      if (--f->refs == 0) delete f;   // a file's metadata dies with the last version
    }
}

void Version::Unref() {
  if (--refs_ == 0) {
    prev_->next_ = next_;
    next_->prev_ = prev_;
    delete this;
  }
}

uint64_t Version::TotalBytes() const {
  uint64_t n = 0;
  for (const auto& tier : files_)
    for (const FileMetaData* f : tier) n += f->file_size;
  return n;
}

void Version::ForEachOverlapping(const Slice& user_key,
                                 const std::function<bool(int, FileMetaData*)>& fn) const {
  for (int tier = 0; tier < NumTiers(); ++tier) {
    // Within a tier, NEWEST FIRST. Files in a tier produced by tiered compaction OVERLAP
    // (unlike leveled L1+, where they are disjoint and a binary search suffices), so the
    // tier must be walked file by file in recency order and the first hit wins.
    std::vector<FileMetaData*> ordered = files_[tier];
    std::sort(ordered.begin(), ordered.end(),
              [](const FileMetaData* a, const FileMetaData* b) { return a->number > b->number; });
    for (FileMetaData* f : ordered) {
      // The cheapest filter of all, and it runs before the Bloom filter because it costs
      // no I/O at all -- the key range lives in the manifest, not the file (SPEC 3.5).
      if (user_key.compare(f->smallest_user_key()) < 0) continue;
      if (user_key.compare(f->largest_user_key()) > 0) continue;
      if (!fn(tier, f)) return;
    }
  }
}

Status Version::Get(const ReadOptions& options, const LookupKey& key, std::string* value,
                    TableCache* table_cache, Stats* stats) {
  Status result = Status::NotFound(Slice());
  ForEachOverlapping(key.user_key(), [&](int, FileMetaData* f) {
    std::string v;
    bool tombstone = false;
    Status s = table_cache->Lookup(options, f->number, f->file_size, key.internal_key(), &v,
                                   &tombstone);
    if (s.ok()) { *value = std::move(v); result = Status::OK(); return false; }
    if (tombstone) {
      // A HIT that happens to be a delete. Stop here: continuing to an older file would
      // resurrect the value this tombstone hides (E-2).
      result = Status::NotFound(Slice());
      return false;
    }
    if (!s.IsNotFound()) { result = s; return false; }   // a real I/O or corruption error
    return true;                                          // genuinely absent; keep looking
  });
  return result;
}

void Version::AddIterators(const ReadOptions& options, TableCache* table_cache,
                           std::vector<Iterator*>* out) const {
  for (int tier = 0; tier < NumTiers(); ++tier)
    for (FileMetaData* f : files_[tier])
      out->push_back(table_cache->NewIterator(options, f->number, f->file_size));
}

std::string Version::DebugString() const {
  std::string r;
  for (int tier = 0; tier < NumTiers(); ++tier) {
    r += "tier " + std::to_string(tier) + ":";
    for (const FileMetaData* f : files_[tier])
      r += " " + std::to_string(f->number) + "(" + std::to_string(f->file_size) + "B)";
    r += "\n";
  }
  return r;
}

// ---------------------------------------------------------------- VersionBuilder

// Applies a sequence of edits to a base version and produces the next one. Kept separate
// from Version so that a Version is genuinely immutable once published -- the whole
// concurrency argument depends on that.
class VersionBuilder {
 public:
  VersionBuilder(VersionSet* vset, Version* base) : vset_(vset), base_(base) {
    base_->Ref();
    tiers_.resize(std::max<size_t>(base_->files_.size(), 1));
    for (size_t t = 0; t < base_->files_.size(); ++t)
      for (FileMetaData* f : base_->files_[t]) tiers_[t].push_back(f);
  }
  ~VersionBuilder() { base_->Unref(); }

  void Apply(const VersionEdit& edit) {
    for (const auto& d : edit.deleted_files_) {
      const int tier = d.first;
      if (tier >= static_cast<int>(tiers_.size())) continue;
      auto& v = tiers_[tier];
      v.erase(std::remove_if(v.begin(), v.end(),
                             [&](FileMetaData* f) { return f->number == d.second; }),
              v.end());
    }
    for (const auto& nf : edit.new_files_) {
      const int tier = nf.first;
      if (tier >= static_cast<int>(tiers_.size())) tiers_.resize(tier + 1);
      auto* f = new FileMetaData(nf.second);
      f->refs = 0;
      tiers_[tier].push_back(f);
      owned_.push_back(f);
    }
  }

  void SaveTo(Version* v) {
    v->files_ = tiers_;
    for (auto& tier : v->files_) {
      // Ascending file number within a tier: allocation order, hence write order. The read
      // path reverses it for recency; keeping the stored order canonical makes the
      // manifest and DebugString deterministic.
      std::sort(tier.begin(), tier.end(),
                [](const FileMetaData* a, const FileMetaData* b) { return a->number < b->number; });
      for (FileMetaData* f : tier) ++f->refs;
    }
    owned_.clear();   // ownership has passed to the version's refcounts
  }

 private:
  VersionSet* vset_;
  Version* base_;
  std::vector<std::vector<FileMetaData*>> tiers_;
  std::vector<FileMetaData*> owned_;
};

// ---------------------------------------------------------------- VersionSet

VersionSet::VersionSet(const std::string& dbname, const Options& options,
                       TableCache* table_cache, Stats* stats)
    : env_(options.env ? options.env : Env::Default()),
      dbname_(dbname),
      options_(options),
      table_cache_(table_cache),
      stats_(stats) {
  dummy_versions_.next_ = &dummy_versions_;
  dummy_versions_.prev_ = &dummy_versions_;
  AppendVersion(new Version(this));
}

VersionSet::~VersionSet() {
  if (current_) current_->Unref();
}

void VersionSet::AppendVersion(Version* v) {
  v->Ref();
  if (current_) current_->Unref();
  current_ = v;
  v->prev_ = dummy_versions_.prev_;
  v->next_ = &dummy_versions_;
  v->prev_->next_ = v;
  v->next_->prev_ = v;
}

void VersionSet::AddLiveFiles(std::set<uint64_t>* live) const {
  for (Version* v = dummy_versions_.next_; v != &dummy_versions_; v = v->next_)
    for (const auto& tier : v->files_)
      for (const FileMetaData* f : tier) live->insert(f->number);
}

Status VersionSet::WriteSnapshot(WalWriter* writer) {
  VersionEdit edit;
  edit.SetComparatorName(Slice(kComparatorName));
  edit.SetLogNumber(log_number_);
  edit.SetPrevLogNumber(prev_log_number_);
  edit.SetNextFile(next_file_number_);
  edit.SetLastSequence(last_sequence_);
  for (int tier = 0; tier < current_->NumTiers(); ++tier)
    for (FileMetaData* f : current_->files_[tier]) edit.AddFile(tier, *f);
  std::string record;
  edit.EncodeTo(&record);
  return writer->AddRecord(Slice(record));
}

Status VersionSet::CreateNew() {
  manifest_file_number_ = NewFileNumber();
  const std::string manifest = ManifestFileName(dbname_, manifest_file_number_);

  std::unique_ptr<WritableFile> file;
  Status s = env_->NewWritableFile(manifest, &file);
  if (!s.ok()) return s;
  std::unique_ptr<WalWriter> writer;
  s = WalWriter::Create(std::move(file), manifest_file_number_, &writer);
  if (!s.ok()) return s;
  s = WriteSnapshot(writer.get());
  if (s.ok()) s = writer->Sync();
  if (!s.ok()) return s;

  // CURRENT is rotated atomically: write .tmp, fsync it, rename, fsync the DIRECTORY.
  // The directory fsync is not optional -- without it the rename can be lost on a crash,
  // leaving CURRENT naming a manifest that is gone: an unopenable database (E-7).
  const std::string tmp = TempFileName(dbname_, manifest_file_number_);
  s = WriteStringToFileSync(env_, Slice(ManifestFileName("", manifest_file_number_).substr(1) + "\n"), tmp);
  if (!s.ok()) return s;
  s = env_->RenameFile(tmp, CurrentFileName(dbname_));
  if (!s.ok()) { env_->DeleteFile(tmp); return s; }
  s = env_->SyncDir(dbname_);
  if (!s.ok()) return s;

  manifest_log_ = std::move(writer);
  return Status::OK();
}

Status VersionSet::LogAndApply(VersionEdit* edit, TrackedMutex* mu) {
  // Keep the counter ahead of anything this edit records, so the manifest is always
  // self-consistent even if a caller built a FileMetaData without allocating through
  // NewFileNumber(). Recovery double-checks the same property (B12); doing it on both
  // sides means a stale manifest cannot be produced in the first place.
  for (const auto& nf : edit->new_files_) MarkFileNumberUsed(nf.second.number);
  if (!edit->has_log_number_) edit->SetLogNumber(log_number_);
  if (!edit->has_prev_log_number_) edit->SetPrevLogNumber(prev_log_number_);
  edit->SetNextFile(next_file_number_);
  edit->SetLastSequence(last_sequence_);

  // Build the new version FIRST, so that a malformed edit fails before anything is
  // written to disk.
  auto* v = new Version(this);
  {
    VersionBuilder builder(this, current_);
    builder.Apply(*edit);
    builder.SaveTo(v);
  }

  // SPEC 3.9 ordering: append -> fsync -> install. Only one thread is ever inside this
  // sequence (SPEC 3.8.4's single background thread + the foreground holding db_mutex_
  // around the call), which is why no manifest lock is needed and why on-disk record order
  // always equals in-memory install order (S23).
  std::string record;
  edit->EncodeTo(&record);

  // S23. If a second thread ever reaches here concurrently, the two appends interleave
  // mid-record, the CRC fails at the splice point, and the torn-tail rule then silently
  // discards BOTH edits and every later one (E-38). Assert rather than hope.
  const bool was_appending = appending_.exchange(true, std::memory_order_acq_rel);
  LSMENG_ASSERT(!was_appending,
                "two threads appending to the MANIFEST -- SPEC 3.8.4 says there is one "
                "background thread (S23, E-38)");

  // S13: db_mutex_ is released across the append and the fsync. A manifest fsync is
  // milliseconds; holding the lock across it would block every queued writer and every
  // Get, which is precisely the p99.9 damage R13 exists to remove.
  if (mu) mu->unlock();
  Status s = manifest_log_->AddRecord(Slice(record));
  if (s.ok()) s = manifest_log_->Sync();
  if (mu) mu->lock();
  appending_.store(false, std::memory_order_release);

  if (!s.ok()) { delete v; return s; }

  AppendVersion(v);
  log_number_ = edit->log_number_;
  prev_log_number_ = edit->prev_log_number_;
  return Status::OK();
}

Status VersionSet::Recover() {
  std::string current;
  Status s = ReadFileToString(env_, CurrentFileName(dbname_), &current);
  if (!s.ok()) return s;   // NotFound here means "no database", which Open distinguishes
  if (current.empty() || current.back() != '\n')
    return Status::Corruption("CURRENT does not end with a newline: " + CurrentFileName(dbname_));
  current.pop_back();

  const std::string manifest = dbname_ + "/" + current;
  // The number of the manifest CURRENT actually names. This is NOT next_file_number:
  // conflating them made RemoveObsoleteFiles delete the LIVE manifest as soon as the file
  // counter had moved past it, leaving CURRENT pointing at nothing. See CHALLENGES B14.
  uint64_t current_manifest_number = 0;
  {
    std::string kind;
    if (!ParseFileName(current, &current_manifest_number, &kind) || kind != "MANIFEST")
      return Status::Corruption("CURRENT does not name a manifest: '" + current + "'");
  }
  std::unique_ptr<WalReader> reader;
  s = WalReader::Open(env_, manifest, &reader);
  if (!s.ok()) {
    // E-7's failure, surfaced with the filename: CURRENT names a manifest that is missing
    // or unreadable. "Corruption" with no filename is how debugging sessions get long.
    return Status::Corruption("CURRENT names manifest " + manifest + " which cannot be read: " +
                              s.ToString());
  }

  Version* v = new Version(this);
  VersionBuilder builder(this, current_);
  bool have_log = false, have_prev_log = false, have_next_file = false, have_last_seq = false;
  uint64_t log_number = 0, prev_log_number = 0, next_file = 0;
  SequenceNumber last_seq = 0;
  bool saw_comparator = false;
  int records = 0;
  uint64_t highest_seen = 0;

  Slice record;
  std::string scratch;
  while (reader->ReadRecord(&record, &scratch)) {
    VersionEdit edit;
    s = edit.DecodeFrom(record);
    if (!s.ok()) { delete v; return s; }
    ++records;

    if (edit.has_comparator_) {
      if (edit.comparator_ != kComparatorName) {
        delete v;
        return Status::InvalidArgument("manifest was written with comparator '" +
                                       edit.comparator_ + "', this build uses '" +
                                       kComparatorName + "'");
      }
      saw_comparator = true;
    }
    builder.Apply(edit);
    if (edit.has_log_number_) { log_number = edit.log_number_; have_log = true; }
    if (edit.has_prev_log_number_) { prev_log_number = edit.prev_log_number_; have_prev_log = true; }
    if (edit.has_next_file_number_) { next_file = edit.next_file_number_; have_next_file = true; }
    if (edit.has_last_sequence_) { last_seq = edit.last_sequence_; have_last_seq = true; }

    // Trust the FILE SET, not just the counter. The manifest carries next_file_number as
    // a field, but nothing on disk guarantees it is consistent with the file numbers
    // actually recorded -- a crash, an older build, or a bug could leave it behind. If it
    // is, recovery hands out a number that is ALREADY IN USE, and the next flush creates
    // 000005.sst directly on top of a live SST the manifest still references. Silent data
    // loss, with every checksum passing. So the high-water mark is recomputed from
    // everything observed. See CHALLENGES B12.
    for (const auto& nf : edit.new_files_) highest_seen = std::max(highest_seen, nf.second.number);
    for (const auto& d : edit.deleted_files_) highest_seen = std::max(highest_seen, d.second);
    if (edit.has_log_number_) highest_seen = std::max(highest_seen, edit.log_number_);
    if (edit.has_prev_log_number_) highest_seen = std::max(highest_seen, edit.prev_log_number_);
  }

  // A torn tail is NORMAL -- a crash during LogAndApply leaves exactly that, and the edit
  // simply did not happen (its SST becomes an orphan, E-6). What is NOT normal is a
  // manifest with no usable records at all.
  if (records == 0) {
    delete v;
    return Status::Corruption("manifest " + manifest + " contains no readable records (" +
                              reader->StopReasonString() + ")");
  }
  if (!saw_comparator) {
    delete v;
    return Status::Corruption("manifest " + manifest + " has no comparator record");
  }
  if (!have_next_file) {
    delete v;
    return Status::Corruption("manifest " + manifest + " has no next-file-number record");
  }

  builder.SaveTo(v);
  AppendVersion(v);

  manifest_file_number_ = current_manifest_number;
  next_file_number_ = std::max(next_file + 1, highest_seen + 1);
  MarkFileNumberUsed(current_manifest_number);
  last_sequence_ = have_last_seq ? last_seq : 0;
  log_number_ = have_log ? log_number : 0;
  prev_log_number_ = have_prev_log ? prev_log_number : 0;

  // Continue appending to the SAME manifest rather than starting a new one, so a database
  // opened and closed repeatedly does not accumulate manifests.
  std::unique_ptr<WritableFile> file;
  s = env_->NewAppendableFile(manifest, &file);
  if (!s.ok()) return s;
  return WalWriter::Open(std::move(file), reader->log_number(), &manifest_log_);
}

}  // namespace lsmeng

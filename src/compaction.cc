// SPEC 3.8. Tiered compaction: the T oldest files of a tier merge into one file in the
// next tier.
//
// FOUR THINGS HERE WERE WRONG IN SPEC v1 AND ARE FIXED, each of which silently loses or
// resurrects data:
//   1. Input selection must be the T OLDEST files (3.8.1). A size-chosen subset breaks the
//      tier-ordering invariant the read path's correctness rests on (11.4, E-30).
//   2. The dedup predicate is about the PREVIOUSLY EMITTED version, not the current one
//      (11.2, E-2d). Testing the entry's own sequence drops exactly the version an old
//      snapshot needs.
//   3. `is_bottom_tier` is NOT sufficient to drop a tombstone (11.1, E-2e).
//   4. oldest_snapshot_seq with no live snapshot is last_sequence_, never 0 (E-33).
#include <algorithm>
#include <cstdio>
#include <memory>

#include "db_impl.h"
#include "lsmeng/sst.h"
#include "merging_iterator.h"

namespace lsmeng {
namespace {
// Inputs normally all come from input_tier, but the max_tiers fold (SPEC 3.8.2) also pulls
// in every file of the deepest tier. Recording which tier each input came from is what lets
// the VersionEdit delete it from the right place -- deleting from the wrong tier leaves a
// phantom entry that the read path would still search.
int TierOf(const Compaction& c, const FileMetaData* f) { return c.InputTierFor(f->number); }
}  // namespace

bool Compaction::NoOlderDataCanExist(const Slice& user_key) const {
  // Pure in-memory range arithmetic -- the key ranges live in the MANIFEST, not the files
  // (SPEC 3.5), so this costs no I/O at all. The file count here is bounded by
  // max_tiers * tier_trigger (28 by default), so a linear scan is cheaper than the cursor
  // the spec sketches; if that ever stops being true a profile will say so.
  for (const FileMetaData* f : others) {
    if (user_key.compare(f->smallest_user_key()) < 0) continue;
    if (user_key.compare(f->largest_user_key()) > 0) continue;
    return false;   // some file we are not rewriting could hold an older value
  }
  return true;
}

bool DBImpl::ShouldCompact() const {
  Version* v = versions_->current();
  for (int tier = 0; tier < v->NumTiers(); ++tier)
    if (v->NumFilesAtTier(tier) >= options_.tier_trigger) return true;
  return false;
}

bool DBImpl::PickCompaction(Compaction* c) {
  Version* v = versions_->current();

  // SHALLOWEST eligible tier, always. Tier-0 backlog is what stalls writers, so draining it
  // takes precedence over reducing read amplification deeper down. A compaction in progress
  // is never preempted, so a deep merge can head-of-line-block tier 0 -- a known cost of
  // the single-background-thread design, made observable rather than hidden (E-31).
  int tier = -1;
  for (int t = 0; t < v->NumTiers(); ++t) {
    if (v->NumFilesAtTier(t) >= options_.tier_trigger) { tier = t; break; }
  }
  if (tier < 0) return false;

  c->input_tier = tier;
  c->inputs.clear();
  c->others.clear();

  std::vector<FileMetaData*> tier_files = v->FilesAtTier(tier);
  // THE T OLDEST, by file number ascending. File numbers are allocated monotonically, so
  // this is write order, so this is recency order. SPEC 3.8.1's induction depends on the
  // inputs being an oldest-first PREFIX: everything moved down was written before
  // everything left behind, so no version in tier t+1 is newer than a version of the same
  // key in tier t.
  std::sort(tier_files.begin(), tier_files.end(),
            [](const FileMetaData* a, const FileMetaData* b) { return a->number < b->number; });
  const size_t take = std::min<size_t>(static_cast<size_t>(options_.tier_trigger), tier_files.size());
  c->inputs.assign(tier_files.begin(), tier_files.begin() + static_cast<long>(take));
  c->input_tier_of.clear();
  for (const FileMetaData* f : c->inputs) c->input_tier_of[f->number] = tier;

  c->output_tier = tier + 1;
  if (c->output_tier >= options_.max_tiers) {
    // SPEC 3.8.2 / E-32: bound tier count. Rather than creating tier max_tiers, merge the
    // WHOLE deepest tier together with the inputs into that same tier. Consuming every file
    // in the tier keeps 3.8.1 trivially -- nothing older is left behind to be shadowed.
    c->output_tier = options_.max_tiers - 1;
    if (c->output_tier != tier) {
      for (FileMetaData* f : v->FilesAtTier(c->output_tier)) {
        c->inputs.push_back(f);
        c->input_tier_of[f->number] = c->output_tier;
      }
    } else {
      c->inputs.assign(tier_files.begin(), tier_files.end());
      for (const FileMetaData* f : c->inputs) c->input_tier_of[f->number] = tier;
    }
  }

  // Everything in the output tier and deeper that we are NOT rewriting.
  std::set<uint64_t> input_numbers;
  for (const FileMetaData* f : c->inputs) input_numbers.insert(f->number);
  for (int t = c->output_tier; t < v->NumTiers(); ++t)
    for (FileMetaData* f : v->FilesAtTier(t))
      if (input_numbers.count(f->number) == 0) c->others.push_back(f);

  // SPEC 3.8.3 / E-33. Captured ONCE, under the mutex, at schedule time, and held constant
  // for the whole merge. With no live snapshot the value is last_sequence_, NOT 0 -- a zero
  // turns compaction into a pure merge that reclaims nothing while every correctness test
  // still passes. Re-reading mid-merge would be wrong in the other direction: a snapshot
  // registered after the capture gets a sequence above last_sequence_ at capture time, so
  // it cannot need anything already dropped.
  c->oldest_snapshot = OldestSnapshotLocked();
  LSMENG_ASSERT(c->oldest_snapshot > 0 || versions_->LastSequence() == 0,
                "oldest_snapshot of 0 would make compaction reclaim nothing (E-33)");
  return true;
}

Status DBImpl::DoCompaction(Compaction* c) {
  // Inputs are immutable files, so this whole function runs with NO LOCK HELD. Only the
  // final install takes db_mutex_, for pointer manipulation (SPEC 3.8.4).
  std::vector<Iterator*> children;
  children.reserve(c->inputs.size());
  for (const FileMetaData* f : c->inputs)
    children.push_back(table_cache_->NewIterator(ReadOptions(), f->number, f->file_size));
  std::unique_ptr<Iterator> input(
      NewMergingIterator(children.data(), static_cast<int>(children.size())));

  FileMetaData out;
  {
    std::unique_lock<TrackedMutex> lock(mutex_);
    out.number = versions_->NewFileNumber();
    pending_outputs_.insert(out.number);
  }
  const std::string fname = SstFileName(dbname_, out.number);

  std::unique_ptr<WritableFile> file;
  Status s = env_->NewWritableFile(fname, &file);
  if (!s.ok()) return s;

  SstBuilder builder(options_, file.get());
  uint64_t dropped = 0, kept = 0, input_bytes = 0;
  for (const FileMetaData* f : c->inputs) input_bytes += f->file_size;

  std::string last_user_key;
  bool have_last_user_key = false;
  SequenceNumber last_seq_for_key = kMaxSequenceNumber;

  for (input->SeekToFirst(); input->Valid(); input->Next()) {
    ParsedInternalKey p;
    if (!ParseInternalKey(input->key(), &p)) {
      s = Status::Corruption("malformed internal key during compaction");
      break;
    }
    // COPY: p.user_key points into a data block that Next() may free (the B18 lesson).
    const std::string user_key = p.user_key.ToString();

    if (!have_last_user_key || user_key != last_user_key) {
      last_user_key = user_key;
      have_last_user_key = true;
      last_seq_for_key = kMaxSequenceNumber;   // nothing newer has been seen for this key
    }

    // THE DEDUP RULE (SPEC 3.8.3 / 11.2). The predicate is about the PREVIOUSLY EMITTED
    // version, not this one: an entry is unobservable only if the version that SUPERSEDES
    // it is itself visible to the oldest live snapshot. Testing `p.sequence` instead --
    // which is what v1 specified -- drops exactly the version that snapshot needs, every
    // time, because the first version at or below the boundary always satisfies it.
    bool drop = (last_seq_for_key <= c->oldest_snapshot);

    if (p.type == kTypeDeletion && p.sequence <= c->oldest_snapshot &&
        c->NoOlderDataCanExist(Slice(user_key))) {
      // The tombstone has done its job: nothing we are leaving behind can hold an older
      // value for this key, and no live snapshot can still see under it.
      drop = true;
    }

    last_seq_for_key = p.sequence;   // BEFORE the next iteration

    if (drop) { ++dropped; continue; }
    builder.Add(input->key(), input->value());
    ++kept;
  }

  if (s.ok()) s = input->status();
  if (s.ok()) s = builder.Finish();
  if (s.ok()) {
    out.file_size = builder.info().file_size;
    out.smallest = builder.info().smallest_key;
    out.largest = builder.info().largest_key;
    out.num_entries = builder.info().num_entries;
    out.num_deletions = builder.info().num_deletions;
  }
  // SPEC 3.9: the DATA is durable before the manifest names it. Never the reverse.
  if (s.ok()) s = file->Sync();
  if (s.ok()) s = file->Close();
  if (s.ok()) s = env_->SyncDir(dbname_);

  if (!s.ok()) {
    // E-27: ENOSPC from a compaction is surfaced, and the partial output is removed rather
    // than left as a mystery file.
    env_->DeleteFile(fname);
    std::unique_lock<TrackedMutex> lock(mutex_);
    pending_outputs_.erase(out.number);
    return s;
  }

  VersionEdit edit;
  for (const FileMetaData* f : c->inputs) edit.DeleteFile(TierOf(*c, f), f->number);
  if (out.num_entries > 0) edit.AddFile(c->output_tier, out);

  {
    std::unique_lock<TrackedMutex> lock(mutex_);
    pending_outputs_.erase(out.number);
    s = versions_->LogAndApply(&edit, &mutex_);
    if (s.ok()) {
      stats_.Add(kCompactions, 1);
      stats_.Add(kBytesCompacted, input_bytes);
      RemoveObsoleteFiles();
    }
    bg_cv_.notify_all();
  }
  if (s.ok() && out.num_entries == 0) env_->DeleteFile(fname);   // everything was dropped
  return s;
}

}  // namespace lsmeng

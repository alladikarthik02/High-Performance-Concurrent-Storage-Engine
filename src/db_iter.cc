// SPEC 3.12. The user-facing iterator: turns a stream of INTERNAL keys (every version of
// every key, newest first) into a stream of USER keys (one visible value each).
//
// Three jobs, and each one is a place a bug hides:
//   1. Snapshot filtering: skip any version with seq > the snapshot's.
//   2. Deduplication: for each user key, keep only the FIRST version that survives (1),
//      which is the newest visible one -- a direct consequence of the comparator.
//   3. Tombstones: emit nothing for a key whose newest visible version is a deletion, and
//      do not fall through to an older version of the same key.
#include <string>

#include "db_impl.h"
#include "lsmeng/dbformat.h"
#include "lsmeng/iterator.h"

namespace lsmeng {
namespace {

class DBIter final : public Iterator {
 public:
  DBIter(Iterator* internal, SequenceNumber sequence)
      : iter_(internal), sequence_(sequence) {}
  ~DBIter() override { delete iter_; }

  bool Valid() const override { return valid_; }
  // Callers see the USER key. key_ stores the internal key because Next()/Prev() need
  // the sequence tag to step correctly; exposing it would leak the encoding.
  Slice key() const override { return ExtractUserKey(Slice(key_)); }
  Slice value() const override { return Slice(value_); }
  Status status() const override { return status_.ok() ? iter_->status() : status_; }

  void SeekToFirst() override {
    iter_->SeekToFirst();
    FindNextVisible();
  }

  void Seek(const Slice& target) override {
    // Seek to the newest possible version of `target` at or below our snapshot, then walk
    // forward. kValueTypeForSeek in the packed tag makes this land on the first entry with
    // user key >= target rather than skipping an exact match.
    LookupKey lk(target, sequence_);
    iter_->Seek(lk.internal_key());
    FindNextVisible();
  }

  void Next() override {
    if (!valid_) return;
    // Step past EVERY remaining version of the current user key before looking for the
    // next one. Without this the same user key is emitted once per stored version.
    //
    // key_ holds the INTERNAL key (user key + 8 tag bytes), so it must be trimmed before
    // comparing against user keys. Passing it whole made every comparison fail, so the
    // loop never advanced the iterator, FindNextVisible re-found the same entry, and the
    // caller's `for (...; it->Next())` spun forever. See CHALLENGES B17.
    const std::string current_user = ExtractUserKey(Slice(key_)).ToString();
    while (iter_->Valid() && SameUserKeyAs(current_user)) iter_->Next();
    FindNextVisible();
  }

  void SeekToLast() override {
    iter_->SeekToLast();
    if (!iter_->Valid()) { valid_ = false; return; }
    // The last INTERNAL key belongs to the largest user key, but it may be an older
    // version or a tombstone, so resolve it properly and walk backwards if it is not
    // visible.
    ParsedInternalKey p;
    if (!ParseInternalKey(iter_->key(), &p)) { SetCorrupt(); return; }

    // COPY the user key before doing anything that moves the iterator. `p.user_key` points
    // INTO the underlying data block, and ResolveUserKey re-seeks -- which makes the SST's
    // two-level iterator drop its current block and load another, freeing those bytes.
    // Using the slice afterwards is a use-after-free. This is exactly the contract
    // iterator.h states ("valid only until the next movement"), violated in my own code
    // and caught by ASan rather than by any test. See CHALLENGES B18.
    const std::string last_user = p.user_key.ToString();
    if (ResolveUserKey(last_user)) return;
    PrevFrom(last_user);
  }

  void Prev() override {
    if (!valid_) return;
    PrevFrom(ExtractUserKey(Slice(key_)).ToString());
  }

 private:
  bool SameUserKeyAs(const std::string& user_key) const {
    ParsedInternalKey p;
    if (!ParseInternalKey(iter_->key(), &p)) return false;
    return p.user_key.compare(Slice(user_key)) == 0;
  }

  void SetCorrupt() {
    valid_ = false;
    status_ = Status::Corruption("malformed internal key during iteration");
  }

  // Walk forward from wherever the internal iterator is, emitting the first user key whose
  // newest visible version is a value.
  void FindNextVisible() {
    while (iter_->Valid()) {
      ParsedInternalKey p;
      if (!ParseInternalKey(iter_->key(), &p)) { SetCorrupt(); return; }
      if (p.sequence <= sequence_) {
        // The first version at or below the snapshot IS the newest visible one, because
        // equal user keys sort newest-first.
        if (p.type == kTypeValue) {
          key_ = iter_->key().ToString();
          value_ = iter_->value().ToString();
          valid_ = true;
          return;
        }
        // A tombstone: this user key is deleted as far as this snapshot is concerned.
        // Skip ALL of its versions -- falling through to an older one would resurrect it.
        const std::string deleted = p.user_key.ToString();
        while (iter_->Valid()) {
          ParsedInternalKey q;
          if (!ParseInternalKey(iter_->key(), &q)) { SetCorrupt(); return; }
          if (q.user_key.compare(Slice(deleted)) != 0) break;
          iter_->Next();
        }
        continue;
      }
      iter_->Next();   // too new for this snapshot
    }
    valid_ = false;
  }

  // Position on the newest visible version of `user_key`, if there is one.
  bool ResolveUserKey(const std::string& user_key) {
    LookupKey lk(Slice(user_key), sequence_);
    iter_->Seek(lk.internal_key());
    if (!iter_->Valid()) return false;
    ParsedInternalKey p;
    if (!ParseInternalKey(iter_->key(), &p)) { SetCorrupt(); return false; }
    if (p.user_key.compare(Slice(user_key)) != 0) return false;
    if (p.type != kTypeValue) return false;   // deleted at this snapshot
    key_ = iter_->key().ToString();
    value_ = iter_->value().ToString();
    valid_ = true;
    return true;
  }

  // The expensive direction, and SPEC 3.12 says so. Rather than unwinding versions by
  // hand -- which is where reverse iteration bugs live -- each step does one Seek to the
  // smallest internal key of the current user key, steps back once to land on the largest
  // internal key strictly BELOW it (i.e. some version of the previous user key), and then
  // resolves that user key properly. If it turns out to be invisible (a tombstone, or only
  // versions newer than the snapshot), the loop continues to the key before it.
  void PrevFrom(const std::string& from_user_key) {
    std::string boundary = from_user_key;
    while (true) {
      // (key, kMaxSequenceNumber) is the SMALLEST internal key with this user key, because
      // equal user keys sort by descending sequence. Everything before it therefore has a
      // strictly smaller user key.
      LookupKey lk(Slice(boundary), kMaxSequenceNumber);
      iter_->Seek(lk.internal_key());
      if (iter_->Valid()) iter_->Prev();
      else iter_->SeekToLast();

      if (!iter_->Valid()) { valid_ = false; return; }
      ParsedInternalKey p;
      if (!ParseInternalKey(iter_->key(), &p)) { SetCorrupt(); return; }
      const std::string candidate = p.user_key.ToString();
      if (ResolveUserKey(candidate)) return;
      if (!status_.ok()) return;
      boundary = candidate;   // invisible; try the user key before it
    }
  }

  Iterator* iter_;
  const SequenceNumber sequence_;
  std::string key_;      // the INTERNAL key of the current entry
  std::string value_;
  bool valid_ = false;
  Status status_;
};

}  // namespace

Iterator* NewDBIterator(Iterator* internal, SequenceNumber sequence) {
  return new DBIter(internal, sequence);
}

}  // namespace lsmeng

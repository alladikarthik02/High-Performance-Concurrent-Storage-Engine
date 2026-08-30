#pragma once
#include <string>

#include "lsmeng/dbformat.h"
#include "lsmeng/slice.h"
#include "lsmeng/status.h"

namespace lsmeng {

class MemTable;

// SPEC 3.3. The atomic unit of a write, and the payload of exactly one WAL record.
//
// ONE BATCH = ONE RECORD, and that is what makes atomicity real (SPEC E-4). A record is
// all-or-nothing because a single CRC covers the whole payload, so recovery either replays
// every mutation in the batch or none of them. There is no fragmentation across records --
// which is precisely why the 8 MiB batch cap in SPEC 3.1 is a hard limit and not a
// suggestion. A batch that had to span records could be half-applied by a crash, and a
// user who relied on a two-key invariant would find it broken.
//
// REPRESENTATION (this is the on-disk payload format, not an internal convenience):
//   [ seq : fixed64 ][ count : fixed32 ]
//   repeated count times:
//     [ type : 1 ][ klen : varint ][ key ][ vlen : varint ][ value ]
//                                          (vlen/value absent for kTypeDeletion)
class WriteBatch {
 public:
  WriteBatch() { Clear(); }

  void Put(const Slice& key, const Slice& value);
  void Delete(const Slice& key);
  void Clear();

  int Count() const;
  size_t ApproximateSize() const { return rep_.size(); }

  SequenceNumber Sequence() const;
  void SetSequence(SequenceNumber seq);

  // The serialized form: what goes into the WAL, verbatim.
  const std::string& Contents() const { return rep_; }
  Status SetContents(const Slice& contents);

  // Replay into a memtable, assigning consecutive sequence numbers from Sequence().
  Status InsertInto(MemTable* mem) const;

  // Merge `src` into this batch, for group commit (SPEC 3.2 step 5).
  void Append(const WriteBatch& src);

  // Walk the records without a memtable. Used by recovery diagnostics and by tests.
  class Handler {
   public:
    virtual ~Handler() = default;
    virtual void Put(const Slice& key, const Slice& value) = 0;
    virtual void Delete(const Slice& key) = 0;
  };
  Status Iterate(Handler* handler) const;

  static constexpr size_t kHeaderSize = 12;   // fixed64 seq + fixed32 count

 private:
  std::string rep_;
};

}  // namespace lsmeng

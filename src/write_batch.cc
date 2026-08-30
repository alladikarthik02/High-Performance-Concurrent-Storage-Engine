#include "lsmeng/write_batch.h"

#include "lsmeng/coding.h"
#include "lsmeng/memtable.h"

namespace lsmeng {

void WriteBatch::Clear() {
  rep_.clear();
  rep_.resize(kHeaderSize, '\0');
}

int WriteBatch::Count() const { return static_cast<int>(DecodeFixed32(rep_.data() + 8)); }
SequenceNumber WriteBatch::Sequence() const { return DecodeFixed64(rep_.data()); }
void WriteBatch::SetSequence(SequenceNumber seq) { EncodeFixed64(&rep_[0], seq); }

namespace {
void SetCount(std::string* rep, int n) { EncodeFixed32(&(*rep)[8], static_cast<uint32_t>(n)); }
}  // namespace

void WriteBatch::Put(const Slice& key, const Slice& value) {
  SetCount(&rep_, Count() + 1);
  rep_.push_back(static_cast<char>(kTypeValue));
  PutLengthPrefixedSlice(&rep_, key);
  PutLengthPrefixedSlice(&rep_, value);
}

void WriteBatch::Delete(const Slice& key) {
  SetCount(&rep_, Count() + 1);
  rep_.push_back(static_cast<char>(kTypeDeletion));
  PutLengthPrefixedSlice(&rep_, key);
}

Status WriteBatch::SetContents(const Slice& contents) {
  if (contents.size() < kHeaderSize)
    return Status::Corruption("WriteBatch shorter than its header");
  rep_.assign(contents.data(), contents.size());
  return Status::OK();
}

void WriteBatch::Append(const WriteBatch& src) {
  SetCount(&rep_, Count() + src.Count());
  rep_.append(src.rep_.data() + kHeaderSize, src.rep_.size() - kHeaderSize);
}

Status WriteBatch::Iterate(Handler* handler) const {
  Slice input(rep_);
  if (input.size() < kHeaderSize) return Status::Corruption("WriteBatch too small");
  input.remove_prefix(kHeaderSize);

  int found = 0;
  Slice key, value;
  while (!input.empty()) {
    ++found;
    const char tag = input[0];
    input.remove_prefix(1);
    switch (static_cast<ValueType>(tag)) {
      case kTypeValue:
        if (!GetLengthPrefixedSlice(&input, &key) || !GetLengthPrefixedSlice(&input, &value))
          return Status::Corruption("bad Put record in WriteBatch");
        handler->Put(key, value);
        break;
      case kTypeDeletion:
        if (!GetLengthPrefixedSlice(&input, &key))
          return Status::Corruption("bad Delete record in WriteBatch");
        handler->Delete(key);
        break;
      default:
        return Status::Corruption("unknown WriteBatch record tag");
    }
  }
  // The declared count and the actual records must agree. A mismatch means the payload was
  // truncated or spliced -- and since the CRC already passed, that would be an encoder bug
  // rather than disk corruption, which is exactly the kind of thing that silently drops
  // the tail of a batch.
  if (found != Count()) return Status::Corruption("WriteBatch count does not match records");
  return Status::OK();
}

namespace {
class MemTableInserter : public WriteBatch::Handler {
 public:
  SequenceNumber seq;
  MemTable* mem;
  void Put(const Slice& key, const Slice& value) override {
    mem->Add(seq, kTypeValue, key, value);
    ++seq;
  }
  void Delete(const Slice& key) override {
    mem->Add(seq, kTypeDeletion, key, Slice());
    ++seq;
  }
};
}  // namespace

Status WriteBatch::InsertInto(MemTable* mem) const {
  MemTableInserter inserter;
  inserter.seq = Sequence();
  inserter.mem = mem;
  return Iterate(&inserter);
}

}  // namespace lsmeng

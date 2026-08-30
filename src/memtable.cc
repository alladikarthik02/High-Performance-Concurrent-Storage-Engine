#include "lsmeng/memtable.h"

#include <cstring>

namespace lsmeng {

void MemTable::Add(SequenceNumber seq, ValueType type, const Slice& key, const Slice& value) {
  const size_t key_size = key.size();
  const size_t val_size = value.size();
  const size_t internal_key_size = key_size + 8;
  const size_t encoded_len = VarintLength(internal_key_size) + internal_key_size +
                             VarintLength(val_size) + val_size;

  char* buf = arena_.Allocate(encoded_len);
  char* p = EncodeVarint32(buf, static_cast<uint32_t>(internal_key_size));
  std::memcpy(p, key.data(), key_size);
  p += key_size;
  EncodeFixed64(p, PackSequenceAndType(seq, type));
  p += 8;
  p = EncodeVarint32(p, static_cast<uint32_t>(val_size));
  std::memcpy(p, value.data(), val_size);
  assert(p + val_size == buf + encoded_len);

  table_.Insert(buf);
}

bool MemTable::Get(const LookupKey& key, std::string* value, Status* s) {
  Table::Iterator iter(&table_);
  iter.Seek(key.memtable_key().data());
  if (!iter.Valid()) return false;

  // The seek lands on the first entry >= (user_key, seq, kValueTypeForSeek). Because equal
  // user keys sort newest-first (SPEC 3.1), that entry is the newest version at or below
  // our snapshot -- IF its user key matches. If it does not, this memtable simply has no
  // version of the key and the caller must keep looking in older levels.
  const char* entry = iter.key();
  uint32_t key_length = 0;
  const char* key_ptr = GetVarint32Ptr(entry, entry + 5, &key_length);
  if (key_ptr == nullptr || key_length < 8) return false;   // corrupt: refuse to guess

  if (Slice(key_ptr, key_length - 8).compare(key.user_key()) != 0) return false;

  const uint64_t tag = DecodeFixed64(key_ptr + key_length - 8);
  switch (static_cast<ValueType>(tag & 0xFF)) {
    case kTypeValue: {
      uint32_t vlen = 0;
      const char* vp = GetVarint32Ptr(key_ptr + key_length, key_ptr + key_length + 5, &vlen);
      value->assign(vp, vlen);
      *s = Status::OK();
      return true;
    }
    case kTypeDeletion:
      // Found a tombstone. This is a HIT, not a miss: it must stop the search here, or the
      // read falls through to an older level and resurrects the deleted value.
      *s = Status::NotFound(Slice());
      return true;
  }
  return false;
}

MemTableIterator* MemTable::NewIterator() { return new MemTableIterator(this); }

namespace {
class MemTableInternalIterator final : public Iterator {
 public:
  explicit MemTableInternalIterator(MemTable* m) : it_(m) {}
  bool Valid() const override { return it_.Valid(); }
  void SeekToFirst() override { it_.SeekToFirst(); }
  void SeekToLast() override { it_.SeekToLast(); }
  void Seek(const Slice& target) override { it_.Seek(target); }
  void Next() override { it_.Next(); }
  void Prev() override { it_.Prev(); }
  Slice key() const override { return it_.key(); }
  Slice value() const override { return it_.value(); }
  Status status() const override { return Status::OK(); }   // memory cannot be corrupt

 private:
  MemTableIterator it_;
};
}  // namespace

Iterator* MemTable::NewInternalIterator() { return new MemTableInternalIterator(this); }

}  // namespace lsmeng

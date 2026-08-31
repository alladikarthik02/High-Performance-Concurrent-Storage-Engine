#include "lsmeng/block.h"

#include <algorithm>
#include <cassert>
#include <cstring>

#include "lsmeng/coding.h"

namespace lsmeng {

// ---------------------------------------------------------------- BlockBuilder

void BlockBuilder::Reset() {
  buffer_.clear();
  restarts_.clear();
  restarts_.push_back(0);   // the first entry is always a restart point
  counter_ = 0;
  finished_ = false;
  last_key_.clear();
}

void BlockBuilder::Add(const Slice& key, const Slice& value) {
  assert(!finished_);
  // The caller must add keys in ascending order; a violation would silently produce a
  // block whose binary search returns wrong answers, so it is asserted rather than hoped.
  assert(buffer_.empty() ||
         InternalKeyComparator::Compare(Slice(last_key_), key) < 0 ||
         last_key_.empty());

  size_t shared = 0;
  if (counter_ < restart_interval_) {
    const size_t min_len = std::min(last_key_.size(), key.size());
    while (shared < min_len && last_key_[shared] == key[shared]) ++shared;
  } else {
    // A restart point: store the key in full. This is the entry a binary search can land
    // on without having decoded anything before it.
    restarts_.push_back(static_cast<uint32_t>(buffer_.size()));
    counter_ = 0;
  }
  const size_t non_shared = key.size() - shared;

  PutVarint32(&buffer_, static_cast<uint32_t>(shared));
  PutVarint32(&buffer_, static_cast<uint32_t>(non_shared));
  PutVarint32(&buffer_, static_cast<uint32_t>(value.size()));
  buffer_.append(key.data() + shared, non_shared);
  buffer_.append(value.data(), value.size());

  last_key_.assign(key.data(), key.size());
  ++counter_;
}

Slice BlockBuilder::Finish() {
  for (uint32_t r : restarts_) PutFixed32(&buffer_, r);
  PutFixed32(&buffer_, static_cast<uint32_t>(restarts_.size()));
  finished_ = true;
  return Slice(buffer_);
}

// ---------------------------------------------------------------- Block

Block::Block(const Slice& contents) : data_(contents) {
  // A block must at least hold the restart count. Anything shorter is not a block, and
  // saying so here means every iterator below can assume the trailer exists.
  if (data_.size() < sizeof(uint32_t)) { corrupt_ = true; return; }
  num_restarts_ = DecodeFixed32(data_.data() + data_.size() - sizeof(uint32_t));
  const size_t max_restarts = (data_.size() - sizeof(uint32_t)) / sizeof(uint32_t);
  if (num_restarts_ == 0 || num_restarts_ > max_restarts) { corrupt_ = true; return; }
  restart_offset_ =
      static_cast<uint32_t>(data_.size() - (1 + num_restarts_) * sizeof(uint32_t));
}

namespace {
// Decode one entry at `p`. Returns nullptr on any inconsistency -- a block that failed its
// CRC never reaches here, but a builder bug or a truncated cache entry could, and walking
// off the end is not an acceptable response to either.
const char* DecodeEntry(const char* p, const char* limit, uint32_t* shared,
                        uint32_t* non_shared, uint32_t* value_length) {
  if (limit - p < 3) return nullptr;
  *shared = static_cast<uint8_t>(p[0]);
  *non_shared = static_cast<uint8_t>(p[1]);
  *value_length = static_cast<uint8_t>(p[2]);
  if ((*shared | *non_shared | *value_length) < 128) {
    p += 3;   // fast path: all three fit in one byte each
  } else {
    if ((p = GetVarint32Ptr(p, limit, shared)) == nullptr) return nullptr;
    if ((p = GetVarint32Ptr(p, limit, non_shared)) == nullptr) return nullptr;
    if ((p = GetVarint32Ptr(p, limit, value_length)) == nullptr) return nullptr;
  }
  if (static_cast<uint32_t>(limit - p) < (*non_shared + *value_length)) return nullptr;
  return p;
}
}  // namespace

class Block::Iter final : public Iterator {
 public:
  Iter(const char* data, uint32_t restarts, uint32_t num_restarts)
      : data_(data), restarts_(restarts), num_restarts_(num_restarts) {}

  bool Valid() const override { return current_ < restarts_; }
  Status status() const override { return status_; }
  Slice key() const override { return Slice(key_); }
  Slice value() const override { return value_; }

  void Next() override {
    assert(Valid());
    ParseNextKey();
  }

  void Prev() override {
    assert(Valid());
    // No back pointers inside a block, so stepping back means restarting at the previous
    // restart point and walking forward. Bounded by restart_interval (16), which is what
    // makes it acceptable -- but it IS the expensive direction, as SPEC 3.12 says.
    const uint32_t original = current_;
    while (GetRestartPoint(restart_index_) >= original) {
      if (restart_index_ == 0) { current_ = restarts_; restart_index_ = num_restarts_; return; }
      --restart_index_;
    }
    SeekToRestartPoint(restart_index_);
    do {
    } while (ParseNextKey() && NextEntryOffset() < original);
  }

  void Seek(const Slice& target) override {
    // Binary search over restart points: find the LAST restart whose key is < target, then
    // scan forward at most restart_interval entries. This is what the restart array buys.
    uint32_t left = 0, right = num_restarts_ - 1;
    while (left < right) {
      const uint32_t mid = (left + right + 1) / 2;
      const uint32_t region_offset = GetRestartPoint(mid);
      uint32_t shared, non_shared, value_length;
      const char* key_ptr = DecodeEntry(data_ + region_offset, data_ + restarts_, &shared,
                                        &non_shared, &value_length);
      if (key_ptr == nullptr || shared != 0) { CorruptionError(); return; }
      const Slice mid_key(key_ptr, non_shared);
      if (InternalKeyComparator::Compare(mid_key, target) < 0) left = mid;
      else right = mid - 1;
    }
    SeekToRestartPoint(left);
    while (true) {
      if (!ParseNextKey()) return;
      if (InternalKeyComparator::Compare(Slice(key_), target) >= 0) return;
    }
  }

  void SeekToFirst() override { SeekToRestartPoint(0); ParseNextKey(); }

  void SeekToLast() override {
    SeekToRestartPoint(num_restarts_ - 1);
    while (ParseNextKey() && NextEntryOffset() < restarts_) {}
  }

 private:
  uint32_t NextEntryOffset() const {
    return static_cast<uint32_t>((value_.data() + value_.size()) - data_);
  }
  uint32_t GetRestartPoint(uint32_t index) const {
    return DecodeFixed32(data_ + restarts_ + index * sizeof(uint32_t));
  }
  void SeekToRestartPoint(uint32_t index) {
    key_.clear();
    restart_index_ = index;
    const uint32_t offset = GetRestartPoint(index);
    // value_ is used only as a cursor here; its size is 0 so NextEntryOffset() == offset.
    value_ = Slice(data_ + offset, 0);
  }
  void CorruptionError() {
    current_ = restarts_;
    restart_index_ = num_restarts_;
    status_ = Status::Corruption("bad entry in block");
    key_.clear();
    value_ = Slice();
  }

  bool ParseNextKey() {
    current_ = NextEntryOffset();
    const char* p = data_ + current_;
    const char* limit = data_ + restarts_;
    if (p >= limit) { current_ = restarts_; restart_index_ = num_restarts_; return false; }

    uint32_t shared, non_shared, value_length;
    p = DecodeEntry(p, limit, &shared, &non_shared, &value_length);
    if (p == nullptr || key_.size() < shared) { CorruptionError(); return false; }

    // The prefix-compression invariant in action: reuse `shared` bytes of the PREVIOUS
    // key, then append the delta. If a builder ever emitted an entry sharing with anything
    // other than its immediate predecessor, this is where the wrong key silently appears.
    key_.resize(shared);
    key_.append(p, non_shared);
    value_ = Slice(p + non_shared, value_length);
    while (restart_index_ + 1 < num_restarts_ &&
           GetRestartPoint(restart_index_ + 1) < current_) {
      ++restart_index_;
    }
    return true;
  }

  const char* const data_;
  const uint32_t restarts_;
  const uint32_t num_restarts_;
  uint32_t current_ = 0;
  uint32_t restart_index_ = 0;
  std::string key_;
  Slice value_;
  Status status_;
};

bool Block::SeekTo(const Slice& target, Visitor* visitor) const {
  if (corrupt_) return false;
  Iter it(data_.data(), restart_offset_, num_restarts_);   // ON THE STACK -- no allocation
  it.Seek(target);
  if (!it.Valid()) return false;
  // Called HERE, while `it` is alive: it.key() points into the iterator's own reconstructed
  // key buffer, which dies with the iterator.
  visitor->OnEntry(it.key(), it.value());
  return true;
}

Iterator* Block::NewIterator() const {
  if (corrupt_) return NewErrorIterator(Status::Corruption("malformed block trailer"));
  return new Iter(data_.data(), restart_offset_, num_restarts_);
}

// ---------------------------------------------------------------- error iterators

namespace {
class EmptyIterator final : public Iterator {
 public:
  explicit EmptyIterator(const Status& s) : status_(s) {}
  bool Valid() const override { return false; }
  void SeekToFirst() override {}
  void SeekToLast() override {}
  void Seek(const Slice&) override {}
  void Next() override {}
  void Prev() override {}
  Slice key() const override { return Slice(); }
  Slice value() const override { return Slice(); }
  Status status() const override { return status_; }

 private:
  Status status_;
};
}  // namespace

Iterator* NewErrorIterator(const Status& s) { return new EmptyIterator(s); }
Iterator* NewEmptyIterator() { return new EmptyIterator(Status::OK()); }

}  // namespace lsmeng

#include "merging_iterator.h"

namespace lsmeng {
namespace {

class MergingIterator final : public Iterator {
 public:
  MergingIterator(Iterator** children, int n) : n_(n) {
    children_.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) children_.push_back(children[i]);
  }
  ~MergingIterator() override { for (Iterator* c : children_) delete c; }

  bool Valid() const override { return current_ != nullptr; }
  Slice key() const override { return current_->key(); }
  Slice value() const override { return current_->value(); }

  Status status() const override {
    // The FIRST error wins and is sticky for the caller: a scan that hit corruption in one
    // file must not report OK just because the remaining children were fine.
    for (Iterator* c : children_) {
      Status s = c->status();
      if (!s.ok()) return s;
    }
    return Status::OK();
  }

  void SeekToFirst() override {
    for (Iterator* c : children_) c->SeekToFirst();
    FindSmallest();
    direction_ = kForward;
  }

  void SeekToLast() override {
    for (Iterator* c : children_) c->SeekToLast();
    FindLargest();
    direction_ = kReverse;
  }

  void Seek(const Slice& target) override {
    for (Iterator* c : children_) c->Seek(target);
    FindSmallest();
    direction_ = kForward;
  }

  void Next() override {
    // Changing direction is the subtle part. When moving forward after a reverse walk,
    // every child except the current one is positioned at the largest entry < current --
    // one step BEHIND where a forward merge needs it. Each must be re-seeked to just past
    // the current key before the merge is valid again. Getting this wrong yields duplicate
    // or skipped entries only after a direction change, which is exactly the case a
    // forward-only test never reaches (E-15).
    if (direction_ != kForward) {
      for (Iterator* c : children_) {
        if (c == current_) continue;
        c->Seek(key());
        if (c->Valid() && InternalKeyComparator::Compare(key(), c->key()) == 0) c->Next();
      }
      direction_ = kForward;
    }
    current_->Next();
    FindSmallest();
  }

  void Prev() override {
    if (direction_ != kReverse) {
      for (Iterator* c : children_) {
        if (c == current_) continue;
        c->Seek(key());
        if (c->Valid()) c->Prev();      // land strictly before the current key
        else c->SeekToLast();           // this child is entirely before it
      }
      direction_ = kReverse;
    }
    current_->Prev();
    FindLargest();
  }

 private:
  enum Direction { kForward, kReverse };

  void FindSmallest() {
    Iterator* smallest = nullptr;
    for (Iterator* c : children_) {
      if (!c->Valid()) continue;
      if (smallest == nullptr ||
          InternalKeyComparator::Compare(c->key(), smallest->key()) < 0)
        smallest = c;
    }
    current_ = smallest;
  }

  void FindLargest() {
    Iterator* largest = nullptr;
    for (auto it = children_.rbegin(); it != children_.rend(); ++it) {
      Iterator* c = *it;
      if (!c->Valid()) continue;
      if (largest == nullptr ||
          InternalKeyComparator::Compare(c->key(), largest->key()) > 0)
        largest = c;
    }
    current_ = largest;
  }

  int n_;
  std::vector<Iterator*> children_;
  Iterator* current_ = nullptr;
  Direction direction_ = kForward;
};

}  // namespace

Iterator* NewMergingIterator(Iterator** children, int n) {
  if (n == 0) return NewEmptyIterator();
  if (n == 1) return children[0];
  return new MergingIterator(children, n);
}

}  // namespace lsmeng

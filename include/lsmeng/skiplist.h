#pragma once
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <new>

#include "lsmeng/arena.h"

namespace lsmeng {

// SPEC 3.4. A skip list with ONE writer and ANY NUMBER of concurrent readers, and NO LOCK
// ANYWHERE.
//
// WHY A SKIP LIST AND NOT A BALANCED TREE -- the concurrency answer, not the textbook one:
// insertion only allocates new nodes and publishes forward pointers. There is no
// rebalancing, so a reader traversing the structure can never observe a transiently
// inconsistent shape. A red-black tree rotates, and a rotation is exactly the kind of
// intermediate state that would force every reader to take a lock.
//
// THE WRITER INVARIANT is external: exactly one thread inserts at a time, guaranteed by
// the group-commit leader rule (SPEC 3.2 -- a thread touches the memtable only while it is
// writers_.front(), and there is exactly one front). This class does not enforce it; it
// asserts it (S12).
//
// THE MEMORY ORDERING is the single most important fact in the project:
//   * the writer publishes each forward pointer with store(RELEASE)
//   * readers follow pointers with load(ACQUIRE)
// The release/acquire pair is what guarantees that a reader who observes the pointer also
// observes the fully-initialised key bytes behind it. Without it a reader can see a
// non-null Node* whose key is still uninitialised memory -- and the failure is
// architecture-dependent, timing-dependent, and invisible on x86. This is precisely what
// the TSan configuration exists to check (S6).
//
// NOTHING IS EVER DELETED. A memtable is filled, frozen, flushed, and destroyed whole, so
// there is no node-reclamation problem and therefore no need for hazard pointers or RCU.
// Being able to say WHY they are unnecessary is stronger than using them.

template <typename Key, class Comparator>
class SkipList {
 private:
  struct Node;

 public:
  explicit SkipList(Comparator cmp, Arena* arena)
      : compare_(cmp), arena_(arena), head_(NewNode(0, kMaxHeight)), max_height_(1), rnd_(0xdeadbeef) {
    for (int i = 0; i < kMaxHeight; ++i) head_->SetNext(i, nullptr);
  }
  SkipList(const SkipList&) = delete;
  SkipList& operator=(const SkipList&) = delete;

  // Single writer only. The key must not already be present.
  void Insert(const Key& key) {
    Node* prev[kMaxHeight];
    Node* x = FindGreaterOrEqual(key, prev);
    assert(x == nullptr || !Equal(key, x->key));

    const int height = RandomHeight();
    if (height > GetMaxHeight()) {
      for (int i = GetMaxHeight(); i < height; ++i) prev[i] = head_;
      // Relaxed is correct here, and worth understanding rather than copying: a reader
      // that observes the OLD max_height_ simply starts its search lower down and still
      // finds every node, because search always descends to level 0. A reader that
      // observes the NEW value will read head_->next[i] for a level that is still nullptr,
      // which is also fine. Neither case can miss a node.
      max_height_.store(height, std::memory_order_relaxed);
    }

    x = NewNode(key, height);
    for (int i = 0; i < height; ++i) {
      // NoBarrier at level i is safe because the node is not reachable until the
      // release-store below links it in; anything that can see it has already synchronised.
      x->NoBarrier_SetNext(i, prev[i]->NoBarrier_Next(i));
      prev[i]->SetNext(i, x);   // release: publishes x and everything written into it
    }
  }

  bool Contains(const Key& key) const {
    Node* x = FindGreaterOrEqual(key, nullptr);
    return x != nullptr && Equal(key, x->key);
  }

  class Iterator {
   public:
    explicit Iterator(const SkipList* list) : list_(list), node_(nullptr) {}
    bool Valid() const { return node_ != nullptr; }
    const Key& key() const { assert(Valid()); return node_->key; }
    void Next() { assert(Valid()); node_ = node_->Next(0); }
    void Prev() {
      // No back pointers: walk from the head. Costs a search, which is why SPEC 3.12 says
      // reverse iteration is the expensive direction -- but it keeps every node one word
      // smaller and keeps insertion to forward pointers only, which is what makes the
      // lock-free reader argument hold.
      assert(Valid());
      node_ = list_->FindLessThan(node_->key);
      if (node_ == list_->head_) node_ = nullptr;
    }
    void Seek(const Key& target) { node_ = list_->FindGreaterOrEqual(target, nullptr); }
    void SeekToFirst() { node_ = list_->head_->Next(0); }
    void SeekToLast() {
      node_ = list_->FindLast();
      if (node_ == list_->head_) node_ = nullptr;
    }

   private:
    const SkipList* list_;
    Node* node_;
  };

 private:
  enum { kMaxHeight = 12 };   // ~4^12 = 16M entries at branching factor 4

  Node* NewNode(const Key& key, int height) {
    char* mem = arena_->AllocateAligned(sizeof(Node) + sizeof(std::atomic<Node*>) * (height - 1));
    return new (mem) Node(key);
  }

  int GetMaxHeight() const { return max_height_.load(std::memory_order_relaxed); }

  int RandomHeight() {
    static const unsigned int kBranching = 4;
    int height = 1;
    while (height < kMaxHeight && (Next(rnd_) % kBranching) == 0) ++height;
    return height;
  }
  static uint32_t Next(uint32_t& seed) {
    // Park-Miller. Deterministic per list, so a failing test replays identically -- a
    // skip list whose shape depends on rand() is not reproducible, and shape is exactly
    // what a search bug depends on.
    seed = static_cast<uint32_t>((static_cast<uint64_t>(seed) * 48271) % 2147483647);
    return seed;
  }

  bool Equal(const Key& a, const Key& b) const { return compare_(a, b) == 0; }

  Node* FindGreaterOrEqual(const Key& key, Node** prev) const {
    Node* x = head_;
    int level = GetMaxHeight() - 1;
    while (true) {
      Node* next = x->Next(level);     // acquire
      if (next != nullptr && compare_(next->key, key) < 0) {
        x = next;                      // keep going right
      } else {
        if (prev != nullptr) prev[level] = x;
        if (level == 0) return next;
        --level;                       // descend
      }
    }
  }

  Node* FindLessThan(const Key& key) const {
    Node* x = head_;
    int level = GetMaxHeight() - 1;
    while (true) {
      Node* next = x->Next(level);
      if (next == nullptr || compare_(next->key, key) >= 0) {
        if (level == 0) return x;
        --level;
      } else {
        x = next;
      }
    }
  }

  Node* FindLast() const {
    Node* x = head_;
    int level = GetMaxHeight() - 1;
    while (true) {
      Node* next = x->Next(level);
      if (next == nullptr) {
        if (level == 0) return x;
        --level;
      } else {
        x = next;
      }
    }
  }

  Comparator const compare_;
  Arena* const arena_;
  Node* const head_;
  std::atomic<int> max_height_;
  uint32_t rnd_;
};

template <typename Key, class Comparator>
struct SkipList<Key, Comparator>::Node {
  explicit Node(const Key& k) : key(k) {}
  Key const key;

  Node* Next(int n) {
    assert(n >= 0);
    return next_[n].load(std::memory_order_acquire);
  }
  void SetNext(int n, Node* x) {
    assert(n >= 0);
    next_[n].store(x, std::memory_order_release);
  }
  Node* NoBarrier_Next(int n) { return next_[n].load(std::memory_order_relaxed); }
  void NoBarrier_SetNext(int n, Node* x) { next_[n].store(x, std::memory_order_relaxed); }

 private:
  // Length equals the node's height; the array is allocated with the node. next_[0] is the
  // bottom level, which is the one every search must reach.
  std::atomic<Node*> next_[1];
};

}  // namespace lsmeng

#pragma once
#include <cstring>
#include <string>

namespace lsmeng {

// A non-owning (pointer, length) pair. SPEC 3.1: keys and values are arbitrary byte
// strings -- embedded NUL bytes and non-UTF-8 bytes are legal. That is the entire reason
// this type exists instead of const char*: a C string cannot represent a key containing a
// zero byte, and a storage engine that silently truncates such a key is broken in a way
// nobody notices until it costs someone their data.
//
// Lifetime: a Slice never owns its bytes. Every API taking one either copies immediately
// or documents that the caller must outlive the call.
class Slice {
 public:
  Slice() : data_(""), size_(0) {}
  Slice(const char* d, size_t n) : data_(d), size_(n) {}
  Slice(const std::string& s) : data_(s.data()), size_(s.size()) {}
  Slice(const char* s) : data_(s), size_(std::strlen(s)) {}

  const char* data() const { return data_; }
  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }
  char operator[](size_t i) const { return data_[i]; }

  void remove_prefix(size_t n) { data_ += n; size_ -= n; }
  std::string ToString() const { return std::string(data_, size_); }

  // Bytewise, shorter-is-smaller on a prefix tie. SPEC E-16: "abc" < "abcd" must hold,
  // and memcmp alone over min(len) would call them equal. The length tiebreak is not
  // decoration -- an index whose comparator got this wrong loses exactly the keys that
  // are prefixes of other keys.
  int compare(const Slice& b) const {
    const size_t min_len = size_ < b.size_ ? size_ : b.size_;
    int r = std::memcmp(data_, b.data_, min_len);
    if (r == 0) {
      if (size_ < b.size_) r = -1;
      else if (size_ > b.size_) r = +1;
    }
    return r;
  }

  bool starts_with(const Slice& p) const {
    return size_ >= p.size_ && std::memcmp(data_, p.data_, p.size_) == 0;
  }

 private:
  const char* data_;
  size_t size_;
};

inline bool operator==(const Slice& a, const Slice& b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0;
}
inline bool operator!=(const Slice& a, const Slice& b) { return !(a == b); }

}  // namespace lsmeng

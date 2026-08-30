#pragma once
#include <string>
#include <utility>

#include "lsmeng/slice.h"

namespace lsmeng {

// SPEC 4.1 / S25. Status carries a machine-readable Code as well as a message.
//
// Why the code matters more than it looks: the randomized model test (SPEC 7 layer 3) has
// to distinguish NotFound -- the model agrees the key is absent, so the test PASSES --
// from Corruption -- the engine is broken, so the test MUST FAIL. With only ok() and
// ToString(), the single most valuable test in the plan would report a corrupted database
// as a successful miss unless it string-matched a format that is not a contract. That was
// a real defect in SPEC v1 (SPEC 11.14).
//
// ToString() is for humans. S25: no test may branch on it.
class Status {
 public:
  enum class Code {
    kOk = 0,
    kNotFound,
    kCorruption,
    kIOError,
    kInvalidArgument,
    kNotSupported,
  };

  Status() : code_(Code::kOk) {}

  static Status OK() { return Status(); }
  static Status NotFound(const Slice& m) { return Status(Code::kNotFound, m); }
  static Status Corruption(const Slice& m) { return Status(Code::kCorruption, m); }
  static Status IOError(const Slice& m) { return Status(Code::kIOError, m); }
  static Status InvalidArgument(const Slice& m) { return Status(Code::kInvalidArgument, m); }
  static Status NotSupported(const Slice& m) { return Status(Code::kNotSupported, m); }

  Code code() const { return code_; }
  bool ok() const { return code_ == Code::kOk; }
  bool IsNotFound() const { return code_ == Code::kNotFound; }
  bool IsCorruption() const { return code_ == Code::kCorruption; }
  bool IsIOError() const { return code_ == Code::kIOError; }
  bool IsInvalidArgument() const { return code_ == Code::kInvalidArgument; }
  bool IsNotSupported() const { return code_ == Code::kNotSupported; }

  const std::string& message() const { return msg_; }

  std::string ToString() const {
    const char* n = "Unknown";
    switch (code_) {
      case Code::kOk: return "OK";
      case Code::kNotFound: n = "NotFound"; break;
      case Code::kCorruption: n = "Corruption"; break;
      case Code::kIOError: n = "IOError"; break;
      case Code::kInvalidArgument: n = "InvalidArgument"; break;
      case Code::kNotSupported: n = "NotSupported"; break;
    }
    return msg_.empty() ? std::string(n) : std::string(n) + ": " + msg_;
  }

 private:
  Status(Code c, const Slice& m) : code_(c), msg_(m.ToString()) {}
  Code code_;
  std::string msg_;
};

}  // namespace lsmeng

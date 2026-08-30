#include "lsmeng/version_edit.h"

#include "lsmeng/coding.h"

namespace lsmeng {
namespace {
// Tag numbers are part of the on-disk format and must never be reused with a different
// meaning: an old manifest read by a new build would then be silently misinterpreted.
enum Tag {
  kComparator = 1,
  kLogNumber = 2,
  kNextFileNumber = 3,
  kLastSequence = 4,
  kDeletedFile = 6,
  kNewFile = 7,
  kPrevLogNumber = 9,
};
}  // namespace

void VersionEdit::Clear() { *this = VersionEdit(); }

void VersionEdit::EncodeTo(std::string* dst) const {
  if (has_comparator_) { PutVarint32(dst, kComparator); PutLengthPrefixedSlice(dst, Slice(comparator_)); }
  if (has_log_number_) { PutVarint32(dst, kLogNumber); PutVarint64(dst, log_number_); }
  if (has_prev_log_number_) { PutVarint32(dst, kPrevLogNumber); PutVarint64(dst, prev_log_number_); }
  if (has_next_file_number_) { PutVarint32(dst, kNextFileNumber); PutVarint64(dst, next_file_number_); }
  if (has_last_sequence_) { PutVarint32(dst, kLastSequence); PutVarint64(dst, last_sequence_); }

  for (const auto& d : deleted_files_) {
    PutVarint32(dst, kDeletedFile);
    PutVarint32(dst, static_cast<uint32_t>(d.first));
    PutVarint64(dst, d.second);
  }
  for (const auto& nf : new_files_) {
    const FileMetaData& f = nf.second;
    PutVarint32(dst, kNewFile);
    PutVarint32(dst, static_cast<uint32_t>(nf.first));
    PutVarint64(dst, f.number);
    PutVarint64(dst, f.file_size);
    PutLengthPrefixedSlice(dst, Slice(f.smallest));
    PutLengthPrefixedSlice(dst, Slice(f.largest));
    PutVarint64(dst, f.num_entries);
    PutVarint64(dst, f.num_deletions);
  }
}

Status VersionEdit::DecodeFrom(const Slice& src) {
  Clear();
  Slice input = src;
  uint32_t tag = 0;

  // Every field is checked. A manifest record whose CRC passed can still be malformed if
  // an older or newer build wrote it, and "assume it parses" would turn a version mismatch
  // into a wild read.
  while (GetVarint32(&input, &tag)) {
    switch (tag) {
      case kComparator: {
        Slice s;
        if (!GetLengthPrefixedSlice(&input, &s)) return Status::Corruption("VersionEdit: comparator");
        SetComparatorName(s);
        break;
      }
      case kLogNumber:
        if (!GetVarint64(&input, &log_number_)) return Status::Corruption("VersionEdit: log_number");
        has_log_number_ = true;
        break;
      case kPrevLogNumber:
        if (!GetVarint64(&input, &prev_log_number_)) return Status::Corruption("VersionEdit: prev_log_number");
        has_prev_log_number_ = true;
        break;
      case kNextFileNumber:
        if (!GetVarint64(&input, &next_file_number_)) return Status::Corruption("VersionEdit: next_file_number");
        has_next_file_number_ = true;
        break;
      case kLastSequence:
        if (!GetVarint64(&input, &last_sequence_)) return Status::Corruption("VersionEdit: last_sequence");
        has_last_sequence_ = true;
        break;
      case kDeletedFile: {
        uint32_t tier = 0;
        uint64_t number = 0;
        if (!GetVarint32(&input, &tier) || !GetVarint64(&input, &number))
          return Status::Corruption("VersionEdit: deleted file");
        deleted_files_.insert({static_cast<int>(tier), number});
        break;
      }
      case kNewFile: {
        uint32_t tier = 0;
        FileMetaData f;
        Slice small, large;
        if (!GetVarint32(&input, &tier) || !GetVarint64(&input, &f.number) ||
            !GetVarint64(&input, &f.file_size) || !GetLengthPrefixedSlice(&input, &small) ||
            !GetLengthPrefixedSlice(&input, &large) || !GetVarint64(&input, &f.num_entries) ||
            !GetVarint64(&input, &f.num_deletions))
          return Status::Corruption("VersionEdit: new file");
        f.smallest = small.ToString();
        f.largest = large.ToString();
        new_files_.emplace_back(static_cast<int>(tier), f);
        break;
      }
      default:
        return Status::Corruption("VersionEdit: unknown tag");
    }
  }
  if (!input.empty()) return Status::Corruption("VersionEdit: trailing bytes");
  return Status::OK();
}

std::string VersionEdit::DebugString() const {
  std::string r = "VersionEdit{";
  if (has_log_number_) r += " log=" + std::to_string(log_number_);
  if (has_prev_log_number_) r += " prev_log=" + std::to_string(prev_log_number_);
  if (has_next_file_number_) r += " next_file=" + std::to_string(next_file_number_);
  if (has_last_sequence_) r += " last_seq=" + std::to_string(last_sequence_);
  for (const auto& d : deleted_files_)
    r += " del(tier" + std::to_string(d.first) + "," + std::to_string(d.second) + ")";
  for (const auto& n : new_files_)
    r += " add(tier" + std::to_string(n.first) + "," + std::to_string(n.second.number) + "," +
         std::to_string(n.second.file_size) + "B)";
  return r + " }";
}

}  // namespace lsmeng

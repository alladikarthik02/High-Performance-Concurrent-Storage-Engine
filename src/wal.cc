#include "lsmeng/wal.h"

#include <cstring>

#include "lsmeng/coding.h"
#include "lsmeng/crc32c.h"

namespace lsmeng {
namespace {

const char kMagic[8] = {'L', 'S', 'M', 'W', 'A', 'L', '\x00', '\x01'};

// The CRC is computed over (log_number || len || type || payload). Folding the log number
// in is what makes an all-zero header fail its own checksum (E-5), and it also means a
// record from log 7 cannot validate if it somehow appears in log 8 -- which is the shape
// of a stale-file-reuse bug.
uint32_t RecordCrc(uint64_t log_number, uint32_t len, uint8_t type, const char* payload,
                   size_t n) {
  char hdr[13];
  EncodeFixed64(hdr, log_number);
  EncodeFixed32(hdr + 8, len);
  hdr[12] = static_cast<char>(type);
  return crc32c::Extend(crc32c::Value(hdr, sizeof(hdr)), payload, n);
}

}  // namespace

Status WalWriter::Create(std::unique_ptr<WritableFile> file, uint64_t log_number,
                         std::unique_ptr<WalWriter>* out) {
  if (log_number == 0) return Status::InvalidArgument("log_number 0 is reserved (E-5)");
  std::string hdr(kMagic, sizeof(kMagic));
  PutFixed64(&hdr, log_number);
  Status s = file->Append(Slice(hdr));
  if (!s.ok()) return s;
  out->reset(new WalWriter(std::move(file), log_number));
  return Status::OK();
}

Status WalWriter::Open(std::unique_ptr<WritableFile> file, uint64_t log_number,
                       std::unique_ptr<WalWriter>* out) {
  if (log_number == 0) return Status::InvalidArgument("log_number 0 is reserved (E-5)");
  out->reset(new WalWriter(std::move(file), log_number));
  return Status::OK();
}

Status WalWriter::AddRecord(const Slice& payload) {
  if (payload.empty())
    return Status::InvalidArgument("zero-length WAL record is invalid by construction (E-5)");
  if (payload.size() > 0xFFFFFFFFull)
    return Status::InvalidArgument("WAL record exceeds 4 GiB");

  const uint32_t len = static_cast<uint32_t>(payload.size());
  char hdr[kWalRecordHeaderSize];
  EncodeFixed32(hdr, RecordCrc(log_number_, len, kWalFullBatch, payload.data(), payload.size()));
  EncodeFixed32(hdr + 4, len);
  hdr[8] = static_cast<char>(kWalFullBatch);

  Status s = file_->Append(Slice(hdr, sizeof(hdr)));
  if (!s.ok()) return s;
  return file_->Append(payload);
}

// SPEC S1: for an acknowledged sync=true write the bytes must be in the WAL and durable
// BEFORE the acknowledgement, and before the memtable insert is visible. This is that
// durability point.
Status WalWriter::Sync() { return file_->Sync(); }
Status WalWriter::Close() { return file_->Close(); }

Status WalReader::Open(Env* env, const std::string& fname, std::unique_ptr<WalReader>* out) {
  std::unique_ptr<SequentialFile> f;
  Status s = env->NewSequentialFile(fname, &f);
  if (!s.ok()) return s;

  char space[kWalHeaderSize];
  Slice hdr;
  s = f->Read(kWalHeaderSize, &hdr, space);
  if (!s.ok()) return s;
  if (hdr.size() < kWalHeaderSize)
    return Status::Corruption("WAL " + fname + ": file shorter than its header");
  if (std::memcmp(hdr.data(), kMagic, sizeof(kMagic)) != 0)
    return Status::Corruption("WAL " + fname + ": bad magic (not a log file)");
  const uint64_t n = DecodeFixed64(hdr.data() + 8);
  if (n == 0) return Status::Corruption("WAL " + fname + ": log_number 0 is reserved");

  out->reset(new WalReader(std::move(f), n));
  return Status::OK();
}

bool WalReader::Fill(size_t need) {
  while (buf_.size() - buf_pos_ < need && !eof_) {
    // Compact rather than grow without bound: a long log would otherwise buffer entirely.
    if (buf_pos_ > 0) { buf_.erase(0, buf_pos_); buf_pos_ = 0; }
    char space[32768];
    Slice got;
    if (!file_->Read(sizeof(space), &got, space).ok()) { eof_ = true; break; }
    if (got.empty()) { eof_ = true; break; }
    buf_.append(got.data(), got.size());
  }
  return buf_.size() - buf_pos_ >= need;
}

bool WalReader::ReadRecord(Slice* record, std::string* scratch) {
  // Check 1: a torn header. Fewer than 9 bytes left means the writer died mid-header.
  if (!Fill(kWalRecordHeaderSize)) {
    stop_ = (buf_.size() - buf_pos_ == 0) ? Stop::kEof : Stop::kTornHeader;
    return false;
  }

  const char* h = buf_.data() + buf_pos_;
  const uint32_t crc = DecodeFixed32(h);
  const uint32_t len = DecodeFixed32(h + 4);
  const uint8_t type = static_cast<uint8_t>(h[8]);

  // Check 2: E-5. A zero length is invalid by construction, so a zero-filled region cannot
  // masquerade as a stream of valid empty records.
  if (len == 0) { stop_ = Stop::kZeroLength; return false; }

  // Check 3: a torn payload.
  if (!Fill(kWalRecordHeaderSize + len)) { stop_ = Stop::kTornPayload; return false; }

  const char* payload = buf_.data() + buf_pos_ + kWalRecordHeaderSize;

  // Check 4: the checksum.
  if (RecordCrc(log_number_, len, type, payload, len) != crc) { stop_ = Stop::kBadCrc; return false; }

  scratch->assign(payload, len);
  *record = Slice(*scratch);
  buf_pos_ += kWalRecordHeaderSize + len;
  offset_ += kWalRecordHeaderSize + len;
  return true;
}

const char* WalReader::StopReasonString() const {
  switch (stop_) {
    case Stop::kEof: return "clean end of file";
    case Stop::kTornHeader: return "torn record header";
    case Stop::kZeroLength: return "zero-length record (E-5: zero-fill defence)";
    case Stop::kTornPayload: return "torn record payload";
    case Stop::kBadCrc: return "CRC mismatch";
  }
  return "unknown";
}

}  // namespace lsmeng

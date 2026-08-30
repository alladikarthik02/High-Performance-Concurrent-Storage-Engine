#pragma once
#include <cstdint>
#include <memory>
#include <string>

#include "lsmeng/env.h"
#include "lsmeng/slice.h"
#include "lsmeng/status.h"

namespace lsmeng {

// SPEC 3.3. The write-ahead log: framing, and torn-tail recovery.
//
// FILE LAYOUT
//   Header (16 bytes, once):
//     [ magic      : 8 ]   "LSMWAL\x00\x01"
//     [ log_number : 8 ]   little-endian, NEVER 0
//   Record (repeated to EOF):
//     [ crc32c  : 4 ]   over ( len || type || payload ), SEEDED with log_number
//     [ len     : 4 ]   payload length, MUST be >= 1
//     [ type    : 1 ]
//     [ payload : len ]
//
// WHY len == 0 IS BANNED, and why the CRC is seeded (SPEC E-5):
//   CRC32C("") is 0x00000000. So `crc=0, len=0, type=0` is a SELF-CONSISTENT record -- and
//   a zero-filled region is exactly what a filesystem produces for a preallocated-but-
//   unwritten extent. Without a defence, a zero hole parses as infinitely many valid empty
//   records and recovery walks off into garbage believing it. Two independent defences:
//   a zero length is rejected outright, and the CRC is seeded with the file's non-zero
//   log_number so an all-zero header cannot match its own checksum. Belt and braces,
//   because this failure is silent.
//
// THE SAME FRAMING IS REUSED BY THE MANIFEST (SPEC 3.9). Not laziness: it means the
// manifest's crash behaviour is tested by the WAL's tests.

constexpr size_t kWalHeaderSize = 16;
constexpr size_t kWalRecordHeaderSize = 9;   // crc(4) + len(4) + type(1)
constexpr uint8_t kWalFullBatch = 0x01;

class WalWriter {
 public:
  // Takes an already-open file. The caller creates it (and dir-syncs it) -- SPEC 3.2.1
  // pre-creates logs on the background thread precisely so that a memtable switch never
  // performs I/O under db_mutex_.
  static Status Create(std::unique_ptr<WritableFile> file, uint64_t log_number,
                       std::unique_ptr<WalWriter>* out);
  // Reopen an existing log for appending (recovery may continue a partially-filled log).
  static Status Open(std::unique_ptr<WritableFile> file, uint64_t log_number,
                     std::unique_ptr<WalWriter>* out);

  Status AddRecord(const Slice& payload);
  Status Sync();
  Status Close();
  uint64_t log_number() const { return log_number_; }

 private:
  WalWriter(std::unique_ptr<WritableFile> f, uint64_t n)
      : file_(std::move(f)), log_number_(n) {}
  std::unique_ptr<WritableFile> file_;
  uint64_t log_number_;
};

class WalReader {
 public:
  // Why a reader reports its stopping reason rather than just ending: recovery must be
  // able to distinguish "clean end of file" from "torn tail" from "this is not a log at
  // all". The first two are normal; the third is a Corruption the caller must surface.
  enum class Stop {
    kEof,             // clean end
    kTornHeader,      // fewer than 9 bytes remained
    kZeroLength,      // len == 0 -- E-5, the zero-fill defence
    kTornPayload,     // fewer than len bytes remained
    kBadCrc,          // checksum mismatch
  };

  static Status Open(Env* env, const std::string& fname, std::unique_ptr<WalReader>* out);

  // Returns true and fills `record` while records remain and verify. Returns false at the
  // first failure of any kind; stop_reason() then says which.
  bool ReadRecord(Slice* record, std::string* scratch);

  Stop stop_reason() const { return stop_; }
  const char* StopReasonString() const;
  uint64_t log_number() const { return log_number_; }
  // Byte offset where reading stopped -- the point the file is logically truncated at.
  uint64_t good_offset() const { return offset_; }

 private:
  WalReader(std::unique_ptr<SequentialFile> f, uint64_t n)
      : file_(std::move(f)), log_number_(n), offset_(kWalHeaderSize) {}

  std::unique_ptr<SequentialFile> file_;
  uint64_t log_number_;
  uint64_t offset_;
  Stop stop_ = Stop::kEof;
  std::string buf_;      // bytes read from the file but not yet consumed
  size_t buf_pos_ = 0;
  bool eof_ = false;

  bool Fill(size_t need);
};

}  // namespace lsmeng

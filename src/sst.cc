#include "lsmeng/sst.h"

#include <cassert>
#include <cstring>

#include "lsmeng/coding.h"
#include "lsmeng/crc32c.h"
#include "lsmeng/dbformat.h"

namespace lsmeng {

// ---------------------------------------------------------------- handles & footer

void BlockHandle::EncodeTo(std::string* dst) const {
  PutVarint64(dst, offset);
  PutVarint64(dst, size);
}

Status BlockHandle::DecodeFrom(Slice* input) {
  if (GetVarint64(input, &offset) && GetVarint64(input, &size)) return Status::OK();
  return Status::Corruption("bad BlockHandle");
}

void Footer::EncodeTo(std::string* dst) const {
  const size_t original = dst->size();
  bloom_handle.EncodeTo(dst);
  index_handle.EncodeTo(dst);
  // Zero-pad to a FIXED 40 bytes so the footer can always be read by seeking to
  // file_size - 48, without first knowing how long the varints happened to be.
  dst->resize(original + kFooterSize - 8, '\0');
  char m[8];
  EncodeFixed64(m, kSstMagic);
  dst->append(m, 8);   // magic LAST: an interrupted write cannot produce a parseable footer
  assert(dst->size() == original + kFooterSize);
}

Status Footer::DecodeFrom(const Slice& input) {
  if (input.size() != kFooterSize) return Status::Corruption("footer is not 48 bytes");
  const uint64_t magic = DecodeFixed64(input.data() + kFooterSize - 8);
  // The magic is checked FIRST and is the last thing written, so a file whose footer was
  // never completed is rejected here rather than being parsed into nonsense.
  if (magic != kSstMagic) return Status::Corruption("bad SST magic (not an lsmeng table)");
  Slice in(input.data(), kFooterSize - 8);
  Status s = bloom_handle.DecodeFrom(&in);
  if (s.ok()) s = index_handle.DecodeFrom(&in);
  return s;
}

// ---------------------------------------------------------------- SstBuilder

SstBuilder::SstBuilder(const Options& options, WritableFile* file)
    : options_(options),
      file_(file),
      data_block_(options.block_restart_interval),
      index_block_(1),                       // an index is dense; restart every entry
      filter_(options.bloom_bits_per_key > 0 ? options.bloom_bits_per_key : 1) {}

SstBuilder::~SstBuilder() = default;

void SstBuilder::Add(const Slice& internal_key, const Slice& value) {
  if (!status_.ok()) return;
  assert(info_.num_entries == 0 ||
         InternalKeyComparator::Compare(Slice(last_key_), internal_key) < 0);

  // Emit the index entry for the PREVIOUS block, now that we know this block has started.
  //
  // THE SEPARATOR RULE, and the bug it is written against (SPEC E-9, CHALLENGES B11):
  // the index key S for block i must satisfy   last_key(i) <= S < first_key(i+1).
  // The FIRST version of this code used first_key(i+1) itself, which satisfies the left
  // half and *violates the right half by exactly one key*: a Seek for that key finds the
  // index entry whose key EQUALS it, which points at block i -- the block before the one
  // holding it. The result is that precisely the keys sitting on a block boundary become
  // unreachable by Get, while a full scan still returns them, so the file looks fine.
  //
  // Using last_key(i) makes the inequality strict on both sides by construction and is
  // impossible to get wrong. It costs a slightly larger index than a shortest-separator
  // scheme would; that optimisation is a named next step, and correctness comes first.
  if (pending_index_entry_) {
    std::string handle_encoding;
    pending_handle_.EncodeTo(&handle_encoding);
    index_block_.Add(Slice(last_key_), Slice(handle_encoding));
    pending_index_entry_ = false;
  }

  ParsedInternalKey parsed;
  if (ParseInternalKey(internal_key, &parsed)) {
    // SPEC E-2a: EVERY entry goes into the filter, tombstones included. Omitting a
    // tombstone's key makes the filter say "not present", the file holding the delete is
    // skipped, and an older value in a deeper tier resurrects. This is the single worst
    // bug available in the design, and it is one missing line.
    if (options_.bloom_bits_per_key > 0) filter_.AddKey(parsed.user_key);
    if (parsed.type == kTypeDeletion) ++info_.num_deletions;
  } else {
    status_ = Status::Corruption("SstBuilder given a malformed internal key");
    return;
  }

  if (info_.num_entries == 0) info_.smallest_key.assign(internal_key.data(), internal_key.size());
  info_.largest_key.assign(internal_key.data(), internal_key.size());
  last_key_.assign(internal_key.data(), internal_key.size());
  ++info_.num_entries;

  data_block_.Add(internal_key, value);

  // Flush AFTER adding, so a value larger than block_size produces a one-entry oversized
  // block rather than an infinite loop or an empty block (SPEC E-28).
  if (data_block_.CurrentSizeEstimate() >= options_.block_size) FlushDataBlock();
}

void SstBuilder::FlushDataBlock() {
  if (data_block_.empty()) return;
  WriteBlock(&data_block_, &pending_handle_);
  pending_index_entry_ = true;
  if (status_.ok()) status_ = file_->Flush();
}

void SstBuilder::WriteBlock(BlockBuilder* b, BlockHandle* handle) {
  WriteRawBlock(b->Finish(), handle);
  b->Reset();
}

void SstBuilder::WriteRawBlock(const Slice& contents, BlockHandle* handle) {
  handle->offset = offset_;
  handle->size = contents.size();
  status_ = file_->Append(contents);
  if (!status_.ok()) return;

  char trailer[kBlockTrailerSize];
  trailer[0] = static_cast<char>(kNoCompression);
  const uint32_t crc =
      crc32c::Extend(crc32c::Value(contents.data(), contents.size()), trailer, 1);
  EncodeFixed32(trailer + 1, crc);
  status_ = file_->Append(Slice(trailer, kBlockTrailerSize));
  if (status_.ok()) offset_ += contents.size() + kBlockTrailerSize;
}

Status SstBuilder::Finish() {
  if (abandoned_) return Status::OK();
  FlushDataBlock();
  finished_ = true;
  if (!status_.ok()) return status_;

  BlockHandle bloom_handle, index_handle;
  if (options_.bloom_bits_per_key > 0) {
    std::string filter_block;
    filter_.Finish(&filter_block);
    WriteRawBlock(Slice(filter_block), &bloom_handle);
    if (!status_.ok()) return status_;
  }

  // The last data block's index entry. Same rule: its key is that block's last key, so a
  // Seek for any key <= it lands here and a Seek beyond it correctly finds nothing.
  if (pending_index_entry_) {
    std::string handle_encoding;
    pending_handle_.EncodeTo(&handle_encoding);
    index_block_.Add(Slice(last_key_), Slice(handle_encoding));
    pending_index_entry_ = false;
  }
  WriteBlock(&index_block_, &index_handle);
  if (!status_.ok()) return status_;

  Footer footer;
  footer.bloom_handle = bloom_handle;
  footer.index_handle = index_handle;
  std::string footer_encoding;
  footer.EncodeTo(&footer_encoding);
  status_ = file_->Append(Slice(footer_encoding));
  if (status_.ok()) offset_ += footer_encoding.size();
  info_.file_size = offset_;
  return status_;
}

// ---------------------------------------------------------------- SstReader

Status SstReader::ReadBlock(const BlockHandle& handle, bool verify, std::string* out) const {
  out->resize(handle.size + kBlockTrailerSize);
  Slice got;
  Status s = file_->Read(handle.offset, handle.size + kBlockTrailerSize, &got, &(*out)[0]);
  if (!s.ok()) return s;
  if (got.size() != handle.size + kBlockTrailerSize)
    return Status::Corruption("short read of SST block");

  const char* data = got.data();
  if (verify) {
    // S7: no block is used without CRC verification. The type byte is inside the CRC, so a
    // flipped type cannot reinterpret otherwise-good bytes.
    const uint32_t stored = DecodeFixed32(data + handle.size + 1);
    const uint32_t actual =
        crc32c::Extend(crc32c::Value(data, handle.size), data + handle.size, 1);
    if (stored != actual) return Status::Corruption("SST block checksum mismatch");
  }
  const uint8_t type = static_cast<uint8_t>(data[handle.size]);
  if (type != kNoCompression) return Status::Corruption("unknown SST block compression type");
  out->resize(handle.size);
  return Status::OK();
}

Status SstReader::Open(const Options& options, std::unique_ptr<RandomAccessFile> file,
                       uint64_t file_size, Stats* stats, std::unique_ptr<SstReader>* out) {
  if (file_size < kFooterSize)
    return Status::Corruption("file is too short to be an SST");

  char space[kFooterSize];
  Slice footer_input;
  Status s = file->Read(file_size - kFooterSize, kFooterSize, &footer_input, space);
  if (!s.ok()) return s;

  std::unique_ptr<SstReader> r(new SstReader(options, std::move(file), stats));
  s = r->footer_.DecodeFrom(footer_input);
  if (!s.ok()) return s;

  s = r->ReadBlock(r->footer_.index_handle, options.paranoid_checks, &r->index_data_);
  if (!s.ok()) return s;
  r->index_block_.reset(new Block(Slice(r->index_data_)));
  if (!r->index_block_->ok()) return Status::Corruption("malformed SST index block");

  // The filter is read ONCE, here, and pinned. E-34: it is live memory attached to an open
  // file, not cache, which is why the table cache must be bounded in bytes.
  if (!r->footer_.bloom_handle.empty()) {
    s = r->ReadBlock(r->footer_.bloom_handle, options.paranoid_checks, &r->filter_data_);
    if (!s.ok()) {
      // S8: a filter that fails to load must degrade to "maybe present", never to "not
      // present". Failing open costs a read; failing closed loses data. So this is NOT
      // propagated as an open failure.
      r->filter_data_.clear();
    }
    if (stats) stats->Add(kFilterBlocksRead, 1);
  }
  r->filter_ = BloomFilter(Slice(r->filter_data_));
  *out = std::move(r);
  return Status::OK();
}

Iterator* SstReader::BlockIterator(const Slice& index_value) const {
  BlockHandle handle;
  Slice input = index_value;
  Status s = handle.DecodeFrom(&input);
  if (!s.ok()) return NewErrorIterator(s);

  // No block cache yet -- T6 adds it. Reading into a heap buffer owned by the iterator
  // keeps the ownership story identical once the cache lands.
  auto* contents = new std::string();
  s = ReadBlock(handle, options_.paranoid_checks, contents);
  if (!s.ok()) { delete contents; return NewErrorIterator(s); }
  if (stats_) stats_->Add(kBlocksRead, 1);

  auto* block = new Block(Slice(*contents));
  Iterator* it = block->NewIterator();
  // Ownership: the iterator outlives neither, so both are deleted with it. A cleanup hook
  // is added in T6 when the cache takes over; for now the block and buffer are leaked into
  // the iterator's lifetime deliberately and freed by the wrapper below.
  struct Owning final : Iterator {
    Iterator* inner;
    Block* blk;
    std::string* buf;
    ~Owning() override { delete inner; delete blk; delete buf; }
    bool Valid() const override { return inner->Valid(); }
    void SeekToFirst() override { inner->SeekToFirst(); }
    void SeekToLast() override { inner->SeekToLast(); }
    void Seek(const Slice& t) override { inner->Seek(t); }
    void Next() override { inner->Next(); }
    void Prev() override { inner->Prev(); }
    Slice key() const override { return inner->key(); }
    Slice value() const override { return inner->value(); }
    Status status() const override { return inner->status(); }
  };
  auto* own = new Owning();
  own->inner = it;
  own->blk = block;
  own->buf = contents;
  return own;
}

uint32_t SstReader::NumDataBlocks() const {
  uint32_t n = 0;
  std::unique_ptr<Iterator> it(index_block_->NewIterator());
  for (it->SeekToFirst(); it->Valid(); it->Next()) ++n;
  return n;
}

Status SstReader::Get(const ReadOptions& options, const Slice& internal_key,
                      std::string* value, bool* found_tombstone) const {
  *found_tombstone = false;
  const Slice user_key = ExtractUserKey(internal_key);

  if (!filter_.empty()) {
    if (stats_) stats_->Add(kBloomChecked, 1);
    if (!filter_.MayContain(user_key)) {
      if (stats_) stats_->Add(kBloomRejected, 1);
      return Status::NotFound(Slice());   // certain, and no data block was read
    }
  }

  std::unique_ptr<Iterator> index_iter(index_block_->NewIterator());
  index_iter->Seek(internal_key);
  if (!index_iter->Valid()) return Status::NotFound(Slice());

  std::unique_ptr<Iterator> block_iter(BlockIterator(index_iter->value()));
  block_iter->Seek(internal_key);
  if (!block_iter->Valid()) return block_iter->status().ok() ? Status::NotFound(Slice())
                                                             : block_iter->status();

  ParsedInternalKey parsed;
  if (!ParseInternalKey(block_iter->key(), &parsed))
    return Status::Corruption("malformed internal key in SST");
  if (parsed.user_key.compare(user_key) != 0) return Status::NotFound(Slice());

  if (parsed.type == kTypeDeletion) {
    // A hit that is a delete. The caller must STOP here rather than continue to older
    // files, or the read resurrects the value this tombstone hides (SPEC 3.7, E-2).
    *found_tombstone = true;
    return Status::NotFound(Slice());
  }
  value->assign(block_iter->value().data(), block_iter->value().size());
  return Status::OK();
}

Iterator* SstReader::NewIterator(const ReadOptions& options) const {
  // A two-level iterator: walk the index, and for each entry walk that data block.
  class TwoLevel final : public Iterator {
   public:
    TwoLevel(const SstReader* r, Iterator* index) : r_(r), index_(index) {}
    ~TwoLevel() override { delete index_; delete data_; }

    bool Valid() const override { return data_ != nullptr && data_->Valid(); }
    Slice key() const override { return data_->key(); }
    Slice value() const override { return data_->value(); }
    Status status() const override {
      if (!index_->status().ok()) return index_->status();
      if (data_ && !data_->status().ok()) return data_->status();
      return status_;
    }

    void SeekToFirst() override { index_->SeekToFirst(); InitData(); SkipForward(); }
    void SeekToLast() override { index_->SeekToLast(); InitData(); if (data_) data_->SeekToLast(); SkipBackward(); }
    void Seek(const Slice& target) override {
      index_->Seek(target);
      InitData();
      if (data_) data_->Seek(target);
      SkipForward();
    }
    void Next() override { data_->Next(); SkipForward(); }
    void Prev() override { data_->Prev(); SkipBackward(); }

   private:
    void InitData() {
      delete data_;
      data_ = index_->Valid() ? r_->BlockIterator(index_->value()) : nullptr;
      if (data_) data_->SeekToFirst();
    }
    // A data block is never empty in a well-formed file, but a corrupt one could be, and
    // an iterator that stops on an empty block would silently truncate a scan.
    void SkipForward() {
      while (data_ != nullptr && !data_->Valid()) {
        if (!data_->status().ok()) { status_ = data_->status(); return; }
        index_->Next();
        InitData();
      }
    }
    void SkipBackward() {
      while (data_ != nullptr && !data_->Valid()) {
        if (!data_->status().ok()) { status_ = data_->status(); return; }
        index_->Prev();
        if (!index_->Valid()) { delete data_; data_ = nullptr; return; }
        delete data_;
        data_ = r_->BlockIterator(index_->value());
        data_->SeekToLast();
      }
    }

    const SstReader* r_;
    Iterator* index_;
    Iterator* data_ = nullptr;
    Status status_;
  };
  return new TwoLevel(this, index_block_->NewIterator());
}

}  // namespace lsmeng

// sst_dump -- read an SST's footer, index, filter parameters and entries.
//
// SPEC 4.3. This exists because SPEC E-8 and E-9 describe failures where the FILE is
// internally consistent and every checksum passes, and the only symptom is a key that
// comes back wrong. When that happens you need to see what is actually on disk, not what
// the reader thinks is there. (It earned its place immediately: CHALLENGES B11 was an
// index-separator bug of exactly that kind.)
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "lsmeng/dbformat.h"
#include "lsmeng/env.h"
#include "lsmeng/sst.h"
#include "lsmeng/stats.h"

using namespace lsmeng;

namespace {
std::string Printable(const Slice& s, size_t max = 48) {
  std::string out;
  for (size_t i = 0; i < s.size() && i < max; ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c >= 0x20 && c < 0x7F) out.push_back(static_cast<char>(c));
    else { char buf[8]; std::snprintf(buf, sizeof(buf), "\\x%02x", c); out += buf; }
  }
  if (s.size() > max) out += "...";
  return out;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: sst_dump <file.sst> [--entries]\n");
    return 2;
  }
  const std::string path = argv[1];
  const bool show_entries = (argc > 2 && std::strcmp(argv[2], "--entries") == 0);

  Env* env = Env::Default();
  uint64_t size = 0;
  Status s = env->GetFileSize(path, &size);
  if (!s.ok()) { std::fprintf(stderr, "%s\n", s.ToString().c_str()); return 1; }

  std::unique_ptr<RandomAccessFile> f;
  s = env->NewRandomAccessFile(path, &f);
  if (!s.ok()) { std::fprintf(stderr, "%s\n", s.ToString().c_str()); return 1; }

  Options opt;
  Stats stats;
  std::unique_ptr<SstReader> r;
  s = SstReader::Open(opt, std::move(f), size, &stats, &r);
  if (!s.ok()) { std::fprintf(stderr, "%s\n", s.ToString().c_str()); return 1; }

  std::printf("file            %s\n", path.c_str());
  std::printf("size            %llu bytes\n", (unsigned long long)size);
  std::printf("index handle    offset=%llu size=%llu\n",
              (unsigned long long)r->footer().index_handle.offset,
              (unsigned long long)r->footer().index_handle.size);
  std::printf("bloom handle    offset=%llu size=%llu\n",
              (unsigned long long)r->footer().bloom_handle.offset,
              (unsigned long long)r->footer().bloom_handle.size);
  std::printf("filter resident %zu bytes\n", r->FilterMemoryBytes());
  std::printf("data blocks     %u\n", r->NumDataBlocks());

  uint64_t entries = 0, deletions = 0, key_bytes = 0, value_bytes = 0;
  std::string first, last;
  std::unique_ptr<Iterator> it(r->NewIterator(ReadOptions()));
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    ParsedInternalKey p;
    if (!ParseInternalKey(it->key(), &p)) { std::printf("  !! malformed internal key\n"); break; }
    if (entries == 0) first = p.user_key.ToString();
    last = p.user_key.ToString();
    ++entries;
    if (p.type == kTypeDeletion) ++deletions;
    key_bytes += it->key().size();
    value_bytes += it->value().size();
    if (show_entries)
      std::printf("  %-40s seq=%-10llu %s  value(%zu)=%s\n", Printable(p.user_key).c_str(),
                  (unsigned long long)p.sequence,
                  p.type == kTypeDeletion ? "DEL" : "PUT", it->value().size(),
                  Printable(it->value(), 24).c_str());
  }
  if (!it->status().ok()) std::printf("  !! %s\n", it->status().ToString().c_str());

  std::printf("entries         %llu (%llu deletions)\n", (unsigned long long)entries,
              (unsigned long long)deletions);
  std::printf("smallest key    %s\n", Printable(Slice(first)).c_str());
  std::printf("largest key     %s\n", Printable(Slice(last)).c_str());
  std::printf("key bytes       %llu\n", (unsigned long long)key_bytes);
  std::printf("value bytes     %llu\n", (unsigned long long)value_bytes);
  if (entries) {
    const double overhead = static_cast<double>(size) /
                            static_cast<double>(key_bytes + value_bytes);
    std::printf("file/user ratio %.3f  (prefix compression + index + filter + trailers)\n",
                overhead);
  }
  return 0;
}

#pragma once
#include <cstdint>
#include <cstring>
#include <string>

#include "lsmeng/slice.h"

namespace lsmeng {

// Fixed-width and varint encoding. Everything on disk is LITTLE-ENDIAN regardless of the
// host, so a database written on one machine reads on another. The engine runs on aarch64
// and x86-64, both little-endian, so these compile to a plain load/store -- but writing
// them byte-by-byte is what makes the file format a specification rather than a memory
// dump.

inline void EncodeFixed32(char* dst, uint32_t v) {
  auto* p = reinterpret_cast<uint8_t*>(dst);
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
  p[2] = static_cast<uint8_t>(v >> 16);
  p[3] = static_cast<uint8_t>(v >> 24);
}
inline void EncodeFixed64(char* dst, uint64_t v) {
  EncodeFixed32(dst, static_cast<uint32_t>(v));
  EncodeFixed32(dst + 4, static_cast<uint32_t>(v >> 32));
}
inline uint32_t DecodeFixed32(const char* p) {
  const auto* b = reinterpret_cast<const uint8_t*>(p);
  return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
         (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
}
inline uint64_t DecodeFixed64(const char* p) {
  return static_cast<uint64_t>(DecodeFixed32(p)) |
         (static_cast<uint64_t>(DecodeFixed32(p + 4)) << 32);
}

inline void PutFixed32(std::string* d, uint32_t v) { char b[4]; EncodeFixed32(b, v); d->append(b, 4); }
inline void PutFixed64(std::string* d, uint64_t v) { char b[8]; EncodeFixed64(b, v); d->append(b, 8); }

// Varint: 7 bits of payload per byte, high bit = "more follows". A 32-bit value takes 1-5
// bytes, a 64-bit value 1-10. Keys and values in a data block are length-prefixed with
// these, and since most lengths are small, this is where a large part of the block's size
// saving comes from.
inline char* EncodeVarint32(char* dst, uint32_t v) {
  auto* p = reinterpret_cast<uint8_t*>(dst);
  while (v >= 0x80) { *p++ = static_cast<uint8_t>(v) | 0x80; v >>= 7; }
  *p++ = static_cast<uint8_t>(v);
  return reinterpret_cast<char*>(p);
}
inline char* EncodeVarint64(char* dst, uint64_t v) {
  auto* p = reinterpret_cast<uint8_t*>(dst);
  while (v >= 0x80) { *p++ = static_cast<uint8_t>(v) | 0x80; v >>= 7; }
  *p++ = static_cast<uint8_t>(v);
  return reinterpret_cast<char*>(p);
}
inline void PutVarint32(std::string* d, uint32_t v) { char b[5]; d->append(b, EncodeVarint32(b, v) - b); }
inline void PutVarint64(std::string* d, uint64_t v) { char b[10]; d->append(b, EncodeVarint64(b, v) - b); }

inline void PutLengthPrefixedSlice(std::string* d, const Slice& s) {
  PutVarint32(d, static_cast<uint32_t>(s.size()));
  d->append(s.data(), s.size());
}

// Returns nullptr on a malformed or truncated varint. Every decode site must check --
// these functions parse bytes that may have come off a disk written by a crashed process,
// so "trust the encoding" is not available to us. A varint that runs past `limit` is
// exactly what a torn record looks like.
inline const char* GetVarint32Ptr(const char* p, const char* limit, uint32_t* value) {
  uint32_t result = 0;
  for (uint32_t shift = 0; shift <= 28 && p < limit; shift += 7) {
    uint32_t byte = *reinterpret_cast<const uint8_t*>(p);
    ++p;
    if (byte & 0x80) {
      result |= ((byte & 0x7F) << shift);
    } else {
      result |= (byte << shift);
      *value = result;
      return p;
    }
  }
  return nullptr;
}
inline const char* GetVarint64Ptr(const char* p, const char* limit, uint64_t* value) {
  uint64_t result = 0;
  for (uint32_t shift = 0; shift <= 63 && p < limit; shift += 7) {
    uint64_t byte = *reinterpret_cast<const uint8_t*>(p);
    ++p;
    if (byte & 0x80) {
      result |= ((byte & 0x7F) << shift);
    } else {
      result |= (byte << shift);
      *value = result;
      return p;
    }
  }
  return nullptr;
}

inline bool GetVarint32(Slice* input, uint32_t* value) {
  const char* p = GetVarint32Ptr(input->data(), input->data() + input->size(), value);
  if (p == nullptr) return false;
  *input = Slice(p, static_cast<size_t>(input->data() + input->size() - p));
  return true;
}
inline bool GetVarint64(Slice* input, uint64_t* value) {
  const char* p = GetVarint64Ptr(input->data(), input->data() + input->size(), value);
  if (p == nullptr) return false;
  *input = Slice(p, static_cast<size_t>(input->data() + input->size() - p));
  return true;
}
inline bool GetLengthPrefixedSlice(Slice* input, Slice* result) {
  uint32_t len = 0;
  if (!GetVarint32(input, &len)) return false;
  if (input->size() < len) return false;
  *result = Slice(input->data(), len);
  input->remove_prefix(len);
  return true;
}

inline int VarintLength(uint64_t v) {
  int len = 1;
  while (v >= 0x80) { v >>= 7; ++len; }
  return len;
}

}  // namespace lsmeng

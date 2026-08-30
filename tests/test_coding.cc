// T1: fixed-width and varint encoding.
//
// These decoders parse bytes that may have come off a disk written by a crashed process,
// so the cases that matter most are the malformed ones. A decoder that trusts its input
// turns a torn record into a wild pointer.
#include "tests/test.h"

#include <string>
#include <vector>

#include "lsmeng/coding.h"

using namespace lsmeng;

TEST(fixed_width_roundtrip_is_little_endian_on_the_wire) {
  char b[8];
  EncodeFixed32(b, 0x01020304u);
  CHECK_EQ(static_cast<int>(static_cast<uint8_t>(b[0])), 0x04);   // LE, byte 0 is lowest
  CHECK_EQ(static_cast<int>(static_cast<uint8_t>(b[3])), 0x01);
  CHECK_EQ(DecodeFixed32(b), 0x01020304u);
  EncodeFixed64(b, 0x0102030405060708ull);
  CHECK_EQ(static_cast<int>(static_cast<uint8_t>(b[0])), 0x08);
  CHECK_EQ(DecodeFixed64(b), 0x0102030405060708ull);
  for (uint32_t v : {0u, 1u, 0x7Fu, 0x80u, 0xFFFFu, 0xFFFFFFFFu}) {
    EncodeFixed32(b, v); CHECK_EQ(DecodeFixed32(b), v);
  }
  for (uint64_t v : {0ull, 1ull, ~0ull, 1ull << 63}) {
    EncodeFixed64(b, v); CHECK_EQ(DecodeFixed64(b), v);
  }
}

TEST(varint32_roundtrip_including_every_length_boundary) {
  // 1..5 bytes. The boundaries are where an off-by-one in the shift loop lives.
  const uint32_t values[] = {0, 1, 0x7F, 0x80, 0x3FFF, 0x4000, 0x1FFFFF, 0x200000,
                             0xFFFFFFF, 0x10000000, 0xFFFFFFFFu};
  for (uint32_t v : values) {
    TCTX("v=" << v);
    std::string s;
    PutVarint32(&s, v);
    CHECK_EQ(static_cast<int>(s.size()), VarintLength(v));
    Slice in(s);
    uint32_t got = 0;
    CHECK(GetVarint32(&in, &got));
    CHECK_EQ(got, v);
    CHECK_EQ(in.size(), 0u);
  }
}

TEST(varint64_roundtrip_including_the_ten_byte_maximum) {
  std::string s;
  std::vector<uint64_t> values;
  for (int shift = 0; shift < 64; ++shift) values.push_back(1ull << shift);
  values.push_back(0);
  values.push_back(~0ull);   // 10 bytes -- the maximum, and the loop bound that matters
  for (uint64_t v : values) {
    TCTX("v=" << v);
    s.clear();
    PutVarint64(&s, v);
    Slice in(s);
    uint64_t got = 0;
    CHECK(GetVarint64(&in, &got));
    CHECK_EQ(got, v);
  }
  s.clear();
  PutVarint64(&s, ~0ull);
  CHECK_EQ(s.size(), 10u);
}

TEST(truncated_varint_is_rejected_not_read_past_the_end) {
  // This is the case that matters: a torn WAL record ends mid-varint. The decoder must
  // return false, not walk off the buffer -- ASan would catch the walk, but only if a
  // test creates the situation, and in production nothing would.
  std::string s;
  PutVarint64(&s, ~0ull);
  for (size_t keep = 0; keep < s.size(); ++keep) {
    TCTX("keep=" << keep);
    Slice in(s.data(), keep);
    uint64_t got = 0;
    CHECK(!GetVarint64(&in, &got));
  }
}

TEST(varint_with_all_continuation_bits_set_terminates) {
  // A hostile/corrupt encoding: every byte says "more follows". The shift bound must stop
  // it rather than loop until it reads something else.
  std::string s(12, static_cast<char>(0xFF));
  Slice in(s);
  uint64_t got = 0;
  CHECK(!GetVarint64(&in, &got));
  Slice in32(s);
  uint32_t got32 = 0;
  CHECK(!GetVarint32(&in32, &got32));
}

TEST(length_prefixed_slice_roundtrip_and_truncation) {
  std::string s;
  const std::string payload("with\0nul", 8);   // arbitrary bytes are legal (SPEC 3.1)
  PutLengthPrefixedSlice(&s, payload);
  PutLengthPrefixedSlice(&s, "");
  Slice in(s), a, b;
  CHECK(GetLengthPrefixedSlice(&in, &a));
  CHECK_EQ(a.ToString(), payload);
  CHECK(GetLengthPrefixedSlice(&in, &b));
  CHECK_EQ(b.size(), 0u);
  CHECK_EQ(in.size(), 0u);
  // A length that claims more bytes than remain must be refused.
  std::string bad;
  PutVarint32(&bad, 100);
  bad.append("short");
  Slice bin(bad), out;
  CHECK(!GetLengthPrefixedSlice(&bin, &out));
}

RUN_ALL()

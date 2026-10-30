#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <vector>

#include "ByteReader.h"
#include "ByteWriter.h"

namespace packetmux {
namespace {

using Bytes = std::vector<uint8_t>;

TEST(ByteWriterTest, WritesBigEndian) {
  Bytes out;
  ByteWriter::appendU8(0x01U, &out);
  ByteWriter::appendU16(0x0203U, &out);
  ByteWriter::appendU32(0x04050607U, &out);
  ByteWriter::appendU64(0x08090A0B0C0D0E0FULL, &out);
  EXPECT_EQ(out, (Bytes{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
                        0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F}));
}

TEST(ByteWriterTest, AppendsToExistingContent) {
  Bytes out{0xAA};
  const Bytes extra{0x01, 0x02};
  ByteWriter::appendBytes(extra, &out);
  EXPECT_EQ(out, (Bytes{0xAA, 0x01, 0x02}));
}

TEST(ByteReaderTest, ReadsBigEndian) {
  const Bytes data{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                   0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F};
  ByteReader reader(data);
  uint8_t u8 = 0U;
  uint16_t u16 = 0U;
  uint32_t u32 = 0U;
  uint64_t u64 = 0U;
  ASSERT_TRUE(reader.readU8(&u8));
  ASSERT_TRUE(reader.readU16(&u16));
  ASSERT_TRUE(reader.readU32(&u32));
  ASSERT_TRUE(reader.readU64(&u64));
  EXPECT_EQ(u8, 0x01U);
  EXPECT_EQ(u16, 0x0203U);
  EXPECT_EQ(u32, 0x04050607U);
  EXPECT_EQ(u64, 0x08090A0B0C0D0E0FULL);
  EXPECT_EQ(reader.remaining(), 0U);
}

TEST(ByteReaderTest, ReadPastEndFailsAndKeepsCursor) {
  const Bytes data{0x01, 0x02, 0x03};
  ByteReader reader(data);
  uint32_t u32 = 0U;
  uint64_t u64 = 0U;
  std::span<const uint8_t> bytes;
  EXPECT_FALSE(reader.readU32(&u32));
  EXPECT_FALSE(reader.readU64(&u64));
  EXPECT_FALSE(reader.readBytes(4U, &bytes));
  EXPECT_EQ(reader.remaining(), 3U);

  uint16_t u16 = 0U;
  ASSERT_TRUE(reader.readU16(&u16));
  EXPECT_EQ(u16, 0x0102U);
  EXPECT_FALSE(reader.readU16(&u16));
  EXPECT_EQ(reader.remaining(), 1U);
}

TEST(ByteReaderTest, ReadBytesBorrowsFromInput) {
  const Bytes data{0x01, 0x02, 0x03};
  ByteReader reader(data);
  std::span<const uint8_t> bytes;
  uint8_t skipped = 0U;
  ASSERT_TRUE(reader.readU8(&skipped));
  ASSERT_TRUE(reader.readBytes(2U, &bytes));
  EXPECT_EQ(bytes.data(), data.data() + 1);
  EXPECT_EQ(bytes.size(), 2U);
}

TEST(ByteReaderTest, HugeLengthDoesNotOverflow) {
  const Bytes data{0x01, 0x02};
  ByteReader reader(data);
  uint8_t skipped = 0U;
  ASSERT_TRUE(reader.readU8(&skipped));
  std::span<const uint8_t> bytes;
  EXPECT_FALSE(reader.readBytes(SIZE_MAX, &bytes));
  EXPECT_EQ(reader.remaining(), 1U);
}

}  // namespace
}  // namespace packetmux

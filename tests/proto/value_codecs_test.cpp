/// @file value_codecs_test.cpp
/// @brief Tests for the shared value codecs: how the bytes of a reading map onto a number, and what is "no value".

#include "proto_value_codecs.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>

using namespace esphome::home_io_control;

namespace {

std::optional<float> decode(ValueCodec codec, std::initializer_list<uint8_t> bytes, uint16_t max_value = 0) {
  const std::vector<uint8_t> data(bytes);
  return decode_value(codec, data, max_value);
}

}  // namespace

TEST(ValueCodecs, TenthsAreLittleEndianAndScaledDown) {
  EXPECT_FLOAT_EQ(*decode(ValueCodec::TENTHS_U16, {0xCE, 0x00}), 20.6F);
  EXPECT_FLOAT_EQ(*decode(ValueCodec::TENTHS_U16, {0x18, 0x01}), 28.0F) << "the high byte counts";
  EXPECT_FLOAT_EQ(*decode(ValueCodec::TENTHS_U16_OR_ZERO, {0x00, 0x00}), 0.0F);
}

TEST(ValueCodecs, HundredthsAreSignedAndMinusOneIsASensorFault) {
  EXPECT_FLOAT_EQ(*decode(ValueCodec::HUNDREDTHS_S16, {0x48, 0x07}), 18.64F);
  EXPECT_FLOAT_EQ(*decode(ValueCodec::HUNDREDTHS_S16, {0x9C, 0xFF}), -1.0F) << "-100 hundredths is a temperature";
  const std::optional<float> fault = decode(ValueCodec::HUNDREDTHS_S16, {0xFF, 0xFF});
  ASSERT_TRUE(fault.has_value()) << "the device answered";
  EXPECT_TRUE(std::isnan(*fault));
}

TEST(ValueCodecs, AFlagIsZeroOrOne) {
  EXPECT_FLOAT_EQ(*decode(ValueCodec::BINARY, {0x00}), 0.0F);
  EXPECT_FLOAT_EQ(*decode(ValueCodec::BINARY, {0x01}), 1.0F);
  EXPECT_FALSE(decode(ValueCodec::BINARY, {0x02}).has_value());
}

TEST(ValueCodecs, ABoundedByteStopsAtItsMaximum) {
  EXPECT_FLOAT_EQ(*decode(ValueCodec::BOUNDED_BYTE, {0x64}, 100), 100.0F);
  EXPECT_FALSE(decode(ValueCodec::BOUNDED_BYTE, {0x65}, 100).has_value());
  EXPECT_FLOAT_EQ(*decode(ValueCodec::BOUNDED_BYTE, {0xFF}, VALUE_BYTE_MAX), 255.0F) << "a byte with no bound";
}

TEST(ValueCodecs, ABoundedWordIsLittleEndianAndStopsAtItsMaximum) {
  EXPECT_FLOAT_EQ(*decode(ValueCodec::BOUNDED_U16, {0x10, 0x27}, 10000), 10000.0F);
  EXPECT_FALSE(decode(ValueCodec::BOUNDED_U16, {0x11, 0x27}, 10000).has_value());
}

TEST(ValueCodecs, WholeUnitsInABigEndianWordStartWithTheMostSignificantByte) {
  EXPECT_FLOAT_EQ(*decode(ValueCodec::U16_BE, {0x0D, 0x5E}), 3422.0F);
  EXPECT_FLOAT_EQ(*decode(ValueCodec::U16_BE, {0xFF, 0xFF}), 65535.0F) << "wider than a signed 16-bit value";
}

TEST(ValueCodecs, APairShowsItsFirstByteOnly) {
  EXPECT_FLOAT_EQ(*decode(ValueCodec::FIRST_BYTE_OF_PAIR, {0x3C, 0xB4}), 60.0F);
}

TEST(ValueCodecs, TooFewBytesAreNoValue) {
  for (const ValueCodec codec : {ValueCodec::TENTHS_U16, ValueCodec::HUNDREDTHS_S16, ValueCodec::BOUNDED_U16,
                                 ValueCodec::U16_BE, ValueCodec::FIRST_BYTE_OF_PAIR}) {
    EXPECT_FALSE(decode(codec, {0x01}, 0xFFFF).has_value()) << static_cast<int>(codec);
  }
  for (const ValueCodec codec : {ValueCodec::BINARY, ValueCodec::BOUNDED_BYTE})
    EXPECT_FALSE(decode(codec, {}, VALUE_BYTE_MAX).has_value()) << static_cast<int>(codec);
}

TEST(ValueCodecs, BytesBeyondTheValueAreIgnored) {
  EXPECT_FLOAT_EQ(*decode(ValueCodec::BINARY, {0x01, 0xAA, 0xBB}), 1.0F);
  EXPECT_FLOAT_EQ(*decode(ValueCodec::U16_BE, {0x00, 0x2A, 0xFF}), 42.0F);
}

TEST(ValueCodecs, NoneNeverDecodes) {
  EXPECT_EQ(value_codec_length(ValueCodec::NONE), 0u);
  EXPECT_FALSE(decode(ValueCodec::NONE, {0x01, 0x02}).has_value());
}

TEST(ValueCodecs, EveryCodecReadsOneOrTwoBytes) {
  for (const ValueCodec codec : {ValueCodec::BINARY, ValueCodec::BOUNDED_BYTE})
    EXPECT_EQ(value_codec_length(codec), 1u);
  for (const ValueCodec codec : {ValueCodec::TENTHS_U16, ValueCodec::TENTHS_U16_OR_ZERO, ValueCodec::HUNDREDTHS_S16,
                                 ValueCodec::BOUNDED_U16, ValueCodec::U16_BE, ValueCodec::FIRST_BYTE_OF_PAIR})
    EXPECT_EQ(value_codec_length(codec), 2u);
}

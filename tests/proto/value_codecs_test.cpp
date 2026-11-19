/// @file value_codecs_test.cpp
/// @brief Tests for the shared value codecs: how the bytes of a reading map onto a number, and what is "no value".

#include "proto_codecs.h"
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

TEST(ValueCodecs, AMarkedWordCarriesANumberOnlyBehindThePresentMarker) {
  EXPECT_FLOAT_EQ(*decode(ValueCodec::MARKED_U16_BE, {0x80, 0x0C, 0xBE}), 3262.0F);
  EXPECT_FLOAT_EQ(*decode(ValueCodec::MARKED_U16_BE, {0x80, 0xFF, 0xFF}), 65535.0F)
      << "a full word behind the present marker is a number";
  EXPECT_FALSE(decode(ValueCodec::MARKED_U16_BE, {0x81, 0xFF, 0xFF}).has_value()) << "the device's no-value pattern";
  EXPECT_FALSE(decode(ValueCodec::MARKED_U16_BE, {0x00, 0x0C, 0xBE}).has_value()) << "any other marker";
  EXPECT_FALSE(decode(ValueCodec::MARKED_U16_BE, {0x80, 0x0C}).has_value()) << "a cut-off value";
  EXPECT_EQ(value_codec_length(ValueCodec::MARKED_U16_BE), 3u);
}

TEST(ValueCodecs, AMarkedTenthsWordIsSignedAndCarriesANumberOnlyBehindThePresentMarker) {
  // An Atlantic heat pump's outdoor temperature entry (issue #244): 0x0077 tenths.
  EXPECT_FLOAT_EQ(*decode(ValueCodec::MARKED_TENTHS_S16_BE, {0x80, 0x00, 0x77}), 11.9F);
  EXPECT_FLOAT_EQ(*decode(ValueCodec::MARKED_TENTHS_S16_BE, {0x80, 0xFF, 0xCE}), -5.0F) << "signed: -50 tenths";
  EXPECT_FALSE(decode(ValueCodec::MARKED_TENTHS_S16_BE, {0x81, 0xFF, 0xFF}).has_value())
      << "the no-value pattern is not -0.1";
  EXPECT_FALSE(decode(ValueCodec::MARKED_TENTHS_S16_BE, {0x00, 0x00, 0x77}).has_value()) << "any other marker";
  EXPECT_FALSE(decode(ValueCodec::MARKED_TENTHS_S16_BE, {0x80, 0x00}).has_value()) << "a cut-off value";
}

TEST(ValueCodecs, APairShowsItsFirstByteOnly) {
  EXPECT_FLOAT_EQ(*decode(ValueCodec::FIRST_BYTE_OF_PAIR, {0x3C, 0xB4}), 60.0F);
}

TEST(ValueCodecs, ASignedBigEndianWordIsTenthsOfAUnit) {
  EXPECT_FLOAT_EQ(*decode(ValueCodec::TENTHS_S16_BE, {0x02, 0x45}), 58.1F) << "a water temperature, 581 tenths";
  EXPECT_FLOAT_EQ(*decode(ValueCodec::TENTHS_S16_BE, {0x01, 0xF0}), 49.6F);
  EXPECT_FLOAT_EQ(*decode(ValueCodec::TENTHS_S16_BE, {0xFF, 0xCE}), -5.0F) << "signed: -50 tenths";
  EXPECT_FLOAT_EQ(*decode(ValueCodec::TENTHS_S16_BE, {0x80, 0x00}), -3276.8F) << "the lowest word";
  EXPECT_FLOAT_EQ(*decode(ValueCodec::TENTHS_S16_BE, {0x7F, 0xFF}), 3276.7F) << "the highest word";
  EXPECT_FLOAT_EQ(*decode(ValueCodec::TENTHS_S16_BE, {0x02, 0x45, 0x00, 0x5C, 0x02, 0x78}), 58.1F)
      << "the four bytes after the word are not read";
  EXPECT_FALSE(decode(ValueCodec::TENTHS_S16_BE, {0x02}).has_value());
  EXPECT_EQ(value_codec_length(ValueCodec::TENTHS_S16_BE), 2u);
}

TEST(ValueCodecs, AnUnsignedBigEndianLongIsThousandthsOfAUnit) {
  EXPECT_FLOAT_EQ(*decode(ValueCodec::THOUSANDTHS_U32_BE, {0x00, 0x1B, 0xF3, 0x42}), 1831.746F)
      << "1 831 746 Wh shown as kWh";
  EXPECT_FLOAT_EQ(*decode(ValueCodec::THOUSANDTHS_U32_BE, {0x00, 0x00, 0x00, 0x00}), 0.0F);
  EXPECT_FLOAT_EQ(*decode(ValueCodec::THOUSANDTHS_U32_BE, {0x00, 0x00, 0x03, 0xE8}), 1.0F);
  EXPECT_FLOAT_EQ(*decode(ValueCodec::THOUSANDTHS_U32_BE, {0xFF, 0xFF, 0xFF, 0xFF}), 4294967.295F)
      << "the whole range of the word, not read as signed";
  EXPECT_FALSE(decode(ValueCodec::THOUSANDTHS_U32_BE, {0x00, 0x1B, 0xF3}).has_value()) << "a cut-off value";
  EXPECT_EQ(value_codec_length(ValueCodec::THOUSANDTHS_U32_BE), 4u);
}

TEST(ValueCodecs, AFlagOverASliceIsOnWhenAnyByteIsNonZero) {
  EXPECT_FLOAT_EQ(*decode(ValueCodec::ANY_NONZERO, {0x00}), 0.0F);
  EXPECT_FLOAT_EQ(*decode(ValueCodec::ANY_NONZERO, {0x00, 0x00}), 0.0F);
  EXPECT_FLOAT_EQ(*decode(ValueCodec::ANY_NONZERO, {0x01}), 1.0F);
  EXPECT_FLOAT_EQ(*decode(ValueCodec::ANY_NONZERO, {0xFF}), 1.0F) << "any byte, not only 1";
  EXPECT_FLOAT_EQ(*decode(ValueCodec::ANY_NONZERO, {0x00, 0x01}), 1.0F) << "either byte of a slice turns it on";
  EXPECT_FLOAT_EQ(*decode(ValueCodec::ANY_NONZERO, {0x01, 0x00}), 1.0F);
  EXPECT_FALSE(decode(ValueCodec::ANY_NONZERO, {}).has_value()) << "an empty slice says nothing";
  EXPECT_EQ(value_codec_length(ValueCodec::ANY_NONZERO), 1u);
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

TEST(ValueCodecs, EveryCodecReadsAsManyBytesAsItsEncodingHolds) {
  const struct {
    ValueCodec codec;
    size_t length;
  } cases[] = {
      {ValueCodec::NONE, 0},
      {ValueCodec::BINARY, 1},
      {ValueCodec::BOUNDED_BYTE, 1},
      {ValueCodec::ANY_NONZERO, 1},
      {ValueCodec::TENTHS_U16, 2},
      {ValueCodec::TENTHS_U16_OR_ZERO, 2},
      {ValueCodec::HUNDREDTHS_S16, 2},
      {ValueCodec::BOUNDED_U16, 2},
      {ValueCodec::SCALED_U16, 2},
      {ValueCodec::U16_BE, 2},
      {ValueCodec::FIRST_BYTE_OF_PAIR, 2},
      {ValueCodec::TENTHS_S16_BE, 2},
      {ValueCodec::MARKED_U16_BE, 3},
      {ValueCodec::MARKED_TENTHS_S16_BE, 3},
      {ValueCodec::THOUSANDTHS_U32_BE, 4},
  };
  for (const auto &c : cases)
    EXPECT_EQ(value_codec_length(c.codec), c.length) << static_cast<int>(c.codec);
}

// === Encoding: the inverse of decoding ===

namespace {

/// The bytes encode_value() writes for a value; empty when it refuses.
std::vector<uint8_t> encode(const ValueForm &form, float value) {
  std::array<uint8_t, 4> out{};
  const size_t n = encode_value(form, value, out);
  return std::vector<uint8_t>(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(n));
}

}  // namespace

TEST(ValueCodecsEncode, EveryWritableFormDecodesBackToTheValueItEncoded) {
  const struct {
    ValueForm form;
    float value;
  } cases[] = {
      {{.codec = ValueCodec::TENTHS_U16}, 20.5F},
      {{.codec = ValueCodec::TENTHS_U16}, 28.0F},
      {{.codec = ValueCodec::TENTHS_U16}, 6553.5F},
      {{.codec = ValueCodec::TENTHS_U16_OR_ZERO}, 17.5F},
      {{.codec = ValueCodec::TENTHS_U16_OR_ZERO}, 0.0F},
      {{.codec = ValueCodec::HUNDREDTHS_S16}, 18.64F},
      {{.codec = ValueCodec::HUNDREDTHS_S16}, -1.0F},
      {{.codec = ValueCodec::BINARY}, 0.0F},
      {{.codec = ValueCodec::BINARY}, 1.0F},
      {{.codec = ValueCodec::BOUNDED_BYTE, .max_value = 100}, 100.0F},
      {{.codec = ValueCodec::BOUNDED_BYTE, .max_value = VALUE_BYTE_MAX}, 255.0F},
      {{.codec = ValueCodec::BOUNDED_U16, .max_value = 10000}, 10000.0F},
      {{.codec = ValueCodec::BOUNDED_U16, .max_value = 10000}, 0.0F},
      {{.codec = ValueCodec::U16_BE}, 3422.0F},
      {{.codec = ValueCodec::U16_BE}, 65535.0F},
      {{.codec = ValueCodec::MARKED_U16_BE}, 3262.0F},
  };
  for (const auto &c : cases) {
    const std::vector<uint8_t> bytes = encode(c.form, c.value);
    ASSERT_EQ(bytes.size(), value_codec_length(c.form.codec)) << "codec " << static_cast<int>(c.form.codec);
    const std::optional<float> back = decode_value(c.form.codec, bytes, c.form.max_value);
    ASSERT_TRUE(back.has_value()) << "codec " << static_cast<int>(c.form.codec) << " value " << c.value;
    EXPECT_NEAR(*back, c.value, 0.0051F) << "codec " << static_cast<int>(c.form.codec);
  }
}

TEST(ValueCodecsEncode, TheBytesAreInTheOrderTheRegisterReadsBackIn) {
  EXPECT_EQ(encode({.codec = ValueCodec::TENTHS_U16}, 28.0F), (std::vector<uint8_t>{0x18, 0x01}));
  EXPECT_EQ(encode({.codec = ValueCodec::TENTHS_U16}, 20.55F), (std::vector<uint8_t>{0xCE, 0x00}))
      << "the half rounds away from zero";
  EXPECT_EQ(encode({.codec = ValueCodec::HUNDREDTHS_S16}, -1.0F), (std::vector<uint8_t>{0x9C, 0xFF}));
  EXPECT_EQ(encode({.codec = ValueCodec::BOUNDED_U16, .max_value = 10000}, 10000.0F),
            (std::vector<uint8_t>{0x10, 0x27}));
  EXPECT_EQ(encode({.codec = ValueCodec::U16_BE}, 3422.0F), (std::vector<uint8_t>{0x0D, 0x5E}))
      << "the one most-significant-byte-first word";
  EXPECT_EQ(encode({.codec = ValueCodec::MARKED_U16_BE}, 3262.0F), (std::vector<uint8_t>{0x80, 0x0C, 0xBE}));
}

TEST(ValueCodecsEncode, AValueOutsideTheFormIsRefusedNeverRoundedIntoRange) {
  const float nan = NAN;
  const float inf = INFINITY;
  const ValueForm tenths = {.codec = ValueCodec::TENTHS_U16, .write_range = ValueRange{7.0F, 28.0F}};
  for (const float value : {6.9F, 28.1F, 0.0F, -1.0F, nan, inf, -inf})
    EXPECT_TRUE(encode(tenths, value).empty()) << value;
  EXPECT_FALSE(encode(tenths, 7.0F).empty());
  EXPECT_FALSE(encode(tenths, 28.0F).empty());

  const ValueForm no_range = {.codec = ValueCodec::TENTHS_U16};
  EXPECT_TRUE(encode(no_range, 6553.6F).empty()) << "more than 16 bits of tenths";
  EXPECT_TRUE(encode(no_range, -0.1F).empty()) << "an unsigned word holds no negative";

  for (const float value : {2.0F, -1.0F, 0.5F, nan, inf})
    EXPECT_TRUE(encode({.codec = ValueCodec::BINARY}, value).empty()) << value;
  for (const float value : {101.0F, -1.0F, 1.5F, nan, inf})
    EXPECT_TRUE(encode({.codec = ValueCodec::BOUNDED_BYTE, .max_value = 100}, value).empty()) << value;
  EXPECT_TRUE(encode({.codec = ValueCodec::BOUNDED_BYTE, .max_value = 1000}, 256.0F).empty())
      << "a byte holds no more than 255, whatever the bound says";
  for (const float value : {10001.0F, -1.0F, 0.5F, nan})
    EXPECT_TRUE(encode({.codec = ValueCodec::BOUNDED_U16, .max_value = 10000}, value).empty()) << value;
  for (const float value : {65536.0F, -1.0F, 0.5F, nan})
    EXPECT_TRUE(encode({.codec = ValueCodec::U16_BE}, value).empty()) << value;
}

TEST(ValueCodecsEncode, AWriteRangeNarrowsTheSpanOfTheCodecAndNeverWidensIt) {
  // A range wider than the bytes hold: a value outside the span of the bytes is refused, not cast out of range.
  const ValueRange too_wide{-100.0F, 100000.0F};
  EXPECT_TRUE(encode({.codec = ValueCodec::TENTHS_U16, .write_range = too_wide}, 7000.0F).empty()) << "above 6553.5";
  EXPECT_TRUE(encode({.codec = ValueCodec::TENTHS_U16, .write_range = too_wide}, -1.0F).empty()) << "an unsigned word";
  EXPECT_EQ(encode({.codec = ValueCodec::TENTHS_U16, .write_range = too_wide}, 20.0F),
            (std::vector<uint8_t>{0xC8, 0x00}));
  EXPECT_TRUE(encode({.codec = ValueCodec::HUNDREDTHS_S16, .write_range = too_wide}, 400.0F).empty()) << "above 327.67";
  EXPECT_TRUE(encode({.codec = ValueCodec::HUNDREDTHS_S16, .write_range = too_wide}, -400.0F).empty());
}

TEST(ValueCodecsScale, AScaleMustRiseStrictlyInBothFields) {
  constexpr ValueScaleKnot rising[] = {{0, 0}, {50, 1000}, {100, 10000}};
  constexpr ValueScaleKnot equal_natural[] = {{0, 0}, {0, 1000}};
  constexpr ValueScaleKnot equal_raw[] = {{0, 0}, {50, 1000}, {100, 1000}};
  constexpr ValueScaleKnot falling[] = {{0, 0}, {50, 1000}, {40, 2000}};
  constexpr ValueScaleKnot single[] = {{0, 0}};
  static_assert(value_scale_rises(rising));
  EXPECT_FALSE(value_scale_rises(equal_natural)) << "two equal knots would divide by zero";
  EXPECT_FALSE(value_scale_rises(equal_raw));
  EXPECT_FALSE(value_scale_rises(falling));
  EXPECT_FALSE(value_scale_rises(single));
  EXPECT_FALSE(value_scale_rises({}));
}

TEST(ValueCodecsEncode, OnlyTheOrZeroFormTakesAZeroOutsideItsRange) {
  const ValueRange range{7.0F, 28.0F};
  EXPECT_EQ(encode({.codec = ValueCodec::TENTHS_U16_OR_ZERO, .write_range = range}, 0.0F),
            (std::vector<uint8_t>{0x00, 0x00}));
  EXPECT_EQ(encode({.codec = ValueCodec::TENTHS_U16_OR_ZERO, .write_range = range}, -0.0F),
            (std::vector<uint8_t>{0x00, 0x00}))
      << "any value that equals zero cancels";
  EXPECT_TRUE(encode({.codec = ValueCodec::TENTHS_U16_OR_ZERO, .write_range = range}, 0.1F).empty());
  EXPECT_TRUE(encode({.codec = ValueCodec::TENTHS_U16, .write_range = range}, 0.0F).empty());
}

TEST(ValueCodecsEncode, ASensorFaultPatternIsNotAValue) {
  EXPECT_TRUE(encode({.codec = ValueCodec::HUNDREDTHS_S16}, -0.01F).empty()) << "raw -1 reads back as a fault";
  EXPECT_TRUE(encode({.codec = ValueCodec::HUNDREDTHS_S16}, 400.0F).empty());
}

TEST(ValueCodecsEncode, AFormWithoutAWritableValueEncodesNothing) {
  EXPECT_TRUE(encode({.codec = ValueCodec::NONE}, 1.0F).empty());
  EXPECT_TRUE(encode({.codec = ValueCodec::FIRST_BYTE_OF_PAIR}, 1.0F).empty()) << "its second byte is a limit";
}

TEST(ValueCodecsEncode, TheReadOnlyFormsEncodeNothing) {
  EXPECT_TRUE(encode({.codec = ValueCodec::TENTHS_S16_BE}, 58.1F).empty());
  EXPECT_TRUE(encode({.codec = ValueCodec::THOUSANDTHS_U32_BE}, 1.0F).empty());
  EXPECT_TRUE(encode({.codec = ValueCodec::ANY_NONZERO}, 1.0F).empty());
  EXPECT_TRUE(encode({.codec = ValueCodec::ANY_NONZERO}, 0.0F).empty());
  EXPECT_TRUE(encode({.codec = ValueCodec::MARKED_TENTHS_S16_BE}, 11.9F).empty());
}

TEST(ValueCodecsEncode, ABufferThatIsTooShortTakesNothing) {
  std::array<uint8_t, 4> out{};
  EXPECT_EQ(encode_value({.codec = ValueCodec::TENTHS_U16}, 20.0F, std::span<uint8_t>(out.data(), 1)), 0u);
  EXPECT_EQ(encode_value({.codec = ValueCodec::MARKED_U16_BE}, 20.0F, std::span<uint8_t>(out.data(), 2)), 0u);
  EXPECT_EQ(encode_value({.codec = ValueCodec::BINARY}, 1.0F, std::span<uint8_t>(out.data(), 1)), 1u);
}

TEST(ValueCodecsEncode, ABoundedIntegerIsAWholeNumberInsideItsBound) {
  EXPECT_TRUE(value_is_bounded_integer(0.0F, 0));
  EXPECT_TRUE(value_is_bounded_integer(10.0F, 10));
  EXPECT_FALSE(value_is_bounded_integer(11.0F, 10));
  EXPECT_FALSE(value_is_bounded_integer(-1.0F, 10));
  EXPECT_FALSE(value_is_bounded_integer(1.5F, 10));
  EXPECT_FALSE(value_is_bounded_integer(NAN, 10));
  EXPECT_FALSE(value_is_bounded_integer(INFINITY, 10));
}

// === The scaled form ===

namespace {

constexpr ValueScaleKnot TEST_SCALE[] = {{0, 0}, {10, 100}, {100, 1000}};

ValueForm scaled() { return {.codec = ValueCodec::SCALED_U16, .max_value = 1000, .scale = TEST_SCALE}; }

}  // namespace

TEST(ValueCodecsScaled, ARegisterValueDecodesLinearBetweenItsKnots) {
  const auto decode_raw = [](uint16_t raw) {
    const std::vector<uint8_t> bytes = {static_cast<uint8_t>(raw & 0xFF), static_cast<uint8_t>(raw >> 8)};
    return decode_value(scaled(), bytes);
  };
  EXPECT_FLOAT_EQ(*decode_raw(0), 0.0F);
  EXPECT_FLOAT_EQ(*decode_raw(50), 5.0F);
  EXPECT_FLOAT_EQ(*decode_raw(100), 10.0F);
  EXPECT_FLOAT_EQ(*decode_raw(550), 55.0F);
  EXPECT_FLOAT_EQ(*decode_raw(1000), 100.0F);
  EXPECT_FALSE(decode_raw(1001).has_value()) << "above the register's maximum";
}

TEST(ValueCodecsScaled, AValueEncodesToTheNearestRegisterValueAndRoundTrips) {
  EXPECT_EQ(encode(scaled(), 0.0F), (std::vector<uint8_t>{0x00, 0x00}));
  EXPECT_EQ(encode(scaled(), 10.0F), (std::vector<uint8_t>{0x64, 0x00}));
  EXPECT_EQ(encode(scaled(), 100.0F), (std::vector<uint8_t>{0xE8, 0x03}));
  EXPECT_EQ(encode(scaled(), 55.0F), (std::vector<uint8_t>{0x26, 0x02})) << "550";
  EXPECT_EQ(encode(scaled(), 5.04F), (std::vector<uint8_t>{0x32, 0x00})) << "50.4 rounds to 50";
  for (float value = 0.0F; value <= 100.0F; value += 2.5F) {
    const std::vector<uint8_t> bytes = encode(scaled(), value);
    ASSERT_EQ(bytes.size(), 2u) << value;
    EXPECT_NEAR(*decode_value(scaled(), bytes), value, 0.06F) << value;
  }
}

TEST(ValueCodecsScaled, AValueOutsideTheScaleIsRefused) {
  for (const float value : {-0.1F, 100.1F, NAN, INFINITY, -INFINITY})
    EXPECT_TRUE(encode(scaled(), value).empty()) << value;
  EXPECT_TRUE(encode({.codec = ValueCodec::SCALED_U16, .max_value = 1000}, 5.0F).empty()) << "a form with no scale";
}

TEST(ValueCodecs, AWordReadBigEndianHasItsFirstByteMostSignificant) {
  const uint8_t bytes[] = {0x12, 0x34, 0xFF};
  EXPECT_EQ(read_u16_be(bytes), 0x1234);
  EXPECT_EQ(read_u16_be(bytes + 1), 0x34FF);
}

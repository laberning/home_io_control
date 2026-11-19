/// @file preamble_conversion_test.cpp
/// @brief Every preamble is a duration; each radio converts it to programmed bytes (ADR 0097).
///
/// RadioDriver::preamble_bytes_for_ms() turns a duration into the bytes a driver programs, from
/// the chip's bits per preamble byte. The tests pin, for every driver and every preamble kind:
/// the byte counts the defaults give, and that the bytes last the duration on air within a
/// millisecond when measured by the driver's own tx_air_time_us(), so the same setting is the same
/// time on every board.

#include "radio_interface.h"
#include "radio_lr1121.h"
#include "radio_sx1262.h"
#include "radio_sx1276.h"

#include "stubs/radio_test_common.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>

using namespace esphome::home_io_control;

namespace {

/// One real driver per coder, constructed without init() (no SPI traffic).
struct Drivers {
  MockSpi spi;
  MockPin rst, dio0, dio1, dio4, busy;
  RadioSX1276 sx1276{&spi, &rst, &dio0, &dio4, 17, 0x80};
  RadioSX1262 sx1262{&spi, &rst, &dio1, &busy, 17, 0x03};
  RadioLR1121 lr1121{&spi, &rst, &dio1, &busy, 17, 0x07};
  MockRadio mock;
  TenBitPreambleRadio ten_bit_mock;

  /// Every driver with a label for failure messages.
  std::vector<std::pair<const char *, RadioDriver *>> all() {
    return {
        {"sx1276", &sx1276}, {"sx1262", &sx1262}, {"lr1121", &lr1121}, {"mock", &mock}, {"mock-10-bit", &ten_bit_mock}};
  }
};

/// The time on air of the preamble alone, in milliseconds, by the driver's own air-time model.
double preamble_air_time_ms(const RadioDriver &radio, uint16_t preamble_bytes) {
  RadioTxConfig with;
  with.preamble_len = preamble_bytes;
  RadioTxConfig without;
  without.preamble_len = 0;
  return (static_cast<double>(radio.tx_air_time_us(13, with)) - radio.tx_air_time_us(13, without)) / 1000.0;
}

struct Kind {
  const char *name;
  float ms;
};

constexpr Kind kKinds[] = {
    {"wake-up", LOW_POWER_WAKE_MS},
    {"start", START_PREAMBLE_MS},
    {"response", RESPONSE_PREAMBLE_MS},
    {"cold broadcast reply", COLD_BROADCAST_REPLY_PREAMBLE_MS},
};

}  // namespace

TEST(PreambleConversion, EachRadioReportsItsBitsPerPreambleByte) {
  Drivers d;
  EXPECT_EQ(d.sx1276.preamble_bits_per_byte(), SX1276_PREAMBLE_BITS_PER_BYTE);
  EXPECT_EQ(d.sx1262.preamble_bits_per_byte(), SOFT_PHY_PREAMBLE_BITS_PER_BYTE);
  EXPECT_EQ(d.lr1121.preamble_bits_per_byte(), SOFT_PHY_PREAMBLE_BITS_PER_BYTE);
}

TEST(PreambleConversion, DefaultsGiveTheExpectedBytesOnEachCoder) {
  Drivers d;
  // 38400 bps: 38.4 bit periods per millisecond, divided by the bits per programmed byte.
  struct Row {
    const char *kind;
    float ms;
    uint16_t raw_stream_bytes;  // SX1262, LR1121
    uint16_t ten_bit_bytes;     // SX1276
  };
  const Row rows[] = {
      {"wake-up", LOW_POWER_WAKE_MS, 2880, 2304},
      {"start", START_PREAMBLE_MS, 48, 38},
      {"response", RESPONSE_PREAMBLE_MS, 17, 13},
      {"cold broadcast reply", COLD_BROADCAST_REPLY_PREAMBLE_MS, 96, 77},
  };
  for (const Row &row : rows) {
    EXPECT_EQ(d.sx1262.preamble_bytes_for_ms(row.ms), row.raw_stream_bytes) << "sx1262 " << row.kind;
    EXPECT_EQ(d.lr1121.preamble_bytes_for_ms(row.ms), row.raw_stream_bytes) << "lr1121 " << row.kind;
    EXPECT_EQ(d.sx1276.preamble_bytes_for_ms(row.ms), row.ten_bit_bytes) << "sx1276 " << row.kind;
  }
}

TEST(PreambleConversion, ConvertedBytesLastTheDurationOnAirOnEveryRadioAndKind) {
  Drivers d;
  for (const auto &[radio_name, radio] : d.all()) {
    for (const Kind &kind : kKinds) {
      const uint16_t bytes = radio->preamble_bytes_for_ms(kind.ms);
      EXPECT_NEAR(preamble_air_time_ms(*radio, bytes), kind.ms, 1.0) << radio_name << " " << kind.name;
    }
  }
}

TEST(PreambleConversion, ADurationInsideTheTuningRangeStaysWithinAMillisecondOfItself) {
  // The whole range the YAML keys accept, in 0.5 ms steps from the floor to the wake-up ceiling.
  Drivers d;
  for (const auto &[radio_name, radio] : d.all()) {
    for (float ms = 1.5F; ms <= 1000.0F; ms += (ms < 70.0F ? 0.5F : 10.0F)) {
      EXPECT_NEAR(preamble_air_time_ms(*radio, radio->preamble_bytes_for_ms(ms)), ms, 1.0) << radio_name << " " << ms;
    }
  }
}

TEST(PreambleConversion, ASlowerCoderNeedsFewerBytesForTheSameDuration) {
  Drivers d;
  EXPECT_LT(d.sx1276.preamble_bytes_for_ms(LOW_POWER_WAKE_MS), d.sx1262.preamble_bytes_for_ms(LOW_POWER_WAKE_MS));
  EXPECT_EQ(d.sx1262.preamble_bytes_for_ms(LOW_POWER_WAKE_MS), d.lr1121.preamble_bytes_for_ms(LOW_POWER_WAKE_MS));
}

TEST(PreambleConversion, TheInverseGivesTheDurationABytesValueHasAlwaysLasted) {
  Drivers d;
  // 2048 bytes: 427 ms on a raw byte stream, 533 ms on the 10-bit coder (bench measurements).
  EXPECT_NEAR(d.sx1262.preamble_ms_for_bytes(2048), 426.67F, 0.01F);
  EXPECT_NEAR(d.sx1276.preamble_ms_for_bytes(2048), 533.33F, 0.01F);
  for (const auto &[radio_name, radio] : d.all()) {
    for (const uint16_t bytes : {1, 8, 12, 32, 48, 80, 1024, 2048, 4096}) {
      EXPECT_EQ(radio->preamble_bytes_for_ms(radio->preamble_ms_for_bytes(bytes)), bytes) << radio_name << " " << bytes;
    }
  }
}

TEST(PreambleConversion, TheResultIsClampedToWhatAPreambleLengthFieldHolds) {
  Drivers d;
  EXPECT_EQ(d.sx1262.preamble_bytes_for_ms(0.0F), 1) << "a preamble is never empty";
  EXPECT_EQ(d.sx1262.preamble_bytes_for_ms(-5.0F), 1);
  EXPECT_EQ(d.sx1262.preamble_bytes_for_ms(1.0e9F), UINT16_MAX);
}

TEST(PreambleConversion, EveryDriverStartsWithTheDefaultResponsePreambleConverted) {
  Drivers d;
  EXPECT_EQ(d.sx1262.response_preamble(), 17);
  EXPECT_EQ(d.lr1121.response_preamble(), 17);
  EXPECT_EQ(d.sx1276.response_preamble(), 13);
}

TEST(PreambleConversion, ApplyTuningConvertsTheResponseDurationPerRadio) {
  Drivers d;
  TuningConfig tuning;
  tuning.response_preamble_ms = 10.0F;
  d.sx1262.apply_tuning(tuning);
  d.lr1121.apply_tuning(tuning);
  d.sx1276.apply_tuning(tuning);
  EXPECT_EQ(d.sx1262.response_preamble(), 48);
  EXPECT_EQ(d.lr1121.response_preamble(), 48);
  EXPECT_EQ(d.sx1276.response_preamble(), 38);
  EXPECT_NEAR(preamble_air_time_ms(d.sx1262, d.sx1262.response_preamble()),
              preamble_air_time_ms(d.sx1276, d.sx1276.response_preamble()), 1.0)
      << "the same setting lasts the same on both coders";
}

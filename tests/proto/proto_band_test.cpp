#include "proto_band.h"

#include "test_bands.h"
#include "test_helpers.h"

using namespace esphome::home_io_control;

// ============================================================================
// The band value (ADR 0132): the 868 MHz values, the channel-to-air-frequency mapping, and the
// timing and default a band carries.
// ============================================================================

TEST(ProtoBand, The868BandHoldsTodaysTimingAndNoLowPowerDefault) {
  EXPECT_EQ(IO_868_BAND.start_reply_window_ms, 400);
  EXPECT_EQ(IO_868_BAND.low_power_reply_window_ms, 550);
  EXPECT_EQ(IO_868_BAND.retry_gap_ms, 250);
  EXPECT_FALSE(IO_868_BAND.low_power_by_default);
  EXPECT_EQ(IO_868_BAND.late_answer_warn_ms, 0) << "0 reports no late answer";
}

TEST(ProtoBand, The868BandPrintsEachChannelAtItsOwnFrequency) {
  for (const uint32_t channel : IO_CHANNELS)
    EXPECT_EQ(IO_868_BAND.air_hz_of(channel), channel);
}

TEST(ProtoBand, AnotherBandMapsEachChannelToItsAirFrequency) {
  // The second band is invented (tests/support/test_bands.h).
  const IoBand band = test_bands::second_band();
  EXPECT_EQ(band.air_hz_of(FREQ_CH2), test_bands::SECOND_BAND_CH2_HZ);
  EXPECT_EQ(band.air_hz_of(FREQ_CH1), test_bands::SECOND_BAND_CH1_HZ);
  EXPECT_EQ(band.air_hz_of(FREQ_CH3), test_bands::SECOND_BAND_CH3_HZ);
}

TEST(ProtoBand, AFrequencyThatIsNoChannelComesBackUnchanged) {
  EXPECT_EQ(test_bands::second_band().air_hz_of(123456), 123456U);
  EXPECT_EQ(IO_868_BAND.air_hz_of(0), 0U);
}

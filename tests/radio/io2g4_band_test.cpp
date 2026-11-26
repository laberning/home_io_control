/// @file io2g4_band_test.cpp
/// @brief The 2.4 GHz band as the hub reads it (ADR 0132): each value of IO_2G4_BAND, and the hub's own rules
/// reading it: the channels a request may use, the busy limit before a quiet level, the reply windows, the retry
/// gap and the late-answer report.
/// @ingroup hioc_radio

#include <gtest/gtest.h>

#include "decisions_attempts.h"
#include "decisions_channel.h"
#include "decisions_exchange.h"
#include "radio_io2g4_phy.h"

using namespace esphome::home_io_control;
using namespace esphome::home_io_control::bridle;

TEST(Io2g4Band, TheThreeChannelsLieAt2425And2450And2475MHzIndexedLikeTheHubsChannels) {
  // IO_CHANNELS is channel 2, 1, 3.
  EXPECT_EQ(IO_2G4_BAND.air_hz_of(FREQ_CH2), 2450000000u);
  EXPECT_EQ(IO_2G4_BAND.air_hz_of(FREQ_CH1), 2425000000u);
  EXPECT_EQ(IO_2G4_BAND.air_hz_of(FREQ_CH3), 2475000000u);
}

TEST(Io2g4Band, EveryChannelWakesALowPowerReceiver) { EXPECT_EQ(IO_2G4_BAND.wake_up_channels, ChannelSet::all()); }

TEST(Io2g4Band, TheBusyLimitBeforeAQuietLevelIsMinus75Dbm) {
  EXPECT_EQ(IO_2G4_BAND.busy_limit_until_quiet_level_dbm, -75);
  EXPECT_EQ(decisions::busy_limit_dbm(std::nullopt, IO_2G4_BAND.busy_limit_until_quiet_level_dbm), -75)
      << "a bridge learns no quiet level, so this is its limit";
}

TEST(Io2g4Band, BothReplyWindowsAreSixtyMillisecondsAndTheGapFifty) {
  EXPECT_EQ(IO_2G4_BAND.start_reply_window_ms, 60);
  EXPECT_EQ(IO_2G4_BAND.low_power_reply_window_ms, 60);
  EXPECT_EQ(IO_2G4_BAND.retry_gap_ms, 50);
}

TEST(Io2g4Band, ADeviceIsLowPowerUnlessItsConfigurationSaysOtherwise) { EXPECT_TRUE(IO_2G4_BAND.low_power_by_default); }

TEST(Io2g4Band, AnAnswerIsLateAfterThirtyMilliseconds) {
  EXPECT_EQ(IO_2G4_BAND.late_answer_warn_ms, 30);
  EXPECT_FALSE(decisions::answer_is_late(30000, IO_2G4_BAND));
  EXPECT_TRUE(decisions::answer_is_late(30001, IO_2G4_BAND));
}

TEST(Io2g4Band, TheValuesAreTheBandsAndNotThe868MHzOnes) {
  EXPECT_NE(IO_2G4_BAND.busy_limit_until_quiet_level_dbm, IO_868_BAND.busy_limit_until_quiet_level_dbm);
  EXPECT_NE(IO_2G4_BAND.start_reply_window_ms, IO_868_BAND.start_reply_window_ms);
  EXPECT_NE(IO_2G4_BAND.low_power_reply_window_ms, IO_868_BAND.low_power_reply_window_ms);
  EXPECT_NE(IO_2G4_BAND.retry_gap_ms, IO_868_BAND.retry_gap_ms);
  EXPECT_NE(IO_2G4_BAND.low_power_by_default, IO_868_BAND.low_power_by_default);
  EXPECT_NE(IO_2G4_BAND.late_answer_warn_ms, IO_868_BAND.late_answer_warn_ms);
}

// === The hub's rules over this band ===

TEST(Io2g4Band, ARequestToALowPowerReceiverMayUseAllThreeChannels) {
  EXPECT_EQ(decisions::request_channels(true, IO_2G4_BAND, 0), ChannelSet::all());
  EXPECT_EQ(decisions::request_channels(false, IO_2G4_BAND, 0), ChannelSet::all());
}

TEST(Io2g4Band, AWifiChannelOnTheMiddleOfTheBandTakesChannel2OutOfEveryRequest) {
  // The hub's Wi-Fi on 2450 MHz (channel 8): channel 2 lies on it, 2425 and 2475 MHz are 25 MHz away.
  const ChannelSet channels = decisions::request_channels(true, IO_2G4_BAND, 2450000000u);
  EXPECT_FALSE(channels.contains(FREQ_CH2));
  EXPECT_TRUE(channels.contains(FREQ_CH1));
  EXPECT_TRUE(channels.contains(FREQ_CH3));
  EXPECT_EQ(channels.count(), 2u);
}

TEST(Io2g4Band, TheStartWindowIsTheBandsUntilTheUserSetsOne) {
  TuningConfig tuning;
  EXPECT_EQ(decisions::start_reply_window_ms(tuning, IO_2G4_BAND), 60);
  tuning.exchange_start_response_wait_ms = 400;
  EXPECT_EQ(decisions::start_reply_window_ms(tuning, IO_2G4_BAND), 400);
}

TEST(Io2g4Band, ARetryGapOfAPlanIsTheBandsGap) {
  EXPECT_EQ(decisions::attempt_plan_for_tries(3, 60, IO_2G4_BAND).gap_ms, 50);
}

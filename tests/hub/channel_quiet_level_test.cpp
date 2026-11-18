#include "channel_quiet_level.h"

#include "test_helpers.h"

using namespace esphome::home_io_control;

// ============================================================================
// The tally of idle readings, the busy limit rule and the log bookkeeping of ADR 0096. Pure: numbers in,
// numbers out, no radio or clock.
// ============================================================================

namespace {

/// Feed @p count readings of @p level_dbm on @p freq.
void feed(ChannelQuietLevel &tally, uint32_t freq, int16_t level_dbm, int count) {
  for (int i = 0; i < count; i++)
    tally.add_reading(freq, level_dbm);
}

}  // namespace

TEST(ChannelQuietLevel, IsUnknownUntilAChannelHasEnoughReadings) {
  ChannelQuietLevel tally;
  feed(tally, FREQ_CH2, -100, QUIET_LEVEL_MIN_READINGS - 1);
  EXPECT_FALSE(tally.quiet_level_dbm(FREQ_CH2).has_value());
  tally.add_reading(FREQ_CH2, -100);
  EXPECT_EQ(tally.quiet_level_dbm(FREQ_CH2), -100);
}

TEST(ChannelQuietLevel, IsTheMedianNotTheHighestOrTheMean) {
  ChannelQuietLevel tally;
  feed(tally, FREQ_CH1, -110, 150);
  feed(tally, FREQ_CH1, -100, 40);
  feed(tally, FREQ_CH1, -60, 10);  // rare strong readings do not move the median
  EXPECT_EQ(tally.quiet_level_dbm(FREQ_CH1), -110);
  feed(tally, FREQ_CH1, -100, 200);  // now the middle reading is -100
  EXPECT_EQ(tally.quiet_level_dbm(FREQ_CH1), -100);
}

TEST(ChannelQuietLevel, BoardLevelNeedsEveryChannelAndTakesTheLowest) {
  ChannelQuietLevel tally;
  feed(tally, FREQ_CH1, -108, QUIET_LEVEL_MIN_READINGS);
  feed(tally, FREQ_CH2, -100, QUIET_LEVEL_MIN_READINGS);
  EXPECT_FALSE(tally.board_quiet_level().has_value()) << "channel 3 has no level yet";
  feed(tally, FREQ_CH3, -95, QUIET_LEVEL_MIN_READINGS);
  const auto board = tally.board_quiet_level();
  ASSERT_TRUE(board.has_value());
  EXPECT_EQ(board->level_dbm, -108);
  EXPECT_EQ(board->freq_hz, FREQ_CH1) << "names the channel the level was measured on";
}

TEST(ChannelQuietLevel, ReadingsOutsideTheRangeCountAtTheEndBins) {
  ChannelQuietLevel tally;
  feed(tally, FREQ_CH1, -200, QUIET_LEVEL_MIN_READINGS);
  feed(tally, FREQ_CH2, 0, QUIET_LEVEL_MIN_READINGS);
  EXPECT_EQ(tally.quiet_level_dbm(FREQ_CH1), -130);
  EXPECT_EQ(tally.quiet_level_dbm(FREQ_CH2), -60);
}

TEST(ChannelQuietLevel, IgnoresAFrequencyThatIsNoChannel) {
  ChannelQuietLevel tally;
  feed(tally, 433920000, -100, QUIET_LEVEL_MIN_READINGS);
  EXPECT_FALSE(tally.board_quiet_level().has_value());
  EXPECT_FALSE(tally.quiet_level_dbm(433920000).has_value());
}

TEST(ChannelQuietLevel, FollowsAChangeOfTheSurroundingsOnceReadingsAreHalved) {
  ChannelQuietLevel tally;
  feed(tally, FREQ_CH1, -110, QUIET_LEVEL_HALVING_READINGS - 1);
  ASSERT_EQ(tally.quiet_level_dbm(FREQ_CH1), -110);
  // The halving at 4096 readings keeps about 2048 of the old; after that the new level overtakes
  // within a couple of thousand readings instead of the 4096 an unbounded tally would need.
  feed(tally, FREQ_CH1, -95, 1);  // reaches the halving count
  feed(tally, FREQ_CH1, -95, 2100);
  EXPECT_EQ(tally.quiet_level_dbm(FREQ_CH1), -95);
}

TEST(ChannelQuietLevel, KeepsAKnownLevelAcrossTheHalving) {
  ChannelQuietLevel tally;
  feed(tally, FREQ_CH3, -105, QUIET_LEVEL_HALVING_READINGS * 3);
  EXPECT_EQ(tally.quiet_level_dbm(FREQ_CH3), -105);
}

TEST(DecisionsChannel, BusyLimitIsMinusEightyFiveUntilTheQuietLevelIsKnown) {
  EXPECT_EQ(decisions::busy_limit_dbm(std::nullopt), -85);
}

TEST(DecisionsChannel, BusyLimitIsTheQuietLevelPlusTheMarginInsideTheBounds) {
  EXPECT_EQ(decisions::busy_limit_dbm(-100), -81);
  EXPECT_EQ(decisions::busy_limit_dbm(-95), -76);
  EXPECT_EQ(decisions::busy_limit_dbm(-108), -89) << "bench V4.3, the noisiest of the four";
}

TEST(DecisionsChannel, BusyLimitNeverGoesBelowMinusNinety) {
  EXPECT_EQ(decisions::busy_limit_dbm(-113), -90) << "bench V2";
  EXPECT_EQ(decisions::busy_limit_dbm(-109), -90) << "-109 + 19 is exactly the bound";
  EXPECT_EQ(decisions::busy_limit_dbm(-130), -90);
}

TEST(DecisionsChannel, BusyLimitNeverGoesAboveMinusSeventyFive) {
  EXPECT_EQ(decisions::busy_limit_dbm(-89), -75) << "a board that idles at -89 dBm";
  EXPECT_EQ(decisions::busy_limit_dbm(-60), -75);
}

TEST(DecisionsChannel, OnlyANoisyBoardLeavesLittleMargin) {
  EXPECT_FALSE(decisions::quiet_level_leaves_little_margin(-113)) << "bench V2";
  EXPECT_FALSE(decisions::quiet_level_leaves_little_margin(-108)) << "bench V4.3";
  EXPECT_FALSE(decisions::quiet_level_leaves_little_margin(-89)) << "-89 + 4 = -85 is exactly 10 dB below the bound";
  EXPECT_TRUE(decisions::quiet_level_leaves_little_margin(-88));
}

TEST(BusyLimitLog, SaysNothingWhileTheQuietLevelIsUnknown) {
  BusyLimitLog log;
  const BusyLimitReport report = log.update(std::nullopt);
  EXPECT_FALSE(report.limit_to_log_dbm.has_value());
  EXPECT_FALSE(report.little_margin.has_value());
}

TEST(BusyLimitLog, ReportsTheLimitWhenFirstKnownThenOnlyOnAChangeOfThreeDecibelsOrMore) {
  BusyLimitLog log;
  const auto first = log.update(ChannelLevel{FREQ_CH1, -100});
  EXPECT_EQ(first.limit_to_log_dbm, -81);
  EXPECT_FALSE(log.update(ChannelLevel{FREQ_CH1, -100}).limit_to_log_dbm.has_value());
  EXPECT_FALSE(log.update(ChannelLevel{FREQ_CH1, -98}).limit_to_log_dbm.has_value()) << "2 dB";
  EXPECT_EQ(log.update(ChannelLevel{FREQ_CH1, -97}).limit_to_log_dbm, -78) << "3 dB from the logged -81";
  EXPECT_FALSE(log.update(ChannelLevel{FREQ_CH1, -98}).limit_to_log_dbm.has_value()) << "measured from the last log";
}

TEST(BusyLimitLog, DoesNotReportAChangeThatTheBoundsAbsorb) {
  BusyLimitLog log;
  EXPECT_EQ(log.update(ChannelLevel{FREQ_CH1, -113}).limit_to_log_dbm, -90);
  EXPECT_FALSE(log.update(ChannelLevel{FREQ_CH1, -108}).limit_to_log_dbm.has_value()) << "the limit stayed at -90";
}

TEST(BusyLimitLog, WarnsOncePerBootNamingTheChannelAndItsLevel) {
  BusyLimitLog log;
  const auto warning = log.update(ChannelLevel{FREQ_CH3, -88}).little_margin;
  ASSERT_TRUE(warning.has_value());
  EXPECT_EQ(warning->freq_hz, FREQ_CH3);
  EXPECT_EQ(warning->level_dbm, -88);
  EXPECT_FALSE(log.update(ChannelLevel{FREQ_CH3, -88}).little_margin.has_value());
  EXPECT_FALSE(log.update(ChannelLevel{FREQ_CH3, -80}).little_margin.has_value()) << "still once";
}

TEST(BusyLimitLog, DoesNotWarnForAQuietBoard) {
  BusyLimitLog log;
  EXPECT_FALSE(log.update(ChannelLevel{FREQ_CH1, -108}).little_margin.has_value());
  EXPECT_FALSE(log.update(ChannelLevel{FREQ_CH1, -89}).little_margin.has_value());
}

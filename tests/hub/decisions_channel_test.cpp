#include "decisions_channel.h"

#include "test_helpers.h"

#include <set>

using namespace esphome::home_io_control;

// ============================================================================
// Channel access decisions (ADR 0096): the channel of each check, the back-off after a busy one,
// when the access time is spent, which channel a forced send takes. Pure helpers in
// decisions_channel.h, tested without radio or clock; the random number is an argument.
// ============================================================================

namespace {

/// A band that is not 868 MHz, with invented air frequencies (a test fixture, not a real band): every
/// channel may wake a low-power receiver, and its fallback busy limit differs from the 868 MHz one.
IoBand test_band_2g4() {
  IoBand band;
  band.air_hz = {2450000000U, 2410000000U, 2475000000U};  // CH2, CH1, CH3
  band.wake_up_channels = ChannelSet::all();
  band.busy_limit_until_quiet_level_dbm = -70;
  return band;
}

}  // namespace

TEST(DecisionsChannel, The868BandHoldsTodaysConstants) {
  EXPECT_EQ(IO_868_BAND.air_hz, (std::array<uint32_t, 3>{FREQ_CH2, FREQ_CH1, FREQ_CH3}));
  EXPECT_EQ(IO_868_BAND.wake_up_channels, ChannelSet::only(FREQ_CH2));
  EXPECT_EQ(IO_868_BAND.busy_limit_until_quiet_level_dbm, CHANNEL_LIMIT_UNTIL_QUIET_LEVEL_DBM);
  EXPECT_EQ(IO_868_BAND.busy_limit_until_quiet_level_dbm, -85);
}

TEST(DecisionsChannel, AChannelSetKnowsItsMembersInChannelOrder) {
  const ChannelSet all = ChannelSet::all();
  EXPECT_EQ(all.count(), 3u);
  EXPECT_EQ(all.nth(0), FREQ_CH2);
  EXPECT_EQ(all.nth(1), FREQ_CH1);
  EXPECT_EQ(all.nth(2), FREQ_CH3);
  const ChannelSet one = ChannelSet::only(FREQ_CH3);
  EXPECT_EQ(one.count(), 1u);
  EXPECT_TRUE(one.contains(FREQ_CH3));
  EXPECT_FALSE(one.contains(FREQ_CH2));
  EXPECT_EQ(ChannelSet::only(123456).count(), 0u) << "a frequency that is no channel makes an empty set";
}

TEST(DecisionsChannel, OnThe868BandALowPowerRequestMayUseChannel2Only) {
  EXPECT_EQ(decisions::request_channels(true, IO_868_BAND, 0), ChannelSet::only(FREQ_CH2));
  EXPECT_EQ(decisions::request_channels(false, IO_868_BAND, 0), ChannelSet::all());
}

TEST(DecisionsChannel, ABandCanLetALowPowerRequestUseEveryChannel) {
  EXPECT_EQ(decisions::request_channels(true, test_band_2g4(), 0), ChannelSet::all());
}

TEST(DecisionsChannel, AChannelInsideTheGuardOfTheFrequencyToAvoidIsLeftOut) {
  const IoBand band = test_band_2g4();
  const ChannelSet near_ch2 = decisions::request_channels(false, band, 2450000000U);
  EXPECT_FALSE(near_ch2.contains(FREQ_CH2));
  EXPECT_EQ(near_ch2.count(), 2u);
  EXPECT_FALSE(
      decisions::request_channels(false, band, 2410000000U + decisions::WIFI_CHANNEL_GUARD_HZ).contains(FREQ_CH1))
      << "the guard's edge is inside";
  EXPECT_TRUE(
      decisions::request_channels(false, band, 2410000000U + decisions::WIFI_CHANNEL_GUARD_HZ + 1).contains(FREQ_CH1))
      << "one hertz beyond the guard is outside";
  EXPECT_FALSE(decisions::request_channels(true, band, 2475000000U).contains(FREQ_CH3)) << "low power too";
}

TEST(DecisionsChannel, AnExclusionThatWouldLeaveNoChannelIsDropped) {
  // The only wake-up channel of the 868 MHz layout, with a frequency to avoid that sits on it.
  EXPECT_EQ(decisions::request_channels(true, IO_868_BAND, FREQ_CH2), ChannelSet::only(FREQ_CH2));
}

TEST(DecisionsChannel, A2g4FrequencyExcludesNoChannelOfThe868Band) {
  EXPECT_EQ(decisions::request_channels(false, IO_868_BAND, 2437000000U), ChannelSet::all());
  EXPECT_EQ(decisions::request_channels(true, IO_868_BAND, 2437000000U), ChannelSet::only(FREQ_CH2));
}

TEST(DecisionsChannel, TheFirstCheckCanTakeAnyChannelOfTheSet) {
  std::set<uint32_t> seen;
  for (uint32_t random = 0; random < 3; random++)
    seen.insert(decisions::request_channel(ChannelSet::all(), /*avoid_freq=*/0, random));
  EXPECT_EQ(seen, (std::set<uint32_t>{FREQ_CH1, FREQ_CH2, FREQ_CH3}));
}

TEST(DecisionsChannel, ADrawOfZeroIsChannel2AsBeforeChannelAccess) {
  EXPECT_EQ(decisions::request_channel(ChannelSet::all(), 0, 0), FREQ_CH2);
}

TEST(DecisionsChannel, TheChannelToLeaveIsNeverChosenWhateverTheRandomNumber) {
  for (const uint32_t avoid : {FREQ_CH1, FREQ_CH2, FREQ_CH3}) {
    std::set<uint32_t> seen;
    for (uint32_t random = 0; random < 1000; random++) {
      const uint32_t freq = decisions::request_channel(ChannelSet::all(), avoid, random);
      EXPECT_NE(freq, avoid) << "random=" << random;
      seen.insert(freq);
    }
    EXPECT_EQ(seen.size(), 2u) << "both other channels stay reachable";
  }
}

TEST(DecisionsChannel, AFrequencyThatIsNoChannelExcludesNothing) {
  EXPECT_EQ(decisions::request_channel(ChannelSet::all(), 123456, 1), FREQ_CH1);
}

TEST(DecisionsChannel, ADrawStaysInsideTheSet) {
  const ChannelSet two = decisions::request_channels(false, test_band_2g4(), 2475000000U);  // CH3 out
  for (uint32_t random = 0; random < 100; random++)
    EXPECT_NE(decisions::request_channel(two, 0, random), FREQ_CH3) << "random=" << random;
}

TEST(DecisionsChannel, ASetOfOneChannelReturnsItEvenWhenItIsTheOneToLeave) {
  const ChannelSet one = ChannelSet::only(FREQ_CH2);
  EXPECT_EQ(decisions::request_channel(one, 0, 7), FREQ_CH2);
  EXPECT_EQ(decisions::request_channel(one, FREQ_CH2, 7), FREQ_CH2);
}

TEST(DecisionsChannel, ALowPowerRequestHasALongerAccessTime) {
  EXPECT_EQ(decisions::channel_access_budget_ms(false), CHANNEL_ACCESS_BUDGET_MS);
  EXPECT_EQ(decisions::channel_access_budget_ms(true), LOW_POWER_CHANNEL_ACCESS_BUDGET_MS);
  EXPECT_GT(LOW_POWER_CHANNEL_ACCESS_BUDGET_MS, CHANNEL_ACCESS_BUDGET_MS);
}

TEST(DecisionsChannel, TheBackOffCeilingGrowsWithEveryBusyCheckAndThenStays) {
  uint32_t previous = 0;
  for (uint8_t check = 0; check < decisions::BACKOFF_CEILINGS_MS.size(); check++) {
    EXPECT_GT(decisions::backoff_ceiling_ms(check), previous) << "check " << int(check);
    previous = decisions::backoff_ceiling_ms(check);
  }
  EXPECT_EQ(decisions::backoff_ceiling_ms(static_cast<uint8_t>(decisions::BACKOFF_CEILINGS_MS.size())), previous);
  EXPECT_EQ(decisions::backoff_ceiling_ms(200), previous) << "a late check uses the last ceiling";
  EXPECT_LE(previous, 42u) << "the longest back-off stays at about 40 ms";
}

TEST(DecisionsChannel, TheBackOffIsRandomBetweenOneMillisecondAndTheCeiling) {
  std::set<uint32_t> seen;
  for (uint32_t random = 0; random < 200; random++) {
    const auto backoff = decisions::next_backoff_ms(/*elapsed_ms=*/0, /*busy_checks=*/2, false, random);
    ASSERT_TRUE(backoff.has_value());
    EXPECT_GE(*backoff, 1u);
    EXPECT_LE(*backoff, decisions::backoff_ceiling_ms(2));
    seen.insert(*backoff);
  }
  EXPECT_EQ(seen.size(), decisions::backoff_ceiling_ms(2)) << "every value up to the ceiling is possible";
}

TEST(DecisionsChannel, AnAccessTimeThatIsSpentLeavesNoBackOff) {
  EXPECT_FALSE(decisions::next_backoff_ms(CHANNEL_ACCESS_BUDGET_MS, 0, false, 0).has_value());
  EXPECT_FALSE(decisions::next_backoff_ms(CHANNEL_ACCESS_BUDGET_MS + 50, 3, false, 0).has_value());
  EXPECT_TRUE(decisions::next_backoff_ms(CHANNEL_ACCESS_BUDGET_MS - 1, 0, false, 0).has_value());
}

TEST(DecisionsChannel, TheLastBackOffEndsAtTheLimitNotAfterIt) {
  // 10 ms left and a random draw that would wait 40: the wait is cut to the 10 ms that remain.
  const auto backoff = decisions::next_backoff_ms(CHANNEL_ACCESS_BUDGET_MS - 10, 4, false, 39);
  ASSERT_TRUE(backoff.has_value());
  EXPECT_EQ(*backoff, 10u);
}

TEST(DecisionsChannel, ALowPowerRequestKeepsCheckingUntilItsLongerLimit) {
  EXPECT_TRUE(decisions::next_backoff_ms(CHANNEL_ACCESS_BUDGET_MS + 50, 4, true, 0).has_value());
  EXPECT_FALSE(decisions::next_backoff_ms(LOW_POWER_CHANNEL_ACCESS_BUDGET_MS, 4, true, 0).has_value());
}

TEST(DecisionsChannel, TheElapsedTimeIsAnUnsignedDifferenceSoAWrapOfMillisIsHarmless) {
  const uint32_t begin = 0xFFFFFFF0u;
  const uint32_t now = 0x00000010u;  // 32 ms later, across the wrap
  EXPECT_TRUE(decisions::next_backoff_ms(now - begin, 0, false, 0).has_value());
  EXPECT_FALSE(decisions::next_backoff_ms(now - begin + CHANNEL_ACCESS_BUDGET_MS, 0, false, 0).has_value());
}

TEST(DecisionsChannel, AReadingAtTheThresholdIsBusy) {
  EXPECT_TRUE(decisions::channel_is_busy(-90, -90)) << "the radio's own check treats the threshold as busy";
  EXPECT_TRUE(decisions::channel_is_busy(-50, -90));
  EXPECT_FALSE(decisions::channel_is_busy(-91, -90));
}

TEST(DecisionsChannel, AForcedSendTakesTheQuietestChannel) {
  // Levels in REQUEST_CHANNELS order: CH2, CH1, CH3.
  const ChannelSet all = ChannelSet::all();
  EXPECT_EQ(decisions::forced_send_channel(all, {-50, -70, -60}), FREQ_CH1);
  EXPECT_EQ(decisions::forced_send_channel(all, {-80, -70, -60}), FREQ_CH2);
  EXPECT_EQ(decisions::forced_send_channel(all, {-50, -70, -95}), FREQ_CH3);
  EXPECT_EQ(decisions::forced_send_channel(all, {-60, -60, -60}), FREQ_CH2) << "a tie keeps channel 2";
}

TEST(DecisionsChannel, AForcedSendStaysInsideTheSet) {
  EXPECT_EQ(decisions::forced_send_channel(ChannelSet::only(FREQ_CH2), {-50, -95, -95}), FREQ_CH2);
  const ChannelSet without_ch1 = decisions::request_channels(false, test_band_2g4(), 2410000000U);
  EXPECT_EQ(decisions::forced_send_channel(without_ch1, {-50, -120, -60}), FREQ_CH3)
      << "CH1 is the quietest but outside the set";
}

TEST(DecisionsChannel, OnlyASendOntoAFreeChannelOrAFailedTransmitIsEvidenceWhenTheAnswerIsMissing) {
  using Status = decisions::ChannelAccessStatus;
  EXPECT_TRUE(decisions::silence_is_evidence(Status::SENT));
  EXPECT_FALSE(decisions::silence_is_evidence(Status::SENT_FORCED)) << "a collision explains the silence";
  EXPECT_FALSE(decisions::silence_is_evidence(Status::CHANNEL_BUSY)) << "it was never sent";
  EXPECT_TRUE(decisions::silence_is_evidence(Status::FAILED)) << "a failed transmit keeps its earlier meaning";
}

TEST(DecisionsChannel, OnlyASentRequestHasAnAnswerToListenFor) {
  using Status = decisions::ChannelAccessStatus;
  EXPECT_TRUE(decisions::request_was_sent(Status::SENT));
  EXPECT_TRUE(decisions::request_was_sent(Status::SENT_FORCED));
  EXPECT_FALSE(decisions::request_was_sent(Status::CHANNEL_BUSY));
  EXPECT_FALSE(decisions::request_was_sent(Status::FAILED));
}

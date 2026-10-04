#include <gtest/gtest.h>

#include "esphome/core/hal.h"

// ============================================================================
// TestClock test suite
// ============================================================================
// tests/include/esphome/core/hal.h's two clock modes: legacy (the default, bit-for-bit today's
// per-call counters) and manual (a single explicitly-moved clock, opt-in per test via
// ManualClock). See hal.h's own doc comments for why each exists.
// ============================================================================

using esphome::delay;
using esphome::micros;
using esphome::millis;
using esphome::test_clock::ManualClock;

namespace {

// The legacy counters are global and monotonic for the whole test binary (by design — see hal.h),
// so these tests only ever assert on *deltas* between calls, never on an absolute value.

TEST(TestClock, LegacyMillisAdvancesByOnePerCall) {
  const uint32_t first = millis();
  const uint32_t second = millis();
  EXPECT_EQ(second - first, 1u);
}

TEST(TestClock, LegacyMicrosAdvancesByOnePerCall) {
  const uint32_t first = micros();
  const uint32_t second = micros();
  EXPECT_EQ(second - first, 1u);
}

TEST(TestClock, LegacyDelayAndPeekDoNotMoveTheLegacyCounter) {
  const uint32_t before = millis();
  delay(5000);
  for (int i = 0; i < 10; i++)
    (void) esphome::test_clock::peek_ms();
  const uint32_t after = millis();
  EXPECT_EQ(after - before, 1u) << "delay() and peek_ms() must not consume a legacy tick";
}

TEST(TestClock, ManualClockStartsAtTheRequestedTimeAndHoldsUntilAdvanced) {
  ManualClock clock(1000);
  EXPECT_EQ(millis(), 1000u);
  EXPECT_EQ(millis(), 1000u) << "manual time doesn't move just by being read";
}

TEST(TestClock, ManualClockDefaultsAboveZeroSoLastSeenMsZeroStillMeansNever) {
  ManualClock clock;
  EXPECT_EQ(millis(), esphome::test_clock::DEFAULT_MANUAL_START_MS)
      << "production reads millis()==0 as \"never happened yet\" in places (e.g. last_seen_ms); "
         "a zero default start would make a fresh device's first update indistinguishable from that";
}

TEST(TestClock, ManualClockAdvanceMsMovesMillis) {
  ManualClock clock(1000);
  esphome::test_clock::advance_ms(500);
  EXPECT_EQ(millis(), 1500u);
}

TEST(TestClock, ManualClockAdvanceUsMovesMicrosAndMillisConsistently) {
  ManualClock clock(0);
  esphome::test_clock::advance_us(1500);
  EXPECT_EQ(micros(), 1500u);
  EXPECT_EQ(millis(), 1u) << "millis() must always be micros()/1000 -- one source of truth, not two counters";
}

TEST(TestClock, ManualClockSetMsJumpsToAnAbsoluteTime) {
  ManualClock clock(0);
  esphome::test_clock::set_ms(50000);
  EXPECT_EQ(millis(), 50000u);
}

TEST(TestClock, ManualClockDelayAdvancesAndStillRecords) {
  ManualClock clock(1000);
  esphome::test_hal::reset_delays();
  delay(250);
  EXPECT_EQ(millis(), 1250u);
  ASSERT_EQ(esphome::test_hal::recorded_delays().size(), 1u);
  EXPECT_EQ(esphome::test_hal::recorded_delays()[0], 250u);
}

TEST(TestClock, ManualClockWrapsAroundUint32MaxLikeRealMillis) {
  ManualClock clock(0);
  constexpr uint32_t near_max = 0xFFFFFFF0u;
  esphome::test_clock::set_ms(near_max);
  esphome::test_clock::advance_ms(0x20);
  EXPECT_EQ(millis(), near_max + 0x20u) << "uint32_t wraparound, exactly as a real device's millis() would do";
}

TEST(TestClock, ManualClockRestoresLegacyModeWhenItGoesOutOfScope) {
  const uint32_t before_scope = millis();
  {
    ManualClock clock(1000);
    EXPECT_TRUE(esphome::test_clock::is_manual());
  }
  EXPECT_FALSE(esphome::test_clock::is_manual());
  EXPECT_EQ(millis(), before_scope + 1u) << "the legacy counter must be exactly where it would have been "
                                            "without the manual excursion -- manual mode never touches it";
}

// Separate suite name (gtest convention: a *DeathTest suffix isolates death tests so they run
// before any other threads/state could make the fork unsafe).
TEST(TestClockDeathTest, SpinGuardAbortsOnAManualClockThatNeverAdvances) {
  EXPECT_DEATH(
      {
        ManualClock clock;
        for (;;)
          (void) millis();
      },
      "manual time never advanced");
}

TEST(TestClockDeathTest, AdvanceMsAbortsInLegacyMode) {
  ASSERT_FALSE(esphome::test_clock::is_manual()) << "sanity: this test must start legacy";
  EXPECT_DEATH({ esphome::test_clock::advance_ms(1); }, "");
}

TEST(TestClockDeathTest, SetMsAbortsInLegacyMode) {
  ASSERT_FALSE(esphome::test_clock::is_manual()) << "sanity: this test must start legacy";
  EXPECT_DEATH({ esphome::test_clock::set_ms(1000); }, "");
}

TEST(TestClockDeathTest, NestedManualClockAborts) {
  EXPECT_DEATH(
      {
        ManualClock outer;
        ManualClock inner;
      },
      "nested ManualClock");
}

}  // namespace

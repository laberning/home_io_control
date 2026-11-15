/// @file radio_preamble_hold_test.cpp
/// @brief Unit tests for PreambleHold, the idle receiver's renewable preamble hold.
/// @ingroup hioc_radio

#include "radio_preamble_hold.h"

#include "esphome/core/hal.h"

#include <gtest/gtest.h>

using namespace esphome;
using namespace esphome::home_io_control;

namespace {

constexpr PreambleHoldTiming TIMING{.slice_ms = 30, .redetect_ms = 4, .chain_ms = 300};

/// A scripted chip: whether a preamble is detected now, and how often the hold restarted RX.
struct FakeChip {
  bool preamble{false};
  int restarts{0};
};

/// One idle-hop consultation of `hold` against `chip`; the restart models the driver's retune, which
/// releases the hold.
bool consult(PreambleHold &hold, FakeChip &chip) {
  return hold.hold_renewing(
      millis(), [&] { return chip.preamble; },
      [&] {
        chip.restarts++;
        hold.release();
      });
}

}  // namespace

TEST(PreambleHold, NothingHeldWithoutAPreamble) {
  test_clock::ManualClock clock;
  PreambleHold hold(TIMING);
  FakeChip chip;
  EXPECT_FALSE(consult(hold, chip));
  EXPECT_FALSE(hold.active());
  EXPECT_FALSE(hold.hold(millis(), false));
}

TEST(PreambleHold, ARunningSliceAnswersWithoutReadingTheChip) {
  test_clock::ManualClock clock;
  PreambleHold hold(TIMING);
  FakeChip chip{.preamble = true};
  ASSERT_TRUE(consult(hold, chip));

  int reads = 0;
  test_clock::advance_ms(TIMING.slice_ms - 1);
  EXPECT_TRUE(hold.hold_renewing(
      millis(),
      [&] {
        reads++;
        return false;
      },
      [] {}));
  EXPECT_EQ(reads, 0) << "reading the chip costs an SPI transfer; a running slice needs none";
}

TEST(PreambleHold, RenewsAtTheSliceEndAndHoldsWhileThePreambleReturns) {
  test_clock::ManualClock clock;
  PreambleHold hold(TIMING);
  FakeChip chip{.preamble = true};
  ASSERT_TRUE(consult(hold, chip));

  test_clock::advance_ms(TIMING.slice_ms);
  chip.preamble = false;  // the restart cleared the detection
  EXPECT_TRUE(consult(hold, chip)) << "renew: the slice ended but the hop is kept here";
  EXPECT_EQ(chip.restarts, 1);

  test_clock::advance_ms(1);
  chip.preamble = true;  // detected again about a millisecond later
  EXPECT_TRUE(consult(hold, chip));
  EXPECT_TRUE(hold.active());
  test_clock::advance_ms(TIMING.slice_ms - 1);
  EXPECT_TRUE(consult(hold, chip)) << "a full new slice";
  EXPECT_EQ(chip.restarts, 1);
}

TEST(PreambleHold, ReleasesTheHopWhenThePreambleDoesNotReturn) {
  test_clock::ManualClock clock;
  PreambleHold hold(TIMING);
  FakeChip chip{.preamble = true};
  ASSERT_TRUE(consult(hold, chip));
  test_clock::advance_ms(TIMING.slice_ms);
  chip.preamble = false;
  ASSERT_TRUE(consult(hold, chip));

  test_clock::advance_ms(TIMING.redetect_ms - 1);
  EXPECT_TRUE(consult(hold, chip)) << "inside the re-detection window";
  test_clock::advance_ms(1);
  EXPECT_FALSE(consult(hold, chip)) << "window over";
  EXPECT_FALSE(consult(hold, chip)) << "and it stays over";
}

TEST(PreambleHold, ChainLimitBoundsTheWholeRunOfRenewals) {
  test_clock::ManualClock clock;
  PreambleHold hold(TIMING);
  FakeChip chip{.preamble = true};  // a detector that never stops

  const uint32_t start = millis();
  while (consult(hold, chip) && millis() - start < 10 * TIMING.chain_ms)
    test_clock::advance_ms(1);

  EXPECT_GE(millis() - start, TIMING.chain_ms);
  EXPECT_LT(millis() - start, TIMING.chain_ms + TIMING.slice_ms);
  EXPECT_GE(chip.restarts, 8) << "renewed through the run, not only at its end";
  EXPECT_FALSE(consult(hold, chip)) << "expired until released";
  EXPECT_EQ(chip.restarts, 9) << "no further restart once the chain is spent";
}

TEST(PreambleHold, ANewPreambleAfterARelease_StartsANewChain) {
  test_clock::ManualClock clock;
  PreambleHold hold(TIMING);
  FakeChip chip{.preamble = true};
  const uint32_t start = millis();
  while (consult(hold, chip) && millis() - start < 10 * TIMING.chain_ms)
    test_clock::advance_ms(1);
  hold.release();  // the hop retuned

  EXPECT_TRUE(consult(hold, chip));
  test_clock::advance_ms(TIMING.chain_ms - TIMING.slice_ms);
  EXPECT_TRUE(consult(hold, chip)) << "a fresh chain, not the spent one";
}

TEST(PreambleHold, ReleaseEndsARenewalInProgress) {
  test_clock::ManualClock clock;
  PreambleHold hold(TIMING);
  FakeChip chip{.preamble = true};
  ASSERT_TRUE(consult(hold, chip));
  test_clock::advance_ms(TIMING.slice_ms);
  chip.preamble = false;
  ASSERT_TRUE(consult(hold, chip));  // renewing, waiting for the preamble

  hold.release();
  EXPECT_FALSE(consult(hold, chip)) << "a retune forgets the wait for the old channel's preamble";
}

TEST(PreambleHold, WorksAcrossTheMillisWrap) {
  test_clock::ManualClock clock(0xFFFFFFFFu - 9);
  PreambleHold hold(TIMING);
  FakeChip chip{.preamble = true};
  ASSERT_TRUE(consult(hold, chip));

  test_clock::advance_ms(TIMING.slice_ms - 1);
  EXPECT_TRUE(consult(hold, chip));
  EXPECT_EQ(chip.restarts, 0) << "the wrap does not end the slice early";
  test_clock::advance_ms(1);
  chip.preamble = false;
  EXPECT_TRUE(consult(hold, chip));
  EXPECT_EQ(chip.restarts, 1) << "the renew happens on time";
  test_clock::advance_ms(TIMING.redetect_ms);
  EXPECT_FALSE(consult(hold, chip)) << "and the window is measured across the wrap too";
}

TEST(PreambleHold, PlainHoldEndsAtTheSliceAndStaysActiveUntilReleased) {
  // The blocking listens' use: no renewal, the caller resets RX when it is told to give up.
  test_clock::ManualClock clock;
  PreambleHold hold(TIMING);

  EXPECT_TRUE(hold.hold(millis(), true));
  test_clock::advance_ms(TIMING.slice_ms - 1);
  EXPECT_TRUE(hold.hold(millis(), true));
  test_clock::advance_ms(1);
  EXPECT_FALSE(hold.hold(millis(), true)) << "past the slice: noise or too long to be an answer";
  EXPECT_TRUE(hold.active());
  hold.release();
  EXPECT_FALSE(hold.active());
  EXPECT_TRUE(hold.hold(millis(), true)) << "a released hold starts afresh";
}

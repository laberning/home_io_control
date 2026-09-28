#include <gtest/gtest.h>

#include "esphome/core/hal.h"
#include "support/timed_release.h"

#include <vector>

// ============================================================================
// SendTimeline test suite
// ============================================================================
// The timed-release rule on its own, without a radio double: a reply anchored to a send index or
// to a send's bytes is due a fixed offset after that send started, and not before it happened.
// ============================================================================

using esphome::test_clock::ManualClock;

namespace {

void record(SendTimeline &timeline, const std::vector<uint8_t> &bytes) {
  timeline.record(bytes.data(), static_cast<uint8_t>(bytes.size()));
}

TEST(SendTimeline, IndexAnchorReleasesAtOffsetFromThatSendsStart) {
  ManualClock clock(1000);
  SendTimeline timeline;
  record(timeline, {0x01});
  esphome::test_clock::advance_ms(50);
  record(timeline, {0x02});

  EXPECT_EQ(timeline.release_us(TimedRelease{0, {}, 300}), 1000000u + 300u);
  EXPECT_EQ(timeline.release_us(TimedRelease{1, {}, 300}), 1050000u + 300u);
}

TEST(SendTimeline, ByteAnchorUsesTheFirstMatchingSend) {
  ManualClock clock(1000);
  SendTimeline timeline;
  record(timeline, {0xAA});
  esphome::test_clock::advance_ms(10);
  record(timeline, {0xBB, 0xCC});
  esphome::test_clock::advance_ms(10);
  record(timeline, {0xBB, 0xCC});

  EXPECT_EQ(timeline.release_us(TimedRelease{std::nullopt, {0xBB, 0xCC}, 7}), 1010000u + 7u);
}

TEST(SendTimeline, AnchorNotYetSentHasNoReleaseTime) {
  ManualClock clock;
  SendTimeline timeline;
  record(timeline, {0x01});

  EXPECT_FALSE(timeline.release_us(TimedRelease{1, {}, 0}).has_value());
  EXPECT_FALSE(timeline.release_us(TimedRelease{std::nullopt, {0x02}, 0}).has_value());
}

TEST(SendTimeline, LegacyClockRecordsZeroStartTimes) {
  SendTimeline timeline;
  record(timeline, {0x01});
  ASSERT_EQ(timeline.start_us().size(), 1u);
  EXPECT_EQ(timeline.start_us()[0], 0u);
}

TEST(SendTimeline, ClearForgetsEverySend) {
  ManualClock clock;
  SendTimeline timeline;
  record(timeline, {0x01});
  timeline.clear();

  EXPECT_TRUE(timeline.sent_data().empty());
  EXPECT_TRUE(timeline.start_us().empty());
  EXPECT_FALSE(timeline.release_us(TimedRelease{0, {}, 0}).has_value());
}

}  // namespace

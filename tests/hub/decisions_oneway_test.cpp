#include "decisions_oneway.h"

#include "test_helpers.h"

using namespace esphome::home_io_control;
using namespace test;

// ============================================================================
// One-way rules: burst de-duplication, background-poll deferral and burst start.
// ============================================================================
// Pure helpers in decisions_oneway.h, tested without radio or timing.

// ============================================================================
// 1W burst suppression — is_duplicate_1w_frame()
// ============================================================================
// The key includes the decoded intent, not just the command byte: a move and a
// stop are both CMD_EXECUTE and differ only in main0.

namespace {

decisions::OneWayDedupState dedup_state(const char *src, uint8_t cmd, bool has_intent, uint8_t main0,
                                        uint32_t timestamp, const uint8_t dst[NODE_ID_SIZE] = BROADCAST_DISCOVER_ALT) {
  decisions::OneWayDedupState state{src, cmd, has_intent, main0, 0x00, timestamp};
  memcpy(state.dst, dst, NODE_ID_SIZE);
  return state;
}

decisions::OneWayDedupState sequenced_state(const char *src, uint8_t main0, uint16_t sequence, uint32_t timestamp,
                                            const uint8_t dst[NODE_ID_SIZE] = BROADCAST_DISCOVER_ALT) {
  decisions::OneWayDedupState state = dedup_state(src, CMD_EXECUTE, true, main0, timestamp, dst);
  state.has_sequence = true;
  state.sequence = sequence;
  return state;
}

constexpr uint8_t ROLLER_SHUTTER_CLASS[NODE_ID_SIZE] = {0x00, 0x00, 0xBF};
constexpr uint8_t AWNING_CLASS[NODE_ID_SIZE] = {0x00, 0x00, 0xFF};
constexpr uint8_t VENETIAN_BLIND_CLASS[NODE_ID_SIZE] = {0x00, 0x00, 0x7F};

constexpr uint32_t DEDUP_WINDOW_MS = 2000;

}  // namespace

TEST(Decisions, OneWayDedupSameSequenceInsideWindowIsARepeat) {
  const auto last = sequenced_state("AABBCC", 0xC8, 0x1F44, 1000);
  EXPECT_TRUE(decisions::is_duplicate_1w_frame(last, sequenced_state("AABBCC", 0xC8, 0x1F44, 1040), DEDUP_WINDOW_MS));
}

TEST(Decisions, OneWayDedupNewSequenceIsANewPressInsideTheWindow) {
  const auto last = sequenced_state("AABBCC", 0xC8, 0x1F44, 1000);
  EXPECT_FALSE(decisions::is_duplicate_1w_frame(last, sequenced_state("AABBCC", 0xC8, 0x1F47, 1200), DEDUP_WINDOW_MS))
      << "same sender, command and intent 200 ms apart, but a new rolling sequence";
}

TEST(Decisions, OneWayDedupSameSequenceAfterTheWindowIsNew) {
  const auto last = sequenced_state("AABBCC", 0xC8, 0x1F44, 1000);
  EXPECT_FALSE(decisions::is_duplicate_1w_frame(last, sequenced_state("AABBCC", 0xC8, 0x1F44, 3000), DEDUP_WINDOW_MS));
}

TEST(Decisions, OneWayDedupSequenceRuleSurvivesTheMillisWrap) {
  const uint32_t before_wrap = UINT32_MAX - 10;
  const auto last = sequenced_state("AABBCC", 0xC8, 0x1F44, before_wrap);
  EXPECT_TRUE(decisions::is_duplicate_1w_frame(last, sequenced_state("AABBCC", 0xC8, 0x1F44, before_wrap + 50),
                                               DEDUP_WINDOW_MS));
}

TEST(Decisions, OneWayDedupOneSidedSequenceFallsBackToTheKey) {
  const auto last = sequenced_state("AABBCC", 0xC8, 0x1F44, 1000);
  const auto incoming = dedup_state("AABBCC", CMD_EXECUTE, true, 0xC8, 1100);
  EXPECT_TRUE(decisions::is_duplicate_1w_frame(last, incoming, DEDUP_WINDOW_MS));
}

TEST(Decisions, OneWayDedupOneTriggerSweptAcrossClassesIsOnePress) {
  // A sensor sends one trigger to several device classes, each class frame with its own sequence
  // (corpus: node 540B83, 0x6050 to 00043F and 0x6052 to 00047F in one burst).
  constexpr uint8_t HORIZONTAL_AWNING_CLASS[NODE_ID_SIZE] = {0x00, 0x04, 0x3F};
  constexpr uint8_t SECOND_CLASS[NODE_ID_SIZE] = {0x00, 0x04, 0x7F};
  const auto last = sequenced_state("540B83", 0xD1, 0x6050, 1000, HORIZONTAL_AWNING_CLASS);
  EXPECT_TRUE(decisions::is_duplicate_1w_frame(last, sequenced_state("540B83", 0xD1, 0x6052, 1100, SECOND_CLASS),
                                               DEDUP_WINDOW_MS))
      << "a new sequence to another class with the same intent is the same trigger";
  EXPECT_FALSE(decisions::is_duplicate_1w_frame(last, sequenced_state("540B83", 0xD2, 0x6052, 1100, SECOND_CLASS),
                                                DEDUP_WINDOW_MS))
      << "a different intent stays a new press";
}

TEST(Decisions, OneWayDedupFirstFrameIsNeverADuplicate) {
  const decisions::OneWayDedupState nothing_seen_yet{};
  const auto incoming = dedup_state("AABBCC", CMD_EXECUTE, true, 0xC8, 1000);

  EXPECT_FALSE(decisions::is_duplicate_1w_frame(nothing_seen_yet, incoming, DEDUP_WINDOW_MS))
      << "with no previous frame recorded the src IDs differ, so nothing can be suppressed";
}

TEST(Decisions, OneWayDedupSuppressesIdenticalRepeatInsideWindow) {
  const auto last = dedup_state("AABBCC", CMD_EXECUTE, true, 0xC8, 1000);
  const auto repeat = dedup_state("AABBCC", CMD_EXECUTE, true, 0xC8, 1040);

  EXPECT_TRUE(decisions::is_duplicate_1w_frame(last, repeat, DEDUP_WINDOW_MS))
      << "the 4x/40ms reliability burst must collapse into one logical press";
}

TEST(Decisions, OneWayDedupAllowsIdenticalRepeatAfterWindow) {
  const auto last = dedup_state("AABBCC", CMD_EXECUTE, true, 0xC8, 1000);
  const auto later = dedup_state("AABBCC", CMD_EXECUTE, true, 0xC8, 1000 + DEDUP_WINDOW_MS);

  EXPECT_FALSE(decisions::is_duplicate_1w_frame(last, later, DEDUP_WINDOW_MS))
      << "a genuine second press after the window is a new logical press";
}

TEST(Decisions, OneWayDedupDoesNotSuppressStopAfterMove) {
  // Both frames are CMD_EXECUTE; only main0 differs. Keying on the command byte alone would
  // discard the stop, losing the sender event, the optimistic clear, and the immediate poll.
  const auto move = dedup_state("AABBCC", CMD_EXECUTE, true, 0xC8, 1000);
  const auto stop = dedup_state("AABBCC", CMD_EXECUTE, true, POS_STOP, 1500);

  EXPECT_FALSE(decisions::is_duplicate_1w_frame(move, stop, DEDUP_WINDOW_MS))
      << "a stop pressed shortly after a move carries a different intent and must be processed";
}

TEST(Decisions, OneWayDedupDoesNotSuppressDifferentRemote) {
  const auto last = dedup_state("AABBCC", CMD_EXECUTE, true, 0xC8, 1000);
  const auto other_remote = dedup_state("DDEEFF", CMD_EXECUTE, true, 0xC8, 1040);

  EXPECT_FALSE(decisions::is_duplicate_1w_frame(last, other_remote, DEDUP_WINDOW_MS))
      << "two remotes pressed at once are two presses";
}

TEST(Decisions, OneWayDedupIgnoresIntentBytesWhenNoIntentWasDecoded) {
  // Commands without a decodable intent (e.g. write-private) carry stale main0/main1; the key
  // must fall back to src+cmd for them rather than comparing meaningless bytes.
  const auto last = dedup_state("AABBCC", CMD_WRITE_PRIVATE, false, 0x11, 1000);
  const auto repeat = dedup_state("AABBCC", CMD_WRITE_PRIVATE, false, 0x99, 1040);

  EXPECT_TRUE(decisions::is_duplicate_1w_frame(last, repeat, DEDUP_WINDOW_MS))
      << "intent-less commands dedup on src+cmd+dst, never on the meaningless intent bytes";
}

TEST(Decisions, OneWayDedupKeepsEachClassOfAnIntentLessSweep) {
  // A VELUX KLI GEAR press sends 0x2E to roller_shutter (00 00 BF) then awning (00 00 FF) ~600 ms
  // apart (issue #74). Each class it names is one a new controller must enroll on, so each is logged.
  const auto first_class = dedup_state("5A9E00", CMD_DISCOVER_ALT_REQ, false, 0x00, 1000, ROLLER_SHUTTER_CLASS);
  const auto second_class = dedup_state("5A9E00", CMD_DISCOVER_ALT_REQ, false, 0x00, 1600, AWNING_CLASS);

  EXPECT_FALSE(decisions::is_duplicate_1w_frame(first_class, second_class, DEDUP_WINDOW_MS))
      << "an intent-less frame to a different destination is not a repeat";
}

TEST(Decisions, OneWayDedupIgnoresDestinationForAnIntentBearingPress) {
  // Sender events and optimistic state hang off intent-bearing frames: one press must stay one
  // press even if a remote sends the same intent to more than one address.
  const auto to_all = dedup_state("AABBCC", CMD_EXECUTE, true, 0xC8, 1000, BROADCAST_DISCOVER_ALT);
  const auto to_class = dedup_state("AABBCC", CMD_EXECUTE, true, 0xC8, 1040, VENETIAN_BLIND_CLASS);

  EXPECT_TRUE(decisions::is_duplicate_1w_frame(to_all, to_class, DEDUP_WINDOW_MS))
      << "the destination must not split one intent-bearing press into two";
}

TEST(Decisions, OneWayDedupSurvivesMillisWrap) {
  const auto last = dedup_state("AABBCC", CMD_EXECUTE, true, 0xC8, 0xFFFFFF00);
  const auto after_wrap = dedup_state("AABBCC", CMD_EXECUTE, true, 0xC8, 0x00000040);

  EXPECT_TRUE(decisions::is_duplicate_1w_frame(last, after_wrap, DEDUP_WINDOW_MS))
      << "unsigned subtraction must keep the window correct across the millis() wrap";
}

// ============================================================================
// Background-poll deferral — defer_background_poll_for_1w_activity()
// ============================================================================

TEST(Decisions, DeferHoldsBackgroundPollDuringRemoteActivity) {
  EXPECT_TRUE(decisions::defer_background_poll_for_1w_activity(/*next_op_is_background=*/true,
                                                               /*first_1w_activity_ms=*/1000,
                                                               /*last_1w_activity_ms=*/1000,
                                                               /*now=*/1100, /*quiet_ms=*/700,
                                                               /*max_defer_ms=*/5000))
      << "a poll must not seize the radio while the remote that triggered it is still transmitting";
}

TEST(Decisions, DeferReleasesBackgroundPollAfterQuietPeriod) {
  EXPECT_FALSE(decisions::defer_background_poll_for_1w_activity(true, 1000, 1000, 1700, 700, 5000))
      << "the hold must expire exactly at the quiet period, not linger";
}

TEST(Decisions, DeferNeverHoldsBackControlOperations) {
  EXPECT_FALSE(decisions::defer_background_poll_for_1w_activity(/*next_op_is_background=*/false,
                                                                /*first_1w_activity_ms=*/1000,
                                                                /*last_1w_activity_ms=*/1000,
                                                                /*now=*/1100, /*quiet_ms=*/700,
                                                                /*max_defer_ms=*/5000))
      << "a user command must not wait on a remote the user may not even own";
}

TEST(Decisions, DeferIsInactiveBeforeAnyRemoteIsHeard) {
  EXPECT_FALSE(decisions::defer_background_poll_for_1w_activity(true, /*first_1w_activity_ms=*/0,
                                                                /*last_1w_activity_ms=*/0,
                                                                /*now=*/500, /*quiet_ms=*/700,
                                                                /*max_defer_ms=*/5000))
      << "0 means no 1W frame has ever been seen, not 'heard one at boot'";
}

TEST(Decisions, DeferHoldsThroughRepeatedReArmingUnderTheCap) {
  // Burst started at 0; a frame every 500ms keeps re-arming the quiet-period hold, but the cap
  // (5000ms from burst start) has not been reached yet.
  EXPECT_TRUE(decisions::defer_background_poll_for_1w_activity(true, /*first_1w_activity_ms=*/0,
                                                               /*last_1w_activity_ms=*/4500,
                                                               /*now=*/4600, /*quiet_ms=*/700,
                                                               /*max_defer_ms=*/5000))
      << "sustained sub-quiet_ms traffic must keep deferring right up to the cap";
}

TEST(Decisions, DeferReleasesAtTheCapEvenMidBurst) {
  // Same sustained traffic, but now() has reached the cap measured from first_1w_activity_ms —
  // the poll must be let through even though the most recent frame is still within quiet_ms.
  EXPECT_FALSE(decisions::defer_background_poll_for_1w_activity(true, /*first_1w_activity_ms=*/0,
                                                                /*last_1w_activity_ms=*/4900,
                                                                /*now=*/5000, /*quiet_ms=*/700,
                                                                /*max_defer_ms=*/5000))
      << "sustained 1W traffic must not starve a background poll past max_defer_ms";
}

// ============================================================================
// Burst start-of-window detection — oneway_burst_started_fresh()
// ============================================================================

TEST(Decisions, BurstStartsFreshWhenNoneSeenYet) {
  EXPECT_TRUE(decisions::oneway_burst_started_fresh(/*last_1w_activity_ms=*/0, /*now=*/500, /*quiet_ms=*/700));
}

TEST(Decisions, BurstStartsFreshAfterQuietGap) {
  EXPECT_TRUE(decisions::oneway_burst_started_fresh(/*last_1w_activity_ms=*/1000, /*now=*/1700, /*quiet_ms=*/700))
      << "a gap reaching quiet_ms means the previous burst already ended";
}

TEST(Decisions, BurstContinuesWithinQuietGap) {
  EXPECT_FALSE(decisions::oneway_burst_started_fresh(/*last_1w_activity_ms=*/1000, /*now=*/1699, /*quiet_ms=*/700))
      << "a frame arriving just inside quiet_ms extends the current burst rather than starting a new one";
}

// ============================================================================
// Frames heard during an exchange — keep_overheard_frame_for_oneway_handling()
// ============================================================================

TEST(Decisions, OverheardOneWayFrameIsKeptForTheRemoteHandling) {
  IoFrame f{};
  init_frame(f, /*is_2w=*/false, /*start=*/true, /*end=*/true, /*low_power=*/false);
  EXPECT_TRUE(decisions::keep_overheard_frame_for_oneway_handling(f));
}

TEST(Decisions, OverheardTwoWayFrameIsNotKept) {
  IoFrame f{};
  init_frame(f, /*is_2w=*/true, /*start=*/true, /*end=*/true, /*low_power=*/false);
  EXPECT_FALSE(decisions::keep_overheard_frame_for_oneway_handling(f));
}

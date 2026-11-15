/// @file oneway_receiver_test.cpp
/// @brief Tests for OneWayReceiver (oneway_receiver.h): burst window, duplicate filter, pairing-gesture
/// sighting and sender allowlist. Needs no hub, radio or clock: every call carries its own timestamp.

#include "oneway_receiver.h"

#include "test_helpers.h"

using namespace esphome::home_io_control;

namespace {

const uint8_t REMOTE_ID[NODE_ID_SIZE] = {0xAA, 0xBB, 0xCC};
const uint8_t OTHER_REMOTE_ID[NODE_ID_SIZE] = {0x11, 0x22, 0x33};
const uint8_t ROLLER_CLASS[NODE_ID_SIZE] = {0x00, 0x00, 0xBF};
const uint8_t AWNING_CLASS[NODE_ID_SIZE] = {0x00, 0x00, 0xFF};

constexpr uint32_t T0 = 100000;
constexpr uint8_t MOVE_CLOSE = 0xC8;
constexpr uint8_t MOVE_STOP = 0xD2;

IoFrame execute_frame(uint8_t main0, const uint8_t *src = REMOTE_ID, const uint8_t *dst = ROLLER_CLASS) {
  IoFrame f{};
  init_frame(f, /*is_2w=*/false, /*start=*/true, /*end=*/true, /*low_power=*/false);
  set_dst(f, dst);
  set_src(f, src);
  const uint8_t payload[4] = {ORIGINATOR_USER_REMOTE, 0x41, main0, 0x00};
  set_cmd(f, CMD_EXECUTE, payload, sizeof(payload));
  return f;
}

/// A frame without a decodable intent, sent to a device class (one step of a class sweep).
IoFrame sweep_frame(const uint8_t *dst) {
  IoFrame f{};
  init_frame(f, /*is_2w=*/false, /*start=*/true, /*end=*/true, /*low_power=*/false);
  set_dst(f, dst);
  set_src(f, REMOTE_ID);
  set_cmd(f, CMD_WRITE_PRIVATE, nullptr, 0);
  return f;
}

IoFrame pairing_gesture_frame(const uint8_t *src = REMOTE_ID) {
  IoFrame f{};
  init_frame(f, /*is_2w=*/false, /*start=*/true, /*end=*/true, /*low_power=*/false);
  set_dst(f, BROADCAST_DISCOVER_ALT);
  set_src(f, src);
  set_cmd(f, CMD_WRITE_PRIVATE, nullptr, 0);
  return f;
}

bool new_press(OneWayReceiver &rx, const IoFrame &frame, uint32_t now) {
  return rx.is_new_press(frame, decode_1w_frame(frame), now);
}

}  // namespace

// ============================================================================
// Burst window
// ============================================================================

TEST(OneWayReceiver, NoActivityNeverDefersAPoll) {
  const OneWayReceiver rx;
  EXPECT_EQ(rx.last_activity_ms(), 0u);
  EXPECT_FALSE(rx.defers_background_poll(true, T0));
}

TEST(OneWayReceiver, ActivityDefersABackgroundPollForTheQuietPeriod) {
  OneWayReceiver rx;
  rx.note_activity(T0);

  EXPECT_TRUE(rx.defers_background_poll(true, T0 + ONEWAY_QUIET_PERIOD_MS - 1));
  EXPECT_FALSE(rx.defers_background_poll(true, T0 + ONEWAY_QUIET_PERIOD_MS));
}

TEST(OneWayReceiver, ActivityNeverDefersAUserCommand) {
  OneWayReceiver rx;
  rx.note_activity(T0);

  EXPECT_FALSE(rx.defers_background_poll(false, T0 + 1));
}

TEST(OneWayReceiver, EachFrameExtendsTheQuietPeriod) {
  OneWayReceiver rx;
  rx.note_activity(T0);
  rx.note_activity(T0 + 600);

  EXPECT_TRUE(rx.defers_background_poll(true, T0 + 600 + ONEWAY_QUIET_PERIOD_MS - 1));
  EXPECT_EQ(rx.last_activity_ms(), T0 + 600);
}

TEST(OneWayReceiver, SustainedTrafficReleasesThePollAtTheDeferCap) {
  OneWayReceiver rx;
  // A frame every 600 ms, inside the quiet period, so the burst never ends by itself.
  uint32_t t = T0;
  for (; t < T0 + ONEWAY_POLL_DEFER_CAP_MS; t += 600)
    rx.note_activity(t);

  EXPECT_TRUE(rx.defers_background_poll(true, T0 + ONEWAY_POLL_DEFER_CAP_MS - 1));
  EXPECT_FALSE(rx.defers_background_poll(true, T0 + ONEWAY_POLL_DEFER_CAP_MS))
      << "the cap counts from the burst's start, not from the latest frame";
}

TEST(OneWayReceiver, AFrameAfterAFullQuietPeriodStartsAFreshBurstWindow) {
  OneWayReceiver rx;
  rx.note_activity(T0);
  // Well past the cap since the first burst began, but the gap since its last frame exceeds the quiet
  // period, so the next frame opens a new window instead of inheriting the old start.
  const uint32_t later = T0 + ONEWAY_POLL_DEFER_CAP_MS + 1000;
  rx.note_activity(later);

  EXPECT_TRUE(rx.defers_background_poll(true, later + 1));
}

TEST(OneWayReceiver, BurstWindowSurvivesTheMillisWrap) {
  OneWayReceiver rx;
  const uint32_t before_wrap = UINT32_MAX - 100;
  rx.note_activity(before_wrap);
  rx.note_activity(before_wrap + 300);  // wraps to a small number

  EXPECT_TRUE(rx.defers_background_poll(true, before_wrap + 300 + 100));
  EXPECT_FALSE(rx.defers_background_poll(true, before_wrap + 300 + ONEWAY_QUIET_PERIOD_MS));
}

TEST(OneWayReceiver, NoteFrameCountsAsActivityEvenForARepeat) {
  OneWayReceiver rx;
  const IoFrame frame = execute_frame(MOVE_CLOSE);
  rx.note_frame(frame, T0, -60);
  ASSERT_TRUE(new_press(rx, frame, T0));

  rx.note_frame(frame, T0 + 40, -60);
  EXPECT_FALSE(new_press(rx, frame, T0 + 40));

  EXPECT_EQ(rx.last_activity_ms(), T0 + 40);
}

// ============================================================================
// Duplicate filter
// ============================================================================

TEST(OneWayReceiver, TheFirstPressIsNew) {
  OneWayReceiver rx;
  EXPECT_TRUE(new_press(rx, execute_frame(MOVE_CLOSE), T0));
}

TEST(OneWayReceiver, ARepeatInsideTheWindowIsNotANewPress) {
  OneWayReceiver rx;
  const IoFrame frame = execute_frame(MOVE_CLOSE);
  ASSERT_TRUE(new_press(rx, frame, T0));

  EXPECT_FALSE(new_press(rx, frame, T0 + 40));
  EXPECT_FALSE(new_press(rx, frame, T0 + ONEWAY_DEDUP_WINDOW_MS - 1));
}

TEST(OneWayReceiver, TheSamePressAfterTheWindowIsNew) {
  OneWayReceiver rx;
  const IoFrame frame = execute_frame(MOVE_CLOSE);
  ASSERT_TRUE(new_press(rx, frame, T0));

  EXPECT_TRUE(new_press(rx, frame, T0 + ONEWAY_DEDUP_WINDOW_MS));
}

TEST(OneWayReceiver, AStopRightAfterAMoveIsANewPress) {
  OneWayReceiver rx;
  ASSERT_TRUE(new_press(rx, execute_frame(MOVE_CLOSE), T0));

  EXPECT_TRUE(new_press(rx, execute_frame(MOVE_STOP), T0 + 100))
      << "move and stop share CMD_EXECUTE; the decoded intent tells them apart";
}

TEST(OneWayReceiver, ARepeatedPressFromAnotherRemoteIsANewPress) {
  OneWayReceiver rx;
  ASSERT_TRUE(new_press(rx, execute_frame(MOVE_CLOSE), T0));

  EXPECT_TRUE(new_press(rx, execute_frame(MOVE_CLOSE, OTHER_REMOTE_ID), T0 + 10));
}

TEST(OneWayReceiver, EveryClassOfASweepIsKept) {
  OneWayReceiver rx;
  EXPECT_TRUE(new_press(rx, sweep_frame(ROLLER_CLASS), T0));
  EXPECT_TRUE(new_press(rx, sweep_frame(AWNING_CLASS), T0 + 300));
  EXPECT_FALSE(new_press(rx, sweep_frame(AWNING_CLASS), T0 + 340)) << "but a repeat of one class is dropped";
}

TEST(OneWayReceiver, ADroppedRepeatDoesNotMoveTheWindow) {
  OneWayReceiver rx;
  const IoFrame frame = execute_frame(MOVE_CLOSE);
  ASSERT_TRUE(new_press(rx, frame, T0));
  ASSERT_FALSE(new_press(rx, frame, T0 + 1500));

  // A held button resends for longer than the window; the window counts from the logged press.
  EXPECT_TRUE(new_press(rx, frame, T0 + ONEWAY_DEDUP_WINDOW_MS));
}

// ============================================================================
// Pairing-gesture sighting
// ============================================================================

TEST(OneWayReceiver, NoSightingBeforeAGesture) {
  const OneWayReceiver rx;
  EXPECT_EQ(rx.recent_pairing_sighting().seen_ms, 0u);
}

TEST(OneWayReceiver, APairingGestureIsRememberedWithItsRssi) {
  OneWayReceiver rx;
  rx.note_frame(pairing_gesture_frame(), T0, -57);

  const RecentOneWayPairingSighting &sighting = rx.recent_pairing_sighting();
  EXPECT_EQ(sighting.seen_ms, T0);
  EXPECT_EQ(sighting.rssi, -57);
  EXPECT_EQ(sighting.cmd, CMD_WRITE_PRIVATE);
  EXPECT_EQ(memcmp(sighting.src, REMOTE_ID, NODE_ID_SIZE), 0);
  EXPECT_EQ(memcmp(sighting.dst, BROADCAST_DISCOVER_ALT, NODE_ID_SIZE), 0);
}

TEST(OneWayReceiver, ARepeatedGestureRefreshesTheSighting) {
  OneWayReceiver rx;
  rx.note_frame(pairing_gesture_frame(), T0, -57);
  rx.note_frame(pairing_gesture_frame(), T0 + 3000, -49);

  EXPECT_EQ(rx.recent_pairing_sighting().seen_ms, T0 + 3000);
  EXPECT_EQ(rx.recent_pairing_sighting().rssi, -49);
}

TEST(OneWayReceiver, AnOrdinaryPressLeavesTheSightingAlone) {
  OneWayReceiver rx;
  rx.note_frame(pairing_gesture_frame(), T0, -57);
  rx.note_frame(execute_frame(MOVE_CLOSE), T0 + 500, -40);

  EXPECT_EQ(rx.recent_pairing_sighting().seen_ms, T0);
  EXPECT_EQ(rx.recent_pairing_sighting().rssi, -57);
}

// ============================================================================
// Sender allowlist
// ============================================================================

TEST(OneWayReceiver, EmptyAllowlistExposesNobody) {
  const OneWayReceiver rx;
  EXPECT_FALSE(rx.is_exposed("AABBCC"));
}

TEST(OneWayReceiver, OnlyListedSendersAreExposed) {
  OneWayReceiver rx;
  rx.add_exposed_sender("AABBCC");
  rx.add_exposed_sender("9D6085");

  EXPECT_TRUE(rx.is_exposed("AABBCC"));
  EXPECT_TRUE(rx.is_exposed("9D6085"));
  EXPECT_FALSE(rx.is_exposed("112233"));
}

// ============================================================================
// Frames heard during an exchange
// ============================================================================

namespace {
/// Everything the receiver keeps, taken out oldest first.
std::vector<KeptOneWayFrame> drain_kept(OneWayReceiver &rx) {
  std::vector<KeptOneWayFrame> all;
  KeptOneWayFrame k;
  while (rx.pop_kept(k))
    all.push_back(k);
  return all;
}
}  // namespace

TEST(OneWayReceiver, KeptFramesComeBackOldestFirstWithTheirArrivalAndRssi) {
  OneWayReceiver rx;
  rx.keep_overheard(execute_frame(MOVE_CLOSE), T0, -80);
  rx.keep_overheard(execute_frame(MOVE_STOP), T0 + 40, -75);

  const auto kept = drain_kept(rx);
  ASSERT_EQ(kept.size(), 2u);
  EXPECT_EQ(decode_1w_frame(kept[0].frame).main0, MOVE_CLOSE);
  EXPECT_EQ(kept[0].heard_ms, T0);
  EXPECT_EQ(kept[0].rssi_dbm, -80);
  EXPECT_EQ(decode_1w_frame(kept[1].frame).main0, MOVE_STOP);
  EXPECT_EQ(kept[1].heard_ms, T0 + 40);
}

TEST(OneWayReceiver, TakingTheKeptFramesEmptiesThem) {
  OneWayReceiver rx;
  rx.keep_overheard(execute_frame(MOVE_CLOSE), T0, -80);
  ASSERT_TRUE(rx.has_kept());

  EXPECT_EQ(drain_kept(rx).size(), 1u);
  EXPECT_FALSE(rx.has_kept());
  KeptOneWayFrame untouched;
  untouched.heard_ms = 123;
  EXPECT_FALSE(rx.pop_kept(untouched));
  EXPECT_EQ(untouched.heard_ms, 123u) << "a failed pop leaves the output alone";
}

TEST(OneWayReceiver, AFifthKeptFrameDropsTheOldest) {
  OneWayReceiver rx;
  for (uint32_t i = 0; i < ONEWAY_KEPT_FRAMES_MAX + 1; i++)
    rx.keep_overheard(execute_frame(MOVE_CLOSE), T0 + i, -80);

  const auto kept = drain_kept(rx);
  ASSERT_EQ(kept.size(), ONEWAY_KEPT_FRAMES_MAX);
  EXPECT_EQ(kept.front().heard_ms, T0 + 1) << "the first frame was dropped";
  EXPECT_EQ(kept.back().heard_ms, T0 + ONEWAY_KEPT_FRAMES_MAX);
}

TEST(OneWayReceiver, TheKeptRingKeepsArrivalOrderAfterWrappingSeveralTimes) {
  OneWayReceiver rx;
  const uint32_t total = ONEWAY_KEPT_FRAMES_MAX * 2 + 3;  // wraps the ring more than once
  for (uint32_t i = 0; i < total; i++)
    rx.keep_overheard(execute_frame(MOVE_CLOSE), T0 + i, static_cast<int16_t>(-100 + i));

  const auto kept = drain_kept(rx);
  ASSERT_EQ(kept.size(), ONEWAY_KEPT_FRAMES_MAX);
  for (uint32_t i = 0; i < ONEWAY_KEPT_FRAMES_MAX; i++) {
    const uint32_t expect = total - ONEWAY_KEPT_FRAMES_MAX + i;
    EXPECT_EQ(kept[i].heard_ms, T0 + expect);
    EXPECT_EQ(kept[i].rssi_dbm, static_cast<int16_t>(-100 + expect));
  }
}

TEST(OneWayReceiver, TheKeptRingRefillsAfterBeingTaken) {
  OneWayReceiver rx;
  // Leave the head mid-ring, then empty it, so the refill starts from a non-zero head.
  for (uint32_t i = 0; i < 3; i++)
    rx.keep_overheard(execute_frame(MOVE_CLOSE), T0 + i, -80);
  ASSERT_EQ(drain_kept(rx).size(), 3u);

  for (uint32_t i = 0; i < ONEWAY_KEPT_FRAMES_MAX; i++)
    rx.keep_overheard(execute_frame(MOVE_STOP), (T0 + 1000) + i, -70);

  const auto kept = drain_kept(rx);
  ASSERT_EQ(kept.size(), ONEWAY_KEPT_FRAMES_MAX);
  for (uint32_t i = 0; i < ONEWAY_KEPT_FRAMES_MAX; i++)
    EXPECT_EQ(kept[i].heard_ms, (T0 + 1000) + i);
  EXPECT_FALSE(rx.has_kept());
}

TEST(OneWayReceiver, ATwoWayFrameIsNotKept) {
  OneWayReceiver rx;
  IoFrame two_way{};
  init_frame(two_way, /*is_2w=*/true, /*start=*/true, /*end=*/true, /*low_power=*/false);
  set_src(two_way, REMOTE_ID);
  set_dst(two_way, ROLLER_CLASS);
  set_cmd(two_way, CMD_PRIVATE_RESP, nullptr, 0);

  rx.keep_overheard(two_way, T0, -80);

  EXPECT_FALSE(rx.has_kept());
}

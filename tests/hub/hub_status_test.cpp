/// @file hub_status_test.cpp
/// @brief Tests for the inbound status handling of hub_status.cpp: update_device_status_ and process_received_packet_
/// decode a status reply, the optimistic overlay a decoded observation supersedes, the remaining travel time and the
/// INFO2 and error responses.
///
/// The other subjects are in the hub_status_*_test.cpp files.

#include "hub_core.h"
#include "hub_internal.h"
#include "radio_interface.h"
#include "proto_frame.h"
#include "esphome/core/application.h"
#include "esphome/core/component.h"

#include "corpus_test_helpers.h"
#include "test_helpers.h"
#include "stubs/radio_test_common.h"
#include "log_capture.h"

using namespace esphome::home_io_control;
using test::encode_device_metadata;
using test::TestableHubComponent;

TEST(HubStatus, PrivateResponseMarkerTargetUsesCurrentWhenStopped) {
  TestableHubComponent comp;
  comp.add_device("ABC123");

  IoFrame frame{};
  init_frame(frame, true, false, false, false);
  uint8_t own[3] = {0xC0, 0xFF, 0xEE};
  uint8_t device[3] = {0xAB, 0xC1, 0x23};
  set_dst(frame, own);
  set_src(frame, device);
  uint8_t payload[8] = {test::STATUS_BYTE_AT_REST, 0x00, POS_UNKNOWN, 0x00, 0x64, 0x00, 0x00, 0x00};
  ASSERT_TRUE(set_cmd(frame, CMD_PRIVATE_RESP, payload, sizeof(payload)));

  comp.update_device_status_(frame);

  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  EXPECT_FLOAT_EQ(dev->position, 50.0f) << "valid current should decode to 50 percent";
  EXPECT_FLOAT_EQ(dev->target, 50.0f) << "marker target should normalize to current when stopped";
  EXPECT_TRUE(dev->is_stopped) << "matching normalized target/current should remain stopped";
}

namespace {

/// A trusted 0x04 status reply from ABC123: at rest or moving, target and current raw.
IoFrame make_status_reply(bool stopped, uint16_t target_raw, uint16_t current_raw) {
  IoFrame frame{};
  init_frame(frame, true, false, false, false);
  uint8_t own[3] = {0xC0, 0xFF, 0xEE};
  uint8_t device[3] = {0xAB, 0xC1, 0x23};
  set_dst(frame, own);
  set_src(frame, device);
  uint8_t payload[8] = {static_cast<uint8_t>(stopped ? test::STATUS_BYTE_AT_REST : test::STATUS_BYTE_MOVING),
                        0x00,
                        static_cast<uint8_t>(target_raw >> 8),
                        static_cast<uint8_t>(target_raw & 0xFF),
                        static_cast<uint8_t>(current_raw >> 8),
                        static_cast<uint8_t>(current_raw & 0xFF),
                        0x00,
                        0x00};
  EXPECT_TRUE(set_cmd(frame, CMD_PRIVATE_RESP, payload, sizeof(payload)));
  return frame;
}

}  // namespace

namespace {

/// A 0x04 from ABC123 that says moving (target 100 %, current 50 %) with @p remaining_s at bytes 6-7.
IoFrame make_moving_reply(uint16_t remaining_s) {
  IoFrame frame = make_status_reply(false, 0xC800, 0x6400);
  frame.data[6] = static_cast<uint8_t>(remaining_s >> 8);
  frame.data[7] = static_cast<uint8_t>(remaining_s & 0xFF);
  return frame;
}

/// Feed @p frame to a tracked ABC123 (no configured interval) and return the scheduled delay.
uint32_t next_poll_delay_after(TestableHubComponent &comp, const IoFrame &frame, bool trust_position = true) {
  comp.begin_status_poll_tracking_("ABC123", 0);
  comp.poll_policy_.set_next_update("ABC123", 0);
  comp.update_device_status_(frame, trust_position);
  return comp.poll_policy_.get_next_update("ABC123") - esphome::millis();
}

}  // namespace

// --- remaining travel time (status reply bytes 6-7) -------------------------------------------

TEST(HubStatus, RemainingTimeIsReadAsTwoBytes) {
  esphome::test_clock::ManualClock clock(50000);
  TestableHubComponent comp;
  comp.add_device("ABC123");
  comp.set_device_status_poll_interval("ABC123", 400000);
  EXPECT_EQ(next_poll_delay_after(comp, make_moving_reply(0x0100)), 256000u + 1000u)
      << "256 s left: byte 6 is the high byte of the count";
}

TEST(HubStatus, RemainingTimePlusASecondShortensTheDefaultDelay) {
  esphome::test_clock::ManualClock clock(50000);
  TestableHubComponent comp;
  comp.add_device("ABC123");
  EXPECT_EQ(next_poll_delay_after(comp, make_moving_reply(1)), 2000u);
  EXPECT_EQ(next_poll_delay_after(comp, make_moving_reply(10)), DEFAULT_SETTLE_POLL_DELAY_MS)
      << "a long remaining time never stretches the default";
}

// Corpus: a Somfy Izymo answers an EXECUTE with `06 00` at bytes 6-7, a tilt ack with `8D 00`.
// The bytes of a command's reply are not a remaining time.
TEST(HubStatus, RemainingTimeIsIgnoredInTheReplyToACommand) {
  esphome::test_clock::ManualClock clock(50000);
  TestableHubComponent comp;
  comp.add_device("ABC123");
  EXPECT_EQ(next_poll_delay_after(comp, make_moving_reply(1), /*trust_position=*/false), DEFAULT_SETTLE_POLL_DELAY_MS);
  EXPECT_EQ(next_poll_delay_after(comp, make_moving_reply(0x0600), /*trust_position=*/false),
            DEFAULT_SETTLE_POLL_DELAY_MS);
  EXPECT_FALSE(comp.get_device("ABC123")->reports_remaining_time)
      << "a command's reply never teaches the hub that the device counts down";
}

TEST(HubStatus, AllBitsSetMeansNoRemainingTime) {
  esphome::test_clock::ManualClock clock(50000);
  TestableHubComponent comp;
  comp.add_device("ABC123");
  EXPECT_EQ(next_poll_delay_after(comp, make_moving_reply(0xFFFF)), DEFAULT_SETTLE_POLL_DELAY_MS);
}

// Issue #178: a Somfy cover counts down and reports 0 in its last second of travel.
TEST(HubStatus, ZeroFromADeviceThatCountsDownMeansAboutToStop) {
  esphome::test_clock::ManualClock clock(50000);
  TestableHubComponent comp;
  comp.add_device("ABC123");
  next_poll_delay_after(comp, make_moving_reply(3));
  EXPECT_TRUE(comp.get_device("ABC123")->reports_remaining_time);
  EXPECT_EQ(next_poll_delay_after(comp, make_moving_reply(0)), 1000u);
}

// A device that has never sent a count gets the default cadence from a 0: it may not provide one.
TEST(HubStatus, ZeroFromADeviceThatNeverCountsIsNoRemainingTime) {
  esphome::test_clock::ManualClock clock(50000);
  TestableHubComponent comp;
  comp.add_device("ABC123");
  EXPECT_EQ(next_poll_delay_after(comp, make_moving_reply(0)), DEFAULT_SETTLE_POLL_DELAY_MS);
  EXPECT_FALSE(comp.get_device("ABC123")->reports_remaining_time);
}

// A STOPPED off target waits the default delay for its confirming poll, even from a device that
// counts down: classify_stopped_report() relies on that gap to tell a slow motor start from rest.
TEST(HubStatus, ZeroInAStoppedReportKeepsTheDefaultDelay) {
  esphome::test_clock::ManualClock clock(50000);
  TestableHubComponent comp;
  comp.add_device("ABC123");
  next_poll_delay_after(comp, make_moving_reply(3));
  ASSERT_TRUE(comp.get_device("ABC123")->reports_remaining_time);
  EXPECT_EQ(next_poll_delay_after(comp, make_status_reply(true, 0xC800, 0x6400)), DEFAULT_SETTLE_POLL_DELAY_MS);
  EXPECT_STREQ(detail::describe_motion(*comp.get_device("ABC123")), "settling");
}

TEST(HubStatus, StoppedOffTargetWithAFollowUpPollSettlesOnce) {
  esphome::test_clock::ManualClock clock(50000);
  TestableHubComponent comp;
  comp.add_device("ABC123");
  comp.begin_status_poll_tracking_("ABC123", 0);

  comp.update_device_status_(make_status_reply(true, 0xC800, 0x6400));

  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  EXPECT_FLOAT_EQ(dev->target, 100.0f);
  EXPECT_FLOAT_EQ(dev->position, 50.0f);
  EXPECT_FALSE(dev->is_stopped) << "the first off-target stopped report waits for one more poll";
  EXPECT_TRUE(dev->stop_candidate.armed());
  EXPECT_STREQ(detail::describe_motion(*dev), "settling");
  EXPECT_TRUE(comp.poll_policy_.is_tracking_active("ABC123", esphome::millis())) << "the confirming poll is still due";
  EXPECT_NE(comp.poll_policy_.get_next_update("ABC123"), 0u);
}

TEST(HubStatus, StoppedOffTargetMarksItsConfirmingPollAsASettlingRead) {
  esphome::test_clock::ManualClock clock(50000);
  TestableHubComponent comp;
  comp.add_device("ABC123");
  comp.begin_status_poll_tracking_("ABC123", 0);

  comp.update_device_status_(make_status_reply(true, 0xC800, 0x6400));

  EXPECT_TRUE(comp.poll_policy_.take_confirm_stopped("ABC123"))
      << "the next poll decides whether the cover rests here, so it gets the settling plan";
}

TEST(HubStatus, AMovingReportBeforeTheConfirmingPollDropsItsSettlingMark) {
  esphome::test_clock::ManualClock clock(50000);
  TestableHubComponent comp;
  comp.add_device("ABC123");
  comp.begin_status_poll_tracking_("ABC123", 0);

  comp.update_device_status_(make_status_reply(true, 0xC800, 0x6400));
  comp.update_device_status_(make_moving_reply(10));  // moved again, e.g. by another controller

  ASSERT_TRUE(comp.poll_policy_.is_tracking_active("ABC123", esphome::millis()));
  EXPECT_FALSE(comp.poll_policy_.take_confirm_stopped("ABC123"))
      << "the next poll looks at a moving cover: a progress read, not a settling one";
}

TEST(HubStatus, ReportsThatNeedNoConfirmationLeaveNoSettlingMark) {
  esphome::test_clock::ManualClock clock(50000);
  TestableHubComponent comp;
  comp.add_device("ABC123");
  comp.begin_status_poll_tracking_("ABC123", 0);

  comp.update_device_status_(make_moving_reply(10));
  EXPECT_FALSE(comp.poll_policy_.take_confirm_stopped("ABC123")) << "a moving report is a progress look";
  comp.update_device_status_(make_status_reply(true, 0x6400, 0x6400));
  EXPECT_FALSE(comp.poll_policy_.take_confirm_stopped("ABC123")) << "stopped on target needs no confirmation";
}

TEST(HubStatus, StoppedOffTargetWithNoPollToConfirmByLeavesNoSettlingMark) {
  esphome::test_clock::ManualClock clock(50000);
  TestableHubComponent comp;
  comp.add_device("ABC123");  // not tracked

  comp.update_device_status_(make_status_reply(true, 0xC800, 0x6400));

  EXPECT_FALSE(comp.poll_policy_.take_confirm_stopped("ABC123"));
}

TEST(HubStatus, SecondIdenticalStoppedOffTargetReportIsAtRestAndEndsPolling) {
  esphome::test_clock::ManualClock clock(50000);
  TestableHubComponent comp;
  comp.add_device("ABC123");
  comp.begin_status_poll_tracking_("ABC123", 0);

  comp.update_device_status_(make_status_reply(true, 0xC800, 0x6400));
  esphome::test_clock::advance_ms(3000);
  comp.update_device_status_(make_status_reply(true, 0xC800, 0x6400));

  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  EXPECT_TRUE(dev->is_stopped) << "two reports at the same place: at rest off target";
  EXPECT_TRUE(dev->stop_candidate.armed()) << "kept, so a later poll at the same place confirms at once";
  EXPECT_FALSE(comp.poll_policy_.is_tracking_active("ABC123", esphome::millis())) << "nothing left to poll for";
}

TEST(HubStatus, StoppedOffTargetThatMovesOnKeepsTracking) {
  esphome::test_clock::ManualClock clock(50000);
  TestableHubComponent comp;
  comp.add_device("ABC123");
  comp.begin_status_poll_tracking_("ABC123", 0);

  comp.update_device_status_(make_status_reply(true, 0xC800, 0x6400));
  esphome::test_clock::advance_ms(3000);
  comp.update_device_status_(make_status_reply(true, 0xC800, 0x7800));

  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  EXPECT_FALSE(dev->is_stopped) << "a changed position means it was not at rest: settle again";
  EXPECT_FLOAT_EQ(dev->stop_candidate.position, 60.0f) << "the newer report is the one to confirm";
  EXPECT_TRUE(comp.poll_policy_.is_tracking_active("ABC123", esphome::millis()));
}

TEST(HubStatus, SettlingDeviceWhoseConfirmingPollNeverComesBackIsAtRest) {
  // The confirming poll goes unanswered until the failure ladder no longer fits in the window: the
  // device's last word was "stopped", so it stands rather than leaving the cover moving for good.
  esphome::test_clock::ManualClock clock(50000);
  TestableHubComponent comp;
  comp.add_device("ABC123");
  comp.begin_status_poll_tracking_("ABC123", 0);
  comp.update_device_status_(make_status_reply(true, 0xC800, 0x6400));
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  ASSERT_TRUE(detail::is_settling(*dev));
  int notified = 0;
  comp.register_device_callback([&](const std::string &, const IoDevice &) { notified++; });

  esphome::test_clock::advance_ms(MAX_TRACKED_STATUS_POLL_WINDOW_MS);
  ASSERT_EQ(comp.poll_policy_.on_exchange_failed("ABC123", false, esphome::millis()), 0u);
  comp.settle_ended_status_tracking_();

  EXPECT_TRUE(dev->is_stopped);
  EXPECT_FALSE(detail::is_settling(*dev));
  EXPECT_EQ(notified, 1) << "the cover is told, so it goes idle";
}

TEST(HubStatus, SettlingDeviceWhoseConfirmingPollFallsPastTheWindowIsAtRest) {
  // A long configured interval puts the confirming poll after the tracking deadline, so it is
  // dropped instead of sent; the device's own "stopped" then stands.
  esphome::test_clock::ManualClock clock(50000);
  TestableHubComponent comp;
  comp.add_device("ABC123");
  comp.poll_policy_.set_interval("ABC123", 60000);
  comp.begin_status_poll_tracking_("ABC123", 0);
  esphome::test_clock::advance_ms(MAX_TRACKED_STATUS_POLL_WINDOW_MS - 30000);
  comp.update_device_status_(make_status_reply(true, 0xC800, 0x6400));
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  ASSERT_TRUE(detail::is_settling(*dev));

  esphome::test_clock::advance_ms(61000);
  EXPECT_EQ(comp.poll_policy_.pop_due_device(esphome::millis()), std::nullopt) << "past the deadline: dropped";
  comp.settle_ended_status_tracking_();

  EXPECT_TRUE(dev->is_stopped);
}

// Runs the tracking window out with a failed poll and settles what ended.
static void end_tracking_window(TestableHubComponent &comp, const std::string &id) {
  esphome::test_clock::advance_ms(MAX_TRACKED_STATUS_POLL_WINDOW_MS);
  ASSERT_EQ(comp.poll_policy_.on_exchange_failed(id, false, esphome::millis()), 0u);
  comp.settle_ended_status_tracking_();
}

TEST(HubStatus, EndedTrackingTakesAMovingDeviceAsAtRest) {
  // The device last reported "moving" and every poll since failed: with no poll left the cover would
  // show movement for good, so it is shown at rest at the last reported position.
  esphome::test_clock::ManualClock clock(50000);
  TestableHubComponent comp;
  comp.add_device("ABC123");
  comp.begin_status_poll_tracking_("ABC123", 0);
  comp.update_device_status_(make_status_reply(false, 0xC800, 0x6400));
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  const float position = dev->position;
  const float target = dev->target;
  int notified = 0;
  comp.register_device_callback([&](const std::string &, const IoDevice &) { notified++; });

  end_tracking_window(comp, "ABC123");

  EXPECT_TRUE(dev->is_stopped);
  EXPECT_FLOAT_EQ(dev->position, position) << "the last reported position stands";
  EXPECT_FLOAT_EQ(dev->target, target);
  EXPECT_EQ(notified, 1) << "the cover is told, so it goes idle";
}

TEST(HubStatus, EndedTrackingTakesAHubCommandedMoveAsAtRest) {
  esphome::test_clock::ManualClock clock(50000);
  TestableHubComponent comp;
  comp.add_device("ABC123");
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  dev->is_stopped = true;
  dev->optimistic.target = 0.0f;
  dev->optimistic.motion = OptimisticState::Motion::MOVING;
  ASSERT_FALSE(effective_is_stopped(*dev));
  comp.begin_status_poll_tracking_("ABC123", 0);
  int notified = 0;
  comp.register_device_callback([&](const std::string &, const IoDevice &) { notified++; });

  end_tracking_window(comp, "ABC123");

  EXPECT_TRUE(effective_is_stopped(*dev)) << "the prediction is withdrawn";
  EXPECT_EQ(dev->optimistic.motion, OptimisticState::Motion::NONE);
  EXPECT_EQ(notified, 1);
}

TEST(HubStatus, EndedTrackingClearsTheMovingEvidenceSoTheNextRequestWakesTheDevice) {
  esphome::test_clock::ManualClock clock(50000);
  TestableHubComponent comp;
  comp.add_device("ABC123");
  comp.begin_status_poll_tracking_("ABC123", 0);
  comp.update_device_status_(make_status_reply(false, 0xC800, 0x6400));
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  note_moving_evidence(*dev, esphome::millis());
  ASSERT_NE(dev->last_moving_evidence_ms, 0u);

  end_tracking_window(comp, "ABC123");

  EXPECT_EQ(dev->last_moving_evidence_ms, 0u) << "a resting device is read as asleep again";
}

TEST(HubStatus, EndedTrackingLeavesADeviceThatReportedStoppedAlone) {
  esphome::test_clock::ManualClock clock(50000);
  TestableHubComponent comp;
  comp.add_device("ABC123");
  comp.begin_status_poll_tracking_("ABC123", 0);
  comp.update_device_status_(make_status_reply(true, 0xC800, 0xC800));
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  ASSERT_TRUE(dev->is_stopped);
  int notified = 0;
  comp.register_device_callback([&](const std::string &, const IoDevice &) { notified++; });

  end_tracking_window(comp, "ABC123");

  EXPECT_TRUE(dev->is_stopped);
  EXPECT_EQ(notified, 0) << "nothing changed, nobody is told";
}

TEST(HubStatus, EndedTrackingLeavesAHeaterAlone) {
  esphome::test_clock::ManualClock clock(50000);
  TestableHubComponent comp;
  comp.add_device("ABC123");
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  dev->type = DeviceType::HEAT_PUMP;
  dev->is_stopped = false;
  comp.begin_status_poll_tracking_("ABC123", 0);
  int notified = 0;
  comp.register_device_callback([&](const std::string &, const IoDevice &) { notified++; });

  end_tracking_window(comp, "ABC123");

  EXPECT_FALSE(dev->is_stopped);
  EXPECT_EQ(notified, 0);
}

TEST(HubStatus, EndedTrackingLeavesAHeatLevelHeaterAlone) {
  // An exterior heater takes its heat level in the position field but never moves.
  esphome::test_clock::ManualClock clock(50000);
  TestableHubComponent comp;
  comp.add_device("ABC123");
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  dev->type = DeviceType::EXTERIOR_HEATING;
  dev->is_stopped = false;
  comp.begin_status_poll_tracking_("ABC123", 0);
  int notified = 0;
  comp.register_device_callback([&](const std::string &, const IoDevice &) { notified++; });

  end_tracking_window(comp, "ABC123");

  EXPECT_FALSE(dev->is_stopped);
  EXPECT_EQ(notified, 0);
}

TEST(HubStatus, StoppedOffTargetWithoutAFollowUpPollIsAtRest) {
  // Nothing tracks the device (e.g. an untracked one-off poll), so nothing would ever confirm the
  // report: reading it as moving would leave the cover moving for good.
  TestableHubComponent comp;
  comp.add_device("ABC123");

  comp.update_device_status_(make_status_reply(true, 0xC800, 0x6400));

  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  EXPECT_FLOAT_EQ(dev->target, 100.0f) << "valid target should decode to 100 percent";
  EXPECT_FLOAT_EQ(dev->position, 50.0f) << "valid current should decode to 50 percent";
  EXPECT_TRUE(dev->is_stopped) << "the device's own flag stands when no poll can confirm it";
}

TEST(HubStatus, UntrackedMovingReadOpensTheWindowAndFollowsTheMoveToItsEnd) {
  // The read shortly after start-up has no window: the moving answer opens one, so the cover is
  // followed until the device reports stopped.
  esphome::test_clock::ManualClock clock(50000);
  TestableHubComponent comp;
  comp.add_device("ABC123");
  ASSERT_FALSE(comp.poll_policy_.is_tracking_active("ABC123", esphome::millis()));

  comp.update_device_status_(make_status_reply(false, 0xC800, 0x3200));

  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  EXPECT_FALSE(dev->is_stopped);
  EXPECT_TRUE(comp.poll_policy_.is_tracking_active("ABC123", esphome::millis()));
  EXPECT_NE(comp.poll_policy_.get_next_update("ABC123"), 0u) << "the next poll is scheduled";

  esphome::test_clock::advance_ms(3000);
  comp.update_device_status_(make_status_reply(true, 0xC800, 0xC800));
  EXPECT_TRUE(dev->is_stopped);
  EXPECT_FALSE(comp.poll_policy_.is_tracking_active("ABC123", esphome::millis()));
  EXPECT_EQ(comp.poll_policy_.get_next_update("ABC123"), 0u);
}

TEST(HubStatus, UntrackedMovingStatusUpdateSchedulesAPoll) {
  esphome::test_clock::ManualClock clock(50000);
  TestableHubComponent comp;
  comp.add_device("054E17");

  IoFrame f{};
  init_frame(f, true, false, true, false);
  uint8_t src[3] = {0x05, 0x4E, 0x17};
  uint8_t dst[3] = {0xC0, 0xFF, 0xEE};
  set_src(f, src);
  set_dst(f, dst);
  uint8_t payload[11] = {test::STATUS_BYTE_MOVING, 0x00, 0x00, 0x00, 0x00, 0xC8, 0x00, 0x32, 0x00, 0x00, 0x00};
  set_cmd(f, CMD_STATUS_UPDATE, payload, sizeof(payload));

  comp.update_device_status_(f);

  EXPECT_TRUE(comp.poll_policy_.is_tracking_active("054E17", esphome::millis()));
  EXPECT_NE(comp.poll_policy_.get_next_update("054E17"), 0u);
}

TEST(HubStatus, MovingReplyAfterTheWindowExpiredSchedulesNoPollAndTheCoverIsAtRest) {
  esphome::test_clock::ManualClock clock(50000);
  TestableHubComponent comp;
  comp.add_device("ABC123");
  comp.begin_status_poll_tracking_("ABC123", 0);
  comp.update_device_status_(make_status_reply(false, 0xC800, 0x6400));
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  const float position = dev->position;
  esphome::test_clock::advance_ms(MAX_TRACKED_STATUS_POLL_WINDOW_MS + 1000);

  comp.update_device_status_(make_status_reply(false, 0xC800, 0x6400));

  EXPECT_EQ(comp.poll_policy_.get_next_update("ABC123"), 0u) << "no further poll";
  EXPECT_FALSE(comp.poll_policy_.is_tracking_active("ABC123", esphome::millis()));
  comp.settle_ended_status_tracking_();
  EXPECT_TRUE(dev->is_stopped);
  EXPECT_FLOAT_EQ(dev->position, position);
  EXPECT_EQ(comp.poll_policy_.pop_ended_device(), std::nullopt) << "settled once";
}

TEST(HubStatus, StuckMovingDeviceIsAtRestWhenItsWindowRunsOutAndAFreshReadOpensANewOne) {
  esphome::test_clock::ManualClock clock(50000);
  TestableHubComponent comp;
  comp.add_device("ABC123");
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);

  comp.update_device_status_(make_status_reply(false, 0xC800, 0x6400));  // a read opens the window
  ASSERT_TRUE(comp.poll_policy_.is_tracking_active("ABC123", esphome::millis()));
  const uint32_t deadline = comp.poll_policy_.get_poll_deadline("ABC123");
  for (int i = 0; i < 3; i++) {
    esphome::test_clock::advance_ms(60000);
    comp.update_device_status_(make_status_reply(false, 0xC800, 0x6400));  // every reply still moving
  }
  EXPECT_EQ(comp.poll_policy_.get_poll_deadline("ABC123"), deadline) << "replies never extend the window";

  esphome::test_clock::advance_ms(MAX_TRACKED_STATUS_POLL_WINDOW_MS);
  comp.update_device_status_(make_status_reply(false, 0xC800, 0x6400));
  comp.settle_ended_status_tracking_();
  EXPECT_TRUE(dev->is_stopped) << "at rest at expiry";
  EXPECT_EQ(comp.poll_policy_.get_next_update("ABC123"), 0u);

  comp.update_device_status_(make_status_reply(false, 0xC800, 0x6400));  // a fresh read, not an own follow-up
  EXPECT_FALSE(dev->is_stopped);
  EXPECT_TRUE(comp.poll_policy_.is_tracking_active("ABC123", esphome::millis()));
  EXPECT_NE(comp.poll_policy_.get_next_update("ABC123"), 0u);
}

TEST(HubStatus, MovingReportOpensNoWindowForAHeater) {
  esphome::test_clock::ManualClock clock(50000);
  TestableHubComponent comp;
  comp.add_device("ABC123");
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  test::set_kind(*dev, DeviceType::ELECTRICAL_HEATER, 0);

  comp.update_device_status_(make_status_reply(false, 0xC800, 0x6400));

  EXPECT_FALSE(comp.poll_policy_.is_tracking_active("ABC123", esphome::millis()));
  EXPECT_EQ(comp.poll_policy_.get_next_update("ABC123"), 0u);
}

TEST(HubStatus, MovingReportOpensNoWindowForAKindWithoutAnExecutePosition) {
  esphome::test_clock::ManualClock clock(50000);
  TestableHubComponent comp;
  comp.add_device("ABC123");
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  test::set_kind(*dev, DeviceType::BEACON, 0);
  ASSERT_FALSE(detail::known_device_reports_motion_through_status(*dev));

  comp.update_device_status_(make_status_reply(false, 0xC800, 0x6400));

  EXPECT_FALSE(comp.poll_policy_.is_tracking_active("ABC123", esphome::millis()));
  EXPECT_EQ(comp.poll_policy_.get_next_update("ABC123"), 0u);
}

TEST(HubStatus, UntrackedStoppedOffTargetReportOpensNoWindow) {
  esphome::test_clock::ManualClock clock(50000);
  TestableHubComponent comp;
  comp.add_device("ABC123");

  comp.update_device_status_(make_status_reply(true, 0xC800, 0x6400));

  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  EXPECT_TRUE(dev->is_stopped) << "a stopped answer is final";
  EXPECT_FALSE(comp.poll_policy_.is_tracking_active("ABC123", esphome::millis()));
  EXPECT_EQ(comp.poll_policy_.get_poll_deadline("ABC123"), 0u);
}

TEST(HubStatus, MovingReportForgetsTheStopCandidate) {
  esphome::test_clock::ManualClock clock(50000);
  TestableHubComponent comp;
  comp.add_device("ABC123");
  comp.begin_status_poll_tracking_("ABC123", 0);

  comp.update_device_status_(make_status_reply(true, 0xC800, 0x6400));
  comp.update_device_status_(make_status_reply(false, 0xC800, 0x6400));

  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  EXPECT_FALSE(dev->stop_candidate.armed());
  EXPECT_STREQ(detail::describe_motion(*dev), "moving");
}

TEST(HubStatus, StopAckOnACoverAtRestOffTargetDoesNotSetItMovingAgain) {
  // A STOP pressed on a cover already accepted at rest off its target: its ack ("stopped") is held
  // against that accepted rest, and the settle poll reporting the same place confirms at once, so
  // the cover never shows a direction again.
  esphome::test_clock::ManualClock clock(50000);
  TestableHubComponent comp;
  comp.add_device("ABC123");
  comp.begin_status_poll_tracking_("ABC123", 0);
  comp.update_device_status_(make_status_reply(true, 0xC800, 0x6400));
  comp.update_device_status_(make_status_reply(true, 0xC800, 0x6400));
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  ASSERT_TRUE(dev->is_stopped);

  comp.begin_status_poll_tracking_("ABC123", 0);
  comp.update_device_status_(make_status_reply(true, 0x0000, 0x0000), /*trust_position=*/false);
  EXPECT_TRUE(dev->is_stopped) << "the ack's stale positions are the rest already accepted";
  comp.begin_status_poll_tracking_("ABC123", 0);  // the command path re-arms the window

  comp.update_device_status_(make_status_reply(true, 0xC800, 0x6400));
  EXPECT_TRUE(dev->is_stopped) << "same place again: no settling round";
  EXPECT_FALSE(comp.poll_policy_.is_tracking_active("ABC123", esphome::millis()));
}

TEST(HubStatus, FirstReportAfterACommandToANewTargetDoesNotConfirmAnOldStop) {
  // At rest off target, then sent to 0 %: a motor that has not started yet answers "stopped, target
  // 0 %, current 50 %". The remembered stop had target 100 %, so it cannot confirm this one.
  esphome::test_clock::ManualClock clock(50000);
  TestableHubComponent comp;
  comp.add_device("ABC123");
  comp.begin_status_poll_tracking_("ABC123", 0);
  comp.update_device_status_(make_status_reply(true, 0xC800, 0x6400));
  comp.update_device_status_(make_status_reply(true, 0xC800, 0x6400));

  comp.begin_status_poll_tracking_("ABC123", 0);
  comp.update_device_status_(make_status_reply(true, 0xC800, 0x6400), /*trust_position=*/false);
  // The "stopped" ack ends the window; the command path re-arms it (arm_execute_confirmation_poll_()).
  comp.begin_status_poll_tracking_("ABC123", 0);
  comp.update_device_status_(make_status_reply(true, 0x0000, 0x6400));

  EXPECT_FALSE(comp.get_device("ABC123")->is_stopped) << "settling: the next poll shows whether it moves";
  EXPECT_TRUE(comp.poll_policy_.is_tracking_active("ABC123", esphome::millis()));
}

TEST(HubStatus, HeaterReplyWithSetpointAndReadingApartIsNeverSettling) {
  // A heater's target and current are a setpoint and a reading; their gap is no movement, so the
  // settle loop must not run for it.
  esphome::test_clock::ManualClock clock(50000);
  TestableHubComponent comp;
  comp.add_device("ABC123");
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  test::set_kind(*dev, DeviceType::ELECTRICAL_HEATER, 0);  // heating-level heater: no motion
  comp.begin_status_poll_tracking_("ABC123", 0);

  comp.update_device_status_(make_status_reply(false, 0xC800, 0x6400));

  EXPECT_TRUE(dev->is_stopped);
  EXPECT_FALSE(dev->stop_candidate.armed());
  EXPECT_FALSE(comp.poll_policy_.is_tracking_active("ABC123", esphome::millis()));
}

TEST(HubStatus, UnsolicitedStatusUpdateStoppedOffTargetSettlesThenConfirms) {
  esphome::test_clock::ManualClock clock(50000);
  TestableHubComponent comp;
  comp.add_device("054E17");
  comp.begin_status_poll_tracking_("054E17", 0);

  IoFrame f{};
  init_frame(f, true, false, true, false);
  uint8_t src[3] = {0x05, 0x4E, 0x17};
  uint8_t dst[3] = {0xC0, 0xFF, 0xEE};
  set_src(f, src);
  set_dst(f, dst);
  // Stopped, target [5..6] = 0xC800 (100 %), current [7..8] = 0xC755 (171 raw short).
  uint8_t payload[11] = {test::STATUS_BYTE_AT_REST, 0x00, 0x00, 0x00, 0x00, 0xC8, 0x00, 0xC7, 0x55, 0x00, 0x00};
  set_cmd(f, CMD_STATUS_UPDATE, payload, sizeof(payload));

  comp.update_device_status_(f);
  auto *dev = comp.get_device("054E17");
  ASSERT_NE(dev, nullptr);
  EXPECT_FALSE(dev->is_stopped) << "settling on the 0x71 path too";
  EXPECT_TRUE(comp.poll_policy_.is_tracking_active("054E17", esphome::millis()));

  comp.update_device_status_(f);
  EXPECT_TRUE(dev->is_stopped);
  EXPECT_FALSE(comp.poll_policy_.is_tracking_active("054E17", esphome::millis()));
}

TEST(HubStatus, SettlingReportClearsMovingEvidence) {
  // At rest a VELUX SSL hears only the wake-up preamble, so the confirming poll must lead with it:
  // the wake belief follows the device's "stopped", not the hub's "settling" (issue #151).
  esphome::test_clock::ManualClock clock(50000);
  TestableHubComponent comp;
  comp.add_device("ABC123");
  comp.begin_status_poll_tracking_("ABC123", 0);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  note_moving_evidence(*dev, esphome::millis());

  comp.update_device_status_(make_status_reply(true, 0xC800, 0x6400));

  ASSERT_FALSE(dev->is_stopped);
  EXPECT_EQ(dev->last_moving_evidence_ms, 0u);
}

// ============================================================================
// Optimistic-overlay supersede rule (a decoded observation replaces a prediction, per axis)
// ============================================================================

// §7.7 — a trusted status observation supersedes the position prediction.
TEST(HubStatus, TrustedStatusObservationSupersedesPositionPrediction) {
  TestableHubComponent comp;
  comp.add_device("ABC123", {DeviceType::ROLLER_SHUTTER, 0});
  ASSERT_TRUE(comp.apply_optimistic_target("ABC123", 25.0f));
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  ASSERT_EQ(dev->optimistic.motion, OptimisticState::Motion::MOVING);

  IoFrame frame{};
  init_frame(frame, true, false, false, false);
  uint8_t own[3] = {0xC0, 0xFF, 0xEE};
  uint8_t device[3] = {0xAB, 0xC1, 0x23};
  set_dst(frame, own);
  set_src(frame, device);
  // stopped, target=0xC800 (100%), current=0xC800 (100%)
  uint8_t payload[8] = {test::STATUS_BYTE_AT_REST, 0x00, 0xC8, 0x00, 0xC8, 0x00, 0x00, 0x00};
  ASSERT_TRUE(set_cmd(frame, CMD_PRIVATE_RESP, payload, sizeof(payload)));

  comp.update_device_status_(frame);

  EXPECT_EQ(dev->optimistic.target, UNKNOWN_POSITION) << "a decoded position supersedes the position prediction";
  EXPECT_EQ(dev->optimistic.motion, OptimisticState::Motion::NONE);
  EXPECT_FLOAT_EQ(dev->position, 100.0f);
  EXPECT_FLOAT_EQ(effective_target(*dev), 100.0f) << "effective now follows the observation";
}

// §7.8 — a trusted extended status observation supersedes the tilt prediction.
TEST(HubStatus, TrustedExtendedStatusObservationSupersedesTiltPrediction) {
  TestableHubComponent comp;
  comp.add_device("ABC123", {DeviceType::INTERIOR_VENETIAN_BLIND, 0});
  ASSERT_TRUE(comp.apply_optimistic_tilt("ABC123", 70.0f));
  ASSERT_TRUE(comp.apply_optimistic_target("ABC123", 25.0f));
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);

  IoFrame frame{};
  init_frame(frame, true, false, false, false);
  uint8_t own[3] = {0xC0, 0xFF, 0xEE};
  uint8_t device[3] = {0xAB, 0xC1, 0x23};
  set_dst(frame, own);
  set_src(frame, device);
  // stopped, target/current 0xC800, then the extended tilt block: selector at [12], raw 0x6400 -> 50%.
  uint8_t payload[15] = {test::STATUS_BYTE_AT_REST, 0x00, 0xC8, 0x00, 0xC8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                         STATUS_TILT_SELECTOR,      0x64, 0x00};
  ASSERT_TRUE(set_cmd(frame, CMD_PRIVATE_RESP, payload, sizeof(payload)));

  comp.update_device_status_(frame);

  EXPECT_EQ(dev->optimistic.tilt, UNKNOWN_POSITION) << "a decoded tilt supersedes the tilt prediction";
  EXPECT_FLOAT_EQ(dev->tilt, 50.0f);
  EXPECT_EQ(dev->optimistic.target, UNKNOWN_POSITION) << "the decoded position also supersedes the position prediction";
}

// §7.9 — regression guard: an execute ack decoded with trust_position=false clears *nothing*.
TEST(HubStatus, ExecuteAckWithoutTrustedPositionSupersedesNothing) {
  TestableHubComponent comp;
  comp.add_device("ABC123", {DeviceType::INTERIOR_VENETIAN_BLIND, 0});
  ASSERT_TRUE(comp.apply_optimistic_target("ABC123", 25.0f));
  ASSERT_TRUE(comp.apply_optimistic_tilt("ABC123", 70.0f));
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);

  IoFrame frame{};
  init_frame(frame, true, false, false, false);
  uint8_t own[3] = {0xC0, 0xFF, 0xEE};
  uint8_t device[3] = {0xAB, 0xC1, 0x23};
  set_dst(frame, own);
  set_src(frame, device);
  // The ack reports stopped and echoes pre-command (here: zeroed) target/current.
  uint8_t payload[8] = {test::STATUS_BYTE_AT_REST, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  ASSERT_TRUE(set_cmd(frame, CMD_PRIVATE_RESP, payload, sizeof(payload)));

  comp.update_device_status_(frame, /*trust_position=*/false);

  EXPECT_FLOAT_EQ(dev->optimistic.target, 25.0f) << "the execute-ack path decodes no position, so it clears nothing";
  EXPECT_EQ(dev->optimistic.motion, OptimisticState::Motion::MOVING);
  EXPECT_FLOAT_EQ(dev->optimistic.tilt, 70.0f);
  EXPECT_FLOAT_EQ(effective_target(*dev), 25.0f) << "the prediction still drives the effective value";
  EXPECT_FALSE(effective_is_stopped(*dev)) << "motion is still MOVING even though the ack said stopped";
}

// §7.10 — an unsolicited 0x71 supersedes the position prediction but leaves the tilt prediction standing.
TEST(HubStatus, UnsolicitedStatusUpdateSupersedesPositionButNotTilt) {
  TestableHubComponent comp;
  comp.add_device("ABC123", {DeviceType::INTERIOR_VENETIAN_BLIND, 0});
  ASSERT_TRUE(comp.apply_optimistic_target("ABC123", 25.0f));
  ASSERT_TRUE(comp.apply_optimistic_tilt("ABC123", 70.0f));
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);

  IoFrame frame{};
  init_frame(frame, true, false, false, false);
  uint8_t own[3] = {0xC0, 0xFF, 0xEE};
  uint8_t device[3] = {0xAB, 0xC1, 0x23};
  set_dst(frame, own);
  set_src(frame, device);
  // 0x71 layout: stopped flag at [0], target MSB at [5], current MSB at [7]; both 0xC800 -> 100%.
  uint8_t payload[11] = {test::STATUS_BYTE_AT_REST, 0x00, 0x00, 0x00, 0x00, 0xC8, 0x00, 0xC8, 0x00, 0x00, 0x00};
  ASSERT_TRUE(set_cmd(frame, CMD_STATUS_UPDATE, payload, sizeof(payload)));

  comp.update_device_status_(frame);

  EXPECT_EQ(dev->optimistic.target, UNKNOWN_POSITION) << "the 0x71 decodes a position, superseding that prediction";
  EXPECT_EQ(dev->optimistic.motion, OptimisticState::Motion::NONE);
  EXPECT_FLOAT_EQ(dev->optimistic.tilt, 70.0f) << "the 0x71 path decodes no tilt, so the tilt prediction stands";
  EXPECT_FLOAT_EQ(effective_tilt(*dev), 70.0f);
}

// ============================================================================
// Inbound status update tests (update_device_status_ paths)
// ============================================================================

TEST(HubStatus, StatusUpdateFrameHandling) {
  // The CMD_STATUS_UPDATE path through process_received_packet_ triggers
  // authentication (authenticate_request_) which requires a challenge response
  // from the mock radio. Test update_device_status_ directly instead.
  TestableHubComponent comp;
  comp.add_device("054E17");

  IoFrame f{};
  init_frame(f, true, false, true, false);
  uint8_t src[3] = {0x05, 0x4E, 0x17};
  uint8_t dst[3] = {0xC0, 0xFF, 0xEE};
  set_src(f, src);
  set_dst(f, dst);
  // Status update payload: stopped, target at [5..6]=0xC800 (100%), current at [7..8]=0xC800 (100%)
  uint8_t payload[11] = {test::STATUS_BYTE_AT_REST, 0x00, 0x00, 0x00, 0x00, 0xC8, 0x00, 0xC8, 0x00, 0x00, 0x00};
  set_cmd(f, CMD_STATUS_UPDATE, payload, sizeof(payload));

  comp.update_device_status_(f);

  auto *dev = comp.get_device("054E17");
  ASSERT_NE(dev, nullptr);
  EXPECT_FLOAT_EQ(dev->position, 100.0f) << "status update should decode position to 100%";
  EXPECT_TRUE(dev->is_stopped) << "device should be stopped";
}

TEST(HubStatus, StatusUpdateMovingFrameHandling) {
  TestableHubComponent comp;
  comp.add_device("054E17");

  IoFrame f{};
  init_frame(f, true, false, true, false);
  uint8_t src[3] = {0x05, 0x4E, 0x17};
  uint8_t dst[3] = {0xC0, 0xFF, 0xEE};
  set_src(f, src);
  set_dst(f, dst);
  // Status update: moving (STATUS_STOPPED NOT set), target at [5..6]=0xC800 (100%), current at [7..8]=0x3200 (25%)
  uint8_t payload[11] = {test::STATUS_BYTE_MOVING, 0x00, 0x00, 0x00, 0x00, 0xC8, 0x00, 0x32, 0x00, 0x00, 0x00};
  set_cmd(f, CMD_STATUS_UPDATE, payload, sizeof(payload));

  comp.update_device_status_(f);

  auto *dev = comp.get_device("054E17");
  ASSERT_NE(dev, nullptr);
  EXPECT_FLOAT_EQ(dev->target, 100.0f) << "target should decode to 100%";
  EXPECT_FLOAT_EQ(dev->position, 25.0f) << "position should decode to 25%";
  EXPECT_FALSE(dev->is_stopped) << "device should be moving";
}

TEST(HubStatus, UnsolicitedMovingStatusUpdateStampsMovingEvidence) {
  esphome::test_clock::ManualClock clock(7000);
  TestableHubComponent comp;
  comp.add_device("054E17");

  IoFrame f{};
  init_frame(f, true, false, true, false);
  uint8_t src[3] = {0x05, 0x4E, 0x17};
  uint8_t dst[3] = {0xC0, 0xFF, 0xEE};
  set_src(f, src);
  set_dst(f, dst);
  uint8_t moving[11] = {test::STATUS_BYTE_MOVING, 0x00, 0x00, 0x00, 0x00, 0xC8, 0x00, 0x32, 0x00, 0x00, 0x00};
  set_cmd(f, CMD_STATUS_UPDATE, moving, sizeof(moving));

  comp.update_device_status_(f);

  EXPECT_EQ(comp.get_device("054E17")->last_moving_evidence_ms, 7000u)
      << "a decoded 'not stopped' status is evidence the receiver is travelling";
}

TEST(HubStatus, TrustedMovingReportWithACountStampsWhenTheCountRunsOut) {
  esphome::test_clock::ManualClock clock(50000);
  TestableHubComponent comp;
  comp.add_device("ABC123");
  comp.begin_status_poll_tracking_("ABC123", 0);

  comp.update_device_status_(make_moving_reply(12));

  const IoDevice *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  EXPECT_EQ(dev->moving_until_ms, 50000u + 12000u + PREDICTED_END_MARGIN_MS) << "last report + R + a second";
}

TEST(HubStatus, TheEndOfTravelIsForgottenWithTheMovingEvidence) {
  esphome::test_clock::ManualClock clock(50000);
  TestableHubComponent comp;
  comp.add_device("ABC123");
  comp.begin_status_poll_tracking_("ABC123", 0);
  comp.update_device_status_(make_moving_reply(12));
  IoDevice *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  ASSERT_NE(dev->moving_until_ms, 0u);

  note_moving_evidence(*dev, esphome::millis());
  EXPECT_EQ(dev->moving_until_ms, 0u) << "a new sign of travel says nothing about when it ends";

  note_moving_until(*dev, 99999);
  comp.update_device_status_(make_status_reply(true, 0x6400, 0x6400));
  EXPECT_EQ(dev->moving_until_ms, 0u) << "a stopped report spends it";
}

TEST(HubStatus, MovingReportWithoutACountOrAnAckLeavesTheEndUnknown) {
  esphome::test_clock::ManualClock clock(50000);
  TestableHubComponent comp;
  comp.add_device("ABC123");
  comp.begin_status_poll_tracking_("ABC123", 0);

  comp.update_device_status_(make_moving_reply(0xFFFF));  // "no remaining time provided"
  EXPECT_EQ(comp.get_device("ABC123")->moving_until_ms, 0u);

  comp.update_device_status_(make_moving_reply(12),
                             /*trust_position=*/false);  // an execute ack's bytes mean something else
  EXPECT_EQ(comp.get_device("ABC123")->moving_until_ms, 0u);
}

TEST(HubStatus, UnsolicitedStoppedStatusUpdateClearsMovingEvidence) {
  esphome::test_clock::ManualClock clock(7000);
  TestableHubComponent comp;
  comp.add_device("054E17");
  note_moving_evidence(*comp.get_device("054E17"), esphome::millis());  // it was travelling

  IoFrame f{};
  init_frame(f, true, false, true, false);
  uint8_t src[3] = {0x05, 0x4E, 0x17};
  uint8_t dst[3] = {0xC0, 0xFF, 0xEE};
  set_src(f, src);
  set_dst(f, dst);
  uint8_t stopped[11] = {test::STATUS_BYTE_AT_REST, 0x00, 0x00, 0x00, 0x00, 0xC8, 0x00, 0xC8, 0x00, 0x00, 0x00};
  set_cmd(f, CMD_STATUS_UPDATE, stopped, sizeof(stopped));

  comp.update_device_status_(f);

  EXPECT_EQ(comp.get_device("054E17")->last_moving_evidence_ms, 0u) << "an observed stop spends the evidence";
}

TEST(HubStatus, GetInfo2RespUpdatesDeviceType) {
  TestableHubComponent comp;
  comp.add_device("054E17");

  IoFrame f{};
  init_frame(f, true, false, true, false);
  uint8_t src[3] = {0x05, 0x4E, 0x17};
  uint8_t dst[3] = {0xC0, 0xFF, 0xEE};
  set_src(f, src);
  set_dst(f, dst);
  // INFO2 response: data[10..11] use the shared packed type/subtype metadata layout.
  uint8_t payload[12] = {0};
  encode_device_metadata(DeviceType::ROLLER_SHUTTER, 0, &payload[10]);
  set_cmd(f, CMD_GET_INFO2_RESP, payload, sizeof(payload));

  // update_device_status_ is called directly (not through process_received_packet_)
  comp.update_device_status_(f);

  auto *dev = comp.get_device("054E17");
  ASSERT_NE(dev, nullptr);
  EXPECT_EQ(dev->type, DeviceType::ROLLER_SHUTTER) << "INFO2 response should update device type";
  EXPECT_EQ(dev->subtype, 0u) << "INFO2 response should update device subtype";
}

TEST(HubStatus, GetInfo2RespDoesNotOverwriteDeclaredType) {
  TestableHubComponent comp;
  comp.add_device("054E17", {DeviceType::VERTICAL_EXTERIOR_AWNING, 7});

  IoFrame f{};
  init_frame(f, true, false, true, false);
  uint8_t src[3] = {0x05, 0x4E, 0x17};
  uint8_t dst[3] = {0xC0, 0xFF, 0xEE};
  set_src(f, src);
  set_dst(f, dst);
  uint8_t payload[12] = {0};
  encode_device_metadata(DeviceType::ROLLER_SHUTTER, 0, &payload[10]);
  set_cmd(f, CMD_GET_INFO2_RESP, payload, sizeof(payload));

  comp.update_device_status_(f);

  auto *dev = comp.get_device("054E17");
  ASSERT_NE(dev, nullptr);
  EXPECT_EQ(dev->type, DeviceType::VERTICAL_EXTERIOR_AWNING) << "INFO2 must not overwrite a YAML-declared device type";
  EXPECT_EQ(dev->subtype, 7u) << "INFO2 must not overwrite a YAML-declared subtype";
}

namespace {

/// An INFO2 response from "054E17" carrying the given kind.
IoFrame make_info2_response(DeviceType type, uint8_t subtype) {
  IoFrame f{};
  init_frame(f, true, false, true, false);
  uint8_t src[3] = {0x05, 0x4E, 0x17};
  uint8_t dst[3] = {0xC0, 0xFF, 0xEE};
  set_src(f, src);
  set_dst(f, dst);
  uint8_t payload[12] = {0};
  encode_device_metadata(type, subtype, &payload[10]);
  set_cmd(f, CMD_GET_INFO2_RESP, payload, sizeof(payload));
  return f;
}

}  // namespace

TEST(HubStatus, GetInfo2RespResolvesTheFeaturesOfTheLearnedKind) {
  TestableHubComponent comp;
  comp.add_device("054E17", {.entity_class = DeviceCapabilityClass::COVER});

  comp.update_device_status_(make_info2_response(DeviceType::EXTERIOR_VENETIAN_BLIND, 0));

  const IoDevice *dev = comp.get_device("054E17");
  ASSERT_NE(dev, nullptr);
  EXPECT_EQ(dev->features, device_features(DeviceType::EXTERIOR_VENETIAN_BLIND, 0, DeviceModel::NONE));
  EXPECT_TRUE(has_feature(dev->features, device_feature::TILT));
}

TEST(HubStatus, GetInfo2RespTeachesAnInvertedKindItsDefaultDirection) {
  TestableHubComponent comp;
  comp.add_device("054E17", {.entity_class = DeviceCapabilityClass::COVER});

  comp.update_device_status_(make_info2_response(DeviceType::HORIZONTAL_AWNING, 0));

  EXPECT_TRUE(comp.get_device("054E17")->inverted);
}

TEST(HubStatus, AnExplicitInvertPositionFalseSurvivesALearnedInvertedKind) {
  TestableHubComponent comp;
  comp.add_device("054E17", {.entity_class = DeviceCapabilityClass::COVER, .invert_override = false});

  comp.update_device_status_(make_info2_response(DeviceType::HORIZONTAL_AWNING, 0));

  EXPECT_FALSE(comp.get_device("054E17")->inverted) << "the YAML `invert_position: false` outranks the kind's default";
}

TEST(HubStatus, ALearnedSubtype58CoverHasNoTilt) {
  TestableHubComponent comp;
  comp.add_device("054E17", {.entity_class = DeviceCapabilityClass::COVER});

  comp.update_device_status_(make_info2_response(DeviceType::EXTERIOR_VENETIAN_BLIND, SUBTYPE_MAIN_PARAMETER_ONLY));

  EXPECT_FALSE(has_feature(comp.get_device("054E17")->features, device_feature::TILT));
}

TEST(HubStatus, ErrorRespDoesNotMutateTrackedPosition) {
  TestableHubComponent comp;
  comp.add_device("054E17");

  auto *dev = comp.get_device("054E17");
  ASSERT_NE(dev, nullptr);
  dev->position = 25.0f;
  dev->target = 40.0f;
  dev->is_stopped = false;

  IoFrame f{};
  init_frame(f, true, false, true, false);
  uint8_t src[3] = {0x05, 0x4E, 0x17};
  uint8_t dst[3] = {0xC0, 0xFF, 0xEE};
  set_src(f, src);
  set_dst(f, dst);
  uint8_t payload[1] = {RESULT_LIMITATION_BY_WIND};
  set_cmd(f, CMD_ERROR_RESP, payload, sizeof(payload));

  comp.update_device_status_(f);

  EXPECT_FLOAT_EQ(dev->position, 25.0f) << "error responses should not overwrite last known position";
  EXPECT_FLOAT_EQ(dev->target, 40.0f) << "error responses should not overwrite last known target";
  EXPECT_FALSE(dev->is_stopped) << "error responses should not change movement state";
}

TEST(HubStatus, ErrorRespSetsLastResultCode) {
  TestableHubComponent comp;
  comp.add_device("054E17");

  auto *dev = comp.get_device("054E17");
  ASSERT_NE(dev, nullptr);
  EXPECT_EQ(dev->last_result_code, 0u) << "newly added device should start with no recorded result";

  IoFrame f{};
  init_frame(f, true, false, true, false);
  uint8_t src[3] = {0x05, 0x4E, 0x17};
  uint8_t dst[3] = {0xC0, 0xFF, 0xEE};
  set_src(f, src);
  set_dst(f, dst);
  uint8_t payload[1] = {RESULT_LIMITATION_BY_RAIN};
  set_cmd(f, CMD_ERROR_RESP, payload, sizeof(payload));

  comp.update_device_status_(f);

  EXPECT_EQ(dev->last_result_code, RESULT_LIMITATION_BY_RAIN) << "unsolicited 0xFE should record the result code";
  EXPECT_NE(dev->last_result_at_ms, 0u) << "unsolicited 0xFE should stamp a recorded-at timestamp";
}

TEST(HubStatus, ErrorRespWithNonLimitationCodeStillRecords) {
  TestableHubComponent comp;
  comp.add_device("054E17");
  auto *dev = comp.get_device("054E17");
  ASSERT_NE(dev, nullptr);

  IoFrame f{};
  init_frame(f, true, false, true, false);
  uint8_t src[3] = {0x05, 0x4E, 0x17};
  uint8_t dst[3] = {0xC0, 0xFF, 0xEE};
  set_src(f, src);
  set_dst(f, dst);
  uint8_t payload[1] = {RESULT_COMMAND_COMPLETED_OK};
  set_cmd(f, CMD_ERROR_RESP, payload, sizeof(payload));

  comp.update_device_status_(f);

  EXPECT_EQ(dev->last_result_code, RESULT_COMMAND_COMPLETED_OK)
      << "non-limitation result codes should still be recorded on the device";
}

TEST(HubStatus, ErrorRespWithEmptyPayloadDoesNotSetLastResultCode) {
  TestableHubComponent comp;
  comp.add_device("054E17");
  auto *dev = comp.get_device("054E17");
  ASSERT_NE(dev, nullptr);

  IoFrame f{};
  init_frame(f, true, false, true, false);
  uint8_t src[3] = {0x05, 0x4E, 0x17};
  uint8_t dst[3] = {0xC0, 0xFF, 0xEE};
  set_src(f, src);
  set_dst(f, dst);
  set_cmd(f, CMD_ERROR_RESP);  // zero-length payload — fails the ERROR_RESPONSE_MIN_DATA_LEN guard

  comp.update_device_status_(f);

  EXPECT_EQ(dev->last_result_code, 0u) << "a too-short 0xFE must keep hitting the existing rejection path unchanged";
}

TEST(HubStatus, SuccessfulPrivateResponseClearsLastResultCode) {
  TestableHubComponent comp;
  comp.add_device("054E17");
  auto *dev = comp.get_device("054E17");
  ASSERT_NE(dev, nullptr);
  dev->last_result_code = RESULT_LIMITATION_BY_WIND;
  dev->last_result_at_ms = 12345;

  IoFrame f{};
  init_frame(f, true, false, false, false);
  uint8_t src[3] = {0x05, 0x4E, 0x17};
  uint8_t dst[3] = {0xC0, 0xFF, 0xEE};
  set_src(f, src);
  set_dst(f, dst);
  // Bytes 0=flags(stopped), 2-3=target(0x0000=0%), 4-5=current(0x0000=0%).
  uint8_t payload[6] = {test::STATUS_BYTE_AT_REST, 0x00, 0x00, 0x00, 0x00, 0x00};
  set_cmd(f, CMD_PRIVATE_RESP, payload, sizeof(payload));

  comp.update_device_status_(f);

  EXPECT_EQ(dev->last_result_code, 0u) << "a subsequent successful status reply should clear a stale result reason";
  EXPECT_EQ(dev->last_result_at_ms, 0u) << "clearing the result code should also clear its timestamp";
}

TEST(HubStatus, SuccessfulStatusUpdateClearsLastResultCode) {
  TestableHubComponent comp;
  comp.add_device("054E17");
  auto *dev = comp.get_device("054E17");
  ASSERT_NE(dev, nullptr);
  dev->last_result_code = RESULT_LIMITATION_BY_RAIN;
  dev->last_result_at_ms = 12345;

  IoFrame f{};
  init_frame(f, true, false, false, false);
  uint8_t src[3] = {0x05, 0x4E, 0x17};
  uint8_t dst[3] = {0xC0, 0xFF, 0xEE};
  set_src(f, src);
  set_dst(f, dst);
  // 0x71 status update: byte0=flags(stopped), byte1=status byte, bytes5-6=target, bytes7-8=current.
  uint8_t payload[11] = {test::STATUS_BYTE_AT_REST, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  set_cmd(f, CMD_STATUS_UPDATE, payload, sizeof(payload));

  comp.update_device_status_(f);

  EXPECT_EQ(dev->last_result_code, 0u)
      << "a subsequent device-initiated status update should clear a stale result reason";
  EXPECT_EQ(dev->last_result_at_ms, 0u) << "clearing the result code should also clear its timestamp";
}

// The 0x71 Command Originator used to be read at data[1] — the status byte, which names no
// ORIGINATOR_* value, so the log line rendered "unknown" on every frame ever captured. The
// rendered line is unobservable on host (ESP_LOG* is a no-op stub), so the log call was split
// into detail::describe_status_update_originator() (hub_internal.h) and these tests assert that
// pure function's output directly — the same call the production log line makes. The second test
// pins that the length guard skips only the log line and does not tighten branch acceptance.
TEST(HubStatus, StatusUpdateOriginatorIsAtOffset14AndDecodePathUndisturbed) {
  TestableHubComponent comp;
  comp.add_device("054E17");

  IoFrame f{};
  init_frame(f, true, false, false, false);
  uint8_t src[3] = {0x05, 0x4E, 0x17};
  uint8_t dst[3] = {0xC0, 0xFF, 0xEE};
  set_src(f, src);
  set_dst(f, dst);
  // Real fixture bytes: multi_somfy_unsolicited_status_burst frame 0 (16-byte 0x71 payload).
  // data[0]=0x04 moving, data[1]=0x61 status byte, data[5..6]=C8 00 target 100%,
  // data[7..8]=D4 00 current unknown, data[14]=0x01 Command Originator (User Remote Control).
  uint8_t payload[16] = {0x04, 0x61, 0x10, 0x0A, 0x0B, 0xC8, 0x00, 0xD4,
                         0x00, 0xFF, 0xFF, 0x0A, 0x6F, 0x56, 0x01, 0x00};
  set_cmd(f, CMD_STATUS_UPDATE, payload, sizeof(payload));

  comp.update_device_status_(f);

  auto *dev = comp.get_device("054E17");
  ASSERT_NE(dev, nullptr);
  EXPECT_FLOAT_EQ(dev->target, 100.0f) << "target at data[5..6]=C8 00 should still decode to 100%";
  EXPECT_FLOAT_EQ(dev->position, UNKNOWN_POSITION) << "current at data[7..8]=D4 00 is unknown, unchanged by the fix";
  EXPECT_FALSE(dev->is_stopped) << "data[0]=0x04 is execution state 4 (executing)";

  // The production log line's own argument. Reading data[1] instead would render
  // "unknown(0x61)" here, which is exactly the bug this pins.
  EXPECT_EQ(detail::describe_status_update_originator(f), "user_remote(0x01)")
      << "the Command Originator is data[14], not data[1] (the status byte 0x61)";
}

TEST(HubStatus, StatusUpdateMinimumLengthFrameStillApplied) {
  TestableHubComponent comp;
  comp.add_device("054E17");

  IoFrame f{};
  init_frame(f, true, false, false, false);
  uint8_t src[3] = {0x05, 0x4E, 0x17};
  uint8_t dst[3] = {0xC0, 0xFF, 0xEE};
  set_src(f, src);
  set_dst(f, dst);
  // 11-byte 0x71 (== STATUS_UPDATE_MIN_DATA_LEN): stopped, target [5..6]=C8 00, current [7..8]=C8 00.
  // Too short to carry data[14], so the originator log line is skipped — but the frame must still
  // be decoded and applied.
  uint8_t payload[11] = {test::STATUS_BYTE_AT_REST, 0x00, 0x00, 0x00, 0x00, 0xC8, 0x00, 0xC8, 0x00, 0x00, 0x00};
  set_cmd(f, CMD_STATUS_UPDATE, payload, sizeof(payload));

  EXPECT_TRUE(detail::describe_status_update_originator(f).empty())
      << "a frame too short to carry data[14] must report no originator rather than a stale byte";

  comp.update_device_status_(f);

  auto *dev = comp.get_device("054E17");
  ASSERT_NE(dev, nullptr);
  EXPECT_FLOAT_EQ(dev->target, 100.0f) << "a minimum-length 0x71 must still decode its target";
  EXPECT_FLOAT_EQ(dev->position, 100.0f) << "a minimum-length 0x71 must still decode its position";
  EXPECT_TRUE(dev->is_stopped) << "the originator length guard must not tighten branch acceptance";
}

// === A request overheard from an unknown controller is named in the log ===

namespace {

/// A 2W request (first frame of an exchange) from @p src to @p dst.
RadioRxPacket request_packet(const uint8_t src[3], const uint8_t dst[3], uint8_t cmd = CMD_GET_NAME) {
  IoFrame f{};
  init_frame(f, true, true, false, false);
  set_src(f, src);
  set_dst(f, dst);
  set_cmd(f, cmd);
  return test::make_rx_packet(f);
}

}  // namespace

TEST(HubStatus, ARequestFromAnUnknownControllerIsNamedOncePerInterval) {
  test::RxTestableComponent comp;
  MockRadio radio;
  test::setup_rx_test_component(comp, radio);
  esphome::test_clock::ManualClock clock;
  test::LogCapture log;
  const uint8_t controller[3] = {0x43, 0x44, 0xE3};
  const uint8_t other_controller[3] = {0x43, 0x44, 0xE4};
  const uint8_t device[3] = {0x05, 0x4E, 0x17};

  comp.process_received_packet_(request_packet(controller, device));
  EXPECT_EQ(log.count('I'), 1u) << "first sight of a controller";

  log.clear();
  esphome::test_clock::advance_ms(1000);
  comp.process_received_packet_(request_packet(controller, device));
  EXPECT_EQ(log.count('I'), 0u) << "the same controller inside the interval";
  comp.process_received_packet_(request_packet(other_controller, device));
  EXPECT_EQ(log.count('I'), 1u) << "another controller is named at once";

  log.clear();
  esphome::test_clock::advance_ms(decisions::HeardSourceLog::INTERVAL_MS);
  comp.process_received_packet_(request_packet(controller, device));
  EXPECT_EQ(log.count('I'), 1u) << "named again after the interval";
}

TEST(HubStatus, ARequestFromOurOwnNodeOrARegisteredDeviceIsNotNamed) {
  test::RxTestableComponent comp;
  MockRadio radio;
  test::setup_rx_test_component(comp, radio);
  esphome::test_clock::ManualClock clock;
  test::LogCapture log;
  const uint8_t device[3] = {0x05, 0x4E, 0x17};
  const uint8_t controller[3] = {0x43, 0x44, 0xE3};

  comp.process_received_packet_(request_packet(comp.node_id_, device));
  comp.process_received_packet_(request_packet(device, controller));
  EXPECT_EQ(log.count('I'), 0u) << "neither the hub nor a registered device is a foreign controller";
}

TEST(HubStatus, AReplyFromAnUnknownNodeIsNotNamedAsAController) {
  test::RxTestableComponent comp;
  MockRadio radio;
  test::setup_rx_test_component(comp, radio);
  esphome::test_clock::ManualClock clock;
  test::LogCapture log;
  IoFrame f{};
  init_frame(f, true, false, true, false);
  const uint8_t src[3] = {0x43, 0x44, 0xE3};
  const uint8_t dst[3] = {0x05, 0x4E, 0x17};
  set_src(f, src);
  set_dst(f, dst);
  set_cmd(f, CMD_GET_NAME);
  comp.process_received_packet_(test::make_rx_packet(f));
  EXPECT_EQ(log.count('I'), 0u) << "only the first frame of an exchange is a request";
}

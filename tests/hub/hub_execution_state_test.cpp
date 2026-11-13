/// @file hub_execution_state_test.cpp
/// @brief How the hub follows a cover by the execution state in byte 0 of its status replies.
///
/// A cover that waits for its turn or for power is followed like a running one, states 0 and 1 settle
/// it, and a count that stops falling backs the progress reads off. The sequences model a VELUX SML
/// shutter queued behind another on one supply: 0x2B (state 3), 0x2C (state 4), 0x2D (state 5) are
/// the status bytes it answered a main-parameter read with.
/// \todo An ordinary status reply carrying state 3 is not captured yet; these tests feed one as a
///       synthetic frame and fail the day a capture contradicts the assumption.

#include "hub_core.h"
#include "hub_internal.h"
#include "platform_companion_sensors.h"
#include "proto_frame.h"
#include "esphome/core/application.h"

#include "test_helpers.h"

using namespace esphome::home_io_control;
using test::TestableHubComponent;

namespace {

// Status bytes as the SML answered: result code 5 over the state.
constexpr uint8_t BYTE_NON_EXECUTING = 0x28;
constexpr uint8_t BYTE_ERROR = 0x29;
constexpr uint8_t BYTE_WAITING_FOR_POWER = 0x2B;
constexpr uint8_t BYTE_EXECUTING = 0x2C;
constexpr uint8_t BYTE_DONE = 0x2D;

constexpr uint16_t TARGET_OPEN_RAW = 0x0000;  ///< 0 %: fully open.
constexpr uint16_t AT_75_PERCENT_RAW = 0x9600;

/// An ordinary status reply (CMD_PRIVATE_RESP) from ABC123 with @p status_byte, target and current raw
/// positions and @p remaining_s at bytes 6-7.
IoFrame status_reply(uint8_t status_byte, uint16_t target_raw, uint16_t current_raw, uint16_t remaining_s = 0xFFFF) {
  IoFrame frame{};
  init_frame(frame, true, false, false, false);
  uint8_t own[3] = {0xC0, 0xFF, 0xEE};
  uint8_t device[3] = {0xAB, 0xC1, 0x23};
  set_dst(frame, own);
  set_src(frame, device);
  const uint8_t payload[8] = {status_byte,
                              0x00,
                              static_cast<uint8_t>(target_raw >> 8),
                              static_cast<uint8_t>(target_raw & 0xFF),
                              static_cast<uint8_t>(current_raw >> 8),
                              static_cast<uint8_t>(current_raw & 0xFF),
                              static_cast<uint8_t>(remaining_s >> 8),
                              static_cast<uint8_t>(remaining_s & 0xFF)};
  EXPECT_TRUE(set_cmd(frame, CMD_PRIVATE_RESP, payload, sizeof(payload)));
  return frame;
}

/// A device-originated status update (CMD_STATUS_UPDATE) from ABC123 with @p status_byte.
IoFrame status_update(uint8_t status_byte, uint16_t target_raw, uint16_t current_raw) {
  IoFrame frame{};
  init_frame(frame, true, false, true, false);
  uint8_t device[3] = {0xAB, 0xC1, 0x23};
  uint8_t own[3] = {0xC0, 0xFF, 0xEE};
  set_src(frame, device);
  set_dst(frame, own);
  const uint8_t payload[11] = {status_byte,
                               0x00,
                               0x00,
                               0x00,
                               0x00,
                               static_cast<uint8_t>(target_raw >> 8),
                               static_cast<uint8_t>(target_raw & 0xFF),
                               static_cast<uint8_t>(current_raw >> 8),
                               static_cast<uint8_t>(current_raw & 0xFF),
                               0x00,
                               0x00};
  EXPECT_TRUE(set_cmd(frame, CMD_STATUS_UPDATE, payload, sizeof(payload)));
  return frame;
}

/// A hub with ABC123 registered and a follow-up window open, as after the hub's own command.
struct QueuedCoverRig {
  esphome::test_clock::ManualClock clock{50000};
  TestableHubComponent comp;
  IoDevice *dev{nullptr};

  QueuedCoverRig() {
    comp.add_device("ABC123");
    dev = comp.get_device("ABC123");
    comp.begin_status_poll_tracking_("ABC123", 0);
  }

  void feed(const IoFrame &frame) { comp.update_device_status_(frame); }
  [[nodiscard]] bool tracking() const { return comp.poll_policy_.is_tracking_active("ABC123", esphome::millis()); }
};

}  // namespace

TEST(HubExecutionState, QueuedCoverIsFollowedFromWaitingThroughRunningToDone) {
  QueuedCoverRig rig;
  ASSERT_NE(rig.dev, nullptr);

  // Asked to open from 75 %: it waits for the other shutter's motor, then runs, then is done.
  rig.feed(status_reply(BYTE_WAITING_FOR_POWER, TARGET_OPEN_RAW, AT_75_PERCENT_RAW));
  EXPECT_EQ(rig.dev->execution_state, ExecutionState::WAITING_FOR_POWER);
  EXPECT_FALSE(effective_is_stopped(*rig.dev)) << "a queued cover is not at rest";
  EXPECT_FLOAT_EQ(rig.dev->target, 0.0F);
  EXPECT_FLOAT_EQ(rig.dev->position, 75.0F);
  EXPECT_TRUE(rig.tracking());
  EXPECT_NE(rig.comp.poll_policy_.get_next_update("ABC123"), 0u) << "its next read is scheduled";

  esphome::test_clock::advance_ms(DEFAULT_SETTLE_POLL_DELAY_MS);
  rig.feed(status_reply(BYTE_WAITING_FOR_POWER, TARGET_OPEN_RAW, AT_75_PERCENT_RAW));
  EXPECT_FALSE(effective_is_stopped(*rig.dev));
  EXPECT_TRUE(rig.tracking());

  esphome::test_clock::advance_ms(DEFAULT_SETTLE_POLL_DELAY_MS);
  rig.feed(status_reply(BYTE_EXECUTING, TARGET_OPEN_RAW, 0x6400));
  EXPECT_EQ(rig.dev->execution_state, ExecutionState::EXECUTING);
  EXPECT_FALSE(effective_is_stopped(*rig.dev));
  EXPECT_FLOAT_EQ(rig.dev->position, 50.0F);

  esphome::test_clock::advance_ms(DEFAULT_SETTLE_POLL_DELAY_MS);
  rig.feed(status_reply(BYTE_DONE, TARGET_OPEN_RAW, TARGET_OPEN_RAW));
  EXPECT_EQ(rig.dev->execution_state, ExecutionState::DONE);
  EXPECT_TRUE(rig.dev->is_stopped) << "when it reports done it settles as before";
  EXPECT_FALSE(rig.tracking());
  EXPECT_EQ(rig.comp.poll_policy_.get_next_update("ABC123"), 0u);
}

TEST(HubExecutionState, TheBareStateBytesAlsoFollowTheCover) {
  // 0x03, 0x04 and 0x05 are the states with a result code of 0, the form the corpus shows for a running
  // and a finished cover.
  QueuedCoverRig rig;
  rig.feed(status_reply(0x03, TARGET_OPEN_RAW, AT_75_PERCENT_RAW));
  EXPECT_FALSE(effective_is_stopped(*rig.dev));
  rig.feed(status_reply(0x04, TARGET_OPEN_RAW, 0x6400));
  EXPECT_FALSE(effective_is_stopped(*rig.dev));
  rig.feed(status_reply(0x05, TARGET_OPEN_RAW, TARGET_OPEN_RAW));
  EXPECT_TRUE(effective_is_stopped(*rig.dev));
}

TEST(HubExecutionState, AWaitingCoverFoundByAnUntrackedReadOpensTheWindow) {
  // A status refresh or a read after boot finds a cover that is waiting: nothing had opened a window.
  esphome::test_clock::ManualClock clock(50000);
  TestableHubComponent comp;
  comp.add_device("ABC123");

  comp.update_device_status_(status_reply(BYTE_WAITING_FOR_POWER, TARGET_OPEN_RAW, AT_75_PERCENT_RAW));

  EXPECT_FALSE(effective_is_stopped(*comp.get_device("ABC123")));
  EXPECT_TRUE(comp.poll_policy_.is_tracking_active("ABC123", esphome::millis()));
  EXPECT_NE(comp.poll_policy_.get_next_update("ABC123"), 0u);
}

TEST(HubExecutionState, AWaitingCoverWaitingForPowerOfTheOtherKindOfReplyIsFollowedToo) {
  // Waiting for its turn (state 2) is moving as well; byte 0x2A.
  QueuedCoverRig rig;
  rig.feed(status_reply(0x2A, TARGET_OPEN_RAW, AT_75_PERCENT_RAW));
  EXPECT_EQ(rig.dev->execution_state, ExecutionState::WAITING_FOR_EXECUTING);
  EXPECT_FALSE(effective_is_stopped(*rig.dev));
  EXPECT_TRUE(rig.tracking());
}

TEST(HubExecutionState, AStatusUpdateIsReadTheSameWay) {
  esphome::test_clock::ManualClock clock(50000);
  TestableHubComponent comp;
  comp.add_device("ABC123");
  comp.begin_status_poll_tracking_("ABC123", 0);

  comp.update_device_status_(status_update(BYTE_WAITING_FOR_POWER, TARGET_OPEN_RAW, AT_75_PERCENT_RAW));

  IoDevice *dev = comp.get_device("ABC123");
  EXPECT_EQ(dev->execution_state, ExecutionState::WAITING_FOR_POWER);
  EXPECT_FALSE(effective_is_stopped(*dev));
  EXPECT_NE(comp.poll_policy_.get_next_update("ABC123"), 0u);
}

TEST(HubExecutionState, NonExecutingAndErrorSettleTheCoverAtItsReportedPosition) {
  // A report that says at rest off its target is confirmed by one more read (decisions::
  // classify_stopped_report()); the second one settles the cover where it reported itself.
  for (const uint8_t status_byte : {BYTE_NON_EXECUTING, BYTE_ERROR}) {
    SCOPED_TRACE(static_cast<unsigned>(status_byte));
    QueuedCoverRig rig;
    rig.feed(status_reply(status_byte, TARGET_OPEN_RAW, AT_75_PERCENT_RAW));
    EXPECT_FALSE(rig.dev->is_stopped) << "the first one is confirmed, as for any stop off target";
    EXPECT_TRUE(rig.tracking());

    esphome::test_clock::advance_ms(DEFAULT_SETTLE_POLL_DELAY_MS);
    rig.feed(status_reply(status_byte, TARGET_OPEN_RAW, AT_75_PERCENT_RAW));

    EXPECT_TRUE(rig.dev->is_stopped) << "it could not run, so it stays where it reported itself";
    EXPECT_FLOAT_EQ(rig.dev->position, 75.0F);
    EXPECT_FALSE(rig.tracking()) << "nothing left to follow";
    EXPECT_EQ(rig.comp.poll_policy_.get_next_update("ABC123"), 0u);
  }
}

TEST(HubExecutionState, NonExecutingOnTargetSettlesAtOnce) {
  QueuedCoverRig rig;
  rig.feed(status_reply(BYTE_NON_EXECUTING, AT_75_PERCENT_RAW, AT_75_PERCENT_RAW));
  EXPECT_TRUE(rig.dev->is_stopped);
  EXPECT_FALSE(rig.tracking());
}

TEST(HubExecutionState, ABitZeroSetByteWithStateOneIsNoLongerReadAsDoneButStillAtRest) {
  // 0x01 is state 1 (error), whose bit 0 is also set: the cover is at rest either way, and now says why.
  QueuedCoverRig rig;
  rig.feed(status_reply(0x01, TARGET_OPEN_RAW, TARGET_OPEN_RAW));
  EXPECT_TRUE(rig.dev->is_stopped);
  EXPECT_EQ(rig.dev->execution_state, ExecutionState::EXECUTION_ERROR);
}

TEST(HubExecutionState, UndefinedStatesFallBackToTheStoppedBit) {
  for (const auto &row : {std::pair<uint8_t, bool>{0x06, false}, std::pair<uint8_t, bool>{0x07, true},
                          std::pair<uint8_t, bool>{0x2E, false}, std::pair<uint8_t, bool>{0x2F, true}}) {
    SCOPED_TRACE(static_cast<unsigned>(row.first));
    QueuedCoverRig rig;
    rig.feed(status_reply(row.first, 0x6400, 0x6400));
    EXPECT_EQ(rig.dev->execution_state, ExecutionState::UNDEFINED);
    EXPECT_EQ(rig.dev->is_stopped, row.second) << "bit 0 decides, as before the state was read";
  }
}

TEST(HubExecutionState, AHeaterKeepsNoStateAndIsAlwaysAtRest) {
  esphome::test_clock::ManualClock clock(50000);
  TestableHubComponent comp;
  comp.add_device("ABC123");
  IoDevice *dev = comp.get_device("ABC123");
  test::set_kind(*dev, DeviceType::ELECTRICAL_HEATER, 0);

  comp.update_device_status_(status_reply(BYTE_WAITING_FOR_POWER, 0x6400, 0x6400));

  EXPECT_TRUE(dev->is_stopped) << "a heater has no motion, whatever its byte 0 holds";
  EXPECT_EQ(dev->execution_state, ExecutionState::UNDEFINED);
  EXPECT_FALSE(comp.poll_policy_.is_tracking_active("ABC123", esphome::millis()));
}

TEST(HubExecutionState, AWaitingCoverThatNeverRunsIsShownAtRestWhenTheWindowRunsOut) {
  QueuedCoverRig rig;
  rig.feed(status_reply(BYTE_WAITING_FOR_POWER, TARGET_OPEN_RAW, AT_75_PERCENT_RAW));
  const uint32_t deadline = rig.comp.poll_policy_.get_poll_deadline("ABC123");
  EXPECT_EQ(deadline - esphome::millis(), MAX_TRACKED_STATUS_POLL_WINDOW_MS) << "the hub's own command opened it";

  for (int i = 0; i < 3; i++) {
    esphome::test_clock::advance_ms(60000);
    rig.feed(status_reply(BYTE_WAITING_FOR_POWER, TARGET_OPEN_RAW, AT_75_PERCENT_RAW));
  }
  EXPECT_EQ(rig.comp.poll_policy_.get_poll_deadline("ABC123"), deadline) << "later reports never move the deadline";

  esphome::test_clock::advance_ms(MAX_TRACKED_STATUS_POLL_WINDOW_MS);
  rig.feed(status_reply(BYTE_WAITING_FOR_POWER, TARGET_OPEN_RAW, AT_75_PERCENT_RAW));
  rig.comp.settle_ended_status_tracking_();

  EXPECT_TRUE(rig.dev->is_stopped) << "still reporting waiting after the give-up time";
  EXPECT_FLOAT_EQ(rig.dev->position, 75.0F);
  EXPECT_EQ(rig.dev->execution_state, ExecutionState::UNDEFINED) << "its old word no longer raises an issue";
  EXPECT_EQ(rig.comp.poll_policy_.get_next_update("ABC123"), 0u);
}

TEST(HubExecutionState, AWindowOpenedByTheFirstMovingReportIsMeasuredFromThatReport) {
  // No command and no overheard traffic opened the window: the first moving report does, and the
  // give-up time counts from it.
  esphome::test_clock::ManualClock clock(50000);
  TestableHubComponent comp;
  comp.add_device("ABC123");

  comp.update_device_status_(status_reply(BYTE_WAITING_FOR_POWER, TARGET_OPEN_RAW, AT_75_PERCENT_RAW));
  const uint32_t opened = esphome::millis();

  esphome::test_clock::advance_ms(MAX_TRACKED_STATUS_POLL_WINDOW_MS - 1000);
  comp.update_device_status_(status_reply(BYTE_EXECUTING, TARGET_OPEN_RAW, 0x6400));
  EXPECT_EQ(comp.poll_policy_.get_poll_deadline("ABC123"), opened + MAX_TRACKED_STATUS_POLL_WINDOW_MS);
  EXPECT_TRUE(comp.poll_policy_.is_tracking_active("ABC123", esphome::millis())) << "still inside the window";

  esphome::test_clock::advance_ms(2000);
  comp.update_device_status_(status_reply(BYTE_EXECUTING, TARGET_OPEN_RAW, 0x6400));
  comp.settle_ended_status_tracking_();
  EXPECT_TRUE(comp.get_device("ABC123")->is_stopped) << "240 s after the first report it is given up";
}

// --- frozen count --------------------------------------------------------------------------

namespace {

/// Delay to the next read after a trusted moving report of ABC123 carrying a count of @p remaining_s.
uint32_t delay_after_count(QueuedCoverRig &rig, uint16_t remaining_s) {
  rig.comp.poll_policy_.set_next_update("ABC123", 0);
  rig.feed(status_reply(BYTE_WAITING_FOR_POWER, TARGET_OPEN_RAW, AT_75_PERCENT_RAW, remaining_s));
  return rig.comp.poll_policy_.get_next_update("ABC123") - esphome::millis();
}

}  // namespace

TEST(HubExecutionState, AFrozenCountBacksTheReadsOffInGrowingSteps) {
  QueuedCoverRig rig;
  EXPECT_EQ(delay_after_count(rig, 30), DEFAULT_SETTLE_POLL_DELAY_MS) << "the first count has nothing to compare";
  EXPECT_EQ(delay_after_count(rig, 30), DEFAULT_SETTLE_POLL_DELAY_MS);
  EXPECT_EQ(delay_after_count(rig, 30), DEFAULT_SETTLE_POLL_DELAY_MS);
  EXPECT_EQ(delay_after_count(rig, 30), FROZEN_COUNT_BACKOFF_FIRST_MS) << "three reports that did not fall";
  EXPECT_EQ(delay_after_count(rig, 30), FROZEN_COUNT_BACKOFF_SECOND_MS);
  EXPECT_EQ(delay_after_count(rig, 30), FROZEN_COUNT_BACKOFF_MAX_MS);
  EXPECT_EQ(delay_after_count(rig, 30), FROZEN_COUNT_BACKOFF_MAX_MS) << "the last step stays";
}

TEST(HubExecutionState, AFallingCountReturnsToTheShortCadence) {
  QueuedCoverRig rig;
  for (int i = 0; i < 6; i++)
    delay_after_count(rig, 30);
  ASSERT_EQ(delay_after_count(rig, 30), FROZEN_COUNT_BACKOFF_MAX_MS);

  EXPECT_EQ(delay_after_count(rig, 29), DEFAULT_SETTLE_POLL_DELAY_MS) << "it started to run: back to 3 s";
  EXPECT_EQ(delay_after_count(rig, 29), DEFAULT_SETTLE_POLL_DELAY_MS) << "and the steps start over";
}

TEST(HubExecutionState, ASlowButFallingCountIsNeverBackedOff) {
  QueuedCoverRig rig;
  for (uint16_t remaining = 40; remaining > 20; remaining--)
    EXPECT_EQ(delay_after_count(rig, remaining), DEFAULT_SETTLE_POLL_DELAY_MS) << remaining << " s left";
}

// --- Active Issue --------------------------------------------------------------------------

TEST(HubExecutionState, ActiveIssueNamesWaitingForPowerWhileItLastsAndClearsWhenItRuns) {
  QueuedCoverRig rig;
  IOHomeActiveIssueTextSensor sensor;
  sensor.set_parent(&rig.comp);
  sensor.set_device_id("ABC123");
  sensor.setup();
  ASSERT_EQ(sensor.state, "");

  rig.feed(status_reply(BYTE_WAITING_FOR_POWER, TARGET_OPEN_RAW, AT_75_PERCENT_RAW));
  rig.comp.notify_device_update_("ABC123");
  EXPECT_EQ(sensor.state, "NODE_WAITING_FOR_POWER");

  rig.feed(status_reply(BYTE_EXECUTING, TARGET_OPEN_RAW, 0x6400));
  rig.comp.notify_device_update_("ABC123");
  EXPECT_EQ(sensor.state, "") << "it runs: no issue";
}

TEST(HubExecutionState, ActiveIssueNamesAnUnexecutedAndAFailedMoveUntilTheNextStateClearsIt) {
  QueuedCoverRig rig;
  IOHomeActiveIssueTextSensor sensor;
  sensor.set_parent(&rig.comp);
  sensor.set_device_id("ABC123");
  sensor.setup();

  rig.feed(status_reply(BYTE_NON_EXECUTING, TARGET_OPEN_RAW, AT_75_PERCENT_RAW));
  rig.comp.notify_device_update_("ABC123");
  EXPECT_EQ(sensor.state, ACTIVE_ISSUE_NOT_EXECUTING);

  rig.feed(status_reply(BYTE_ERROR, TARGET_OPEN_RAW, AT_75_PERCENT_RAW));
  rig.comp.notify_device_update_("ABC123");
  EXPECT_EQ(sensor.state, ACTIVE_ISSUE_EXECUTION_ERROR);

  rig.feed(status_reply(BYTE_DONE, AT_75_PERCENT_RAW, AT_75_PERCENT_RAW));
  rig.comp.notify_device_update_("ABC123");
  EXPECT_EQ(sensor.state, "");
}

TEST(HubExecutionState, ARainLimitStillWinsOverTheExecutionStateInTheActiveIssue) {
  QueuedCoverRig rig;
  IOHomeActiveIssueTextSensor sensor;
  sensor.set_parent(&rig.comp);
  sensor.set_device_id("ABC123");
  sensor.setup();
  rig.dev->limited_by_rain = true;

  rig.feed(status_reply(BYTE_WAITING_FOR_POWER, TARGET_OPEN_RAW, AT_75_PERCENT_RAW));
  rig.dev->limited_by_rain = true;
  rig.comp.notify_device_update_("ABC123");

  EXPECT_EQ(sensor.state, "LIMITATION_BY_RAIN");
}

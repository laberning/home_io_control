/// @file hub_status_rain_battery_test.cpp
/// @brief Tests for what a status reply reports beyond position in hub_status.cpp: the rain limitation of a window and
/// the coarse battery level.

#include "hub_core.h"
#include "hub_internal.h"
#include "radio_interface.h"
#include "proto_frame.h"
#include "esphome/core/application.h"
#include "esphome/core/component.h"

#include "corpus_test_helpers.h"
#include "test_helpers.h"
#include "stubs/radio_test_common.h"

using namespace esphome::home_io_control;
using test::make_rx_packet;
using test::RxTestableComponent;
using test::setup_rx_test_component;
using test::TestableHubComponent;

// ============================================================================
// Derived rain limitation — dev.limited_by_rain, from the status replies a VELUX window sends
// (issue #98). The payloads are the dry / rain-closing / rain-at-rest replies a reporter pasted
// from a live window, with made-up node ids (the originals were masked).
// ============================================================================

namespace {

/// A CMD_PRIVATE_RESP status reply from "ABC123" with the 14-byte layout: flags, 00, target,
/// current, remaining, last master (3), originator, 00 00.
IoFrame make_window_status_reply(const uint8_t (&payload)[14]) {
  IoFrame f{};
  init_frame(f, true, false, false, false);
  const uint8_t src[3] = {0xAB, 0xC1, 0x23};
  const uint8_t dst[3] = {0xC0, 0xFF, 0xEE};
  set_src(f, src);
  set_dst(f, dst);
  set_cmd(f, CMD_PRIVATE_RESP, payload, sizeof(payload));
  return f;
}

constexpr uint8_t DRY_AT_REST[14] = {0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                     0x00, 0xC0, 0xFF, 0xEE, 0x01, 0x00, 0x00};
constexpr uint8_t RAIN_CLOSING[14] = {0x04, 0x00, 0xD8, 0x01, 0x20, 0xDD, 0x00,
                                      0x22, 0x00, 0x00, 0x32, 0x02, 0x00, 0x00};
constexpr uint8_t RAIN_AT_REST[14] = {0x05, 0x00, 0xD8, 0x01, 0xBA, 0x00, 0x00,
                                      0x00, 0x00, 0x00, 0x32, 0x02, 0x00, 0x00};
/// What a window might report after a clamped open if its record switches to the hub: stopped at
/// the ventilation position, originator 01.
constexpr uint8_t CLAMPED_AT_REST_HUB_ORIGINATOR[14] = {0x05, 0x00, 0xD8, 0x01, 0xBA, 0x00, 0x00,
                                                        0x00, 0xC0, 0xFF, 0xEE, 0x01, 0x00, 0x00};

}  // namespace

TEST(HubStatus, RainClosingReplySetsLimitedByRain) {
  TestableHubComponent comp;
  comp.add_device("ABC123");
  comp.update_device_status_(make_window_status_reply(RAIN_CLOSING));

  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  EXPECT_TRUE(dev->limited_by_rain);
  EXPECT_NE(dev->last_rain_evidence_ms, 0u);
  EXPECT_EQ(dev->last_result_code, 0) << "the derived flag never touches the explicit result";
}

TEST(HubStatus, RainStateAtRestKeepsLimitedByRain) {
  TestableHubComponent comp;
  comp.add_device("ABC123");
  comp.update_device_status_(make_window_status_reply(RAIN_CLOSING));
  comp.update_device_status_(make_window_status_reply(RAIN_AT_REST));

  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  EXPECT_TRUE(dev->limited_by_rain);
  EXPECT_TRUE(dev->is_stopped);
  EXPECT_FLOAT_EQ(dev->position, dev->target) << "stopped device's selector target normalizes to its position";
  EXPECT_EQ(dev->last_result_code, 0);
}

TEST(HubStatus, ReplyAfterRainEndedAndOpenSucceededClearsLimitedByRain) {
  TestableHubComponent comp;
  comp.add_device("ABC123");
  comp.update_device_status_(make_window_status_reply(RAIN_AT_REST));
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  ASSERT_TRUE(dev->limited_by_rain);

  dev->optimistic.target = 0.0F;  // an open was issued and predicted
  comp.update_device_status_(make_window_status_reply(DRY_AT_REST));

  EXPECT_FALSE(dev->limited_by_rain);
  EXPECT_EQ(dev->last_rain_evidence_ms, 0u) << "an unclamped command ends the rain memory";
  EXPECT_EQ(dev->last_result_code, 0);
}

TEST(HubStatus, ClampedOpenAfterRainEvidenceSetsLimitedByRainEvenIfOriginatorSwitches) {
  TestableHubComponent comp;
  comp.add_device("ABC123");
  comp.update_device_status_(make_window_status_reply(RAIN_AT_REST));
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);

  dev->optimistic.target = 0.0F;  // open accepted by the window, then clamped to 93 %
  comp.update_device_status_(make_window_status_reply(CLAMPED_AT_REST_HUB_ORIGINATOR));

  EXPECT_EQ(dev->last_command_originator, 0x01);
  EXPECT_TRUE(dev->limited_by_rain) << "rule b: stopped at 93 % against a predicted 0 %, with fresh rain evidence";
  EXPECT_EQ(dev->last_result_code, 0);
}

TEST(HubStatus, ClampWithoutPriorRainEvidenceIsNotLabelledRain) {
  TestableHubComponent comp;
  comp.add_device("ABC123");
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);

  dev->optimistic.target = 0.0F;
  comp.update_device_status_(make_window_status_reply(CLAMPED_AT_REST_HUB_ORIGINATOR));

  EXPECT_FALSE(dev->limited_by_rain) << "an end stop or obstacle must not be mislabelled as rain";
}

TEST(HubStatus, ExecuteAckNeverFlipsLimitedByRain) {
  TestableHubComponent comp;
  comp.add_device("ABC123");
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);

  // An ack carrying a rain originator is not a report of the last command (stale record layout).
  comp.update_device_status_(make_window_status_reply(RAIN_CLOSING), /*trust_position=*/false);
  EXPECT_FALSE(dev->limited_by_rain);

  comp.update_device_status_(make_window_status_reply(RAIN_AT_REST));
  ASSERT_TRUE(dev->limited_by_rain);
  comp.update_device_status_(make_window_status_reply(DRY_AT_REST), /*trust_position=*/false);
  EXPECT_TRUE(dev->limited_by_rain) << "an ack must not clear a derived flag either";
}

TEST(HubStatus, StatusUpdateNamingRainSensorSetsLimitedByRain) {
  // A 0x71 carries the record at the shifted offset (see StatusUpdateRecordsLastCommanderAtTheShiftedOffset).
  TestableHubComponent comp;
  comp.add_device("ABC123");

  IoFrame f{};
  init_frame(f, true, false, false, false);
  const uint8_t src[3] = {0xAB, 0xC1, 0x23};
  const uint8_t dst[3] = {0xC0, 0xFF, 0xEE};
  set_src(f, src);
  set_dst(f, dst);
  const uint8_t payload[16] = {0x05, 0x60, 0x10, 0x0A, 0x0B, 0x00, 0x00, 0x00,
                               0x00, 0x00, 0x00, 0x00, 0x00, 0x32, 0x02, 0x00};
  set_cmd(f, CMD_STATUS_UPDATE, payload, sizeof(payload));
  comp.update_device_status_(f);

  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  EXPECT_TRUE(dev->has_last_command);
  EXPECT_TRUE(dev->limited_by_rain);
}

TEST(HubStatus, RainEvidenceExpiresAfterTheHoldWindow) {
  TestableHubComponent comp;
  comp.add_device("ABC123");
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);

  // Evidence stamped long ago (millis() in the host stub is small, so use a stamp the hold window
  // cannot reach back to: wrap-safe subtraction makes now - stamp huge).
  comp.update_device_status_(make_window_status_reply(RAIN_AT_REST));
  dev->last_rain_evidence_ms = esphome::millis() - RAIN_EVIDENCE_HOLD_MS - 1;
  dev->optimistic.target = 0.0F;
  comp.update_device_status_(make_window_status_reply(CLAMPED_AT_REST_HUB_ORIGINATOR));

  EXPECT_FALSE(dev->limited_by_rain);
}

TEST(HubStatus, ReplyWithoutALastCommandRecordDoesNotRefreshRainEvidence) {
  // The record persists on the device record between replies; a 0x71 too short to carry one must
  // not re-stamp the rain evidence from that stale record.
  TestableHubComponent comp;
  comp.add_device("ABC123");
  comp.update_device_status_(make_window_status_reply(RAIN_AT_REST));
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  const uint32_t stamp = esphome::millis() - 1000;
  dev->last_rain_evidence_ms = stamp;

  IoFrame f{};
  init_frame(f, true, false, false, false);
  const uint8_t src[3] = {0xAB, 0xC1, 0x23};
  const uint8_t dst[3] = {0xC0, 0xFF, 0xEE};
  set_src(f, src);
  set_dst(f, dst);
  const uint8_t payload[11] = {0x05, 0x60, 0x10, 0x0A, 0x0B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  set_cmd(f, CMD_STATUS_UPDATE, payload, sizeof(payload));
  comp.update_device_status_(f);

  EXPECT_EQ(dev->last_rain_evidence_ms, stamp);
}

TEST(HubStatus, OverheardLimitationReplyIsNeverAppliedToTheDeviceRecord) {
  // A KLF 200 polling a window we also hold: the window's 0x26 reply is rendered (DEBUG) and
  // nothing else -- its sender's key is unproven (ADR 0022), and the layout is an assumption.
  RxTestableComponent comp;
  MockRadio radio;
  setup_rx_test_component(comp, radio);
  auto *dev = comp.get_device("054E17");
  ASSERT_NE(dev, nullptr);
  dev->position = 42.0F;
  dev->target = 42.0F;
  dev->is_stopped = true;
  const uint8_t last_result_code = dev->last_result_code;

  IoFrame reply{};
  init_frame(reply, true, false, true, false);
  const uint8_t window[3] = {0x05, 0x4E, 0x17};
  const uint8_t klf[3] = {0x97, 0xDE, 0x1A};
  set_src(reply, window);
  set_dst(reply, klf);
  const uint8_t data[LIMITATION_RESP_DATA_SIZE] = {0x00, 0xBA, 0x00, 0x02, 0x1D};
  set_cmd(reply, CMD_LIMITATION_STATUS_RESP, data, sizeof(data));
  comp.process_received_packet_(make_rx_packet(reply));

  EXPECT_FLOAT_EQ(dev->position, 42.0F);
  EXPECT_FLOAT_EQ(dev->target, 42.0F);
  EXPECT_TRUE(dev->is_stopped);
  EXPECT_FALSE(dev->limited_by_rain);
  EXPECT_EQ(dev->last_result_code, last_result_code);
}

TEST(HubStatus, OverheardLimitationRequestToOurDeviceStillSchedulesTheRemoteActivityPoll) {
  // Rendering the request must not consume it: a KLF 200 talking to a device we own is remote
  // activity like any other and keeps triggering the status poll.
  RxTestableComponent comp;
  MockRadio radio;
  setup_rx_test_component(comp, radio);

  IoFrame req{};
  init_frame(req, true, true, false, false);
  const uint8_t klf[3] = {0x97, 0xDE, 0x1A};
  const uint8_t window[3] = {0x05, 0x4E, 0x17};
  set_src(req, klf);
  set_dst(req, window);
  const uint8_t data[LIMITATION_REQ_DATA_SIZE] = {static_cast<uint8_t>(LimitationType::MINIMUM), 0x00, 0x00};
  set_cmd(req, CMD_LIMITATION_STATUS_REQ, data, sizeof(data));
  comp.process_received_packet_(make_rx_packet(req));

  EXPECT_EQ(comp.last_timeout_id_, decisions::remote_poll_timer_id(window));
}

// ============================================================================
// Coarse battery level (data[1] bits 5-6)
// ============================================================================
// Same byte in 0x04 and 0x71 payloads. Read from status-poll replies and device status updates only,
// never from an execute ack, whose layout follows the request.

namespace {

/// Build a status-bearing frame from E461E9 (the RS100 Solar of somfy_rs100_statuspoll_kig300_sx1276).
IoFrame make_rs100_status_frame(uint8_t cmd, const uint8_t *payload, uint8_t len) {
  IoFrame f{};
  init_frame(f, true, false, false, false);
  uint8_t src[3] = {0xE4, 0x61, 0xE9};
  uint8_t dst[3] = {0xA0, 0x25, 0xE1};
  set_src(f, src);
  set_dst(f, dst);
  set_cmd(f, cmd, payload, len);
  return f;
}

}  // namespace

TEST(HubStatus, StatusPollReplyRecordsFullBatteryOnASolarMotor) {
  // tests/corpus/captures/statuspoll/somfy_rs100_statuspoll_kig300_sx1276.yaml frame 3 (0x04): data[1] = 0x60.
  TestableHubComponent comp;
  comp.add_device("E461E9");
  const uint8_t payload[14] = {0x04, 0x60, 0x00, 0x00, 0x77, 0xCE, 0x00, 0x0B, 0xBE, 0xFE, 0xDB, 0x01, 0x00, 0x00};

  comp.update_device_status_(make_rs100_status_frame(CMD_PRIVATE_RESP, payload, sizeof(payload)));

  EXPECT_EQ(battery_state_of(*comp.get_device("E461E9")), BatteryState::FULL_CHARGE);
}

TEST(HubStatus, StatusUpdateRecordsTheBatteryAtTheSameOffset) {
  // Same corpus file, frame 2 (0x71): data[1] = 0x60 as in the 0x04 reply.
  TestableHubComponent comp;
  comp.add_device("E461E9");
  const uint8_t payload[16] = {0x04, 0x60, 0x10, 0x0A, 0x0B, 0x00, 0x00, 0xAC,
                               0x9E, 0x00, 0x0F, 0xBE, 0xFE, 0xDB, 0x01, 0x00};

  comp.update_device_status_(make_rs100_status_frame(CMD_STATUS_UPDATE, payload, sizeof(payload)));

  EXPECT_EQ(battery_state_of(*comp.get_device("E461E9")), BatteryState::FULL_CHARGE);
}

TEST(HubStatus, BatteryFollowsALaterLowerLevel) {
  TestableHubComponent comp;
  comp.add_device("E461E9");
  uint8_t payload[14] = {0x04, 0x60, 0x00, 0x00, 0x77, 0xCE, 0x00, 0x0B, 0xBE, 0xFE, 0xDB, 0x01, 0x00, 0x00};
  comp.update_device_status_(make_rs100_status_frame(CMD_PRIVATE_RESP, payload, sizeof(payload)));

  payload[1] = 0x20;  // level 1, the other bits clear
  comp.update_device_status_(make_rs100_status_frame(CMD_PRIVATE_RESP, payload, sizeof(payload)));

  EXPECT_EQ(battery_state_of(*comp.get_device("E461E9")), BatteryState::LOW_CHARGE);
}

TEST(HubStatus, ExecuteAckDoesNotChangeTheBattery) {
  // A non-zero level (0x20 = low) applied as an execute ack must not land, so the result pins the
  // trust_position gate rather than a payload that happens to decode to NOT_REPORTED.
  TestableHubComponent comp;
  comp.add_device("E461E9");
  const uint8_t payload[8] = {0x04, 0x20, 0x00, 0x00, 0x77, 0xCE, 0x00, 0x0B};

  comp.update_device_status_(make_rs100_status_frame(CMD_PRIVATE_RESP, payload, sizeof(payload)),
                             /*trust_position=*/false);

  EXPECT_EQ(battery_state_of(*comp.get_device("E461E9")), BatteryState::NOT_REPORTED);
}

TEST(HubStatus, MainsReplyLeavesTheBatteryNotReported) {
  // Mains Sunea replies read 0x00 or 0x80 at data[1] (somfy_awning_statuspoll_success_sx1262):
  // bit 7 is the status-update flag, not part of the battery field.
  TestableHubComponent comp;
  comp.add_device("E461E9");
  const uint8_t payload[8] = {0x01, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

  comp.update_device_status_(make_rs100_status_frame(CMD_PRIVATE_RESP, payload, sizeof(payload)));

  EXPECT_EQ(battery_state_of(*comp.get_device("E461E9")), BatteryState::NOT_REPORTED);
}

TEST(HubStatus, AReplyThatShowsNoBatteryMarksTheBatteryReadingAbsentAndALaterLevelLiftsIt) {
  TestableHubComponent comp;
  comp.add_device("E461E9");
  IoDevice &dev = *comp.get_device("E461E9");
  uint8_t payload[8] = {0x01, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

  comp.update_device_status_(make_rs100_status_frame(CMD_PRIVATE_RESP, payload, sizeof(payload)));
  EXPECT_TRUE(dev.reading_values.absent(reading_slot(DeviceReading::BATTERY_STATE)));
  EXPECT_FALSE(reading_value(dev, DeviceReading::BATTERY_STATE).has_value());
  EXPECT_FALSE(reading_value(dev, DeviceReading::BATTERY_LOW).has_value())
      << "no state: off must mean a device said so";

  payload[1] = 0x60;
  comp.update_device_status_(make_rs100_status_frame(CMD_PRIVATE_RESP, payload, sizeof(payload)));
  EXPECT_FALSE(dev.reading_values.absent(reading_slot(DeviceReading::BATTERY_STATE)));
  EXPECT_EQ(battery_state_of(dev), BatteryState::FULL_CHARGE);
  EXPECT_EQ(reading_value(dev, DeviceReading::BATTERY_LOW), std::optional<float>(0.0F));
}

TEST(HubStatus, ALaterNoBatteryReplyDropsALevelThatWasShown) {
  TestableHubComponent comp;
  comp.add_device("E461E9");
  uint8_t payload[8] = {0x01, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  comp.update_device_status_(make_rs100_status_frame(CMD_PRIVATE_RESP, payload, sizeof(payload)));
  ASSERT_EQ(reading_value(*comp.get_device("E461E9"), DeviceReading::BATTERY_LOW), std::optional<float>(1.0F));

  payload[1] = 0x00;
  comp.update_device_status_(make_rs100_status_frame(CMD_PRIVATE_RESP, payload, sizeof(payload)));

  EXPECT_EQ(battery_state_of(*comp.get_device("E461E9")), BatteryState::NOT_REPORTED);
  EXPECT_FALSE(reading_value(*comp.get_device("E461E9"), DeviceReading::BATTERY_LOW).has_value());
}

TEST(HubStatus, StatusLineNamesAReportedBatteryLevel) {
  IoDevice dev{};
  dev.reading_values.store(reading_slot(DeviceReading::BATTERY_STATE), static_cast<float>(BatteryState::FULL_CHARGE));
  EXPECT_EQ(detail::describe_battery(dev), " battery=full") << "a reported level is logged with or without the key";
}

TEST(HubStatus, StatusLineNamesAMissingLevelOnlyWhenBatteryEntitiesAreDeclared) {
  IoDevice dev{};
  EXPECT_EQ(detail::describe_battery(dev), "") << "mains devices without `battery: true` keep their line unchanged";

  ask_reading(dev, DeviceReading::BATTERY_STATE);  // what `battery: true` declares
  EXPECT_EQ(detail::describe_battery(dev), " battery=not_reported");
}

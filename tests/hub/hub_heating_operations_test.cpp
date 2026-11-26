/// @file hub_heating_operations_test.cpp
/// @brief Tests for the heater commands of hub_operations.cpp: send_heating_command, register reads and the heater
/// poll, and the level, exterior, temperature, towel dryer and night light heaters.

#include "hub_core.h"
#include "hub_internal.h"
#include "proto_frame.h"
#include "proto_commands.h"
#include "proto_private_registers.h"

#include "corpus_test_helpers.h"
#include "test_helpers.h"
#include "reading_store.h"
#include "stubs/radio_test_common.h"

#include <cmath>
#include <cstring>
#include <functional>
#include <utility>
#include <vector>

#include "hub_operations_fixture.h"

using namespace esphome::home_io_control;
using namespace hub_operations_test;

// ========================================================================================
// send_heating_command tests (CMD_WRITE_PRIVATE 0x20)
// ========================================================================================

/// A device event report (a 0x71 nobody subscribed to) from the heater ABC123 to the hub.
static IoFrame build_device_event_report(const uint8_t dst[3]) {
  IoFrame f{};
  init_frame(f, true, false, true, false);
  const uint8_t device_node_id[3] = {0xAB, 0xC1, 0x23};
  set_src(f, device_node_id);
  set_dst(f, dst);
  const uint8_t payload[4] = {0x20, 0x05, 0x06, 0x2A};
  set_cmd(f, CMD_SET_CONFIG1_RESP, payload, sizeof(payload));
  return f;
}

TEST(HubOperations, HeatingCommandAuthenticatedWithoutFinalResponseIsCountedAsAFailure) {
  // A register write is not an EXECUTE, so a challenged exchange that is never closed counts one
  // exchange failure, as for a status poll that got no payload. The challenge is still a frame from
  // the heater, so last contact is stamped, and the unconfirmed count records it too.
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  test::set_kind(*dev, DeviceType::ELECTRICAL_HEATER, 1);

  // Only the challenge — no closing reply.
  IoFrame challenge = build_challenge_request(dev->node_id, comp.node_id_);
  uint8_t raw[64];
  uint8_t raw_len = serialize(challenge, raw, sizeof(raw));
  RadioRxPacket pkt{};
  pkt.len = raw_len;
  memcpy(pkt.data, raw, raw_len);
  pkt.freq_hz = FREQ_CH2;
  radio.queue_rx(pkt);

  comp.send_heating_command("ABC123", DeviceRegister::COMFORT_SETPOINT, 20.5f);

  EXPECT_EQ(dev->exchange_unconfirmed_count, 1u);
  EXPECT_EQ(dev->exchange_timeout_count, 1u);
  EXPECT_NE(dev->last_seen_ms, 0u) << "the challenge came from the heater";
}

TEST(HubOperations, AnErrorReplyToAHeatingWriteRecordsItsResultCode) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  test::set_kind(*dev, DeviceType::ELECTRICAL_HEATER, 1);
  ASSERT_EQ(dev->last_seen_ms, 0u);
  radio.queue_rx(test::make_rx_packet(build_error_response(comp.node_id_, RESULT_LIMITATION_BY_WIND)));

  EXPECT_FALSE(comp.send_heating_command("ABC123", DeviceRegister::COMFORT_SETPOINT, 20.5f));

  EXPECT_EQ(dev->last_result_code, RESULT_LIMITATION_BY_WIND);
  EXPECT_NE(dev->last_seen_ms, 0u) << "a refusal is still a frame from the heater";
  EXPECT_EQ(dev->exchange_timeout_count, 0u);
}

TEST(HubOperations, ADeviceEventDuringAHeatingWriteIsRecordedAndTheWriteAskedAgain) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  test::set_kind(*dev, DeviceType::ELECTRICAL_HEATER, 1);

  radio.queue_rx(test::make_rx_packet(build_device_event_report(comp.node_id_)));
  radio.queue_rx(test::make_rx_packet(test::make_device_reply(
      comp.node_id_, CMD_WRITE_PRIVATE_ACK, {0x0C, 0x61, 0x01, 0x03, 0xCD, 0x00}, test::DEVICE_ABC123_ID)));

  EXPECT_TRUE(comp.send_heating_command("ABC123", DeviceRegister::COMFORT_SETPOINT, 20.5f));

  EXPECT_EQ(comp.device_event_count(), 1u);
  EXPECT_NE(comp.last_device_event().find(" ABC123 0x"), std::string::npos) << comp.last_device_event();
  EXPECT_EQ(radio.get_sent_data().size(), 2u) << "the request is sent once more after the report";
  EXPECT_EQ(dev->exchange_timeout_count, 0u) << "the device answered, just with something else first";
}

// ========================================================================================
// Heater register reads and the heater poll
// ========================================================================================

namespace {

// Register low byte of a sent CMD_WRITE_PRIVATE frame (payload `0C <dir> 01 <reg>` starts at offset 9).
uint8_t sent_register(const std::vector<uint8_t> &tx) { return tx.at(12); }

IoDevice *make_heater(TestableComponent &comp, MockRadio &radio) {
  setup_cover_component(comp, radio);
  auto *dev = comp.get_device("ABC123");
  EXPECT_NE(dev, nullptr);
  test::set_kind(*dev, DeviceType::ELECTRICAL_HEATER, 1);
  return dev;
}

}  // namespace

TEST(HubOperations, ADeviceEventDuringAHeatingRegisterReadIsRecordedAndTheReadAnswerUsed) {
  TestableComponent comp;
  MockRadio radio;
  auto *dev = make_heater(comp, radio);
  radio.queue_rx(test::make_rx_packet(build_device_event_report(comp.node_id_)));
  radio.queue_rx(test::make_rx_packet(test::make_device_reply(
      comp.node_id_, CMD_WRITE_PRIVATE_ACK, {0x0C, 0x60, 0x01, 0x15, 0x1B, 0x08}, test::DEVICE_ABC123_ID)));

  float value = 0.0F;
  ASSERT_TRUE(comp.read_heating_register("ABC123", DeviceRegister::ROOM_TEMPERATURE, &value, EXCHANGE_RETRY_COUNT));

  EXPECT_FLOAT_EQ(value, 20.75F) << "the answer to the repeated read is decoded";
  EXPECT_EQ(comp.device_event_count(), 1u);
  EXPECT_NE(comp.last_device_event().find(" ABC123 0x"), std::string::npos) << comp.last_device_event();
  ASSERT_EQ(radio.get_sent_data().size(), 2u) << "the read is sent once more after the report";
  EXPECT_EQ(radio.get_sent_data()[1].at(10), PRIVATE_REGISTER_DIRECTION_GET) << "the repeat is the same read";
  EXPECT_EQ(dev->exchange_timeout_count, 0u);
}

TEST(HubOperations, HeaterCadenceTimerHasItsOwnIdAndARemotePollDoesNotCancelIt) {
  const uint8_t node[3] = {0xAB, 0xC1, 0x23};
  EXPECT_NE(decisions::heating_poll_timer_id(node), decisions::remote_poll_timer_id(node));

  TestableComponent comp;
  MockRadio radio;
  make_heater(comp, radio);
  // A failed poll still re-arms the cadence.
  EXPECT_FALSE(comp.request_device_status("ABC123"));
  EXPECT_EQ(comp.last_timeout_id_, decisions::heating_poll_timer_id(node));

  // A linked remote's poll uses the other id, so the pending cadence timer is a different timer.
  comp.schedule_status_poll_("ABC123", 2000);
  EXPECT_EQ(comp.last_timeout_id_, decisions::remote_poll_timer_id(node));
  EXPECT_NE(comp.last_timeout_id_, decisions::heating_poll_timer_id(node));
}

// === Heater poll cadence ===

TEST(HubOperations, ARegisterHeaterPollBeforeTheHubIsInitialisedStillArmsTheNextPoll) {
  TestableComponent comp;
  MockRadio radio;
  const IoDevice *dev = make_heater(comp, radio);
  comp.initialized_ = false;

  EXPECT_FALSE(comp.request_device_status("ABC123"));
  EXPECT_EQ(comp.last_timeout_id_, decisions::heating_poll_timer_id(dev->node_id));
  EXPECT_EQ(comp.last_timeout_ms_, HEATING_POLL_DEFAULT_INTERVAL_MS);
  EXPECT_TRUE(radio.get_sent_data().empty()) << "nothing is sent while the hub is not initialised";
}

// === Heating-level heater (52/0) ===

namespace {

IoDevice *make_level_heater(TestableComponent &comp, MockRadio &radio) {
  setup_cover_component(comp, radio);
  auto *dev = comp.get_device("ABC123");
  EXPECT_NE(dev, nullptr);
  test::set_kind(*dev, DeviceType::ELECTRICAL_HEATER, 0);
  return dev;
}

// A 0x04 status reply whose current main parameter is `mp`.
IoFrame build_level_status_response(const uint8_t dst[3], uint16_t mp, uint8_t flags = test::STATUS_BYTE_AT_REST) {
  IoFrame f{};
  init_frame(f, true, false, true, false);
  uint8_t device_node_id[3] = {0xAB, 0xC1, 0x23};
  set_dst(f, dst);
  set_src(f, device_node_id);
  const uint8_t payload[8] = {test::STATUS_BYTE_AT_REST,
                              0x00,
                              static_cast<uint8_t>(mp >> 8U),
                              static_cast<uint8_t>(mp),
                              static_cast<uint8_t>(mp >> 8U),
                              static_cast<uint8_t>(mp),
                              0x00,
                              0x00};
  set_cmd(f, CMD_PRIVATE_RESP, payload, sizeof(payload));
  return f;
}

}  // namespace

TEST(HubOperations, ALevelHeaterGetsNoRegisterFrameFromAnyEntryPoint) {
  TestableComponent comp;
  MockRadio radio;
  make_level_heater(comp, radio);

  EXPECT_FALSE(comp.send_heating_command("ABC123", DeviceRegister::COMFORT_SETPOINT, 20.5F));
  EXPECT_FALSE(comp.send_heating_command("ABC123", DeviceRegister::PAIRED_DEVICES, 0.0F));
  EXPECT_FALSE(comp.read_heating_register("ABC123", DeviceRegister::ROOM_TEMPERATURE, nullptr, EXCHANGE_RETRY_COUNT));
  EXPECT_TRUE(radio.get_sent_data().empty())
      << "no CMD_WRITE_PRIVATE frame for a heater that takes the standard EXECUTE";
}

TEST(HubOperations, ALevelHeaterIsPolledWithTheStatusReadAndItsLevelIsStored) {
  TestableComponent comp;
  MockRadio radio;
  auto *dev = make_level_heater(comp, radio);
  radio.queue_rx(test::make_rx_packet(
      build_level_status_response(comp.node_id_, heating_level_main_parameter(heating_level::ECO))));

  EXPECT_TRUE(comp.request_device_status("ABC123"));

  ASSERT_EQ(radio.get_sent_data().size(), 1u);
  EXPECT_EQ(radio.get_sent_data().front()[8], CMD_PRIVATE) << "the ordinary 0x03 status read, not a register read";
  EXPECT_EQ(test::wire_read(*dev, DeviceReading::HEATING_LEVEL), heating_level::ECO);
  EXPECT_EQ(comp.last_timeout_id_, decisions::heating_poll_timer_id(dev->node_id)) << "the heater cadence is armed";
}

TEST(HubOperations, AStatusMainParameterThatIsNotALevelLeavesTheLevelUnread) {
  TestableComponent comp;
  MockRadio radio;
  auto *dev = make_level_heater(comp, radio);
  radio.queue_rx(test::make_rx_packet(build_level_status_response(comp.node_id_, 0xC800)));

  EXPECT_TRUE(comp.request_device_status("ABC123"));
  EXPECT_EQ(test::wire_read(*dev, DeviceReading::HEATING_LEVEL), -1);
}

TEST(HubOperations, AFailedLevelHeaterPollMovesOntoTheRetryLadder) {
  TestableComponent comp;
  MockRadio radio;
  make_level_heater(comp, radio);

  EXPECT_FALSE(comp.request_device_status("ABC123"));
  EXPECT_EQ(comp.poll_policy_.get_status_poll_failures("ABC123"), 1u);
  EXPECT_NE(comp.poll_policy_.get_next_update("ABC123"), 0u);
}

TEST(HubOperations, ALevelHeaterPollBeforeTheHubIsInitialisedStillArmsTheNextPoll) {
  TestableComponent comp;
  MockRadio radio;
  const IoDevice *dev = make_level_heater(comp, radio);
  comp.initialized_ = false;

  EXPECT_FALSE(comp.request_device_status("ABC123"));
  EXPECT_EQ(comp.last_timeout_id_, decisions::heating_poll_timer_id(dev->node_id));
  EXPECT_EQ(comp.last_timeout_ms_, HEATING_POLL_DEFAULT_INTERVAL_MS);
  EXPECT_TRUE(radio.get_sent_data().empty());
}

TEST(HubOperations, ALevelHeaterPollOfAnUnregisteredDeviceArmsNoTimer) {
  TestableComponent comp;
  MockRadio radio;
  make_level_heater(comp, radio);

  EXPECT_FALSE(comp.request_device_status("DEF456"));
  EXPECT_EQ(comp.last_timeout_id_, 0u);
}

TEST(HubOperations, ALevelHeaterPollArmsTheNextPollWhetherItSucceedsOrFails) {
  const uint8_t node[3] = {0xAB, 0xC1, 0x23};
  {
    TestableComponent comp;
    MockRadio radio;
    make_level_heater(comp, radio);
    radio.queue_rx(test::make_rx_packet(
        build_level_status_response(comp.node_id_, heating_level_main_parameter(heating_level::ECO))));
    EXPECT_TRUE(comp.request_device_status("ABC123"));
    EXPECT_EQ(comp.last_timeout_id_, decisions::heating_poll_timer_id(node));
    EXPECT_EQ(comp.last_timeout_ms_, HEATING_POLL_DEFAULT_INTERVAL_MS);
  }
  {
    TestableComponent comp;
    MockRadio radio;
    make_level_heater(comp, radio);
    EXPECT_FALSE(comp.request_device_status("ABC123"));
    EXPECT_EQ(comp.last_timeout_id_, decisions::heating_poll_timer_id(node));
    EXPECT_EQ(comp.last_timeout_ms_, HEATING_POLL_DEFAULT_INTERVAL_MS);
  }
}

TEST(HubOperations, SetHeatingLevelSendsTheLevelInTheMainParameter) {
  TestableComponent comp;
  MockRadio radio;
  make_level_heater(comp, radio);
  radio.queue_rx(test::make_rx_packet(
      build_level_status_response(comp.node_id_, heating_level_main_parameter(heating_level::COMFORT))));

  EXPECT_TRUE(comp.heater_controller_.set_heating_level("ABC123", heating_level::COMFORT));

  ASSERT_GE(radio.get_sent_data().size(), 1u);
  const auto &tx = radio.get_sent_data().front();
  EXPECT_EQ(tx[8], CMD_EXECUTE);
  EXPECT_EQ(tx[11], 0xFC) << "main parameter high byte";
  EXPECT_EQ(tx[12], heating_level::COMFORT);
  EXPECT_EQ(tx[10], 0xE7) << "ACEI of the vendor bridge's level frame";
}

TEST(HubOperations, ALevelHeaterIsNeverInTheMovementSettleLoop) {
  TestableComponent comp;
  MockRadio radio;
  auto *dev = make_level_heater(comp, radio);
  // Both replies say "moving" (no STATUS_STOPPED bit); a heater has no motion, so that must not
  // start a series of follow-up polls.
  const uint16_t mp = heating_level_main_parameter(heating_level::COMFORT);
  radio.queue_rx(
      test::make_rx_packet(build_level_status_response(comp.node_id_, mp, /*flags=*/test::STATUS_BYTE_MOVING)));

  EXPECT_TRUE(comp.heater_controller_.set_heating_level("ABC123", heating_level::COMFORT));
  EXPECT_TRUE(dev->is_stopped) << "the heater is at rest whatever its flags byte says";
  EXPECT_NE(comp.poll_policy_.get_next_update("ABC123"), 0u) << "exactly one read-back poll is scheduled";

  radio.queue_rx(
      test::make_rx_packet(build_level_status_response(comp.node_id_, mp, /*flags=*/test::STATUS_BYTE_MOVING)));
  EXPECT_TRUE(comp.request_device_status("ABC123"));
  EXPECT_EQ(test::wire_read(*dev, DeviceReading::HEATING_LEVEL), heating_level::COMFORT);
  EXPECT_EQ(comp.poll_policy_.get_next_update("ABC123"), 0u) << "the read-back ends the settle loop";
  EXPECT_FALSE(comp.poll_policy_.is_tracking_active("ABC123", esphome::millis()));
}

// === Interface for pilot-wire heaters (52/0, model PILOT_WIRE_INTERFACE) ===

namespace {

IoDevice *make_pilot_wire_interface(TestableComponent &comp, MockRadio &radio) {
  setup_cover_component(comp, radio);
  auto *dev = comp.get_device("ABC123");
  EXPECT_NE(dev, nullptr);
  test::set_kind(*dev, DeviceType::ELECTRICAL_HEATER, 0, DeviceModel::PILOT_WIRE_INTERFACE);
  return dev;
}

}  // namespace

// Its status carries no level and it rarely answers one (issue #228): the type table gives it no status read.
TEST(HubOperations, APilotWireInterfaceIsSentNoStatusReadFromAnyEntryPoint) {
  esphome::test_clock::ManualClock clock{200000};
  TestableComponent comp;
  MockRadio radio;
  esphome::test_scheduler::clear();
  auto *dev = make_pilot_wire_interface(comp, radio);
  ASSERT_TRUE(has_feature(dev->features, device_feature::NO_STATUS_READ));

  comp.queue_request_device_status("ABC123");  // the read after boot and the heater's regular poll
  EXPECT_TRUE(comp.op_queue_.empty());
  EXPECT_FALSE(comp.request_device_status("ABC123"));  // a refresh
  // The Cozytouch bridge commanding the interface, and the interface answering the bridge's challenge.
  comp.act_on_foreign_frame_("ABC123", ForeignFrameKind::REQUEST_TO_DEVICE, "8F5BA3", CMD_EXECUTE);
  comp.act_on_foreign_frame_("ABC123", ForeignFrameKind::DEVICE_CHALLENGE_TO_OTHER, "8F5BA3", CMD_CHALLENGE_REQ);
  EXPECT_FALSE(comp.poll_policy_.is_tracking_active("ABC123", esphome::millis()));

  esphome::test_scheduler::run_until(esphome::millis() + 3600000);
  EXPECT_TRUE(comp.op_queue_.empty());
  EXPECT_TRUE(radio.get_sent_data().empty());
  esphome::test_scheduler::clear();
}

TEST(HubOperations, ALevelCommandStillReachesAPilotWireInterfaceWithNoReadBackAfterIt) {
  esphome::test_clock::ManualClock clock{200000};
  TestableComponent comp;
  MockRadio radio;
  esphome::test_scheduler::clear();
  make_pilot_wire_interface(comp, radio);
  radio.queue_rx(test::make_rx_packet(
      build_level_status_response(comp.node_id_, heating_level_main_parameter(heating_level::ECO))));

  EXPECT_TRUE(comp.heater_controller_.set_heating_level("ABC123", heating_level::ECO));

  ASSERT_EQ(radio.get_sent_data().size(), 1u);
  EXPECT_EQ(radio.get_sent_data().front()[8], CMD_EXECUTE);
  EXPECT_EQ(radio.get_sent_data().front()[12], heating_level::ECO);
  EXPECT_EQ(comp.poll_policy_.get_next_update("ABC123"), 0u) << "no read-back poll is scheduled";
  esphome::test_scheduler::run_until(esphome::millis() + 3600000);
  EXPECT_TRUE(comp.op_queue_.empty());
  EXPECT_EQ(radio.get_sent_data().size(), 1u) << "only the command went out";
  esphome::test_scheduler::clear();
}

// === Exterior heating (21/0, 21/58): heat level as an IO position ===

namespace {

IoDevice *make_exterior_heater(TestableComponent &comp, MockRadio &radio, uint8_t subtype) {
  setup_cover_component(comp, radio);
  auto *dev = comp.get_device("ABC123");
  EXPECT_NE(dev, nullptr);
  test::set_kind(*dev, DeviceType::EXTERIOR_HEATING, subtype);
  return dev;
}

// A 0x04 status reply whose current and target position are `position_percent`.
IoFrame build_position_status_response(const uint8_t dst[3], uint8_t position_percent, uint8_t flags) {
  IoFrame f{};
  init_frame(f, true, false, true, false);
  uint8_t device_node_id[3] = {0xAB, 0xC1, 0x23};
  set_dst(f, dst);
  set_src(f, device_node_id);
  const uint8_t msb = static_cast<uint8_t>(2 * position_percent);
  const uint8_t payload[8] = {flags, 0x00, msb, 0x00, msb, 0x00, 0x00, 0x00};
  set_cmd(f, CMD_PRIVATE_RESP, payload, sizeof(payload));
  return f;
}

}  // namespace

TEST(HubOperations, ExteriorHeatingHeatLevelTravelsAsTheInvertedPosition) {
  // Heat p % is IO position 100 - p; the wire main parameter is position x 0x200 (0 % heat = 0xC800).
  const struct {
    uint8_t heat;
    uint8_t position;
    uint8_t mp_high;
  } cases[] = {{0, 100, 0xC8}, {1, 99, 0xC6}, {50, 50, 0x64}, {99, 1, 0x02}, {100, 0, 0x00}};
  for (const auto &tc : cases) {
    TestableComponent comp;
    MockRadio radio;
    make_exterior_heater(comp, radio, 0);
    radio.queue_rx(
        test::make_rx_packet(build_position_status_response(comp.node_id_, tc.position, test::STATUS_BYTE_AT_REST)));

    EXPECT_TRUE(comp.set_fan_position("ABC123", tc.position)) << +tc.heat;

    ASSERT_GE(radio.get_sent_data().size(), 1u);
    const auto &tx = radio.get_sent_data().front();
    EXPECT_EQ(tx[8], CMD_EXECUTE);
    EXPECT_EQ(tx[11], tc.mp_high) << "heat " << +tc.heat << " %";
    EXPECT_EQ(tx[12], 0x00);
  }
}

TEST(HubOperations, EveryExteriorHeaterIsSentTheMainParameterOnly) {
  for (const uint8_t subtype : {0, 58}) {
    TestableComponent comp;
    MockRadio radio;
    make_exterior_heater(comp, radio, subtype);
    radio.queue_rx(test::make_rx_packet(build_position_status_response(comp.node_id_, 0, test::STATUS_BYTE_AT_REST)));

    EXPECT_TRUE(comp.set_fan_position("ABC123", 0)) << +subtype;

    const auto &tx = radio.get_sent_data().front();
    EXPECT_EQ(tx[8], CMD_EXECUTE);
    EXPECT_EQ(tx[11], 0x00) << "main parameter: full heat";
    EXPECT_EQ(tx[12], 0x00);
    // 6-byte payload (originator, ACEI, MP, MP, 0, 0): no functional-parameter block, so the frame is
    // as short as a cover's STOP.
    IoFrame frame{};
    ASSERT_TRUE(parse(tx.data(), static_cast<uint8_t>(tx.size()), frame));
    EXPECT_EQ(frame.data_len, 6u) << "subtype " << +subtype;
  }
}

TEST(HubOperations, SetFanPositionRefusesWhatIsNotAnExteriorHeater) {
  TestableComponent cover;
  MockRadio cover_radio;
  setup_cover_component(cover, cover_radio);  // ROLLER_SHUTTER
  EXPECT_FALSE(cover.set_fan_position("ABC123", 0));

  TestableComponent heater;
  MockRadio heater_radio;
  make_heater(heater, heater_radio);  // 52/1 climate
  EXPECT_FALSE(heater.set_fan_position("ABC123", 0));
  EXPECT_TRUE(cover_radio.get_sent_data().empty());
  EXPECT_TRUE(heater_radio.get_sent_data().empty());
}

TEST(HubOperations, ExteriorHeatingGetsNoHeatingFrameFromAnyClimateEntryPoint) {
  TestableComponent comp;
  MockRadio radio;
  make_exterior_heater(comp, radio, 0);
  EXPECT_FALSE(comp.send_heating_command("ABC123", DeviceRegister::COMFORT_SETPOINT, 20.0F));
  EXPECT_FALSE(comp.read_heating_register("ABC123", DeviceRegister::ROOM_TEMPERATURE, nullptr, EXCHANGE_RETRY_COUNT));
  EXPECT_FALSE(comp.heater_controller_.set_heating_level("ABC123", heating_level::COMFORT));
  EXPECT_TRUE(radio.get_sent_data().empty());
}

TEST(HubOperations, ExteriorHeatingIsPolledWithTheStatusReadAndDecodedLikeALight) {
  TestableComponent comp;
  MockRadio radio;
  auto *dev = make_exterior_heater(comp, radio, 0);
  radio.queue_rx(test::make_rx_packet(build_position_status_response(comp.node_id_, 30, test::STATUS_BYTE_AT_REST)));

  EXPECT_TRUE(comp.request_device_status("ABC123"));

  ASSERT_EQ(radio.get_sent_data().size(), 1u);
  EXPECT_EQ(radio.get_sent_data().front()[8], CMD_PRIVATE) << "the ordinary 0x03 status read";
  EXPECT_FLOAT_EQ(dev->position, 30.0F) << "heat 70 %";
}

TEST(HubOperations, ExteriorHeatingReplyThatSaysMovingGetsOneFollowUpPollNotASeries) {
  TestableComponent comp;
  MockRadio radio;
  auto *dev = make_exterior_heater(comp, radio, 0);
  radio.queue_rx(
      test::make_rx_packet(build_position_status_response(comp.node_id_, 0, /*flags=*/test::STATUS_BYTE_MOVING)));

  EXPECT_TRUE(comp.set_fan_position("ABC123", 0));
  EXPECT_TRUE(dev->is_stopped) << "an exterior heater has no motion, whatever its flags byte says";
  EXPECT_NE(comp.poll_policy_.get_next_update("ABC123"), 0u) << "one read-back poll is scheduled";

  radio.queue_rx(
      test::make_rx_packet(build_position_status_response(comp.node_id_, 0, /*flags=*/test::STATUS_BYTE_MOVING)));
  EXPECT_TRUE(comp.request_device_status("ABC123"));
  EXPECT_EQ(comp.poll_policy_.get_next_update("ABC123"), 0u) << "the read-back ends the settle loop";
  EXPECT_FALSE(comp.poll_policy_.is_tracking_active("ABC123", esphome::millis()));
}

TEST(HubOperations, QueuedFanPositionIsDispatchedToTheHeater) {
  TestableComponent comp;
  MockRadio radio;
  make_exterior_heater(comp, radio, 0);
  radio.queue_rx(test::make_rx_packet(build_position_status_response(comp.node_id_, 40, test::STATUS_BYTE_AT_REST)));

  comp.queue_set_fan_position("ABC123", 40);
  ASSERT_EQ(comp.op_queue_.size(), 1u);
  EXPECT_EQ(comp.op_queue_.front().type, PendingOperationType::SET_FAN_POSITION);
  comp.process_pending_operation_();
  EXPECT_TRUE(comp.op_queue_.empty());
  ASSERT_GE(radio.get_sent_data().size(), 1u);
  EXPECT_EQ(radio.get_sent_data().front()[11], 0x50) << "position 40 -> 0x50";
}

TEST(HubOperations, QueuedFanPositionIsRejectedForAnotherDeviceClass) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  comp.queue_set_fan_position("ABC123", 40);
  EXPECT_TRUE(comp.op_queue_.empty());
}

// === Temperature in the main parameter (14, 22/0) ===

namespace {

IoDevice *make_temperature_heater(TestableComponent &comp, MockRadio &radio, DeviceType type) {
  setup_cover_component(comp, radio);
  auto *dev = comp.get_device("ABC123");
  EXPECT_NE(dev, nullptr);
  test::set_kind(*dev, type, 0);
  return dev;
}

}  // namespace

TEST(HubOperations, TemperatureHeatersGetNoRegisterFrameFromAnyEntryPoint) {
  for (const DeviceType type : {DeviceType::HEATING_TEMPERATURE_INTERFACE, DeviceType::HEAT_PUMP}) {
    TestableComponent comp;
    MockRadio radio;
    make_temperature_heater(comp, radio, type);
    EXPECT_FALSE(comp.send_heating_command("ABC123", DeviceRegister::COMFORT_SETPOINT, 20.5F));
    EXPECT_FALSE(comp.read_heating_register("ABC123", DeviceRegister::ROOM_TEMPERATURE, nullptr, EXCHANGE_RETRY_COUNT));
    EXPECT_FALSE(comp.heater_controller_.set_heating_level("ABC123", heating_level::COMFORT));
    EXPECT_TRUE(radio.get_sent_data().empty())
        << "no CMD_WRITE_PRIVATE frame for a heater that takes the standard EXECUTE";
  }
}

TEST(HubOperations, SetHeatingTemperatureSendsTheMainParameterAloneOrWithFp9OrFp16) {
  TestableComponent plain;
  MockRadio plain_radio;
  make_temperature_heater(plain, plain_radio, DeviceType::HEAT_PUMP);
  plain_radio.queue_rx(test::make_rx_packet(build_level_status_response(plain.node_id_, 25600)));
  EXPECT_TRUE(plain.heater_controller_.set_heating_temperature("ABC123", 25600, HeatingFp{}));
  const auto &tx = plain_radio.get_sent_data().front();
  EXPECT_EQ(tx[8], CMD_EXECUTE);
  EXPECT_EQ(tx[11], 0x64);
  EXPECT_EQ(tx[12], 0x00);

  TestableComponent fp9;
  MockRadio fp9_radio;
  make_temperature_heater(fp9, fp9_radio, DeviceType::HEATING_TEMPERATURE_INTERFACE);
  fp9_radio.queue_rx(test::make_rx_packet(build_level_status_response(fp9.node_id_, 25600)));
  EXPECT_TRUE(fp9.heater_controller_.set_heating_temperature(
      "ABC123", 25600, HeatingFp{HeatingFpKind::FP9, heating_mode_flags::MANUAL}));
  const auto &tx9 = fp9_radio.get_sent_data().front();
  EXPECT_EQ(tx9.size() - tx.size(), 2u) << "FP9 with both bitmaps and a two-byte value: 8 bytes against 6";
  EXPECT_EQ(tx9[13], 0x00) << "FP1-FP8 bitmap";
  EXPECT_EQ(tx9[14], 0x80) << "FP9 present";
  EXPECT_EQ(tx9[15], 0x00) << "FP9 high byte";
  EXPECT_EQ(tx9[16], 0x02) << "FP9: manual";

  TestableComponent fp16;
  MockRadio fp16_radio;
  make_temperature_heater(fp16, fp16_radio, DeviceType::HEAT_PUMP);
  fp16_radio.queue_rx(test::make_rx_packet(build_level_status_response(fp16.node_id_, 0xD80F)));
  EXPECT_TRUE(fp16.heater_controller_.set_heating_temperature("ABC123", 0xD80F,
                                                              HeatingFp{HeatingFpKind::FP16, heating_fp16::ON}));
  const auto &tx16 = fp16_radio.get_sent_data().front();
  EXPECT_EQ(tx16.size() - tx.size(), 2u) << "FP16 is two bytes behind the two bitmaps: 8 bytes against 6";
  EXPECT_EQ(tx16[11], 0xD8);
  EXPECT_EQ(tx16[12], 0x0F);
  EXPECT_EQ(tx16[13], 0x00) << "FP1-FP8 bitmap";
  EXPECT_EQ(tx16[14], 0x01) << "FP16 present: the last bit of the second bitmap";
  EXPECT_EQ(tx16[15], 0x40);
  EXPECT_EQ(tx16[16], 0x00) << "FP16: on";
}

TEST(HubOperations, SetHeatingTemperatureRefusesEveryOtherKindOfDevice) {
  TestableComponent cover;
  MockRadio cover_radio;
  setup_cover_component(cover, cover_radio);
  EXPECT_FALSE(cover.heater_controller_.set_heating_temperature("ABC123", 25600, HeatingFp{}));

  TestableComponent level;
  MockRadio level_radio;
  make_level_heater(level, level_radio);
  EXPECT_FALSE(level.heater_controller_.set_heating_temperature("ABC123", 25600, HeatingFp{}));

  TestableComponent registers;
  MockRadio register_radio;
  make_heater(registers, register_radio);
  EXPECT_FALSE(registers.heater_controller_.set_heating_temperature("ABC123", 25600, HeatingFp{}));
  EXPECT_TRUE(cover_radio.get_sent_data().empty());
  EXPECT_TRUE(level_radio.get_sent_data().empty());
  EXPECT_TRUE(register_radio.get_sent_data().empty());
}

TEST(HubOperations, ATemperatureStatusReplyIsDecodedOverTheDevicesRange) {
  TestableComponent comp;
  MockRadio radio;
  auto *dev = make_temperature_heater(comp, radio, DeviceType::HEATING_TEMPERATURE_INTERFACE);
  radio.queue_rx(test::make_rx_packet(build_level_status_response(comp.node_id_, 25600)));  // half way
  EXPECT_TRUE(comp.request_device_status("ABC123"));
  EXPECT_NEAR(test::number_read(*dev, DeviceReading::STATUS_SETPOINT), 20.0F, 0.003F) << "default range -40..80";
  EXPECT_EQ(test::wire_read(*dev, DeviceReading::HEATING_LEVEL), -1);

  comp.set_device_temperature_range("ABC123", 10.0F, 30.0F);
  radio.queue_rx(test::make_rx_packet(build_level_status_response(comp.node_id_, 25600)));
  EXPECT_TRUE(comp.request_device_status("ABC123"));
  EXPECT_NEAR(test::number_read(*dev, DeviceReading::STATUS_SETPOINT), 20.0F, 0.003F);
  radio.queue_rx(test::make_rx_packet(build_level_status_response(comp.node_id_, 12800)));
  EXPECT_TRUE(comp.request_device_status("ABC123"));
  EXPECT_NEAR(test::number_read(*dev, DeviceReading::STATUS_SETPOINT), 15.0F, 0.003F) << "a quarter of 10..30";
}

TEST(HubOperations, AnAliasStatusReplyIsKeptAsAnAliasNotATemperature) {
  TestableComponent comp;
  MockRadio radio;
  auto *dev = make_temperature_heater(comp, radio, DeviceType::HEAT_PUMP);
  test::store_reading(*dev, DeviceReading::STATUS_SETPOINT, 19.0F);
  radio.queue_rx(test::make_rx_packet(build_level_status_response(comp.node_id_, 0xD80E)));
  EXPECT_TRUE(comp.request_device_status("ABC123"));
  EXPECT_EQ(test::wire_read(*dev, DeviceReading::HEATING_LEVEL), heating_alias::HEAT_PUMP_SETBACK);
  EXPECT_FLOAT_EQ(test::number_read(*dev, DeviceReading::STATUS_SETPOINT), 19.0F)
      << "an alias says nothing about the temperature";

  // A plain temperature afterwards clears the alias.
  radio.queue_rx(test::make_rx_packet(build_level_status_response(comp.node_id_, 25600)));
  EXPECT_TRUE(comp.request_device_status("ABC123"));
  EXPECT_EQ(test::wire_read(*dev, DeviceReading::HEATING_LEVEL), -1);
}

TEST(HubOperations, ATemperatureHeaterReplyThatSaysMovingGetsOneFollowUpPoll) {
  TestableComponent comp;
  MockRadio radio;
  auto *dev = make_temperature_heater(comp, radio, DeviceType::HEATING_TEMPERATURE_INTERFACE);
  radio.queue_rx(
      test::make_rx_packet(build_level_status_response(comp.node_id_, 25600, /*flags=*/test::STATUS_BYTE_MOVING)));

  EXPECT_TRUE(comp.heater_controller_.set_heating_temperature(
      "ABC123", 25600, HeatingFp{HeatingFpKind::FP9, heating_mode_flags::MANUAL}));
  EXPECT_TRUE(dev->is_stopped) << "a thermostat has no motion, whatever its flags byte says";
  EXPECT_NE(comp.poll_policy_.get_next_update("ABC123"), 0u) << "one read-back poll is scheduled";

  radio.queue_rx(
      test::make_rx_packet(build_level_status_response(comp.node_id_, 25600, /*flags=*/test::STATUS_BYTE_MOVING)));
  EXPECT_TRUE(comp.request_device_status("ABC123"));
  EXPECT_EQ(comp.poll_policy_.get_next_update("ABC123"), 0u);
  EXPECT_FALSE(comp.poll_policy_.is_tracking_active("ABC123", esphome::millis()));
}

TEST(HubOperations, ATemperatureHeaterPollIsTheGenericStatusReadOnTheHeaterCadence) {
  for (const DeviceType type : {DeviceType::HEATING_TEMPERATURE_INTERFACE, DeviceType::HEAT_PUMP}) {
    {
      TestableComponent comp;
      MockRadio radio;
      const IoDevice *dev = make_temperature_heater(comp, radio, type);
      comp.initialized_ = false;
      EXPECT_FALSE(comp.request_device_status("ABC123"));
      EXPECT_EQ(comp.last_timeout_id_, decisions::heating_poll_timer_id(dev->node_id))
          << "the cadence continues before the hub is initialised";
      EXPECT_EQ(comp.last_timeout_ms_, HEATING_POLL_DEFAULT_INTERVAL_MS);
      EXPECT_TRUE(radio.get_sent_data().empty());
    }
    {
      TestableComponent comp;
      MockRadio radio;
      const IoDevice *dev = make_temperature_heater(comp, radio, type);
      // Nothing queued: the read fails, and the cadence continues all the same.
      EXPECT_FALSE(comp.request_device_status("ABC123"));
      EXPECT_EQ(comp.last_timeout_id_, decisions::heating_poll_timer_id(dev->node_id));

      IoFrame generic{};
      ASSERT_TRUE(create_get_status(generic, comp.node_id_, dev->node_id, dev->low_power));
      ASSERT_FALSE(radio.get_sent_data().empty());
      for (const auto &tx : radio.get_sent_data()) {
        ASSERT_EQ(tx.size(), FRAME_MIN_SIZE + generic.data_len) << "the generic status request, not the tilt variant";
        EXPECT_EQ(tx[8], CMD_PRIVATE);
        EXPECT_EQ(std::vector<uint8_t>(tx.begin() + 9, tx.end()),
                  std::vector<uint8_t>(generic.data, generic.data + generic.data_len));
      }
    }
  }
}

TEST(HubOperations, SetHeatingLevelRefusesARegisterHeaterAndACover) {
  TestableComponent heater;
  MockRadio heater_radio;
  make_heater(heater, heater_radio);  // 52/1
  EXPECT_FALSE(heater.heater_controller_.set_heating_level("ABC123", heating_level::COMFORT));
  EXPECT_TRUE(heater_radio.get_sent_data().empty());

  TestableComponent cover;
  MockRadio cover_radio;
  setup_cover_component(cover, cover_radio);
  EXPECT_FALSE(cover.heater_controller_.set_heating_level("ABC123", heating_level::COMFORT));
  EXPECT_TRUE(cover_radio.get_sent_data().empty());
}

// ============================================================================
// Towel dryer (52/1, model TOWEL_DRYER), the raw register read and the model check
// ============================================================================

namespace {

IoDevice *make_towel_dryer(TestableComponent &comp, MockRadio &radio) {
  IoDevice *dev = make_heater(comp, radio);
  test::set_kind(*dev, DeviceType::ELECTRICAL_HEATER, 1, DeviceModel::TOWEL_DRYER);
  return dev;
}

void queue_register_reply(TestableComponent &comp, MockRadio &radio, const std::vector<uint8_t> &payload) {
  radio.queue_rx(test::make_rx_packet(
      test::make_device_reply(comp.node_id_, CMD_WRITE_PRIVATE_ACK, payload, test::DEVICE_ABC123_ID)));
}

}  // namespace

namespace {

// A 0x04 reply to the main-parameter status read (create_status_mp_fp_read()): the main parameter follows the
// status byte, and the two functional-parameter echoes read 00 (the layout of corpus
// atlantic_adelis_probe_status_mp_fp_programme).
IoFrame build_towel_status_response(const uint8_t dst[3], uint16_t mp) {
  IoFrame f{};
  init_frame(f, true, false, true, false);
  uint8_t device_node_id[3] = {0xAB, 0xC1, 0x23};
  set_dst(f, dst);
  set_src(f, device_node_id);
  const uint8_t payload[6] = {test::STATUS_BYTE_AT_REST, 0x00, static_cast<uint8_t>(mp >> 8U),
                              static_cast<uint8_t>(mp),  0x00, 0x00};
  set_cmd(f, CMD_PRIVATE_RESP, payload, sizeof(payload));
  return f;
}

/// Stand-in for the status read in the list of sent registers: it is no register read.
constexpr uint8_t STATUS_READ = 0xFF;

/// What one sent frame was: a register's low byte, or STATUS_READ for the main-parameter status read.
uint8_t sent_step(const std::vector<uint8_t> &tx) { return tx.at(8) == CMD_PRIVATE ? STATUS_READ : sent_register(tx); }

constexpr uint16_t LEVEL_COMFORT_MP = 0xFC05;  ///< The status read's main parameter in a comfort period.
constexpr uint16_t LEVEL_ECO_MP = 0xFC02;      ///< The status read's main parameter in an eco period.
constexpr uint16_t LEVEL_BOOST_MP = 0xFC07;    ///< The status read's main parameter in a boost.

}  // namespace

TEST(HubOperations, TowelDryerPollReadsItsOwnRegistersThenTheBootRegisterTheLevelAndTheModelName) {
  TestableComponent comp;
  MockRadio radio;
  IoDevice *dev = make_towel_dryer(comp, radio);
  EXPECT_FALSE(has_feature(dev->features, device_feature::WINDOW_COMPANIONS)) << "the model row has no window";
  // Values from the BD684B sweep with drying on (issue #202).
  queue_register_reply(comp, radio, {0x0C, 0x60, 0x01, 0x15, 0xB7, 0x07});  // 19.75 C
  queue_register_reply(comp, radio, {0x0C, 0x60, 0x01, 0x03, 0xBE, 0x00});  // 19.0 C
  queue_register_reply(comp, radio, {0x0C, 0x60, 0x01, 0x36, 0x03});        // MANUEL
  queue_register_reply(comp, radio, {0x0C, 0x60, 0x01, 0x3E, 0x02});        // drying
  queue_register_reply(comp, radio, {0x0C, 0x60, 0x01, 0x26, 0x64});        // 100 %
  queue_register_reply(comp, radio, {0x0C, 0x60, 0x01, 0x02, 0x18, 0x01});  // 28.0 C
  radio.queue_rx(test::make_rx_packet(build_towel_status_response(comp.node_id_, LEVEL_BOOST_MP)));
  queue_register_reply(comp, radio, corpus_test::adelis_name_reply());

  EXPECT_TRUE(comp.request_device_status("ABC123"));

  const uint8_t expected_steps[] = {0x15, 0x03, 0x36, 0x3E, 0x26, 0x02, STATUS_READ, 0x19};
  ASSERT_EQ(radio.get_sent_data().size(), std::size(expected_steps))
      << "no window registers, no setback, and the mode register is not asked twice for the model check";
  for (size_t i = 0; i < std::size(expected_steps); i++)
    EXPECT_EQ(sent_step(radio.get_sent_data()[i]), expected_steps[i]) << "step " << i;
  for (const auto &tx : radio.get_sent_data()) {
    if (sent_step(tx) != STATUS_READ) {
      EXPECT_EQ(tx.at(10), PRIVATE_REGISTER_DIRECTION_GET) << "a poll only reads";
    }
  }
  EXPECT_FLOAT_EQ(test::number_read(*dev, DeviceReading::ROOM_TEMPERATURE), 19.75F);
  EXPECT_EQ(test::wire_read(*dev, DeviceReading::TOWEL_MODE), 3);
  EXPECT_EQ(test::wire_read(*dev, DeviceReading::TEMPORARY_STATE), 2);
  EXPECT_EQ(test::wire_read(*dev, DeviceReading::WORKING_RATE), 100);
  EXPECT_EQ(test::wire_read(*dev, DeviceReading::HEATING_LEVEL), heating_level::BOOST)
      << "the level the heater applies, from the status read";
  EXPECT_FALSE(test::has_reading(*dev, DeviceRegister::TOWEL_TEMPERATURE_IN_FORCE))
      << "drying: no temperature in force is read";
  EXPECT_EQ(test::wire_read(*dev, DeviceReading::MODE), -1) << "the radiator's mode register is never read";
  EXPECT_TRUE(dev->model_checked);

  const size_t sent_before = radio.get_sent_data().size();
  EXPECT_FALSE(comp.request_device_status("ABC123"));  // nothing queued: only the first register is tried
  for (size_t i = sent_before; i < radio.get_sent_data().size(); i++)
    EXPECT_EQ(sent_register(radio.get_sent_data()[i]), 0x15);
}

namespace {

/// One towel-dryer poll: the five core registers (mode and temporary state as given), answered, and the
/// frames it sent.
struct TowelPoll {
  TowelDryerMode mode;
  HeatingTemporaryState state;
  std::vector<uint8_t> in_force_reply;  ///< Answer to register 0x0137 when it is asked; empty queues none.
  std::optional<uint16_t> level_mp;     ///< Main parameter of the status read's answer; none queues no answer.
  std::vector<std::vector<uint8_t>> boot_replies;   ///< Answers to the boot registers (first poll only).
  std::vector<std::vector<uint8_t>> model_replies;  ///< Answers to the model check (first poll only).
};

/// Run one poll of the towel dryer; returns what it sent, in order (a register's low byte, STATUS_READ).
std::vector<uint8_t> run_towel_poll(TestableComponent &comp, MockRadio &radio, const TowelPoll &poll) {
  const size_t before = radio.get_sent_data().size();
  queue_register_reply(comp, radio, {0x0C, 0x60, 0x01, 0x15, 0xB7, 0x07});
  queue_register_reply(comp, radio, {0x0C, 0x60, 0x01, 0x03, 0xC8, 0x00});  // comfort setpoint 20.0 C
  queue_register_reply(comp, radio, {0x0C, 0x60, 0x01, 0x36, static_cast<uint8_t>(poll.mode)});
  queue_register_reply(comp, radio, {0x0C, 0x60, 0x01, 0x3E, static_cast<uint8_t>(poll.state)});
  queue_register_reply(comp, radio, {0x0C, 0x60, 0x01, 0x26, 0x00});
  if (!poll.in_force_reply.empty())
    queue_register_reply(comp, radio, poll.in_force_reply);
  for (const auto &reply : poll.boot_replies)
    queue_register_reply(comp, radio, reply);
  if (poll.level_mp.has_value())
    radio.queue_rx(test::make_rx_packet(build_towel_status_response(comp.node_id_, *poll.level_mp)));
  for (const auto &reply : poll.model_replies)
    queue_register_reply(comp, radio, reply);
  comp.request_device_status("ABC123");
  std::vector<uint8_t> steps;
  for (size_t i = before; i < radio.get_sent_data().size(); i++)
    steps.push_back(sent_step(radio.get_sent_data()[i]));
  return steps;
}

const std::vector<uint8_t> IN_FORCE_COMFORT = {0x0C, 0x60, 0x01, 0x37, 0xC8, 0x00};  // 20.0 C
const std::vector<uint8_t> IN_FORCE_ECO = {0x0C, 0x60, 0x01, 0x37, 0xA5, 0x00};      // 16.5 C

/// A towel dryer whose boot registers and model check are done, so a poll sends only its recurring reads.
IoDevice *make_settled_towel_dryer(TestableComponent &comp, MockRadio &radio) {
  IoDevice *dev = make_towel_dryer(comp, radio);
  dev->model_checked = true;
  test::store_reading(*dev, DeviceReading::MAX_SETPOINT, 28.0F);
  return dev;
}

}  // namespace

TEST(HubOperations, TowelDryerPollReadsTheTemperatureInForceOnlyWhileTheProgrammeRunsAndTheLevelInEveryMode) {
  TestableComponent comp;
  MockRadio radio;
  IoDevice *dev = make_towel_dryer(comp, radio);
  const auto PROG = TowelDryerMode::PROG;
  const auto NONE = HeatingTemporaryState::NONE;
  const std::vector<uint8_t> boot_max = {0x0C, 0x60, 0x01, 0x02, 0x18, 0x01};

  // A comfort period: the core registers, 0x0137, the boot register (first poll only), the status read, the
  // model name.
  EXPECT_EQ(
      run_towel_poll(comp, radio,
                     {PROG, NONE, IN_FORCE_COMFORT, LEVEL_COMFORT_MP, {boot_max}, {corpus_test::adelis_name_reply()}}),
      (std::vector<uint8_t>{0x15, 0x03, 0x36, 0x3E, 0x26, 0x37, 0x02, STATUS_READ, 0x19}));
  EXPECT_FLOAT_EQ(test::number_read(*dev, DeviceReading::TOWEL_TEMPERATURE_IN_FORCE), 20.0F);
  EXPECT_EQ(test::wire_read(*dev, DeviceReading::HEATING_LEVEL), heating_level::COMFORT);
  EXPECT_TRUE(heating_device_facts(dev, true).programme_runs);

  // An eco period: the same reads, the level is eco and the target the temperature in force.
  EXPECT_EQ(run_towel_poll(comp, radio, {PROG, NONE, IN_FORCE_ECO, LEVEL_ECO_MP, {}, {}}),
            (std::vector<uint8_t>{0x15, 0x03, 0x36, 0x3E, 0x26, 0x37, STATUS_READ}));
  EXPECT_FLOAT_EQ(test::number_read(*dev, DeviceReading::TOWEL_TEMPERATURE_IN_FORCE), 16.5F);
  EXPECT_EQ(test::wire_read(*dev, DeviceReading::HEATING_LEVEL), heating_level::ECO);
  EXPECT_TRUE(heating_device_facts(dev, true).programme_runs);

  // Boost on top of the programme: no 0x0137 read, the temperature in force goes, the level reads boost.
  EXPECT_EQ(run_towel_poll(comp, radio, {PROG, HeatingTemporaryState::BOOST, {}, LEVEL_BOOST_MP, {}, {}}),
            (std::vector<uint8_t>{0x15, 0x03, 0x36, 0x3E, 0x26, STATUS_READ}));
  EXPECT_FALSE(test::has_reading(*dev, DeviceRegister::TOWEL_TEMPERATURE_IN_FORCE));
  EXPECT_EQ(test::wire_read(*dev, DeviceReading::HEATING_LEVEL), heating_level::BOOST);
  EXPECT_FALSE(heating_device_facts(dev, true).programme_runs);

  // Back in the programme: read again, and the earlier eco value is not what shows.
  EXPECT_EQ(run_towel_poll(comp, radio, {PROG, NONE, IN_FORCE_COMFORT, LEVEL_COMFORT_MP, {}, {}}),
            (std::vector<uint8_t>{0x15, 0x03, 0x36, 0x3E, 0x26, 0x37, STATUS_READ}));
  EXPECT_FLOAT_EQ(test::number_read(*dev, DeviceReading::TOWEL_TEMPERATURE_IN_FORCE), 20.0F);

  // MANUEL: no 0x0137 read and its value is dropped, the status read still runs.
  EXPECT_EQ(run_towel_poll(comp, radio, {TowelDryerMode::MANUAL, NONE, {}, LEVEL_COMFORT_MP, {}, {}}),
            (std::vector<uint8_t>{0x15, 0x03, 0x36, 0x3E, 0x26, STATUS_READ}));
  EXPECT_FALSE(test::has_reading(*dev, DeviceRegister::TOWEL_TEMPERATURE_IN_FORCE));
  for (const auto &tx : radio.get_sent_data()) {
    if (sent_step(tx) != STATUS_READ) {
      EXPECT_EQ(tx.at(10), PRIVATE_REGISTER_DIRECTION_GET) << "a poll only reads";
    }
  }
}

TEST(HubOperations, ATowelDryerInDryingIsNeverAskedForItsTemperatureAndAStaleValueIsNotKept) {
  TestableComponent comp;
  MockRadio radio;
  IoDevice *dev = make_settled_towel_dryer(comp, radio);
  test::store_reading(*dev, DeviceReading::TOWEL_TEMPERATURE_IN_FORCE, 16.5F);  // from an earlier programme run
  EXPECT_EQ(
      run_towel_poll(comp, radio, {TowelDryerMode::PROG, HeatingTemporaryState::DRYING, {}, LEVEL_BOOST_MP, {}, {}}),
      (std::vector<uint8_t>{0x15, 0x03, 0x36, 0x3E, 0x26, STATUS_READ}))
      << "no 0x0137 read while drying";
  EXPECT_FALSE(test::has_reading(*dev, DeviceRegister::TOWEL_TEMPERATURE_IN_FORCE));
}

TEST(HubOperations, ThePollShowsTheProgrammeRunningInEveryPeriodAndNotInHeat) {
  TestableComponent comp;
  MockRadio radio;
  IoDevice *dev = make_settled_towel_dryer(comp, radio);
  const std::vector<uint8_t> in_force_override = {0x0C, 0x60, 0x01, 0x37, 0xAF, 0x00};  // 17.5 C

  EXPECT_EQ(
      run_towel_poll(comp, radio,
                     {TowelDryerMode::PROG, HeatingTemporaryState::NONE, in_force_override, LEVEL_ECO_MP, {}, {}}),
      (std::vector<uint8_t>{0x15, 0x03, 0x36, 0x3E, 0x26, 0x37, STATUS_READ}))
      << "a temperature that fits no period needs no override or setback read: the level names the period";
  EXPECT_FLOAT_EQ(test::number_read(*dev, DeviceReading::TOWEL_TEMPERATURE_IN_FORCE), 17.5F);
  EXPECT_TRUE(heating_device_facts(dev, true).programme_runs) << "an eco period";

  EXPECT_EQ(
      run_towel_poll(comp, radio,
                     {TowelDryerMode::PROG, HeatingTemporaryState::NONE, IN_FORCE_COMFORT, LEVEL_COMFORT_MP, {}, {}}),
      (std::vector<uint8_t>{0x15, 0x03, 0x36, 0x3E, 0x26, 0x37, STATUS_READ}));
  EXPECT_TRUE(heating_device_facts(dev, true).programme_runs) << "a comfort period";

  run_towel_poll(comp, radio, {TowelDryerMode::MANUAL, HeatingTemporaryState::NONE, {}, LEVEL_COMFORT_MP, {}, {}});
  EXPECT_FALSE(heating_device_facts(dev, true).programme_runs) << "heat";
}

namespace {

/// A radiator whose boot registers and model check are done, so a poll sends only its recurring reads.
IoDevice *make_settled_radiator(TestableComponent &comp, MockRadio &radio) {
  IoDevice *dev = make_heater(comp, radio);
  dev->model_checked = true;
  test::store_reading(*dev, DeviceReading::MAX_SETPOINT, 28.0F);
  test::store_reading(*dev, DeviceReading::OPEN_WINDOW_DETECTION, 0);
  return dev;
}

/// Queue the five core registers of a radiator poll with the mode register @p mode; no reply to 0x012A.
void queue_radiator_core(TestableComponent &comp, MockRadio &radio, HeatingMode mode) {
  queue_register_reply(comp, radio, {0x0C, 0x60, 0x01, 0x15, 0xB7, 0x07});
  queue_register_reply(comp, radio, {0x0C, 0x60, 0x01, 0x03, 0xC8, 0x00});
  queue_register_reply(comp, radio, {0x0C, 0x60, 0x01, 0x00, static_cast<uint8_t>(mode)});
  queue_register_reply(comp, radio, {0x0C, 0x60, 0x01, 0x17, 0x02});
  queue_register_reply(comp, radio, {0x0C, 0x60, 0x01, 0x0E, 0x00});
}

/// Queue the open-window-detection reply (off) a radiator poll reads after its core registers and 0x012A.
void queue_radiator_detection(TestableComponent &comp, MockRadio &radio) {
  queue_register_reply(comp, radio, {0x0C, 0x60, 0x01, 0x06, 0x00});
}

std::vector<uint8_t> sent_steps_since(MockRadio &radio, size_t before) {
  std::vector<uint8_t> steps;
  for (size_t i = before; i < radio.get_sent_data().size(); i++)
    steps.push_back(sent_step(radio.get_sent_data()[i]));
  return steps;
}

}  // namespace

TEST(HubOperations, ARadiatorPollReadsRegister012AOnlyWhileTheProgrammeRunsAndAsksNoStatusRead) {
  TestableComponent comp;
  MockRadio radio;
  IoDevice *dev = make_settled_radiator(comp, radio);

  for (const HeatingMode mode : {HeatingMode::PROG, HeatingMode::AUTO}) {
    const size_t before = radio.get_sent_data().size();
    queue_radiator_core(comp, radio, mode);
    queue_register_reply(comp, radio, {0x0C, 0x60, 0x01, 0x2A, 0xAF, 0x00});  // 17.5 C
    queue_radiator_detection(comp, radio);
    EXPECT_TRUE(comp.request_device_status("ABC123"));
    EXPECT_EQ(sent_steps_since(radio, before), (std::vector<uint8_t>{0x15, 0x03, 0x00, 0x17, 0x0E, 0x2A, 0x06}))
        << "no status read: no radiator is known to report a level";
    EXPECT_FLOAT_EQ(test::number_read(*dev, DeviceReading::TEMPERATURE_IN_FORCE), 17.5F);
    EXPECT_TRUE(heating_device_facts(dev, true).programme_runs);
  }

  const size_t before = radio.get_sent_data().size();
  queue_radiator_core(comp, radio, HeatingMode::MANUAL);
  queue_radiator_detection(comp, radio);
  EXPECT_TRUE(comp.request_device_status("ABC123"));
  EXPECT_EQ(sent_steps_since(radio, before), (std::vector<uint8_t>{0x15, 0x03, 0x00, 0x17, 0x0E, 0x06}))
      << "heat: the temperature in force is not asked";
  EXPECT_FALSE(test::has_reading(*dev, DeviceRegister::TEMPERATURE_IN_FORCE)) << "and the earlier value is dropped";
}

TEST(HubOperations, ARadiatorThatAnswers012AEmptyIsNotAskedAgain) {
  TestableComponent comp;
  MockRadio radio;
  IoDevice *dev = make_settled_radiator(comp, radio);

  size_t before = radio.get_sent_data().size();
  queue_radiator_core(comp, radio, HeatingMode::PROG);
  queue_register_reply(comp, radio, {0x0C, 0x60, 0x01, 0x2A});  // answered, no value
  queue_radiator_detection(comp, radio);
  EXPECT_TRUE(comp.request_device_status("ABC123")) << "an empty answer is no poll failure";
  EXPECT_EQ(sent_steps_since(radio, before), (std::vector<uint8_t>{0x15, 0x03, 0x00, 0x17, 0x0E, 0x2A, 0x06}));
  EXPECT_EQ(test::in_force_of(*dev), InForceSupport::ABSENT);
  EXPECT_FALSE(heating_device_facts(dev, true).programme_runs) << "no temperature in force to override";
  EXPECT_FALSE(heating_device_facts(dev, true).in_force_unresolved) << "and nothing left to ask";

  before = radio.get_sent_data().size();
  queue_radiator_core(comp, radio, HeatingMode::PROG);
  queue_radiator_detection(comp, radio);
  EXPECT_TRUE(comp.request_device_status("ABC123"));
  EXPECT_EQ(sent_steps_since(radio, before), (std::vector<uint8_t>{0x15, 0x03, 0x00, 0x17, 0x0E, 0x06}))
      << "the read is not repeated until the next boot";
}

TEST(HubOperations, ARadiatorThatReported012AKeepsReadingItAfterAnEmptyAnswer) {
  TestableComponent comp;
  MockRadio radio;
  IoDevice *dev = make_settled_radiator(comp, radio);

  queue_radiator_core(comp, radio, HeatingMode::PROG);
  queue_register_reply(comp, radio, {0x0C, 0x60, 0x01, 0x2A, 0xAF, 0x00});  // 17.5 C
  queue_radiator_detection(comp, radio);
  EXPECT_TRUE(comp.request_device_status("ABC123"));
  ASSERT_EQ(test::in_force_of(*dev), InForceSupport::REPORTED);

  queue_radiator_core(comp, radio, HeatingMode::PROG);
  queue_register_reply(comp, radio, {0x0C, 0x60, 0x01, 0x2A});  // answered, no value
  queue_radiator_detection(comp, radio);
  EXPECT_TRUE(comp.request_device_status("ABC123"));
  EXPECT_EQ(test::in_force_of(*dev), InForceSupport::REPORTED) << "a heater that has shown the register still has it";
  EXPECT_TRUE(heating_device_facts(dev, true).programme_runs);

  const size_t before = radio.get_sent_data().size();
  queue_radiator_core(comp, radio, HeatingMode::PROG);
  queue_register_reply(comp, radio, {0x0C, 0x60, 0x01, 0x2A, 0xB4, 0x00});  // 18.0 C
  queue_radiator_detection(comp, radio);
  EXPECT_TRUE(comp.request_device_status("ABC123"));
  EXPECT_EQ(sent_steps_since(radio, before), (std::vector<uint8_t>{0x15, 0x03, 0x00, 0x17, 0x0E, 0x2A, 0x06}))
      << "the next poll asks again";
  EXPECT_FLOAT_EQ(test::number_read(*dev, DeviceReading::TEMPERATURE_IN_FORCE), 18.0F);
}

TEST(HubOperations, AnUnansweredRead012AIsRetriedAtTheNextPollNotMarkedMissing) {
  TestableComponent comp;
  MockRadio radio;
  IoDevice *dev = make_settled_radiator(comp, radio);
  queue_radiator_core(comp, radio, HeatingMode::PROG);  // no answer to 0x2A: the exchange times out

  comp.request_device_status("ABC123");

  EXPECT_EQ(test::in_force_of(*dev), InForceSupport::UNKNOWN) << "no answer at all says nothing about the register";
}

TEST(HubOperations, ARadiatorThatReported012AKeepsTheOverrideRuleWhenALaterReadIsMissed) {
  TestableComponent comp;
  MockRadio radio;
  IoDevice *dev = make_settled_radiator(comp, radio);

  queue_radiator_core(comp, radio, HeatingMode::PROG);
  queue_register_reply(comp, radio, {0x0C, 0x60, 0x01, 0x2A, 0xAF, 0x00});  // 17.5 C
  queue_radiator_detection(comp, radio);
  EXPECT_TRUE(comp.request_device_status("ABC123"));
  EXPECT_EQ(test::in_force_of(*dev), InForceSupport::REPORTED);

  // Heat drops the value; back in the programme (set on the radiator) the read of 0x012A gets no reply.
  queue_radiator_core(comp, radio, HeatingMode::MANUAL);
  queue_radiator_detection(comp, radio);
  EXPECT_TRUE(comp.request_device_status("ABC123"));
  queue_radiator_core(comp, radio, HeatingMode::AUTO);
  comp.request_device_status("ABC123");
  ASSERT_FALSE(test::has_reading(*dev, DeviceRegister::TEMPERATURE_IN_FORCE));
  EXPECT_EQ(test::in_force_of(*dev), InForceSupport::REPORTED) << "a missed read changes nothing";

  EXPECT_TRUE(heating_device_facts(dev, true).programme_runs)
      << "the programme runs: a missed read must not turn a target into a change of the comfort setpoint";
  EXPECT_FALSE(heating_device_facts(dev, true).in_force_unresolved);
}

TEST(HubOperations, TheReadBeforeATargetAsksTheModeThenTheTemperatureInForceOfAnUnresolvedRadiator) {
  TestableComponent comp;
  MockRadio radio;
  IoDevice *dev = make_settled_radiator(comp, radio);
  test::store_reading(*dev, DeviceReading::MODE,
                      static_cast<int16_t>(HeatingMode::MANUAL));  // stale: the heater runs its programme now
  queue_register_reply(comp, radio, {0x0C, 0x60, 0x01, 0x00, static_cast<uint8_t>(HeatingMode::PROG)});
  queue_register_reply(comp, radio, {0x0C, 0x60, 0x01, 0x2A, 0xAF, 0x00});  // 17.5 C
  const size_t before = radio.get_sent_data().size();

  EXPECT_TRUE(comp.read_programme_state("ABC123"));

  EXPECT_EQ(sent_steps_since(radio, before), (std::vector<uint8_t>{0x00, 0x2A}));
  EXPECT_EQ(test::in_force_of(*dev), InForceSupport::REPORTED);
  EXPECT_FLOAT_EQ(test::number_read(*dev, DeviceReading::TEMPERATURE_IN_FORCE), 17.5F)
      << "the card shows what the heater said";
  EXPECT_TRUE(heating_device_facts(dev, true).programme_runs);
}

TEST(HubOperations, TheReadBeforeATargetMarksAHeaterThatAnswersTheTemperatureInForceEmptyAsWithoutIt) {
  TestableComponent comp;
  MockRadio radio;
  IoDevice *dev = make_settled_radiator(comp, radio);
  queue_register_reply(comp, radio, {0x0C, 0x60, 0x01, 0x00, static_cast<uint8_t>(HeatingMode::PROG)});
  queue_register_reply(comp, radio, {0x0C, 0x60, 0x01, 0x2A});  // answered, no value

  EXPECT_TRUE(comp.read_programme_state("ABC123"));

  EXPECT_EQ(test::in_force_of(*dev), InForceSupport::ABSENT);
  EXPECT_FALSE(heating_device_facts(dev, true).programme_runs);
  EXPECT_FALSE(heating_device_facts(dev, true).in_force_unresolved);
}

TEST(HubOperations, TheReadBeforeATargetLeavesTheTemperatureInForceUnresolvedWhileTheHeaterStaysSilent) {
  TestableComponent comp;
  MockRadio radio;
  IoDevice *dev = make_settled_radiator(comp, radio);
  queue_register_reply(comp, radio, {0x0C, 0x60, 0x01, 0x00, static_cast<uint8_t>(HeatingMode::PROG)});

  EXPECT_TRUE(comp.read_programme_state("ABC123")) << "the mode was answered";  // 0x2A gets no reply

  EXPECT_EQ(test::in_force_of(*dev), InForceSupport::UNKNOWN);
  EXPECT_TRUE(heating_device_facts(dev, true).in_force_unresolved) << "the plan refuses a target";
  EXPECT_FALSE(heating_device_facts(dev, true).programme_runs);
}

TEST(HubOperations, TheReadBeforeATargetAsksOnlyTheModeInHeatOnceTheHeaterAnsweredOrHasNoRegister) {
  for (const InForceSupport support : {InForceSupport::UNKNOWN, InForceSupport::REPORTED, InForceSupport::ABSENT}) {
    TestableComponent comp;
    MockRadio radio;
    IoDevice *dev = make_settled_radiator(comp, radio);
    test::set_in_force_support(*dev, support);
    queue_register_reply(comp, radio, {0x0C, 0x60, 0x01, 0x00, static_cast<uint8_t>(HeatingMode::MANUAL)});
    const size_t before = radio.get_sent_data().size();

    EXPECT_TRUE(comp.read_programme_state("ABC123"));

    EXPECT_EQ(sent_steps_since(radio, before), std::vector<uint8_t>{0x00})
        << "heat: no programme runs, so no temperature in force is asked";
  }
  for (const InForceSupport support : {InForceSupport::REPORTED, InForceSupport::ABSENT}) {
    TestableComponent comp;
    MockRadio radio;
    IoDevice *dev = make_settled_radiator(comp, radio);
    test::set_in_force_support(*dev, support);
    queue_register_reply(comp, radio, {0x0C, 0x60, 0x01, 0x00, static_cast<uint8_t>(HeatingMode::PROG)});
    const size_t before = radio.get_sent_data().size();

    EXPECT_TRUE(comp.read_programme_state("ABC123"));

    EXPECT_EQ(sent_steps_since(radio, before), std::vector<uint8_t>{0x00})
        << "the programme runs and the heater has answered: the poll refreshes the value";
  }
}

TEST(HubOperations, AStatusReplyThatCarriesNoLevelClearsTheStoredLevel) {
  TestableComponent comp;
  MockRadio radio;
  IoDevice *dev = make_settled_towel_dryer(comp, radio);
  test::store_reading(*dev, DeviceReading::HEATING_LEVEL, heating_level::ECO);

  run_towel_poll(comp, radio, {TowelDryerMode::MANUAL, HeatingTemporaryState::NONE, {}, 0xC800, {}, {}});

  EXPECT_EQ(test::wire_read(*dev, DeviceReading::HEATING_LEVEL), -1)
      << "a main parameter that is no level is forgotten: no eco period is assumed";
}

TEST(HubOperations, AnUnansweredStatusReadIsNoPollFailureAndKeepsTheLastLevel) {
  TestableComponent comp;
  MockRadio radio;
  IoDevice *dev = make_settled_towel_dryer(comp, radio);
  test::store_reading(*dev, DeviceReading::HEATING_LEVEL, heating_level::COMFORT);

  const std::vector<uint8_t> steps =
      run_towel_poll(comp, radio, {TowelDryerMode::MANUAL, HeatingTemporaryState::NONE, {}, std::nullopt, {}, {}});

  ASSERT_GT(steps.size(), 5u);
  for (size_t i = 5; i < steps.size(); i++)
    EXPECT_EQ(steps[i], STATUS_READ) << "only status reads (the retries) follow the registers";
  EXPECT_EQ(test::wire_read(*dev, DeviceReading::HEATING_LEVEL), heating_level::COMFORT);
  EXPECT_EQ(comp.last_timeout_id_, decisions::heating_poll_timer_id(dev->node_id)) << "the cadence continues";
}

TEST(HubOperations, ATowelDryerDeclaredOnARadiatorStillGetsTheModelCheck) {
  TestableComponent comp;
  MockRadio radio;
  IoDevice *dev = make_towel_dryer(comp, radio);
  queue_register_reply(comp, radio, {0x0C, 0x60, 0x01, 0x15, 0x1B, 0x08});
  queue_register_reply(comp, radio, {0x0C, 0x60, 0x01, 0x03, 0xC3, 0x00});
  queue_register_reply(comp, radio, {0x0C, 0x60, 0x01, 0x36});  // a radiator holds no towel-dryer mode
  queue_register_reply(comp, radio, {0x0C, 0x60, 0x01, 0x19, 'R', 'A', 'D'});
  queue_register_reply(comp, radio, {0x0C, 0x60, 0x01, 0x36});

  EXPECT_FALSE(comp.request_device_status("ABC123")) << "the poll fails at the empty mode register";

  const uint8_t expected_registers[] = {0x15, 0x03, 0x36, 0x19, 0x36};
  ASSERT_EQ(radio.get_sent_data().size(), std::size(expected_registers));
  for (size_t i = 0; i < std::size(expected_registers); i++)
    EXPECT_EQ(sent_register(radio.get_sent_data()[i]), expected_registers[i]) << "read " << i;
  EXPECT_TRUE(dev->model_checked) << "both reads answered: the radiator hint was logged";
}

// ============================================================================
// Heater with a night light (52/1, model HEATER_WITH_LIGHT)
// ============================================================================

namespace {

IoDevice *make_heater_with_light(TestableComponent &comp, MockRadio &radio) {
  IoDevice *dev = make_heater(comp, radio);
  test::set_kind(*dev, DeviceType::ELECTRICAL_HEATER, 1, DeviceModel::HEATER_WITH_LIGHT);
  return dev;
}

/// Answers the radiator's five poll registers and its open-window-detection setting (read on every poll).
void queue_radiator_poll(TestableComponent &comp, MockRadio &radio) {
  queue_register_reply(comp, radio, {0x0C, 0x60, 0x01, 0x15, 0x1B, 0x08});  // 20.75 C
  queue_register_reply(comp, radio, {0x0C, 0x60, 0x01, 0x03, 0xC3, 0x00});  // 19.5 C
  queue_register_reply(comp, radio, {0x0C, 0x60, 0x01, 0x00, 0x01});        // manual
  queue_register_reply(comp, radio, {0x0C, 0x60, 0x01, 0x17, 0x02});        // heating
  queue_register_reply(comp, radio, {0x0C, 0x60, 0x01, 0x0E, 0x00});        // window closed
  queue_register_reply(comp, radio, {0x0C, 0x60, 0x01, 0x06, 0x01});        // detection on
}

}  // namespace

TEST(HubOperations, HeaterWithLightPollReadsTheLightAfterTheRadiatorRegisters) {
  TestableComponent comp;
  MockRadio radio;
  IoDevice *dev = make_heater_with_light(comp, radio);
  EXPECT_TRUE(has_feature(dev->features, device_feature::WINDOW_COMPANIONS));
  EXPECT_TRUE(has_feature(dev->features, device_feature::NIGHT_LIGHT));
  queue_radiator_poll(comp, radio);
  // Values of the issue #222 sweep with the light on.
  queue_register_reply(comp, radio, {0x0C, 0x60, 0x01, 0x44, 0x01});
  queue_register_reply(comp, radio, {0x0C, 0x60, 0x01, 0x42, 0x64, 0x00});
  queue_register_reply(comp, radio, {0x0C, 0x60, 0x01, 0x02, 0x18, 0x01});
  queue_register_reply(comp, radio, {0x0C, 0x60, 0x01, 0x19, 'H', 'E', 'K', 'L', 'A'});
  queue_register_reply(comp, radio, {0x0C, 0x60, 0x01, 0x36});

  EXPECT_TRUE(comp.request_device_status("ABC123"));

  const uint8_t expected_registers[] = {0x15, 0x03, 0x00, 0x17, 0x0E, 0x06, 0x44, 0x42, 0x02, 0x19, 0x36};
  ASSERT_EQ(radio.get_sent_data().size(), std::size(expected_registers));
  for (size_t i = 0; i < std::size(expected_registers); i++)
    EXPECT_EQ(sent_register(radio.get_sent_data()[i]), expected_registers[i]) << "read " << i;
  for (const auto &tx : radio.get_sent_data())
    EXPECT_EQ(tx.at(10), PRIVATE_REGISTER_DIRECTION_GET) << "a poll only reads";
  EXPECT_EQ(test::wire_read(*dev, DeviceReading::NIGHT_LIGHT), 1);
  ASSERT_TRUE(reading_value(*dev, DeviceReading::NIGHT_LIGHT_BRIGHTNESS).has_value());
  EXPECT_NEAR(*reading_value(*dev, DeviceReading::NIGHT_LIGHT_BRIGHTNESS), 3.3F, 1e-4F)
      << "register value 100 is held as a percentage";

  // Unlike the boot registers, the light is read again on the next poll.
  const size_t sent_before = radio.get_sent_data().size();
  queue_radiator_poll(comp, radio);
  queue_register_reply(comp, radio, {0x0C, 0x60, 0x01, 0x44, 0x00});
  queue_register_reply(comp, radio, {0x0C, 0x60, 0x01, 0x42, 0x64, 0x00});
  EXPECT_TRUE(comp.request_device_status("ABC123"));
  EXPECT_EQ(radio.get_sent_data().size() - sent_before, 8u);
  EXPECT_EQ(test::wire_read(*dev, DeviceReading::NIGHT_LIGHT), 0);
}

TEST(HubOperations, AHeaterWithoutTheLightDoesNotFailItsPoll) {
  TestableComponent comp;
  MockRadio radio;
  IoDevice *dev = make_heater_with_light(comp, radio);
  // Boot registers and model check done, so the poll is the five registers, the detection setting and the light.
  dev->model_checked = true;
  test::store_reading(*dev, DeviceReading::MAX_SETPOINT, 28.0F);
  test::store_reading(*dev, DeviceReading::OPEN_WINDOW_DETECTION, 1);
  queue_radiator_poll(comp, radio);
  queue_register_reply(comp, radio, {0x0C, 0x60, 0x01, 0x44});  // no light: answered without a value
  queue_register_reply(comp, radio, {0x0C, 0x60, 0x01, 0x42});

  EXPECT_TRUE(comp.request_device_status("ABC123")) << "a missing light costs a read, not the poll";

  EXPECT_EQ(radio.get_sent_data().size(), 8u);
  EXPECT_EQ(test::wire_read(*dev, DeviceReading::NIGHT_LIGHT), -1);
  EXPECT_FALSE(comp.poll_policy_.is_tracking_active("ABC123", esphome::millis()));
  EXPECT_EQ(comp.poll_policy_.get_status_poll_failures("ABC123"), 0u);
}

TEST(HubOperations, AFailedHeaterPollDoesNotGoOnToTheLight) {
  TestableComponent comp;
  MockRadio radio;
  make_heater_with_light(comp, radio);
  // Nothing queued: the room temperature never answers.

  EXPECT_FALSE(comp.request_device_status("ABC123"));

  ASSERT_FALSE(radio.get_sent_data().empty());
  for (const auto &tx : radio.get_sent_data())
    EXPECT_EQ(sent_register(tx), 0x15) << "a heater that does not answer is not asked for its light";
  EXPECT_EQ(comp.poll_policy_.get_status_poll_failures("ABC123"), 1u);
}

TEST(HubOperations, NightLightWritesSendExactBytes) {
  TestableComponent comp;
  MockRadio radio;
  make_heater_with_light(comp, radio);
  queue_register_reply(comp, radio, {0x0C, 0x61, 0x01, 0x42, 0x88, 0x13});
  ASSERT_TRUE(
      comp.send_heating_command("ABC123", DeviceRegister::NIGHT_LIGHT_BRIGHTNESS, 78.75F));  // register value 5000
  queue_register_reply(comp, radio, {0x0C, 0x61, 0x01, 0x44, 0x01});
  ASSERT_TRUE(comp.send_heating_command("ABC123", DeviceRegister::NIGHT_LIGHT, 1.0F));

  ASSERT_EQ(radio.get_sent_data().size(), 2u);
  const auto &brightness = radio.get_sent_data()[0];
  EXPECT_EQ(std::vector<uint8_t>(brightness.begin() + 9, brightness.begin() + 15),
            (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x42, 0x88, 0x13}));
  const auto &on = radio.get_sent_data()[1];
  EXPECT_EQ(std::vector<uint8_t>(on.begin() + 9, on.begin() + 14),
            (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x44, 0x01}));
}

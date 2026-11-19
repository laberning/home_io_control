/// @file heater_controller_test.cpp
/// @brief HeaterController without a hub: the register heater's writes and reads and the EXECUTE funnel of the
/// level and temperature heaters, and the poll timer (HeaterControllerFixture).
///
/// Planned climate writes are in heater_climate_write_test.cpp, the water heater in heater_controller_dhw_test.cpp,
/// regular polls in heater_poll_test.cpp and the decoding of a status answer in heater_status_decoding_test.cpp.
/// What crosses the hub's own bookkeeping (device event reports, exchange counters, the status decoder, the
/// settle poll) is tested through the hub's forwarders in hub_heating_operations_test.cpp, hub_operations_test.cpp
/// and hub_dhw_test.cpp.

#include "heater_controller.h"
#include "payload_frame.h"
#include "reading_store.h"
#include "water_heater_values.h"
#include "decisions_status.h"
#include "proto_codecs.h"
#include "proto_commands.h"
#include "proto_device_readings.h"
#include "proto_dhw.h"
#include "proto_frame.h"
#include "proto_heating.h"
#include "proto_heating_profiles.h"
#include "proto_private_registers.h"

#include "heater_controller_fixture.h"
#include "heater_controller_helpers.h"
#include "corpus_test_helpers.h"

#include <array>
#include <cmath>
#include <functional>
#include <vector>

using namespace esphome::home_io_control;
using namespace heater_controller_test;

namespace {

/// Run send_heating_command against an electrical heater with an adjustable setpoint with a single reply frame
/// (`cmd` and `payload`) queued as the device's answer.
bool run_heating_command_with_reply(uint8_t cmd, const std::vector<uint8_t> &payload) {
  HeaterControllerFixture fx;
  make_heater(fx);
  fx.queue_reply(cmd, payload);
  return fx.controller.send_heating_command("ABC123", DeviceRegister::PAIRED_DEVICES, 0.0F);
}

}  // namespace

// === Register heater: writes ===

TEST(HeaterController, SendHeatingCommandAcceptsClimateDeviceAndSendsExactBytes) {
  HeaterControllerFixture fx;
  make_heater(fx);
  fx.queue_register_ack({0x0C, 0x61, 0x01, 0x03, 0xCD, 0x00});

  EXPECT_TRUE(fx.controller.send_heating_command("ABC123", DeviceRegister::COMFORT_SETPOINT, 20.5f));

  ASSERT_EQ(fx.radio.get_sent_data().size(), 1u);
  const auto &tx = fx.radio.get_sent_data().front();
  ASSERT_GE(tx.size(), 15u);
  EXPECT_EQ(tx[8], CMD_WRITE_PRIVATE) << "command byte should be CMD_WRITE_PRIVATE (0x20)";
  // Payload begins at wire offset 9 — {0x0C, 0x61, 0x01, 0x03, 0xCD, 0x00} for 20.5 C.
  EXPECT_EQ(tx[9], 0x0C);
  EXPECT_EQ(tx[10], 0x61);
  EXPECT_EQ(tx[11], 0x01);
  EXPECT_EQ(tx[12], 0x03);
  EXPECT_EQ(tx[13], 0xCD) << "20.5 C -> 205 -> 0xCD";
  EXPECT_EQ(tx[14], 0x00) << "trailing literal constant";
}

TEST(HeaterController, AnAcknowledgedHeatingWritePollsTheHeaterAgainSoon) {
  // Heating / idle only shows in the regulator register, which the poll reads (issue #165).
  HeaterControllerFixture fx;
  const IoDevice *dev = make_heater(fx);
  fx.queue_register_ack({0x0C, 0x61, 0x01, 0x03, 0xCD, 0x00});

  ASSERT_TRUE(fx.controller.send_heating_command("ABC123", DeviceRegister::COMFORT_SETPOINT, 20.5f));

  ASSERT_FALSE(fx.timers.empty());
  EXPECT_EQ(fx.timers.back().id, decisions::heating_poll_timer_id(dev->node_id));
  EXPECT_EQ(fx.timers.back().delay_ms, HEATING_POST_WRITE_POLL_DELAY_MS);
}

TEST(HeaterController, AnUnacknowledgedHeatingWriteSchedulesNoExtraPoll) {
  HeaterControllerFixture fx;
  const IoDevice *dev = make_heater(fx);

  EXPECT_FALSE(fx.controller.send_heating_command("ABC123", DeviceRegister::COMFORT_SETPOINT, 20.5f));
  for (const auto &timer : fx.timers)
    EXPECT_NE(timer.id, decisions::heating_poll_timer_id(dev->node_id));
}

TEST(HeaterController, SendHeatingCommandRejectsNonClimateDevice) {
  HeaterControllerFixture fx;
  make_cover(fx);

  EXPECT_FALSE(fx.controller.send_heating_command("ABC123", DeviceRegister::COMFORT_SETPOINT, 20.5f))
      << "cover devices must be rejected by the climate capability predicate";
  EXPECT_TRUE(fx.radio.get_sent_data().empty()) << "a rejected heating command must not transmit";
}

TEST(HeaterController, SendHeatingCommandRejectsOutOfRangeTemperature) {
  HeaterControllerFixture fx;
  make_heater(fx);

  EXPECT_FALSE(fx.controller.send_heating_command("ABC123", DeviceRegister::COMFORT_SETPOINT, 28.1f));
  EXPECT_TRUE(fx.radio.get_sent_data().empty()) << "an unencodable value must not transmit";
}

TEST(HeaterController, SendHeatingCommandFailsWithoutAnyReply) {
  HeaterControllerFixture fx;
  make_heater(fx);

  // No reply queued -> exchange times out -> not acknowledged.
  EXPECT_FALSE(fx.controller.send_heating_command("ABC123", DeviceRegister::PAIRED_DEVICES, 0.0f));
}

TEST(HeaterController, SendHeatingCommandFailsBeforeTheHubIsInitialisedAndForAnUnknownDevice) {
  HeaterControllerFixture fx;
  make_heater(fx);

  EXPECT_FALSE(fx.controller.send_heating_command("DEF456", DeviceRegister::COMFORT_SETPOINT, 20.5f));
  fx.initialized = false;
  EXPECT_FALSE(fx.controller.send_heating_command("ABC123", DeviceRegister::COMFORT_SETPOINT, 20.5f));
  EXPECT_TRUE(fx.radio.get_sent_data().empty());
}

// power_on / midnight_sync are register reads: the 0x21 ACK carries a payload the controller logs at
// DEBUG (bytes_to_hex) but does not decode. Exercise both an ACK with payload bytes and the
// empty-payload ACK to prove the log path handles either without crashing and still returns true.
TEST(HeaterController, SendHeatingCommandAcceptsReadAckWithPayload) {
  // Stand-in for a 0x0130 setpoint-block read answer (contents not decoded, only logged).
  EXPECT_TRUE(run_heating_command_with_reply(CMD_WRITE_PRIVATE_ACK,
                                             {0x0C, 0x60, 0x01, 0x30, 0xC8, 0x00, 0x23, 0x00, 0x28, 0x00}))
      << "a CMD_WRITE_PRIVATE_ACK carrying a read payload is still an acknowledgement";
}

TEST(HeaterController, SendHeatingCommandAcceptsReadAckWithEmptyPayload) {
  EXPECT_TRUE(run_heating_command_with_reply(CMD_WRITE_PRIVATE_ACK, {}))
      << "an empty-payload ACK must not trip the payload log path";
}

TEST(HeaterController, SendHeatingCommandFailsOnNonAckReply) {
  // The acknowledged check is `response.cmd == CMD_WRITE_PRIVATE_ACK`; a reply with any other cmd
  // must fail the command.
  EXPECT_FALSE(run_heating_command_with_reply(CMD_ERROR_RESP, {RESULT_LIMITATION_BY_WIND}))
      << "a CMD_ERROR_RESP reply must not count as an acknowledgement";
  EXPECT_FALSE(run_heating_command_with_reply(CMD_PRIVATE_RESP,
                                              {test::STATUS_BYTE_AT_REST, 0x00, 0xC8, 0x00, 0xC8, 0x00, 0x00, 0x00}))
      << "a CMD_PRIVATE_RESP reply must not count as an acknowledgement";
}

TEST(HeaterController, AUserWriteToAHeaterKeepsTheFullRetryBudget) {
  HeaterControllerFixture fx;
  make_heater(fx);
  // No reply: every try is spent.
  EXPECT_FALSE(fx.controller.send_heating_command("ABC123", DeviceRegister::COMFORT_SETPOINT, 20.5F));
  EXPECT_GT(fx.radio.get_sent_data().size(), static_cast<size_t>(SCHEDULED_POLL_MAX_TRIES));
  EXPECT_LE(fx.radio.get_sent_data().size(), static_cast<size_t>(EXCHANGE_RETRY_COUNT));
}

TEST(HeaterController, TowelDryerModeAndTemporaryStateWritesSendExactBytes) {
  HeaterControllerFixture fx;
  make_towel_dryer(fx);
  fx.queue_register_ack({0x0C, 0x61, 0x01, 0x36, 0x03});
  ASSERT_TRUE(fx.controller.send_heating_command("ABC123", DeviceRegister::TOWEL_MODE, 3.0F));
  fx.queue_register_ack({0x0C, 0x61, 0x01, 0x3E, 0x01});
  ASSERT_TRUE(fx.controller.send_heating_command("ABC123", DeviceRegister::TEMPORARY_STATE, 1.0F));

  ASSERT_EQ(fx.radio.get_sent_data().size(), 2u);
  // The payload starts at wire offset 9; a one-byte write is five bytes.
  EXPECT_EQ(sent_payload(fx.radio.get_sent_data()[0], 5), (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x36, 0x03}));
  EXPECT_EQ(sent_payload(fx.radio.get_sent_data()[1], 5), (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x3E, 0x01}));
}

// === Register heater: reads ===

TEST(HeaterController, ReadHeatingRegisterDecodesAndStoresTheValue) {
  HeaterControllerFixture fx;
  const IoDevice *dev = make_heater(fx);
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x15, 0x1B, 0x08});

  float value = 0.0F;
  ASSERT_TRUE(
      fx.controller.read_heating_register("ABC123", DeviceRegister::ROOM_TEMPERATURE, &value, EXCHANGE_RETRY_COUNT));

  EXPECT_FLOAT_EQ(value, 20.75F);
  EXPECT_FLOAT_EQ(test::number_read(*dev, DeviceReading::ROOM_TEMPERATURE), 20.75F);
  ASSERT_EQ(fx.radio.get_sent_data().size(), 1u);
  const auto &tx = fx.radio.get_sent_data().front();
  EXPECT_EQ(tx[8], CMD_WRITE_PRIVATE);
  EXPECT_EQ(tx[9], 0x0C);
  EXPECT_EQ(tx[10], 0x60) << "a read is a get";
  EXPECT_EQ(tx[11], 0x01);
  EXPECT_EQ(tx[12], 0x15);
}

TEST(HeaterController, ASensorFaultOfTheRoomTemperatureIsKeptAsAnAnsweredReading) {
  HeaterControllerFixture fx;
  const IoDevice *dev = make_heater(fx);
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x15, 0xFF, 0xFF});  // raw -1: the heater reports a faulty sensor

  EXPECT_TRUE(
      fx.controller.read_heating_register("ABC123", DeviceRegister::ROOM_TEMPERATURE, nullptr, EXCHANGE_RETRY_COUNT))
      << "the heater did answer";

  ASSERT_TRUE(reading_value(*dev, DeviceReading::ROOM_TEMPERATURE).has_value())
      << "held, so the card shows no temperature";
  EXPECT_TRUE(std::isnan(*reading_value(*dev, DeviceReading::ROOM_TEMPERATURE)));
}

TEST(HeaterController, ReadHeatingRegisterNotifiesTheEntities) {
  HeaterControllerFixture fx;
  make_heater(fx);
  int notifications = 0;
  fx.registry.subscribe([&notifications](const std::string &, const IoDevice &) { notifications++; });
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x15, 0x1B, 0x08});

  ASSERT_TRUE(
      fx.controller.read_heating_register("ABC123", DeviceRegister::ROOM_TEMPERATURE, nullptr, EXCHANGE_RETRY_COUNT));

  EXPECT_EQ(notifications, 1) << "what an entity shows is what the heater reported, notified once after the store";
}

TEST(HeaterController, AReadReplyWithoutAValueNotifiesOnce) {
  HeaterControllerFixture fx;
  make_heater(fx);
  int notifications = 0;
  fx.registry.subscribe([&notifications](const std::string &, const IoDevice &) { notifications++; });
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x15});

  EXPECT_FALSE(
      fx.controller.read_heating_register("ABC123", DeviceRegister::ROOM_TEMPERATURE, nullptr, EXCHANGE_RETRY_COUNT));
  EXPECT_EQ(notifications, 1) << "the heater was heard, so its link health is shown, once";
}

TEST(HeaterController, AReadReplyForAnotherRegisterNotifiesOnce) {
  HeaterControllerFixture fx;
  make_heater(fx);
  int notifications = 0;
  fx.registry.subscribe([&notifications](const std::string &, const IoDevice &) { notifications++; });
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x16, 0x1B, 0x08});

  EXPECT_FALSE(
      fx.controller.read_heating_register("ABC123", DeviceRegister::ROOM_TEMPERATURE, nullptr, EXCHANGE_RETRY_COUNT));
  EXPECT_EQ(notifications, 1) << "nothing is stored from it, but the heater was heard";
}

TEST(HeaterController, AReadAnsweredWithAnotherCommandNotifiesOnce) {
  HeaterControllerFixture fx;
  make_heater(fx);
  int notifications = 0;
  fx.registry.subscribe([&notifications](const std::string &, const IoDevice &) { notifications++; });
  fx.queue_reply(CMD_PRIVATE_RESP, {test::STATUS_BYTE_AT_REST, 0x00, 0xC8, 0x00, 0xC8, 0x00, 0x00, 0x00});

  EXPECT_FALSE(
      fx.controller.read_heating_register("ABC123", DeviceRegister::ROOM_TEMPERATURE, nullptr, EXCHANGE_RETRY_COUNT));
  EXPECT_EQ(notifications, 1) << "the heater was heard, so its link health is shown, once";
}

TEST(HeaterController, ReadHeatingRegisterWithoutAValueInTheReplyFails) {
  HeaterControllerFixture fx;
  const IoDevice *dev = make_heater(fx);
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x15});

  EXPECT_FALSE(
      fx.controller.read_heating_register("ABC123", DeviceRegister::ROOM_TEMPERATURE, nullptr, EXCHANGE_RETRY_COUNT));
  EXPECT_TRUE(std::isnan(test::number_read(*dev, DeviceReading::ROOM_TEMPERATURE)))
      << "nothing is stored from a reply without a value";
}

TEST(HeaterController, ReadHeatingRegisterRejectsWhatItCannotRead) {
  HeaterControllerFixture fx;
  make_heater(fx);
  EXPECT_FALSE(fx.controller.read_heating_register("ABC123", DeviceRegister::COUNT, nullptr, EXCHANGE_RETRY_COUNT))
      << "a function past the table has no register";
  EXPECT_TRUE(fx.radio.get_sent_data().empty());

  HeaterControllerFixture cover;
  make_cover(cover);
  EXPECT_FALSE(cover.controller.read_heating_register("ABC123", DeviceRegister::ROOM_TEMPERATURE, nullptr,
                                                      EXCHANGE_RETRY_COUNT));
  EXPECT_TRUE(cover.radio.get_sent_data().empty());
}

TEST(HeaterController, ReadPrivateRegisterChecksTheEchoAndRefusesADeviceWithoutRegisters) {
  HeaterControllerFixture fx;
  make_heater(fx);
  fx.queue_register_ack(corpus_test::adelis_name_reply());
  IoFrame response{};
  ASSERT_TRUE(fx.controller.read_private_register("ABC123", 0x01, 0x19, "model_name", response, 1));
  EXPECT_EQ(response.data_len, corpus_test::adelis_name_reply().size());
  EXPECT_EQ(response.data[PRIVATE_REGISTER_VALUE_OFFSET], 'A');
  EXPECT_EQ(sent_payload(fx.radio.get_sent_data().back(), 4), (std::vector<uint8_t>{0x0C, 0x60, 0x01, 0x19}));

  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x18, 0x41});
  EXPECT_FALSE(fx.controller.read_private_register("ABC123", 0x01, 0x19, "model_name", response, 1))
      << "an answer for another register is not this register's";

  HeaterControllerFixture cover;
  make_cover(cover);
  EXPECT_FALSE(cover.controller.read_private_register("ABC123", 0x01, 0x19, "model_name", response, 1));
  EXPECT_TRUE(cover.radio.get_sent_data().empty());
}

// === Register heater: the EXECUTE heaters and the poll timer ===

TEST(HeaterController, ALevelAndATemperatureHeaterAreCommandedThroughTheCoverShapedExecuteFunnel) {
  HeaterControllerFixture level;
  ASSERT_NE(level.add_device_of_kind(DeviceType::ELECTRICAL_HEATER, 0), nullptr);
  ASSERT_TRUE(device_uses_heating_levels(DeviceType::ELECTRICAL_HEATER, 0));

  EXPECT_TRUE(level.controller.set_heating_level("ABC123", heating_level::ECO));

  ASSERT_EQ(level.executes.size(), 1u);
  EXPECT_EQ(level.executes[0].action, "heating level");
  EXPECT_EQ(level.executes[0].kind, HeaterExecuteKind::WITH_ROLLBACK);
  ASSERT_TRUE(level.executes[0].built);
  IoFrame expected;
  const IoDevice *dev = level.registry.get("ABC123");
  ASSERT_TRUE(create_execute_heating_level(expected, level.node_id, dev->node_id, dev->low_power, heating_level::ECO));
  uint8_t expected_raw[64];
  uint8_t got_raw[64];
  const uint8_t expected_len = serialize(expected, expected_raw, sizeof(expected_raw));
  const uint8_t got_len = serialize(level.executes[0].request, got_raw, sizeof(got_raw));
  EXPECT_EQ(std::vector<uint8_t>(got_raw, got_raw + got_len),
            std::vector<uint8_t>(expected_raw, expected_raw + expected_len));

  HeaterControllerFixture temperature;
  temperature.add_device_of_kind(DeviceType::HEAT_PUMP);
  ASSERT_TRUE(device_uses_heating_temperature(DeviceType::HEAT_PUMP, 0));
  EXPECT_TRUE(temperature.controller.set_heating_temperature("ABC123", 0x1234, HeatingFp{}));
  ASSERT_EQ(temperature.executes.size(), 1u);
  EXPECT_EQ(temperature.executes[0].action, "heating temperature");
  EXPECT_EQ(temperature.executes[0].kind, HeaterExecuteKind::WITH_ROLLBACK);
  EXPECT_TRUE(temperature.executes[0].built);
}

TEST(HeaterController, AHeatingLevelIsNeverBuiltForADeviceTheGuardRefuses) {
  HeaterControllerFixture fx;
  make_heater(fx);  // a register heater, not a level heater

  EXPECT_FALSE(fx.controller.set_heating_level("ABC123", heating_level::ECO));
  ASSERT_EQ(fx.executes.size(), 1u);
  EXPECT_FALSE(fx.executes[0].built);
  EXPECT_TRUE(fx.radio.get_sent_data().empty());
}

TEST(HeaterController, ThePollTimerHasTheHeaterIdAndQueuesAStatusRequestWhenItFires) {
  HeaterControllerFixture fx;
  const IoDevice *dev = make_heater(fx);

  fx.controller.schedule_poll("ABC123", 1234);

  ASSERT_EQ(fx.timers.size(), 1u);
  EXPECT_EQ(fx.timers[0].id, decisions::heating_poll_timer_id(dev->node_id));
  EXPECT_NE(fx.timers[0].id, decisions::remote_poll_timer_id(dev->node_id));
  EXPECT_EQ(fx.timers[0].delay_ms, 1234u);
  EXPECT_TRUE(fx.queued_status_requests.empty()) << "nothing is queued before the timer fires";
  fx.timers[0].callback();
  EXPECT_EQ(fx.queued_status_requests, std::vector<std::string>{"ABC123"});
}

TEST(HeaterController, AMalformedDeviceIdArmsNoPollTimer) {
  HeaterControllerFixture fx;
  fx.controller.schedule_poll("not-hex", 1000);
  EXPECT_TRUE(fx.timers.empty());
}

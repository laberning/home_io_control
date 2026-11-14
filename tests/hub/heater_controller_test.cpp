/// @file heater_controller_test.cpp
/// @brief HeaterController without a hub: the register heater's writes and reads, the water heater's writes and
/// reads, and the contract of the calls it makes on the hub (HeaterControllerFixture).
///
/// What crosses the hub's own bookkeeping (device event reports, exchange counters, the status decoder, the
/// settle poll) is tested through the hub's forwarders in hub_operations_test.cpp and hub_dhw_test.cpp.

#include "heater_controller.h"
#include "hub_decisions.h"
#include "proto_commands.h"
#include "proto_dhw.h"
#include "proto_frame.h"
#include "proto_heating.h"

#include "heater_controller_fixture.h"

#include <cmath>
#include <functional>
#include <vector>

using namespace esphome::home_io_control;
using heater_controller_test::HeaterControllerFixture;

namespace {

IoDevice *make_heater(HeaterControllerFixture &fx) {
  IoDevice *dev = fx.add_device_of_kind(DeviceType::ELECTRICAL_HEATER, 1);
  EXPECT_NE(dev, nullptr);
  return dev;
}

IoDevice *make_towel_dryer(HeaterControllerFixture &fx) {
  IoDevice *dev = make_heater(fx);
  dev->model = DeviceModel::TOWEL_DRYER;
  resolve_device_features(*dev);
  return dev;
}

IoDevice *make_water_heater(HeaterControllerFixture &fx) {
  DeviceConfig config;
  config.type = DeviceType::DOMESTIC_HOT_WATER;
  config.entity_class = DeviceCapabilityClass::WATER_HEATER;
  IoDevice *dev = fx.add_device(config);
  EXPECT_NE(dev, nullptr);
  return dev;
}

IoDevice *make_cover(HeaterControllerFixture &fx) { return fx.add_device_of_kind(DeviceType::ROLLER_SHUTTER); }

/// The model name an Atlantic Adelis reports in register 0x0119 (corpus atlantic_adelis_probe_register_01_sweep).
const std::vector<uint8_t> ADELIS_NAME_REPLY = {0x0C, 0x60, 0x01, 0x19, 'A', 'D', 'E', 'L', 'I',
                                                'S',  ' ',  '2',  ' ',  'D', 'I', 'G', 'I', 'T'};

/// Run send_heating_command against an electrical heater with an adjustable setpoint with a single reply frame
/// (`cmd` and `payload`) queued as the device's answer.
bool run_heating_command_with_reply(uint8_t cmd, const std::vector<uint8_t> &payload) {
  HeaterControllerFixture fx;
  make_heater(fx);
  fx.queue_reply(cmd, payload);
  return fx.controller.send_heating_command("ABC123", HeatingFunction::POWER_ON, 0.0F);
}

/// The payload bytes (from wire offset 9) of a sent frame.
std::vector<uint8_t> sent_payload(const std::vector<uint8_t> &tx, size_t len) {
  return std::vector<uint8_t>(tx.begin() + 9, tx.begin() + 9 + len);
}

}  // namespace

// === Register heater: writes ===

TEST(HeaterController, SendHeatingCommandAcceptsClimateDeviceAndSendsExactBytes) {
  HeaterControllerFixture fx;
  make_heater(fx);
  fx.queue_register_ack({0x0C, 0x61, 0x01, 0x03, 0xCD, 0x00});

  EXPECT_TRUE(fx.controller.send_heating_command("ABC123", HeatingFunction::SET_TEMPERATURE, 20.5f));

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

  ASSERT_TRUE(fx.controller.send_heating_command("ABC123", HeatingFunction::SET_TEMPERATURE, 20.5f));

  ASSERT_FALSE(fx.timers.empty());
  EXPECT_EQ(fx.timers.back().id, decisions::heating_poll_timer_id(dev->node_id));
  EXPECT_EQ(fx.timers.back().delay_ms, HEATING_POST_WRITE_POLL_DELAY_MS);
}

TEST(HeaterController, AnUnacknowledgedHeatingWriteSchedulesNoExtraPoll) {
  HeaterControllerFixture fx;
  const IoDevice *dev = make_heater(fx);

  EXPECT_FALSE(fx.controller.send_heating_command("ABC123", HeatingFunction::SET_TEMPERATURE, 20.5f));
  for (const auto &timer : fx.timers)
    EXPECT_NE(timer.id, decisions::heating_poll_timer_id(dev->node_id));
}

TEST(HeaterController, SendHeatingCommandRejectsNonClimateDevice) {
  HeaterControllerFixture fx;
  make_cover(fx);

  EXPECT_FALSE(fx.controller.send_heating_command("ABC123", HeatingFunction::SET_TEMPERATURE, 20.5f))
      << "cover devices must be rejected by the climate capability predicate";
  EXPECT_TRUE(fx.radio.get_sent_data().empty()) << "a rejected heating command must not transmit";
}

TEST(HeaterController, SendHeatingCommandRejectsOutOfRangeTemperature) {
  HeaterControllerFixture fx;
  make_heater(fx);

  EXPECT_FALSE(fx.controller.send_heating_command("ABC123", HeatingFunction::SET_TEMPERATURE, 28.1f));
  EXPECT_TRUE(fx.radio.get_sent_data().empty()) << "an unencodable value must not transmit";
}

TEST(HeaterController, SendHeatingCommandFailsWithoutAnyReply) {
  HeaterControllerFixture fx;
  make_heater(fx);

  // No reply queued -> exchange times out -> not acknowledged.
  EXPECT_FALSE(fx.controller.send_heating_command("ABC123", HeatingFunction::POWER_ON, 0.0f));
}

TEST(HeaterController, SendHeatingCommandFailsBeforeTheHubIsInitialisedAndForAnUnknownDevice) {
  HeaterControllerFixture fx;
  make_heater(fx);

  EXPECT_FALSE(fx.controller.send_heating_command("DEF456", HeatingFunction::SET_TEMPERATURE, 20.5f));
  fx.initialized = false;
  EXPECT_FALSE(fx.controller.send_heating_command("ABC123", HeatingFunction::SET_TEMPERATURE, 20.5f));
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
  EXPECT_FALSE(fx.controller.send_heating_command("ABC123", HeatingFunction::SET_TEMPERATURE, 20.5F));
  EXPECT_GT(fx.radio.get_sent_data().size(), static_cast<size_t>(SCHEDULED_POLL_MAX_TRIES));
  EXPECT_LE(fx.radio.get_sent_data().size(), static_cast<size_t>(EXCHANGE_RETRY_COUNT));
}

TEST(HeaterController, TowelDryerModeAndTemporaryStateWritesSendExactBytes) {
  HeaterControllerFixture fx;
  make_towel_dryer(fx);
  fx.queue_register_ack({0x0C, 0x61, 0x01, 0x36, 0x03});
  ASSERT_TRUE(fx.controller.send_heating_command("ABC123", HeatingFunction::TOWEL_MODE, 3.0F));
  fx.queue_register_ack({0x0C, 0x61, 0x01, 0x3E, 0x01});
  ASSERT_TRUE(fx.controller.send_heating_command("ABC123", HeatingFunction::TEMPORARY_STATE, 1.0F));

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
      fx.controller.read_heating_register("ABC123", HeatingFunction::ROOM_TEMPERATURE, &value, EXCHANGE_RETRY_COUNT));

  EXPECT_FLOAT_EQ(value, 20.75F);
  EXPECT_FLOAT_EQ(dev->heating.room_temperature_c, 20.75F);
  ASSERT_EQ(fx.radio.get_sent_data().size(), 1u);
  const auto &tx = fx.radio.get_sent_data().front();
  EXPECT_EQ(tx[8], CMD_WRITE_PRIVATE);
  EXPECT_EQ(tx[9], 0x0C);
  EXPECT_EQ(tx[10], 0x60) << "a read is a get";
  EXPECT_EQ(tx[11], 0x01);
  EXPECT_EQ(tx[12], 0x15);
}

TEST(HeaterController, ReadHeatingRegisterNotifiesTheEntities) {
  HeaterControllerFixture fx;
  make_heater(fx);
  int notifications = 0;
  fx.registry.subscribe([&notifications](const std::string &, const IoDevice &) { notifications++; });
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x15, 0x1B, 0x08});

  ASSERT_TRUE(
      fx.controller.read_heating_register("ABC123", HeatingFunction::ROOM_TEMPERATURE, nullptr, EXCHANGE_RETRY_COUNT));

  EXPECT_GE(notifications, 1) << "what an entity shows is what the heater reported";
}

TEST(HeaterController, ReadHeatingRegisterWithoutAValueInTheReplyFails) {
  HeaterControllerFixture fx;
  const IoDevice *dev = make_heater(fx);
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x15});

  EXPECT_FALSE(
      fx.controller.read_heating_register("ABC123", HeatingFunction::ROOM_TEMPERATURE, nullptr, EXCHANGE_RETRY_COUNT));
  EXPECT_TRUE(std::isnan(dev->heating.room_temperature_c)) << "nothing is stored from a reply without a value";
}

TEST(HeaterController, ReadHeatingRegisterRejectsWhatItCannotRead) {
  HeaterControllerFixture fx;
  make_heater(fx);
  EXPECT_FALSE(
      fx.controller.read_heating_register("ABC123", HeatingFunction::SET_PRESENCE, nullptr, EXCHANGE_RETRY_COUNT))
      << "write-only register";
  EXPECT_TRUE(fx.radio.get_sent_data().empty());

  HeaterControllerFixture cover;
  make_cover(cover);
  EXPECT_FALSE(cover.controller.read_heating_register("ABC123", HeatingFunction::ROOM_TEMPERATURE, nullptr,
                                                      EXCHANGE_RETRY_COUNT));
  EXPECT_TRUE(cover.radio.get_sent_data().empty());
}

TEST(HeaterController, ReadPrivateRegisterChecksTheEchoAndRefusesADeviceWithoutRegisters) {
  HeaterControllerFixture fx;
  make_heater(fx);
  fx.queue_register_ack(ADELIS_NAME_REPLY);
  IoFrame response{};
  ASSERT_TRUE(fx.controller.read_private_register("ABC123", 0x01, 0x19, "model_name", response, 1));
  EXPECT_EQ(response.data_len, ADELIS_NAME_REPLY.size());
  EXPECT_EQ(response.data[HEATING_REPLY_VALUE_OFFSET], 'A');
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

TEST(HeaterController, SetDeviceTemperatureRangeStoresTheRangeOnTheDevice) {
  HeaterControllerFixture fx;
  const IoDevice *dev = make_heater(fx);

  fx.controller.set_device_temperature_range("ABC123", 7.0F, 28.0F);
  fx.controller.set_device_temperature_range("DEF456", 1.0F, 2.0F);  // unknown: no effect, no crash

  EXPECT_FLOAT_EQ(dev->temperature_range_min_c, 7.0F);
  EXPECT_FLOAT_EQ(dev->temperature_range_max_c, 28.0F);
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

// === Water heater ===

TEST(HeaterController, ATargetOutsideTheHeatersRangeIsNeverSent) {
  HeaterControllerFixture fx;
  make_water_heater(fx);
  EXPECT_FALSE(fx.controller.set_water_heater_target("ABC123", 19.5F));
  EXPECT_FALSE(fx.controller.set_water_heater_target("ABC123", 62.5F));
  EXPECT_FALSE(fx.controller.set_water_heater_target("ABC123", NAN));
  EXPECT_TRUE(fx.radio.get_sent_data().empty());
  EXPECT_TRUE(fx.executes.empty());
}

TEST(HeaterController, TheTargetIsAMainParameterOnlyExecuteThroughTheBareOperation) {
  HeaterControllerFixture fx;
  const IoDevice *dev = make_water_heater(fx);

  EXPECT_TRUE(fx.controller.set_water_heater_target("ABC123", 54.0F));

  ASSERT_EQ(fx.executes.size(), 1u);
  EXPECT_EQ(fx.executes[0].action, "hot water target");
  EXPECT_EQ(fx.executes[0].rejection_profile, "water heater");
  EXPECT_EQ(fx.executes[0].kind, HeaterExecuteKind::TRY) << "a water heater never moves: no overlay to roll back";
  ASSERT_TRUE(fx.executes[0].built);
  IoFrame expected;
  ASSERT_TRUE(create_execute_heating_main_parameter(expected, fx.node_id, dev->node_id, false, 0xA1E8));
  uint8_t expected_raw[64];
  uint8_t got_raw[64];
  const uint8_t expected_len = serialize(expected, expected_raw, sizeof(expected_raw));
  const uint8_t got_len = serialize(fx.executes[0].request, got_raw, sizeof(got_raw));
  EXPECT_EQ(std::vector<uint8_t>(got_raw, got_raw + got_len),
            std::vector<uint8_t>(expected_raw, expected_raw + expected_len));
  ASSERT_EQ(fx.timers.size(), 1u);
  EXPECT_EQ(fx.timers[0].id, decisions::heating_poll_timer_id(dev->node_id));
  EXPECT_EQ(fx.timers[0].delay_ms, HEATING_POST_WRITE_POLL_DELAY_MS) << "the heating state is read again soon";
}

TEST(HeaterController, ARefusedTargetExecuteMovesNoPoll) {
  HeaterControllerFixture fx;
  make_water_heater(fx);
  fx.execute_result = false;

  EXPECT_FALSE(fx.controller.set_water_heater_target("ABC123", 54.0F));
  EXPECT_TRUE(fx.timers.empty());
}

TEST(HeaterController, AnUnacknowledgedModeWriteIsNotReadBack) {
  HeaterControllerFixture fx;
  const IoDevice *dev = make_water_heater(fx);
  EXPECT_FALSE(fx.controller.set_water_heater_mode("ABC123", DhwMode::AUTO));
  for (const auto &tx : fx.radio.get_sent_data())
    EXPECT_EQ(tx[10], HEATING_DIRECTION_SET) << "only the write is tried";
  EXPECT_LE(fx.radio.get_sent_data().size(), static_cast<size_t>(EXCHANGE_RETRY_COUNT));
  EXPECT_EQ(dev->dhw.mode, -1);
  EXPECT_TRUE(fx.status_reads.empty());
}

TEST(HeaterController, TheModeIsARegisterWriteReadBackAtOnceThenTheStatusIsReadForTheTarget) {
  HeaterControllerFixture fx;
  const IoDevice *dev = make_water_heater(fx);
  fx.queue_register_ack({0x0C, 0x61, 0x00, 0x06, 0x02});
  fx.queue_register_ack({0x0C, 0x60, 0x00, 0x06, 0x02});

  EXPECT_TRUE(fx.controller.set_water_heater_mode("ABC123", DhwMode::MANUAL_ECO_OFF));

  ASSERT_EQ(fx.radio.get_sent_data().size(), 2u);
  const auto &write = fx.radio.get_sent_data()[0];
  EXPECT_EQ(write[8], CMD_WRITE_PRIVATE);
  EXPECT_EQ(sent_payload(write, 5), (std::vector<uint8_t>{0x0C, 0x61, 0x00, 0x06, 0x02}));
  EXPECT_EQ(sent_payload(fx.radio.get_sent_data()[1], 4), (std::vector<uint8_t>{0x0C, 0x60, 0x00, 0x06}));
  EXPECT_EQ(dev->dhw.mode, static_cast<int16_t>(DhwMode::MANUAL_ECO_OFF));
  ASSERT_EQ(fx.status_reads.size(), 1u) << "the target follows the mode: the poll's status read";
  EXPECT_EQ(fx.status_reads[0].request.cmd, CMD_PRIVATE);
  EXPECT_EQ(fx.status_reads[0].request.data[0], PRIVATE_STATUS_MP_FP);
  EXPECT_EQ(fx.status_reads[0].retry_after_fail_ms, 0u) << "the status read arms no backoff of its own";
  EXPECT_EQ(fx.status_reads[0].max_tries, EXCHANGE_RETRY_COUNT);
  ASSERT_FALSE(fx.timers.empty());
  EXPECT_EQ(fx.timers.back().delay_ms, HEATING_POST_WRITE_POLL_DELAY_MS);
}

TEST(HeaterController, AMissedStatusReadAfterAModeWriteDoesNotFailTheWrite) {
  HeaterControllerFixture fx;
  make_water_heater(fx);
  fx.status_read_result = false;
  fx.queue_register_ack({0x0C, 0x61, 0x00, 0x06, 0x00});
  fx.queue_register_ack({0x0C, 0x60, 0x00, 0x06, 0x00});

  EXPECT_TRUE(fx.controller.set_water_heater_mode("ABC123", DhwMode::AUTO));
  ASSERT_FALSE(fx.timers.empty());
  EXPECT_EQ(fx.timers.back().delay_ms, HEATING_POST_WRITE_POLL_DELAY_MS) << "the poll still reads it soon";
}

TEST(HeaterController, BoostOnWritesTheDurationThenTheBoostWordThenReadsTheStatus) {
  HeaterControllerFixture fx;
  make_water_heater(fx);
  fx.queue_register_ack({0x0C, 0x61, 0x00, 0x00, 0x00});  // the heater acks the duration without keeping it
  fx.queue_register_ack({0x0C, 0x60, 0x00, 0x00, 0x01});  // its read-back

  EXPECT_TRUE(fx.controller.set_water_heater_boost("ABC123", true));

  ASSERT_EQ(fx.radio.get_sent_data().size(), 2u);
  EXPECT_EQ(sent_payload(fx.radio.get_sent_data()[0], 5), (std::vector<uint8_t>{0x0C, 0x61, 0x00, 0x00, 0x01}))
      << "the duration goes first, although the heater ignores it on its own";
  ASSERT_EQ(fx.executes.size(), 1u);
  EXPECT_EQ(fx.executes[0].action, "hot water boost");
  EXPECT_EQ(fx.executes[0].kind, HeaterExecuteKind::TRY);
  ASSERT_TRUE(fx.executes[0].built);
  const IoFrame &execute = fx.executes[0].request;
  EXPECT_EQ(std::vector<uint8_t>(execute.data + execute.data_len - 6, execute.data + execute.data_len),
            (std::vector<uint8_t>{0xD4, 0x00, 0x00, 0x01, 0x49, 0x00}));
  EXPECT_EQ(fx.status_reads.size(), 1u) << "the status is read at once";
}

TEST(HeaterController, SwitchingOffSendsTheOffWordOnlyAndReadsTheStatus) {
  for (const bool boost : {true, false}) {
    HeaterControllerFixture fx;
    make_water_heater(fx);

    EXPECT_TRUE(boost ? fx.controller.set_water_heater_boost("ABC123", false)
                      : fx.controller.set_water_heater_away("ABC123", false));

    EXPECT_TRUE(fx.radio.get_sent_data().empty()) << boost << ": no duration write, no read-back of one";
    ASSERT_EQ(fx.executes.size(), 1u);
    EXPECT_EQ(fx.executes[0].action, "hot water boost and away off");
    const IoFrame &execute = fx.executes[0].request;
    EXPECT_EQ(std::vector<uint8_t>(execute.data + execute.data_len - 6, execute.data + execute.data_len),
              (std::vector<uint8_t>{0xD4, 0x00, 0x00, 0x01, 0x4A, 0x00}));
    EXPECT_EQ(fx.status_reads.size(), 1u);
  }
}

TEST(HeaterController, AnUnacknowledgedModeWordIsNotReadBack) {
  HeaterControllerFixture fx;
  make_water_heater(fx);
  fx.execute_result = false;

  EXPECT_FALSE(fx.controller.set_water_heater_boost("ABC123", false));
  EXPECT_TRUE(fx.status_reads.empty());
}

TEST(HeaterController, AWriteWhoseCapabilityBitIsClearIsRefusedOnceFp15IsRead) {
  HeaterControllerFixture fx;
  IoDevice *dev = make_water_heater(fx);
  dev->dhw.capabilities = DHW_CAPABILITY_AWAY;  // no boost
  EXPECT_FALSE(fx.controller.set_water_heater_boost("ABC123", true));
  EXPECT_FALSE(fx.controller.set_water_heater_boost("ABC123", false))
      << "off is refused too: the word is not supported";
  dev->dhw.capabilities = DHW_CAPABILITY_BOOST;  // no away
  EXPECT_FALSE(fx.controller.set_water_heater_away("ABC123", true));
  EXPECT_TRUE(fx.radio.get_sent_data().empty()) << "nothing reaches the radio";
  EXPECT_TRUE(fx.executes.empty());
}

TEST(HeaterController, ARadiatorAndACoverAreNeverSentAFamilyZeroFrame) {
  for (const DeviceType type : {DeviceType::ELECTRICAL_HEATER, DeviceType::ROLLER_SHUTTER}) {
    HeaterControllerFixture fx;
    fx.add_device_of_kind(type, type == DeviceType::ELECTRICAL_HEATER ? 1 : 0);
    IoFrame response;
    EXPECT_FALSE(fx.controller.read_dhw_register("ABC123", DHW_REG_MODE, "dhw_mode", response, 1));
    EXPECT_FALSE(fx.controller.read_dhw_reading("ABC123", DHW_REG_MODE, 1));
    EXPECT_FALSE(fx.controller.set_water_heater_mode("ABC123", DhwMode::AUTO));
    EXPECT_TRUE(fx.radio.get_sent_data().empty()) << static_cast<unsigned>(type);
  }
}

TEST(HeaterController, AReadingIsFoldedIntoTheWaterHeaterRecord) {
  HeaterControllerFixture fx;
  const IoDevice *dev = make_water_heater(fx);
  fx.queue_register_ack({0x0C, 0x60, 0x00, DHW_REG_MODE, 0x01});  // manual, ECO on

  EXPECT_TRUE(fx.controller.read_dhw_reading("ABC123", DHW_REG_MODE, 1));

  EXPECT_EQ(dev->dhw.mode, static_cast<int16_t>(DhwMode::MANUAL_ECO_ON));
}

TEST(HeaterController, ADhwStatusReadAsksForTheMainParameterAndFp15AndFp16) {
  HeaterControllerFixture fx;
  make_water_heater(fx);

  EXPECT_TRUE(fx.controller.read_dhw_status("ABC123", 2));
  ASSERT_EQ(fx.status_reads.size(), 1u);
  EXPECT_EQ(fx.status_reads[0].request.cmd, CMD_PRIVATE);
  EXPECT_EQ(fx.status_reads[0].request.data[1], DHW_STATUS_FPI1);
  EXPECT_EQ(fx.status_reads[0].request.data[2], DHW_STATUS_FPI2);
  EXPECT_EQ(fx.status_reads[0].max_tries, 2);

  fx.status_read_result = false;
  EXPECT_FALSE(fx.controller.read_dhw_status("ABC123", 2));
  EXPECT_FALSE(fx.controller.read_dhw_status("DEF456", 2)) << "an unknown device is never asked";
  EXPECT_EQ(fx.status_reads.size(), 2u);
}

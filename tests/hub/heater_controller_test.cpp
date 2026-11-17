/// @file heater_controller_test.cpp
/// @brief HeaterController without a hub: the register heater's writes and reads, the water heater's writes and
/// reads, and the contract of the calls it makes on the hub (HeaterControllerFixture).
///
/// What crosses the hub's own bookkeeping (device event reports, exchange counters, the status decoder, the
/// settle poll) is tested through the hub's forwarders in hub_operations_test.cpp and hub_dhw_test.cpp.

#include "heater_controller.h"
#include "decisions_status.h"
#include "proto_codecs.h"
#include "proto_commands.h"
#include "proto_dhw.h"
#include "proto_frame.h"
#include "proto_heating.h"
#include "proto_private_registers.h"

#include "heater_controller_fixture.h"
#include "heating_test_data.h"

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

using heating_test_data::ADELIS_NAME_REPLY;

/// Run send_heating_command against an electrical heater with an adjustable setpoint with a single reply frame
/// (`cmd` and `payload`) queued as the device's answer.
bool run_heating_command_with_reply(uint8_t cmd, const std::vector<uint8_t> &payload) {
  HeaterControllerFixture fx;
  make_heater(fx);
  fx.queue_reply(cmd, payload);
  return fx.controller.send_heating_command("ABC123", HeatingFunction::POWER_ON, 0.0F);
}

/// Register low byte of a sent CMD_WRITE_PRIVATE frame (payload `0C <dir> 01 <reg>` starts at offset 9).
uint8_t sent_register(const std::vector<uint8_t> &tx) { return tx.at(12); }

/// Queue the replies of one answered register-heater poll of the radiator (the five poll registers, then the
/// open-window-detection setting).
void queue_radiator_poll(HeaterControllerFixture &fx, uint8_t room_hi, uint8_t room_lo, uint8_t regulator,
                         uint8_t window, uint8_t detection = 0x01) {
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x15, room_hi, room_lo});
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x03, 0xC3, 0x00});
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x00, 0x01});
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x17, regulator});
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x0E, window});
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x06, detection});
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

// === Planned climate writes ===

namespace {

/// A plan that writes one register and reads it back.
HeatingWritePlan register_plan(HeatingFunction fn, float value) {
  HeatingWritePlan plan;
  plan.kind = HeatingPlanKind::REGISTER;
  plan.register_fn = fn;
  plan.register_value = value;
  plan.read_back = true;
  plan.read_back_fn = fn;
  return plan;
}

}  // namespace

TEST(HeaterController, APlannedRegisterWriteSendsItsBytesAppliesTheHookThenReadsBack) {
  HeaterControllerFixture fx;
  make_heater(fx);
  fx.queue_register_ack({0x0C, 0x61, 0x01, 0x03, 0xCD, 0x00});
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x03, 0xCD, 0x00});
  size_t frames_when_accepted = 0;
  int hook_calls = 0;

  const bool accepted =
      fx.controller.execute_heating_plan("ABC123", register_plan(HeatingFunction::SET_TEMPERATURE, 20.5F), [&] {
        hook_calls++;
        frames_when_accepted = fx.radio.get_sent_data().size();
      });

  EXPECT_TRUE(accepted);
  EXPECT_EQ(hook_calls, 1);
  EXPECT_EQ(frames_when_accepted, 1u) << "the hook runs after the write and before the read-back";
  ASSERT_EQ(fx.radio.get_sent_data().size(), 2u);
  EXPECT_EQ(sent_payload(fx.radio.get_sent_data()[0], 6), (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x03, 0xCD, 0x00}));
  EXPECT_EQ(fx.radio.get_sent_data()[1][10], 0x60) << "the read-back is a get of the same register";
  EXPECT_EQ(sent_register(fx.radio.get_sent_data()[1]), 0x03);
}

namespace {

/// A towel dryer in its programme, comfort 19.0 C, holding @p in_force_c (a number marks it reported), whose
/// status read reported @p level.
IoDevice *make_towel_dryer_in_programme(HeaterControllerFixture &fx, float in_force_c, uint8_t level) {
  IoDevice *dev = make_towel_dryer(fx);
  fx.status_reply_level = level;  // the level the next status read answers
  dev->heating.towel_mode = static_cast<int16_t>(TowelDryerMode::PROG);
  dev->heating.temporary_state = static_cast<int16_t>(HeatingTemporaryState::NONE);
  dev->heating.setpoint_c = 19.0F;
  dev->heating.level = level;
  dev->heating.in_force_temperature_c = in_force_c;
  if (!std::isnan(in_force_c))
    dev->temperature_in_force = TemperatureInForceSupport::REPORTED;
  return dev;
}

/// A radiator running its programme in @p mode, comfort 19.0 C, holding @p in_force_c (a number marks it
/// reported).
IoDevice *make_radiator_in_programme(HeaterControllerFixture &fx, HeatingMode mode, float in_force_c) {
  IoDevice *dev = make_heater(fx);
  dev->heating.mode = static_cast<int16_t>(mode);
  dev->heating.setpoint_c = 19.0F;
  dev->heating.in_force_temperature_c = in_force_c;
  if (!std::isnan(in_force_c))
    dev->temperature_in_force = TemperatureInForceSupport::REPORTED;
  return dev;
}

/// An override of @p value written to the override register with its read-back: a plan, not a decision, so the
/// tests of execute_heating_plan() do not depend on any stored reading.
HeatingWritePlan override_plan(float value) {
  HeatingWritePlan plan = register_plan(HeatingFunction::DEROGATED_SETPOINT, value);
  plan.temporary_override = true;
  return plan;
}

/// Queue the acknowledgement of a write of register @p reg and the read-back reply of @p tenths_lo / 0.
void queue_register_write_and_read_back(HeaterControllerFixture &fx, uint8_t reg, uint8_t write_lo, uint8_t back_lo) {
  fx.queue_register_ack({0x0C, 0x61, 0x01, reg, write_lo, 0x00});
  fx.queue_register_ack({0x0C, 0x60, 0x01, reg, back_lo, 0x00});
}

/// The registers a frame list read or wrote, in order.
std::vector<uint8_t> sent_registers(const HeaterControllerFixture &fx) {
  std::vector<uint8_t> registers;
  for (const auto &frame : fx.radio.get_sent_data())
    registers.push_back(sent_register(frame));
  return registers;
}

/// The registers of the frames sent since @p before frames.
std::vector<uint8_t> sent_registers_since(const HeaterControllerFixture &fx, size_t before) {
  std::vector<uint8_t> registers = sent_registers(fx);
  registers.erase(registers.begin(), registers.begin() + static_cast<std::ptrdiff_t>(before));
  return registers;
}

/// Whether the frame is a write (`0x61`) and not a read.
bool is_write_frame(const std::vector<uint8_t> &tx) { return tx.at(10) == 0x61; }

/// The writes a fixture's radio carried, as register numbers.
std::vector<uint8_t> written_registers(const HeaterControllerFixture &fx) {
  std::vector<uint8_t> registers;
  for (const auto &frame : fx.radio.get_sent_data()) {
    if (is_write_frame(frame))
      registers.push_back(sent_register(frame));
  }
  return registers;
}

/// What the climate entity does for a target: ask the heater what it runs, plan from the record, and send.
struct TargetOutcome {
  bool state_answered{false};  ///< The heater answered the mode (and the timed run where it matters).
  HeatingWritePlan plan;       ///< What the target became.
  bool sent{false};            ///< The write was sent and acknowledged.
};

TargetOutcome set_target(HeaterControllerFixture &fx, float target_c) {
  TargetOutcome outcome;
  outcome.state_answered = fx.controller.read_programme_state("ABC123");
  const IoDevice *dev = fx.registry.get("ABC123");
  outcome.plan =
      plan_heating_write(*find_heating_profile(*dev), {.kind = HeatingWriteKind::TARGET, .target_c = target_c},
                         heating_device_facts(dev, outcome.state_answered));
  outcome.sent = outcome.plan.is_write() && fx.controller.execute_heating_plan("ABC123", outcome.plan, nullptr);
  return outcome;
}

/// Queue the heater's reply to a read of register @p reg with the value bytes @p value.
void queue_read_reply(HeaterControllerFixture &fx, uint8_t reg, std::vector<uint8_t> value) {
  std::vector<uint8_t> payload = {0x0C, 0x60, 0x01, reg};
  payload.insert(payload.end(), value.begin(), value.end());
  fx.queue_register_ack(payload);
}

constexpr uint8_t RADIATOR_MODE = 0x00;
constexpr uint8_t RADIATOR_IN_FORCE = 0x2A;
constexpr uint8_t DRYER_MODE = 0x36;
constexpr uint8_t DRYER_TIMED_RUN = 0x3E;
constexpr uint8_t DRYER_IN_FORCE = 0x37;
constexpr uint8_t OVERRIDE_REGISTER = 0x1D;
constexpr uint8_t COMFORT_REGISTER = 0x03;

constexpr uint8_t RADIATOR_HEAT = static_cast<uint8_t>(HeatingMode::MANUAL);
constexpr uint8_t RADIATOR_PROGRAMME = static_cast<uint8_t>(HeatingMode::PROG);
constexpr uint8_t DRYER_HEAT = static_cast<uint8_t>(TowelDryerMode::MANUAL);
constexpr uint8_t DRYER_PROGRAMME = static_cast<uint8_t>(TowelDryerMode::PROG);
constexpr uint8_t NO_TIMED_RUN = static_cast<uint8_t>(HeatingTemporaryState::NONE);
constexpr uint8_t TIMED_BOOST = static_cast<uint8_t>(HeatingTemporaryState::BOOST);

}  // namespace

TEST(HeaterController, AnOverrideInAnEcoOrAComfortPeriodSendsTheOverrideRegisterAndShowsTheNewTargetAtTheReadBack) {
  for (const uint8_t level : {heating_level::ECO, heating_level::COMFORT}) {
    HeaterControllerFixture fx;
    IoDevice *dev = make_towel_dryer_in_programme(fx, level == heating_level::ECO ? 15.5F : 19.0F, level);
    const HeatingProfile &profile = *find_heating_profile(*dev);
    float seen = NAN;
    fx.registry.subscribe([&seen](const std::string &, const IoDevice &d) { seen = d.heating.in_force_temperature_c; });
    queue_register_write_and_read_back(fx, 0x1D, 0xAF, 0xAF);

    ASSERT_TRUE(fx.controller.execute_heating_plan("ABC123", override_plan(17.5F), nullptr)) << "level " << +level;

    ASSERT_EQ(fx.radio.get_sent_data().size(), 2u);
    EXPECT_EQ(sent_payload(fx.radio.get_sent_data()[0], 6), (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x1D, 0xAF, 0x00}));
    EXPECT_EQ(sent_register(fx.radio.get_sent_data()[1]), 0x1D) << "the read-back is of the override register";
    for (const auto &frame : fx.radio.get_sent_data())
      EXPECT_NE(sent_register(frame), 0x03) << "the comfort setpoint is never written";
    EXPECT_FLOAT_EQ(seen, 17.5F) << "the card shows the new target before the next poll";
    EXPECT_FLOAT_EQ(dev->heating.setpoint_c, 19.0F) << "the comfort setpoint is untouched";

    // The next poll's 0x37 replaces the shown value.
    fx.queue_register_ack({0x0C, 0x60, 0x01, 0x37, 0xAA, 0x00});
    ASSERT_TRUE(fx.controller.read_heating_register("ABC123", profile.programme_period->in_force_fn, nullptr,
                                                    EXCHANGE_RETRY_COUNT));
    EXPECT_FLOAT_EQ(dev->heating.in_force_temperature_c, 17.0F) << "what the dryer reports replaces the shown value";
  }
}

// === The temperature in force is read once after a write that starts the programme (display only) ===

namespace {

/// Whether a device reports its temperature in force, as one of the stored readings of a heater's record.
float stored_in_force(const IoDevice &dev) { return dev.heating.in_force_temperature_c; }

}  // namespace

TEST(HeaterController, AModeWriteIntoTheProgrammeIsReadBackThenTheTemperatureInForceIsReadOnce) {
  struct Case {
    bool towel;
    uint8_t mode_register;
    uint8_t in_force_register;
    uint8_t from_wire;
    uint8_t to_wire;
  };
  for (const Case &c :
       {Case{true, DRYER_MODE, DRYER_IN_FORCE, DRYER_HEAT, DRYER_PROGRAMME},
        Case{false, RADIATOR_MODE, RADIATOR_IN_FORCE, RADIATOR_HEAT, static_cast<uint8_t>(HeatingMode::AUTO)}}) {
    HeaterControllerFixture fx;
    IoDevice *dev = c.towel ? make_towel_dryer_in_programme(fx, NAN, heating_level::COMFORT)
                            : make_radiator_in_programme(fx, HeatingMode::MANUAL, NAN);
    if (c.towel)
      dev->heating.towel_mode = c.from_wire;
    const HeatingFunction mode_fn = find_heating_profile(*dev)->mode_fn;
    fx.queue_register_ack({0x0C, 0x61, 0x01, c.mode_register, c.to_wire});
    fx.queue_register_ack({0x0C, 0x60, 0x01, c.mode_register, c.to_wire});
    queue_read_reply(fx, c.in_force_register, {0xAF, 0x00});  // 17.5 C
    ASSERT_EQ(dev->temperature_in_force, TemperatureInForceSupport::UNKNOWN);

    ASSERT_TRUE(fx.controller.execute_heating_plan("ABC123", register_plan(mode_fn, c.to_wire), nullptr));

    EXPECT_EQ(sent_registers(fx), (std::vector<uint8_t>{c.mode_register, c.mode_register, c.in_force_register}))
        << "write, read-back, one read of the temperature in force, towel " << c.towel;
    EXPECT_FLOAT_EQ(stored_in_force(*dev), 17.5F);
    EXPECT_EQ(dev->temperature_in_force, TemperatureInForceSupport::REPORTED);
  }
}

TEST(HeaterController, AWriteThatDoesNotStartTheProgrammeReadsNoTemperatureInForce) {
  // A mode write into heat, a boost started on the dryer: the read-back shows no programme running.
  {
    HeaterControllerFixture fx;
    IoDevice *dev = make_radiator_in_programme(fx, HeatingMode::PROG, 17.0F);
    queue_register_write_and_read_back(fx, RADIATOR_MODE, RADIATOR_HEAT, RADIATOR_HEAT);
    ASSERT_TRUE(fx.controller.execute_heating_plan(
        "ABC123", register_plan(find_heating_profile(*dev)->mode_fn, RADIATOR_HEAT), nullptr));
    EXPECT_EQ(sent_registers(fx), (std::vector<uint8_t>{RADIATOR_MODE, RADIATOR_MODE}));
  }
  {
    HeaterControllerFixture fx;
    make_towel_dryer_in_programme(fx, NAN, heating_level::COMFORT);
    queue_register_write_and_read_back(fx, DRYER_MODE, DRYER_HEAT, DRYER_HEAT);
    ASSERT_TRUE(
        fx.controller.execute_heating_plan("ABC123", register_plan(HeatingFunction::TOWEL_MODE, DRYER_HEAT), nullptr));
    EXPECT_EQ(sent_registers(fx), (std::vector<uint8_t>{DRYER_MODE, DRYER_MODE}));
  }
  {
    HeaterControllerFixture fx;
    make_towel_dryer_in_programme(fx, NAN, heating_level::COMFORT);
    queue_register_write_and_read_back(fx, DRYER_TIMED_RUN, TIMED_BOOST, TIMED_BOOST);
    ASSERT_TRUE(fx.controller.execute_heating_plan(
        "ABC123", register_plan(HeatingFunction::TEMPORARY_STATE, TIMED_BOOST), nullptr));
    EXPECT_EQ(sent_registers(fx), (std::vector<uint8_t>{DRYER_TIMED_RUN, DRYER_TIMED_RUN}))
        << "a boost on top of the programme is not the programme running";
  }
}

TEST(HeaterController, ADryerBoostEndedWhileTheModeShowsTheProgrammeReadsTheTemperatureInForce) {
  HeaterControllerFixture fx;
  IoDevice *dev = make_towel_dryer_in_programme(fx, NAN, heating_level::COMFORT);
  dev->heating.temporary_state = TIMED_BOOST;
  queue_register_write_and_read_back(fx, DRYER_TIMED_RUN, NO_TIMED_RUN, NO_TIMED_RUN);
  queue_read_reply(fx, DRYER_IN_FORCE, {0xAF, 0x00});

  ASSERT_TRUE(fx.controller.execute_heating_plan(
      "ABC123", register_plan(HeatingFunction::TEMPORARY_STATE, NO_TIMED_RUN), nullptr));

  EXPECT_EQ(sent_registers(fx), (std::vector<uint8_t>{DRYER_TIMED_RUN, DRYER_TIMED_RUN, DRYER_IN_FORCE}));
  EXPECT_FLOAT_EQ(stored_in_force(*dev), 17.5F);
}

TEST(HeaterController, ASilentModeReadBackReadsNoTemperatureInForce) {
  // The stored mode already shows the programme, so only the missing read-back keeps the read away.
  HeaterControllerFixture fx;
  IoDevice *dev = make_radiator_in_programme(fx, HeatingMode::PROG, NAN);
  fx.queue_register_ack({0x0C, 0x61, 0x01, RADIATOR_MODE, static_cast<uint8_t>(HeatingMode::AUTO)});  // no read-back

  ASSERT_TRUE(fx.controller.execute_heating_plan(
      "ABC123", register_plan(find_heating_profile(*dev)->mode_fn, static_cast<float>(HeatingMode::AUTO)), nullptr));

  for (const uint8_t reg : sent_registers(fx))
    EXPECT_EQ(reg, RADIATOR_MODE) << "the write and its read-back attempts only";
}

TEST(HeaterController, AHeaterWithoutATemperatureInForceIsNotAskedAfterAModeWrite) {
  HeaterControllerFixture fx;
  IoDevice *dev = make_radiator_in_programme(fx, HeatingMode::MANUAL, NAN);
  dev->temperature_in_force = TemperatureInForceSupport::ABSENT;
  const uint8_t auto_wire = static_cast<uint8_t>(HeatingMode::AUTO);
  queue_register_write_and_read_back(fx, RADIATOR_MODE, auto_wire, auto_wire);

  ASSERT_TRUE(fx.controller.execute_heating_plan(
      "ABC123", register_plan(find_heating_profile(*dev)->mode_fn, auto_wire), nullptr));

  EXPECT_EQ(sent_registers(fx), (std::vector<uint8_t>{RADIATOR_MODE, RADIATOR_MODE}));
}

TEST(HeaterController, AProfileWithoutAProgrammeIsNotAskedAfterAWrite) {
  // A heating-level heater has no programme period: its plan has no register read-back and nothing is read.
  HeaterControllerFixture fx;
  ASSERT_NE(fx.add_device_of_kind(DeviceType::ELECTRICAL_HEATER, 0), nullptr);
  ASSERT_EQ(find_heating_profile(*fx.registry.get("ABC123"))->programme_period, nullptr);
  HeatingWritePlan plan;
  plan.kind = HeatingPlanKind::LEVEL;
  plan.level = heating_level::ECO;

  EXPECT_TRUE(fx.controller.execute_heating_plan("ABC123", plan, nullptr));

  EXPECT_TRUE(fx.radio.get_sent_data().empty()) << "the level is an execute, and no register is read";
}

TEST(HeaterController, ASilentTemperatureInForceReadAfterAModeWriteChangesNothingElse) {
  HeaterControllerFixture fx;
  IoDevice *dev = make_radiator_in_programme(fx, HeatingMode::MANUAL, NAN);
  const uint8_t auto_wire = static_cast<uint8_t>(HeatingMode::AUTO);
  queue_register_write_and_read_back(fx, RADIATOR_MODE, auto_wire, auto_wire);  // the in-force read gets no reply

  ASSERT_TRUE(fx.controller.execute_heating_plan(
      "ABC123", register_plan(find_heating_profile(*dev)->mode_fn, auto_wire), nullptr));

  const std::vector<uint8_t> registers = sent_registers(fx);
  ASSERT_GE(registers.size(), 3u);
  EXPECT_EQ(registers[2], RADIATOR_IN_FORCE);
  EXPECT_EQ(dev->temperature_in_force, TemperatureInForceSupport::UNKNOWN) << "a missed read says nothing";
  EXPECT_TRUE(std::isnan(stored_in_force(*dev)));
  EXPECT_EQ(dev->heating.mode, auto_wire) << "the mode the read-back stored stays";
  EXPECT_TRUE(written_registers(fx) == std::vector<uint8_t>{RADIATOR_MODE}) << "nothing else is written";
}

TEST(HeaterController, AnEmptyTemperatureInForceAfterAWriteLeavesTheTargetsOwnReadToDecide) {
  // The display read goes by the stored readings, and on a dryer one of the two registers that say "the programme
  // runs" was not read with the write: the heater may run something else and answer the register empty. Only the
  // target's own read, made on fresh readings, may mark the heater as having no temperature in force.
  struct Case {
    const char *what;
    uint8_t written_register;
    uint8_t written_wire;
    uint8_t stored_mode;
    uint8_t stored_timed_run;
  };
  for (const Case &c :
       {Case{"mode written, stored timed run stale", DRYER_MODE, DRYER_PROGRAMME, DRYER_HEAT, NO_TIMED_RUN},
        Case{"boost ended, stored mode stale", DRYER_TIMED_RUN, NO_TIMED_RUN, DRYER_PROGRAMME, TIMED_BOOST}}) {
    HeaterControllerFixture fx;
    IoDevice *dev = make_towel_dryer_in_programme(fx, NAN, heating_level::ECO);
    dev->heating.towel_mode = c.stored_mode;
    dev->heating.temporary_state = c.stored_timed_run;
    const HeatingFunction fn =
        c.written_register == DRYER_MODE ? HeatingFunction::TOWEL_MODE : HeatingFunction::TEMPORARY_STATE;
    queue_register_write_and_read_back(fx, c.written_register, c.written_wire, c.written_wire);
    queue_read_reply(fx, DRYER_IN_FORCE, {});  // answered, no value

    ASSERT_TRUE(fx.controller.execute_heating_plan("ABC123", register_plan(fn, c.written_wire), nullptr)) << c.what;
    ASSERT_EQ(sent_registers(fx).back(), DRYER_IN_FORCE) << c.what;
    EXPECT_EQ(dev->temperature_in_force, TemperatureInForceSupport::UNKNOWN) << c.what;

    // The next target asks the heater, which now runs its programme and reports the temperature in force.
    const size_t before = fx.radio.get_sent_data().size();
    queue_read_reply(fx, DRYER_MODE, {DRYER_PROGRAMME});
    queue_read_reply(fx, DRYER_TIMED_RUN, {NO_TIMED_RUN});
    queue_read_reply(fx, DRYER_IN_FORCE, {0xAF, 0x00});
    queue_register_write_and_read_back(fx, OVERRIDE_REGISTER, 0xD2, 0xD2);

    EXPECT_TRUE(set_target(fx, 21.0F).sent) << c.what;

    EXPECT_EQ(sent_registers_since(fx, before),
              (std::vector<uint8_t>{DRYER_MODE, DRYER_TIMED_RUN, DRYER_IN_FORCE, OVERRIDE_REGISTER, OVERRIDE_REGISTER}))
        << c.what;
    EXPECT_EQ(dev->temperature_in_force, TemperatureInForceSupport::REPORTED) << c.what;
  }
}

TEST(HeaterController, AnOverrideWriteReadsNoTemperatureInForceOfItsOwn) {
  // Its read-back already stores the value in force.
  HeaterControllerFixture fx;
  IoDevice *dev = make_radiator_in_programme(fx, HeatingMode::PROG, 17.0F);
  queue_register_write_and_read_back(fx, OVERRIDE_REGISTER, 0xAF, 0xAF);

  ASSERT_TRUE(fx.controller.execute_heating_plan("ABC123", override_plan(17.5F), nullptr));

  EXPECT_EQ(sent_registers(fx), (std::vector<uint8_t>{OVERRIDE_REGISTER, OVERRIDE_REGISTER}));
  EXPECT_FLOAT_EQ(stored_in_force(*dev), 17.5F);
}

// === A target asks the heater what it runs first ===

TEST(HeaterController, ATargetOnAProgrammeHeaterSendsTheSameExchangesEveryTimeWhateverTheStoredReadingsSay) {
  // Radiator (mode 0x00, temperature in force 0x2A) and towel dryer (mode 0x36, timed run 0x3E, temperature in
  // force 0x37). R = read, W = write.
  struct Case {
    const char *what;
    bool towel;
    TemperatureInForceSupport support;
    std::vector<std::vector<uint8_t>> replies;  // to the reads, in order: register, value bytes
    std::vector<uint8_t> sequence;              // registers, in the order they are sent
    std::vector<uint8_t> writes;                // the registers written
  };
  const std::vector<uint8_t> in_force = {0xAF, 0x00};  // 17.5 C
  const std::vector<Case> cases = {
      {"radiator in heat: R mode, W+R comfort",
       false,
       TemperatureInForceSupport::UNKNOWN,
       {{RADIATOR_MODE, RADIATOR_HEAT}},
       {RADIATOR_MODE, COMFORT_REGISTER, COMFORT_REGISTER},
       {COMFORT_REGISTER}},
      {"radiator in the programme, reported: R mode, W+R override",
       false,
       TemperatureInForceSupport::REPORTED,
       {{RADIATOR_MODE, RADIATOR_PROGRAMME}},
       {RADIATOR_MODE, OVERRIDE_REGISTER, OVERRIDE_REGISTER},
       {OVERRIDE_REGISTER}},
      {"radiator in the programme, unknown: R mode, R in force, W+R override",
       false,
       TemperatureInForceSupport::UNKNOWN,
       {{RADIATOR_MODE, RADIATOR_PROGRAMME}, {RADIATOR_IN_FORCE, 0xAF, 0x00}},
       {RADIATOR_MODE, RADIATOR_IN_FORCE, OVERRIDE_REGISTER, OVERRIDE_REGISTER},
       {OVERRIDE_REGISTER}},
      {"radiator in the programme, absent: R mode, W+R comfort",
       false,
       TemperatureInForceSupport::ABSENT,
       {{RADIATOR_MODE, RADIATOR_PROGRAMME}},
       {RADIATOR_MODE, COMFORT_REGISTER, COMFORT_REGISTER},
       {COMFORT_REGISTER}},
      {"dryer in heat: R mode, W+R comfort",
       true,
       TemperatureInForceSupport::UNKNOWN,
       {{DRYER_MODE, DRYER_HEAT}},
       {DRYER_MODE, COMFORT_REGISTER, COMFORT_REGISTER},
       {COMFORT_REGISTER}},
      {"dryer in the programme, reported: R mode, R timed run, W+R override",
       true,
       TemperatureInForceSupport::REPORTED,
       {{DRYER_MODE, DRYER_PROGRAMME}, {DRYER_TIMED_RUN, NO_TIMED_RUN}},
       {DRYER_MODE, DRYER_TIMED_RUN, OVERRIDE_REGISTER, OVERRIDE_REGISTER},
       {OVERRIDE_REGISTER}},
      {"dryer in the programme, unknown: R mode, R timed run, R in force, W+R override",
       true,
       TemperatureInForceSupport::UNKNOWN,
       {{DRYER_MODE, DRYER_PROGRAMME}, {DRYER_TIMED_RUN, NO_TIMED_RUN}, {DRYER_IN_FORCE, 0xAF, 0x00}},
       {DRYER_MODE, DRYER_TIMED_RUN, DRYER_IN_FORCE, OVERRIDE_REGISTER, OVERRIDE_REGISTER},
       {OVERRIDE_REGISTER}},
      {"dryer in the programme, absent: R mode, R timed run, W+R comfort",
       true,
       TemperatureInForceSupport::ABSENT,
       {{DRYER_MODE, DRYER_PROGRAMME}, {DRYER_TIMED_RUN, NO_TIMED_RUN}},
       {DRYER_MODE, DRYER_TIMED_RUN, COMFORT_REGISTER, COMFORT_REGISTER},
       {COMFORT_REGISTER}},
      {"dryer in the programme with a boost on top: R mode, R timed run, W+R comfort",
       true,
       TemperatureInForceSupport::REPORTED,
       {{DRYER_MODE, DRYER_PROGRAMME}, {DRYER_TIMED_RUN, TIMED_BOOST}},
       {DRYER_MODE, DRYER_TIMED_RUN, COMFORT_REGISTER, COMFORT_REGISTER},
       {COMFORT_REGISTER}},
  };
  for (const Case &c : cases) {
    // Whatever the hub stored (heat, or the programme with a boost on a dryer), the heater's answer decides.
    for (const bool stored_in_heat : {false, true}) {
      HeaterControllerFixture fx;
      IoDevice *dev = c.towel ? make_towel_dryer_in_programme(fx, NAN, heating_level::ECO)
                              : make_radiator_in_programme(fx, HeatingMode::MANUAL, NAN);
      dev->temperature_in_force = c.support;
      if (c.support == TemperatureInForceSupport::REPORTED)
        dev->heating.in_force_temperature_c = 17.0F;
      if (c.towel) {
        dev->heating.towel_mode = stored_in_heat ? DRYER_HEAT : DRYER_PROGRAMME;
        dev->heating.temporary_state = stored_in_heat ? TIMED_BOOST : NO_TIMED_RUN;
      } else {
        dev->heating.mode = stored_in_heat ? RADIATOR_HEAT : RADIATOR_PROGRAMME;
      }
      for (const auto &reply : c.replies)
        queue_read_reply(fx, reply[0], std::vector<uint8_t>(reply.begin() + 1, reply.end()));
      fx.queue_register_ack({0x0C, 0x61, 0x01, c.writes.front(), 0xD2, 0x00});
      fx.queue_register_ack({0x0C, 0x60, 0x01, c.writes.front(), 0xD2, 0x00});

      const TargetOutcome outcome = set_target(fx, 21.0F);

      EXPECT_TRUE(outcome.sent) << c.what << ", stored " << stored_in_heat;
      EXPECT_EQ(sent_registers(fx), c.sequence) << c.what << ", stored " << stored_in_heat;
      EXPECT_EQ(written_registers(fx), c.writes) << c.what;
    }
  }
}

TEST(HeaterController, AHeaterNowInTheProgrammeTakesTheTargetAsAnOverrideWhateverTheHubStoredBefore) {
  // Stored: heat. The heater runs its programme (set on its panel since the last poll).
  for (const bool towel : {false, true}) {
    HeaterControllerFixture fx;
    IoDevice *dev = towel ? make_towel_dryer_in_programme(fx, 17.0F, heating_level::ECO)
                          : make_radiator_in_programme(fx, HeatingMode::PROG, 17.0F);
    if (towel)
      dev->heating.towel_mode = DRYER_HEAT;
    else
      dev->heating.mode = RADIATOR_HEAT;
    queue_read_reply(fx, towel ? DRYER_MODE : RADIATOR_MODE, {towel ? DRYER_PROGRAMME : RADIATOR_PROGRAMME});
    if (towel)
      queue_read_reply(fx, DRYER_TIMED_RUN, {NO_TIMED_RUN});
    queue_register_write_and_read_back(fx, OVERRIDE_REGISTER, 0xD2, 0xD2);

    const TargetOutcome outcome = set_target(fx, 21.0F);

    EXPECT_TRUE(outcome.sent) << "towel " << towel;
    EXPECT_TRUE(outcome.plan.temporary_override);
    EXPECT_EQ(written_registers(fx), std::vector<uint8_t>{OVERRIDE_REGISTER}) << "the comfort setpoint is not written";
    EXPECT_FLOAT_EQ(dev->heating.setpoint_c, 19.0F);
  }
}

TEST(HeaterController, AHeaterNowInHeatTakesTheTargetAsTheComfortSetpointWhateverTheHubStoredBefore) {
  // Stored: the programme, with a reported temperature in force. The heater was set to heat.
  for (const bool towel : {false, true}) {
    HeaterControllerFixture fx;
    IoDevice *dev = towel ? make_towel_dryer_in_programme(fx, 17.0F, heating_level::COMFORT)
                          : make_radiator_in_programme(fx, HeatingMode::PROG, 17.0F);
    queue_read_reply(fx, towel ? DRYER_MODE : RADIATOR_MODE, {towel ? DRYER_HEAT : RADIATOR_HEAT});
    queue_register_write_and_read_back(fx, COMFORT_REGISTER, 0xD2, 0xD2);

    const TargetOutcome outcome = set_target(fx, 21.0F);

    EXPECT_TRUE(outcome.sent) << "towel " << towel;
    EXPECT_FALSE(outcome.plan.temporary_override);
    EXPECT_EQ(written_registers(fx), std::vector<uint8_t>{COMFORT_REGISTER});
    EXPECT_FLOAT_EQ(dev->heating.setpoint_c, 21.0F);
  }
}

TEST(HeaterController, ADryerThatStoredABoostButAnswersNoTimedRunTakesTheTargetAsAnOverride) {
  HeaterControllerFixture fx;
  IoDevice *dev = make_towel_dryer_in_programme(fx, 17.0F, heating_level::ECO);
  dev->heating.temporary_state = static_cast<int16_t>(HeatingTemporaryState::BOOST);
  queue_read_reply(fx, DRYER_MODE, {DRYER_PROGRAMME});
  queue_read_reply(fx, DRYER_TIMED_RUN, {NO_TIMED_RUN});
  queue_register_write_and_read_back(fx, OVERRIDE_REGISTER, 0xD2, 0xD2);

  EXPECT_TRUE(set_target(fx, 21.0F).sent);

  EXPECT_EQ(written_registers(fx), std::vector<uint8_t>{OVERRIDE_REGISTER});
  EXPECT_EQ(dev->heating.value_of(HeatingFunction::TEMPORARY_STATE), static_cast<int16_t>(HeatingTemporaryState::NONE));
}

TEST(HeaterController, ADryerThatStoredTheProgrammeButAnswersABoostTakesTheTargetAsTheComfortSetpoint) {
  HeaterControllerFixture fx;
  make_towel_dryer_in_programme(fx, 17.0F, heating_level::COMFORT);
  queue_read_reply(fx, DRYER_MODE, {DRYER_PROGRAMME});
  queue_read_reply(fx, DRYER_TIMED_RUN, {TIMED_BOOST});
  queue_register_write_and_read_back(fx, COMFORT_REGISTER, 0xD2, 0xD2);

  EXPECT_TRUE(set_target(fx, 21.0F).sent);

  EXPECT_EQ(written_registers(fx), std::vector<uint8_t>{COMFORT_REGISTER});
}

TEST(HeaterController, ATargetOnAHeaterNeverReadInThisBootAsksTheHeaterAndFollowsItsAnswer) {
  for (const bool towel : {false, true}) {
    HeaterControllerFixture fx;
    IoDevice *dev = towel ? make_towel_dryer(fx) : make_heater(fx);
    fx.status_reply_level = heating_level::ECO;
    ASSERT_EQ(dev->heating.value_of(towel ? HeatingFunction::TOWEL_MODE : HeatingFunction::SET_MODE), -1);
    queue_read_reply(fx, towel ? DRYER_MODE : RADIATOR_MODE, {towel ? DRYER_PROGRAMME : RADIATOR_PROGRAMME});
    if (towel)
      queue_read_reply(fx, DRYER_TIMED_RUN, {NO_TIMED_RUN});
    queue_read_reply(fx, towel ? DRYER_IN_FORCE : RADIATOR_IN_FORCE, {0xAF, 0x00});
    queue_register_write_and_read_back(fx, OVERRIDE_REGISTER, 0xD2, 0xD2);

    EXPECT_TRUE(set_target(fx, 21.0F).sent) << "towel " << towel;

    EXPECT_EQ(written_registers(fx), std::vector<uint8_t>{OVERRIDE_REGISTER});
  }
}

TEST(HeaterController, AModeWriteWhoseAcknowledgementWasLostIsFollowedByATargetThatAsksTheHeater) {
  // The write reached the heater (it runs its programme now) but its acknowledgement did not arrive.
  HeaterControllerFixture fx;
  IoDevice *dev = make_radiator_in_programme(fx, HeatingMode::MANUAL, 17.0F);
  EXPECT_FALSE(fx.controller.execute_heating_plan(
      "ABC123", register_plan(HeatingFunction::SET_MODE, static_cast<float>(HeatingMode::AUTO)), nullptr));
  const size_t before = fx.radio.get_sent_data().size();
  queue_read_reply(fx, RADIATOR_MODE, {static_cast<uint8_t>(HeatingMode::AUTO)});
  queue_register_write_and_read_back(fx, OVERRIDE_REGISTER, 0xD2, 0xD2);

  EXPECT_TRUE(set_target(fx, 21.0F).sent);

  EXPECT_EQ(sent_registers_since(fx, before),
            (std::vector<uint8_t>{RADIATOR_MODE, OVERRIDE_REGISTER, OVERRIDE_REGISTER}));
  EXPECT_FLOAT_EQ(dev->heating.setpoint_c, 19.0F) << "the comfort setpoint is not written";
}

TEST(HeaterController, AModeSetWithTheActionIsFollowedByATargetThatAsksTheHeater) {
  // The heating_control action writes the mode and reads nothing back.
  HeaterControllerFixture fx;
  IoDevice *dev = make_radiator_in_programme(fx, HeatingMode::MANUAL, 17.0F);
  fx.queue_register_ack({0x0C, 0x61, 0x01, 0x00, static_cast<uint8_t>(HeatingMode::PROG)});
  ASSERT_TRUE(
      fx.controller.send_heating_command("ABC123", HeatingFunction::SET_MODE, static_cast<float>(HeatingMode::PROG)));
  ASSERT_EQ(fx.radio.get_sent_data().size(), 1u) << "the write alone";
  const size_t before = fx.radio.get_sent_data().size();
  queue_read_reply(fx, RADIATOR_MODE, {RADIATOR_PROGRAMME});
  queue_register_write_and_read_back(fx, OVERRIDE_REGISTER, 0xD2, 0xD2);

  EXPECT_TRUE(set_target(fx, 21.0F).sent);

  EXPECT_EQ(sent_registers_since(fx, before),
            (std::vector<uint8_t>{RADIATOR_MODE, OVERRIDE_REGISTER, OVERRIDE_REGISTER}));
  EXPECT_FLOAT_EQ(dev->heating.setpoint_c, 19.0F);
}

TEST(HeaterController, AModeAndATargetInOneCallReadTheTemperatureInForceOnceAndTheModeAgainBeforeTheTarget) {
  // The mode write's read-back, the display read of the temperature in force, then the target's own read of the
  // mode. The target's rule reads the temperature in force only while it is UNKNOWN: the display read answered
  // it, so it is not sent twice.
  HeaterControllerFixture fx;
  make_radiator_in_programme(fx, HeatingMode::MANUAL, NAN);
  const uint8_t auto_wire = static_cast<uint8_t>(HeatingMode::AUTO);
  fx.queue_register_ack({0x0C, 0x61, 0x01, RADIATOR_MODE, auto_wire});
  fx.queue_register_ack({0x0C, 0x60, 0x01, RADIATOR_MODE, auto_wire});
  queue_read_reply(fx, RADIATOR_IN_FORCE, {0xAF, 0x00});
  queue_read_reply(fx, RADIATOR_MODE, {auto_wire});
  queue_register_write_and_read_back(fx, OVERRIDE_REGISTER, 0xD2, 0xD2);

  ASSERT_TRUE(fx.controller.execute_heating_plan(
      "ABC123", register_plan(HeatingFunction::SET_MODE, static_cast<float>(HeatingMode::AUTO)), nullptr));
  EXPECT_TRUE(set_target(fx, 21.0F).sent);

  EXPECT_EQ(sent_registers(fx), (std::vector<uint8_t>{RADIATOR_MODE, RADIATOR_MODE, RADIATOR_IN_FORCE, RADIATOR_MODE,
                                                      OVERRIDE_REGISTER, OVERRIDE_REGISTER}));
}

TEST(HeaterController, AHeaterThatDoesNotAnswerTheModeReadGetsNothingWrittenAndNothingChanges) {
  for (const bool towel : {false, true}) {
    HeaterControllerFixture fx;
    IoDevice *dev = towel ? make_towel_dryer_in_programme(fx, 17.0F, heating_level::COMFORT)
                          : make_radiator_in_programme(fx, HeatingMode::PROG, 17.0F);
    const IoDevice before = *dev;

    const TargetOutcome outcome = set_target(fx, 21.0F);  // no reply

    EXPECT_FALSE(outcome.state_answered) << "towel " << towel;
    EXPECT_EQ(outcome.plan.refusal, HeatingRefusal::PROGRAMME_STATE_UNANSWERED);
    EXPECT_FALSE(outcome.sent);
    EXPECT_FALSE(sent_registers(fx).empty());
    for (const uint8_t reg : sent_registers(fx))
      EXPECT_EQ(reg, towel ? DRYER_MODE : RADIATOR_MODE) << "only the mode is asked, and nothing is written";
    EXPECT_TRUE(written_registers(fx).empty());
    EXPECT_EQ(dev->heating.mode, before.heating.mode) << "the stored reading stays as it was";
    EXPECT_EQ(dev->heating.towel_mode, before.heating.towel_mode);
    EXPECT_EQ(dev->temperature_in_force, before.temperature_in_force);
    EXPECT_FLOAT_EQ(dev->heating.in_force_temperature_c, before.heating.in_force_temperature_c);
  }
}

TEST(HeaterController, ADryerInItsProgrammeWhoseTimedRunIsNotAnsweredGetsNothingWritten) {
  HeaterControllerFixture fx;
  make_towel_dryer_in_programme(fx, 17.0F, heating_level::COMFORT);
  queue_read_reply(fx, DRYER_MODE, {DRYER_PROGRAMME});  // the timed-run read gets no reply

  const TargetOutcome outcome = set_target(fx, 21.0F);

  EXPECT_FALSE(outcome.state_answered);
  EXPECT_EQ(outcome.plan.refusal, HeatingRefusal::PROGRAMME_STATE_UNANSWERED)
      << "a boost or drying run could be on top: neither register is known to be right";
  const std::vector<uint8_t> reads = sent_registers(fx);
  ASSERT_GE(reads.size(), 2u);
  EXPECT_EQ(reads.front(), DRYER_MODE);
  for (size_t i = 1; i < reads.size(); i++)
    EXPECT_EQ(reads[i], DRYER_TIMED_RUN) << "nothing is asked after a silent read";
  EXPECT_TRUE(written_registers(fx).empty());
}

TEST(HeaterController, AModeReadAnsweredWithoutAValueGetsNothingWrittenAndIsNotAskedAgain) {
  HeaterControllerFixture fx;
  make_radiator_in_programme(fx, HeatingMode::PROG, 17.0F);
  fx.queue_register_ack({0x0C, 0x60, 0x01, RADIATOR_MODE});  // the mode register echoed without a value

  const TargetOutcome outcome = set_target(fx, 21.0F);

  EXPECT_FALSE(outcome.state_answered);
  EXPECT_EQ(outcome.plan.refusal, HeatingRefusal::PROGRAMME_STATE_UNANSWERED);
  EXPECT_EQ(sent_registers(fx), std::vector<uint8_t>{RADIATOR_MODE}) << "one answered read, not asked again";
}

TEST(HeaterController, AHeaterInTheProgrammeThatDoesNotAnswerTheTemperatureInForceGetsNothingWritten) {
  HeaterControllerFixture fx;
  IoDevice *dev = make_radiator_in_programme(fx, HeatingMode::PROG, NAN);
  queue_read_reply(fx, RADIATOR_MODE, {RADIATOR_PROGRAMME});  // the read of 0x012A gets no reply

  const TargetOutcome outcome = set_target(fx, 21.0F);

  EXPECT_TRUE(outcome.state_answered);
  EXPECT_EQ(outcome.plan.refusal, HeatingRefusal::IN_FORCE_UNKNOWN);
  EXPECT_FALSE(outcome.sent);
  EXPECT_TRUE(written_registers(fx).empty());
  EXPECT_EQ(dev->temperature_in_force, TemperatureInForceSupport::UNKNOWN);
}

TEST(HeaterController, AHeaterThatAnswersTheTemperatureInForceEmptyTakesTheTargetAsTheComfortSetpoint) {
  HeaterControllerFixture fx;
  IoDevice *dev = make_radiator_in_programme(fx, HeatingMode::MANUAL, NAN);
  queue_read_reply(fx, RADIATOR_MODE, {RADIATOR_PROGRAMME});
  queue_read_reply(fx, RADIATOR_IN_FORCE, {});  // answered, no value
  queue_register_write_and_read_back(fx, COMFORT_REGISTER, 0xD2, 0xD2);

  EXPECT_TRUE(set_target(fx, 21.0F).sent);

  EXPECT_EQ(dev->temperature_in_force, TemperatureInForceSupport::ABSENT);
  EXPECT_EQ(written_registers(fx), std::vector<uint8_t>{COMFORT_REGISTER});
}

TEST(HeaterController, ADryerTargetInAComfortPeriodAsksForTheLevelThenWritesTheComfortSetpoint) {
  HeaterControllerFixture fx;
  IoDevice *dev = make_towel_dryer_in_programme(fx, 19.0F, heating_level::COMFORT);
  queue_read_reply(fx, DRYER_MODE, {DRYER_PROGRAMME});
  queue_read_reply(fx, DRYER_TIMED_RUN, {NO_TIMED_RUN});
  queue_register_write_and_read_back(fx, COMFORT_REGISTER, 0xD2, 0xD2);

  const TargetOutcome outcome = set_target(fx, 21.0F);

  EXPECT_TRUE(outcome.sent);
  EXPECT_FALSE(outcome.plan.temporary_override);
  EXPECT_EQ(sent_registers(fx),
            (std::vector<uint8_t>{DRYER_MODE, DRYER_TIMED_RUN, COMFORT_REGISTER, COMFORT_REGISTER}));
  EXPECT_EQ(written_registers(fx), std::vector<uint8_t>{COMFORT_REGISTER}) << "the override is not written";
  ASSERT_EQ(fx.status_reads.size(), 1u) << "one status read, after the registers and before the write";
  EXPECT_EQ(fx.status_reads[0].max_tries, EXCHANGE_RETRY_COUNT);
  EXPECT_FLOAT_EQ(dev->heating.setpoint_c, 21.0F);
}

TEST(HeaterController, ADryerTargetInAnEcoPeriodAsksForTheLevelThenWritesTheOverride) {
  HeaterControllerFixture fx;
  make_towel_dryer_in_programme(fx, 16.5F, heating_level::ECO);
  queue_read_reply(fx, DRYER_MODE, {DRYER_PROGRAMME});
  queue_read_reply(fx, DRYER_TIMED_RUN, {NO_TIMED_RUN});
  queue_register_write_and_read_back(fx, OVERRIDE_REGISTER, 0xD2, 0xD2);

  const TargetOutcome outcome = set_target(fx, 21.0F);

  EXPECT_TRUE(outcome.sent);
  EXPECT_TRUE(outcome.plan.temporary_override);
  EXPECT_EQ(written_registers(fx), std::vector<uint8_t>{OVERRIDE_REGISTER});
  EXPECT_EQ(fx.status_reads.size(), 1u);
}

TEST(HeaterController, ADryerWhoseStoredLevelIsOldTakesTheLevelOfTheReadMadeForTheTarget) {
  // The poll stored eco; the period has changed since. The stored level is not used.
  HeaterControllerFixture fx;
  IoDevice *dev = make_towel_dryer_in_programme(fx, 16.5F, heating_level::ECO);
  fx.status_reply_level = heating_level::COMFORT;
  queue_read_reply(fx, DRYER_MODE, {DRYER_PROGRAMME});
  queue_read_reply(fx, DRYER_TIMED_RUN, {NO_TIMED_RUN});
  queue_register_write_and_read_back(fx, COMFORT_REGISTER, 0xD2, 0xD2);

  EXPECT_TRUE(set_target(fx, 21.0F).sent);

  EXPECT_EQ(written_registers(fx), std::vector<uint8_t>{COMFORT_REGISTER});
  EXPECT_EQ(dev->heating.level, heating_level::COMFORT);
}

TEST(HeaterController, ADryerThatDoesNotAnswerTheLevelReadGetsNothingWritten) {
  HeaterControllerFixture fx;
  IoDevice *dev = make_towel_dryer_in_programme(fx, 16.5F, heating_level::ECO);
  fx.status_reply_level = -1;  // the status read brings no level; the stored eco is old
  queue_read_reply(fx, DRYER_MODE, {DRYER_PROGRAMME});
  queue_read_reply(fx, DRYER_TIMED_RUN, {NO_TIMED_RUN});

  const TargetOutcome outcome = set_target(fx, 21.0F);

  EXPECT_TRUE(outcome.state_answered) << "the registers answered";
  EXPECT_EQ(outcome.plan.refusal, HeatingRefusal::PERIOD_UNKNOWN);
  EXPECT_FALSE(outcome.sent);
  EXPECT_TRUE(written_registers(fx).empty());
  EXPECT_EQ(fx.status_reads.size(), 1u);
  EXPECT_EQ(dev->heating.level, -1) << "the old level is not kept as if it were fresh";
}

TEST(HeaterController, ATargetThatNeedsNoPeriodMakesNoStatusRead) {
  // heat and a boost on top: the comfort setpoint without a period; a radiator: no such rule.
  {
    HeaterControllerFixture fx;
    make_towel_dryer_in_programme(fx, 17.0F, heating_level::ECO);
    queue_read_reply(fx, DRYER_MODE, {DRYER_HEAT});
    queue_register_write_and_read_back(fx, COMFORT_REGISTER, 0xD2, 0xD2);
    EXPECT_TRUE(set_target(fx, 21.0F).sent);
    EXPECT_TRUE(fx.status_reads.empty()) << "heat";
  }
  {
    HeaterControllerFixture fx;
    make_towel_dryer_in_programme(fx, 17.0F, heating_level::ECO);
    queue_read_reply(fx, DRYER_MODE, {DRYER_PROGRAMME});
    queue_read_reply(fx, DRYER_TIMED_RUN, {TIMED_BOOST});
    queue_register_write_and_read_back(fx, COMFORT_REGISTER, 0xD2, 0xD2);
    EXPECT_TRUE(set_target(fx, 21.0F).sent);
    EXPECT_TRUE(fx.status_reads.empty()) << "a boost on top";
  }
  {
    HeaterControllerFixture fx;
    make_radiator_in_programme(fx, HeatingMode::PROG, 17.0F);
    queue_read_reply(fx, RADIATOR_MODE, {RADIATOR_PROGRAMME});
    queue_register_write_and_read_back(fx, OVERRIDE_REGISTER, 0xD2, 0xD2);
    EXPECT_TRUE(set_target(fx, 21.0F).sent);
    EXPECT_TRUE(fx.status_reads.empty()) << "a radiator keeps the override in every period";
  }
}

TEST(HeaterController, AHeaterWithoutAProgrammeIsNotReadBeforeATarget) {
  HeaterControllerFixture fx;
  IoDevice *plain = fx.add_device_of_kind(DeviceType::ELECTRICAL_HEATER, 0);
  ASSERT_NE(plain, nullptr);

  EXPECT_FALSE(fx.controller.read_programme_state("ABC123"));
  EXPECT_TRUE(fx.radio.get_sent_data().empty()) << "a profile without a programme period has nothing to read";
}

TEST(HeaterController, ARefusedOverrideWritesNothingElseAndTheCardShowsWhatTheHeaterHolds) {
  HeaterControllerFixture fx;
  IoDevice *dev = make_radiator_in_programme(fx, HeatingMode::PROG, 17.0F);
  // The heater accepts the write but holds 0: no override runs.
  queue_register_write_and_read_back(fx, 0x1D, 0xC8, 0x00);

  EXPECT_TRUE(fx.controller.execute_heating_plan("ABC123", override_plan(20.0F), nullptr))
      << "the write was acknowledged";

  ASSERT_EQ(fx.radio.get_sent_data().size(), 2u) << "no fallback write of the comfort setpoint";
  for (const auto &frame : fx.radio.get_sent_data())
    EXPECT_NE(sent_register(frame), 0x03);
  EXPECT_FLOAT_EQ(dev->heating.setpoint_c, 19.0F);
  EXPECT_FLOAT_EQ(dev->heating.in_force_temperature_c, 17.0F) << "what the heater held stays shown until the poll";
  EXPECT_FLOAT_EQ(dev->heating.derogated_setpoint_c, 0.0F);
}

TEST(HeaterController, AnOverrideTheHeaterClampsShowsTheValueItHolds) {
  HeaterControllerFixture fx;
  IoDevice *dev = make_radiator_in_programme(fx, HeatingMode::PROG, 17.0F);
  queue_register_write_and_read_back(fx, 0x1D, 0xC8, 0xBE);  // asked 20.0, holds 19.0

  EXPECT_TRUE(fx.controller.execute_heating_plan("ABC123", override_plan(20.0F), nullptr));

  EXPECT_FLOAT_EQ(dev->heating.in_force_temperature_c, 19.0F);
  EXPECT_FLOAT_EQ(dev->heating.setpoint_c, 19.0F);
}

TEST(HeaterController, AnOverrideReadBackOfZeroLeavesTheTemperatureInForceAlone) {
  HeaterControllerFixture fx;
  IoDevice *dev = make_towel_dryer_in_programme(fx, 15.5F, heating_level::ECO);
  fx.queue_register_ack({0x0C, 0x61, 0x01, 0x1D, 0x00, 0x00});
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x1D, 0x00, 0x00});

  ASSERT_TRUE(
      fx.controller.execute_heating_plan("ABC123", register_plan(HeatingFunction::DEROGATED_SETPOINT, 0.0F), nullptr));

  EXPECT_FLOAT_EQ(dev->heating.in_force_temperature_c, 15.5F) << "a dryer that kept no override shows what it held";
}

TEST(HeaterController, AComfortReadOutsideAWriteDoesNotMoveTheTemperatureInForce) {
  HeaterControllerFixture fx;
  const IoDevice *dev = make_towel_dryer_in_programme(fx, 19.0F, heating_level::COMFORT);
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x03, 0xC8, 0x00});

  ASSERT_TRUE(
      fx.controller.read_heating_register("ABC123", HeatingFunction::SET_TEMPERATURE, nullptr, EXCHANGE_RETRY_COUNT));

  EXPECT_FLOAT_EQ(dev->heating.setpoint_c, 20.0F);
  EXPECT_FLOAT_EQ(dev->heating.in_force_temperature_c, 19.0F) << "only the poll's own read of 0x37 changes it";
}

TEST(HeaterController, APlannedRegisterWriteWithoutReadBackSendsOnlyTheWrite) {
  HeaterControllerFixture fx;
  make_heater(fx);
  fx.queue_register_ack({0x0C, 0x61, 0x01, 0x03, 0xCD, 0x00});
  HeatingWritePlan plan = register_plan(HeatingFunction::SET_TEMPERATURE, 20.5F);
  plan.read_back = false;

  EXPECT_TRUE(fx.controller.execute_heating_plan("ABC123", plan, nullptr));
  EXPECT_EQ(fx.radio.get_sent_data().size(), 1u);
}

TEST(HeaterController, AnUnacknowledgedPlannedWriteRunsNoHookAndSendsNoReadBack) {
  HeaterControllerFixture fx;
  make_heater(fx);
  int hook_calls = 0;

  EXPECT_FALSE(fx.controller.execute_heating_plan("ABC123", register_plan(HeatingFunction::SET_TEMPERATURE, 20.5F),
                                                  [&] { hook_calls++; }));

  EXPECT_EQ(hook_calls, 0);
  for (const auto &tx : fx.radio.get_sent_data())
    EXPECT_EQ(tx[10], 0x61) << "only write attempts, no read-back";
}

TEST(HeaterController, ARefusedPlanSendsNothing) {
  HeaterControllerFixture fx;
  make_heater(fx);
  HeatingWritePlan refused;
  refused.refusal = HeatingRefusal::NO_MATCH;
  int hook_calls = 0;

  EXPECT_FALSE(fx.controller.execute_heating_plan("ABC123", refused, [&] { hook_calls++; }));

  EXPECT_EQ(hook_calls, 0);
  EXPECT_TRUE(fx.radio.get_sent_data().empty());
  EXPECT_TRUE(fx.executes.empty());
}

TEST(HeaterController, APlannedLevelWriteIsTheLevelExecuteAndIsNotReadBack) {
  HeaterControllerFixture fx;
  ASSERT_NE(fx.add_device_of_kind(DeviceType::ELECTRICAL_HEATER, 0), nullptr);
  HeatingWritePlan plan;
  plan.kind = HeatingPlanKind::LEVEL;
  plan.level = heating_level::ECO;
  int hook_calls = 0;

  EXPECT_TRUE(fx.controller.execute_heating_plan("ABC123", plan, [&] { hook_calls++; }));

  EXPECT_EQ(hook_calls, 1);
  ASSERT_EQ(fx.executes.size(), 1u);
  EXPECT_EQ(fx.executes[0].action, "heating level");
  ASSERT_TRUE(fx.executes[0].built);
  IoFrame expected;
  const IoDevice *dev = fx.registry.get("ABC123");
  ASSERT_TRUE(create_execute_heating_level(expected, fx.node_id, dev->node_id, dev->low_power, heating_level::ECO));
  uint8_t expected_raw[64];
  uint8_t got_raw[64];
  const uint8_t expected_len = serialize(expected, expected_raw, sizeof(expected_raw));
  const uint8_t got_len = serialize(fx.executes[0].request, got_raw, sizeof(got_raw));
  EXPECT_EQ(std::vector<uint8_t>(got_raw, got_raw + got_len),
            std::vector<uint8_t>(expected_raw, expected_raw + expected_len));
  EXPECT_TRUE(fx.radio.get_sent_data().empty()) << "no register read-back after a level write";
}

TEST(HeaterController, APlannedTemperatureWriteCarriesItsMainParameterAndFunctionalParameter) {
  HeaterControllerFixture fx;
  fx.add_device_of_kind(DeviceType::HEAT_PUMP);
  HeatingWritePlan plan;
  plan.kind = HeatingPlanKind::TEMPERATURE;
  plan.main_parameter = 0xD80F;
  plan.fp = HeatingFp{HeatingFpKind::FP16, heating_fp16::ON};

  EXPECT_TRUE(fx.controller.execute_heating_plan("ABC123", plan, nullptr));

  ASSERT_EQ(fx.executes.size(), 1u);
  EXPECT_EQ(fx.executes[0].action, "heating temperature");
  ASSERT_TRUE(fx.executes[0].built);
  IoFrame expected;
  const IoDevice *dev = fx.registry.get("ABC123");
  ASSERT_TRUE(
      create_execute_heating_with_fp16(expected, fx.node_id, dev->node_id, dev->low_power, 0xD80F, heating_fp16::ON));
  uint8_t expected_raw[64];
  uint8_t got_raw[64];
  const uint8_t expected_len = serialize(expected, expected_raw, sizeof(expected_raw));
  const uint8_t got_len = serialize(fx.executes[0].request, got_raw, sizeof(got_raw));
  EXPECT_EQ(std::vector<uint8_t>(got_raw, got_raw + got_len),
            std::vector<uint8_t>(expected_raw, expected_raw + expected_len));
  EXPECT_TRUE(fx.radio.get_sent_data().empty());
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
    EXPECT_EQ(tx[10], PRIVATE_REGISTER_DIRECTION_SET) << "only the write is tried";
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

// === Regular polls ===

TEST(HeaterController, PollsIsTrueForARegisterHeaterAWaterHeaterAndAStatusReadHeaterOnly) {
  HeaterControllerFixture radiator;
  EXPECT_TRUE(HeaterController::polls(*make_heater(radiator)));
  HeaterControllerFixture water;
  EXPECT_TRUE(HeaterController::polls(*make_water_heater(water)));
  for (const DeviceType type : {DeviceType::HEATING_TEMPERATURE_INTERFACE, DeviceType::HEAT_PUMP}) {
    HeaterControllerFixture fx;
    EXPECT_TRUE(HeaterController::polls(*fx.add_device_of_kind(type))) << static_cast<unsigned>(type);
  }
  HeaterControllerFixture level;
  EXPECT_TRUE(HeaterController::polls(*level.add_device_of_kind(DeviceType::ELECTRICAL_HEATER, 0)));
  HeaterControllerFixture cover;
  EXPECT_FALSE(HeaterController::polls(*make_cover(cover)));
}

TEST(HeaterController, HeaterPollReadsTheDetectionSettingEveryPollAndTheMaximumAndModelCheckOnce) {
  HeaterControllerFixture fx;
  auto *dev = make_heater(fx);
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x15, 0x1B, 0x08});  // 20.75 C
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x03, 0xC3, 0x00});  // 19.5 C
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x00, 0x01});        // manual
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x17, 0x02});        // heating
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x0E, 0x01});        // window open
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x06, 0x01});        // detection on
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x02, 0x18, 0x01});  // 28.0 C
  // The one-off model check: a model name (synthetic), and the towel-dryer mode register answered empty.
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x19, 'R', 'A', 'D', 'I', 'A', 'T'});
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x36});

  EXPECT_TRUE(fx.controller.poll("ABC123")) << "a heater is polled over its registers, not with 0x03";

  ASSERT_EQ(fx.radio.get_sent_data().size(), 9u);
  const uint8_t expected_registers[] = {0x15, 0x03, 0x00, 0x17, 0x0E, 0x06, 0x02, 0x19, 0x36};
  for (size_t i = 0; i < 9; i++)
    EXPECT_EQ(sent_register(fx.radio.get_sent_data()[i]), expected_registers[i]) << "read " << i;
  EXPECT_TRUE(dev->model_checked) << "both model-check reads were answered";
  EXPECT_FLOAT_EQ(dev->heating.room_temperature_c, 20.75F);
  EXPECT_FLOAT_EQ(dev->heating.setpoint_c, 19.5F);
  EXPECT_EQ(dev->heating.mode, 1);
  EXPECT_EQ(dev->heating.regulator, 2);
  EXPECT_EQ(dev->heating.window_open, 1);
  EXPECT_FLOAT_EQ(dev->heating.max_setpoint_c, 28.0F);
  EXPECT_EQ(dev->heating.open_window_detection, 1);
  EXPECT_FALSE(fx.poll_policy.is_tracking_active("ABC123", esphome::millis())) << "a good poll closes its retry window";

  // The maximum and the model check run once; the detection setting is read again, and a value the
  // vendor app changed in between reaches the readings (the switch state).
  const size_t sent_before = fx.radio.get_sent_data().size();
  queue_radiator_poll(fx, 0x59, 0x06, 0x01, 0x00, 0x00);
  EXPECT_TRUE(fx.controller.poll("ABC123"));
  ASSERT_EQ(fx.radio.get_sent_data().size() - sent_before, 6u);
  for (size_t i = sent_before; i < fx.radio.get_sent_data().size(); i++)
    EXPECT_NE(sent_register(fx.radio.get_sent_data()[i]), 0x02) << "the maximum is not read again";
  EXPECT_EQ(sent_register(fx.radio.get_sent_data().back()), 0x06);
  EXPECT_FLOAT_EQ(dev->heating.room_temperature_c, 16.25F);
  EXPECT_EQ(dev->heating.window_open, 0);
  EXPECT_FLOAT_EQ(dev->heating.max_setpoint_c, 28.0F);
  EXPECT_EQ(dev->heating.open_window_detection, 0) << "a change from the vendor app shows on the next poll";
}

TEST(HeaterController, AMissedDetectionReadKeepsTheSwitchAndDoesNotFailThePoll) {
  HeaterControllerFixture fx;
  auto *dev = make_heater(fx);
  // Boot registers and model check done, so the poll is the five registers and the detection setting.
  dev->model_checked = true;
  dev->heating.max_setpoint_c = 28.0F;
  dev->heating.open_window_detection = 1;
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x15, 0x1B, 0x08});  // 20.75 C
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x03, 0xC3, 0x00});
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x00, 0x01});
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x17, 0x02});
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x0E, 0x00});
  // Nothing queued for 0x0106: the detection setting never answers.

  EXPECT_TRUE(fx.controller.poll("ABC123")) << "a missed setting costs a read, not the poll";

  EXPECT_EQ(sent_register(fx.radio.get_sent_data().back()), 0x06) << "the setting was asked for";
  EXPECT_EQ(dev->heating.open_window_detection, 1) << "and the switch keeps its state";
  EXPECT_FLOAT_EQ(dev->heating.room_temperature_c, 20.75F);
  EXPECT_TRUE(fx.backoffs.empty());
  EXPECT_EQ(fx.poll_policy.get_status_poll_failures("ABC123"), 0u);
}

/// Queue the replies of a towel dryer's five core registers (room, setpoint, mode, temporary state, rate).
void queue_towel_dryer_poll(HeaterControllerFixture &fx) {
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x15, 0x1B, 0x08});
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x03, 0xC3, 0x00});
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x36, 0x01});
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x3E, 0x00});
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x26, 0x00});
}

/// @return Whether a sent frame of the poll read the central-heating mode (register 0x0153).
bool read_central_heating_mode(const std::vector<std::vector<uint8_t>> &sent) {
  return std::any_of(sent.begin(), sent.end(), [](const auto &tx) { return sent_register(tx) == 0x53; });
}

TEST(HeaterController, ADryerWithoutTheCentralHeatingSensorNeverReadsRegister0x53) {
  HeaterControllerFixture fx;
  IoDevice *dev = make_towel_dryer(fx);
  queue_towel_dryer_poll(fx);

  EXPECT_TRUE(fx.controller.poll("ABC123"));
  EXPECT_FALSE(read_central_heating_mode(fx.radio.get_sent_data()));
  EXPECT_EQ(dev->central_heating_mode_read, CentralHeatingModeRead::NOT_ASKED);
  EXPECT_FALSE(dev->heating.has(HeatingFunction::CENTRAL_HEATING_MODE));
}

TEST(HeaterController, ADryerWithTheCentralHeatingSensorReadsItAtEveryPoll) {
  HeaterControllerFixture fx;
  IoDevice *dev = make_towel_dryer(fx);
  fx.controller.enable_central_heating_mode_read("ABC123");
  queue_towel_dryer_poll(fx);
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x53, 0x01});

  EXPECT_TRUE(fx.controller.poll("ABC123"));
  ASSERT_GE(fx.radio.get_sent_data().size(), 6u);
  EXPECT_EQ(sent_register(fx.radio.get_sent_data()[5]), 0x53) << "right after the five core registers";
  EXPECT_EQ(dev->heating.central_heating_mode, 1);

  const size_t sent_before = fx.radio.get_sent_data().size();
  queue_towel_dryer_poll(fx);
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x53, 0x00});
  EXPECT_TRUE(fx.controller.poll("ABC123"));
  EXPECT_EQ(sent_register(fx.radio.get_sent_data()[sent_before + 5]), 0x53) << "read again, not once";
  EXPECT_EQ(dev->heating.central_heating_mode, 0);
  EXPECT_EQ(dev->central_heating_mode_read, CentralHeatingModeRead::ASKED);
}

TEST(HeaterController, AnAnswerWithoutAValueStopsTheCentralHeatingReadAndClearsTheReading) {
  HeaterControllerFixture fx;
  IoDevice *dev = make_towel_dryer(fx);
  fx.controller.enable_central_heating_mode_read("ABC123");
  dev->heating.central_heating_mode = 1;  // an earlier answer
  queue_towel_dryer_poll(fx);
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x53});  // an electric unit: no value

  EXPECT_TRUE(fx.controller.poll("ABC123")) << "a missing register is not a poll failure";
  EXPECT_EQ(dev->central_heating_mode_read, CentralHeatingModeRead::ABSENT);
  EXPECT_FALSE(dev->heating.has(HeatingFunction::CENTRAL_HEATING_MODE));

  const size_t sent_before = fx.radio.get_sent_data().size();
  queue_towel_dryer_poll(fx);
  EXPECT_TRUE(fx.controller.poll("ABC123"));
  const auto &sent = fx.radio.get_sent_data();
  EXPECT_FALSE(read_central_heating_mode({sent.begin() + static_cast<std::ptrdiff_t>(sent_before), sent.end()}))
      << "not asked again until the next boot";
}

TEST(HeaterController, AnUnknownCentralHeatingValueIsTreatedAsNoValue) {
  HeaterControllerFixture fx;
  IoDevice *dev = make_towel_dryer(fx);
  fx.controller.enable_central_heating_mode_read("ABC123");
  queue_towel_dryer_poll(fx);
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x53, 0x02});

  EXPECT_TRUE(fx.controller.poll("ABC123"));
  EXPECT_EQ(dev->central_heating_mode_read, CentralHeatingModeRead::ABSENT);
  EXPECT_FALSE(dev->heating.has(HeatingFunction::CENTRAL_HEATING_MODE));
}

TEST(HeaterController, AMissedCentralHeatingReadChangesNothing) {
  HeaterControllerFixture fx;
  IoDevice *dev = make_towel_dryer(fx);
  fx.controller.enable_central_heating_mode_read("ABC123");
  dev->heating.central_heating_mode = 1;
  queue_towel_dryer_poll(fx);  // no reply to the read of 0x53

  EXPECT_TRUE(fx.controller.poll("ABC123"));
  EXPECT_EQ(dev->central_heating_mode_read, CentralHeatingModeRead::ASKED) << "asked again at the next poll";
  EXPECT_EQ(dev->heating.central_heating_mode, 1);
}

TEST(HeaterController, TheCentralHeatingReadIsRefusedOnAKindWithoutTheCapability) {
  HeaterControllerFixture fx;
  IoDevice *radiator = make_heater(fx);
  fx.controller.enable_central_heating_mode_read("ABC123");
  EXPECT_EQ(radiator->central_heating_mode_read, CentralHeatingModeRead::NOT_ASKED);
}

TEST(HeaterController, HeaterPollStopsAtTheFirstFailureAndBacksOff) {
  HeaterControllerFixture fx;
  auto *dev = make_heater(fx);
  // Nothing queued: the first register never answers.

  EXPECT_FALSE(fx.controller.poll("ABC123"));

  ASSERT_FALSE(fx.radio.get_sent_data().empty());
  for (const auto &tx : fx.radio.get_sent_data())
    EXPECT_EQ(sent_register(tx), 0x15) << "only the first register is asked for (retries of it)";
  EXPECT_LE(fx.radio.get_sent_data().size(), SCHEDULED_POLL_MAX_TRIES)
      << "a background poll is held to the scheduled-poll band; the ladder is its retry";
  EXPECT_EQ(fx.poll_policy.get_status_poll_failures("ABC123"), 1u) << "the failure moves onto the retry ladder";
  EXPECT_NE(fx.poll_policy.get_next_update("ABC123"), 0u) << "and a retry is scheduled";
  EXPECT_TRUE(std::isnan(dev->heating.setpoint_c));
  ASSERT_EQ(fx.backoffs.size(), 1u);
  EXPECT_EQ(fx.backoffs[0].first, "ABC123");
  EXPECT_FALSE(fx.backoffs[0].second) << "no challenge was seen, so the failure is not auth-like";
}

TEST(HeaterController, ASucceededRegisterHeaterPollClearsTheLadderAndArmsTheNextPollAtTheConfiguredInterval) {
  HeaterControllerFixture fx;
  const IoDevice *dev = make_heater(fx);
  fx.poll_policy.set_interval("ABC123", 120000);
  queue_radiator_poll(fx, 0x1B, 0x08, 0x02, 0x01);

  EXPECT_TRUE(fx.controller.poll("ABC123"));

  EXPECT_TRUE(fx.backoffs.empty());
  ASSERT_FALSE(fx.timers.empty());
  EXPECT_EQ(fx.timers.back().id, decisions::heating_poll_timer_id(dev->node_id));
  EXPECT_EQ(fx.timers.back().delay_ms, 120000u) << "the configured interval, not the default";
}

TEST(HeaterController, ARegisterHeaterPollBeforeTheHubIsInitialisedSendsNothingAndStillArmsTheNextPoll) {
  HeaterControllerFixture fx;
  const IoDevice *dev = make_heater(fx);
  fx.initialized = false;

  EXPECT_FALSE(fx.controller.poll("ABC123"));

  ASSERT_EQ(fx.timers.size(), 1u);
  EXPECT_EQ(fx.timers[0].id, decisions::heating_poll_timer_id(dev->node_id));
  EXPECT_EQ(fx.timers[0].delay_ms, HEATING_POLL_DEFAULT_INTERVAL_MS);
  EXPECT_TRUE(fx.radio.get_sent_data().empty());
  EXPECT_TRUE(fx.backoffs.empty());
}

TEST(HeaterController, ARegisterHeaterPollOfAnUnregisteredDeviceArmsNoTimer) {
  HeaterControllerFixture fx;
  make_heater(fx);

  EXPECT_FALSE(fx.controller.poll("DEF456"));
  EXPECT_TRUE(fx.timers.empty());
}

TEST(HeaterController, TheModelCheckRunsABoundedNumberOfTimesAndOnlyOnAKindWithModels) {
  HeaterControllerFixture fx;
  IoDevice *dev = make_heater(fx);
  // Nothing answers: each run asks for the name and the mode register once.
  for (uint8_t run = 0; run < decisions::HEATER_MODEL_CHECK_MAX_RUNS + 1; run++)
    fx.controller.check_heater_model("ABC123", 1);
  EXPECT_EQ(fx.radio.get_sent_data().size(), 2u * decisions::HEATER_MODEL_CHECK_MAX_RUNS);
  EXPECT_FALSE(dev->model_checked);

  HeaterControllerFixture plain;
  IoDevice *plain_dev = make_heater(plain);
  test::set_kind(*plain_dev, DeviceType::ELECTRICAL_HEATER, 0);
  plain.controller.check_heater_model("ABC123", 1);
  EXPECT_TRUE(plain.radio.get_sent_data().empty()) << "52/0 has no model rows";
}

TEST(HeaterController, ALevelAndATemperatureHeaterArePolledWithTheGenericStatusReadAndNoRegisterFrame) {
  for (const auto &kind :
       {std::make_pair(DeviceType::ELECTRICAL_HEATER, 0), std::make_pair(DeviceType::HEATING_TEMPERATURE_INTERFACE, 0),
        std::make_pair(DeviceType::HEAT_PUMP, 0)}) {
    HeaterControllerFixture fx;
    const IoDevice *dev = fx.add_device_of_kind(kind.first, static_cast<uint8_t>(kind.second));
    fx.poll_policy.set_interval("ABC123", 90000);

    EXPECT_TRUE(fx.controller.poll("ABC123"));

    EXPECT_TRUE(fx.radio.get_sent_data().empty())
        << "no CMD_WRITE_PRIVATE frame for a heater with the standard EXECUTE";
    ASSERT_EQ(fx.status_reads.size(), 1u);
    IoFrame expected;
    ASSERT_TRUE(create_get_status(expected, fx.node_id, dev->node_id, dev->low_power));
    EXPECT_EQ(fx.status_reads[0].request.cmd, expected.cmd);
    EXPECT_EQ(fx.status_reads[0].request.data_len, expected.data_len) << "the generic request, never the tilt one";
    EXPECT_EQ(fx.status_reads[0].retry_after_fail_ms, STATUS_RETRY_AFTER_FAIL_MS);
    EXPECT_EQ(fx.timers.back().delay_ms, 90000u);
  }
}

TEST(HeaterController, AFailedStatusReadPollLeavesTheLadderToTheStatusRead) {
  HeaterControllerFixture fx;
  fx.add_device_of_kind(DeviceType::ELECTRICAL_HEATER, 0);
  fx.status_read_result = false;

  EXPECT_FALSE(fx.controller.poll("ABC123"));

  EXPECT_TRUE(fx.backoffs.empty()) << "the status read runs its own failure ladder";
  ASSERT_EQ(fx.timers.size(), 1u) << "the cadence continues";
}

TEST(HeaterController, ADeviceThatIsNoHeaterIsNotPolledAndArmsNoTimer) {
  HeaterControllerFixture fx;
  make_cover(fx);

  EXPECT_FALSE(fx.controller.poll("ABC123"));

  EXPECT_TRUE(fx.radio.get_sent_data().empty());
  EXPECT_TRUE(fx.status_reads.empty());
  EXPECT_TRUE(fx.timers.empty());
}

TEST(HeaterController, AWaterHeaterPollReadsTheStatusThenTheRegistersThenTheBrandOnce) {
  HeaterControllerFixture fx;
  make_water_heater(fx);
  const auto queue_poll = [&fx](bool with_brand) {
    const uint8_t registers[] = {DHW_REG_WATER_TEMPERATURE, DHW_REG_MODE,          DHW_REG_HEATING,
                                 DHW_REG_BOOST_DURATION,    DHW_REG_AWAY_DURATION, DHW_REG_ENERGY};
    const std::vector<std::vector<uint8_t>> values = {{0x02, 0x45, 0x00, 0x5C, 0x02, 0x78},
                                                      {0x01},
                                                      {0x01, 0xFF, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00},
                                                      {0x01},
                                                      {0x00},
                                                      {0x00, 0x1B, 0xF3, 0x42}};
    for (size_t i = 0; i < std::size(registers); i++) {
      std::vector<uint8_t> payload = {0x0C, 0x60, 0x00, registers[i]};
      payload.insert(payload.end(), values[i].begin(), values[i].end());
      fx.queue_register_ack(payload);
    }
    if (with_brand)
      fx.queue_register_ack({0x0C, 0x60, 0x00, DHW_REG_BRAND, 0x41});
  };
  queue_poll(true);

  EXPECT_TRUE(fx.controller.poll("ABC123"));

  ASSERT_EQ(fx.status_reads.size(), 1u) << "the status read comes first";
  const auto &sent = fx.radio.get_sent_data();
  ASSERT_EQ(sent.size(), 7u);
  const uint8_t expected[] = {DHW_REG_WATER_TEMPERATURE, DHW_REG_MODE,   DHW_REG_HEATING, DHW_REG_BOOST_DURATION,
                              DHW_REG_AWAY_DURATION,     DHW_REG_ENERGY, DHW_REG_BRAND};
  for (size_t i = 0; i < std::size(expected); i++)
    EXPECT_EQ(sent_register(sent[i]), expected[i]) << i;
  EXPECT_EQ(fx.registry.get("ABC123")->dhw.brand, 'A');
  EXPECT_FALSE(fx.poll_policy.is_tracking_active("ABC123", esphome::millis())) << "a good poll closes its window";
  EXPECT_EQ(fx.timers.back().delay_ms, HEATING_POLL_DEFAULT_INTERVAL_MS);

  // The brand is read once: the next poll asks for the six registers only.
  const size_t before = sent.size();
  queue_poll(false);
  EXPECT_TRUE(fx.controller.poll("ABC123"));
  EXPECT_EQ(fx.radio.get_sent_data().size() - before, 6u);
}

TEST(HeaterController, AnUnansweredWaterHeaterStatusReadFailsThePollAndMovesItOntoTheLadder) {
  HeaterControllerFixture fx;
  make_water_heater(fx);
  fx.status_read_result = false;

  EXPECT_FALSE(fx.controller.poll("ABC123"));

  EXPECT_TRUE(fx.radio.get_sent_data().empty()) << "no register is read after a failed status read";
  ASSERT_EQ(fx.backoffs.size(), 1u);
  EXPECT_EQ(fx.poll_policy.get_status_poll_failures("ABC123"), 1u);
  EXPECT_EQ(fx.timers.size(), 1u);
}

TEST(HeaterController, AWaterHeaterPollBeforeTheHubIsInitialisedStillArmsTheNextPoll) {
  HeaterControllerFixture fx;
  const IoDevice *dev = make_water_heater(fx);
  fx.initialized = false;

  EXPECT_FALSE(fx.controller.poll("ABC123"));

  ASSERT_EQ(fx.timers.size(), 1u);
  EXPECT_EQ(fx.timers[0].id, decisions::heating_poll_timer_id(dev->node_id));
  EXPECT_EQ(fx.timers[0].delay_ms, HEATING_POLL_DEFAULT_INTERVAL_MS);
  EXPECT_TRUE(fx.radio.get_sent_data().empty());
}

TEST(HeaterController, AWaterHeaterPollOfAnUnregisteredDeviceArmsNoTimer) {
  HeaterControllerFixture fx;
  make_water_heater(fx);

  EXPECT_FALSE(fx.controller.poll("DEF456"));
  EXPECT_TRUE(fx.timers.empty());
}

// === Decoding a status answer ===

TEST(HeaterStatusDecoding, ALevelHeaterKeepsTheLevelTheReplyCarries) {
  IoDevice dev;
  test::set_kind(dev, DeviceType::ELECTRICAL_HEATER, 0);

  apply_heating_main_parameter("ABC123", dev, heating_level_main_parameter(heating_level::ECO));

  EXPECT_EQ(dev.heating.level, heating_level::ECO);
  EXPECT_EQ(dev.heating.main_parameter, heating_level_main_parameter(heating_level::ECO));
}

TEST(HeaterStatusDecoding, ATemperatureHeaterKeepsTheSetpointAndClearsAStaleAlias) {
  IoDevice dev;
  test::set_kind(dev, DeviceType::HEAT_PUMP, 0);
  dev.heating.level = 3;
  uint16_t mp = 0;
  ASSERT_TRUE(encode_temperature_main_parameter(21.0F, dev.temperature_range_min_c, dev.temperature_range_max_c, mp));

  apply_heating_main_parameter("ABC123", dev, mp);

  EXPECT_NEAR(dev.heating.setpoint_c, 21.0F, 0.1F);
  EXPECT_EQ(dev.heating.level, -1) << "a plain temperature clears a stale alias";
  EXPECT_EQ(dev.heating.main_parameter, mp);
}

TEST(HeaterStatusDecoding, ARegisterHeaterAndADeviceWithoutAProfileAreLeftAlone) {
  IoDevice radiator;
  test::set_kind(radiator, DeviceType::ELECTRICAL_HEATER, 1);
  apply_heating_main_parameter("ABC123", radiator, 0x1234);
  EXPECT_EQ(radiator.heating.main_parameter, -1);

  IoDevice cover;
  test::set_kind(cover, DeviceType::ROLLER_SHUTTER, 0);
  apply_heating_main_parameter("ABC123", cover, 0x1234);
  EXPECT_EQ(cover.heating.main_parameter, -1);
}

TEST(HeaterStatusDecoding, AWaterHeaterStatusReplyGivesTheTargetCapabilitiesAndBoostAndAway) {
  IoDevice dev;
  test::set_kind(dev, DeviceType::DOMESTIC_HOT_WATER, 0);
  IoFrame frame{};
  const uint8_t payload[] = {0x05,
                             0x00,
                             0xA1,
                             0xE8,
                             DHW_STATUS_FPI1,
                             DHW_STATUS_FPI2,
                             0x00,
                             0x00,
                             static_cast<uint8_t>(DHW_CAPABILITY_BOOST >> 8U),
                             static_cast<uint8_t>(DHW_CAPABILITY_BOOST),
                             static_cast<uint8_t>(DHW_OPERATING_MODE_BOOST_ON >> 8U),
                             static_cast<uint8_t>(DHW_OPERATING_MODE_BOOST_ON)};
  set_cmd(frame, CMD_PRIVATE_RESP, payload, sizeof(payload));

  apply_dhw_status_reply("ABC123", dev, frame, true);

  EXPECT_FLOAT_EQ(dev.dhw.target_c, 54.0F);
  EXPECT_EQ(dev.dhw.capabilities, DHW_CAPABILITY_BOOST);
  EXPECT_EQ(dev.dhw.boost, 1);
  EXPECT_EQ(dev.dhw.away, 0);
}

TEST(HeaterStatusDecoding, TheAckOfAnExecuteIsNotDecodedAndANonTemperatureLeavesTheTargetUnknown) {
  IoDevice dev;
  test::set_kind(dev, DeviceType::DOMESTIC_HOT_WATER, 0);
  dev.dhw.target_c = 54.0F;
  IoFrame frame{};
  const uint8_t payload[] = {0x2D, 0x00, 0xD8, 0x12, 0x00, 0x00};
  set_cmd(frame, CMD_PRIVATE_RESP, payload, sizeof(payload));

  apply_dhw_status_reply("ABC123", dev, frame, false);
  EXPECT_FLOAT_EQ(dev.dhw.target_c, 54.0F) << "an EXECUTE ack echoes the request and carries no report";

  apply_dhw_status_reply("ABC123", dev, frame, true);
  EXPECT_TRUE(std::isnan(dev.dhw.target_c)) << "an alias is not a target temperature";
}

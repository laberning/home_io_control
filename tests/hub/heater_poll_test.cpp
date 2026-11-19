/// @file heater_poll_test.cpp
/// @brief HeaterController: regular polls, the opt-in occupancy and energy reads, the run timers and the pinned
/// register sequence of one poll (HeaterControllerFixture).

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

#include <array>
#include <cmath>
#include <functional>
#include <vector>

using namespace esphome::home_io_control;
using namespace heater_controller_test;

namespace {

/// The minutes left of the active run a device shows, NaN where it shows no value (the `remaining_time:` entity).
float remaining_minutes(const IoDevice &dev) { return reading_value(dev, DeviceReading::REMAINING_TIME).value_or(NAN); }

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

}  // namespace

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

TEST(HeaterController, HeaterPollStoresEveryRegisterItReadsAndFollowsAChangeOfTheDetectionSetting) {
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

  EXPECT_TRUE(dev->model_checked) << "both model-check reads were answered";
  EXPECT_FLOAT_EQ(test::number_read(*dev, DeviceReading::ROOM_TEMPERATURE), 20.75F);
  EXPECT_FLOAT_EQ(test::number_read(*dev, DeviceReading::SETPOINT), 19.5F);
  EXPECT_EQ(test::wire_read(*dev, DeviceReading::MODE), 1);
  EXPECT_EQ(test::wire_read(*dev, DeviceReading::REGULATOR_STATE), 2);
  EXPECT_EQ(test::wire_read(*dev, DeviceReading::WINDOW_OPEN), 1);
  EXPECT_FLOAT_EQ(test::number_read(*dev, DeviceReading::MAX_SETPOINT), 28.0F);
  EXPECT_EQ(test::wire_read(*dev, DeviceReading::OPEN_WINDOW_DETECTION), 1);
  EXPECT_FALSE(fx.poll_policy.is_tracking_active("ABC123", esphome::millis())) << "a good poll closes its retry window";

  // The second poll is answered without the maximum and the model check, which run once (HeaterPollPin pins
  // the order); the detection setting is read again, and a value the vendor app changed in between reaches
  // the readings (the switch state).
  queue_radiator_poll(fx, 0x59, 0x06, 0x01, 0x00, 0x00);
  EXPECT_TRUE(fx.controller.poll("ABC123"));
  EXPECT_FLOAT_EQ(test::number_read(*dev, DeviceReading::ROOM_TEMPERATURE), 16.25F);
  EXPECT_EQ(test::wire_read(*dev, DeviceReading::WINDOW_OPEN), 0);
  EXPECT_FLOAT_EQ(test::number_read(*dev, DeviceReading::MAX_SETPOINT), 28.0F);
  EXPECT_EQ(test::wire_read(*dev, DeviceReading::OPEN_WINDOW_DETECTION), 0)
      << "a change from the vendor app shows on the next poll";
}

TEST(HeaterController, AMissedDetectionReadKeepsTheSwitchAndDoesNotFailThePoll) {
  HeaterControllerFixture fx;
  auto *dev = make_heater(fx);
  // Boot registers and model check done, so the poll is the five registers and the detection setting.
  dev->model_checked = true;
  test::store_reading(*dev, DeviceReading::MAX_SETPOINT, 28.0F);
  test::store_reading(*dev, DeviceReading::OPEN_WINDOW_DETECTION, 1);
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x15, 0x1B, 0x08});  // 20.75 C
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x03, 0xC3, 0x00});
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x00, 0x01});
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x17, 0x02});
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x0E, 0x00});
  // Nothing queued for 0x0106: the detection setting never answers.

  EXPECT_TRUE(fx.controller.poll("ABC123")) << "a missed setting costs a read, not the poll";

  EXPECT_EQ(sent_register(fx.radio.get_sent_data().back()), 0x06) << "the setting was asked for";
  EXPECT_EQ(test::wire_read(*dev, DeviceReading::OPEN_WINDOW_DETECTION), 1) << "and the switch keeps its state";
  EXPECT_FLOAT_EQ(test::number_read(*dev, DeviceReading::ROOM_TEMPERATURE), 20.75F);
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
  EXPECT_EQ(test::read_state(*dev, DeviceRegister::CENTRAL_HEATING_MODE), test::ReadState::NOT_ASKED);
  EXPECT_FALSE(test::has_reading(*dev, DeviceRegister::CENTRAL_HEATING_MODE));
}

TEST(HeaterController, ADryerWithTheCentralHeatingSensorReadsItAtEveryPoll) {
  HeaterControllerFixture fx;
  IoDevice *dev = make_towel_dryer(fx);
  fx.controller.enable_device_reading("ABC123", DeviceReading::CENTRAL_HEATING_MODE);
  queue_towel_dryer_poll(fx);
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x53, 0x01});

  EXPECT_TRUE(fx.controller.poll("ABC123"));
  ASSERT_GE(fx.radio.get_sent_data().size(), 6u);
  EXPECT_EQ(sent_register(fx.radio.get_sent_data()[5]), 0x53) << "right after the five core registers";
  EXPECT_EQ(test::wire_read(*dev, DeviceReading::CENTRAL_HEATING_MODE), 1);

  const size_t sent_before = fx.radio.get_sent_data().size();
  queue_towel_dryer_poll(fx);
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x53, 0x00});
  EXPECT_TRUE(fx.controller.poll("ABC123"));
  EXPECT_EQ(sent_register(fx.radio.get_sent_data()[sent_before + 5]), 0x53) << "read again, not once";
  EXPECT_EQ(test::wire_read(*dev, DeviceReading::CENTRAL_HEATING_MODE), 0);
  EXPECT_EQ(test::read_state(*dev, DeviceRegister::CENTRAL_HEATING_MODE), test::ReadState::ASKED);
}

TEST(HeaterController, AnAnswerWithoutAValueStopsTheCentralHeatingReadAndClearsTheReading) {
  HeaterControllerFixture fx;
  IoDevice *dev = make_towel_dryer(fx);
  fx.controller.enable_device_reading("ABC123", DeviceReading::CENTRAL_HEATING_MODE);
  test::store_reading(*dev, DeviceReading::CENTRAL_HEATING_MODE, 1);  // an earlier answer
  queue_towel_dryer_poll(fx);
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x53});  // an electric unit: no value

  EXPECT_TRUE(fx.controller.poll("ABC123")) << "a missing register is not a poll failure";
  EXPECT_EQ(test::read_state(*dev, DeviceRegister::CENTRAL_HEATING_MODE), test::ReadState::ABSENT);
  EXPECT_FALSE(test::has_reading(*dev, DeviceRegister::CENTRAL_HEATING_MODE));

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
  fx.controller.enable_device_reading("ABC123", DeviceReading::CENTRAL_HEATING_MODE);
  queue_towel_dryer_poll(fx);
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x53, 0x02});

  EXPECT_TRUE(fx.controller.poll("ABC123"));
  EXPECT_EQ(test::read_state(*dev, DeviceRegister::CENTRAL_HEATING_MODE), test::ReadState::ABSENT);
  EXPECT_FALSE(test::has_reading(*dev, DeviceRegister::CENTRAL_HEATING_MODE));
}

TEST(HeaterController, AMissedCentralHeatingReadChangesNothing) {
  HeaterControllerFixture fx;
  IoDevice *dev = make_towel_dryer(fx);
  fx.controller.enable_device_reading("ABC123", DeviceReading::CENTRAL_HEATING_MODE);
  test::store_reading(*dev, DeviceReading::CENTRAL_HEATING_MODE, 1);
  queue_towel_dryer_poll(fx);  // no reply to the read of 0x53

  EXPECT_TRUE(fx.controller.poll("ABC123"));
  EXPECT_EQ(test::read_state(*dev, DeviceRegister::CENTRAL_HEATING_MODE), test::ReadState::ASKED)
      << "asked again at the next poll";
  EXPECT_EQ(test::wire_read(*dev, DeviceReading::CENTRAL_HEATING_MODE), 1);
}

// === Occupancy: the second opt-in read, judged apart from the first (the Hekla sweep of issue #222) ===

/// @return A heater with a night light (a Sauter Hekla), its boot reads done so a poll is its extra registers only.
IoDevice *make_heater_with_light(HeaterControllerFixture &fx) {
  IoDevice *dev = make_heater(fx);
  test::set_kind(*dev, DeviceType::ELECTRICAL_HEATER, 1, DeviceModel::HEATER_WITH_LIGHT);
  dev->model_checked = true;
  test::store_reading(*dev, DeviceReading::MAX_SETPOINT, 28.0F);
  return dev;
}

/// Queue the replies of a heater with a light's poll up to and including its three extra registers.
void queue_heater_with_light_poll(HeaterControllerFixture &fx) {
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x15, 0x1B, 0x08});
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x03, 0xC3, 0x00});
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x00, 0x01});
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x17, 0x02});
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x0E, 0x00});
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x06, 0x01});        // open-window detection
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x44, 0x00});        // night light
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x42, 0x00, 0x00});  // night light brightness
}

/// @return The registers the sent frames read, in order.
std::vector<uint8_t> registers_read(const std::vector<std::vector<uint8_t>> &sent) {
  std::vector<uint8_t> out;
  for (const auto &tx : sent)
    out.push_back(sent_register(tx));
  return out;
}

TEST(HeaterController, AHeaterWithoutOccupancyEntitiesNeverReadsRegister0x01Or0x10) {
  HeaterControllerFixture fx;
  make_heater_with_light(fx);
  queue_heater_with_light_poll(fx);

  EXPECT_TRUE(fx.controller.poll("ABC123"));
  for (const uint8_t reg : registers_read(fx.radio.get_sent_data())) {
    EXPECT_NE(reg, 0x01);
    EXPECT_NE(reg, 0x10);
  }
}

TEST(HeaterController, OccupancyRegistersAreReadAfterTheExtraRegistersAndShowTheSweepValues) {
  HeaterControllerFixture fx;
  IoDevice *dev = make_heater_with_light(fx);
  fx.controller.enable_device_reading("ABC123", DeviceReading::OCCUPANCY_DETECTION);
  fx.controller.enable_device_reading("ABC123", DeviceReading::OCCUPANCY);
  queue_heater_with_light_poll(fx);
  fx.queue_register_ack(
      {0x0C, 0x60, 0x01, 0x10, 0x01});  // someone walks around the heater (presence first: enum order)
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x01, 0x01});  // detection on

  EXPECT_TRUE(fx.controller.poll("ABC123"));
  const auto read = registers_read(fx.radio.get_sent_data());
  ASSERT_EQ(read.size(), 10u);
  EXPECT_EQ(read[8], 0x10);
  EXPECT_EQ(read[9], 0x01);
  EXPECT_EQ(test::wire_read(*dev, DeviceReading::OCCUPANCY), 1);
  EXPECT_EQ(test::wire_read(*dev, DeviceReading::OCCUPANCY_DETECTION), 1);
}

TEST(HeaterController, EachOptInRegisterIsMarkedAbsentOnItsOwn) {
  HeaterControllerFixture fx;
  IoDevice *dev = make_heater_with_light(fx);
  fx.controller.enable_device_reading("ABC123", DeviceReading::OCCUPANCY_DETECTION);
  fx.controller.enable_device_reading("ABC123", DeviceReading::OCCUPANCY);
  queue_heater_with_light_poll(fx);
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x10});        // presence: answered without a value
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x01, 0x00});  // detection: off

  EXPECT_TRUE(fx.controller.poll("ABC123")) << "a missing register is not a poll failure";
  EXPECT_EQ(test::read_state(*dev, DeviceRegister::PRESENCE), test::ReadState::ABSENT);
  EXPECT_EQ(test::read_state(*dev, DeviceRegister::OCCUPANCY_DETECTION), test::ReadState::ASKED);
  EXPECT_EQ(test::wire_read(*dev, DeviceReading::OCCUPANCY_DETECTION), 0);
  EXPECT_FALSE(test::has_reading(*dev, DeviceRegister::PRESENCE));

  const size_t sent_before = fx.radio.get_sent_data().size();
  queue_heater_with_light_poll(fx);
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x01, 0x01});
  EXPECT_TRUE(fx.controller.poll("ABC123"));
  const auto &sent = fx.radio.get_sent_data();
  const auto second = registers_read({sent.begin() + static_cast<std::ptrdiff_t>(sent_before), sent.end()});
  EXPECT_EQ(std::count(second.begin(), second.end(), 0x10), 0) << "the absent register is not asked again";
  EXPECT_EQ(second.back(), 0x01) << "the other one still is";
  EXPECT_EQ(test::wire_read(*dev, DeviceReading::OCCUPANCY_DETECTION), 1);
}

TEST(HeaterController, AMissedOccupancyReadKeepsTheReadingAndTheAsk) {
  HeaterControllerFixture fx;
  IoDevice *dev = make_heater_with_light(fx);
  fx.controller.enable_device_reading("ABC123", DeviceReading::OCCUPANCY_DETECTION);
  test::store_reading(*dev, DeviceReading::OCCUPANCY_DETECTION, 1);
  queue_heater_with_light_poll(fx);  // no reply to the read of 0x0101

  EXPECT_TRUE(fx.controller.poll("ABC123"));
  EXPECT_EQ(test::read_state(*dev, DeviceRegister::OCCUPANCY_DETECTION), test::ReadState::ASKED);
  EXPECT_EQ(test::wire_read(*dev, DeviceReading::OCCUPANCY_DETECTION), 1);
}

TEST(HeaterController, OccupancyReadsAreRefusedOnAKindWithoutTheCapabilityAndForOtherRegisters) {
  HeaterControllerFixture fx;
  IoDevice *dryer = make_towel_dryer(fx);
  fx.controller.enable_device_reading("ABC123", DeviceReading::OCCUPANCY_DETECTION);
  fx.controller.enable_device_reading("ABC123", DeviceReading::OCCUPANCY);
  EXPECT_EQ(test::read_state(*dryer, DeviceRegister::OCCUPANCY_DETECTION), test::ReadState::NOT_ASKED);
  EXPECT_EQ(test::read_state(*dryer, DeviceRegister::PRESENCE), test::ReadState::NOT_ASKED);

  HeaterControllerFixture other;
  IoDevice *radiator = make_heater(other);
  other.controller.enable_device_reading("ABC123", DeviceReading::COUNT);
  EXPECT_EQ(test::read_state(*radiator, DeviceRegister::ROOM_TEMPERATURE), test::ReadState::NOT_ASKED)
      << "a value outside the enumeration has no row";
}

TEST(HeaterController, AReadingThatIsNotReadWhenAskedIsIgnoredByEnableDeviceReading) {
  HeaterControllerFixture fx;
  IoDevice *radiator = make_heater(fx);
  const auto polls = [&](ReadingPoll policy) { return test::polled_registers(*radiator, policy); };
  const DeviceReadingRow *room = find_device_reading(DeviceReading::ROOM_TEMPERATURE);
  ASSERT_NE(room, nullptr);
  ASSERT_NE(room->poll, ReadingPoll::WHEN_ASKED) << "the room temperature is polled by the profile, not asked for";
  const auto every_before = polls(ReadingPoll::EVERY_POLL);
  const auto may_miss_before = polls(ReadingPoll::EVERY_POLL_MAY_MISS);
  const auto asked_before = polls(ReadingPoll::WHEN_ASKED);

  fx.controller.enable_device_reading("ABC123", DeviceReading::ROOM_TEMPERATURE);

  EXPECT_EQ(test::read_state(*radiator, DeviceRegister::ROOM_TEMPERATURE), test::ReadState::NOT_ASKED);
  EXPECT_EQ(polls(ReadingPoll::EVERY_POLL), every_before);
  EXPECT_EQ(polls(ReadingPoll::EVERY_POLL_MAY_MISS), may_miss_before);
  EXPECT_EQ(polls(ReadingPoll::WHEN_ASKED), asked_before);
}

TEST(HeaterController, TheCentralHeatingReadIsRefusedOnAKindWithoutTheCapability) {
  HeaterControllerFixture fx;
  IoDevice *radiator = make_heater(fx);
  fx.controller.enable_device_reading("ABC123", DeviceReading::CENTRAL_HEATING_MODE);
  EXPECT_EQ(test::read_state(*radiator, DeviceRegister::CENTRAL_HEATING_MODE), test::ReadState::NOT_ASKED);
}

// === Energy total: the numeric opt-in read (register 0x0116, whole kWh, most significant byte first) ===

TEST(HeaterController, AHeaterWithoutTheEnergySensorNeverReadsRegister0x16) {
  HeaterControllerFixture fx;
  IoDevice *dev = make_towel_dryer(fx);
  queue_towel_dryer_poll(fx);

  EXPECT_TRUE(fx.controller.poll("ABC123"));
  for (const uint8_t reg : registers_read(fx.radio.get_sent_data()))
    EXPECT_NE(reg, 0x16);
  EXPECT_EQ(test::read_state(*dev, DeviceRegister::ENERGY_TOTAL), test::ReadState::NOT_ASKED);
  EXPECT_FALSE(test::has_reading(*dev, DeviceRegister::ENERGY_TOTAL));
}

TEST(HeaterController, TheEnergyTotalIsReadAtEveryPollAndKeptAboveWhatAnInt16Holds) {
  HeaterControllerFixture fx;
  IoDevice *dev = make_towel_dryer(fx);
  fx.controller.enable_device_reading("ABC123", DeviceReading::ENERGY_TOTAL);
  queue_towel_dryer_poll(fx);
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x16, 0x0D, 0x5E});

  EXPECT_TRUE(fx.controller.poll("ABC123"));
  const auto read = registers_read(fx.radio.get_sent_data());
  ASSERT_GE(read.size(), 6u);
  EXPECT_EQ(read[5], 0x16) << "right after the core registers";
  EXPECT_FLOAT_EQ(test::number_read(*dev, DeviceRegister::ENERGY_TOTAL), 3422.0F);

  const size_t sent_before = fx.radio.get_sent_data().size();
  queue_towel_dryer_poll(fx);
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x16, 0xC3, 0x50});  // 50000 kWh: past 32767
  EXPECT_TRUE(fx.controller.poll("ABC123"));
  EXPECT_EQ(sent_register(fx.radio.get_sent_data()[sent_before + 5]), 0x16) << "read again, not once";
  EXPECT_FLOAT_EQ(test::number_read(*dev, DeviceRegister::ENERGY_TOTAL), 50000.0F);
  EXPECT_EQ(test::read_state(*dev, DeviceRegister::ENERGY_TOTAL), test::ReadState::ASKED);
}

TEST(HeaterController, AnEnergyAnswerWithoutAValueStopsTheReadAndClearsTheTotal) {
  HeaterControllerFixture fx;
  IoDevice *dev = make_towel_dryer(fx);
  fx.controller.enable_device_reading("ABC123", DeviceReading::ENERGY_TOTAL);
  test::store_reading(*dev, DeviceReading::ENERGY_TOTAL, 100.0F);  // an earlier answer
  queue_towel_dryer_poll(fx);
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x16});

  EXPECT_TRUE(fx.controller.poll("ABC123")) << "a missing register is not a poll failure";
  EXPECT_EQ(test::read_state(*dev, DeviceRegister::ENERGY_TOTAL), test::ReadState::ABSENT);
  EXPECT_FALSE(test::has_reading(*dev, DeviceRegister::ENERGY_TOTAL));

  const size_t sent_before = fx.radio.get_sent_data().size();
  queue_towel_dryer_poll(fx);
  EXPECT_TRUE(fx.controller.poll("ABC123"));
  const auto &sent = fx.radio.get_sent_data();
  const auto second = registers_read({sent.begin() + static_cast<std::ptrdiff_t>(sent_before), sent.end()});
  EXPECT_EQ(std::count(second.begin(), second.end(), 0x16), 0) << "not asked again until the next boot";
}

TEST(HeaterController, AMissedEnergyReadKeepsTheTotalAndTheAsk) {
  HeaterControllerFixture fx;
  IoDevice *dev = make_towel_dryer(fx);
  fx.controller.enable_device_reading("ABC123", DeviceReading::ENERGY_TOTAL);
  test::store_reading(*dev, DeviceReading::ENERGY_TOTAL, 3422.0F);
  queue_towel_dryer_poll(fx);  // no reply to the read of 0x16

  EXPECT_TRUE(fx.controller.poll("ABC123"));
  EXPECT_EQ(test::read_state(*dev, DeviceRegister::ENERGY_TOTAL), test::ReadState::ASKED);
  EXPECT_FLOAT_EQ(test::number_read(*dev, DeviceReading::ENERGY_TOTAL), 3422.0F);
}

TEST(HeaterController, TheEnergyReadIsRefusedOnAKindWithoutTheCapability) {
  HeaterControllerFixture fx;
  IoDevice *plain = fx.add_device_of_kind(DeviceType::ELECTRICAL_HEATER, 0);
  ASSERT_NE(plain, nullptr);
  fx.controller.enable_device_reading("ABC123", DeviceReading::ENERGY_TOTAL);
  EXPECT_EQ(test::read_state(*plain, DeviceRegister::ENERGY_TOTAL), test::ReadState::NOT_ASKED);
}

// === Run timers: the minutes left of a run only while it runs, and the set durations (towel dryer) ===

/// Queue a towel dryer's five core registers with the given temporary state (0 none, 1 boost, 2 drying).
void queue_towel_dryer_poll_in_state(HeaterControllerFixture &fx, uint8_t temporary_state) {
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x15, 0x1B, 0x08});
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x03, 0xC3, 0x00});
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x36, 0x01});
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x3E, temporary_state});
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x26, 0x00});
}

/// A towel dryer that asks for the four run-timer readings.
IoDevice *make_towel_dryer_with_run_timers(HeaterControllerFixture &fx) {
  IoDevice *dev = make_towel_dryer(fx);
  for (const DeviceReading reading : {DeviceReading::BOOST_REMAINING, DeviceReading::BOOST_DURATION,
                                      DeviceReading::DRYING_REMAINING, DeviceReading::DRYING_DURATION})
    fx.controller.enable_device_reading("ABC123", reading);
  return dev;
}

/// @return How often the sent frames read one register of the heater (the low byte of 0x01xx).
std::ptrdiff_t reads_of(const std::vector<std::vector<uint8_t>> &sent, uint8_t reg) {
  const auto read = registers_read(sent);
  return std::count(read.begin(), read.end(), reg);
}

TEST(HeaterController, ADryerWithoutRunTimerSensorsNeverReadsRegisters0x3ATo0x3D) {
  HeaterControllerFixture fx;
  IoDevice *dev = make_towel_dryer(fx);
  queue_towel_dryer_poll_in_state(fx, 1);

  EXPECT_TRUE(fx.controller.poll("ABC123"));
  for (const uint8_t reg : {0x3A, 0x3B, 0x3C, 0x3D})
    EXPECT_EQ(reads_of(fx.radio.get_sent_data(), reg), 0) << static_cast<int>(reg);
  EXPECT_EQ(test::read_state(*dev, DeviceRegister::BOOST_REMAINING), test::ReadState::NOT_ASKED);
}

TEST(HeaterController, AnIdleDryerIsAskedForTheDurationsOnlyAndShowsNoTimeLeft) {
  HeaterControllerFixture fx;
  IoDevice *dev = make_towel_dryer_with_run_timers(fx);
  queue_towel_dryer_poll_in_state(fx, 0);
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x3B, 0x3C, 0x3C});
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x3D, 0x1E, 0x78});

  EXPECT_TRUE(fx.controller.poll("ABC123"));
  const auto &sent = fx.radio.get_sent_data();
  EXPECT_EQ(reads_of(sent, 0x3A), 0) << "no boost runs";
  EXPECT_EQ(reads_of(sent, 0x3C), 0) << "no drying runs";
  EXPECT_EQ(reads_of(sent, 0x3B), 1);
  EXPECT_EQ(reads_of(sent, 0x3D), 1);
  EXPECT_FLOAT_EQ(test::number_read(*dev, DeviceRegister::BOOST_DURATION), 60.0F);
  EXPECT_FLOAT_EQ(test::number_read(*dev, DeviceRegister::DRYING_DURATION), 30.0F);
  EXPECT_FLOAT_EQ(remaining_minutes(*dev), 0.0F);
}

TEST(HeaterController, ADryerInABoostReadsTheBoostTimeLeftAndNotTheDryingOne) {
  HeaterControllerFixture fx;
  IoDevice *dev = make_towel_dryer_with_run_timers(fx);
  queue_towel_dryer_poll_in_state(fx, 1);
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x3A, 0x37});
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x3B, 0x3C, 0x3C});
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x3D, 0x1E, 0x78});

  EXPECT_TRUE(fx.controller.poll("ABC123"));
  const auto &sent = fx.radio.get_sent_data();
  EXPECT_EQ(reads_of(sent, 0x3A), 1);
  EXPECT_EQ(reads_of(sent, 0x3C), 0);
  EXPECT_FLOAT_EQ(remaining_minutes(*dev), 55.0F);
}

TEST(HeaterController, ADryerInADryingRunReadsTheDryingTimeLeftAndNotTheBoostOne) {
  HeaterControllerFixture fx;
  IoDevice *dev = make_towel_dryer_with_run_timers(fx);
  queue_towel_dryer_poll_in_state(fx, 2);
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x3B, 0x3C, 0x3C});
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x3C, 0x1C});
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x3D, 0x1E, 0x78});

  EXPECT_TRUE(fx.controller.poll("ABC123"));
  const auto &sent = fx.radio.get_sent_data();
  EXPECT_EQ(reads_of(sent, 0x3A), 0);
  EXPECT_EQ(reads_of(sent, 0x3C), 1);
  EXPECT_FLOAT_EQ(remaining_minutes(*dev), 28.0F);
}

TEST(HeaterController, TheTimeLeftOfAnEndedRunIsForgottenAndNotShownAsTheNextRunsFirstValue) {
  HeaterControllerFixture fx;
  IoDevice *dev = make_towel_dryer_with_run_timers(fx);
  test::store_reading(*dev, DeviceReading::BOOST_REMAINING, 5.0F);  // left over from the boost that has ended
  queue_towel_dryer_poll_in_state(fx, 0);

  EXPECT_TRUE(fx.controller.poll("ABC123"));
  EXPECT_FALSE(test::has_reading(*dev, DeviceRegister::BOOST_REMAINING));
}

TEST(HeaterController, ABoostStartedFromHomeAssistantDoesNotShowTheTimeLeftOfTheBoostBefore) {
  // A boost cancelled and started again between two polls: the last poll read 40 minutes left of the old boost.
  HeaterControllerFixture fx;
  IoDevice *dev = make_towel_dryer_with_run_timers(fx);
  test::store_reading(*dev, DeviceReading::TEMPORARY_STATE, static_cast<int16_t>(HeatingTemporaryState::BOOST));
  test::store_reading(*dev, DeviceReading::BOOST_REMAINING, 40.0F);
  test::store_reading(*dev, DeviceReading::DRYING_DURATION, 30.0F);
  queue_register_write_and_read_back(fx, 0x3E, static_cast<uint8_t>(HeatingTemporaryState::BOOST),
                                     static_cast<uint8_t>(HeatingTemporaryState::BOOST));

  ASSERT_TRUE(fx.controller.execute_heating_plan(
      "ABC123", register_plan(DeviceRegister::TEMPORARY_STATE, static_cast<float>(HeatingTemporaryState::BOOST)),
      nullptr));

  EXPECT_TRUE(std::isnan(remaining_minutes(*dev)))
      << "the new boost's time left is unknown until the next poll, not the old boost's 40 minutes";
  EXPECT_FLOAT_EQ(test::number_read(*dev, DeviceRegister::DRYING_DURATION), 30.0F)
      << "a reading without a condition stays";
}

TEST(HeaterController, ARunTimerAnswerWithoutAValueMarksOnlyThatRegisterAbsent) {
  HeaterControllerFixture fx;
  IoDevice *dev = make_towel_dryer_with_run_timers(fx);
  queue_towel_dryer_poll_in_state(fx, 1);
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x3A});  // the boost time left: no value
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x3B, 0x3C, 0x3C});
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x3D, 0x1E, 0x78});

  EXPECT_TRUE(fx.controller.poll("ABC123")) << "a missing register is not a poll failure";
  EXPECT_EQ(test::read_state(*dev, DeviceRegister::BOOST_REMAINING), test::ReadState::ABSENT);
  EXPECT_EQ(test::read_state(*dev, DeviceRegister::BOOST_DURATION), test::ReadState::ASKED);
  EXPECT_TRUE(std::isnan(remaining_minutes(*dev))) << "no value, not 0 minutes";

  const size_t sent_before = fx.radio.get_sent_data().size();
  queue_towel_dryer_poll_in_state(fx, 1);
  EXPECT_TRUE(fx.controller.poll("ABC123"));
  const auto &sent = fx.radio.get_sent_data();
  EXPECT_EQ(reads_of({sent.begin() + static_cast<std::ptrdiff_t>(sent_before), sent.end()}, 0x3A), 0)
      << "not asked again until the next boot";
}

TEST(HeaterController, TheRunTimerReadsAreRefusedOnAKindWithoutTheCapability) {
  HeaterControllerFixture fx;
  IoDevice *radiator = make_heater(fx);
  for (const DeviceReading reading :
       {DeviceReading::BOOST_REMAINING, DeviceReading::BOOST_DURATION, DeviceReading::DRYING_REMAINING,
        DeviceReading::DRYING_DURATION, DeviceReading::REMAINING_TIME}) {
    fx.controller.enable_device_reading("ABC123", reading);
    for (const DeviceRegister fn : reading_polled_registers(*find_device_reading(reading)))
      EXPECT_EQ(test::read_state(*radiator, fn), test::ReadState::NOT_ASKED) << device_register_name(fn);
  }
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
  EXPECT_TRUE(std::isnan(test::number_read(*dev, DeviceReading::SETPOINT)));
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

TEST(HeaterController, TheModelCheckCountsATowelModeByteTheCodecRefusesAsAnAnswerWithAValue) {
  HeaterControllerFixture fx;
  IoDevice *dev = make_heater(fx);
  // A mode byte above the register's range: the heater has the register, so the check is complete, and nothing is
  // stored for the mode (the codec refuses the byte).
  queue_read_reply(fx, 0x19, {'R', 'A', 'D'});
  queue_read_reply(fx, DRYER_MODE, {0x04});

  fx.controller.check_heater_model("ABC123", 1);

  EXPECT_TRUE(dev->model_checked) << "both reads were answered";
  EXPECT_EQ(sent_registers(fx), (std::vector<uint8_t>{0x19, DRYER_MODE}));
  EXPECT_FALSE(test::has_reading(*dev, DeviceRegister::TOWEL_MODE)) << "the model check stores nothing";
}

TEST(HeaterController, TheModelCheckTakesTheTowelModeTheMainPollAlreadyReadAndAsksNoSecondTime) {
  HeaterControllerFixture fx;
  IoDevice *dev = make_towel_dryer(fx);
  test::store_reading(*dev, DeviceReading::TOWEL_MODE, static_cast<int16_t>(TowelDryerMode::PROG));
  queue_read_reply(fx, 0x19, {'A', 'D', 'E'});

  fx.controller.check_heater_model("ABC123", 1);

  EXPECT_TRUE(dev->model_checked);
  EXPECT_EQ(sent_registers(fx), std::vector<uint8_t>{0x19}) << "only the name: the mode was read by the poll";
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
    const uint8_t registers[] = {test::dhw_register_byte(DeviceRegister::DHW_WATER_TEMPERATURE),
                                 test::dhw_register_byte(DeviceRegister::DHW_MODE),
                                 test::dhw_register_byte(DeviceRegister::DHW_HEATING),
                                 test::dhw_register_byte(DeviceRegister::DHW_BOOST_DURATION),
                                 test::dhw_register_byte(DeviceRegister::DHW_AWAY_DURATION),
                                 test::dhw_register_byte(DeviceRegister::DHW_ENERGY)};
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
      fx.queue_register_ack({0x0C, 0x60, 0x00, test::dhw_register_byte(DeviceRegister::DHW_BRAND), 0x41});
  };
  queue_poll(true);

  EXPECT_TRUE(fx.controller.poll("ABC123"));

  ASSERT_EQ(fx.status_reads.size(), 1u) << "the status read comes first";
  const auto &sent = fx.radio.get_sent_data();
  ASSERT_EQ(sent.size(), 7u);
  const uint8_t expected[] = {test::dhw_register_byte(DeviceRegister::DHW_WATER_TEMPERATURE),
                              test::dhw_register_byte(DeviceRegister::DHW_MODE),
                              test::dhw_register_byte(DeviceRegister::DHW_HEATING),
                              test::dhw_register_byte(DeviceRegister::DHW_BOOST_DURATION),
                              test::dhw_register_byte(DeviceRegister::DHW_AWAY_DURATION),
                              test::dhw_register_byte(DeviceRegister::DHW_ENERGY),
                              test::dhw_register_byte(DeviceRegister::DHW_BRAND)};
  for (size_t i = 0; i < std::size(expected); i++)
    EXPECT_EQ(sent_register(sent[i]), expected[i]) << i;
  EXPECT_EQ(test::water_heater_values(*fx.registry.get("ABC123")).brand, 'A');
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

// === What one register-heater poll reads (pinned) ===
//
// The literal register sequences of the first and the second poll of each heater kind, with nothing asked for and with
// every optional reading asked for. They state what a poll reads and in which order: the core registers, the
// temperature in force while the programme runs, the registers that may miss, the asked ones, the one-off maximum, then
// the model check. They do not change when the way the poll chooses its registers does.

namespace {

/// The reply payload (the echo of the register and its value bytes) a pinned poll gets for a register, by default.
std::vector<uint8_t> pin_reply_payload(uint8_t reg) {
  switch (reg) {
    case 0x15:  // room temperature, 20.75 C
      return {0x0C, 0x60, 0x01, reg, 0x1B, 0x08};
    case 0x03:  // setpoint, 19.5 C
    case 0x2A:  // temperature in force of a radiator
    case 0x37:  // temperature in force of a towel dryer
      return {0x0C, 0x60, 0x01, reg, 0xC3, 0x00};
    case 0x02:  // maximum setpoint, 28.0 C
      return {0x0C, 0x60, 0x01, reg, 0x18, 0x01};
    case 0x19:  // model name (synthetic)
      return {0x0C, 0x60, 0x01, reg, 'R', 'A', 'D'};
    case 0x36:  // the towel-dryer mode: the radiator's model check gets no value
      return {0x0C, 0x60, 0x01, reg};
    case 0x42:  // night light brightness
      return {0x0C, 0x60, 0x01, reg, 0x00, 0x00};
    case 0x16:  // energy total
      return {0x0C, 0x60, 0x01, reg, 0x0D, 0x5E};
    case 0x3B:  // boost duration
      return {0x0C, 0x60, 0x01, reg, 0x3C, 0x3C};
    case 0x3D:  // drying duration
      return {0x0C, 0x60, 0x01, reg, 0x1E, 0x78};
    case 0x3A:  // boost minutes left
      return {0x0C, 0x60, 0x01, reg, 0x37};
    case 0x3C:  // drying minutes left
      return {0x0C, 0x60, 0x01, reg, 0x1C};
    default:  // a one-byte register: its value is 1 (the mode 'manual', the detection on) or 0
      return {0x0C, 0x60, 0x01, reg,
              static_cast<uint8_t>(reg == 0x00 || reg == 0x01 || reg == 0x06 || reg == 0x10 ? 1 : 0)};
  }
}

/// A pinned poll's replies that differ from pin_reply_payload(): a register, then its value bytes.
using PinReplies = std::vector<std::pair<uint8_t, std::vector<uint8_t>>>;

/// Run one poll against replies for exactly the registers `expected` names, in that order.
/// @return The registers the poll read.
std::vector<uint8_t> run_pinned_poll(HeaterControllerFixture &fx, const std::vector<uint8_t> &expected,
                                     const PinReplies &replies) {
  const size_t before = fx.radio.get_sent_data().size();
  for (const uint8_t reg : expected) {
    std::vector<uint8_t> payload = pin_reply_payload(reg);
    for (const auto &[replied_reg, value] : replies) {
      if (replied_reg == reg)
        payload = [&] {
          std::vector<uint8_t> v{0x0C, 0x60, 0x01, reg};
          v.insert(v.end(), value.begin(), value.end());
          return v;
        }();
    }
    fx.queue_register_ack(payload);
  }
  EXPECT_TRUE(fx.controller.poll("ABC123"));
  const auto &sent = fx.radio.get_sent_data();
  return registers_read({sent.begin() + static_cast<std::ptrdiff_t>(before), sent.end()});
}

/// Ask for every optional reading; a kind without the capability ignores the ones it has not.
void ask_every_reading(HeaterControllerFixture &fx) {
  for (const DeviceReading reading :
       {DeviceReading::OCCUPANCY_DETECTION, DeviceReading::OCCUPANCY, DeviceReading::ENERGY_TOTAL,
        DeviceReading::CENTRAL_HEATING_MODE, DeviceReading::REMAINING_TIME, DeviceReading::BOOST_DURATION,
        DeviceReading::DRYING_DURATION})
    fx.controller.enable_device_reading("ABC123", reading);
}

using Registers = std::vector<uint8_t>;

IoDevice *make_pinned_heater(HeaterControllerFixture &fx, DeviceModel model) {
  IoDevice *dev = make_heater(fx);
  test::set_kind(*dev, DeviceType::ELECTRICAL_HEATER, 1, model);
  return dev;
}

}  // namespace

TEST(HeaterPollPin, ARadiatorWithNothingAskedReadsItsCoreRegistersTheDetectionSettingTheMaximumAndTheModelCheck) {
  HeaterControllerFixture fx;
  make_pinned_heater(fx, DeviceModel::NONE);
  EXPECT_EQ(run_pinned_poll(fx, {0x15, 0x03, 0x00, 0x17, 0x0E, 0x06, 0x02, 0x19, 0x36}, {}),
            (Registers{0x15, 0x03, 0x00, 0x17, 0x0E, 0x06, 0x02, 0x19, 0x36}));
  EXPECT_EQ(run_pinned_poll(fx, {0x15, 0x03, 0x00, 0x17, 0x0E, 0x06}, {}),
            (Registers{0x15, 0x03, 0x00, 0x17, 0x0E, 0x06}))
      << "the second poll reads neither the maximum nor the model again";
  EXPECT_TRUE(fx.status_reads.empty());
}

TEST(HeaterPollPin, ARadiatorWithEverythingAskedReadsPresenceDetectionAndEnergyAfterTheDetectionSetting) {
  HeaterControllerFixture fx;
  make_pinned_heater(fx, DeviceModel::NONE);
  ask_every_reading(fx);
  EXPECT_EQ(run_pinned_poll(fx, {0x15, 0x03, 0x00, 0x17, 0x0E, 0x06, 0x10, 0x01, 0x16, 0x02, 0x19, 0x36}, {}),
            (Registers{0x15, 0x03, 0x00, 0x17, 0x0E, 0x06, 0x10, 0x01, 0x16, 0x02, 0x19, 0x36}));
  EXPECT_EQ(run_pinned_poll(fx, {0x15, 0x03, 0x00, 0x17, 0x0E, 0x06, 0x10, 0x01, 0x16}, {}),
            (Registers{0x15, 0x03, 0x00, 0x17, 0x0E, 0x06, 0x10, 0x01, 0x16}));
}

TEST(HeaterPollPin, ARadiatorInItsProgrammeReadsTheTemperatureInForceRightAfterTheCoreRegisters) {
  HeaterControllerFixture fx;
  make_pinned_heater(fx, DeviceModel::NONE);
  const PinReplies auto_mode{{0x00, {0x00}}};
  EXPECT_EQ(run_pinned_poll(fx, {0x15, 0x03, 0x00, 0x17, 0x0E, 0x2A, 0x06, 0x02, 0x19, 0x36}, auto_mode),
            (Registers{0x15, 0x03, 0x00, 0x17, 0x0E, 0x2A, 0x06, 0x02, 0x19, 0x36}));
  EXPECT_EQ(run_pinned_poll(fx, {0x15, 0x03, 0x00, 0x17, 0x0E, 0x2A, 0x06}, auto_mode),
            (Registers{0x15, 0x03, 0x00, 0x17, 0x0E, 0x2A, 0x06}));
  EXPECT_TRUE(fx.status_reads.empty()) << "a radiator's poll sends no status read";
}

TEST(HeaterPollPin, AHeaterWithALightReadsTheLightAfterTheDetectionSettingAndNothingMoreWhenNothingIsAsked) {
  HeaterControllerFixture fx;
  make_pinned_heater(fx, DeviceModel::HEATER_WITH_LIGHT);
  EXPECT_EQ(run_pinned_poll(fx, {0x15, 0x03, 0x00, 0x17, 0x0E, 0x06, 0x44, 0x42, 0x02, 0x19, 0x36}, {}),
            (Registers{0x15, 0x03, 0x00, 0x17, 0x0E, 0x06, 0x44, 0x42, 0x02, 0x19, 0x36}));
  EXPECT_EQ(run_pinned_poll(fx, {0x15, 0x03, 0x00, 0x17, 0x0E, 0x06, 0x44, 0x42}, {}),
            (Registers{0x15, 0x03, 0x00, 0x17, 0x0E, 0x06, 0x44, 0x42}));
}

TEST(HeaterPollPin, AHeaterWithALightInItsProgrammeReadsTheTemperatureInForceBeforeTheLightRegisters) {
  HeaterControllerFixture fx;
  make_pinned_heater(fx, DeviceModel::HEATER_WITH_LIGHT);
  const PinReplies auto_mode{{0x00, {0x00}}};
  EXPECT_EQ(run_pinned_poll(fx, {0x15, 0x03, 0x00, 0x17, 0x0E, 0x2A, 0x06, 0x44, 0x42, 0x02, 0x19, 0x36}, auto_mode),
            (Registers{0x15, 0x03, 0x00, 0x17, 0x0E, 0x2A, 0x06, 0x44, 0x42, 0x02, 0x19, 0x36}));
  EXPECT_EQ(run_pinned_poll(fx, {0x15, 0x03, 0x00, 0x17, 0x0E, 0x2A, 0x06, 0x44, 0x42}, auto_mode),
            (Registers{0x15, 0x03, 0x00, 0x17, 0x0E, 0x2A, 0x06, 0x44, 0x42}));
}

TEST(HeaterPollPin, AHeaterWithALightAndEverythingAskedReadsTheAskedRegistersAfterTheLight) {
  HeaterControllerFixture fx;
  make_pinned_heater(fx, DeviceModel::HEATER_WITH_LIGHT);
  ask_every_reading(fx);
  EXPECT_EQ(
      run_pinned_poll(fx, {0x15, 0x03, 0x00, 0x17, 0x0E, 0x06, 0x44, 0x42, 0x10, 0x01, 0x16, 0x02, 0x19, 0x36}, {}),
      (Registers{0x15, 0x03, 0x00, 0x17, 0x0E, 0x06, 0x44, 0x42, 0x10, 0x01, 0x16, 0x02, 0x19, 0x36}));
  EXPECT_EQ(run_pinned_poll(fx, {0x15, 0x03, 0x00, 0x17, 0x0E, 0x06, 0x44, 0x42, 0x10, 0x01, 0x16}, {}),
            (Registers{0x15, 0x03, 0x00, 0x17, 0x0E, 0x06, 0x44, 0x42, 0x10, 0x01, 0x16}));
}

TEST(HeaterPollPin, ATowelDryerWithNothingAskedReadsItsOwnRegistersTheMaximumAndTheModelName) {
  HeaterControllerFixture fx;
  make_towel_dryer(fx);
  const PinReplies manual{{0x36, {0x03}}};
  EXPECT_EQ(run_pinned_poll(fx, {0x15, 0x03, 0x36, 0x3E, 0x26, 0x02, 0x19}, manual),
            (Registers{0x15, 0x03, 0x36, 0x3E, 0x26, 0x02, 0x19}));
  EXPECT_EQ(run_pinned_poll(fx, {0x15, 0x03, 0x36, 0x3E, 0x26}, manual), (Registers{0x15, 0x03, 0x36, 0x3E, 0x26}));
  EXPECT_EQ(fx.status_reads.size(), 2u) << "every poll of a dryer asks for the heating level, in every mode";
}

TEST(HeaterPollPin, ATowelDryerWithEverythingAskedReadsTheDurationsAndTheRunNowOnTop) {
  const PinReplies manual{{0x36, {0x03}}};
  {
    HeaterControllerFixture fx;
    make_towel_dryer(fx);
    ask_every_reading(fx);
    EXPECT_EQ(run_pinned_poll(fx, {0x15, 0x03, 0x36, 0x3E, 0x26, 0x53, 0x16, 0x3B, 0x3D, 0x02, 0x19}, manual),
              (Registers{0x15, 0x03, 0x36, 0x3E, 0x26, 0x53, 0x16, 0x3B, 0x3D, 0x02, 0x19}))
        << "no run: only the durations";
    EXPECT_EQ(run_pinned_poll(fx, {0x15, 0x03, 0x36, 0x3E, 0x26, 0x53, 0x16, 0x3B, 0x3D}, manual),
              (Registers{0x15, 0x03, 0x36, 0x3E, 0x26, 0x53, 0x16, 0x3B, 0x3D}));
  }
  {
    HeaterControllerFixture fx;
    make_towel_dryer(fx);
    ask_every_reading(fx);
    EXPECT_EQ(run_pinned_poll(fx, {0x15, 0x03, 0x36, 0x3E, 0x26, 0x53, 0x16, 0x3A, 0x3B, 0x3D, 0x02, 0x19},
                              {{0x36, {0x03}}, {0x3E, {0x01}}}),
              (Registers{0x15, 0x03, 0x36, 0x3E, 0x26, 0x53, 0x16, 0x3A, 0x3B, 0x3D, 0x02, 0x19}))
        << "a boost: its minutes left too";
  }
  {
    HeaterControllerFixture fx;
    make_towel_dryer(fx);
    ask_every_reading(fx);
    EXPECT_EQ(run_pinned_poll(fx, {0x15, 0x03, 0x36, 0x3E, 0x26, 0x53, 0x16, 0x3B, 0x3C, 0x3D, 0x02, 0x19},
                              {{0x36, {0x03}}, {0x3E, {0x02}}}),
              (Registers{0x15, 0x03, 0x36, 0x3E, 0x26, 0x53, 0x16, 0x3B, 0x3C, 0x3D, 0x02, 0x19}))
        << "a drying run: its minutes left too";
  }
}

TEST(HeaterPollPin, ATowelDryerInItsProgrammeReadsTheTemperatureInForceThenAsksForTheLevelOncePerPoll) {
  HeaterControllerFixture fx;
  make_towel_dryer(fx);
  ask_every_reading(fx);
  const PinReplies programme{{0x36, {0x02}}};
  EXPECT_EQ(run_pinned_poll(fx, {0x15, 0x03, 0x36, 0x3E, 0x26, 0x37, 0x53, 0x16, 0x3B, 0x3D, 0x02, 0x19}, programme),
            (Registers{0x15, 0x03, 0x36, 0x3E, 0x26, 0x37, 0x53, 0x16, 0x3B, 0x3D, 0x02, 0x19}));
  EXPECT_EQ(fx.status_reads.size(), 1u);
  EXPECT_EQ(run_pinned_poll(fx, {0x15, 0x03, 0x36, 0x3E, 0x26, 0x37, 0x53, 0x16, 0x3B, 0x3D}, programme),
            (Registers{0x15, 0x03, 0x36, 0x3E, 0x26, 0x37, 0x53, 0x16, 0x3B, 0x3D}));
  EXPECT_EQ(fx.status_reads.size(), 2u) << "every poll asks for the level";
}

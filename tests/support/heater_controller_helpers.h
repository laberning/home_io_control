#pragma once

/// @file heater_controller_helpers.h
/// @brief Builders and readers the HeaterController tests of several files share: devices of each heater kind,
/// register-frame accessors, queued register replies and the register and mode constants of the heater families.
///
/// They work on a HeaterControllerFixture, so a test file names its subject and not the plumbing.

#include "heater_controller.h"
#include "proto_heating.h"
#include "proto_heating_profiles.h"

#include "heater_controller_fixture.h"
#include "test_helpers.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace heater_controller_test {

using namespace esphome::home_io_control;

inline IoDevice *make_heater(HeaterControllerFixture &fx) {
  IoDevice *dev = fx.add_device_of_kind(DeviceType::ELECTRICAL_HEATER, 1);
  EXPECT_NE(dev, nullptr);
  return dev;
}

inline IoDevice *make_towel_dryer(HeaterControllerFixture &fx) {
  IoDevice *dev = make_heater(fx);
  test::set_kind(*dev, DeviceType::ELECTRICAL_HEATER, 1, DeviceModel::TOWEL_DRYER);
  return dev;
}

inline IoDevice *make_water_heater(HeaterControllerFixture &fx) {
  DeviceConfig config;
  config.type = DeviceType::DOMESTIC_HOT_WATER;
  config.entity_class = DeviceCapabilityClass::WATER_HEATER;
  IoDevice *dev = fx.add_device(config);
  EXPECT_NE(dev, nullptr);
  return dev;
}

inline IoDevice *make_cover(HeaterControllerFixture &fx) { return fx.add_device_of_kind(DeviceType::ROLLER_SHUTTER); }

/// Register low byte of a sent CMD_WRITE_PRIVATE frame (payload `0C <dir> 01 <reg>` starts at offset 9).
inline uint8_t sent_register(const std::vector<uint8_t> &tx) { return tx.at(12); }

/// The payload bytes (from wire offset 9) of a sent frame.
inline std::vector<uint8_t> sent_payload(const std::vector<uint8_t> &tx, size_t len) {
  return std::vector<uint8_t>(tx.begin() + 9, tx.begin() + 9 + len);
}

/// A plan that writes one register and reads it back.
inline HeatingWritePlan register_plan(DeviceRegister fn, float value) {
  HeatingWritePlan plan;
  plan.kind = HeatingPlanKind::REGISTER;
  plan.register_fn = fn;
  plan.register_value = value;
  plan.read_back = true;
  return plan;
}

/// Queue the acknowledgement of a write of register @p reg and the read-back reply of @p tenths_lo / 0.
inline void queue_register_write_and_read_back(HeaterControllerFixture &fx, uint8_t reg, uint8_t write_lo,
                                               uint8_t back_lo) {
  fx.queue_register_ack({0x0C, 0x61, 0x01, reg, write_lo, 0x00});
  fx.queue_register_ack({0x0C, 0x60, 0x01, reg, back_lo, 0x00});
}

/// The registers a frame list read or wrote, in order.
inline std::vector<uint8_t> sent_registers(const HeaterControllerFixture &fx) {
  std::vector<uint8_t> registers;
  for (const auto &frame : fx.radio.get_sent_data())
    registers.push_back(sent_register(frame));
  return registers;
}

/// Queue the heater's reply to a read of register @p reg with the value bytes @p value.
inline void queue_read_reply(HeaterControllerFixture &fx, uint8_t reg, std::vector<uint8_t> value) {
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

}  // namespace heater_controller_test

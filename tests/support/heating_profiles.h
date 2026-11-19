#pragma once

/// @file heating_profiles.h
/// @brief Look up the heater profiles the heater tests are about, failing the test when one is missing.

#include "proto_device_types.h"
#include "proto_heating_profiles.h"

#include <cstdint>

#include <gtest/gtest.h>

namespace test {

/// The heating profile of a device kind; the test fails when the kind has none.
inline const esphome::home_io_control::HeatingProfile &profile_of(
    esphome::home_io_control::DeviceType type, uint8_t subtype,
    esphome::home_io_control::DeviceModel model = esphome::home_io_control::DeviceModel::NONE) {
  const esphome::home_io_control::HeatingProfile *profile =
      esphome::home_io_control::find_heating_profile(type, subtype, model);
  EXPECT_NE(profile, nullptr);
  return *profile;
}

/// The profile of a radiator with an adjustable setpoint (electrical heater, subtype 1).
inline const esphome::home_io_control::HeatingProfile &radiator_profile() {
  return profile_of(esphome::home_io_control::DeviceType::ELECTRICAL_HEATER, 1);
}

/// The profile of a towel dryer (electrical heater, subtype 1, model TOWEL_DRYER).
inline const esphome::home_io_control::HeatingProfile &towel_dryer_profile() {
  return profile_of(esphome::home_io_control::DeviceType::ELECTRICAL_HEATER, 1,
                    esphome::home_io_control::DeviceModel::TOWEL_DRYER);
}

/// The profile of a heater that takes comfort levels only (electrical heater, subtype 0).
inline const esphome::home_io_control::HeatingProfile &level_heater_profile() {
  return profile_of(esphome::home_io_control::DeviceType::ELECTRICAL_HEATER, 0);
}

}  // namespace test

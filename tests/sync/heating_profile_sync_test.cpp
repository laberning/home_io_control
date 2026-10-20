/// @file heating_profile_sync_test.cpp
/// @brief Verifies that the window companions climate.py generates (HEATING_WINDOW_PROFILES in
///        hub_validators.py) are exactly the heating profiles whose features say they have them.

#include "proto_heating_profiles.h"
#include "support/python_dict_parser.h"
#include "test_helpers.h"

#include <string>

using namespace esphome::home_io_control;

TEST(HeatingProfileSync, WindowCompanionsMatchTheProfileFeatures) {
  const auto types =
      test::parse_python_uint8_dict("components/home_io_control/hub_validators.py", "DEVICE_TYPE_OPTIONS");
  const auto windows =
      test::parse_python_uint8_dict("components/home_io_control/hub_validators.py", "HEATING_WINDOW_PROFILES");
  ASSERT_FALSE(windows.empty()) << "HEATING_WINDOW_PROFILES not parsed from hub_validators.py";

  // Every Python entry names a profile that has both companions.
  for (const auto &[name, subtype] : windows) {
    ASSERT_TRUE(types.count(name)) << name << " is not a DEVICE_TYPE_OPTIONS name";
    const HeatingProfile *profile = find_heating_profile(static_cast<DeviceType>(types.at(name)), subtype);
    ASSERT_NE(profile, nullptr) << name << "/" << +subtype << " has no heating profile";
    EXPECT_TRUE(profile->features.window_sensor) << name;
    EXPECT_TRUE(profile->features.open_window_detection) << name;
  }

  // Every profile that has the companions is listed in Python (exact rows only; no fallback).
  for (const auto &[name, value] : types) {
    for (const uint8_t subtype : {0, 1, 2, 58}) {
      const HeatingProfile *profile = find_heating_profile(static_cast<DeviceType>(value), subtype);
      if (profile == nullptr || profile->subtype != subtype)
        continue;
      const bool has_window = profile->features.window_sensor || profile->features.open_window_detection;
      const bool listed = windows.count(name) && windows.at(name) == subtype;
      EXPECT_EQ(has_window, listed) << name << "/" << +subtype;
      EXPECT_EQ(profile->features.window_sensor, profile->features.open_window_detection)
          << "climate.py generates the two companions together";
    }
  }
}

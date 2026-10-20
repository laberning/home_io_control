/// @file heating_profiles_test.cpp
/// @brief Tests for the heating profile table, heating-level codec and the level EXECUTE frame.

#include "proto_commands.h"
#include "proto_device_types.h"
#include "proto_heating_profiles.h"

#include <gtest/gtest.h>

using namespace esphome::home_io_control;

namespace {

const uint8_t OWN[NODE_ID_SIZE] = {0x11, 0x22, 0x33};
const uint8_t DST[NODE_ID_SIZE] = {0xAB, 0xC1, 0x23};

}  // namespace

// === Lookup ===

TEST(HeatingProfiles, EachElectricalHeaterKindHasItsOwnControlPath) {
  const HeatingProfile *setpoint = find_heating_profile(DeviceType::ELECTRICAL_HEATER, 1);
  ASSERT_NE(setpoint, nullptr);
  EXPECT_EQ(setpoint->path, HeatingControlPath::PRIVATE_REGISTERS);

  const HeatingProfile *plain = find_heating_profile(DeviceType::ELECTRICAL_HEATER, 0);
  ASSERT_NE(plain, nullptr);
  EXPECT_EQ(plain->path, HeatingControlPath::HEATING_LEVEL);

  EXPECT_EQ(find_heating_profile(DeviceType::ELECTRICAL_HEATER, 9), plain)
      << "an unnamed subtype falls back to subtype 0";
  EXPECT_EQ(find_heating_profile(DeviceType::HEAT_PUMP, 0), nullptr);
  EXPECT_EQ(find_heating_profile(DeviceType::HEATING_TEMPERATURE_INTERFACE, 0), nullptr);
  EXPECT_EQ(find_heating_profile(DeviceType::ROLLER_SHUTTER, 0), nullptr);
  EXPECT_EQ(find_heating_profile(DeviceType::UNKNOWN, 0), nullptr);
}

TEST(HeatingProfiles, RegisterFramesAreForRegisterHeatersOnly) {
  EXPECT_TRUE(device_uses_heating_registers(DeviceType::ELECTRICAL_HEATER, 1));
  EXPECT_FALSE(device_uses_heating_registers(DeviceType::ELECTRICAL_HEATER, 0))
      << "a heater that takes the standard EXECUTE must never be sent a register write";
  EXPECT_FALSE(device_uses_heating_registers(DeviceType::ELECTRICAL_HEATER, 9));
  EXPECT_FALSE(device_uses_heating_registers(DeviceType::HEAT_PUMP, 0));
  EXPECT_TRUE(device_uses_heating_levels(DeviceType::ELECTRICAL_HEATER, 0));
  EXPECT_FALSE(device_uses_heating_levels(DeviceType::ELECTRICAL_HEATER, 1));
}

TEST(HeatingProfiles, ClimateGateAndTypeTableAgreeForEveryRow) {
  // One source: a climate-class row a platform drives has a profile, and a profile exists nowhere else.
  for (const DeviceType type : {DeviceType::ELECTRICAL_HEATER, DeviceType::HEAT_PUMP, DeviceType::EXTERIOR_HEATING,
                                DeviceType::HEATING_TEMPERATURE_INTERFACE, DeviceType::ROLLER_SHUTTER}) {
    for (const uint8_t subtype : {0, 1, 2, 58}) {
      const bool table_says_climate_platform =
          device_capability_class(type, subtype) == DeviceCapabilityClass::CLIMATE &&
          device_has_platform(type, subtype);
      EXPECT_EQ(find_heating_profile(type, subtype) != nullptr, table_says_climate_platform)
          << static_cast<unsigned>(type) << "/" << +subtype;
      EXPECT_EQ(device_supports_climate_control(type, subtype), table_says_climate_platform);
    }
  }
}

// === Profile contents ===

TEST(HeatingProfiles, SetpointHeaterOffersTheCardItsRegistersCanDrive) {
  const HeatingProfile &p = *find_heating_profile(DeviceType::ELECTRICAL_HEATER, 1);
  EXPECT_TRUE(p.features.target_temperature);
  EXPECT_TRUE(p.features.current_temperature);
  EXPECT_TRUE(p.features.action);
  EXPECT_TRUE(p.features.window_sensor);
  EXPECT_TRUE(p.features.open_window_detection);

  ASSERT_EQ(p.mode_count, 3u);
  EXPECT_EQ(p.modes[0].hvac, HeatingHvacMode::OFF);
  EXPECT_EQ(p.modes[0].wire, 4);
  EXPECT_EQ(p.modes[1].hvac, HeatingHvacMode::HEAT);
  EXPECT_EQ(p.modes[1].wire, 1);
  EXPECT_EQ(p.modes[2].hvac, HeatingHvacMode::AUTO);
  EXPECT_EQ(p.modes[2].wire, 0);

  ASSERT_EQ(p.preset_count, 3u);
  EXPECT_EQ(p.presets[0].id, HeatingPresetId::HOME);
  EXPECT_EQ(p.presets[0].write_fn, HeatingFunction::SET_PRESENCE);
  EXPECT_FALSE(p.presets[0].observed) << "presence has no register to read back";
  EXPECT_EQ(p.presets[1].id, HeatingPresetId::AWAY);
  EXPECT_EQ(p.presets[2].id, HeatingPresetId::CUSTOM);
  EXPECT_STREQ(p.presets[2].custom_name, "Program");
  EXPECT_EQ(p.presets[2].write_fn, HeatingFunction::SET_MODE);
  EXPECT_EQ(p.presets[2].wire, 2);

  ASSERT_EQ(p.poll_register_count, 5u);
  EXPECT_EQ(p.poll_registers[0], HeatingFunction::ROOM_TEMPERATURE);
  EXPECT_EQ(p.poll_registers[4], HeatingFunction::WINDOW_OPEN);
  ASSERT_EQ(p.boot_register_count, 2u);
  EXPECT_EQ(p.boot_registers[0], HeatingFunction::MAX_SETPOINT);
  EXPECT_EQ(p.boot_registers[1], HeatingFunction::SET_OPEN_WINDOW_DETECTION);
}

TEST(HeatingProfiles, SetpointHeaterModeRegisterMapsBackToTheCard) {
  const HeatingProfile &p = *find_heating_profile(DeviceType::ELECTRICAL_HEATER, 1);
  const auto *auto_mode = find_observed_state(p, 0);
  ASSERT_NE(auto_mode, nullptr);
  EXPECT_EQ(auto_mode->hvac, HeatingHvacMode::AUTO);
  EXPECT_EQ(auto_mode->preset, -1);
  EXPECT_EQ(find_observed_state(p, 1)->hvac, HeatingHvacMode::HEAT);
  const auto *program = find_observed_state(p, 2);
  ASSERT_NE(program, nullptr);
  EXPECT_EQ(program->hvac, HeatingHvacMode::AUTO) << "the schedule has no HVAC mode of its own";
  ASSERT_GE(program->preset, 0);
  EXPECT_STREQ(p.presets[program->preset].custom_name, "Program");
  EXPECT_EQ(find_observed_state(p, 3)->hvac, HeatingHvacMode::HEAT) << "external is shown as heat";
  EXPECT_EQ(find_observed_state(p, 4)->hvac, HeatingHvacMode::OFF);
  EXPECT_EQ(find_observed_state(p, 5), nullptr);
}

TEST(HeatingProfiles, PlainHeaterOffersLevelsAsModesAndPresets) {
  const HeatingProfile &p = *find_heating_profile(DeviceType::ELECTRICAL_HEATER, 0);
  EXPECT_FALSE(p.features.target_temperature);
  EXPECT_FALSE(p.features.current_temperature);
  EXPECT_FALSE(p.features.action);
  EXPECT_FALSE(p.features.window_sensor);
  EXPECT_FALSE(p.features.open_window_detection);
  EXPECT_EQ(p.poll_register_count, 0u) << "a level heater is polled with the status read";

  ASSERT_EQ(p.mode_count, 2u);
  EXPECT_EQ(p.modes[0].hvac, HeatingHvacMode::OFF);
  EXPECT_EQ(p.modes[0].wire, heating_level::OFF);
  EXPECT_EQ(p.modes[1].hvac, HeatingHvacMode::HEAT);
  EXPECT_EQ(p.modes[1].wire, heating_level::COMFORT);

  // Exact preset set: three standard presets plus the comfort offsets and the read-only secured level.
  ASSERT_EQ(p.preset_count, 7u);
  EXPECT_EQ(p.presets[0].id, HeatingPresetId::AWAY);
  EXPECT_EQ(p.presets[0].wire, heating_level::FROST_PROTECTION);
  EXPECT_EQ(p.presets[1].id, HeatingPresetId::ECO);
  EXPECT_EQ(p.presets[2].id, HeatingPresetId::COMFORT);
  EXPECT_EQ(p.presets[3].id, HeatingPresetId::BOOST);
  EXPECT_EQ(p.presets[3].wire, heating_level::BOOST);
  EXPECT_STREQ(p.presets[4].custom_name, "Comfort -1");
  EXPECT_EQ(p.presets[4].wire, heating_level::COMFORT_MINUS_1);
  EXPECT_STREQ(p.presets[5].custom_name, "Comfort -2");
  EXPECT_EQ(p.presets[5].wire, heating_level::COMFORT_MINUS_2);
  EXPECT_STREQ(p.presets[6].custom_name, "Secured");
  EXPECT_FALSE(p.presets[6].commandable) << "the device reports secured; a controller cannot set it";
}

TEST(HeatingProfiles, EveryLevelTheProfileCanCommandIsReadBackAsItself) {
  const HeatingProfile &p = *find_heating_profile(DeviceType::ELECTRICAL_HEATER, 0);
  for (size_t i = 0; i < p.preset_count; i++) {
    const HeatingObservedState *state = find_observed_state(p, p.presets[i].wire);
    ASSERT_NE(state, nullptr) << "preset " << i;
    EXPECT_EQ(state->hvac, HeatingHvacMode::HEAT);
    EXPECT_EQ(state->preset, static_cast<int8_t>(i)) << "level 0x" << std::hex << +p.presets[i].wire;
  }
  for (size_t i = 0; i < p.mode_count; i++)
    EXPECT_NE(find_observed_state(p, p.modes[i].wire), nullptr) << "mode " << i;
  EXPECT_EQ(find_observed_state(p, heating_level::OFF)->hvac, HeatingHvacMode::OFF);
  EXPECT_EQ(find_observed_state(p, heating_level::OFF)->preset, -1);
  EXPECT_EQ(find_observed_state(p, 0x06), nullptr) << "a level nobody defined";
}

TEST(HeatingProfiles, EveryPresetIndexPointsInsideItsProfile) {
  for (const uint8_t subtype : {0, 1}) {
    const HeatingProfile &p = *find_heating_profile(DeviceType::ELECTRICAL_HEATER, subtype);
    for (size_t i = 0; i < p.observed_count; i++)
      EXPECT_LT(p.observed[i].preset, static_cast<int8_t>(p.preset_count)) << +subtype << " entry " << i;
  }
}

// === Heating level codec ===

TEST(HeatingLevels, MainParameterCarriesThePrefixAndTheLevel) {
  EXPECT_EQ(heating_level_main_parameter(heating_level::OFF), 0xFC00);
  EXPECT_EQ(heating_level_main_parameter(heating_level::FROST_PROTECTION), 0xFC01);
  EXPECT_EQ(heating_level_main_parameter(heating_level::ECO), 0xFC02);
  EXPECT_EQ(heating_level_main_parameter(heating_level::COMFORT_MINUS_2), 0xFC03);
  EXPECT_EQ(heating_level_main_parameter(heating_level::COMFORT_MINUS_1), 0xFC04);
  EXPECT_EQ(heating_level_main_parameter(heating_level::COMFORT), 0xFC05);
  EXPECT_EQ(heating_level_main_parameter(heating_level::BOOST), 0xFC07);
  EXPECT_EQ(heating_level_main_parameter(heating_level::SECURED), 0xFC3F);
}

TEST(HeatingLevels, DecodeEveryLevelIncludingSecured) {
  for (const uint8_t level :
       {heating_level::OFF, heating_level::FROST_PROTECTION, heating_level::ECO, heating_level::COMFORT_MINUS_2,
        heating_level::COMFORT_MINUS_1, heating_level::COMFORT, heating_level::BOOST, heating_level::SECURED}) {
    uint8_t decoded = 0xEE;
    ASSERT_TRUE(decode_heating_level(heating_level_main_parameter(level), decoded)) << +level;
    EXPECT_EQ(decoded, level);
  }
}

TEST(HeatingLevels, APositionIsNotALevel) {
  uint8_t level = 0xEE;
  EXPECT_FALSE(decode_heating_level(0xC800, level)) << "100 % closed";
  EXPECT_FALSE(decode_heating_level(0x0000, level));
  EXPECT_FALSE(decode_heating_level(0xD400, level)) << "unknown-position marker";
  EXPECT_EQ(level, 0xEE) << "a non-level leaves the output untouched";
}

// === Level EXECUTE frame ===

TEST(HeatingLevels, ExecuteFrameIsMainParameterOnlyWithTheLevelInIt) {
  for (const uint8_t level :
       {heating_level::OFF, heating_level::FROST_PROTECTION, heating_level::ECO, heating_level::COMFORT_MINUS_2,
        heating_level::COMFORT_MINUS_1, heating_level::COMFORT, heating_level::BOOST}) {
    IoFrame f{};
    ASSERT_TRUE(create_execute_heating_level(f, OWN, DST, false, level)) << +level;
    EXPECT_EQ(f.cmd, CMD_EXECUTE);
    ASSERT_EQ(f.data_len, 6u);
    EXPECT_EQ(f.data[2], 0xFC) << "main parameter high byte";
    EXPECT_EQ(f.data[3], level) << "main parameter low byte";
    EXPECT_EQ(f.data[4], 0x00) << "no functional parameters";
    EXPECT_EQ(f.data[5], 0x00);
    EXPECT_EQ(0, memcmp(f.dst, DST, NODE_ID_SIZE));
    EXPECT_EQ(0, memcmp(f.src, OWN, NODE_ID_SIZE));
  }
}

TEST(HeatingLevels, ExecuteFrameSharesOriginatorAndPriorityWithThePositionFrame) {
  IoFrame level{};
  IoFrame position{};
  ASSERT_TRUE(create_execute_heating_level(level, OWN, DST, false, heating_level::COMFORT));
  ASSERT_TRUE(create_execute_main_parameter(position, OWN, DST, false, 50));
  EXPECT_EQ(level.data[0], position.data[0]) << "originator";
  EXPECT_EQ(level.data[1], position.data[1]) << "ACEI";
  EXPECT_EQ(level.data_len, position.data_len);
}

TEST(HeatingLevels, ExecuteFrameHonoursTheLowPowerFlagAndRejectsAnUndefinedLevel) {
  IoFrame f{};
  ASSERT_TRUE(create_execute_heating_level(f, OWN, DST, true, heating_level::ECO));
  EXPECT_NE(f.ctrl1 & CTRL1_LOW_POWER, 0);
  EXPECT_FALSE(create_execute_heating_level(f, OWN, DST, false, HEATING_LEVEL_MAX + 1));
}

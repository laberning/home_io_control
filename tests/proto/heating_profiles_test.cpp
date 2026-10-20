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
  EXPECT_EQ(find_heating_profile(DeviceType::HEAT_PUMP, 1), nullptr)
      << "the Atlantic heat pump is a named row of its own and does not inherit the generic heat pump's profile";
  EXPECT_EQ(find_heating_profile(DeviceType::HEAT_PUMP, 2), nullptr);
  EXPECT_EQ(find_heating_profile(DeviceType::EXTERIOR_HEATING, 0), nullptr);
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

// === Temperature in the main parameter (heating temperature interface 14, heat pump 22/0) ===

TEST(HeatingTemperature, DefaultRangeIsMinus40ToPlus80AndMatchesTheDeviceRecord) {
  EXPECT_FLOAT_EQ(HEATING_TEMPERATURE_RANGE_MIN_C, -40.0F);
  EXPECT_FLOAT_EQ(HEATING_TEMPERATURE_RANGE_MAX_C, 80.0F);
  const IoDevice dev{};
  EXPECT_FLOAT_EQ(dev.temperature_range_min_c, HEATING_TEMPERATURE_RANGE_MIN_C)
      << "IoDevice and the profile layer must name the same default range";
  EXPECT_FLOAT_EQ(dev.temperature_range_max_c, HEATING_TEMPERATURE_RANGE_MAX_C);
}

TEST(HeatingTemperature, EncodeAtTheRangeEndsAndMidRange) {
  uint16_t mp = 0xEEEE;
  ASSERT_TRUE(encode_temperature_main_parameter(-40.0F, -40.0F, 80.0F, mp));
  EXPECT_EQ(mp, 0x0000);
  ASSERT_TRUE(encode_temperature_main_parameter(80.0F, -40.0F, 80.0F, mp));
  EXPECT_EQ(mp, 0xC800) << "the top of the range is the full span";
  ASSERT_TRUE(encode_temperature_main_parameter(20.0F, -40.0F, 80.0F, mp));
  EXPECT_EQ(mp, 25600) << "20 C is half way: (20 + 40) / 120 x 51200";
  ASSERT_TRUE(encode_temperature_main_parameter(0.0F, -40.0F, 80.0F, mp));
  EXPECT_EQ(mp, 17067) << "round(40 x 51200 / 120)";
}

TEST(HeatingTemperature, EncodeClampsOutsideTheRangeAndRefusesBadInput) {
  uint16_t mp = 0;
  ASSERT_TRUE(encode_temperature_main_parameter(-100.0F, -40.0F, 80.0F, mp));
  EXPECT_EQ(mp, 0x0000);
  ASSERT_TRUE(encode_temperature_main_parameter(500.0F, -40.0F, 80.0F, mp));
  EXPECT_EQ(mp, 0xC800);
  mp = 0x1234;
  EXPECT_FALSE(encode_temperature_main_parameter(NAN, -40.0F, 80.0F, mp));
  EXPECT_FALSE(encode_temperature_main_parameter(20.0F, 80.0F, -40.0F, mp)) << "an inverted range";
  EXPECT_FALSE(encode_temperature_main_parameter(20.0F, 10.0F, 10.0F, mp)) << "an empty range";
  EXPECT_EQ(mp, 0x1234) << "a refusal leaves the output untouched";
}

TEST(HeatingTemperature, DecodeIsTheInverseWithinTheStepSize) {
  for (const float celsius : {-40.0F, -10.5F, 0.0F, 5.0F, 18.5F, 20.0F, 21.5F, 30.0F, 79.99F}) {
    uint16_t mp = 0;
    ASSERT_TRUE(encode_temperature_main_parameter(celsius, -40.0F, 80.0F, mp));
    float back = 0.0F;
    ASSERT_TRUE(decode_temperature_main_parameter(mp, -40.0F, 80.0F, back));
    EXPECT_NEAR(back, celsius, 0.003F) << "one step is about 0.0023 C";
  }
  float c = 0.0F;
  ASSERT_TRUE(decode_temperature_main_parameter(0xC800, -40.0F, 80.0F, c));
  EXPECT_FLOAT_EQ(c, 80.0F);
}

TEST(HeatingTemperature, ACustomRangeChangesTheEncoding) {
  uint16_t mp = 0;
  ASSERT_TRUE(encode_temperature_main_parameter(20.0F, 10.0F, 30.0F, mp));
  EXPECT_EQ(mp, 25600) << "20 C is the middle of 10..30";
  float c = 0.0F;
  ASSERT_TRUE(decode_temperature_main_parameter(25600, 10.0F, 30.0F, c));
  EXPECT_FLOAT_EQ(c, 20.0F);
}

TEST(HeatingTemperature, AnAliasOrMarkerIsNotATemperature) {
  float c = 123.0F;
  EXPECT_FALSE(decode_temperature_main_parameter(0xD80F, -40.0F, 80.0F, c)) << "comfort alias";
  EXPECT_FALSE(decode_temperature_main_parameter(0xD400, -40.0F, 80.0F, c)) << "unknown-position marker";
  EXPECT_FALSE(decode_temperature_main_parameter(0x0000, 5.0F, 5.0F, c)) << "empty range";
  EXPECT_FLOAT_EQ(c, 123.0F);
}

TEST(HeatingTemperature, AliasMainParametersMatchTheDocumentedValues) {
  EXPECT_EQ(heating_prefixed_main_parameter(HEATING_ALIAS_MP_PREFIX, heating_alias::COMFORT), 0xD80F);
  EXPECT_EQ(heating_prefixed_main_parameter(HEATING_ALIAS_MP_PREFIX, heating_alias::THERMOSTAT_ECO), 0xD80E);
  EXPECT_EQ(heating_prefixed_main_parameter(HEATING_ALIAS_MP_PREFIX, heating_alias::SECURED), 0xD80A);
  EXPECT_EQ(heating_prefixed_main_parameter(HEATING_ALIAS_MP_PREFIX, heating_alias::HEAT_PUMP_SETBACK), 0xD80E);
  EXPECT_EQ(heating_prefixed_main_parameter(HEATING_ALIAS_MP_PREFIX, heating_alias::HEAT_PUMP_ECO), 0xD812)
      << "the heat pump's eco is not the thermostat's";
  EXPECT_EQ(heating_prefixed_main_parameter(HEATING_ALIAS_MP_PREFIX, heating_alias::HEAT_PUMP_HALTED), 0xD813);
  EXPECT_EQ(HEATING_OFF_MP, 0xD400);
}

TEST(HeatingTemperature, FunctionalParameterValuesAreTheDocumentedOnes) {
  EXPECT_EQ(heating_mode_flags::OFF, 0);
  EXPECT_EQ(heating_mode_flags::AUTO, 1);
  EXPECT_EQ(heating_mode_flags::MANUAL, 2);
  EXPECT_EQ(heating_fp16::ON, 0x4000);
  EXPECT_EQ(heating_fp16::OFF, 0x8000);
}

TEST(HeatingTemperature, ThermostatProfileSendsTheDevicesOwnCommands) {
  const HeatingProfile *p = find_heating_profile(DeviceType::HEATING_TEMPERATURE_INTERFACE, 0);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->path, HeatingControlPath::TEMPERATURE_EXECUTE);
  EXPECT_TRUE(p->features.target_temperature);
  EXPECT_FALSE(p->features.current_temperature) << "no temperature is invented";
  EXPECT_FALSE(p->features.mode_observed);
  EXPECT_FALSE(p->fixed_range) << "the thermostat's range is an assumption the YAML may override";
  EXPECT_EQ(p->mp_prefix, HEATING_ALIAS_MP_PREFIX);
  EXPECT_FLOAT_EQ(p->visual_min_c, 5.0F);
  EXPECT_FLOAT_EQ(p->visual_max_c, 30.0F);
  EXPECT_EQ(p->setpoint_fp.kind, HeatingFpKind::FP9);
  EXPECT_EQ(p->setpoint_fp.value, 2) << "a setpoint is manual";

  ASSERT_EQ(p->mode_count, 3u);
  EXPECT_EQ(p->modes[0].hvac, HeatingHvacMode::OFF);
  EXPECT_EQ(p->modes[0].main_parameter, 0xD400);
  EXPECT_EQ(p->modes[0].fp.kind, HeatingFpKind::FP9);
  EXPECT_EQ(p->modes[0].fp.value, 0) << "off: marker + FP9 0";
  EXPECT_EQ(p->modes[1].hvac, HeatingHvacMode::HEAT);
  EXPECT_EQ(p->modes[1].main_parameter, HEATING_MP_FROM_TARGET) << "heat: the target temperature + FP9 2";
  EXPECT_EQ(p->modes[1].fp.value, 2);
  EXPECT_EQ(p->modes[2].hvac, HeatingHvacMode::AUTO);
  EXPECT_EQ(p->modes[2].main_parameter, 0xD80F) << "auto: the comfort alias + FP9 1";
  EXPECT_EQ(p->modes[2].fp.value, 1);

  ASSERT_EQ(p->preset_count, 3u);
  EXPECT_EQ(p->presets[0].id, HeatingPresetId::COMFORT);
  EXPECT_EQ(p->presets[0].wire, 0x0F);
  EXPECT_EQ(p->presets[1].id, HeatingPresetId::ECO);
  EXPECT_EQ(p->presets[1].wire, 0x0E);
  EXPECT_STREQ(p->presets[2].custom_name, "Secured");
  EXPECT_EQ(p->presets[2].wire, 0x0A);
  EXPECT_TRUE(p->presets[2].commandable) << "the thermostat's secured setpoint can be selected";
  for (size_t i = 0; i < p->preset_count; i++)
    EXPECT_EQ(p->presets[i].fp.kind, HeatingFpKind::NONE) << "an alias goes alone, preset " << i;
}

TEST(HeatingTemperature, HeatPumpProfileUsesFp16AndAFixedRange) {
  const HeatingProfile *p = find_heating_profile(DeviceType::HEAT_PUMP, 0);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->path, HeatingControlPath::TEMPERATURE_EXECUTE);
  EXPECT_TRUE(p->fixed_range);
  EXPECT_EQ(p->setpoint_fp.kind, HeatingFpKind::NONE) << "a setpoint carries no functional parameter";

  ASSERT_EQ(p->mode_count, 2u);
  EXPECT_EQ(p->modes[0].hvac, HeatingHvacMode::OFF);
  EXPECT_EQ(p->modes[0].main_parameter, 0xD400);
  EXPECT_EQ(p->modes[0].fp.kind, HeatingFpKind::FP16);
  EXPECT_EQ(p->modes[0].fp.value, 0x8000) << "off: marker + FP16 0x8000";
  EXPECT_EQ(p->modes[1].hvac, HeatingHvacMode::HEAT);
  EXPECT_EQ(p->modes[1].main_parameter, 0xD80F);
  EXPECT_EQ(p->modes[1].fp.value, 0x4000) << "on: an alias + FP16 0x4000";
  for (size_t i = 0; i < p->mode_count; i++)
    EXPECT_NE(p->modes[i].hvac, HeatingHvacMode::AUTO) << "no cooling, no auto";

  ASSERT_EQ(p->preset_count, 4u);
  EXPECT_EQ(p->presets[0].id, HeatingPresetId::COMFORT);
  EXPECT_EQ(p->presets[0].wire, 0x0F);
  EXPECT_EQ(p->presets[1].id, HeatingPresetId::ECO);
  EXPECT_EQ(p->presets[1].wire, 0x12) << "D812";
  EXPECT_STREQ(p->presets[2].custom_name, "Setback");
  EXPECT_EQ(p->presets[2].wire, 0x0E);
  EXPECT_STREQ(p->presets[3].custom_name, "Halted");
  EXPECT_EQ(p->presets[3].wire, 0x13);
  for (size_t i = 0; i < p->preset_count; i++) {
    EXPECT_EQ(p->presets[i].fp.kind, HeatingFpKind::FP16) << "preset " << i;
    EXPECT_EQ(p->presets[i].fp.value, 0x4000);
  }
}

TEST(HeatingTemperature, AliasesMapBackToTheirPresets) {
  const HeatingProfile &t = *find_heating_profile(DeviceType::HEATING_TEMPERATURE_INTERFACE, 0);
  EXPECT_EQ(t.presets[find_observed_state(t, heating_alias::COMFORT)->preset].id, HeatingPresetId::COMFORT);
  EXPECT_EQ(t.presets[find_observed_state(t, heating_alias::THERMOSTAT_ECO)->preset].id, HeatingPresetId::ECO);
  EXPECT_STREQ(t.presets[find_observed_state(t, heating_alias::SECURED)->preset].custom_name, "Secured");
  EXPECT_EQ(find_observed_state(t, 0x12), nullptr) << "D812 is the heat pump's eco, not a thermostat alias";

  const HeatingProfile &h = *find_heating_profile(DeviceType::HEAT_PUMP, 0);
  EXPECT_EQ(h.presets[find_observed_state(h, 0x12)->preset].id, HeatingPresetId::ECO);
  EXPECT_STREQ(h.presets[find_observed_state(h, 0x0E)->preset].custom_name, "Setback");
  EXPECT_STREQ(h.presets[find_observed_state(h, 0x13)->preset].custom_name, "Halted");
  EXPECT_EQ(find_observed_state(h, 0x0A), nullptr);
}

TEST(HeatingTemperature, NoRegisterFrameForTheTemperatureHeaters) {
  EXPECT_FALSE(device_uses_heating_registers(DeviceType::HEATING_TEMPERATURE_INTERFACE, 0));
  EXPECT_FALSE(device_uses_heating_registers(DeviceType::HEAT_PUMP, 0));
  EXPECT_TRUE(device_uses_heating_temperature(DeviceType::HEATING_TEMPERATURE_INTERFACE, 0));
  EXPECT_TRUE(device_uses_heating_temperature(DeviceType::HEAT_PUMP, 0));
  EXPECT_FALSE(device_uses_heating_temperature(DeviceType::ELECTRICAL_HEATER, 0));
  EXPECT_TRUE(device_has_no_motion(DeviceType::HEATING_TEMPERATURE_INTERFACE, 0));
  EXPECT_TRUE(device_has_no_motion(DeviceType::HEAT_PUMP, 0));
}

TEST(HeatingTemperature, ExecuteFramesCarryTheMainParameterAndAtMostOneFunctionalParameter) {
  IoFrame plain{};
  ASSERT_TRUE(create_execute_heating_main_parameter(plain, OWN, DST, false, 0xD80F));
  EXPECT_EQ(plain.cmd, CMD_EXECUTE);
  ASSERT_EQ(plain.data_len, 6u);
  EXPECT_EQ(plain.data[2], 0xD8);
  EXPECT_EQ(plain.data[3], 0x0F);
  EXPECT_EQ(plain.data[4], 0x00);
  EXPECT_EQ(plain.data[5], 0x00);

  IoFrame fp9{};
  ASSERT_TRUE(create_execute_heating_with_mode_flags(fp9, OWN, DST, false, 25600, heating_mode_flags::MANUAL));
  ASSERT_EQ(fp9.data_len, 8u);
  EXPECT_EQ(fp9.data[2], 0x64);
  EXPECT_EQ(fp9.data[3], 0x00);
  EXPECT_EQ(fp9.data[4], 0x00) << "no FP1-FP8";
  EXPECT_EQ(fp9.data[5], 0x80) << "the first bit of the second bitmap is FP9";
  EXPECT_EQ(fp9.data[6], 0x00) << "every FP value is two bytes, high byte first";
  EXPECT_EQ(fp9.data[7], 0x02) << "FP9: manual";
  EXPECT_EQ(fp9.data[0], plain.data[0]) << "originator";
  EXPECT_EQ(fp9.data[1], plain.data[1]) << "ACEI";

  // The thermostat's mode frames: auto D80F, off D400, each with FP9 and a two-byte value.
  IoFrame auto_mode{};
  ASSERT_TRUE(create_execute_heating_with_mode_flags(auto_mode, OWN, DST, false, 0xD80F, heating_mode_flags::AUTO));
  const uint8_t expected_auto[] = {0xD8, 0x0F, 0x00, 0x80, 0x00, 0x01};
  EXPECT_EQ(0, memcmp(auto_mode.data + 2, expected_auto, sizeof(expected_auto)));
  IoFrame off_mode{};
  ASSERT_TRUE(
      create_execute_heating_with_mode_flags(off_mode, OWN, DST, false, HEATING_OFF_MP, heating_mode_flags::OFF));
  const uint8_t expected_off[] = {0xD4, 0x00, 0x00, 0x80, 0x00, 0x00};
  EXPECT_EQ(0, memcmp(off_mode.data + 2, expected_off, sizeof(expected_off)));

  IoFrame fp16{};
  ASSERT_TRUE(create_execute_heating_with_fp16(fp16, OWN, DST, false, HEATING_OFF_MP, heating_fp16::OFF));
  ASSERT_EQ(fp16.data_len, 8u);
  EXPECT_EQ(fp16.data[2], 0xD4);
  EXPECT_EQ(fp16.data[3], 0x00);
  EXPECT_EQ(fp16.data[4], 0x00) << "no FP1-FP8";
  EXPECT_EQ(fp16.data[5], 0x01) << "the last bit of the second bitmap is FP16";
  EXPECT_EQ(fp16.data[6], 0x80);
  EXPECT_EQ(fp16.data[7], 0x00) << "FP16: off";
}

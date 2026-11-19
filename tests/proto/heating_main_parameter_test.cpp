/// @file heating_main_parameter_test.cpp
/// @brief Tests for the heating-level codec, the level EXECUTE frame and the temperature in the main parameter.

#include "proto_codecs.h"
#include "test_helpers.h"
#include "heating_profiles.h"
#include "payload_frame.h"
#include "reading_store.h"
#include "proto_commands.h"
#include "proto_device_types.h"
#include "proto_heating_profiles.h"

#include <gtest/gtest.h>

#include <array>
#include <algorithm>
#include <cmath>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <vector>

using namespace esphome::home_io_control;

namespace {

const uint8_t OWN[NODE_ID_SIZE] = {0x11, 0x22, 0x33};
const uint8_t DST[NODE_ID_SIZE] = {0xAB, 0xC1, 0x23};

}  // namespace

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
    const auto decoded = decode_status_reading(
        DeviceReading::HEATING_LEVEL, test::main_parameter_bytes(heating_level_main_parameter(level)),
        HEATING_LEVEL_MP_PREFIX, HEATING_TEMPERATURE_RANGE_MIN_C, HEATING_TEMPERATURE_RANGE_MAX_C);
    ASSERT_TRUE(decoded.has_value()) << +level;
    EXPECT_EQ(*decoded, level);
  }
}

TEST(HeatingLevels, EveryLevelHasTheNameTheSensorShows) {
  const struct {
    uint8_t level;
    const char *name;
  } names[] = {
      {heating_level::OFF, "off"},
      {heating_level::FROST_PROTECTION, "frost protection"},
      {heating_level::ECO, "eco"},
      {heating_level::COMFORT_MINUS_2, "comfort -2"},
      {heating_level::COMFORT_MINUS_1, "comfort -1"},
      {heating_level::COMFORT, "comfort"},
      {heating_level::BOOST, "boost"},
      {heating_level::SECURED, "secured"},
  };
  for (const auto &entry : names)
    EXPECT_STREQ(heating_level_name(entry.level), entry.name) << +entry.level;
}

TEST(HeatingLevels, AValueThatIsNoLevelHasNoName) {
  for (const uint8_t value : {0x06, 0x08, 0x0F, 0x40, 0xFF})
    EXPECT_EQ(heating_level_name(value), nullptr) << +value;
}

TEST(HeatingLevels, APositionIsNotALevel) {
  const auto decode = [](uint8_t high, uint8_t low) {
    const std::array<uint8_t, 2> bytes = {high, low};
    return decode_status_reading(DeviceReading::HEATING_LEVEL, bytes, HEATING_LEVEL_MP_PREFIX,
                                 HEATING_TEMPERATURE_RANGE_MIN_C, HEATING_TEMPERATURE_RANGE_MAX_C);
  };
  EXPECT_FALSE(decode(0xC8, 0x00)) << "100 % closed";
  EXPECT_FALSE(decode(0x00, 0x00));
  EXPECT_FALSE(decode(0xD4, 0x00)) << "unknown-position marker";
}

// === Level EXECUTE frame ===

TEST(HeatingLevels, ExecuteFrameCarriesTheLevelAndTheBridgeBytes) {
  for (const uint8_t level :
       {heating_level::OFF, heating_level::FROST_PROTECTION, heating_level::ECO, heating_level::COMFORT_MINUS_2,
        heating_level::COMFORT_MINUS_1, heating_level::COMFORT, heating_level::BOOST}) {
    IoFrame f{};
    ASSERT_TRUE(create_execute_heating_level(f, OWN, DST, false, level)) << +level;
    EXPECT_EQ(f.cmd, CMD_EXECUTE);
    // The eight data bytes a vendor bridge sends with every level (corpus: issue #228).
    const uint8_t expected[] = {0x01, 0xE7, 0xFC, level, 0x00, 0x20, 0xFF, 0xFF};
    ASSERT_EQ(f.data_len, sizeof(expected));
    EXPECT_EQ(0, memcmp(f.data, expected, sizeof(expected))) << "level 0x" << std::hex << +level;
    EXPECT_EQ(0, memcmp(f.dst, DST, NODE_ID_SIZE));
    EXPECT_EQ(0, memcmp(f.src, OWN, NODE_ID_SIZE));
  }
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
  const HeatingProfile *p = find_heating_profile(DeviceType::HEATING_TEMPERATURE_INTERFACE, 0, DeviceModel::NONE);
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

  ASSERT_EQ(p->modes.size(), 3u);
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

  ASSERT_EQ(p->presets.size(), 3u);
  EXPECT_EQ(p->presets[0].id, HeatingPresetId::COMFORT);
  EXPECT_EQ(p->presets[0].wire, 0x0F);
  EXPECT_EQ(p->presets[1].id, HeatingPresetId::ECO);
  EXPECT_EQ(p->presets[1].wire, 0x0E);
  EXPECT_STREQ(p->presets[2].custom_name, "Secured");
  EXPECT_EQ(p->presets[2].wire, 0x0A);
  for (size_t i = 0; i < p->presets.size(); i++)
    EXPECT_EQ(p->presets[i].fp.kind, HeatingFpKind::NONE) << "an alias goes alone, preset " << i;
}

TEST(HeatingTemperature, HeatPumpProfileUsesFp16AndAFixedRange) {
  const HeatingProfile *p = find_heating_profile(DeviceType::HEAT_PUMP, 0, DeviceModel::NONE);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->path, HeatingControlPath::TEMPERATURE_EXECUTE);
  EXPECT_TRUE(p->fixed_range);
  EXPECT_EQ(p->setpoint_fp.kind, HeatingFpKind::NONE) << "a setpoint carries no functional parameter";

  ASSERT_EQ(p->modes.size(), 2u);
  EXPECT_EQ(p->modes[0].hvac, HeatingHvacMode::OFF);
  EXPECT_EQ(p->modes[0].main_parameter, 0xD400);
  EXPECT_EQ(p->modes[0].fp.kind, HeatingFpKind::FP16);
  EXPECT_EQ(p->modes[0].fp.value, 0x8000) << "off: marker + FP16 0x8000";
  EXPECT_EQ(p->modes[1].hvac, HeatingHvacMode::HEAT);
  EXPECT_EQ(p->modes[1].main_parameter, 0xD80F);
  EXPECT_EQ(p->modes[1].fp.value, 0x4000) << "on: an alias + FP16 0x4000";
  for (size_t i = 0; i < p->modes.size(); i++)
    EXPECT_NE(p->modes[i].hvac, HeatingHvacMode::AUTO) << "no cooling, no auto";

  ASSERT_EQ(p->presets.size(), 4u);
  EXPECT_EQ(p->presets[0].id, HeatingPresetId::COMFORT);
  EXPECT_EQ(p->presets[0].wire, 0x0F);
  EXPECT_EQ(p->presets[1].id, HeatingPresetId::ECO);
  EXPECT_EQ(p->presets[1].wire, 0x12) << "D812";
  EXPECT_STREQ(p->presets[2].custom_name, "Setback");
  EXPECT_EQ(p->presets[2].wire, 0x0E);
  EXPECT_STREQ(p->presets[3].custom_name, "Halted");
  EXPECT_EQ(p->presets[3].wire, 0x13);
  for (size_t i = 0; i < p->presets.size(); i++) {
    EXPECT_EQ(p->presets[i].fp.kind, HeatingFpKind::FP16) << "preset " << i;
    EXPECT_EQ(p->presets[i].fp.value, 0x4000);
  }
}

TEST(HeatingTemperature, AliasesMapBackToTheirPresets) {
  const HeatingProfile &t = test::profile_of(DeviceType::HEATING_TEMPERATURE_INTERFACE, 0, DeviceModel::NONE);
  EXPECT_EQ(t.presets[find_observed_state(t, heating_alias::COMFORT)->preset].id, HeatingPresetId::COMFORT);
  EXPECT_EQ(t.presets[find_observed_state(t, heating_alias::THERMOSTAT_ECO)->preset].id, HeatingPresetId::ECO);
  EXPECT_STREQ(t.presets[find_observed_state(t, heating_alias::SECURED)->preset].custom_name, "Secured");
  EXPECT_EQ(find_observed_state(t, 0x12), nullptr) << "D812 is the heat pump's eco, not a thermostat alias";

  const HeatingProfile &h = test::profile_of(DeviceType::HEAT_PUMP, 0, DeviceModel::NONE);
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

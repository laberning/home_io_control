/// @file heating_profiles_test.cpp
/// @brief Tests for the heating profile table, heating-level codec and the level EXECUTE frame.

#include "proto_codecs.h"
#include "test_helpers.h"
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

#include "device_table_walk.h"

using namespace esphome::home_io_control;

namespace {

const uint8_t OWN[NODE_ID_SIZE] = {0x11, 0x22, 0x33};
const uint8_t DST[NODE_ID_SIZE] = {0xAB, 0xC1, 0x23};

/// The registers a profile's poll reads, by when it reads them.
struct PinnedPoll {
  std::vector<DeviceRegister> every_poll;           ///< Read every poll; the first failed read ends the poll.
  std::vector<DeviceRegister> every_poll_may_miss;  ///< Read every poll after those; a miss keeps the value.
  std::vector<DeviceRegister> once_after_boot;      ///< Read until the heater has answered once.
};

/// @return The poll of a device of this kind with nothing asked for and nothing read yet, from whatever the table
///         selects it by.
PinnedPoll pinned_poll(DeviceType type, uint8_t subtype, DeviceModel model) {
  IoDevice dev;
  dev.model = model;
  test::set_kind(dev, type, subtype);
  const auto reads = [&](ReadingPoll policy) {
    const HeaterPollList list = device_poll_reads(dev, policy);
    return std::vector<DeviceRegister>(list.begin(), list.end());
  };
  return {reads(ReadingPoll::EVERY_POLL), reads(ReadingPoll::EVERY_POLL_MAY_MISS), reads(ReadingPoll::ONCE_AFTER_BOOT)};
}

}  // namespace

// === Lookup ===

TEST(HeatingProfiles, EachElectricalHeaterKindHasItsOwnControlPath) {
  const HeatingProfile *setpoint = find_heating_profile(DeviceType::ELECTRICAL_HEATER, 1, DeviceModel::NONE);
  ASSERT_NE(setpoint, nullptr);
  EXPECT_EQ(setpoint->path, HeatingControlPath::PRIVATE_REGISTERS);

  const HeatingProfile *plain = find_heating_profile(DeviceType::ELECTRICAL_HEATER, 0, DeviceModel::NONE);
  ASSERT_NE(plain, nullptr);
  EXPECT_EQ(plain->path, HeatingControlPath::HEATING_LEVEL);

  EXPECT_EQ(find_heating_profile(DeviceType::ELECTRICAL_HEATER, 9, DeviceModel::NONE), plain)
      << "an unnamed subtype falls back to subtype 0";
  EXPECT_EQ(find_heating_profile(DeviceType::HEAT_PUMP, 1, DeviceModel::NONE), nullptr)
      << "the Atlantic heat pump is a named row of its own and does not inherit the generic heat pump's profile";
  EXPECT_EQ(find_heating_profile(DeviceType::HEAT_PUMP, 2, DeviceModel::NONE), nullptr);
  EXPECT_EQ(find_heating_profile(DeviceType::EXTERIOR_HEATING, 0, DeviceModel::NONE), nullptr);
  EXPECT_EQ(find_heating_profile(DeviceType::ROLLER_SHUTTER, 0, DeviceModel::NONE), nullptr);
  EXPECT_EQ(find_heating_profile(DeviceType::UNKNOWN, 0, DeviceModel::NONE), nullptr);
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
      EXPECT_EQ(find_heating_profile(type, subtype, DeviceModel::NONE) != nullptr, table_says_climate_platform)
          << static_cast<unsigned>(type) << "/" << +subtype;
      EXPECT_EQ(device_supports_climate_control(type, subtype), table_says_climate_platform);
    }
  }
}

TEST(HeatingProfiles, EveryProfileNamesARowOfTheTypeTable) {
  // find_heating_profile() falls back differently from the type table for a named subtype, so the two
  // lookups stay apart; this keeps a profile from being keyed to a device the table does not know.
  ASSERT_FALSE(heating_profiles().empty());
  for (const HeatingProfile &profile : heating_profiles()) {
    const DeviceTypeInfo *row = profile.model == DeviceModel::NONE
                                    ? find_exact_device_type_info(profile.type, profile.subtype)
                                    : find_device_model_info(profile.type, profile.subtype, profile.model);
    EXPECT_NE(row, nullptr) << format_device_ids(profile.type, profile.subtype) << " model "
                            << static_cast<unsigned>(profile.model);
  }
}

TEST(HeatingProfiles, TheTemperatureRangeCapabilityMarksTheProfilesThatTakeATemperatureWithoutAFixedRange) {
  // The YAML `min_temperature:` / `max_temperature:` keys are accepted by this capability, so it must be set
  // exactly where the climate platform uses the configured range (platform_climate.cpp reads
  // `fixed_range`).
  const auto check = [](const DeviceTypeInfo &row) {
    const HeatingProfile *profile = find_heating_profile(row.type, row.subtype, row.model);
    const bool takes_range = profile != nullptr && profile->path == HeatingControlPath::TEMPERATURE_EXECUTE &&
                             profile->features.target_temperature && !profile->fixed_range;
    EXPECT_EQ(has_feature(row.features, device_feature::TEMPERATURE_RANGE), takes_range)
        << format_device_ids(row.type, row.subtype);
  };
  test::for_each_table_row(check);
  test::for_each_model_row(check);
  EXPECT_TRUE(has_feature(device_features(DeviceType::HEATING_TEMPERATURE_INTERFACE, 0, DeviceModel::NONE),
                          device_feature::TEMPERATURE_RANGE));
  EXPECT_FALSE(
      has_feature(device_features(DeviceType::HEAT_PUMP, 0, DeviceModel::NONE), device_feature::TEMPERATURE_RANGE));
}

// === Profile contents ===

TEST(HeatingProfiles, SetpointHeaterOffersTheCardItsRegistersCanDrive) {
  const HeatingProfile &p = *find_heating_profile(DeviceType::ELECTRICAL_HEATER, 1, DeviceModel::NONE);
  EXPECT_TRUE(p.features.target_temperature);
  EXPECT_TRUE(p.features.current_temperature);
  EXPECT_TRUE(p.features.action);
  const auto polled = pinned_poll(DeviceType::ELECTRICAL_HEATER, 1, DeviceModel::NONE).every_poll;
  EXPECT_TRUE(std::find(polled.begin(), polled.end(), DeviceRegister::WINDOW_OPEN) != polled.end())
      << "the window sensor is polled with every read";

  ASSERT_EQ(p.modes.size(), 3u);
  EXPECT_EQ(p.modes[0].hvac, HeatingHvacMode::OFF);
  EXPECT_EQ(p.modes[0].wire, 4);
  EXPECT_EQ(p.modes[1].hvac, HeatingHvacMode::HEAT);
  EXPECT_EQ(p.modes[1].wire, 1);
  EXPECT_EQ(p.modes[2].hvac, HeatingHvacMode::AUTO);
  EXPECT_EQ(p.modes[2].wire, 0);

  // Both automatic modes are custom presets that write the mode register.
  ASSERT_EQ(p.presets.size(), 2u);
  EXPECT_EQ(p.presets[0].id, HeatingPresetId::CUSTOM);
  EXPECT_STREQ(p.presets[0].custom_name, "Program");
  EXPECT_EQ(p.preset_fn, DeviceRegister::OPERATING_MODE);
  EXPECT_EQ(p.presets[0].wire, 2);
  EXPECT_EQ(p.presets[1].id, HeatingPresetId::CUSTOM);
  EXPECT_STREQ(p.presets[1].custom_name, "Auto");
  EXPECT_EQ(p.presets[1].wire, 0);
}

TEST(HeatingProfiles, SetpointHeaterModeRegisterMapsBackToTheCard) {
  const HeatingProfile &p = *find_heating_profile(DeviceType::ELECTRICAL_HEATER, 1, DeviceModel::NONE);
  const auto *auto_mode = find_observed_state(p, 0);
  ASSERT_NE(auto_mode, nullptr);
  EXPECT_EQ(auto_mode->hvac, HeatingHvacMode::AUTO);
  ASSERT_GE(auto_mode->preset, 0);
  EXPECT_STREQ(p.presets[auto_mode->preset].custom_name, "Auto");
  EXPECT_EQ(find_observed_state(p, 1)->hvac, HeatingHvacMode::HEAT);
  const auto *program = find_observed_state(p, 2);
  ASSERT_NE(program, nullptr);
  EXPECT_EQ(program->hvac, HeatingHvacMode::AUTO) << "AUTO and PROG both show as HVAC auto";
  ASSERT_GE(program->preset, 0);
  EXPECT_STREQ(p.presets[program->preset].custom_name, "Program");
  EXPECT_EQ(find_observed_state(p, 3)->hvac, HeatingHvacMode::HEAT) << "external is shown as heat";
  EXPECT_EQ(find_observed_state(p, 4)->hvac, HeatingHvacMode::OFF);
  EXPECT_EQ(find_observed_state(p, 5), nullptr);
}

TEST(HeatingProfiles, PlainHeaterOffersLevelsAsModesAndPresets) {
  const HeatingProfile &p = *find_heating_profile(DeviceType::ELECTRICAL_HEATER, 0, DeviceModel::NONE);
  EXPECT_FALSE(p.features.target_temperature);
  EXPECT_FALSE(p.features.current_temperature);
  EXPECT_FALSE(p.features.action);
  EXPECT_TRUE(pinned_poll(DeviceType::ELECTRICAL_HEATER, 0, DeviceModel::NONE).every_poll.empty())
      << "a level heater is polled with the status read";

  ASSERT_EQ(p.modes.size(), 2u);
  EXPECT_EQ(p.modes[0].hvac, HeatingHvacMode::OFF);
  EXPECT_EQ(p.modes[0].wire, heating_level::OFF);
  EXPECT_EQ(p.modes[1].hvac, HeatingHvacMode::HEAT);
  EXPECT_EQ(p.modes[1].wire, heating_level::COMFORT);

  // Exact preset set: the standard presets and the comfort offsets; the secured level is reported, never a preset.
  ASSERT_EQ(p.presets.size(), 6u);
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
}

TEST(HeatingProfiles, TheSecuredLevelIsReadAsHeatWithNoPreset) {
  for (const DeviceModel model : {DeviceModel::NONE, DeviceModel::PILOT_WIRE_INTERFACE}) {
    const HeatingProfile &p = *find_heating_profile(DeviceType::ELECTRICAL_HEATER, 0, model);
    const HeatingObservedState *secured = find_observed_state(p, heating_level::SECURED);
    ASSERT_NE(secured, nullptr);
    EXPECT_EQ(secured->hvac, HeatingHvacMode::HEAT);
    EXPECT_EQ(secured->preset, -1) << "no preset: the heater reports it and a controller cannot set it";
  }
}

TEST(HeatingProfiles, EveryLevelTheProfileCanCommandIsReadBackAsItself) {
  const HeatingProfile &p = *find_heating_profile(DeviceType::ELECTRICAL_HEATER, 0, DeviceModel::NONE);
  for (size_t i = 0; i < p.presets.size(); i++) {
    const HeatingObservedState *state = find_observed_state(p, p.presets[i].wire);
    ASSERT_NE(state, nullptr) << "preset " << i;
    EXPECT_EQ(state->hvac, HeatingHvacMode::HEAT);
    EXPECT_EQ(state->preset, static_cast<int8_t>(i)) << "level 0x" << std::hex << +p.presets[i].wire;
  }
  for (size_t i = 0; i < p.modes.size(); i++)
    EXPECT_NE(find_observed_state(p, p.modes[i].wire), nullptr) << "mode " << i;
  EXPECT_EQ(find_observed_state(p, heating_level::OFF)->hvac, HeatingHvacMode::OFF);
  EXPECT_EQ(find_observed_state(p, heating_level::OFF)->preset, -1);
  EXPECT_EQ(find_observed_state(p, 0x06), nullptr) << "a level nobody defined";
}

TEST(HeatingProfiles, ObservedTablesAndPresetsAgreeInEveryProfile) {
  const struct {
    DeviceType type;
    uint8_t subtype;
  } profiles[] = {{DeviceType::ELECTRICAL_HEATER, 0},
                  {DeviceType::ELECTRICAL_HEATER, 1},
                  {DeviceType::HEATING_TEMPERATURE_INTERFACE, 0},
                  {DeviceType::HEAT_PUMP, 0}};
  for (const auto &kind : profiles) {
    const std::string where = std::to_string(static_cast<unsigned>(kind.type)) + "/" + std::to_string(+kind.subtype);
    const HeatingProfile *p = find_heating_profile(kind.type, kind.subtype, DeviceModel::NONE);
    ASSERT_NE(p, nullptr) << where;
    for (size_t r = 0; r < p->observed.size(); r++) {
      const HeatingObservedState &row = p->observed[r];
      ASSERT_LT(row.preset, static_cast<int8_t>(p->presets.size())) << where << " row " << r;
      if (row.preset >= 0) {
        EXPECT_EQ(p->presets[row.preset].wire, row.wire) << where << " row " << r << ": names a preset of its wire";
      }
    }
    // A commanded preset is shown through the observed table, so each one needs exactly one row naming it.
    for (size_t i = 0; i < p->presets.size(); i++) {
      size_t rows = 0;
      for (size_t r = 0; r < p->observed.size(); r++) {
        if (p->observed[r].wire == p->presets[i].wire) {
          rows++;
          EXPECT_EQ(p->observed[r].preset, static_cast<int8_t>(i)) << where << " preset " << i << " row " << r;
        }
      }
      EXPECT_EQ(rows, 1u) << where << " preset " << i;
    }
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

namespace {
/// The two bytes of a heating level's main parameter as a status reply carries them.
std::array<uint8_t, 2> main_parameter_bytes(uint8_t level) {
  const uint16_t mp = heating_level_main_parameter(level);
  return {static_cast<uint8_t>(mp >> 8), static_cast<uint8_t>(mp & 0xFF)};
}
}  // namespace

TEST(HeatingLevels, DecodeEveryLevelIncludingSecured) {
  for (const uint8_t level :
       {heating_level::OFF, heating_level::FROST_PROTECTION, heating_level::ECO, heating_level::COMFORT_MINUS_2,
        heating_level::COMFORT_MINUS_1, heating_level::COMFORT, heating_level::BOOST, heating_level::SECURED}) {
    const auto decoded =
        decode_status_reading(DeviceReading::HEATING_LEVEL, main_parameter_bytes(level), HEATING_LEVEL_MP_PREFIX,
                              HEATING_TEMPERATURE_RANGE_MIN_C, HEATING_TEMPERATURE_RANGE_MAX_C);
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

TEST(HeatingTemperature, EncodeRefusesOutsideTheRangeAndBadInput) {
  uint16_t mp = 0x1234;
  EXPECT_FALSE(encode_temperature_main_parameter(-100.0F, -40.0F, 80.0F, mp)) << "never clamped to the end";
  EXPECT_FALSE(encode_temperature_main_parameter(500.0F, -40.0F, 80.0F, mp));
  EXPECT_FALSE(encode_temperature_main_parameter(80.1F, -40.0F, 80.0F, mp));
  EXPECT_EQ(mp, 0x1234);
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
  const HeatingProfile &t = *find_heating_profile(DeviceType::HEATING_TEMPERATURE_INTERFACE, 0, DeviceModel::NONE);
  EXPECT_EQ(t.presets[find_observed_state(t, heating_alias::COMFORT)->preset].id, HeatingPresetId::COMFORT);
  EXPECT_EQ(t.presets[find_observed_state(t, heating_alias::THERMOSTAT_ECO)->preset].id, HeatingPresetId::ECO);
  EXPECT_STREQ(t.presets[find_observed_state(t, heating_alias::SECURED)->preset].custom_name, "Secured");
  EXPECT_EQ(find_observed_state(t, 0x12), nullptr) << "D812 is the heat pump's eco, not a thermostat alias";

  const HeatingProfile &h = *find_heating_profile(DeviceType::HEAT_PUMP, 0, DeviceModel::NONE);
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

// === Register-read probe gate ===

TEST(HeatingProfiles, RegisterProbeIsAllowedOnlyForRegisterHeatersAndHotWater) {
  EXPECT_TRUE(device_accepts_register_probe(DeviceType::ELECTRICAL_HEATER, 1)) << "register-protocol heater";
  EXPECT_TRUE(device_accepts_register_probe(DeviceType::DOMESTIC_HOT_WATER, 0));
  EXPECT_TRUE(device_accepts_register_probe(DeviceType::DOMESTIC_HOT_WATER, 58)) << "any subtype of the type";

  EXPECT_FALSE(device_accepts_register_probe(DeviceType::ELECTRICAL_HEATER, 0)) << "takes the standard EXECUTE";
  EXPECT_FALSE(device_accepts_register_probe(DeviceType::ROLLER_SHUTTER, 0));
  EXPECT_FALSE(device_accepts_register_probe(DeviceType::VERTICAL_EXTERIOR_AWNING, 0));
  EXPECT_FALSE(device_accepts_register_probe(DeviceType::LIGHT, 0));
  EXPECT_FALSE(device_accepts_register_probe(DeviceType::LOCK, 0));
  EXPECT_FALSE(device_accepts_register_probe(DeviceType::ON_OFF_SWITCH, 0));
  EXPECT_FALSE(device_accepts_register_probe(DeviceType::UNKNOWN, 0));
}

// === Towel dryer (52/1, model TOWEL_DRYER) ===

namespace {

bool lists(std::span<const DeviceRegister> list, DeviceRegister fn) {
  return std::find(list.begin(), list.end(), fn) != list.end();
}

}  // namespace

TEST(HeatingProfilesTowelDryer, TheModelSelectsItsOwnProfileAndNoModelKeepsTheRadiator) {
  const HeatingProfile *radiator = find_heating_profile(DeviceType::ELECTRICAL_HEATER, 1, DeviceModel::NONE);
  const HeatingProfile *towel = find_heating_profile(DeviceType::ELECTRICAL_HEATER, 1, DeviceModel::TOWEL_DRYER);
  ASSERT_NE(radiator, nullptr);
  ASSERT_NE(towel, nullptr);
  EXPECT_NE(towel, radiator);
  EXPECT_EQ(radiator->model, DeviceModel::NONE);
  EXPECT_EQ(towel->model, DeviceModel::TOWEL_DRYER);
  EXPECT_EQ(find_heating_profile(DeviceType::ELECTRICAL_HEATER, 0, DeviceModel::TOWEL_DRYER),
            find_heating_profile(DeviceType::ELECTRICAL_HEATER, 0, DeviceModel::NONE))
      << "a model the (type, subtype) does not offer falls back to its profile";

  IoDevice dev{};
  dev.type = DeviceType::ELECTRICAL_HEATER;
  dev.subtype = 1;
  EXPECT_EQ(find_heating_profile(dev), radiator);
  dev.model = DeviceModel::TOWEL_DRYER;
  EXPECT_EQ(find_heating_profile(dev), towel) << "the device overload passes the declared model";
}

TEST(HeatingProfilesTowelDryer, AModelProfileKeepsTheControlPathOfTheProfileAboveIt) {
  // The path predicates (device_uses_heating_registers() and friends) take no model: this is why.
  for (const auto model :
       {DeviceModel::TOWEL_DRYER, DeviceModel::HEATER_WITH_LIGHT, DeviceModel::PILOT_WIRE_INTERFACE}) {
    for (unsigned type = 0; type <= 0x3FF; type++) {
      for (unsigned subtype = 0; subtype <= 0x3F; subtype++) {
        const auto t = static_cast<DeviceType>(type);
        const auto s = static_cast<uint8_t>(subtype);
        const HeatingProfile *with = find_heating_profile(t, s, model);
        const HeatingProfile *without = find_heating_profile(t, s, DeviceModel::NONE);
        EXPECT_EQ(with != nullptr, without != nullptr) << format_device_ids(t, s);
        if (with != nullptr && without != nullptr) {
          EXPECT_EQ(with->path, without->path) << format_device_ids(t, s);
        }
      }
    }
  }
}

TEST(HeatingProfilesTowelDryer, ModesMapToTheTowelModeRegisterInBothDirections) {
  const HeatingProfile &p = *find_heating_profile(DeviceType::ELECTRICAL_HEATER, 1, DeviceModel::TOWEL_DRYER);
  EXPECT_EQ(p.path, HeatingControlPath::PRIVATE_REGISTERS);
  EXPECT_EQ(p.mode_fn, DeviceRegister::TOWEL_MODE);
  ASSERT_EQ(p.modes.size(), 3u);
  const struct {
    HeatingHvacMode hvac;
    TowelDryerMode wire;
  } expected[] = {{HeatingHvacMode::OFF, TowelDryerMode::OFF},
                  {HeatingHvacMode::HEAT, TowelDryerMode::MANUAL},
                  {HeatingHvacMode::AUTO, TowelDryerMode::PROG}};
  for (size_t i = 0; i < 3; i++) {
    EXPECT_EQ(p.modes[i].hvac, expected[i].hvac) << i;
    EXPECT_EQ(p.modes[i].wire, static_cast<uint8_t>(expected[i].wire)) << i;
    const HeatingObservedState *seen = find_observed_state(p, p.modes[i].wire);
    ASSERT_NE(seen, nullptr) << i;
    EXPECT_EQ(seen->hvac, expected[i].hvac) << "what is written reads back as the same mode";
    EXPECT_EQ(seen->preset, -1) << "the mode register never names a preset";
  }
  EXPECT_EQ(find_observed_state(p, static_cast<uint8_t>(TowelDryerMode::AUTO)), nullptr)
      << "value 1 has never been seen";
}

TEST(HeatingProfilesTowelDryer, PresetsWriteTheTemporaryStateAndReadBackAsThemselves) {
  const HeatingProfile &p = *find_heating_profile(DeviceType::ELECTRICAL_HEATER, 1, DeviceModel::TOWEL_DRYER);
  EXPECT_EQ(p.preset_fn, DeviceRegister::TEMPORARY_STATE);
  ASSERT_FALSE(p.preset_observed.empty());
  ASSERT_EQ(p.presets.size(), 3u) << "none, boost and drying: comfort and eco are no presets";
  EXPECT_EQ(p.presets[0].id, HeatingPresetId::NONE);
  EXPECT_EQ(p.presets[1].id, HeatingPresetId::BOOST);
  EXPECT_EQ(p.presets[2].id, HeatingPresetId::CUSTOM);
  EXPECT_STREQ(p.presets[2].custom_name, "Drying");
  // The timed runs are commanded and read back through the temporary state.
  for (size_t i = 0; i < 3; i++) {
    const HeatingObservedState *seen = find_observed_preset(p, p.presets[i].wire);
    ASSERT_NE(seen, nullptr) << i;
    EXPECT_EQ(seen->preset, static_cast<int8_t>(i)) << "a written preset reads back as itself";
  }
  EXPECT_EQ(p.presets[1].wire, static_cast<uint8_t>(HeatingTemporaryState::BOOST));
  EXPECT_EQ(p.presets[2].wire, static_cast<uint8_t>(HeatingTemporaryState::DRYING));
  EXPECT_EQ(find_observed_preset(p, 3), nullptr);
}

TEST(HeatingProfilesTowelDryer, PollsItsOwnRegistersAndNoWindow) {
  const HeatingProfile &p = *find_heating_profile(DeviceType::ELECTRICAL_HEATER, 1, DeviceModel::TOWEL_DRYER);
  const PinnedPoll poll = pinned_poll(DeviceType::ELECTRICAL_HEATER, 1, DeviceModel::TOWEL_DRYER);
  EXPECT_EQ(poll.every_poll,
            (std::vector<DeviceRegister>{DeviceRegister::ROOM_TEMPERATURE, DeviceRegister::COMFORT_SETPOINT,
                                         DeviceRegister::TOWEL_MODE, DeviceRegister::TEMPORARY_STATE,
                                         DeviceRegister::WORKING_RATE}));
  EXPECT_TRUE(lists(poll.once_after_boot, DeviceRegister::MAX_SETPOINT));
  for (const DeviceRegister never : {DeviceRegister::WINDOW_OPEN, DeviceRegister::OPEN_WINDOW_DETECTION,
                                     DeviceRegister::OPERATING_MODE, DeviceRegister::REGULATOR_STATE}) {
    EXPECT_FALSE(lists(poll.every_poll, never)) << device_register_name(never);
    EXPECT_FALSE(lists(poll.once_after_boot, never)) << device_register_name(never);
    EXPECT_FALSE(lists(poll.every_poll_may_miss, never)) << device_register_name(never);
  }
  EXPECT_EQ(p.action_source, HeatingActionSource::WORKING_RATE);
  EXPECT_FALSE(p.experimental_writes);
}

TEST(HeatingProfilesTowelDryer, PollsTheMaximumAtBootAndNoSetback) {
  const PinnedPoll poll = pinned_poll(DeviceType::ELECTRICAL_HEATER, 1, DeviceModel::TOWEL_DRYER);
  EXPECT_EQ(poll.once_after_boot, (std::vector<DeviceRegister>{DeviceRegister::MAX_SETPOINT}));
  EXPECT_FALSE(lists(poll.every_poll, DeviceRegister::ECO_SETBACK)) << "the level comes from the status read";
  EXPECT_FALSE(lists(poll.every_poll, DeviceRegister::TOWEL_TEMPERATURE_IN_FORCE))
      << "the temperature in force is read only while the programme runs, not on every poll";
}

TEST(HeatingProfilesTowelDryer, TheProgrammePeriodNamesTheRegistersOfTheProgramme) {
  const HeatingProfile &p = *find_heating_profile(DeviceType::ELECTRICAL_HEATER, 1, DeviceModel::TOWEL_DRYER);
  ASSERT_NE(p.programme_period, nullptr);
  const HeatingProgrammePeriod &period = *p.programme_period;
  EXPECT_EQ(period.idle_preset_state, static_cast<uint8_t>(HeatingTemporaryState::NONE));
  EXPECT_EQ(period.in_force_fn, DeviceRegister::TOWEL_TEMPERATURE_IN_FORCE);
  EXPECT_EQ(period.derogation_fn, DeviceRegister::DEROGATED_SETPOINT) << "a target set in auto is written here";
  EXPECT_TRUE(period.reports_level) << "the dryer answers the status read with its level";
  for (const HeatingPresetEntry &preset : p.presets) {
    EXPECT_NE(preset.id, HeatingPresetId::COMFORT) << "the period is shown by the level sensor, not a preset";
    EXPECT_NE(preset.id, HeatingPresetId::ECO);
  }
}

namespace {
const HeatingProfile &towel();
}  // namespace

TEST(HeatingProfilesProgramme, TheRadiatorFamilyNamesItsOwnRegistersAndTheModelsInheritThem) {
  const HeatingProfile *radiator = find_heating_profile(DeviceType::ELECTRICAL_HEATER, 1, DeviceModel::NONE);
  ASSERT_NE(radiator->programme_period, nullptr);
  EXPECT_EQ(radiator->programme_period->in_force_fn, DeviceRegister::TEMPERATURE_IN_FORCE);
  EXPECT_EQ(radiator->programme_period->derogation_fn, DeviceRegister::DEROGATED_SETPOINT);
  EXPECT_FALSE(radiator->programme_period->reports_level)
      << "no radiator has answered the status read with a level, so none is asked";
  EXPECT_EQ(find_heating_profile(DeviceType::ELECTRICAL_HEATER, 1, DeviceModel::HEATER_WITH_LIGHT)->programme_period,
            radiator->programme_period);
  EXPECT_NE(radiator->programme_period, towel().programme_period);
}

TEST(HeatingProfilesProgramme, ProfilesWithoutAProgrammeHaveNoPeriod) {
  EXPECT_EQ(find_heating_profile(DeviceType::ELECTRICAL_HEATER, 0, DeviceModel::NONE)->programme_period, nullptr);
  EXPECT_EQ(find_heating_profile(DeviceType::ELECTRICAL_HEATER, 0, DeviceModel::PILOT_WIRE_INTERFACE)->programme_period,
            nullptr);
  EXPECT_EQ(find_heating_profile(DeviceType::HEAT_PUMP, 0, DeviceModel::NONE)->programme_period, nullptr);
  EXPECT_EQ(find_heating_profile(DeviceType::HEATING_TEMPERATURE_INTERFACE, 0, DeviceModel::NONE)->programme_period,
            nullptr);
}

namespace {

/// A towel dryer's readings in the state of the display: mode, temporary state, comfort 20.0.
IoDevice towel_readings(TowelDryerMode mode, HeatingTemporaryState state, float in_force_c) {
  IoDevice r;
  test::store_reading(r, DeviceReading::TOWEL_MODE, static_cast<int16_t>(mode));
  test::store_reading(r, DeviceReading::TEMPORARY_STATE, static_cast<int16_t>(state));
  test::store_reading(r, DeviceReading::SETPOINT, 20.0F);
  test::store_reading(r, DeviceReading::TOWEL_TEMPERATURE_IN_FORCE, in_force_c);
  return r;
}

const HeatingProfile &towel() {
  return *find_heating_profile(DeviceType::ELECTRICAL_HEATER, 1, DeviceModel::TOWEL_DRYER);
}

}  // namespace

TEST(HeatingProfilesTowelDryer, TheTemperatureInForceIsTheProgrammesTarget) {
  EXPECT_FLOAT_EQ(
      programme_temperature_in_force(towel(), towel_readings(TowelDryerMode::PROG, HeatingTemporaryState::NONE, 20.0F)),
      20.0F);
  EXPECT_FLOAT_EQ(
      programme_temperature_in_force(towel(), towel_readings(TowelDryerMode::PROG, HeatingTemporaryState::NONE, 16.5F)),
      16.5F)
      << "an eco period: whatever the heater holds, with no setback arithmetic";
  EXPECT_FLOAT_EQ(
      programme_temperature_in_force(towel(), towel_readings(TowelDryerMode::PROG, HeatingTemporaryState::NONE, 7.0F)),
      7.0F);
}

TEST(HeatingProfilesTowelDryer, WithoutTheTemperatureInForceThereIsNoTargetToShow) {
  IoDevice r = towel_readings(TowelDryerMode::PROG, HeatingTemporaryState::NONE, 20.0F);
  test::clear_reading(r, DeviceRegister::TOWEL_TEMPERATURE_IN_FORCE);
  EXPECT_TRUE(std::isnan(programme_temperature_in_force(towel(), r))) << "the programme runs, it is just not read yet";
}

// --- the programme runs while the card shows auto -------------------------------------------

namespace {

/// A radiator's readings: the mode register and the temperature in force.
IoDevice radiator_readings(HeatingMode mode, float in_force_c) {
  IoDevice r;
  test::store_reading(r, DeviceReading::MODE, static_cast<int16_t>(mode));
  test::store_reading(r, DeviceReading::SETPOINT, 20.0F);
  test::store_reading(r, DeviceReading::TEMPERATURE_IN_FORCE, in_force_c);
  return r;
}

const HeatingProfile &radiator_profile() {
  return *find_heating_profile(DeviceType::ELECTRICAL_HEATER, 1, DeviceModel::NONE);
}

}  // namespace

TEST(HeatingProfilesProgramme, ARadiatorRunsItsProgrammeInProgAndInAutoButNotInManualOffOrExternal) {
  for (const HeatingMode mode : {HeatingMode::PROG, HeatingMode::AUTO}) {
    const IoDevice r = radiator_readings(mode, 17.5F);
    EXPECT_TRUE(programme_period_active(radiator_profile(), r)) << static_cast<int>(mode);
    EXPECT_FLOAT_EQ(programme_temperature_in_force(radiator_profile(), r), 17.5F) << static_cast<int>(mode);
  }
  for (const HeatingMode mode : {HeatingMode::MANUAL, HeatingMode::OFF, HeatingMode::EXTERNAL_SETPOINT}) {
    const IoDevice r = radiator_readings(mode, 17.5F);
    EXPECT_FALSE(programme_period_active(radiator_profile(), r)) << static_cast<int>(mode);
    EXPECT_TRUE(std::isnan(programme_temperature_in_force(radiator_profile(), r))) << static_cast<int>(mode);
  }
  IoDevice unread;
  EXPECT_FALSE(programme_period_active(radiator_profile(), unread)) << "an unread mode is not the programme";
}

TEST(HeatingProfilesProgramme, ARadiatorsModeRegisterValueIsNotMistakenForATimedRun) {
  // The radiator has no register for timed runs; its preset register defaults to the mode register, whose
  // AUTO value 0 equals the idle value a towel dryer's temporary state reads.
  EXPECT_TRUE(programme_period_active(radiator_profile(), radiator_readings(HeatingMode::PROG, 17.5F)));
}

TEST(HeatingProfilesTowelDryer, TheProgrammeIsActiveOnlyInProgWithNoTimedRunOnTop) {
  const HeatingProfile &p = towel();
  const struct {
    TowelDryerMode mode;
    HeatingTemporaryState state;
    bool active;
  } cases[] = {
      {TowelDryerMode::PROG, HeatingTemporaryState::NONE, true},
      {TowelDryerMode::PROG, HeatingTemporaryState::BOOST, false},
      {TowelDryerMode::PROG, HeatingTemporaryState::DRYING, false},
      {TowelDryerMode::MANUAL, HeatingTemporaryState::NONE, false},
      {TowelDryerMode::OFF, HeatingTemporaryState::NONE, false},
      {TowelDryerMode::AUTO, HeatingTemporaryState::NONE, false},
  };
  for (const auto &c : cases) {
    // 0x0137 reads 1 in boost and 2 in drying: a value that must never be taken for a temperature.
    const IoDevice r = towel_readings(c.mode, c.state, 0.1F);
    EXPECT_EQ(programme_period_active(p, r), c.active)
        << "mode " << static_cast<int>(c.mode) << " state " << static_cast<int>(c.state);
    if (!c.active) {
      EXPECT_TRUE(std::isnan(programme_temperature_in_force(p, r)))
          << "no temperature is reported outside the programme";
    }
  }
  IoDevice unread;
  EXPECT_FALSE(programme_period_active(p, unread)) << "an unread mode is not the programme";
  EXPECT_FALSE(programme_period_active(*find_heating_profile(DeviceType::ELECTRICAL_HEATER, 0, DeviceModel::NONE),
                                       towel_readings(TowelDryerMode::PROG, HeatingTemporaryState::NONE, 20.0F)))
      << "a profile without a programme period has none";
}

TEST(HeatingProfilesTowelDryer, TheTemperatureInForceIsDroppedWhenTheProgrammeStopsAndKeptWhileItRuns) {
  const HeatingProfile &p = towel();
  IoDevice r = towel_readings(TowelDryerMode::PROG, HeatingTemporaryState::NONE, 20.0F);
  clear_inactive_programme_reading(p, r);
  EXPECT_TRUE(test::has_reading(r, DeviceRegister::TOWEL_TEMPERATURE_IN_FORCE)) << "kept while the programme runs";

  test::store_reading(r, DeviceReading::TEMPORARY_STATE, static_cast<int16_t>(HeatingTemporaryState::BOOST));
  clear_inactive_programme_reading(p, r);
  EXPECT_FALSE(test::has_reading(r, DeviceRegister::TOWEL_TEMPERATURE_IN_FORCE)) << "dropped in boost";
  EXPECT_TRUE(test::has_reading(r, DeviceRegister::COMFORT_SETPOINT)) << "the comfort setpoint is not part of it";

  // Back in the programme the old value must not come back.
  test::store_reading(r, DeviceReading::TEMPORARY_STATE, static_cast<int16_t>(HeatingTemporaryState::NONE));
  EXPECT_TRUE(std::isnan(programme_temperature_in_force(p, r)));

  IoDevice plain;
  test::store_reading(plain, DeviceReading::TOWEL_TEMPERATURE_IN_FORCE, 5.0F);
  clear_inactive_programme_reading(*find_heating_profile(DeviceType::ELECTRICAL_HEATER, 0, DeviceModel::NONE), plain);
  EXPECT_TRUE(test::has_reading(plain, DeviceRegister::TOWEL_TEMPERATURE_IN_FORCE))
      << "a profile without a period never touches it";

  IoDevice manual = radiator_readings(HeatingMode::MANUAL, 17.5F);
  clear_inactive_programme_reading(radiator_profile(), manual);
  EXPECT_FALSE(test::has_reading(manual, DeviceRegister::TEMPERATURE_IN_FORCE)) << "a radiator in heat holds none";
}

TEST(HeatingProfilesTowelDryer, TheRadiatorProfileIsUnchangedAndNeverTouchesTheTowelRegisters) {
  const HeatingProfile &p = *find_heating_profile(DeviceType::ELECTRICAL_HEATER, 1, DeviceModel::NONE);
  EXPECT_EQ(p.mode_fn, DeviceRegister::OPERATING_MODE);
  EXPECT_TRUE(p.preset_observed.empty());
  EXPECT_EQ(p.preset_observed.size(), 0u);
  EXPECT_EQ(p.action_source, HeatingActionSource::REGULATOR);
  EXPECT_FALSE(p.experimental_writes);
  const PinnedPoll radiator_poll = pinned_poll(DeviceType::ELECTRICAL_HEATER, 1, DeviceModel::NONE);
  for (const DeviceRegister towel :
       {DeviceRegister::TOWEL_MODE, DeviceRegister::TEMPORARY_STATE, DeviceRegister::WORKING_RATE,
        DeviceRegister::TOWEL_TEMPERATURE_IN_FORCE, DeviceRegister::ECO_SETBACK}) {
    EXPECT_FALSE(lists(radiator_poll.every_poll, towel)) << device_register_name(towel);
    EXPECT_FALSE(lists(radiator_poll.once_after_boot, towel)) << device_register_name(towel);
    EXPECT_NE(p.preset_fn, towel) << device_register_name(towel);
  }
  for (size_t i = 0; i < p.presets.size(); i++)
    EXPECT_NE(p.presets[i].id, HeatingPresetId::NONE) << "the radiator offers no 'none' preset";
}

// === Heater with a night light (52/1, model HEATER_WITH_LIGHT) ===

TEST(HeatingProfilesHeaterWithLight, HasNoProfileOfItsOwnAndIsPolledForTheLightByItsCapability) {
  const HeatingProfile &radiator = *find_heating_profile(DeviceType::ELECTRICAL_HEATER, 1, DeviceModel::NONE);
  EXPECT_EQ(find_heating_profile(DeviceType::ELECTRICAL_HEATER, 1, DeviceModel::HEATER_WITH_LIGHT), &radiator)
      << "the model drives the card exactly as the radiator does";
  EXPECT_EQ(pinned_poll(DeviceType::ELECTRICAL_HEATER, 1, DeviceModel::HEATER_WITH_LIGHT).every_poll,
            pinned_poll(DeviceType::ELECTRICAL_HEATER, 1, DeviceModel::NONE).every_poll);
}

// === Pilot-wire interface (52/0, model PILOT_WIRE_INTERFACE) ===

TEST(HeatingProfilesPilotWireInterface, OffersTheCommandablePilotWireOrdersAndNoBoost) {
  const HeatingProfile &plain = *find_heating_profile(DeviceType::ELECTRICAL_HEATER, 0, DeviceModel::NONE);
  const HeatingProfile *pilot =
      find_heating_profile(DeviceType::ELECTRICAL_HEATER, 0, DeviceModel::PILOT_WIRE_INTERFACE);
  ASSERT_NE(pilot, nullptr);
  ASSERT_NE(pilot, &plain);
  EXPECT_EQ(pilot->model, DeviceModel::PILOT_WIRE_INTERFACE);

  // Away, Eco, Comfort, Comfort -1 and Comfort -2; Off is the mode.
  ASSERT_EQ(pilot->presets.size(), 5u);
  const uint8_t commandable[] = {heating_level::FROST_PROTECTION, heating_level::ECO, heating_level::COMFORT,
                                 heating_level::COMFORT_MINUS_1, heating_level::COMFORT_MINUS_2};
  for (size_t i = 0; i < std::size(commandable); i++) {
    EXPECT_EQ(pilot->presets[i].wire, commandable[i]) << i;
    EXPECT_NE(pilot->presets[i].id, HeatingPresetId::BOOST) << i;
  }
  for (const HeatingPresetEntry &preset : pilot->presets)
    EXPECT_NE(preset.wire, heating_level::BOOST);
  for (const HeatingObservedState &row : pilot->observed)
    EXPECT_NE(row.wire, heating_level::BOOST);
  EXPECT_EQ(pilot->modes.size(), 2u);
}

TEST(HeatingProfilesPilotWireInterface, EveryObservedLevelNamesTheSamePresetAsThePlainHeater) {
  const HeatingProfile &plain = *find_heating_profile(DeviceType::ELECTRICAL_HEATER, 0, DeviceModel::NONE);
  const HeatingProfile &pilot =
      *find_heating_profile(DeviceType::ELECTRICAL_HEATER, 0, DeviceModel::PILOT_WIRE_INTERFACE);
  for (const HeatingObservedState &row : pilot.observed) {
    const auto same = std::find_if(plain.observed.begin(), plain.observed.end(),
                                   [&](const HeatingObservedState &p) { return p.wire == row.wire; });
    ASSERT_NE(same, plain.observed.end()) << static_cast<int>(row.wire);
    EXPECT_EQ(same->hvac, row.hvac);
    if (row.preset == -1) {
      EXPECT_EQ(same->preset, -1);
    } else {
      const HeatingPresetEntry &a = pilot.presets[static_cast<size_t>(row.preset)];
      const HeatingPresetEntry &b = plain.presets[static_cast<size_t>(same->preset)];
      EXPECT_EQ(a.id, b.id);
      EXPECT_EQ(a.wire, b.wire);
    }
  }
}

TEST(HeatingProfilesPilotWireInterface, KeepsThePlainHeatersControlPathAndMainParameterPrefix) {
  const HeatingProfile &plain = *find_heating_profile(DeviceType::ELECTRICAL_HEATER, 0, DeviceModel::NONE);
  const HeatingProfile &pilot =
      *find_heating_profile(DeviceType::ELECTRICAL_HEATER, 0, DeviceModel::PILOT_WIRE_INTERFACE);
  EXPECT_EQ(pilot.path, HeatingControlPath::HEATING_LEVEL);
  EXPECT_EQ(pilot.path, plain.path);
  EXPECT_EQ(pilot.mp_prefix, plain.mp_prefix);
  EXPECT_EQ(pilot.modes.data(), plain.modes.data());
  EXPECT_EQ(pilot.features.mode_observed, plain.features.mode_observed);
  EXPECT_EQ(find_heating_profile(DeviceType::ELECTRICAL_HEATER, 1, DeviceModel::PILOT_WIRE_INTERFACE),
            find_heating_profile(DeviceType::ELECTRICAL_HEATER, 1, DeviceModel::NONE))
      << "a subtype that does not offer the model falls back to its profile";
}

TEST(HeatingProfilesPilotWireInterface, APlainHeaterKeepsItsBoost) {
  const HeatingProfile &plain = *find_heating_profile(DeviceType::ELECTRICAL_HEATER, 0, DeviceModel::NONE);
  EXPECT_EQ(plain.presets.size(), 6u);
  EXPECT_EQ(plain.presets[3].id, HeatingPresetId::BOOST);
}

TEST(HeatingProfilesHeaterWithLight, NoOtherKindIsPolledForTheLight) {
  for (const auto model : {DeviceModel::NONE, DeviceModel::TOWEL_DRYER}) {
    const PinnedPoll poll = pinned_poll(DeviceType::ELECTRICAL_HEATER, 1, model);
    for (const DeviceRegister light : {DeviceRegister::NIGHT_LIGHT, DeviceRegister::NIGHT_LIGHT_BRIGHTNESS}) {
      EXPECT_FALSE(lists(poll.every_poll, light));
      EXPECT_FALSE(lists(poll.every_poll_may_miss, light));
    }
  }
}

// === The registers a heater poll reads (pinned) ===

TEST(HeaterPollReadsPin, TheRadiatorReadsFiveRegistersTheDetectionSettingEveryPollAndTheMaximumOnce) {
  const PinnedPoll poll = pinned_poll(DeviceType::ELECTRICAL_HEATER, 1, DeviceModel::NONE);
  EXPECT_EQ(poll.every_poll,
            (std::vector<DeviceRegister>{DeviceRegister::ROOM_TEMPERATURE, DeviceRegister::COMFORT_SETPOINT,
                                         DeviceRegister::OPERATING_MODE, DeviceRegister::REGULATOR_STATE,
                                         DeviceRegister::WINDOW_OPEN}));
  EXPECT_EQ(poll.every_poll_may_miss, (std::vector<DeviceRegister>{DeviceRegister::OPEN_WINDOW_DETECTION}));
  EXPECT_EQ(poll.once_after_boot, (std::vector<DeviceRegister>{DeviceRegister::MAX_SETPOINT}));
}

TEST(HeaterPollReadsPin, TheHeaterWithALightReadsTheRadiatorsRegistersThenTheLightsTwo) {
  const PinnedPoll poll = pinned_poll(DeviceType::ELECTRICAL_HEATER, 1, DeviceModel::HEATER_WITH_LIGHT);
  EXPECT_EQ(poll.every_poll,
            (std::vector<DeviceRegister>{DeviceRegister::ROOM_TEMPERATURE, DeviceRegister::COMFORT_SETPOINT,
                                         DeviceRegister::OPERATING_MODE, DeviceRegister::REGULATOR_STATE,
                                         DeviceRegister::WINDOW_OPEN}));
  EXPECT_EQ(poll.every_poll_may_miss,
            (std::vector<DeviceRegister>{DeviceRegister::OPEN_WINDOW_DETECTION, DeviceRegister::NIGHT_LIGHT,
                                         DeviceRegister::NIGHT_LIGHT_BRIGHTNESS}));
  EXPECT_EQ(poll.once_after_boot, (std::vector<DeviceRegister>{DeviceRegister::MAX_SETPOINT}));
}

TEST(HeaterPollReadsPin, TheTowelDryerReadsItsFiveRegistersAndTheMaximumOnce) {
  const PinnedPoll poll = pinned_poll(DeviceType::ELECTRICAL_HEATER, 1, DeviceModel::TOWEL_DRYER);
  EXPECT_EQ(poll.every_poll,
            (std::vector<DeviceRegister>{DeviceRegister::ROOM_TEMPERATURE, DeviceRegister::COMFORT_SETPOINT,
                                         DeviceRegister::TOWEL_MODE, DeviceRegister::TEMPORARY_STATE,
                                         DeviceRegister::WORKING_RATE}));
  EXPECT_TRUE(poll.every_poll_may_miss.empty());
  EXPECT_EQ(poll.once_after_boot, (std::vector<DeviceRegister>{DeviceRegister::MAX_SETPOINT}));
}

TEST(HeaterPollReadsPin, AHeaterThatTakesTheStatusReadReadsNoRegisterOfItsOwn) {
  for (const auto &[type, subtype, model] :
       {std::tuple{DeviceType::ELECTRICAL_HEATER, uint8_t{0}, DeviceModel::NONE},
        std::tuple{DeviceType::ELECTRICAL_HEATER, uint8_t{0}, DeviceModel::PILOT_WIRE_INTERFACE},
        std::tuple{DeviceType::HEATING_TEMPERATURE_INTERFACE, uint8_t{0}, DeviceModel::NONE},
        std::tuple{DeviceType::HEAT_PUMP, uint8_t{0}, DeviceModel::NONE}}) {
    const PinnedPoll poll = pinned_poll(type, subtype, model);
    EXPECT_TRUE(poll.every_poll.empty());
    EXPECT_TRUE(poll.every_poll_may_miss.empty());
    EXPECT_TRUE(poll.once_after_boot.empty());
  }
}

namespace {

/// @return Whether changing one register of a device moves the card of its profile, on a device whose mode shows
/// `shown`
///         (the programme's temperature in force replaces the setpoint on the card while the programme runs, so a
///         register may matter in one state only).
bool card_moves_with(const HeatingProfile &profile, DeviceRegister changed_fn, HeatingHvacMode shown) {
  const auto mode = std::ranges::find(profile.observed, shown, &HeatingObservedState::hvac);
  EXPECT_NE(mode, profile.observed.end());
  IoDevice base;
  for (const DeviceReadingRow &row : device_readings()) {
    if (const auto *source = std::get_if<RegisterSource>(&row.source))
      test::store_reading(base, source->fn, 1.0F);
  }
  test::store_reading(base, profile.mode_fn, static_cast<float>(mode->wire));
  if (!profile.preset_observed.empty())
    test::store_reading(base, profile.preset_fn, static_cast<float>(profile.programme_period->idle_preset_state));
  const HeatingCardState before = reduce_heating_card(profile, base, initial_heating_card(profile));
  for (const float value : {0.0F, 2.0F, 18.0F, 22.5F}) {
    IoDevice changed = base;
    test::store_reading(changed, changed_fn, value);
    if (!reduce_heating_card(profile, changed, initial_heating_card(profile)).shows_same_as(before))
      return true;
  }
  return false;
}

}  // namespace

TEST(HeatingProfileReads, TheRegistersTheCardReadsAreTheOnesTheProfileSays) {
  // The card moves with a register exactly when the profile says it reads it, apart from the programme's override,
  // which is read for the write that goes to it and not shown.
  for (const HeatingProfile &profile : heating_profiles()) {
    if (profile.path != HeatingControlPath::PRIVATE_REGISTERS)
      continue;
    for (const DeviceReadingRow &row : device_readings()) {
      const auto *source = std::get_if<RegisterSource>(&row.source);
      if (source == nullptr)
        continue;
      const bool card_reads = card_moves_with(profile, source->fn, HeatingHvacMode::AUTO) ||
                              card_moves_with(profile, source->fn, HeatingHvacMode::HEAT);
      const bool override_register =
          profile.programme_period != nullptr && source->fn == profile.programme_period->derogation_fn;
      EXPECT_EQ(heating_profile_reads(profile, row.reading), card_reads || override_register)
          << row.name << " on " << static_cast<int>(profile.type) << "/" << static_cast<int>(profile.subtype);
    }
  }
}

TEST(HeatingProfileReads, AProfileOfAnotherControlPathReadsNoRegister) {
  for (const HeatingProfile &profile : heating_profiles()) {
    if (profile.path == HeatingControlPath::PRIVATE_REGISTERS)
      continue;
    for (const DeviceReadingRow &row : device_readings())
      EXPECT_FALSE(heating_profile_reads(profile, row.reading)) << row.name;
  }
  EXPECT_FALSE(heating_profile_reads(*find_heating_profile(DeviceType::ELECTRICAL_HEATER, 1, DeviceModel::NONE),
                                     DeviceReading::HEATING_LEVEL))
      << "a status reading is no register";
  EXPECT_FALSE(heating_profile_reads(*find_heating_profile(DeviceType::ELECTRICAL_HEATER, 1, DeviceModel::NONE),
                                     DeviceReading::COUNT));
}

// === Selecting what a poll reads ===

namespace {

/// A radiator with a light: the kind with the most optional parts.
struct SelectingRig {
  const HeatingProfile &profile{
      *find_heating_profile(DeviceType::ELECTRICAL_HEATER, 1, DeviceModel::HEATER_WITH_LIGHT)};
  IoDevice dev;
  SelectingRig() {
    dev.model = DeviceModel::HEATER_WITH_LIGHT;
    test::set_kind(dev, DeviceType::ELECTRICAL_HEATER, 1);
  }
  std::vector<DeviceRegister> reads(ReadingPoll policy) const {
    const HeaterPollList list = device_poll_reads(dev, policy);
    return {list.begin(), list.end()};
  }
};

}  // namespace

TEST(HeaterPollReads, AskedRegistersAreSelectedWhenAskedAndNotAfterAnEmptyAnswer) {
  SelectingRig rig;
  EXPECT_TRUE(rig.reads(ReadingPoll::WHEN_ASKED).empty()) << "nothing asked";
  ask_register_reading(rig.dev, DeviceRegister::OCCUPANCY_DETECTION);
  ask_register_reading(rig.dev, DeviceRegister::PRESENCE);
  EXPECT_EQ(rig.reads(ReadingPoll::WHEN_ASKED),
            (std::vector<DeviceRegister>{DeviceRegister::PRESENCE, DeviceRegister::OCCUPANCY_DETECTION}))
      << "in the order of the table, not of the asking";
  mark_register_reading_absent(rig.dev, DeviceRegister::PRESENCE);
  EXPECT_EQ(rig.reads(ReadingPoll::WHEN_ASKED), (std::vector<DeviceRegister>{DeviceRegister::OCCUPANCY_DETECTION}));
}

TEST(HeaterPollReads, AnAskedRegisterTheKindHasNoCapabilityForIsNotSelected) {
  SelectingRig rig;
  ask_register_reading(rig.dev, DeviceRegister::CENTRAL_HEATING_MODE);
  EXPECT_TRUE(rig.reads(ReadingPoll::WHEN_ASKED).empty()) << "a radiator has no central-heating mode";
}

TEST(HeaterPollReads, TheMaximumIsSelectedUntilItHoldsAValue) {
  SelectingRig rig;
  EXPECT_EQ(rig.reads(ReadingPoll::ONCE_AFTER_BOOT), (std::vector<DeviceRegister>{DeviceRegister::MAX_SETPOINT}));
  test::store_reading(rig.dev, DeviceRegister::MAX_SETPOINT, 28.0F);
  EXPECT_TRUE(rig.reads(ReadingPoll::ONCE_AFTER_BOOT).empty());
}

TEST(HeaterPollReads, ADeviceWithoutAPartIsNotPolledForIt) {
  SelectingRig light;
  EXPECT_EQ(light.reads(ReadingPoll::EVERY_POLL_MAY_MISS),
            (std::vector<DeviceRegister>{DeviceRegister::OPEN_WINDOW_DETECTION, DeviceRegister::NIGHT_LIGHT,
                                         DeviceRegister::NIGHT_LIGHT_BRIGHTNESS}));
  light.dev.model = DeviceModel::NONE;
  test::set_kind(light.dev, DeviceType::ELECTRICAL_HEATER, 1);
  EXPECT_EQ(light.reads(ReadingPoll::EVERY_POLL_MAY_MISS),
            (std::vector<DeviceRegister>{DeviceRegister::OPEN_WINDOW_DETECTION}))
      << "a radiator without the capability reads no light";
}

// === Every preset is a command ===

TEST(HeatingProfiles, EveryPresetOfEveryProfileSendsACommand) {
  for (const HeatingProfile &profile : heating_profiles()) {
    for (const HeatingPresetEntry &preset : profile.presets) {
      const std::string label = std::string(preset.custom_name != nullptr ? preset.custom_name : "standard") + " of " +
                                std::to_string(static_cast<int>(profile.type)) + "/" + std::to_string(profile.subtype);
      switch (profile.path) {
        case HeatingControlPath::PRIVATE_REGISTERS:
          EXPECT_TRUE(heating_write_accepts(profile.preset_fn, static_cast<float>(preset.wire)))
              << label << ": the register's rule takes the value the preset writes";
          break;
        case HeatingControlPath::HEATING_LEVEL:
          EXPECT_NE(preset.wire, heating_level::SECURED) << label << ": a level a heater reports and never takes";
          break;
        case HeatingControlPath::TEMPERATURE_EXECUTE:
          break;
      }
      // A selected preset becomes a write, never a refusal.
      HeatingWriteRequest request;
      request.kind = HeatingWriteKind::PRESET;
      request.preset = preset.id;
      request.custom_name = preset.custom_name;
      const HeatingWritePlan plan = plan_heating_write(profile, request, HeatingDeviceFacts{.has_record = true});
      EXPECT_TRUE(plan.is_write()) << label;
    }
  }
}

TEST(HeatingProfiles, EveryModeAProfileWritesToTheModeRegisterIsWritable) {
  for (const HeatingProfile &p : heating_profiles()) {
    if (p.path != HeatingControlPath::PRIVATE_REGISTERS)
      continue;
    // A profile whose mode register is another one (the towel dryer's) has its own value set.
    if (p.mode_fn == DeviceRegister::OPERATING_MODE) {
      for (const HeatingModeEntry &m : p.modes)
        EXPECT_TRUE(heating_mode_is_writable(static_cast<HeatingMode>(m.wire)))
            << "mode " << static_cast<int>(m.wire) << " of profile " << static_cast<int>(p.type) << "/"
            << static_cast<int>(p.subtype);
    }
    for (const HeatingPresetEntry &pr : p.presets) {
      if (p.preset_fn != DeviceRegister::OPERATING_MODE)
        continue;
      EXPECT_TRUE(heating_mode_is_writable(static_cast<HeatingMode>(pr.wire)))
          << "preset " << static_cast<int>(pr.wire) << " of profile " << static_cast<int>(p.type) << "/"
          << static_cast<int>(p.subtype);
    }
  }
}

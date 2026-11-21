/// @file heating_write_rule_test.cpp
/// @brief Tests for the write rule of every heater register: the one judgement heating_write_accepts() and
/// the range of a register, which every caller asks instead of keeping a bound of its own.

#include "proto_heating.h"
#include "proto_heating_profiles.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

using namespace esphome::home_io_control;

namespace {

/// Every function of the table.
std::vector<DeviceRegister> all_functions() {
  std::vector<DeviceRegister> all;
  for (uint8_t i = 0; i < static_cast<uint8_t>(DeviceRegister::COUNT); i++)
    all.push_back(static_cast<DeviceRegister>(i));
  return all;
}

/// Values that probe every kind of register: whole numbers, tenths, edges, and what is never a value.
std::vector<float> probe_values() {
  std::vector<float> values = {-INFINITY, -1.0F,  -0.0F,    0.0F,     0.5F,     6.9F, 7.0F,    20.55F,
                               28.0F,     28.05F, 10000.0F, 10001.0F, 65535.0F, NAN,  INFINITY};
  for (int whole = 0; whole <= 300; whole++)
    values.push_back(static_cast<float>(whole));
  return values;
}

}  // namespace

TEST(HeatingWriteRule, ARegisterThatCannotBeWrittenAcceptsNothing) {
  for (const DeviceRegister fn : all_functions()) {
    if (device_register_is_writable(fn))
      continue;
    for (const float value : probe_values())
      EXPECT_FALSE(heating_write_accepts(fn, value)) << device_register_name(fn) << " value " << value;
    EXPECT_FALSE(heating_write_range(fn).has_value()) << device_register_name(fn);
  }
}

TEST(HeatingWriteRule, OnlyAWritableTenthsRegisterHasARangeAndItLiesInsideTheCodecsSpan) {
  for (const DeviceRegister fn : all_functions()) {
    const std::optional<ValueRange> range = heating_write_range(fn);
    if (!range)
      continue;
    const ValueCodec codec = device_register_codec(fn);
    EXPECT_TRUE(device_register_is_writable(fn)) << device_register_name(fn);
    EXPECT_TRUE(codec == ValueCodec::TENTHS_U16 || codec == ValueCodec::TENTHS_U16_OR_ZERO) << device_register_name(fn);
    EXPECT_LT(range->min, range->max) << device_register_name(fn);
    EXPECT_GE(range->min, 0.0F) << device_register_name(fn);
    EXPECT_LE(range->max, 6553.5F) << device_register_name(fn);
    EXPECT_TRUE(heating_write_accepts(fn, range->min)) << device_register_name(fn);
    EXPECT_TRUE(heating_write_accepts(fn, range->max)) << device_register_name(fn);
    EXPECT_FALSE(heating_write_accepts(fn, std::nextafter(range->min, -INFINITY))) << device_register_name(fn);
    EXPECT_FALSE(heating_write_accepts(fn, std::nextafter(range->max, INFINITY))) << device_register_name(fn);
  }
}

TEST(HeatingWriteRule, TheWaterHeaterRegistersFollowTheSameRule) {
  // The mode takes the three values the heater reports, a duration any byte; neither has a range of its own.
  for (const float mode : {0.0F, 1.0F, 2.0F})
    EXPECT_TRUE(heating_write_accepts(DeviceRegister::DHW_MODE, mode)) << mode;
  for (const float refused : {3.0F, 255.0F, -1.0F, 0.5F, NAN, INFINITY})
    EXPECT_FALSE(heating_write_accepts(DeviceRegister::DHW_MODE, refused)) << refused;
  for (const DeviceRegister duration : {DeviceRegister::DHW_BOOST_DURATION, DeviceRegister::DHW_AWAY_DURATION}) {
    for (const float accepted : {0.0F, 1.0F, 255.0F})
      EXPECT_TRUE(heating_write_accepts(duration, accepted)) << device_register_name(duration) << " " << accepted;
    for (const float refused : {256.0F, -1.0F, 1.5F, NAN})
      EXPECT_FALSE(heating_write_accepts(duration, refused)) << device_register_name(duration) << " " << refused;
  }
  for (const DeviceRegister fn :
       {DeviceRegister::DHW_MODE, DeviceRegister::DHW_BOOST_DURATION, DeviceRegister::DHW_AWAY_DURATION})
    EXPECT_FALSE(heating_write_range(fn).has_value()) << device_register_name(fn);
}

TEST(HeatingWriteRule, OnlyTheModeRegisterHasAHoleInTheWholeNumbersItAccepts) {
  // A register with no refusal predicate and no range takes one unbroken run of whole numbers: from 0, or from the
  // first knot of its scale (a water heater's shower count, shown as 50 to 70 degrees).
  for (const DeviceRegister fn : all_functions()) {
    if (!device_register_is_writable(fn) || heating_write_range(fn).has_value())
      continue;
    bool started = false;
    bool ended = false;
    bool hole = false;
    for (int whole = 0; whole <= 300; whole++) {
      const bool accepted = heating_write_accepts(fn, static_cast<float>(whole));
      hole = hole || (accepted && ended);
      started = started || accepted;
      ended = ended || (started && !accepted);
    }
    EXPECT_EQ(hole, fn == DeviceRegister::OPERATING_MODE) << device_register_name(fn);
  }
}

TEST(HeatingWriteRule, TheModeRegisterRefusesExactlyTheExternalSetpoint) {
  for (int mode = 0; mode <= 5; mode++) {
    const bool writable =
        mode <= static_cast<int>(HeatingMode::OFF) && heating_mode_is_writable(static_cast<HeatingMode>(mode));
    EXPECT_EQ(heating_write_accepts(DeviceRegister::OPERATING_MODE, static_cast<float>(mode)), writable) << mode;
  }
  EXPECT_FALSE(heating_write_accepts(DeviceRegister::OPERATING_MODE, 255.0F));
}

TEST(HeatingWriteRule, ARegisterProfilesCardRangeIsTheSetpointRule) {
  const struct {
    DeviceModel model;
  } models[] = {{DeviceModel::NONE}, {DeviceModel::TOWEL_DRYER}, {DeviceModel::HEATER_WITH_LIGHT}};
  for (const auto &m : models) {
    const HeatingProfile *profile = find_heating_profile(DeviceType::ELECTRICAL_HEATER, 1, m.model);
    ASSERT_NE(profile, nullptr);
    ASSERT_EQ(profile->path, HeatingControlPath::PRIVATE_REGISTERS);
    const std::optional<ValueRange> rule = heating_write_range(DeviceRegister::COMFORT_SETPOINT);
    ASSERT_TRUE(rule.has_value());
    EXPECT_FLOAT_EQ(profile->visual_min_c, rule->min);
    EXPECT_FLOAT_EQ(profile->visual_max_c, rule->max);
    EXPECT_TRUE(heating_write_accepts(DeviceRegister::COMFORT_SETPOINT, profile->visual_min_c));
    EXPECT_TRUE(heating_write_accepts(DeviceRegister::COMFORT_SETPOINT, profile->visual_max_c));
  }
}

// === The functions the heating_control action offers ===

namespace {

/// The names of exactly the functions the action offers.
const std::vector<const char *> ACTION_FUNCTION_NAMES = {
    "read_paired_devices", "read_setpoint_block", "set_comfort_setpoint",      "set_derogated_setpoint",
    "set_operating_mode",  "set_presence",        "set_open_window_detection", "read_window_open"};

/// A retired function name and the current name of the function it runs.
struct RetiredName {
  const char *old_name;
  const char *new_name;
};

const std::vector<RetiredName> RETIRED_NAMES = {
    {"power_on", "read_paired_devices"},      {"set_temperature", "set_comfort_setpoint"},
    {"set_mode", "set_operating_mode"},       {"set_derogation", "set_derogated_setpoint"},
    {"midnight_sync", "read_setpoint_block"}, {"window_open", "read_window_open"}};

}  // namespace

TEST(HeatingActionFunctions, ExactlyTheFunctionsTheActionOffersAreMarkedAndLookedUpByName) {
  size_t marked = 0;
  for (const DeviceRegister fn : all_functions()) {
    if (heating_action_value_label(fn) != nullptr)
      marked++;
  }
  EXPECT_EQ(marked, ACTION_FUNCTION_NAMES.size());
  for (const char *name : ACTION_FUNCTION_NAMES) {
    const std::optional<HeatingActionFunction> offered = heating_action_function(name);
    ASSERT_TRUE(offered.has_value()) << name;
    EXPECT_EQ(heating_action_function_name(offered->fn), name);
    EXPECT_EQ(offered->replaced_name, nullptr) << name << " is a current name";
    EXPECT_NE(heating_action_value_label(offered->fn), nullptr) << name;
  }
}

TEST(HeatingActionFunctions, AFunctionNameIsSetForAWritableRegisterAndReadForAReadOnlyOne) {
  EXPECT_EQ(heating_action_function_name(DeviceRegister::COMFORT_SETPOINT), "set_comfort_setpoint");
  EXPECT_EQ(heating_action_function_name(DeviceRegister::WINDOW_OPEN), "read_window_open");
  for (const char *name : ACTION_FUNCTION_NAMES) {
    const DeviceRegister fn = heating_action_function(name)->fn;
    const std::string expected =
        std::string(device_register_is_writable(fn) ? "set_" : "read_") + device_register_name(fn);
    EXPECT_EQ(expected, name);
  }
}

TEST(HeatingActionFunctions, ARetiredNameRunsTheFunctionItReplacedAndSaysSo) {
  for (const RetiredName &retired : RETIRED_NAMES) {
    const std::optional<HeatingActionFunction> offered = heating_action_function(retired.old_name);
    ASSERT_TRUE(offered.has_value()) << retired.old_name;
    EXPECT_EQ(heating_action_function_name(offered->fn), retired.new_name) << retired.old_name;
    ASSERT_NE(offered->replaced_name, nullptr) << retired.old_name;
    EXPECT_STREQ(offered->replaced_name, retired.old_name);
    EXPECT_EQ(heating_action_function(retired.new_name)->fn, offered->fn) << retired.old_name;
  }
}

TEST(HeatingActionFunctions, ARegisterTheActionDoesNotOfferIsReachableUnderNoName) {
  for (const DeviceRegister fn : all_functions()) {
    if (heating_action_value_label(fn) != nullptr)
      continue;
    const std::string name = device_register_name(fn);
    EXPECT_EQ(heating_action_function_name(fn), "") << name;
    for (const std::string &candidate : {name, "set_" + name, "read_" + name})
      EXPECT_FALSE(heating_action_function(candidate).has_value()) << candidate;
  }
  // Under the wrong verb, or bare, an offered register is no function either.
  for (const char *name : {"", "unknown", "SET_OPERATING_MODE", " set_operating_mode", "set_operating_mode ",
                           "make_coffee", "read_comfort_setpoint", "set_window_open", "comfort_setpoint"})
    EXPECT_FALSE(heating_action_function(name).has_value()) << "'" << name << "'";
}

TEST(HeatingActionFunctions, AFunctionTakesAValueExactlyWhenItIsWritable) {
  for (const char *name : ACTION_FUNCTION_NAMES) {
    const DeviceRegister fn = heating_action_function(name)->fn;
    const std::string label = heating_action_value_label(fn);
    EXPECT_EQ(label.empty(), !device_register_is_writable(fn)) << name;
  }
}

TEST(HeatingActionFunctions, TheOnlyEnumeratedByteTheActionOffersIsTheMode) {
  // The action reads a bounded byte as a mode word; a second one would need its own words.
  for (const char *name : ACTION_FUNCTION_NAMES) {
    const DeviceRegister fn = heating_action_function(name)->fn;
    if (device_register_is_writable(fn) && device_register_codec(fn) == ValueCodec::BOUNDED_BYTE) {
      EXPECT_EQ(fn, DeviceRegister::OPERATING_MODE) << name;
    }
  }
}

TEST(HeatingModeNames, EveryModeRoundTripsThroughItsWord) {
  for (uint8_t value = 0; value <= static_cast<uint8_t>(HeatingMode::OFF); value++) {
    const auto mode = static_cast<HeatingMode>(value);
    const std::optional<HeatingMode> back = heating_mode_from_name(heating_mode_name(mode));
    ASSERT_TRUE(back.has_value()) << heating_mode_name(mode);
    EXPECT_EQ(*back, mode);
  }
  EXPECT_FALSE(heating_mode_from_name("").has_value());
  EXPECT_FALSE(heating_mode_from_name("heat").has_value());
  EXPECT_FALSE(heating_mode_from_name("AUTO").has_value()) << "the caller folds the case";
}

TEST(HeatingModeNames, OnlyTheWritableModesAreOffered) {
  EXPECT_EQ(heating_writable_mode_names(), "auto, manual, prog, off");
  EXPECT_STREQ(heating_mode_name(HeatingMode::EXTERNAL_SETPOINT), "external");
  EXPECT_FALSE(
      heating_write_accepts(DeviceRegister::OPERATING_MODE, static_cast<float>(*heating_mode_from_name("external"))));
}

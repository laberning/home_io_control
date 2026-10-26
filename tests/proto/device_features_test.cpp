/// @file device_features_test.cpp
/// @brief The capability word of every node-type table row as a literal expectation, the lookup
///        fallbacks, the resolver, the value checks, the named-position table and the undeclared-kind
///        defaults.
/// @ingroup hioc_protocol

#include "proto_commands.h"
#include "proto_device_features.h"
#include "proto_device_model.h"
#include "proto_device_types.h"

#include "device_table_walk.h"
#include "test_helpers.h"

#include <vector>

using namespace esphome::home_io_control;
namespace feature = esphome::home_io_control::device_feature;

namespace {

/// One expected table row: the kind and the whole capability word it states.
struct ExpectedRow {
  DeviceType type;
  uint8_t subtype;
  DeviceFeatures features;
};

// Shorthand for the words below.
constexpr DeviceFeatures MPO = feature::MAIN_PARAMETER_ONLY;
constexpr DeviceFeatures LV = feature::LEVELS;
constexpr DeviceFeatures TI = feature::TILT;
constexpr DeviceFeatures FAV = feature::FAVORITE;
constexpr DeviceFeatures VENT = feature::VENT;
constexpr DeviceFeatures PAR = feature::PARTIAL;
constexpr DeviceFeatures PED = feature::PEDESTRIAN;
constexpr DeviceFeatures EXT = feature::EXTENDED_POSITION_FRAME;
constexpr DeviceFeatures WIN = feature::WINDOW_COMPANIONS;
constexpr DeviceFeatures INV = feature::INVERTED;

/// Every row of the table. A row missing here, or listed with another word, fails the test below.
const std::vector<ExpectedRow> EXPECTED_ROWS = {
    {DeviceType::UNKNOWN, 0, 0},
    {DeviceType::INTERIOR_VENETIAN_BLIND, 0, LV | FAV | TI},
    {DeviceType::ROLLER_SHUTTER, 0, LV | FAV | EXT},
    {DeviceType::ROLLER_SHUTTER, 1, LV | FAV | EXT},
    {DeviceType::ROLLER_SHUTTER, 2, LV | FAV | EXT},
    {DeviceType::VERTICAL_EXTERIOR_AWNING, 0, LV | FAV | EXT},
    {DeviceType::WINDOW_OPENER, 0, LV | FAV | VENT | EXT},
    {DeviceType::WINDOW_OPENER, 1, LV | FAV | VENT | EXT},
    {DeviceType::GARAGE_OPENER, 0, MPO | LV | PAR},
    {DeviceType::GARAGE_OPENER, 58, MPO | PAR},
    {DeviceType::LIGHT, 0, EXT},
    {DeviceType::LIGHT, 1, LV | EXT},
    {DeviceType::LIGHT, 2, LV | EXT},
    {DeviceType::LIGHT, 58, MPO},
    {DeviceType::GATE_OPENER, 0, LV | FAV | PED | EXT},
    {DeviceType::GATE_OPENER, 58, MPO | FAV | PED},
    {DeviceType::ROLLING_DOOR_OPENER, 0, LV | FAV | EXT},
    {DeviceType::LOCK, 0, EXT},
    {DeviceType::LOCK, 1, EXT},
    {DeviceType::LOCK, 2, 0},
    {DeviceType::VERTICAL_INTERIOR_BLIND, 0, LV | FAV | EXT},
    {DeviceType::SECURE_CONFIGURATION_DEVICE, 0, 0},
    {DeviceType::BEACON, 0, 0},
    {DeviceType::DUAL_ROLLER_SHUTTER, 0, LV | FAV},
    {DeviceType::HEATING_TEMPERATURE_INTERFACE, 0, 0},
    {DeviceType::ON_OFF_SWITCH, 0, EXT},
    {DeviceType::ON_OFF_SWITCH, 2, EXT},
    {DeviceType::HORIZONTAL_AWNING, 0, LV | FAV | EXT | INV},
    {DeviceType::HORIZONTAL_AWNING, 1, LV | FAV | EXT | INV},
    {DeviceType::EXTERIOR_VENETIAN_BLIND, 0, LV | FAV | TI | EXT},
    {DeviceType::LOUVRE_BLIND, 0, LV | FAV | TI | EXT},
    {DeviceType::CURTAIN_TRACK, 0, LV | FAV | EXT},
    {DeviceType::VENTILATION_POINT, 0, 0},
    {DeviceType::VENTILATION_POINT, 1, 0},
    {DeviceType::VENTILATION_POINT, 2, 0},
    {DeviceType::VENTILATION_POINT, 3, 0},
    {DeviceType::EXTERIOR_HEATING, 0, MPO | LV},
    {DeviceType::EXTERIOR_HEATING, 58, MPO},
    {DeviceType::HEAT_PUMP, 0, 0},
    {DeviceType::HEAT_PUMP, 1, 0},
    {DeviceType::HEAT_PUMP, 2, 0},
    {DeviceType::INTRUSION_ALARM, 0, 0},
    {DeviceType::SWINGING_SHUTTER, 0, LV | FAV | EXT},
    {DeviceType::SWINGING_SHUTTER, 1, LV | FAV | EXT},
    {DeviceType::SWINGING_SHUTTER, 58, MPO | FAV},
    {DeviceType::SLIDING_WINDOW, 0, LV | FAV | EXT},
    {DeviceType::SLIDING_WINDOW, 1, LV | FAV | EXT},
    {DeviceType::ZONE_CONTROL, 0, 0},
    {DeviceType::BIOCLIMATIC_PERGOLA, 0, LV | FAV | EXT},
    {DeviceType::SIREN, 0, 0},
    {DeviceType::DOMESTIC_HOT_WATER, 0, 0},
    {DeviceType::ELECTRICAL_HEATER, 0, 0},
    {DeviceType::ELECTRICAL_HEATER, 1, WIN},
    {DeviceType::HEAT_RECOVERY_VENTILATION, 0, 0},
    {DeviceType::CENTRAL_HOUSE_CONTROL, 0, 0},
    {DeviceType::TEST_AND_EVALUATION, 0, 0},
    {DeviceType::REMOTE_CONTROLLER, 0, 0},
    {DeviceType::REMOTE_CONTROLLER, 2, 0},
    {DeviceType::REMOTE_CONTROLLER, 3, 0},
    {DeviceType::REMOTE_CONTROLLER, 4, 0},
    {DeviceType::REMOTE_CONTROLLER, 5, 0},
};

}  // namespace

// ============================================================================
// The table, row by row
// ============================================================================

TEST(DeviceFeaturesTable, EveryRowStatesTheExpectedWord) {
  size_t rows_seen = 0;
  test::for_each_table_row([&rows_seen](const DeviceTypeInfo &row) {
    rows_seen++;
    bool listed = false;
    for (const ExpectedRow &expected : EXPECTED_ROWS) {
      if (expected.type == row.type && expected.subtype == row.subtype) {
        listed = true;
        EXPECT_EQ(row.features, expected.features) << format_device_ids(row.type, row.subtype);
      }
    }
    EXPECT_TRUE(listed) << format_device_ids(row.type, row.subtype) << " is in the table but not in this test";
  });
  EXPECT_EQ(rows_seen, EXPECTED_ROWS.size()) << "a row listed here is missing from the table";
}

// ============================================================================
// Lookup
// ============================================================================

TEST(DeviceFeaturesLookup, ASubtype58WithoutARowIsTheMainParameterOnlyVariantOfItsType) {
  const DeviceFeatures roller = device_features(DeviceType::ROLLER_SHUTTER, 0);
  EXPECT_EQ(device_features(DeviceType::ROLLER_SHUTTER, SUBTYPE_MAIN_PARAMETER_ONLY),
            main_parameter_only_variant(roller));
  EXPECT_FALSE(
      has_feature(device_features(DeviceType::INTERIOR_VENETIAN_BLIND, SUBTYPE_MAIN_PARAMETER_ONLY), feature::TILT))
      << "a variant has no functional parameter, so no slat angle";
  EXPECT_TRUE(has_feature(device_features(DeviceType::WINDOW_OPENER, SUBTYPE_MAIN_PARAMETER_ONLY), feature::VENT))
      << "a named position is a main-parameter value and survives the variant";
}

TEST(DeviceFeaturesLookup, AnUnnamedSubtypeTakesTheTypeRowAndAnUnknownTypeHasNoFeatures) {
  EXPECT_EQ(device_features(DeviceType::ROLLER_SHUTTER, 9), device_features(DeviceType::ROLLER_SHUTTER, 0));
  EXPECT_EQ(device_features(static_cast<DeviceType>(26), 0), 0u);
  EXPECT_EQ(device_features(static_cast<DeviceType>(26), 3), 0u);
  EXPECT_EQ(device_features(static_cast<DeviceType>(26), SUBTYPE_MAIN_PARAMETER_ONLY), feature::MAIN_PARAMETER_ONLY)
      << "subtype 58 is main parameter only on any type, row or not";
}

TEST(DeviceFeaturesLookup, TheVariantClearsLevelsTiltAndTheExtendedFrameAndKeepsTheRest) {
  const DeviceFeatures all = feature::LEVELS | feature::TILT | feature::EXTENDED_POSITION_FRAME | feature::FAVORITE |
                             feature::VENT | feature::INVERTED | feature::WINDOW_COMPANIONS;
  EXPECT_EQ(main_parameter_only_variant(all), feature::MAIN_PARAMETER_ONLY | feature::FAVORITE | feature::VENT |
                                                  feature::INVERTED | feature::WINDOW_COMPANIONS);
}

// ============================================================================
// Undeclared kinds
// ============================================================================

TEST(UndeclaredKindFeatures, ACoverKeepsASliderFavouriteVentAndTheLongFrameButNoTilt) {
  const DeviceFeatures bits = undeclared_kind_features(DeviceCapabilityClass::COVER);
  EXPECT_TRUE(
      has_feature(bits, feature::LEVELS | feature::FAVORITE | feature::VENT | feature::EXTENDED_POSITION_FRAME));
  EXPECT_FALSE(has_feature(bits, feature::TILT));
  EXPECT_FALSE(has_feature(bits, feature::MAIN_PARAMETER_ONLY));
}

TEST(UndeclaredKindFeatures, ALightSwitchOrLockSendsTheLongFrameOnly) {
  for (const DeviceCapabilityClass capability :
       {DeviceCapabilityClass::LIGHT, DeviceCapabilityClass::SWITCH, DeviceCapabilityClass::LOCK}) {
    EXPECT_EQ(undeclared_kind_features(capability), feature::EXTENDED_POSITION_FRAME);
  }
}

TEST(UndeclaredKindFeatures, AnExteriorHeatingFanGetsLevelsInTheLongFrame) {
  EXPECT_EQ(undeclared_kind_features(DeviceCapabilityClass::HEATER_LEVEL),
            feature::LEVELS | feature::EXTENDED_POSITION_FRAME);
}

TEST(UndeclaredKindFeatures, AClassWithNoExecutePositionPathHasNoFeatures) {
  for (const DeviceCapabilityClass capability : {DeviceCapabilityClass::UNKNOWN, DeviceCapabilityClass::SENSOR,
                                                 DeviceCapabilityClass::BEACON, DeviceCapabilityClass::CLIMATE}) {
    EXPECT_EQ(undeclared_kind_features(capability), 0u);
  }
}

// ============================================================================
// Main-parameter values
// ============================================================================

TEST(DeviceAcceptsMainParameter, OpenCloseAndStopAreAcceptedByEveryDevice) {
  for (const DeviceFeatures bits : {DeviceFeatures{0}, DeviceFeatures{feature::MAIN_PARAMETER_ONLY}}) {
    EXPECT_TRUE(device_accepts_main_parameter(bits, 0x0000));
    EXPECT_TRUE(device_accepts_main_parameter(bits, 0xC800));
    EXPECT_TRUE(device_accepts_main_parameter(bits, 0xD200));
  }
}

TEST(DeviceAcceptsMainParameter, APercentPositionNeedsLevels) {
  // Issue #163: a garage opener refuses 50 % (0x6400) with FE 28.
  EXPECT_TRUE(device_accepts_main_parameter(feature::LEVELS, 0x6400));
  EXPECT_TRUE(device_accepts_main_parameter(feature::LEVELS, 0x0200));
  EXPECT_TRUE(device_accepts_main_parameter(feature::LEVELS, 0xC700));
  EXPECT_FALSE(device_accepts_main_parameter(feature::MAIN_PARAMETER_ONLY, 0x6400));
  EXPECT_FALSE(device_accepts_main_parameter(0, 0x0001));
}

TEST(DeviceAcceptsMainParameter, ANamedPositionNeedsItsOwnBit) {
  for (const NamedPosition &named : NAMED_POSITIONS) {
    EXPECT_TRUE(device_accepts_main_parameter(named.feature, named.main_parameter)) << named.name;
    EXPECT_FALSE(device_accepts_main_parameter(0, named.main_parameter)) << named.name;
    EXPECT_FALSE(device_accepts_main_parameter(feature::LEVELS, named.main_parameter))
        << named.name << ": levels do not make a named position acceptable";
  }
  EXPECT_FALSE(device_accepts_main_parameter(feature::VENT, 0xD800)) << "the favourite is not the ventilation bit";
  EXPECT_FALSE(device_accepts_main_parameter(feature::FAVORITE, 0xD803));
  EXPECT_TRUE(device_accepts_main_parameter(feature::PARTIAL, 0xD809));
  EXPECT_TRUE(device_accepts_main_parameter(feature::PEDESTRIAN, 0xD807));
  EXPECT_FALSE(device_accepts_main_parameter(feature::PEDESTRIAN, 0xD809)) << "each bit owns its own value";
  EXPECT_FALSE(device_accepts_main_parameter(feature::PARTIAL, 0xD807));
}

TEST(DeviceAcceptsMainParameter, AnUnknownOrReservedValueIsNeverAccepted) {
  const DeviceFeatures everything = 0xFFFF;
  EXPECT_FALSE(device_accepts_main_parameter(everything, 0xD500));
  EXPECT_FALSE(device_accepts_main_parameter(everything, 0xC801));
  EXPECT_FALSE(device_accepts_main_parameter(everything, 0xD400));
  EXPECT_FALSE(device_accepts_main_parameter(everything, 0xFFFF));
}

TEST(PositionMainParameter, AScaledPercentIsTheDoubledWireValueInTheHighByte) {
  EXPECT_EQ(position_main_parameter(0), 0x0000);
  EXPECT_EQ(position_main_parameter(50), 0x6400);
  EXPECT_EQ(position_main_parameter(100), 0xC800);
}

TEST(PositionMainParameter, StopIsTheStopMainParameter) { EXPECT_EQ(position_main_parameter(POS_STOP), 0xD200); }

TEST(PositionMainParameter, AValueAboveOneHundredPercentIsInvalidAndNeverAccepted) {
  // The builder's uint8_t arithmetic wraps above 127 percent; the guard must not follow it.
  for (const uint8_t position : {uint8_t{101}, uint8_t{128}, uint8_t{200}, POS_UNKNOWN, uint8_t{255}}) {
    EXPECT_EQ(position_main_parameter(position), 0xFFFF) << +position;
    EXPECT_FALSE(device_accepts_main_parameter(0xFFFF, position_main_parameter(position))) << +position;
  }
}

// ============================================================================
// Named positions
// ============================================================================

TEST(NamedPositions, EachCommandFindsItsEntryAndOthersFindNone) {
  const NamedPosition *favorite = find_named_position(CoverCommand::FAVORITE);
  ASSERT_NE(favorite, nullptr);
  EXPECT_EQ(favorite->feature, feature::FAVORITE);
  EXPECT_EQ(favorite->main_parameter, 0xD800);
  EXPECT_STREQ(favorite->name, "FAVORITE");

  const NamedPosition *vent = find_named_position(CoverCommand::VENT);
  ASSERT_NE(vent, nullptr);
  EXPECT_EQ(vent->feature, feature::VENT);
  EXPECT_EQ(vent->main_parameter, 0xD803);
  EXPECT_STREQ(vent->name, "VENT");

  EXPECT_EQ(find_named_position(CoverCommand::STOP), nullptr);
  EXPECT_EQ(find_named_position(CoverCommand::FORCE_OPEN), nullptr);
}

TEST(NamedPositions, ACommandMainParameterIsStopOrANamedPositionAndNothingElse) {
  uint16_t main_parameter = 0;
  ASSERT_TRUE(command_main_parameter(CoverCommand::STOP, main_parameter));
  EXPECT_EQ(main_parameter, 0xD200);
  for (const NamedPosition &named : NAMED_POSITIONS) {
    ASSERT_TRUE(command_main_parameter(named.command, main_parameter)) << named.name;
    EXPECT_EQ(main_parameter, named.main_parameter) << named.name;
  }
  main_parameter = 0x1234;
  EXPECT_FALSE(command_main_parameter(CoverCommand::FORCE_OPEN, main_parameter));
  EXPECT_EQ(main_parameter, 0x1234) << "a refused command leaves the value alone";
}

TEST(NamedPositions, EntryNamesAreWhatCoverCommandNameReturns) {
  for (const NamedPosition &named : NAMED_POSITIONS)
    EXPECT_STREQ(named.name, cover_command_name(named.command));
}

TEST(NamedPositions, EachEntryIsWhatTheExecuteBuilderSendsForItsCommand) {
  for (const NamedPosition &named : NAMED_POSITIONS) {
    IoFrame frame{};
    ASSERT_TRUE(create_execute_command(frame, test::OWN_ID, test::DST_ID, false, named.command)) << named.name;
    EXPECT_EQ(main_parameter_of(frame.data[EXECUTE_MAIN_BYTE_OFFSET], frame.data[EXECUTE_MAIN_BYTE_OFFSET + 1]),
              named.main_parameter)
        << named.name;
  }
}

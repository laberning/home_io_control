/// @file device_features_parity_test.cpp
/// @brief The capability bits of the node-type table against the type-keyed helpers they replace,
///        and the value checks, named-position table and undeclared-kind defaults beside them.
/// @ingroup hioc_protocol

#include "proto_commands.h"
#include "proto_device_features.h"
#include "proto_device_model.h"
#include "proto_device_types.h"
#include "proto_heating_profiles.h"

#include "device_table_walk.h"
#include "test_helpers.h"

#include <set>
#include <utility>

using namespace esphome::home_io_control;
namespace feature = esphome::home_io_control::device_feature;

namespace {

/// Every (type, subtype) the parity checks visit: each table row, plus subtypes 0 and 58 of every
/// type that has a row, since the lookup answers those without a row of their own.
std::set<std::pair<DeviceType, uint8_t>> kinds_under_test() {
  std::set<std::pair<DeviceType, uint8_t>> kinds;
  test::for_each_table_row([&kinds](const DeviceTypeInfo &row) {
    kinds.emplace(row.type, row.subtype);
    kinds.emplace(row.type, 0);
    kinds.emplace(row.type, SUBTYPE_MAIN_PARAMETER_ONLY);
  });
  return kinds;
}

std::string kind_label(DeviceType type, uint8_t subtype) { return format_device_ids(type, subtype); }

/// Types whose type-only device_supports_tilt() answers true.
bool is_tilting_type(DeviceType type) {
  return type == DeviceType::INTERIOR_VENETIAN_BLIND || type == DeviceType::VERTICAL_INTERIOR_BLIND ||
         type == DeviceType::EXTERIOR_VENETIAN_BLIND || type == DeviceType::LOUVRE_BLIND;
}

}  // namespace

// ============================================================================
// Parity with the helpers the bits replace
// ============================================================================

TEST(DeviceFeaturesParity, EveryTypeKeyedHelperAgreesWithTheRowBits) {
  for (const auto &[type, subtype] : kinds_under_test()) {
    SCOPED_TRACE(kind_label(type, subtype));
    const DeviceFeatures bits = device_features(type, subtype);

    // Known difference: a subtype-58 variant has no functional parameters, so it has no slat angle,
    // where the type-only helper still answers for the whole type (KLF 200 Appendix 2).
    const bool tilt_expected = device_supports_tilt(type) && subtype != SUBTYPE_MAIN_PARAMETER_ONLY;
    EXPECT_EQ(has_feature(bits, feature::TILT), tilt_expected);
    if (is_tilting_type(type) && subtype == SUBTYPE_MAIN_PARAMETER_ONLY) {
      EXPECT_TRUE(device_supports_tilt(type)) << "the listed difference: the type-only helper says yes at 58";
    }

    EXPECT_EQ(has_feature(bits, feature::VENT), device_supports_vent(type));
    EXPECT_EQ(has_feature(bits, feature::INVERTED), default_inverted_for_type(type));
    EXPECT_EQ(has_feature(bits, feature::MAIN_PARAMETER_ONLY), device_takes_main_parameter_only(type, subtype));

    if (type == DeviceType::LIGHT) {
      EXPECT_EQ(has_feature(bits, feature::LEVELS), device_light_is_brightness_only(type, subtype));
    }
    // The fan platform offers speeds on every exterior-heating subtype except 58.
    if (type == DeviceType::EXTERIOR_HEATING) {
      EXPECT_EQ(has_feature(bits, feature::LEVELS), subtype != SUBTYPE_MAIN_PARAMETER_ONLY);
    }

    const HeatingProfile *profile = find_heating_profile(type, subtype);
    EXPECT_EQ(has_feature(bits, feature::WINDOW_COMPANIONS), profile != nullptr && profile->features.window_sensor);
  }
}

TEST(DeviceFeaturesParity, ExtendedPositionFrameIsTheNegationOfMainParameterOnlyWhereAPositionIsSent) {
  for (const auto &[type, subtype] : kinds_under_test()) {
    SCOPED_TRACE(kind_label(type, subtype));
    if (!device_has_platform(type, subtype))
      continue;
    const DeviceCapabilityClass capability = device_capability_class(type, subtype);
    const bool sends_position =
        capability == DeviceCapabilityClass::COVER || capability == DeviceCapabilityClass::LIGHT ||
        capability == DeviceCapabilityClass::SWITCH || capability == DeviceCapabilityClass::LOCK ||
        capability == DeviceCapabilityClass::HEATER_LEVEL;
    const DeviceFeatures bits = device_features(type, subtype);
    if (sends_position) {
      EXPECT_EQ(has_feature(bits, feature::EXTENDED_POSITION_FRAME), !device_takes_main_parameter_only(type, subtype));
    } else {
      EXPECT_FALSE(has_feature(bits, feature::EXTENDED_POSITION_FRAME));
    }
  }
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

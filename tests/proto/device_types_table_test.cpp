/// @file device_types_table_test.cpp
/// @brief Tests for the node-type / subtype table (proto_device_types.h): lookups, names, platform
///        and behaviour flags, and the 10-bit type field end to end.

#include "proto_codecs.h"
#include "proto_device_model.h"
#include "proto_device_types.h"
#include "proto_heating_profiles.h"

#include <algorithm>

#include "device_table_walk.h"
#include "test_helpers.h"

using namespace esphome::home_io_control;

// ============================================================================
// 10-bit node type survives the wire decode
// ============================================================================

TEST(DeviceTypesTable, RemoteControllerDecodesAsTypeNot255) {
  EXPECT_EQ(decode_packed_device_type(0xFF, 0xC0), DeviceType::REMOTE_CONTROLLER)
      << "packed bytes FF C0 are node type 1023, not the low byte 255";
  EXPECT_EQ(decode_packed_device_subtype(0xC0), 0u);
}

TEST(DeviceTypesTable, TestTypeDecodesAs1008) {
  EXPECT_EQ(decode_packed_device_type(0xFC, 0x00), DeviceType::TEST_AND_EVALUATION);
  EXPECT_EQ(decode_packed_device_type(0x3F, 0xC0), DeviceType::CENTRAL_HOUSE_CONTROL)
      << "255 is a type of its own (packed 3F C0), distinct from 1023";
}

TEST(DeviceTypesTable, PackedTypeAndSubtypeRoundTripAcrossTheTenBitRange) {
  for (const DeviceType type :
       {DeviceType::ROLLER_SHUTTER, DeviceType::ELECTRICAL_HEATER, DeviceType::CENTRAL_HOUSE_CONTROL,
        DeviceType::TEST_AND_EVALUATION, DeviceType::REMOTE_CONTROLLER}) {
    for (const uint8_t subtype : {0, 1, 2, 58}) {
      uint8_t msb = 0;
      uint8_t low = 0;
      encode_packed_device_type(type, subtype, msb, low);
      EXPECT_EQ(decode_packed_device_type(msb, low), type) << static_cast<unsigned>(type) << "/" << +subtype;
      EXPECT_EQ(decode_packed_device_subtype(low), subtype);
    }
  }
}

TEST(DeviceTypesTable, EncodeDropsBitsAboveTheWireField) {
  uint8_t msb = 0;
  uint8_t low = 0;
  encode_packed_device_type(static_cast<DeviceType>(0x402), 5, msb, low);  // bit 10 is outside the field
  EXPECT_EQ(decode_packed_device_type(msb, low), DeviceType::ROLLER_SHUTTER) << "must not alias into the subtype bits";
  EXPECT_EQ(decode_packed_device_subtype(low), 5u);
}

TEST(DeviceTypesTable, TypedBroadcastAddressCarriesTheFullTypeAndRoundTrips) {
  uint8_t addr[NODE_ID_SIZE]{};
  encode_broadcast_address(DeviceType::REMOTE_CONTROLLER, addr);
  EXPECT_EQ(addr[0], 0x00);
  EXPECT_EQ(addr[1], 0xFF);
  EXPECT_EQ(addr[2], 0xFF) << "low type bits 11 plus the all-subtypes mask 3F";
  EXPECT_EQ(broadcast_target_type(addr), DeviceType::REMOTE_CONTROLLER);

  encode_broadcast_address(DeviceType::ELECTRICAL_HEATER, addr);
  EXPECT_EQ(broadcast_target_type(addr), DeviceType::ELECTRICAL_HEATER);
}

// ============================================================================
// Lookups
// ============================================================================

TEST(DeviceTypesTable, ExactRowWinsAndAnUnnamedSubtypeFallsBackToTheTypeRow) {
  const DeviceTypeInfo *exact = find_device_type_info(DeviceType::ELECTRICAL_HEATER, 1);
  ASSERT_NE(exact, nullptr);
  EXPECT_EQ(exact->subtype, 1u);

  const DeviceTypeInfo *fallback = find_device_type_info(DeviceType::ELECTRICAL_HEATER, 9);
  ASSERT_NE(fallback, nullptr);
  EXPECT_EQ(fallback->subtype, 0u) << "subtype 9 has no row of its own";

  EXPECT_EQ(find_exact_device_type_info(DeviceType::ELECTRICAL_HEATER, 9), nullptr);
  EXPECT_EQ(find_device_type_info(static_cast<DeviceType>(26), 0), nullptr) << "26 is undefined";
}

TEST(DeviceTypesTable, EveryRowIsReachableAndTypeRowsComeFirst) {
  // A subtype row for a type with no type row of its own would be unreachable by name.
  for (const DeviceType type : {DeviceType::ROLLER_SHUTTER, DeviceType::LIGHT, DeviceType::LOCK, DeviceType::HEAT_PUMP,
                                DeviceType::REMOTE_CONTROLLER}) {
    EXPECT_NE(find_exact_device_type_info(type, 0), nullptr) << static_cast<unsigned>(type);
  }
}

TEST(DeviceTypesTable, YamlSelectableMatchesTheSchemaNames) {
  EXPECT_STREQ(yaml_device_type_name(DeviceType::ELECTRICAL_HEATER), "electrical_heater");
  EXPECT_STREQ(yaml_device_type_name(DeviceType::VERTICAL_EXTERIOR_AWNING), "vertical_exterior_awning");
  EXPECT_STREQ(yaml_device_type_name(DeviceType::DUAL_ROLLER_SHUTTER), "dual_roller_shutter");
  EXPECT_EQ(yaml_device_type_name(DeviceType::SECURE_CONFIGURATION_DEVICE), nullptr);
  EXPECT_EQ(yaml_device_type_name(DeviceType::REMOTE_CONTROLLER), nullptr);
  EXPECT_EQ(yaml_device_type_name(DeviceType::VENTILATION_POINT), nullptr);
}

// ============================================================================
// Names
// ============================================================================

TEST(DeviceTypesTable, KindNamesFollowTypeCommaSubtype) {
  EXPECT_EQ(format_device_kind(DeviceType::ELECTRICAL_HEATER, 1), "electrical heater, with adjustable setpoint");
  EXPECT_EQ(format_device_kind(DeviceType::ELECTRICAL_HEATER, 0), "electrical heater");
  EXPECT_EQ(format_device_kind(DeviceType::ROLLER_SHUTTER, 2), "roller shutter, with projection");
  EXPECT_EQ(format_device_kind(DeviceType::GARAGE_OPENER, 58), "garage opener, open/close only");
  EXPECT_EQ(format_device_kind(DeviceType::ROLLER_SHUTTER, 33), "roller shutter, subtype 33")
      << "a subtype without a name prints its number";
  EXPECT_EQ(format_device_kind(DeviceType::LOCK, 1), "window lock") << "a standalone name is not appended";
  EXPECT_EQ(format_device_kind(DeviceType::HEAT_PUMP, 2), "boiler (Atlantic)");
  EXPECT_EQ(format_device_kind(DeviceType::REMOTE_CONTROLLER, 0), "remote controller");
  EXPECT_EQ(format_device_kind(static_cast<DeviceType>(26), 0), "unknown type 0x1A");
  EXPECT_EQ(format_device_kind(static_cast<DeviceType>(26), 3), "unknown type 0x1A, subtype 3");
}

TEST(DeviceTypesTable, WireIdsPrintTypeAndSubtype) {
  EXPECT_EQ(format_device_ids(DeviceType::ELECTRICAL_HEATER, 1), "0x34/1");
  EXPECT_EQ(format_device_ids(DeviceType::REMOTE_CONTROLLER, 0), "0x3FF/0");
}

TEST(DeviceTypesTable, DiagnosticAndYamlFormsPrintTheFullTypeValue) {
  EXPECT_EQ(format_device_type_diagnostic(DeviceType::REMOTE_CONTROLLER), "remote_controller (0x3FF)");
  EXPECT_EQ(format_device_type_for_yaml(DeviceType::REMOTE_CONTROLLER), "0x3FF");
  EXPECT_EQ(format_device_type_for_yaml(DeviceType::ELECTRICAL_HEATER), "\"electrical_heater\"");
}

// ============================================================================
// Platform, capability and behaviour flags
// ============================================================================

TEST(DeviceTypesTable, PlatformNameFollowsTheRowNotTheType) {
  EXPECT_STREQ(device_platform_name(DeviceType::ELECTRICAL_HEATER, 1), "climate");
  EXPECT_STREQ(device_platform_name(DeviceType::ELECTRICAL_HEATER, 0), "climate");
  EXPECT_STREQ(device_platform_name(DeviceType::HEAT_PUMP, 0), "climate");
  EXPECT_STREQ(device_platform_name(DeviceType::HEATING_TEMPERATURE_INTERFACE, 0), "climate");
  EXPECT_EQ(device_platform_name(DeviceType::HEAT_PUMP, 1), nullptr);
  EXPECT_STREQ(device_platform_name(DeviceType::EXTERIOR_HEATING, 0), "fan");
  EXPECT_STREQ(device_platform_name(DeviceType::EXTERIOR_HEATING, 58), "fan");
  EXPECT_STREQ(device_platform_name(DeviceType::ROLLER_SHUTTER, 1), "cover");
  EXPECT_STREQ(device_platform_name(DeviceType::LIGHT, 58), "light");
  EXPECT_STREQ(device_platform_name(DeviceType::ON_OFF_SWITCH, 2), "switch");
  EXPECT_STREQ(device_platform_name(DeviceType::LOCK, 1), "lock");
  EXPECT_EQ(device_platform_name(DeviceType::LOCK, 2), nullptr) << "car controller has no platform";
  for (const DeviceType type : {DeviceType::VENTILATION_POINT, DeviceType::INTRUSION_ALARM,
                                DeviceType::SECURE_CONFIGURATION_DEVICE, DeviceType::REMOTE_CONTROLLER}) {
    EXPECT_FALSE(device_has_platform(type, 0)) << static_cast<unsigned>(type);
  }
  EXPECT_FALSE(device_has_platform(DeviceType::HEAT_PUMP, 1)) << "Atlantic profile: other register family";
  EXPECT_FALSE(device_has_platform(DeviceType::HEAT_PUMP, 2));
}

TEST(DeviceTypesTable, CapabilityClassIsSubtypeAware) {
  EXPECT_EQ(device_capability_class(DeviceType::ELECTRICAL_HEATER, 1), DeviceCapabilityClass::CLIMATE);
  EXPECT_EQ(device_capability_class(DeviceType::HEAT_PUMP, 0), DeviceCapabilityClass::CLIMATE);
  EXPECT_EQ(device_capability_class(DeviceType::HEAT_PUMP, 1), DeviceCapabilityClass::UNKNOWN)
      << "the Atlantic profiles must not be driven by the heating payloads";
  EXPECT_EQ(device_capability_class(DeviceType::HEAT_PUMP, 2), DeviceCapabilityClass::UNKNOWN);
  EXPECT_EQ(device_capability_class(DeviceType::SLIDING_WINDOW, 0), DeviceCapabilityClass::COVER);
  EXPECT_EQ(device_capability_class(static_cast<DeviceType>(26), 0), DeviceCapabilityClass::UNKNOWN);
}

TEST(DeviceTypesTable, MainParameterOnlyForSubtype58OfAnyTypeAndEveryGarageOpener) {
  const auto main_parameter_only = [](DeviceType type, uint8_t subtype) {
    return has_feature(device_features(type, subtype), device_feature::MAIN_PARAMETER_ONLY);
  };
  EXPECT_TRUE(main_parameter_only(DeviceType::GARAGE_OPENER, 58));
  EXPECT_TRUE(main_parameter_only(DeviceType::GATE_OPENER, 58));
  EXPECT_TRUE(main_parameter_only(DeviceType::EXTERIOR_HEATING, 58));
  EXPECT_TRUE(main_parameter_only(DeviceType::EXTERIOR_HEATING, 0)) << "every exterior heater";
  EXPECT_TRUE(main_parameter_only(DeviceType::SWINGING_SHUTTER, 58));
  EXPECT_TRUE(main_parameter_only(DeviceType::ROLLER_SHUTTER, 58))
      << "subtype 58 is defined on every type, with or without a row of its own";
  EXPECT_TRUE(main_parameter_only(DeviceType::GARAGE_OPENER, 0)) << "the issue #163 behaviour stays";
  EXPECT_FALSE(main_parameter_only(DeviceType::ROLLER_SHUTTER, 0));
  EXPECT_FALSE(main_parameter_only(DeviceType::GATE_OPENER, 0));
  EXPECT_FALSE(main_parameter_only(DeviceType::SWINGING_SHUTTER, 1));
}

TEST(DeviceTypesTable, DimmableLightRowsAreTheRgbAndTunableWhiteSubtypes) {
  const auto has_levels = [](DeviceType type, uint8_t subtype) {
    return has_feature(device_features(type, subtype), device_feature::LEVELS);
  };
  EXPECT_TRUE(has_levels(DeviceType::LIGHT, 1));
  EXPECT_TRUE(has_levels(DeviceType::LIGHT, 2));
  EXPECT_FALSE(has_levels(DeviceType::LIGHT, 0)) << "the plain light row leaves levels to `dimmable:`";
  EXPECT_FALSE(has_levels(DeviceType::LIGHT, 58));
}

// ============================================================================
// Paste-ready YAML
// ============================================================================

TEST(DeviceTypesTable, HeaterWithSetpointGetsAClimateSnippet) {
  const std::string snippet = build_device_yaml_snippet(DeviceType::ELECTRICAL_HEATER, 1, "AABBCC",
                                                        /*metadata_complete=*/true, /*low_power=*/false);
  EXPECT_NE(snippet.find("  climate:\n"), std::string::npos) << snippet;
  EXPECT_NE(snippet.find("io_device_type: \"electrical_heater\""), std::string::npos) << snippet;
  EXPECT_NE(snippet.find("io_subtype: 1"), std::string::npos) << snippet;
  EXPECT_EQ(snippet.find("invert_position"), std::string::npos);
}

TEST(DeviceTypesTable, SnippetCarriesTheDeviceKeyOnlyWhenOneIsGiven) {
  const std::string key = "00112233445566778899AABBCCDDEEFF";
  const std::string keyed = build_device_yaml_snippet(DeviceType::ELECTRICAL_HEATER, 1, "AABBCC", true, false, key);
  EXPECT_NE(keyed.find("    system_key: \"" + key + "\"\n"), std::string::npos) << keyed;
  EXPECT_NE(keyed.find("io_subtype: 1"), std::string::npos);

  const std::string plain = build_device_yaml_snippet(DeviceType::ELECTRICAL_HEATER, 1, "AABBCC", true, false);
  EXPECT_EQ(plain.find("system_key"), std::string::npos);

  const std::string no_metadata = build_device_yaml_snippet(DeviceType::UNKNOWN, 0, "AABBCC", false, false, key);
  EXPECT_NE(no_metadata.find("system_key: \"" + key + "\""), std::string::npos)
      << "the key also rides in the shape used when the device withheld its type";
}

TEST(DeviceTypesTable, PlainHeaterGetsAClimateSnippet) {
  const std::string snippet = build_device_yaml_snippet(DeviceType::ELECTRICAL_HEATER, 0, "AABBCC", true, false);
  EXPECT_NE(snippet.find("  climate:\n"), std::string::npos) << snippet;
  EXPECT_NE(snippet.find("io_subtype: 0"), std::string::npos) << snippet;
}

TEST(DeviceTypesTable, ExteriorHeatingGetsAFanSnippetForBothSubtypes) {
  for (const uint8_t subtype : {0, 58}) {
    const std::string snippet = build_device_yaml_snippet(DeviceType::EXTERIOR_HEATING, subtype, "AABBCC", true, false);
    EXPECT_NE(snippet.find("  fan:\n"), std::string::npos) << snippet;
    EXPECT_NE(snippet.find("io_device_type: \"exterior_heating\""), std::string::npos) << snippet;
    EXPECT_NE(snippet.find("io_subtype: " + std::to_string(subtype)), std::string::npos) << snippet;
  }
}

TEST(DeviceTypesTable, HeatPumpAndRemoteControllerGetNoSnippet) {
  EXPECT_TRUE(build_device_yaml_snippet(DeviceType::HEAT_PUMP, 1, "AABBCC", true, false).empty());
  EXPECT_TRUE(build_device_yaml_snippet(DeviceType::REMOTE_CONTROLLER, 0, "AABBCC", true, false).empty());
}

TEST(DeviceTypesTable, BrightnessOnlyLightSnippetSetsDimmable) {
  const std::string rgb = build_device_yaml_snippet(DeviceType::LIGHT, 1, "AABBCC", true, false);
  EXPECT_NE(rgb.find("  light:\n"), std::string::npos) << rgb;
  EXPECT_NE(rgb.find("dimmable: true"), std::string::npos) << rgb;

  const std::string onoff = build_device_yaml_snippet(DeviceType::LIGHT, 58, "AABBCC", true, false);
  EXPECT_NE(onoff.find("  light:\n"), std::string::npos) << onoff;
  EXPECT_EQ(onoff.find("dimmable"), std::string::npos) << onoff;

  const std::string plain = build_device_yaml_snippet(DeviceType::LIGHT, 0, "AABBCC", true, false);
  EXPECT_EQ(plain.find("dimmable"), std::string::npos) << plain;
}

TEST(DeviceTypesTable, UnnamedSubtypeOfACoverStillGetsACoverSnippet) {
  const std::string snippet = build_device_yaml_snippet(DeviceType::ROLLER_SHUTTER, 33, "AABBCC", true, false);
  EXPECT_NE(snippet.find("  cover:\n"), std::string::npos) << snippet;
  EXPECT_NE(snippet.find("io_subtype: 33"), std::string::npos) << snippet;
}

// ============================================================================
// Capability-bit invariants
// ============================================================================

TEST(DeviceTypesTableFeatures, TiltImpliesLevels) {
  test::for_each_table_row([](const DeviceTypeInfo &row) {
    if (has_feature(row.features, device_feature::TILT)) {
      EXPECT_TRUE(has_feature(row.features, device_feature::LEVELS)) << format_device_ids(row.type, row.subtype);
    }
  });
}

TEST(DeviceTypesTableFeatures, MainParameterOnlyHasNeitherTiltNorTheExtendedPositionFrame) {
  test::for_each_table_row([](const DeviceTypeInfo &row) {
    if (!has_feature(row.features, device_feature::MAIN_PARAMETER_ONLY))
      return;
    EXPECT_FALSE(has_feature(row.features, device_feature::TILT)) << format_device_ids(row.type, row.subtype);
    EXPECT_FALSE(has_feature(row.features, device_feature::EXTENDED_POSITION_FRAME))
        << format_device_ids(row.type, row.subtype);
  });
}

TEST(DeviceTypesTableFeatures, ASubtypeRowKeepsItsTypesCapabilityClassUnlessItHasNone) {
  test::for_each_table_row([](const DeviceTypeInfo &row) {
    if (row.subtype == 0 || row.capability == DeviceCapabilityClass::UNKNOWN)
      return;
    EXPECT_EQ(row.capability, device_capability_class(row.type, 0)) << format_device_ids(row.type, row.subtype);
  });
}

TEST(DeviceTypesTableFeatures, NoRowUsesBit15) {
  test::for_each_table_row([](const DeviceTypeInfo &row) {
    EXPECT_EQ(row.features & (1U << 15U), 0u) << "bit 15 is the Python mirror's experimental marker";
  });
}

TEST(DeviceTypesTableFeatures, AnExplicitSubtype58RowIsTheVariantOfItsTypeRow) {
  test::for_each_table_row([](const DeviceTypeInfo &row) {
    if (row.subtype != SUBTYPE_MAIN_PARAMETER_ONLY)
      return;
    EXPECT_EQ(row.features, main_parameter_only_variant(device_features(row.type, 0)))
        << format_device_ids(row.type, row.subtype);
  });
}

TEST(DeviceTypesTableFeatures, GarageOpenerRowsHaveNoFavourite) {
  // Issue #163: the garage opener refuses the favourite alias.
  test::for_each_table_row([](const DeviceTypeInfo &row) {
    if (row.type == DeviceType::GARAGE_OPENER) {
      EXPECT_FALSE(has_feature(row.features, device_feature::FAVORITE)) << format_device_ids(row.type, row.subtype);
    }
  });
}

TEST(DeviceTypesTableFeatures, NamedPositionValuesAreUniqueAboveTheClosedPositionAndOwnOneBitEach) {
  DeviceFeatures bits_seen = 0;
  for (const NamedPosition &named : NAMED_POSITIONS) {
    EXPECT_GT(named.main_parameter, MAIN_PARAMETER_CLOSED) << named.name;
    EXPECT_EQ(bits_seen & named.feature, 0u) << named.name << ": the bit is in more than one entry";
    bits_seen |= named.feature;
    for (const NamedPosition &other : NAMED_POSITIONS) {
      if (&other != &named) {
        EXPECT_NE(other.main_parameter, named.main_parameter) << named.name << " and " << other.name;
      }
    }
  }
}

TEST(DeviceTypesTable, TheSnippetSuggestsInvertedAndDimmableFromTheKindsFeatures) {
  EXPECT_NE(
      build_device_yaml_snippet(DeviceType::HORIZONTAL_AWNING, 0, "AABBCC", true, false).find("invert_position: true"),
      std::string::npos);
  EXPECT_NE(build_device_yaml_snippet(DeviceType::LIGHT, 1, "AABBCC", true, false).find("dimmable: true"),
            std::string::npos);
  EXPECT_EQ(build_device_yaml_snippet(DeviceType::LIGHT, 0, "AABBCC", true, false).find("dimmable"), std::string::npos);
  EXPECT_EQ(build_device_yaml_snippet(DeviceType::LIGHT, 58, "AABBCC", true, false).find("dimmable"),
            std::string::npos);
  EXPECT_EQ(build_device_yaml_snippet(DeviceType::ROLLER_SHUTTER, 0, "AABBCC", true, false).find("invert_position"),
            std::string::npos);
}

TEST(DeviceTypesTableFeatures, TiltNamedPositionsAndInversionBelongToCoverRowsOnly) {
  const DeviceFeatures cover_only = device_feature::TILT | device_feature::FAVORITE | device_feature::VENT |
                                    device_feature::PARTIAL | device_feature::PEDESTRIAN | device_feature::INVERTED;
  test::for_each_table_row([cover_only](const DeviceTypeInfo &row) {
    if ((row.features & cover_only) != 0) {
      EXPECT_EQ(row.capability, DeviceCapabilityClass::COVER) << format_device_ids(row.type, row.subtype);
    }
  });
}

TEST(DeviceTypesTableFeatures,
     WindowCompanionsAreExactlyWhereTheHeatingProfilePollsTheWindowAndOffersTheDetectionSwitch) {
  const auto contains = [](const HeatingFunction *list, size_t count, HeatingFunction fn) {
    return std::find(list, list + count, fn) != list + count;
  };
  test::for_each_table_row([&contains](const DeviceTypeInfo &row) {
    const HeatingProfile *profile = find_heating_profile(row.type, row.subtype);
    const bool profile_has_window =
        profile != nullptr &&
        contains(profile->poll_registers, profile->poll_register_count, HeatingFunction::WINDOW_OPEN) &&
        contains(profile->boot_registers, profile->boot_register_count, HeatingFunction::SET_OPEN_WINDOW_DETECTION);
    EXPECT_EQ(has_feature(row.features, device_feature::WINDOW_COMPANIONS), profile_has_window)
        << format_device_ids(row.type, row.subtype);
  });
}

TEST(DeviceTypesTableFeatures, TheExtendedPositionFrameIsTheNegationOfMainParameterOnlyWhereAPositionIsSent) {
  test::for_each_table_row([](const DeviceTypeInfo &row) {
    if (!device_has_platform(row.type, row.subtype))
      return;
    const DeviceCapabilityClass capability = row.capability;
    const bool sends_position =
        capability == DeviceCapabilityClass::COVER || capability == DeviceCapabilityClass::LIGHT ||
        capability == DeviceCapabilityClass::SWITCH || capability == DeviceCapabilityClass::LOCK ||
        capability == DeviceCapabilityClass::HEATER_LEVEL;
    EXPECT_EQ(has_feature(row.features, device_feature::EXTENDED_POSITION_FRAME),
              sends_position && !has_feature(row.features, device_feature::MAIN_PARAMETER_ONLY))
        << format_device_ids(row.type, row.subtype);
  });
}

TEST(DeviceTypesTableFeatures, EveryRowWithCapabilitiesHasAYamlSelectableType) {
  test::for_each_table_row([](const DeviceTypeInfo &row) {
    if (row.features == 0)
      return;
    const DeviceTypeInfo *type_row = find_exact_device_type_info(row.type, 0);
    ASSERT_NE(type_row, nullptr) << format_device_ids(row.type, row.subtype);
    EXPECT_NE(type_row->flags & device_type_flags::YAML_SELECTABLE, 0u)
        << format_device_ids(row.type, row.subtype) << ": the Python mirror can only name a YAML-selectable type";
  });
}

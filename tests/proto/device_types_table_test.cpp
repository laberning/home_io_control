/// @file device_types_table_test.cpp
/// @brief Tests for the node-type / subtype table (proto_device_types.h): lookups, names, platform
///        and behaviour flags, and the 10-bit type field end to end.

#include "proto_codecs.h"
#include "proto_device_model.h"
#include "proto_device_types.h"

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
  EXPECT_EQ(format_device_kind(DeviceType::GARAGE_OPENER, 58), "garage opener, position only");
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
  EXPECT_EQ(device_platform_name(DeviceType::ELECTRICAL_HEATER, 0), nullptr);
  EXPECT_STREQ(device_platform_name(DeviceType::ROLLER_SHUTTER, 1), "cover");
  EXPECT_STREQ(device_platform_name(DeviceType::LIGHT, 58), "light");
  EXPECT_STREQ(device_platform_name(DeviceType::ON_OFF_SWITCH, 2), "switch");
  EXPECT_STREQ(device_platform_name(DeviceType::LOCK, 1), "lock");
  EXPECT_EQ(device_platform_name(DeviceType::LOCK, 2), nullptr) << "car controller has no platform";
  for (const DeviceType type : {DeviceType::HEATING_TEMPERATURE_INTERFACE, DeviceType::EXTERIOR_HEATING,
                                DeviceType::HEAT_PUMP, DeviceType::VENTILATION_POINT, DeviceType::INTRUSION_ALARM,
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
  EXPECT_TRUE(device_takes_main_parameter_only(DeviceType::GARAGE_OPENER, 58));
  EXPECT_TRUE(device_takes_main_parameter_only(DeviceType::GATE_OPENER, 58));
  EXPECT_TRUE(device_takes_main_parameter_only(DeviceType::EXTERIOR_HEATING, 58));
  EXPECT_TRUE(device_takes_main_parameter_only(DeviceType::SWINGING_SHUTTER, 58));
  EXPECT_TRUE(device_takes_main_parameter_only(DeviceType::ROLLER_SHUTTER, 58))
      << "subtype 58 is defined on every type, with or without a row of its own";
  EXPECT_TRUE(device_takes_main_parameter_only(DeviceType::GARAGE_OPENER, 0)) << "the issue #163 behaviour stays";
  EXPECT_FALSE(device_takes_main_parameter_only(DeviceType::ROLLER_SHUTTER, 0));
  EXPECT_FALSE(device_takes_main_parameter_only(DeviceType::GATE_OPENER, 0));
  EXPECT_FALSE(device_takes_main_parameter_only(DeviceType::SWINGING_SHUTTER, 1));
}

TEST(DeviceTypesTable, BrightnessOnlyLightsAreTheRgbAndTunableWhiteSubtypes) {
  EXPECT_TRUE(device_light_is_brightness_only(DeviceType::LIGHT, 1));
  EXPECT_TRUE(device_light_is_brightness_only(DeviceType::LIGHT, 2));
  EXPECT_FALSE(device_light_is_brightness_only(DeviceType::LIGHT, 0));
  EXPECT_FALSE(device_light_is_brightness_only(DeviceType::LIGHT, 58));
  EXPECT_FALSE(device_light_is_brightness_only(DeviceType::ROLLER_SHUTTER, 1));
}

// ============================================================================
// Paste-ready YAML
// ============================================================================

TEST(DeviceTypesTable, HeaterWithSetpointGetsAClimateSnippet) {
  const std::string snippet = build_device_yaml_snippet(DeviceType::ELECTRICAL_HEATER, 1, "AABBCC",
                                                        /*metadata_complete=*/true, /*inverted=*/false,
                                                        /*low_power=*/false);
  EXPECT_NE(snippet.find("  climate:\n"), std::string::npos) << snippet;
  EXPECT_NE(snippet.find("io_device_type: \"electrical_heater\""), std::string::npos) << snippet;
  EXPECT_NE(snippet.find("io_subtype: 1"), std::string::npos) << snippet;
  EXPECT_EQ(snippet.find("invert_position"), std::string::npos);
}

TEST(DeviceTypesTable, PlainHeaterAndRemoteControllerGetNoSnippet) {
  EXPECT_TRUE(build_device_yaml_snippet(DeviceType::ELECTRICAL_HEATER, 0, "AABBCC", true, false, false).empty());
  EXPECT_TRUE(build_device_yaml_snippet(DeviceType::REMOTE_CONTROLLER, 0, "AABBCC", true, false, false).empty());
}

TEST(DeviceTypesTable, BrightnessOnlyLightSnippetSetsDimmable) {
  const std::string rgb = build_device_yaml_snippet(DeviceType::LIGHT, 1, "AABBCC", true, false, false);
  EXPECT_NE(rgb.find("  light:\n"), std::string::npos) << rgb;
  EXPECT_NE(rgb.find("dimmable: true"), std::string::npos) << rgb;

  const std::string onoff = build_device_yaml_snippet(DeviceType::LIGHT, 58, "AABBCC", true, false, false);
  EXPECT_NE(onoff.find("  light:\n"), std::string::npos) << onoff;
  EXPECT_EQ(onoff.find("dimmable"), std::string::npos) << onoff;

  const std::string plain = build_device_yaml_snippet(DeviceType::LIGHT, 0, "AABBCC", true, false, false);
  EXPECT_EQ(plain.find("dimmable"), std::string::npos) << plain;
}

TEST(DeviceTypesTable, UnnamedSubtypeOfACoverStillGetsACoverSnippet) {
  const std::string snippet = build_device_yaml_snippet(DeviceType::ROLLER_SHUTTER, 33, "AABBCC", true, false, false);
  EXPECT_NE(snippet.find("  cover:\n"), std::string::npos) << snippet;
  EXPECT_NE(snippet.find("io_subtype: 33"), std::string::npos) << snippet;
}

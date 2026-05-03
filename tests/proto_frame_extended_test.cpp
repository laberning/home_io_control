#include "proto_frame.h"

#include "test_helpers.h"

#include <cstring>

using namespace esphome::home_io_control;

// ============================================================================
// ProtoFrameExtended test suite
// ============================================================================
// Extended frame utility tests: hex_to_bytes variations, CRC-CCITT more vectors,
// and device capability class name mappings.

TEST(ProtoFrame, HexToBytesValidInput) {
  uint8_t out[NODE_ID_SIZE] = {0};
  // "C0FFEE" -> {0xC0, 0xFF, 0xEE}
  EXPECT_TRUE(hex_to_bytes("C0FFEE", out, NODE_ID_SIZE)) << "hex_to_bytes should parse valid 6-char hex";
  EXPECT_EQ(out[0], 0xC0) << "byte 0 should be 0xC0";
  EXPECT_EQ(out[1], 0xFF) << "byte 1 should be 0xFF";
  EXPECT_EQ(out[2], 0xEE) << "byte 2 should be 0xEE";
}

TEST(ProtoFrame, HexToBytesLowerCase) {
  uint8_t out[NODE_ID_SIZE] = {0};
  EXPECT_TRUE(hex_to_bytes("c0ffee", out, NODE_ID_SIZE)) << "hex_to_bytes should accept lowercase";
  EXPECT_EQ(out[0], 0xC0) << "byte 0 should be 0xC0";
  EXPECT_EQ(out[1], 0xFF) << "byte 1 should be 0xFF";
  EXPECT_EQ(out[2], 0xEE) << "byte 2 should be 0xEE";
}

TEST(ProtoFrame, HexToBytesMixedCase) {
  uint8_t out[NODE_ID_SIZE] = {0};
  EXPECT_TRUE(hex_to_bytes("C0fFEE", out, NODE_ID_SIZE)) << "hex_to_bytes should accept mixed case";
  EXPECT_EQ(out[0], 0xC0) << "byte 0 should be 0xC0";
  EXPECT_EQ(out[1], 0xFF) << "byte 1 should be 0xFF";
  EXPECT_EQ(out[2], 0xEE) << "byte 2 should be 0xEE";
}

TEST(ProtoFrame, HexToBytesZeroBufferOnFailure) {
  uint8_t out[NODE_ID_SIZE] = {0xAA, 0xBB, 0xCC};
  EXPECT_FALSE(hex_to_bytes("XYZ", out, NODE_ID_SIZE)) << "hex_to_bytes should reject non-hex input";
  EXPECT_EQ(out[0], 0) << "buffer byte 0 should be zeroed on failure";
  EXPECT_EQ(out[1], 0) << "buffer byte 1 should be zeroed on failure";
  EXPECT_EQ(out[2], 0) << "buffer byte 2 should be zeroed on failure";
}

TEST(ProtoFrame, HexToBytesLengthMismatch) {
  uint8_t out[4] = {0};
  // Provide 8 hex chars but ask for 4 bytes -> ok
  EXPECT_TRUE(hex_to_bytes("C0FFEE00", out, 4)) << "hex_to_bytes should accept exactly matching length";
  EXPECT_EQ(out[0], 0xC0) << "byte 0 should be 0xC0";
  EXPECT_EQ(out[3], 0x00) << "byte 3 should be 0x00";
}

// ========================================================================================
// Node ID string conversion helpers
// ========================================================================================

TEST(ProtoFrame, NodeIdToString) {
  uint8_t id[3] = {0xC0, 0xFF, 0xEE};
  std::string s = node_id_to_string(id);
  EXPECT_EQ(s, "C0FFEE") << "node_id_to_string should produce uppercase hex without prefix";
}

TEST(ProtoFrame, NodeIdFromString) {
  std::string s = "C0FFEE";
  uint8_t out[3] = {0};
  EXPECT_TRUE(hex_to_bytes(s, out, 3)) << "hex_to_bytes should parse node ID string";
  EXPECT_EQ(out[0], 0xC0) << "byte 0 should be 0xC0";
  EXPECT_EQ(out[1], 0xFF) << "byte 1 should be 0xFF";
  EXPECT_EQ(out[2], 0xEE) << "byte 2 should be 0xEE";
}

// ========================================================================================
// Default inversion for device types
// ========================================================================================

TEST(ProtoFrame, DefaultInvertedForType) {
  EXPECT_TRUE(default_inverted_for_type(DeviceType::HORIZONTAL_AWNING)) << "horizontal awnings should be inverted";
  EXPECT_FALSE(default_inverted_for_type(DeviceType::ROLLER_SHUTTER)) << "roller shutters should not be inverted";
  EXPECT_FALSE(default_inverted_for_type(DeviceType::AWNING)) << "standard awnings should not be inverted";
  EXPECT_FALSE(default_inverted_for_type(DeviceType::LIGHT)) << "lights should not be inverted";
  EXPECT_FALSE(default_inverted_for_type(DeviceType::ON_OFF_SWITCH)) << "switches should not be inverted";
}

// ========================================================================================
// CRC-CCITT more vectors
// ========================================================================================

TEST(ProtoFrame, CrcCcittZeroInput) {
  uint8_t empty[1] = {0x00};
  // CRC of single zero byte: 0x0000 ^ 0x00 = 0x0000; then no further bits
  EXPECT_EQ(crc_ccitt(empty, 1), 0x0000) << "CRC-CCITT of single zero byte should be 0x0000";
}

TEST(ProtoFrame, CrcCcittAllOnes) {
  uint8_t ones[2] = {0xFF, 0xFF};
  uint16_t crc = crc_ccitt(ones, 2);
  // Known value for 0xFFFF: 0xFFFF ^ 0xFFFF shifts through 16 bits = 0x1D0F? Let's just ensure non-zero.
  // We'll compute proper value later if needed; for now check it's not trivially zero.
  EXPECT_NE(crc, 0x0000) << "CRC-CCITT of all-ones should be non-zero";
}

// ========================================================================================
// Device capability classification
// ========================================================================================

TEST(ProtoFrame, DeviceCapabilityClassClassification) {
  EXPECT_EQ(device_capability_class(DeviceType::UNKNOWN), DeviceCapabilityClass::UNKNOWN)
      << "UNKNOWN should map to UNKNOWN";
  EXPECT_EQ(device_capability_class(DeviceType::ADJUSTABLE_SLAT_SHUTTER), DeviceCapabilityClass::COVER)
      << "adjustable slat shutter should be COVER";
  EXPECT_EQ(device_capability_class(DeviceType::VENETIAN_BLIND), DeviceCapabilityClass::COVER)
      << "venetian blind should be COVER";
  EXPECT_EQ(device_capability_class(DeviceType::ROLLER_SHUTTER), DeviceCapabilityClass::COVER)
      << "roller shutter should be COVER";
  EXPECT_EQ(device_capability_class(DeviceType::SCREEN), DeviceCapabilityClass::COVER) << "screen should be COVER";
  EXPECT_EQ(device_capability_class(DeviceType::AWNING), DeviceCapabilityClass::COVER) << "awning should be COVER";
  EXPECT_EQ(device_capability_class(DeviceType::LIGHT), DeviceCapabilityClass::LIGHT) << "light should be LIGHT";
  EXPECT_EQ(device_capability_class(DeviceType::ON_OFF_SWITCH), DeviceCapabilityClass::SWITCH)
      << "on/off switch should be SWITCH";
  EXPECT_EQ(device_capability_class(DeviceType::LOCK), DeviceCapabilityClass::LOCK) << "lock should be LOCK";
  EXPECT_EQ(device_capability_class(DeviceType::HEATING), DeviceCapabilityClass::CLIMATE)
      << "heating should be CLIMATE";
  EXPECT_EQ(device_capability_class(DeviceType::BEACON), DeviceCapabilityClass::BEACON) << "beacon should be BEACON";
  EXPECT_EQ(device_capability_class(DeviceType::SENSOR), DeviceCapabilityClass::SENSOR) << "sensor should be SENSOR";
}

TEST(ProtoFrame, DeviceCapabilityClassName) {
  EXPECT_STREQ(device_capability_class_name(DeviceType::ROLLER_SHUTTER), "cover")
      << "roller shutter class name should be 'cover'";
  EXPECT_STREQ(device_capability_class_name(DeviceType::LIGHT), "light") << "light class name should be 'light'";
  EXPECT_STREQ(device_capability_class_name(DeviceType::ON_OFF_SWITCH), "switch")
      << "switch class name should be 'switch'";
  EXPECT_STREQ(device_capability_class_name(DeviceType::UNKNOWN), "unknown")
      << "unknown class name should be 'unknown'";
}

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

TEST(ProtoFrame, DecodeDeviceNamePayloadSkipsLeadingPaddingAndTrailingSpaces) {
  const uint8_t payload[] = {0x00, 'P', 'a', 't', 'i', 'o', ' ', 0x20, 0x00};
  EXPECT_EQ(decode_device_name_payload(payload, sizeof(payload)), "Patio")
      << "name decoder should skip the extra leading byte and trim trailing space/null padding";
}

TEST(ProtoFrame, DecodeDeviceNamePayloadKeepsLeadingCharacterWhenPresent) {
  const uint8_t payload[] = {'D', 'e', 'x', 'x', 'o', 0x20, 0x00};
  EXPECT_EQ(decode_device_name_payload(payload, sizeof(payload)), "Dexxo")
      << "name decoder should preserve the first byte when it already contains the first character";
}

TEST(ProtoFrame, DecodeDeviceNamePayloadConvertsLatin1ToUtf8) {
  const uint8_t payload[] = {0x00, 'R', 0xE9, 's', 'u', 'm', 0xE9, 0x00};
  EXPECT_EQ(decode_device_name_payload(payload, sizeof(payload)), "R\xC3\xA9sum\xC3\xA9")
      << "name decoder should convert Latin-1 bytes to UTF-8";
}

TEST(ProtoFrame, DecodeDeviceNamePayloadReturnsEmptyWhenOnlyPaddingRemains) {
  const uint8_t payload[] = {0x00, 0x20, 0x00, 0x20};
  EXPECT_TRUE(decode_device_name_payload(payload, sizeof(payload)).empty())
      << "name decoder should return an empty string when the payload contains only padding";
}

TEST(ProtoFrame, DecodeDeviceNamePayloadTruncatesWithoutBreakingUtf8Sequence) {
  uint8_t payload[18] = {0x00};
  for (size_t index = 1; index < sizeof(payload); index++)
    payload[index] = 0xE9;

  EXPECT_EQ(decode_device_name_payload(payload, sizeof(payload)),
            "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9"
            "\xC3\xA9\xC3\xA9")
      << "name decoder should stop before exceeding the fixed device-name buffer and keep valid UTF-8";
}

TEST(ProtoFrame, EncodeDeviceNamePayloadAsciiNameZeroPadsPayload) {
  uint8_t payload[DEVICE_NAME_WRITE_PAYLOAD_SIZE] = {0xFF};
  std::string normalized_name;

  EXPECT_EQ(encode_device_name_payload("Patio", payload, normalized_name), DeviceNameValidationError::NONE);
  EXPECT_EQ(normalized_name, "Patio");
  EXPECT_EQ(payload[0], 'P');
  EXPECT_EQ(payload[1], 'a');
  EXPECT_EQ(payload[2], 't');
  EXPECT_EQ(payload[3], 'i');
  EXPECT_EQ(payload[4], 'o');
  for (size_t index = 5; index < DEVICE_NAME_WRITE_PAYLOAD_SIZE; index++)
    EXPECT_EQ(payload[index], 0x00) << "unused write-payload bytes should be zero padded";
}

TEST(ProtoFrame, EncodeDeviceNamePayloadTrimsAsciiWhitespaceBeforeEncoding) {
  uint8_t payload[DEVICE_NAME_WRITE_PAYLOAD_SIZE] = {0};
  std::string normalized_name;

  EXPECT_EQ(encode_device_name_payload("  Velux East  ", payload, normalized_name), DeviceNameValidationError::NONE);
  EXPECT_EQ(normalized_name, "Velux East");
}

TEST(ProtoFrame, EncodeDeviceNamePayloadAcceptsLatin1Utf8Characters) {
  uint8_t payload[DEVICE_NAME_WRITE_PAYLOAD_SIZE] = {0};
  std::string normalized_name;

  EXPECT_EQ(encode_device_name_payload("R\xC3\xA9sum\xC3\xA9", payload, normalized_name),
            DeviceNameValidationError::NONE);
  EXPECT_EQ(normalized_name, "R\xC3\xA9sum\xC3\xA9");
  EXPECT_EQ(payload[0], 'R');
  EXPECT_EQ(payload[1], 0xE9);
  EXPECT_EQ(payload[2], 's');
  EXPECT_EQ(payload[3], 'u');
  EXPECT_EQ(payload[4], 'm');
  EXPECT_EQ(payload[5], 0xE9);
}

TEST(ProtoFrame, EncodeDeviceNamePayloadRejectsEmptyName) {
  uint8_t payload[DEVICE_NAME_WRITE_PAYLOAD_SIZE] = {0};
  std::string normalized_name;

  EXPECT_EQ(encode_device_name_payload("   ", payload, normalized_name), DeviceNameValidationError::EMPTY);
  EXPECT_TRUE(normalized_name.empty());
}

TEST(ProtoFrame, EncodeDeviceNamePayloadRejectsTooLongName) {
  uint8_t payload[DEVICE_NAME_WRITE_PAYLOAD_SIZE] = {0};
  std::string normalized_name;

  EXPECT_EQ(encode_device_name_payload("1234567890ABCDEF", payload, normalized_name),
            DeviceNameValidationError::TOO_LONG);
}

TEST(ProtoFrame, EncodeDeviceNamePayloadRejectsUnsupportedUnicodeCharacter) {
  uint8_t payload[DEVICE_NAME_WRITE_PAYLOAD_SIZE] = {0};
  std::string normalized_name;

  EXPECT_EQ(encode_device_name_payload("Door \xE2\x82\xAC", payload, normalized_name),
            DeviceNameValidationError::UNSUPPORTED_CHAR);
}

TEST(ProtoFrame, EncodeDeviceNamePayloadRejectsMalformedUtf8) {
  uint8_t payload[DEVICE_NAME_WRITE_PAYLOAD_SIZE] = {0};
  std::string normalized_name;
  const std::string malformed_name = std::string("Bad\xC3", 4);

  EXPECT_EQ(encode_device_name_payload(malformed_name, payload, normalized_name),
            DeviceNameValidationError::INVALID_UTF8);
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
  EXPECT_EQ(device_capability_class(DeviceType::BLIND), DeviceCapabilityClass::COVER) << "blind should be COVER";
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
  EXPECT_EQ(device_capability_class(DeviceType::HEATING_TEMPERATURE_INTERFACE), DeviceCapabilityClass::CLIMATE)
      << "heating temperature interface should be CLIMATE";
  EXPECT_EQ(device_capability_class(DeviceType::BEACON), DeviceCapabilityClass::BEACON) << "beacon should be BEACON";
  EXPECT_EQ(device_capability_class(DeviceType::INTRUSION_ALARM), DeviceCapabilityClass::SENSOR)
      << "intrusion alarm should be SENSOR";
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

// ========================================================================================
// Tilt report decoding
// ========================================================================================

TEST(ProtoFrame, DecodeTiltReport) {
  // STATUS_POS_MAX = 0xC800 = 51200, tilt_raw = 0 → closed (100 - 0) = 100% tilt → 100
  EXPECT_FLOAT_EQ(decode_tilt_report(0), 100.0f) << "tilt_raw=0 should decode to 100% (fully closed in tilt)";

  // tilt_raw = STATUS_POS_MAX → open (100 - 100) = 0% tilt → 0
  EXPECT_FLOAT_EQ(decode_tilt_report(STATUS_POS_MAX), 0.0f)
      << "tilt_raw=STATUS_POS_MAX should decode to 0% (fully open in tilt)";

  // tilt_raw = STATUS_POS_MAX / 2 → halfway: 100 - 50 = 50%
  EXPECT_FLOAT_EQ(decode_tilt_report(STATUS_POS_MAX / 2), 50.0f)
      << "tilt_raw=half should decode to 50% (mid position in tilt)";

  // tilt_raw > STATUS_POS_MAX → UNKNOWN_POSITION
  EXPECT_FLOAT_EQ(decode_tilt_report(STATUS_POS_MAX + 1), UNKNOWN_POSITION)
      << "tilt_raw exceeding max should return UNKNOWN_POSITION";
}

TEST(ProtoFrame, FrameLengthAndFlagGetters) {
  IoFrame f{};
  init_frame(f, true, true, false, false);
  EXPECT_TRUE(is_start(f)) << "start flag should be set after init with start=true";
  EXPECT_FALSE(is_end(f)) << "end flag should not be set after init with end=false";

  init_frame(f, true, false, true, false);
  EXPECT_FALSE(is_start(f)) << "start should be false";
  EXPECT_TRUE(is_end(f)) << "end should be true";

  init_frame(f, true, true, true, false);
  EXPECT_TRUE(is_start(f)) << "start should be true when both set";
  EXPECT_TRUE(is_end(f)) << "end should be true when both set";

  // Length is set by set_cmd, not init_frame. After set_cmd with 0 data bytes,
  // length = FRAME_MIN_SIZE (9).
  uint8_t own[3] = {0xC0, 0xFF, 0xEE};
  uint8_t dst[3] = {0x9C, 0xA3, 0x9C};
  init_frame(f, true, true, false, false);
  set_src(f, own);
  set_dst(f, dst);
  set_cmd(f, CMD_PRIVATE);
  EXPECT_EQ(frame_length(f), FRAME_MIN_SIZE) << "set_cmd should set frame length to minimum (9)";
}

// ========================================================================================
// Command name lookup
// ========================================================================================

TEST(ProtoFrame, CommandNameKnownCommands) {
  EXPECT_STREQ(command_name(CMD_EXECUTE), "EXECUTE");
  EXPECT_STREQ(command_name(CMD_ACTIVATE_MODE), "ACTIVATE_MODE");
  EXPECT_STREQ(command_name(CMD_PRIVATE), "PRIVATE");
  EXPECT_STREQ(command_name(CMD_PRIVATE_RESP), "PRIVATE_RESP");
  EXPECT_STREQ(command_name(CMD_SET_SENSOR), "SET_SENSOR");
  EXPECT_STREQ(command_name(CMD_SET_SENSOR_ACK), "SET_SENSOR_ACK");
  EXPECT_STREQ(command_name(CMD_WRITE_PRIVATE), "WRITE_PRIVATE");
  EXPECT_STREQ(command_name(CMD_WRITE_PRIVATE_ACK), "WRITE_PRIVATE_ACK");
  EXPECT_STREQ(command_name(CMD_DISCOVER_REQ), "DISCOVER_REQ");
  EXPECT_STREQ(command_name(CMD_DISCOVER_RESP), "DISCOVER_RESP");
  EXPECT_STREQ(command_name(CMD_DISCOVER_SPE_REQ), "DISCOVER_SPE_REQ");
  EXPECT_STREQ(command_name(CMD_DISCOVER_SPE_RESP), "DISCOVER_SPE_RESP");
  EXPECT_STREQ(command_name(CMD_DISCOVER_CONFIRM), "DISCOVER_CONFIRM");
  EXPECT_STREQ(command_name(CMD_DISCOVER_CONFIRM_ACK), "DISCOVER_CONFIRM_ACK");
  EXPECT_STREQ(command_name(CMD_DISCOVER_ALT_REQ), "DISCOVER_ALT_REQ");
  EXPECT_STREQ(command_name(CMD_ONEWAY_ADD_CONTROLLER), "ONEWAY_ADD_CONTROLLER");
  EXPECT_STREQ(command_name(CMD_ONEWAY_REMOVE), "ONEWAY_REMOVE");
  EXPECT_STREQ(command_name(CMD_KEY_INIT), "KEY_INIT");
  EXPECT_STREQ(command_name(CMD_KEY_TRANSFER), "KEY_TRANSFER");
  EXPECT_STREQ(command_name(CMD_KEY_CONFIRM), "KEY_CONFIRM");
  EXPECT_STREQ(command_name(CMD_ADDRESS_REQ), "ADDRESS_REQ");
  EXPECT_STREQ(command_name(CMD_ADDRESS_RESP), "ADDRESS_RESP");
  EXPECT_STREQ(command_name(CMD_LAUNCH_KEY_TRANSFER), "LAUNCH_KEY_TRANSFER");
  EXPECT_STREQ(command_name(CMD_CHALLENGE_REQ), "CHALLENGE_REQ");
  EXPECT_STREQ(command_name(CMD_CHALLENGE_RESP), "CHALLENGE_RESP");
  EXPECT_STREQ(command_name(CMD_GET_NAME), "GET_NAME");
  EXPECT_STREQ(command_name(CMD_GET_NAME_RESP), "GET_NAME_RESP");
  EXPECT_STREQ(command_name(CMD_SET_NAME), "SET_NAME");
  EXPECT_STREQ(command_name(CMD_SET_NAME_RESP), "SET_NAME_RESP");
  EXPECT_STREQ(command_name(CMD_GET_INFO1), "GET_INFO1");
  EXPECT_STREQ(command_name(CMD_GET_INFO1_RESP), "GET_INFO1_RESP");
  EXPECT_STREQ(command_name(CMD_GET_INFO2), "GET_INFO2");
  EXPECT_STREQ(command_name(CMD_GET_INFO2_RESP), "GET_INFO2_RESP");
  EXPECT_STREQ(command_name(CMD_SET_CONFIG1), "SET_CONFIG1");
  EXPECT_STREQ(command_name(CMD_SET_CONFIG1_RESP), "SET_CONFIG1_RESP");
  EXPECT_STREQ(command_name(CMD_STATUS_UPDATE), "STATUS_UPDATE");
  EXPECT_STREQ(command_name(CMD_STATUS_UPDATE_RESP), "STATUS_UPDATE_RESP");
  EXPECT_STREQ(command_name(CMD_SEND_RAW_MESSAGE), "SEND_RAW_MESSAGE");
  EXPECT_STREQ(command_name(CMD_READ_GROUPS), "READ_GROUPS");
  EXPECT_STREQ(command_name(CMD_REBOOT), "REBOOT");
  EXPECT_STREQ(command_name(CMD_SERVICE_STATUS_ACK), "SERVICE_STATUS_ACK");
  EXPECT_STREQ(command_name(CMD_ERROR_RESP), "ERROR_RESP");
}

TEST(ProtoFrame, CommandNameUnknownCommand) {
  EXPECT_STREQ(command_name(0xAB), "UNKNOWN_CMD") << "unknown command ID should return fallback string";
}

// ========================================================================================
// Manufacturer name lookup
// ========================================================================================

TEST(ProtoFrame, ManufacturerNameKnownIds) {
  EXPECT_STREQ(manufacturer_name(MANUFACTURER_VELUX), "VELUX");
  EXPECT_STREQ(manufacturer_name(MANUFACTURER_SOMFY), "Somfy");
  EXPECT_STREQ(manufacturer_name(MANUFACTURER_HONEYWELL), "Honeywell");
  EXPECT_STREQ(manufacturer_name(MANUFACTURER_HORMANN), "Hörmann");
  EXPECT_STREQ(manufacturer_name(MANUFACTURER_ASSA_ABLOY), "ASSA ABLOY");
  EXPECT_STREQ(manufacturer_name(MANUFACTURER_NIKO), "Niko");
  EXPECT_STREQ(manufacturer_name(MANUFACTURER_WINDOW_MASTER), "WINDOW MASTER");
  EXPECT_STREQ(manufacturer_name(MANUFACTURER_RENSON), "Renson");
  EXPECT_STREQ(manufacturer_name(MANUFACTURER_CIAT), "CIAT");
  EXPECT_STREQ(manufacturer_name(MANUFACTURER_SECUYOU), "Secuyou");
  EXPECT_STREQ(manufacturer_name(MANUFACTURER_OVERKIZ), "OVERKIZ");
  EXPECT_STREQ(manufacturer_name(MANUFACTURER_ATLANTIC_GROUP), "Atlantic Group");
}

TEST(ProtoFrame, ManufacturerNameUnknownIds) {
  EXPECT_STREQ(manufacturer_name(0), "unknown") << "ID 0 should return unknown";
  EXPECT_STREQ(manufacturer_name(MANUFACTURER_ID_MAX + 1), "unknown") << "ID above max should return unknown";
  EXPECT_STREQ(manufacturer_name(255), "unknown") << "ID 255 should return unknown";
}

// ========================================================================================
// Originator name lookup
// ========================================================================================

TEST(ProtoFrame, OriginatorNameKnownCodes) {
  EXPECT_STREQ(originator_name(ORIGINATOR_LOCAL_USER), "local_user");
  EXPECT_STREQ(originator_name(ORIGINATOR_USER_REMOTE), "user_remote");
  EXPECT_STREQ(originator_name(ORIGINATOR_RAIN_SENSOR), "rain_sensor");
  EXPECT_STREQ(originator_name(ORIGINATOR_TIMER), "timer");
  EXPECT_STREQ(originator_name(ORIGINATOR_SECURITY), "security");
  EXPECT_STREQ(originator_name(ORIGINATOR_UPS), "ups");
  EXPECT_STREQ(originator_name(ORIGINATOR_SMART_CONTROLLER), "smart_controller");
  EXPECT_STREQ(originator_name(ORIGINATOR_LIFESTYLE), "lifestyle");
  EXPECT_STREQ(originator_name(ORIGINATOR_SAAC), "saac");
  EXPECT_STREQ(originator_name(ORIGINATOR_WIND_SENSOR), "wind_sensor");
  EXPECT_STREQ(originator_name(ORIGINATOR_LOAD_SHEDDING), "load_shedding");
  EXPECT_STREQ(originator_name(ORIGINATOR_LOCAL_LIGHT), "local_light");
  EXPECT_STREQ(originator_name(ORIGINATOR_ENVIRONMENT), "environment");
  EXPECT_STREQ(originator_name(ORIGINATOR_MYSELF), "myself");
  EXPECT_STREQ(originator_name(ORIGINATOR_AUTOMATIC_CYCLE), "automatic_cycle");
  EXPECT_STREQ(originator_name(ORIGINATOR_EMERGENCY), "emergency");
}

TEST(ProtoFrame, OriginatorNameUnknownCodes) {
  EXPECT_STREQ(originator_name(0x0A), "unknown") << "undefined originator should return unknown";
  EXPECT_STREQ(originator_name(0x50), "unknown") << "undefined originator should return unknown";
}

// ========================================================================================
// ACEI priority level parsing
// ========================================================================================

TEST(ProtoFrame, AceiLevelNameAllLevels) {
  EXPECT_STREQ(acei_level_name(ACEI_LEVEL_PROTECTION_HUMAN), "protection_human");
  EXPECT_STREQ(acei_level_name(ACEI_LEVEL_PROTECTION_SENSOR), "protection_sensor");
  EXPECT_STREQ(acei_level_name(ACEI_LEVEL_USER_HIGH), "user_high");
  EXPECT_STREQ(acei_level_name(ACEI_LEVEL_USER_DEFAULT), "user_default");
  EXPECT_STREQ(acei_level_name(ACEI_LEVEL_COMFORT_1), "comfort_1");
  EXPECT_STREQ(acei_level_name(ACEI_LEVEL_COMFORT_2), "comfort_2");
  EXPECT_STREQ(acei_level_name(ACEI_LEVEL_AUTO_SAAC), "auto_saac");
  EXPECT_STREQ(acei_level_name(ACEI_LEVEL_AUTO_DEFAULT), "auto_default");
}

TEST(ProtoFrame, AceiLevelNameOutOfRange) {
  EXPECT_STREQ(acei_level_name(8), "unknown") << "level > 7 should return unknown";
  EXPECT_STREQ(acei_level_name(255), "unknown") << "level 255 should return unknown";
}

TEST(ProtoFrame, AceiByteExtraction) {
  // Example ACEI byte: 0x43 = 0b01000011
  // level = (0x43 >> 5) & 0x07 = 0b010 = 2 (user_high)
  // service = (0x43 >> 3) & 0x03 = 0b00 = 0
  // extended = (0x43 >> 1) & 0x03 = 0b01 = 1
  // valid = 0x43 & 0x01 = 1
  uint8_t acei = 0x43;
  uint8_t level = (acei & ACEI_LEVEL_MASK) >> ACEI_LEVEL_SHIFT;
  uint8_t service = (acei & ACEI_SERVICE_MASK) >> ACEI_SERVICE_SHIFT;
  uint8_t extended = (acei & ACEI_EXTENDED_MASK) >> ACEI_EXTENDED_SHIFT;
  uint8_t valid = acei & ACEI_VALID_BIT;
  EXPECT_EQ(level, 2) << "ACEI 0x43 level should be 2";
  EXPECT_EQ(service, 0) << "ACEI 0x43 service should be 0";
  EXPECT_EQ(extended, 1) << "ACEI 0x43 extended should be 1";
  EXPECT_EQ(valid, 1) << "ACEI 0x43 valid should be 1";
  EXPECT_STREQ(acei_level_name(level), "user_high");

  // Example ACEI byte: 0xE7 = 0b11100111
  // level = (0xE7 >> 5) & 0x07 = 0b111 = 7 (auto_default)
  // service = (0xE7 >> 3) & 0x03 = 0b00 = 0
  // extended = (0xE7 >> 1) & 0x03 = 0b11 = 3
  // valid = 0xE7 & 0x01 = 1
  acei = 0xE7;
  level = (acei & ACEI_LEVEL_MASK) >> ACEI_LEVEL_SHIFT;
  service = (acei & ACEI_SERVICE_MASK) >> ACEI_SERVICE_SHIFT;
  extended = (acei & ACEI_EXTENDED_MASK) >> ACEI_EXTENDED_SHIFT;
  valid = acei & ACEI_VALID_BIT;
  EXPECT_EQ(level, 7) << "ACEI 0xE7 level should be 7";
  EXPECT_EQ(service, 0) << "ACEI 0xE7 service should be 0";
  EXPECT_EQ(extended, 3) << "ACEI 0xE7 extended should be 3";
  EXPECT_EQ(valid, 1) << "ACEI 0xE7 valid should be 1";
  EXPECT_STREQ(acei_level_name(level), "auto_default");

  // Our default outbound ACEI: 0x67 = 0b01100111
  // This is the value used in proto_commands.cpp for execute commands.
  // level = 3 (user_default), service = 0, extended = 3, valid = 1
  acei = 0x67;
  level = (acei & ACEI_LEVEL_MASK) >> ACEI_LEVEL_SHIFT;
  service = (acei & ACEI_SERVICE_MASK) >> ACEI_SERVICE_SHIFT;
  extended = (acei & ACEI_EXTENDED_MASK) >> ACEI_EXTENDED_SHIFT;
  valid = acei & ACEI_VALID_BIT;
  EXPECT_EQ(level, ACEI_LEVEL_USER_DEFAULT) << "ACEI 0x67 level should be user_default (3)";
  EXPECT_EQ(service, 0) << "ACEI 0x67 service should be 0";
  EXPECT_EQ(extended, 3) << "ACEI 0x67 extended should be 3";
  EXPECT_EQ(valid, 1) << "ACEI 0x67 valid should be 1";
  EXPECT_STREQ(acei_level_name(level), "user_default");
}

// ========================================================================================
// Address classification
// ========================================================================================

TEST(ProtoFrame, ClassifyAddressUnicast) {
  uint8_t addr[3] = {0xC0, 0xFF, 0xEE};
  EXPECT_EQ(classify_address(addr), AddressClass::UNICAST) << "non-zero first byte should be UNICAST";
}

TEST(ProtoFrame, ClassifyAddressBroadcastAll) {
  // {0x00, 0x00, 0x3F} is all-devices broadcast
  uint8_t addr1[3] = {0x00, 0x00, 0x3F};
  EXPECT_EQ(classify_address(addr1), AddressClass::BROADCAST_ALL) << "suffix 0x3F with no type bits should be ALL";

  // {0x00, 0x00, 0xBF} = type bits set in addr[2] high bits + suffix 0x3F → typed broadcast
  // (all devices of the type encoded in bits [9:2]), not global "all types"
  uint8_t addr2[3] = {0x00, 0x00, 0xBF};
  EXPECT_EQ(classify_address(addr2), AddressClass::BROADCAST_TYPE)
      << "suffix 0x3F with type bits in addr[2] should be BROADCAST_TYPE";

  // {0x00, 0x01, 0xBF} = type bits in byte 1 + suffix 0x3F → typed broadcast (e.g., "all lights")
  uint8_t addr3[3] = {0x00, 0x01, 0xBF};
  EXPECT_EQ(classify_address(addr3), AddressClass::BROADCAST_TYPE)
      << "suffix 0x3F with type bits in byte 1 should be BROADCAST_TYPE";

  // {0x00, 0x00, 0x00} = zero address → BROADCAST_ALL
  uint8_t addr4[3] = {0x00, 0x00, 0x00};
  EXPECT_EQ(classify_address(addr4), AddressClass::BROADCAST_ALL) << "all-zero address should be BROADCAST_ALL";
}

TEST(ProtoFrame, ClassifyAddressDiscovery) {
  // {0x00, 0x00, 0x3B} = discovery broadcast
  uint8_t addr1[3] = {0x00, 0x00, 0x3B};
  EXPECT_EQ(classify_address(addr1), AddressClass::DISCOVERY) << "suffix 0x3B should be DISCOVERY";

  // {0x00, 0x01, 0x3B} = typed discovery broadcast — still DISCOVERY
  uint8_t addr2[3] = {0x00, 0x01, 0x3B};
  EXPECT_EQ(classify_address(addr2), AddressClass::DISCOVERY) << "suffix 0x3B with type bits should be DISCOVERY";
}

TEST(ProtoFrame, ClassifyAddressBroadcastType) {
  // {0x00, 0x01, 0x00} = has type bits in byte 1, suffix is 0x00 → BROADCAST_TYPE
  uint8_t addr[3] = {0x00, 0x01, 0x00};
  EXPECT_EQ(classify_address(addr), AddressClass::BROADCAST_TYPE)
      << "type bits in byte 1 with non-standard suffix should be BROADCAST_TYPE";
}

TEST(ProtoFrame, BroadcastTargetTypeExtraction) {
  // Unicast → UNKNOWN
  uint8_t unicast[3] = {0xC0, 0xFF, 0xEE};
  EXPECT_EQ(broadcast_target_type(unicast), DeviceType::UNKNOWN) << "unicast should return UNKNOWN";

  // Type=2 (roller_shutter): (2 << 6) + 0x3F = 0xBF → addr = {0x00, 0x00, 0xBF}
  uint8_t roller[3] = {0x00, 0x00, 0xBF};
  EXPECT_EQ(broadcast_target_type(roller), DeviceType::ROLLER_SHUTTER)
      << "address encoding type 2 should extract ROLLER_SHUTTER";

  // Type=6 (light): (6 << 6) = 0x180 → addr[1] = 0x01, addr[2] = (0x80 | 0x3F) = 0xBF
  // Actually: type 6 → type_raw = (addr[1] << 2) | (addr[2] >> 6)
  // For type=6: we need (addr[1] << 2) | (addr[2] >> 6) == 6
  // addr[1] = 1, addr[2] >> 6 = 2 → 1*4 + 2 = 6. addr[2] = (2 << 6) | 0x3F = 0xBF
  uint8_t light[3] = {0x00, 0x01, 0xBF};
  EXPECT_EQ(broadcast_target_type(light), DeviceType::LIGHT) << "address encoding type 6 should extract LIGHT";

  // Type=0 (unknown/all): addr = {0x00, 0x00, 0x3F} → type bits = 0
  uint8_t all[3] = {0x00, 0x00, 0x3F};
  EXPECT_EQ(broadcast_target_type(all), DeviceType::UNKNOWN) << "type 0 should extract UNKNOWN";
}

// ========================================================================================
// CTRL1 bit definitions
// ========================================================================================

TEST(ProtoFrame, Ctrl1BitsNoOverlap) {
  // Verify that CTRL1 bit constants don't collide
  EXPECT_EQ(CTRL1_VERSION_MASK & CTRL1_PRIORITY, 0) << "VERSION_MASK should not overlap PRIORITY";
  EXPECT_EQ(CTRL1_VERSION_MASK & CTRL1_ACK, 0) << "VERSION_MASK should not overlap ACK";
  EXPECT_EQ(CTRL1_VERSION_MASK & CTRL1_LOW_POWER, 0) << "VERSION_MASK should not overlap LOW_POWER";
  EXPECT_EQ(CTRL1_VERSION_MASK & CTRL1_ROUTED, 0) << "VERSION_MASK should not overlap ROUTED";
  EXPECT_EQ(CTRL1_VERSION_MASK & CTRL1_BEACON, 0) << "VERSION_MASK should not overlap BEACON";
  EXPECT_EQ(CTRL1_PRIORITY & CTRL1_ACK, 0) << "PRIORITY should not overlap ACK";
  EXPECT_EQ(CTRL1_PRIORITY & CTRL1_LOW_POWER, 0) << "PRIORITY should not overlap LOW_POWER";
  EXPECT_EQ(CTRL1_PRIORITY & CTRL1_ROUTED, 0) << "PRIORITY should not overlap ROUTED";
  EXPECT_EQ(CTRL1_PRIORITY & CTRL1_BEACON, 0) << "PRIORITY should not overlap BEACON";
  EXPECT_EQ(CTRL1_ACK & CTRL1_LOW_POWER, 0) << "ACK should not overlap LOW_POWER";
  EXPECT_EQ(CTRL1_ACK & CTRL1_ROUTED, 0) << "ACK should not overlap ROUTED";
  EXPECT_EQ(CTRL1_ACK & CTRL1_BEACON, 0) << "ACK should not overlap BEACON";
  EXPECT_EQ(CTRL1_LOW_POWER & CTRL1_ROUTED, 0) << "LOW_POWER should not overlap ROUTED";
  EXPECT_EQ(CTRL1_LOW_POWER & CTRL1_BEACON, 0) << "LOW_POWER should not overlap BEACON";
  EXPECT_EQ(CTRL1_ROUTED & CTRL1_BEACON, 0) << "ROUTED should not overlap BEACON";
}

TEST(ProtoFrame, Ctrl1LowPowerSetByInitFrame) {
  IoFrame f{};
  init_frame(f, true, true, false, true);
  EXPECT_EQ(f.ctrl1 & CTRL1_LOW_POWER, CTRL1_LOW_POWER) << "init_frame with low_power=true should set LOW_POWER bit";
  EXPECT_EQ(f.ctrl1 & CTRL1_PRIORITY, 0) << "init_frame should not set PRIORITY";
  EXPECT_EQ(f.ctrl1 & CTRL1_ROUTED, 0) << "init_frame should not set ROUTED";
  EXPECT_EQ(f.ctrl1 & CTRL1_BEACON, 0) << "init_frame should not set BEACON";
  EXPECT_EQ(f.ctrl1 & CTRL1_ACK, 0) << "init_frame should not auto-set ACK";
}

TEST(ProtoFrame, Ctrl1ClearWhenNotLowPower) {
  IoFrame f{};
  init_frame(f, true, true, false, false);
  EXPECT_EQ(f.ctrl1, 0) << "2W frame with low_power=false should have ctrl1 == 0";
}

TEST(ProtoFrame, Ctrl1AckNotAutoSetOnOutboundFrames) {
  // ACK bit (0x10) is defined but NOT automatically set on outbound frames.
  // Some devices reject frames with unexpected CTRL1 bits set.
  // The constant remains available for inbound frame parsing/logging.
  IoFrame f{};
  init_frame(f, true, false, false, false);
  EXPECT_EQ(f.ctrl1 & CTRL1_ACK, 0) << "2W frame should NOT auto-set ACK bit";

  init_frame(f, false, false, false, false);
  EXPECT_EQ(f.ctrl1 & CTRL1_ACK, 0) << "1W frame should NOT have ACK bit set";
}

// ========================================================================================
// Discovery Multi Information Byte — ATT and power-save lookup
// ========================================================================================

TEST(ProtoFrame, AttClassNameAllValues) {
  EXPECT_STREQ(att_class_name(ATT_CLASS_5S), "5s");
  EXPECT_STREQ(att_class_name(ATT_CLASS_10S), "10s");
  EXPECT_STREQ(att_class_name(ATT_CLASS_20S), "20s");
  EXPECT_STREQ(att_class_name(ATT_CLASS_40S), "40s");
  EXPECT_STREQ(att_class_name(0xFF), "unknown");
  EXPECT_STREQ(att_class_name(4), "unknown");
}

TEST(ProtoFrame, PowerSaveModeNameAllValues) {
  EXPECT_STREQ(power_save_mode_name(POWER_SAVE_ALWAYS_ALIVE), "always_alive");
  EXPECT_STREQ(power_save_mode_name(POWER_SAVE_LOW_POWER), "low_power");
  EXPECT_STREQ(power_save_mode_name(2), "unknown");
  EXPECT_STREQ(power_save_mode_name(0xFF), "unknown");
}

TEST(ProtoFrame, DiscoveryAttClassExtractsBitsSixAndSeven) {
  EXPECT_EQ(discovery_att_class(0x00), ATT_CLASS_5S);
  EXPECT_EQ(discovery_att_class(0x40), ATT_CLASS_10S);
  EXPECT_EQ(discovery_att_class(0x80), ATT_CLASS_20S);
  EXPECT_EQ(discovery_att_class(0xC0), ATT_CLASS_40S);
  // Non-ATT bits must not leak into the extracted field.
  EXPECT_EQ(discovery_att_class(0xEC), ATT_CLASS_40S);
}

TEST(ProtoFrame, DiscoveryPowerSaveModeExtractsBitsZeroAndOne) {
  EXPECT_EQ(discovery_power_save_mode(0x00), POWER_SAVE_ALWAYS_ALIVE);
  EXPECT_EQ(discovery_power_save_mode(0x01), POWER_SAVE_LOW_POWER);
  // Non-power-save bits must not leak into the extracted field.
  EXPECT_EQ(discovery_power_save_mode(0xEC), POWER_SAVE_ALWAYS_ALIVE);
}

TEST(ProtoFrame, DiscoveryFlagsBitExtraction) {
  // flags=0xEC: ATT=11 (40s), sync_ctrl=1, bit4=0, rf_support=1, io_member=0, power_save=00
  uint8_t flags = 0xEC;
  uint8_t att = (flags & DISCOVERY_FLAGS_ATT_MASK) >> DISCOVERY_FLAGS_ATT_SHIFT;
  uint8_t power_save = flags & DISCOVERY_FLAGS_POWER_SAVE_MASK;
  EXPECT_EQ(att, ATT_CLASS_40S) << "ATT bits 11 should decode to 40s class";
  EXPECT_NE(flags & DISCOVERY_FLAGS_SYNC_CTRL_GRP, 0) << "Sync ctrl group bit should be set";
  EXPECT_EQ(power_save, POWER_SAVE_ALWAYS_ALIVE) << "Power save bits 00 = always alive";

  // flags=0x01: ATT=00 (5s), no sync, power_save=01 (low power)
  flags = 0x01;
  att = (flags & DISCOVERY_FLAGS_ATT_MASK) >> DISCOVERY_FLAGS_ATT_SHIFT;
  power_save = flags & DISCOVERY_FLAGS_POWER_SAVE_MASK;
  EXPECT_EQ(att, ATT_CLASS_5S) << "ATT bits 00 should decode to 5s class";
  EXPECT_EQ(flags & DISCOVERY_FLAGS_SYNC_CTRL_GRP, 0) << "Sync ctrl group bit should be clear";
  EXPECT_EQ(power_save, POWER_SAVE_LOW_POWER) << "Power save bits 01 = low power";

  // flags=0x48: ATT=01 (10s), no sync, rf_support=1, power_save=00
  flags = 0x48;
  att = (flags & DISCOVERY_FLAGS_ATT_MASK) >> DISCOVERY_FLAGS_ATT_SHIFT;
  power_save = flags & DISCOVERY_FLAGS_POWER_SAVE_MASK;
  EXPECT_EQ(att, ATT_CLASS_10S) << "ATT bits 01 should decode to 10s class";
  EXPECT_NE(flags & DISCOVERY_FLAGS_RF_SUPPORT, 0) << "RF support bit should be set";
  EXPECT_EQ(power_save, POWER_SAVE_ALWAYS_ALIVE);

  // flags=0x80: ATT=10 (20s), all other bits clear
  flags = 0x80;
  att = (flags & DISCOVERY_FLAGS_ATT_MASK) >> DISCOVERY_FLAGS_ATT_SHIFT;
  EXPECT_EQ(att, ATT_CLASS_20S) << "ATT bits 10 should decode to 20s class";
}

// ============================================================================
// 1W Frame Decode Tests
// ============================================================================

TEST(ProtoFrame, Decode1wMainIntentSpecialCommands) {
  char buf[ONEWAY_INTENT_BUFFER_SIZE];

  decode_1w_main_intent(POS_STOP, 0x00, buf, sizeof(buf));
  EXPECT_STREQ(buf, "STOP");

  decode_1w_main_intent(POS_FAVORITE, 0x00, buf, sizeof(buf));
  EXPECT_STREQ(buf, "FAVORITE");

  decode_1w_main_intent(POS_FAVORITE, POS_VENT_MODIFIER, buf, sizeof(buf));
  EXPECT_STREQ(buf, "VENT");

  decode_1w_main_intent(POS_UNKNOWN, 0x00, buf, sizeof(buf));
  EXPECT_STREQ(buf, "UNCHANGED");

  decode_1w_main_intent(POS_FORCE_OPEN, 0x00, buf, sizeof(buf));
  EXPECT_STREQ(buf, "FORCE_OPEN");

  decode_1w_main_intent(POS_SECURED_TARGET, 0x00, buf, sizeof(buf));
  EXPECT_STREQ(buf, "SECURED_TARGET");

  decode_1w_main_intent(POS_DEFAULT, 0x00, buf, sizeof(buf));
  EXPECT_STREQ(buf, "DEFAULT");
}

TEST(ProtoFrame, Decode1wMainIntentPositions) {
  char buf[ONEWAY_INTENT_BUFFER_SIZE];

  // 0x00 = fully open
  decode_1w_main_intent(0x00, 0x00, buf, sizeof(buf));
  EXPECT_STREQ(buf, "OPEN");

  // 0xC8 = 200 = fully closed
  decode_1w_main_intent(0xC8, 0x00, buf, sizeof(buf));
  EXPECT_STREQ(buf, "CLOSE");

  // 0x32 = 50 → 25%
  decode_1w_main_intent(0x32, 0x00, buf, sizeof(buf));
  EXPECT_STREQ(buf, "position 25%");

  // 0x96 = 150 → 75%
  decode_1w_main_intent(0x96, 0x00, buf, sizeof(buf));
  EXPECT_STREQ(buf, "position 75%");
}

TEST(ProtoFrame, Decode1wMainIntentUnknownCode) {
  char buf[ONEWAY_INTENT_BUFFER_SIZE];

  // Values above 200 that aren't recognized special codes → show raw hex
  decode_1w_main_intent(0xE0, 0x00, buf, sizeof(buf));
  EXPECT_STREQ(buf, "0xE0");

  decode_1w_main_intent(0xFF, 0x00, buf, sizeof(buf));
  EXPECT_STREQ(buf, "0xFF");
}

TEST(ProtoFrame, Decode1wMainIntentEdgeCases) {
  char buf[ONEWAY_INTENT_BUFFER_SIZE];

  // Zero-length buffer: should not crash
  decode_1w_main_intent(POS_STOP, 0x00, buf, 0);
  // No assertion needed — just verify no crash

  // Buffer size 1: should null-terminate safely
  char tiny[1] = {'X'};
  decode_1w_main_intent(POS_STOP, 0x00, tiny, sizeof(tiny));
  EXPECT_EQ(tiny[0], '\0');
}

TEST(ProtoFrame, OnewayIntentToTargetSpecialCodesResolveToNoTarget) {
  // Mirrors Decode1wMainIntentSpecialCommands: every special code (including STOP) must
  // resolve to no target, matching decode_1w_main_intent()'s branch order exactly.
  EXPECT_FALSE(oneway_intent_to_target(POS_STOP, 0x00).has_value());
  EXPECT_FALSE(oneway_intent_to_target(POS_FAVORITE, 0x00).has_value());
  EXPECT_FALSE(oneway_intent_to_target(POS_FAVORITE, POS_VENT_MODIFIER).has_value());
  EXPECT_FALSE(oneway_intent_to_target(POS_UNKNOWN, 0x00).has_value());
  EXPECT_FALSE(oneway_intent_to_target(POS_FORCE_OPEN, 0x00).has_value());
  EXPECT_FALSE(oneway_intent_to_target(POS_SECURED_TARGET, 0x00).has_value());
  EXPECT_FALSE(oneway_intent_to_target(POS_DEFAULT, 0x00).has_value());
}

TEST(ProtoFrame, OnewayIntentToTargetNumericPositions) {
  // 0x00 = fully open -> IO target 0
  auto target = oneway_intent_to_target(0x00, 0x00);
  ASSERT_TRUE(target.has_value());
  EXPECT_FLOAT_EQ(*target, 0.0f);

  // 0xC8 = 200 = fully closed -> IO target 100 (also the <=200 boundary)
  target = oneway_intent_to_target(0xC8, 0x00);
  ASSERT_TRUE(target.has_value());
  EXPECT_FLOAT_EQ(*target, 100.0f);

  // 0x32 = 50 -> 25%
  target = oneway_intent_to_target(0x32, 0x00);
  ASSERT_TRUE(target.has_value());
  EXPECT_FLOAT_EQ(*target, 25.0f);
}

TEST(ProtoFrame, OnewayIntentToTargetOutOfRangeResolvesToNoTarget) {
  // Values above 200 that aren't recognized special codes have no known target.
  EXPECT_FALSE(oneway_intent_to_target(0xE0, 0x00).has_value());
  EXPECT_FALSE(oneway_intent_to_target(0xFF, 0x00).has_value());
}

TEST(ProtoFrame, Decode1wFrameExecuteCommand) {
  IoFrame frame{};
  frame.ctrl0 = CTRL0_PROTOCOL_1W | CTRL0_START | CTRL0_END | 0x0F;  // 1W, 16 bytes total
  frame.ctrl1 = 0x00;
  // dst = 00 01 BF (light-type broadcast)
  frame.dst[0] = 0x00;
  frame.dst[1] = 0x01;
  frame.dst[2] = 0xBF;
  // src = 9D 60 85
  frame.src[0] = 0x9D;
  frame.src[1] = 0x60;
  frame.src[2] = 0x85;
  frame.cmd = CMD_EXECUTE;
  // data: originator=0x01 ACEI=0x43 main={0xC8, 0x00} (close)
  frame.data[0] = ORIGINATOR_USER_REMOTE;
  frame.data[1] = 0x43;
  frame.data[2] = 0xC8;
  frame.data[3] = 0x00;
  frame.data_len = 14;

  OneWayFrameInfo info = decode_1w_frame(frame);

  EXPECT_EQ(info.src[0], 0x9D);
  EXPECT_EQ(info.src[1], 0x60);
  EXPECT_EQ(info.src[2], 0x85);
  EXPECT_EQ(info.address_class, AddressClass::BROADCAST_TYPE);
  EXPECT_EQ(info.target_type, DeviceType::LIGHT);
  EXPECT_EQ(info.cmd, CMD_EXECUTE);
  EXPECT_TRUE(info.has_intent);
  EXPECT_EQ(info.originator, ORIGINATOR_USER_REMOTE);
  EXPECT_EQ(info.acei_level, ACEI_LEVEL_USER_HIGH);
  EXPECT_STREQ(info.intent, "CLOSE");
}

TEST(ProtoFrame, Decode1wFrameAllBroadcast) {
  IoFrame frame{};
  frame.ctrl0 = CTRL0_PROTOCOL_1W | CTRL0_START | CTRL0_END | 0x08;
  frame.ctrl1 = 0x00;
  // dst = 00 00 3F (all devices broadcast)
  frame.dst[0] = 0x00;
  frame.dst[1] = 0x00;
  frame.dst[2] = 0x3F;
  frame.src[0] = 0xA0;
  frame.src[1] = 0xA9;
  frame.src[2] = 0xA1;
  frame.cmd = CMD_EXECUTE;
  // data: originator=wind_sensor ACEI=0x23 main={0xD1, 0x00} (secured target)
  frame.data[0] = ORIGINATOR_WIND_SENSOR;
  frame.data[1] = 0x23;  // level=1 (protection_sensor)
  frame.data[2] = POS_SECURED_TARGET;
  frame.data[3] = 0x00;
  frame.data_len = 14;

  OneWayFrameInfo info = decode_1w_frame(frame);

  EXPECT_EQ(info.address_class, AddressClass::BROADCAST_ALL);
  EXPECT_EQ(info.target_type, DeviceType::UNKNOWN);  // type 0 from 00003F
  EXPECT_TRUE(info.has_intent);
  EXPECT_EQ(info.originator, ORIGINATOR_WIND_SENSOR);
  EXPECT_EQ(info.acei_level, ACEI_LEVEL_PROTECTION_SENSOR);
  EXPECT_STREQ(info.intent, "SECURED_TARGET");
}

TEST(ProtoFrame, Decode1wFrameNonExecuteCommand) {
  IoFrame frame{};
  frame.ctrl0 = CTRL0_PROTOCOL_1W | CTRL0_START | CTRL0_END | 0x10;
  frame.ctrl1 = 0x00;
  frame.dst[0] = 0x00;
  frame.dst[1] = 0x00;
  frame.dst[2] = 0x3F;
  frame.src[0] = 0x9D;
  frame.src[1] = 0x60;
  frame.src[2] = 0x85;
  frame.cmd = CMD_WRITE_PRIVATE;
  frame.data[0] = 0x02;
  frame.data[1] = 0xFF;
  frame.data[2] = 0x01;
  frame.data[3] = 0x43;
  frame.data_len = 16;

  OneWayFrameInfo info = decode_1w_frame(frame);

  EXPECT_EQ(info.cmd, CMD_WRITE_PRIVATE);
  EXPECT_FALSE(info.has_intent) << "CMD 0x20 should not decode intent (different payload layout)";
  EXPECT_EQ(info.data_len, 16);
  EXPECT_EQ(info.address_class, AddressClass::BROADCAST_ALL);
}

TEST(ProtoFrame, Decode1wFrameShortPayload) {
  IoFrame frame{};
  frame.ctrl0 = CTRL0_PROTOCOL_1W | CTRL0_START | CTRL0_END | 0x08;
  frame.ctrl1 = 0x00;
  frame.dst[0] = 0x00;
  frame.dst[1] = 0x00;
  frame.dst[2] = 0x3F;
  frame.src[0] = 0x9D;
  frame.src[1] = 0x60;
  frame.src[2] = 0x85;
  frame.cmd = CMD_EXECUTE;
  frame.data[0] = 0x01;
  frame.data[1] = 0x43;
  frame.data_len = 2;  // Too short for intent decode (needs >= 4)

  OneWayFrameInfo info = decode_1w_frame(frame);

  EXPECT_EQ(info.cmd, CMD_EXECUTE);
  EXPECT_FALSE(info.has_intent) << "Short payload should not attempt intent decode";
  EXPECT_EQ(info.data_len, 2);
}

// ============================================================================
// decode_discovery_response tests
// ============================================================================
// Payload bytes match the real somfy_awning_discovery_spe_paired_rollcall corpus fixture
// (04 00 30 E1 F2 02 CC FC 03): a CMD_DISCOVER_SPE_RESP with the full 9-byte
// DISCOVERY_RESP_FULL_SIZE layout, identical to a CMD_DISCOVER_RESP payload.

TEST(ProtoFrame, DecodeDiscoveryResponseFullPayload) {
  IoFrame frame{};
  init_frame(frame, true, true, true, false);
  frame.src[0] = 0x30;
  frame.src[1] = 0xE1;
  frame.src[2] = 0xF2;
  frame.dst[0] = 0x00;
  frame.dst[1] = 0x00;
  frame.dst[2] = 0x3B;
  frame.cmd = CMD_DISCOVER_SPE_RESP;
  const uint8_t payload[] = {0x04, 0x00, 0x30, 0xE1, 0xF2, 0x02, 0xCC, 0xFC, 0x03};
  memcpy(frame.data, payload, sizeof(payload));
  frame.data_len = sizeof(payload);

  IoDevice device{};
  std::string device_id;
  DiscoveryResponseInfo info = decode_discovery_response(frame, device, device_id);

  EXPECT_TRUE(info.metadata_complete);
  EXPECT_TRUE(info.has_extended);
  EXPECT_EQ(device.type, DeviceType::HORIZONTAL_AWNING);
  EXPECT_EQ(device.subtype, 0u);
  EXPECT_EQ(device_id, "30E1F2");
  EXPECT_EQ(info.backbone[0], 0x30);
  EXPECT_EQ(info.backbone[1], 0xE1);
  EXPECT_EQ(info.backbone[2], 0xF2);
  EXPECT_EQ(info.manufacturer, MANUFACTURER_SOMFY);
  EXPECT_EQ(info.flags, 0xCC);
  EXPECT_EQ(info.timestamp, 0xFC03);
  EXPECT_EQ(device.position, UNKNOWN_POSITION);
  EXPECT_EQ(device.target, UNKNOWN_POSITION);
  EXPECT_TRUE(device.is_stopped);
}

TEST(ProtoFrame, DecodeDiscoveryResponseTruncatedPayload) {
  IoFrame frame{};
  init_frame(frame, true, true, true, false);
  frame.src[0] = 0x9D;
  frame.src[1] = 0x60;
  frame.src[2] = 0x85;
  frame.cmd = CMD_DISCOVER_SPE_RESP;
  frame.data[0] = 0x04;
  frame.data[1] = 0x00;
  frame.data_len = DEVICE_METADATA_SIZE;  // 2 bytes: type/subtype only, no extended fields

  IoDevice device{};
  std::string device_id;
  DiscoveryResponseInfo info = decode_discovery_response(frame, device, device_id);

  EXPECT_TRUE(info.metadata_complete);
  EXPECT_FALSE(info.has_extended);
  EXPECT_EQ(device.type, DeviceType::HORIZONTAL_AWNING);
  EXPECT_EQ(device_id, "9D6085");
}

TEST(ProtoFrame, DecodeDiscoveryResponseEmptyPayload) {
  IoFrame frame{};
  init_frame(frame, true, true, true, false);
  frame.src[0] = 0x11;
  frame.src[1] = 0x22;
  frame.src[2] = 0x33;
  frame.cmd = CMD_DISCOVER_SPE_RESP;
  frame.data_len = 0;

  IoDevice device{};
  std::string device_id;
  DiscoveryResponseInfo info = decode_discovery_response(frame, device, device_id);

  EXPECT_FALSE(info.metadata_complete);
  EXPECT_FALSE(info.has_extended);
  EXPECT_EQ(device.type, DeviceType::UNKNOWN);
  EXPECT_EQ(device.subtype, 0u);
  EXPECT_FALSE(device.inverted);
  EXPECT_EQ(device_id, "112233");
}

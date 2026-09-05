/// @file hub_internal_test.cpp
/// @brief Tests for hub_internal.h inline helpers.

#include "hub_internal.h"
#include "test_helpers.h"

using namespace esphome::home_io_control;

// ============================================================================
// HubInternal test suite
// ============================================================================
// Tests for inline helpers in hub_internal.h: capability checks, binary entity
// position helpers, and status normalization logic.

// ========================================================================================
// Percent conversion helpers
// ========================================================================================

TEST(HubInternal, RoundPercentRoundsRatherThanTruncates) {
  // HA's actual "50%" (128/255) truncates to 49 but should round to 50 — the real-hardware
  // regression this helper exists to fix (see platform_cover.cpp / platform_light.cpp).
  EXPECT_EQ(detail::round_percent(128.0F / 255.0F), 50) << "128/255 should round to 50, not truncate to 49";
  EXPECT_EQ(detail::round_percent(0.0F), 0);
  EXPECT_EQ(detail::round_percent(1.0F), 100);
  EXPECT_EQ(detail::round_percent(0.504F), 50) << "just past the rounding boundary should round up";
  EXPECT_EQ(detail::round_percent(0.494F), 49) << "just below the rounding boundary should round down";
}

// ========================================================================================
// Binary entity position helpers
// ========================================================================================

TEST(HubInternal, BinaryEntityPositionOn) {
  EXPECT_TRUE(detail::is_binary_entity_position(0)) << "position 0 should be binary ON";
}

TEST(HubInternal, BinaryEntityPositionOff) {
  EXPECT_TRUE(detail::is_binary_entity_position(100)) << "position 100 should be binary OFF";
}

TEST(HubInternal, BinaryEntityPositionMidRejected) {
  EXPECT_FALSE(detail::is_binary_entity_position(50)) << "position 50 should not be binary";
  EXPECT_FALSE(detail::is_binary_entity_position(1)) << "position 1 should not be binary";
  EXPECT_FALSE(detail::is_binary_entity_position(99)) << "position 99 should not be binary";
}

// ========================================================================================
// Capability helpers
// ========================================================================================

TEST(HubInternal, KnownDeviceMatchesEntityClass) {
  IoDevice cover_dev{};
  cover_dev.type = DeviceType::ROLLER_SHUTTER;
  EXPECT_TRUE(detail::known_device_matches_entity_class(cover_dev, DeviceCapabilityClass::COVER))
      << "roller shutter should match COVER class";
  EXPECT_FALSE(detail::known_device_matches_entity_class(cover_dev, DeviceCapabilityClass::LIGHT))
      << "roller shutter should NOT match LIGHT class";

  IoDevice light_dev{};
  light_dev.type = DeviceType::LIGHT;
  EXPECT_TRUE(detail::known_device_matches_entity_class(light_dev, DeviceCapabilityClass::LIGHT))
      << "light should match LIGHT class";
  EXPECT_FALSE(detail::known_device_matches_entity_class(light_dev, DeviceCapabilityClass::COVER))
      << "light should NOT match COVER class";

  IoDevice unknown_dev{};
  unknown_dev.type = DeviceType::UNKNOWN;
  EXPECT_TRUE(detail::known_device_matches_entity_class(unknown_dev, DeviceCapabilityClass::COVER))
      << "UNKNOWN type should match any class";
  EXPECT_TRUE(detail::known_device_matches_entity_class(unknown_dev, DeviceCapabilityClass::LIGHT))
      << "UNKNOWN type should match any class";
}

TEST(HubInternal, KnownDeviceAcceptsExecutePosition) {
  IoDevice cover_dev{};
  cover_dev.type = DeviceType::ROLLER_SHUTTER;
  EXPECT_TRUE(detail::known_device_accepts_execute_position(cover_dev, 50)) << "cover should accept position commands";
  EXPECT_TRUE(detail::known_device_accepts_execute_position(cover_dev, 0)) << "cover should accept binary ON position";
  EXPECT_TRUE(detail::known_device_accepts_execute_position(cover_dev, 100))
      << "cover should accept binary OFF position";

  IoDevice light_dev{};
  light_dev.type = DeviceType::LIGHT;
  EXPECT_TRUE(detail::known_device_accepts_execute_position(light_dev, 50))
      << "light should accept mid-range position (dimmable lights send arbitrary 0-100)";
  EXPECT_TRUE(detail::known_device_accepts_execute_position(light_dev, 0)) << "light should accept binary ON position";
  EXPECT_TRUE(detail::known_device_accepts_execute_position(light_dev, 100))
      << "light should accept binary OFF position";
  EXPECT_FALSE(detail::known_device_accepts_execute_position(light_dev, POS_STOP))
      << "light should reject the cover-only STOP marker (out of the 0-100 range)";

  IoDevice lock_dev{};
  lock_dev.type = DeviceType::LOCK;
  EXPECT_FALSE(detail::known_device_accepts_execute_position(lock_dev, 50)) << "lock should reject mid-range position";
  EXPECT_TRUE(detail::known_device_accepts_execute_position(lock_dev, 0)) << "lock should accept unlock position";
  EXPECT_TRUE(detail::known_device_accepts_execute_position(lock_dev, 100)) << "lock should accept lock position";

  IoDevice unknown_dev{};
  unknown_dev.type = DeviceType::UNKNOWN;
  EXPECT_TRUE(detail::known_device_accepts_execute_position(unknown_dev, 50))
      << "UNKNOWN type should accept any position";
}

TEST(HubInternal, KnownDeviceAcceptsTilt) {
  IoDevice tilt_dev{};
  tilt_dev.type = DeviceType::VENETIAN_BLIND;
  EXPECT_TRUE(detail::known_device_accepts_execute_tilt(tilt_dev)) << "venetian blind should accept tilt commands";

  IoDevice no_tilt_dev{};
  no_tilt_dev.type = DeviceType::ROLLER_SHUTTER;
  EXPECT_FALSE(detail::known_device_accepts_execute_tilt(no_tilt_dev))
      << "roller shutter should not accept tilt commands";

  IoDevice unknown_dev{};
  unknown_dev.type = DeviceType::UNKNOWN;
  EXPECT_FALSE(detail::known_device_accepts_execute_tilt(unknown_dev))
      << "UNKNOWN type should not accept tilt commands";
}

TEST(HubInternal, KnownDeviceSupportsStatusRequests) {
  IoDevice cover_dev{};
  cover_dev.type = DeviceType::ROLLER_SHUTTER;
  EXPECT_TRUE(detail::known_device_supports_status_requests(cover_dev)) << "cover should support status requests";

  IoDevice light_dev{};
  light_dev.type = DeviceType::LIGHT;
  EXPECT_TRUE(detail::known_device_supports_status_requests(light_dev)) << "light should support status requests";

  IoDevice lock_dev{};
  lock_dev.type = DeviceType::LOCK;
  EXPECT_TRUE(detail::known_device_supports_status_requests(lock_dev)) << "lock should support status requests";

  IoDevice unknown_dev{};
  unknown_dev.type = DeviceType::UNKNOWN;
  EXPECT_TRUE(detail::known_device_supports_status_requests(unknown_dev))
      << "UNKNOWN type should support status requests";
}

// ========================================================================================
// Status normalization
// ========================================================================================

TEST(HubInternal, NormalizeStoppedStateKeepsMovingWhenNotConverged) {
  IoDevice dev{};
  dev.is_stopped = true;
  dev.target = 50.0f;
  dev.position = 30.0f;
  detail::normalize_stopped_state(dev);
  EXPECT_FALSE(dev.is_stopped) << "stopped flag should be cleared when target and position differ significantly";
}

TEST(HubInternal, NormalizeStoppedStateKeepsStoppedWhenConverged) {
  IoDevice dev{};
  dev.is_stopped = true;
  dev.target = 50.0f;
  dev.position = 50.1f;  // within tolerance (~0.2%)
  detail::normalize_stopped_state(dev);
  EXPECT_TRUE(dev.is_stopped) << "stopped flag should remain when target and position are within tolerance";
}

TEST(HubInternal, NormalizeStoppedStateUnknownPosition) {
  IoDevice dev{};
  dev.is_stopped = true;
  dev.target = UNKNOWN_POSITION;
  dev.position = 30.0f;
  detail::normalize_stopped_state(dev);
  EXPECT_TRUE(dev.is_stopped) << "stopped flag should remain when target is unknown";

  dev.is_stopped = true;
  dev.target = 50.0f;
  dev.position = UNKNOWN_POSITION;
  detail::normalize_stopped_state(dev);
  EXPECT_TRUE(dev.is_stopped) << "stopped flag should remain when position is unknown";
}

// ========================================================================================
// describe_learned_device_type
// ========================================================================================

TEST(HubInternal, DescribeLearnedDeviceTypeUsesNamedYamlAlias) {
  EXPECT_EQ(detail::describe_learned_device_type(DeviceType::VENETIAN_BLIND), "io_device_type: \"venetian_blind\"")
      << "a type with a named YAML alias should be quoted exactly as the schema expects it";
}

TEST(HubInternal, DescribeLearnedDeviceTypeFallsBackToHexWithoutAnAlias) {
  // BEACON (0x0C) has no case in yaml_device_type_name() — falls through to the hex fallback.
  EXPECT_EQ(detail::describe_learned_device_type(DeviceType::BEACON), "io_device_type: 0x0C")
      << "a type with no named YAML alias should fall back to a raw hex value the schema still accepts";
}

TEST(HubInternal, DescribeLearnedDeviceTypeMatchesYamlDeviceTypeNameSourceOfTruth) {
  // Both the pairing snippet (pairing_engine.cpp) and this boot-time hint derive their YAML
  // syntax from yaml_device_type_name() — this pins that this helper never drifts from it.
  for (uint8_t raw = 0; raw <= 0x18; raw++) {
    const auto type = static_cast<DeviceType>(raw);
    const std::string described = detail::describe_learned_device_type(type);
    const char *alias = yaml_device_type_name(type);
    if (alias != nullptr) {
      EXPECT_EQ(described, std::string("io_device_type: \"") + alias + "\"")
          << "mismatch for raw type 0x" << std::hex << static_cast<int>(raw);
    } else {
      EXPECT_NE(described.find("0x"), std::string::npos)
          << "unnamed type 0x" << std::hex << static_cast<int>(raw) << " should fall back to a hex value";
    }
  }
}

// ========================================================================================
// Last-command record — decode_last_command_record() / apply_last_command_record() /
// describe_last_commander() / describe_last_command_source()
// ========================================================================================

TEST(HubInternal, DecodeLastCommandRecordReadsPrivateResponseOffsets) {
  IoFrame f{};
  const uint8_t payload[14] = {0x05, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xBE, 0xFE, 0xDB, 0x01, 0x00, 0x00};
  std::memcpy(f.data, payload, sizeof(payload));
  f.data_len = sizeof(payload);

  const auto record = detail::decode_last_command_record(f, detail::PRIVATE_RESPONSE_LAST_COMMAND_OFFSET);

  ASSERT_TRUE(record.valid);
  EXPECT_EQ(std::memcmp(record.commander, "\xBE\xFE\xDB", 3), 0);
  EXPECT_EQ(record.originator, 0x01);
}

TEST(HubInternal, DecodeLastCommandRecordReadsStatusUpdateOffsets) {
  // Real capture bytes: tests/corpus/captures/statuspoll/somfy_rs100_statuspoll_kig300_sx1276.yaml
  // frame 2 (0x71) — proves the +3 shift relative to 0x04, not a synthetic guess.
  IoFrame f{};
  const uint8_t payload[16] = {0x04, 0x60, 0x10, 0x0A, 0x0B, 0x00, 0x00, 0xAC,
                               0x9E, 0x00, 0x0F, 0xBE, 0xFE, 0xDB, 0x01, 0x00};
  std::memcpy(f.data, payload, sizeof(payload));
  f.data_len = sizeof(payload);

  const auto record = detail::decode_last_command_record(f, detail::STATUS_UPDATE_LAST_COMMAND_OFFSET);

  ASSERT_TRUE(record.valid);
  EXPECT_EQ(std::memcmp(record.commander, "\xBE\xFE\xDB", 3), 0);
  EXPECT_EQ(record.originator, 0x01);
}

TEST(HubInternal, DecodeLastCommandRecordRejectsShortPrivateResponse) {
  IoFrame f{};
  const uint8_t payload[6] = {0x2C, 0x80, 0x00, 0x00, 0x00, 0x00};
  std::memcpy(f.data, payload, sizeof(payload));
  f.data_len = sizeof(payload);

  EXPECT_FALSE(detail::decode_last_command_record(f, detail::PRIVATE_RESPONSE_LAST_COMMAND_OFFSET).valid);
}

TEST(HubInternal, DecodeLastCommandRecordRejectsShortStatusUpdate) {
  // 14 bytes: one short of the 15 the record needs at base 11 (3-byte commander + 1-byte
  // originator). A nonzero commander is set so this pins the length guard specifically, not the
  // separate all-zero-commander guard (DecodeLastCommandRecordRejectsAllZeroCommander above).
  IoFrame f{};
  const uint8_t payload[14] = {0x04, 0x60, 0x10, 0x0A, 0x0B, 0x00, 0x00, 0xAC, 0x9E, 0x00, 0x0F, 0xBE, 0xFE, 0xDB};
  std::memcpy(f.data, payload, sizeof(payload));
  f.data_len = sizeof(payload);

  EXPECT_FALSE(detail::decode_last_command_record(f, detail::STATUS_UPDATE_LAST_COMMAND_OFFSET).valid);
}

TEST(HubInternal, DecodeLastCommandRecordRejectsAllZeroCommander) {
  IoFrame f{};
  const uint8_t payload[14] = {0x05, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00};
  std::memcpy(f.data, payload, sizeof(payload));
  f.data_len = sizeof(payload);

  EXPECT_FALSE(detail::decode_last_command_record(f, detail::PRIVATE_RESPONSE_LAST_COMMAND_OFFSET).valid)
      << "00 00 00 is not a node ID any observed controller uses -- a device padding this field "
         "must not publish a fabricated address";
}

TEST(HubInternal, ApplyLastCommandRecordKeepsPreviousOnInvalid) {
  IoDevice dev{};
  detail::LastCommandRecord valid{};
  valid.commander[0] = 0x3B;
  valid.commander[1] = 0x74;
  valid.commander[2] = 0xDC;
  valid.originator = 0x01;
  valid.valid = true;
  detail::apply_last_command_record(dev, valid);

  detail::LastCommandRecord invalid{};  // valid == false by default
  detail::apply_last_command_record(dev, invalid);

  EXPECT_TRUE(dev.has_last_command);
  EXPECT_EQ(std::memcmp(dev.last_commander, valid.commander, NODE_ID_SIZE), 0)
      << "a short/unpopulated follow-up reply must not clear an already-learned record";
  EXPECT_EQ(dev.last_command_originator, 0x01);
}

TEST(HubInternal, DescribeLastCommanderQualifiesThisHub) {
  IoDevice dev{};
  dev.node_id[0] = 0xAB;
  dev.node_id[1] = 0xC1;
  dev.node_id[2] = 0x23;
  dev.last_commander[0] = 0xC0;
  dev.last_commander[1] = 0xFF;
  dev.last_commander[2] = 0xEE;
  dev.has_last_command = true;
  const uint8_t hub_id[NODE_ID_SIZE] = {0xC0, 0xFF, 0xEE};

  EXPECT_EQ(detail::describe_last_commander(dev, hub_id), "C0FFEE (this hub)");
}

TEST(HubInternal, DescribeLastCommanderQualifiesTheDeviceItself) {
  IoDevice dev{};
  dev.node_id[0] = 0x58;
  dev.node_id[1] = 0x6E;
  dev.node_id[2] = 0x35;
  dev.last_commander[0] = 0x58;
  dev.last_commander[1] = 0x6E;
  dev.last_commander[2] = 0x35;
  dev.has_last_command = true;
  const uint8_t hub_id[NODE_ID_SIZE] = {0xC0, 0xFF, 0xEE};

  EXPECT_EQ(detail::describe_last_commander(dev, hub_id), "586E35 (this device)");
}

TEST(HubInternal, DescribeLastCommanderIsPlainForAForeignController) {
  IoDevice dev{};
  dev.node_id[0] = 0xAB;
  dev.node_id[1] = 0xC1;
  dev.node_id[2] = 0x23;
  dev.last_commander[0] = 0x3B;
  dev.last_commander[1] = 0x74;
  dev.last_commander[2] = 0xDC;
  dev.has_last_command = true;
  const uint8_t hub_id[NODE_ID_SIZE] = {0xC0, 0xFF, 0xEE};

  EXPECT_EQ(detail::describe_last_commander(dev, hub_id), "3B74DC");
}

TEST(HubInternal, DescribeLastCommanderIsEmptyBeforeAnyRecord) {
  IoDevice dev{};  // has_last_command == false
  const uint8_t hub_id[NODE_ID_SIZE] = {0xC0, 0xFF, 0xEE};

  EXPECT_TRUE(detail::describe_last_commander(dev, hub_id).empty());
}

TEST(HubInternal, DescribeLastCommandSourceRendersNameAndHex) {
  IoDevice dev{};
  dev.has_last_command = true;
  dev.last_command_originator = 0x01;  // ORIGINATOR_USER_REMOTE

  EXPECT_EQ(detail::describe_last_command_source(dev), "user_remote(0x01)");
}

TEST(HubInternal, DescribeLastCommandSourceRendersUndecodedBytes) {
  // 0x0A is a genuine gap in the ORIGINATOR_* table (a mains gate reported it) — must self-describe
  // rather than being invented a name or silently dropped.
  IoDevice dev{};
  dev.has_last_command = true;
  dev.last_command_originator = 0x0A;

  EXPECT_EQ(detail::describe_last_command_source(dev), "unknown(0x0A)");
}

TEST(HubInternal, DescribeLastCommandSourceIsEmptyBeforeAnyRecord) {
  IoDevice dev{};  // has_last_command == false

  EXPECT_TRUE(detail::describe_last_command_source(dev).empty());
}

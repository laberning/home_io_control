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
  EXPECT_FALSE(detail::known_device_accepts_execute_position(light_dev, 50))
      << "light should reject mid-range position";
  EXPECT_TRUE(detail::known_device_accepts_execute_position(light_dev, 0)) << "light should accept binary ON position";
  EXPECT_TRUE(detail::known_device_accepts_execute_position(light_dev, 100))
      << "light should accept binary OFF position";

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
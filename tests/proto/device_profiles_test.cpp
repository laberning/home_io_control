#include "proto_device_model.h"
#include "hub_decisions.h"

#include "test_helpers.h"

using namespace esphome::home_io_control;

// ============================================================================
// DeviceProfile test suite
// ============================================================================
// Device capability queries: which device types support position vs binary control
// and whether status requests are allowed. Ensures entity feature mapping is safe.
// Device profile capability tests
// ========================================================================================

TEST(DeviceProfile, PositionControlSupportedForCovers) {
  EXPECT_TRUE(device_supports_position_control(DeviceType::VERTICAL_EXTERIOR_AWNING))
      << "awning should support position control";
  EXPECT_TRUE(device_supports_position_control(DeviceType::ROLLER_SHUTTER))
      << "roller shutter should support position control";
  EXPECT_TRUE(device_supports_position_control(DeviceType::INTERIOR_VENETIAN_BLIND))
      << "venetian blind should support position control";
  EXPECT_FALSE(device_supports_position_control(DeviceType::SECURE_CONFIGURATION_DEVICE))
      << "a secure configuration device is a commissioning tool, not a cover";
}

TEST(DeviceProfile, PositionControlNotSupportedForLights) {
  EXPECT_FALSE(device_supports_position_control(DeviceType::LIGHT)) << "light should not report position control";
  EXPECT_FALSE(device_supports_position_control(DeviceType::ON_OFF_SWITCH))
      << "switch should not report position control";
}

TEST(DeviceProfile, BinaryControlSupportedForLightsAndSwitches) {
  EXPECT_TRUE(device_supports_binary_control(DeviceType::LIGHT)) << "light should support binary control";
  EXPECT_TRUE(device_supports_binary_control(DeviceType::ON_OFF_SWITCH)) << "switch should support binary control";
}

TEST(DeviceProfile, BinaryControlNotSupportedForCovers) {
  EXPECT_FALSE(device_supports_binary_control(DeviceType::VERTICAL_EXTERIOR_AWNING))
      << "cover should not support binary control";
}

TEST(DeviceProfile, LockControlSupportedForLocksOnly) {
  EXPECT_TRUE(device_supports_lock_control(DeviceType::LOCK)) << "lock should support lock control";
  EXPECT_FALSE(device_supports_lock_control(DeviceType::LIGHT)) << "light should not report lock control";
}

TEST(DeviceProfile, ClimateControlSupportedForTheAdjustableSetpointHeaterOnly) {
  EXPECT_TRUE(device_supports_climate_control(DeviceType::ELECTRICAL_HEATER, 1))
      << "the electrical heater with an adjustable setpoint is the device the heating payloads fit";
  EXPECT_FALSE(device_supports_climate_control(DeviceType::ELECTRICAL_HEATER, 0))
      << "the plain electrical heater is driven with the standard EXECUTE, which no platform sends yet";
  EXPECT_FALSE(device_supports_climate_control(DeviceType::ELECTRICAL_HEATER, 3))
      << "an unnamed subtype falls back to the plain heater";
  EXPECT_FALSE(device_supports_climate_control(DeviceType::HEATING_TEMPERATURE_INTERFACE, 0));
  EXPECT_FALSE(device_supports_climate_control(DeviceType::EXTERIOR_HEATING, 0));
  EXPECT_FALSE(device_supports_climate_control(DeviceType::HEAT_PUMP, 0));
  EXPECT_FALSE(device_supports_climate_control(DeviceType::HEAT_PUMP, 1)) << "Atlantic profile uses other registers";
  EXPECT_FALSE(device_supports_climate_control(DeviceType::VERTICAL_EXTERIOR_AWNING, 0))
      << "cover should not report climate control";
  EXPECT_FALSE(device_supports_climate_control(DeviceType::LIGHT, 0)) << "light should not report climate control";
  EXPECT_FALSE(device_supports_climate_control(DeviceType::UNKNOWN, 0)) << "unknown should not report climate control";
}

TEST(DeviceProfile, StatusRequestsSupportedForControllableDevices) {
  EXPECT_TRUE(device_supports_status_requests(DeviceType::VERTICAL_EXTERIOR_AWNING))
      << "cover should support status requests";
  EXPECT_TRUE(device_supports_status_requests(DeviceType::LIGHT)) << "light should support status requests";
  EXPECT_TRUE(device_supports_status_requests(DeviceType::ON_OFF_SWITCH)) << "switch should support status requests";
  EXPECT_TRUE(device_supports_status_requests(DeviceType::LOCK)) << "lock should support status requests";
}

TEST(DeviceProfile, OperationProfileNames) {
  EXPECT_STREQ(device_operation_profile_name(DeviceType::VERTICAL_EXTERIOR_AWNING), "cover_position")
      << "awning profile should match";
  EXPECT_STREQ(device_operation_profile_name(DeviceType::INTERIOR_VENETIAN_BLIND), "cover_position_tilt")
      << "venetian blind profile should include tilt";
  EXPECT_STREQ(device_operation_profile_name(DeviceType::LIGHT), "binary_on_off") << "light profile should match";
  EXPECT_STREQ(device_operation_profile_name(DeviceType::ON_OFF_SWITCH), "binary_on_off")
      << "switch profile should match";
  EXPECT_STREQ(device_operation_profile_name(DeviceType::LOCK), "lock") << "lock profile should match";
  EXPECT_STREQ(device_operation_profile_name(DeviceType::UNKNOWN), "unknown") << "unknown profile should match";
}

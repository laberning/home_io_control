#include "hub_decisions.h"
#include "proto_frame.h"

#include "test_helpers.h"

using namespace esphome::home_io_control;
using namespace esphome::home_io_control::decisions;

// ============================================================================
// ProtocolBoundary test suite
// ============================================================================
// Tests for protocol values and boundary behavior directly extracted from
// doxygen comments that are NOT covered by the existing 141 passing tests.

// ========================================================================================
// HubDecisions: frame_matches_nodes (not tested directly, only indirectly via
// frame_matches_exchange_endpoints in decisions_test.cpp)
// ========================================================================================

TEST(ProtocolBoundary, FrameMatchesNodes_Positive) {
  uint8_t src[3] = {0x44, 0x55, 0x66};
  uint8_t dst[3] = {0x11, 0x22, 0x33};
  IoFrame f{};
  init_frame(f, true, true, false, false);
  set_src(f, src);
  set_dst(f, dst);

  EXPECT_TRUE(frame_matches_nodes(f, src, dst)) << "frame with matching src/dst should match";
}

TEST(ProtocolBoundary, FrameMatchesNodes_MismatchedSrc) {
  uint8_t src[3] = {0x44, 0x55, 0x66};
  uint8_t dst[3] = {0x11, 0x22, 0x33};
  uint8_t wrong_src[3] = {0x99, 0x99, 0x99};
  IoFrame f{};
  init_frame(f, true, true, false, false);
  set_src(f, src);
  set_dst(f, dst);

  EXPECT_FALSE(frame_matches_nodes(f, wrong_src, dst)) << "frame with mismatched src should not match";
}

TEST(ProtocolBoundary, FrameMatchesNodes_MismatchedDst) {
  uint8_t src[3] = {0x44, 0x55, 0x66};
  uint8_t dst[3] = {0x11, 0x22, 0x33};
  uint8_t wrong_dst[3] = {0x99, 0x99, 0x99};
  IoFrame f{};
  init_frame(f, true, true, false, false);
  set_src(f, src);
  set_dst(f, dst);

  EXPECT_FALSE(frame_matches_nodes(f, src, wrong_dst)) << "frame with mismatched dst should not match";
}

TEST(ProtocolBoundary, FrameMatchesNodes_ReverseIsNotMatch) {
  // "reverse" means src==expected_dst AND dst==expected_src — this is an
  // exchange-endpoints check, NOT a direct match. frame_matches_nodes must
  // return false for the reverse.
  uint8_t src[3] = {0x44, 0x55, 0x66};
  uint8_t dst[3] = {0x11, 0x22, 0x33};
  IoFrame f{};
  init_frame(f, true, true, false, false);
  set_src(f, src);
  set_dst(f, dst);

  // Caller reversed src/dst - direct match must be false
  EXPECT_FALSE(frame_matches_nodes(f, dst, src)) << "swapped src/dst must not match directly";
}

// ========================================================================================
// HubDecisions: is_exchange_internal_command
// doxygen: "Returns true for CMD_CHALLENGE_REQ and CMD_CHALLENGE_RESP; these are
//           ephemeral cryptographic handshake frames that carry no useful info
//           for a passive observer."
// ========================================================================================

TEST(ProtocolBoundary, IsExchangeInternalCommand_ChallengeReq) {
  EXPECT_TRUE(is_exchange_internal_command(CMD_CHALLENGE_REQ))
      << "CMD_CHALLENGE_REQ (0x3C) must be classified as exchange-internal";
}

TEST(ProtocolBoundary, IsExchangeInternalCommand_ChallengeResp) {
  EXPECT_TRUE(is_exchange_internal_command(CMD_CHALLENGE_RESP))
      << "CMD_CHALLENGE_RESP (0x3D) must be classified as exchange-internal";
}

TEST(ProtocolBoundary, IsExchangeInternalCommand_StatusRespNotInternal) {
  EXPECT_FALSE(is_exchange_internal_command(CMD_PRIVATE_RESP))
      << "CMD_PRIVATE_RESP (0x04) must NOT be exchange-internal — it carries real status data";
}

TEST(ProtocolBoundary, IsExchangeInternalCommand_ExecuteNotInternal) {
  EXPECT_FALSE(is_exchange_internal_command(CMD_EXECUTE))
      << "CMD_EXECUTE (0x00) must NOT be exchange-internal — it is the user command";
}

TEST(ProtocolBoundary, IsExchangeInternalCommand_ZeroNotInternal) {
  // Edge value 0x00 should not be internal — it is CMD_EXECUTE
  EXPECT_FALSE(is_exchange_internal_command(0x00)) << "command 0x00 must not be exchange-internal";
}

// ========================================================================================
// Device support queries: device_supports_tilt
// doxygen: "true for venetian blinds, blinds, external venetian blinds, louvre blinds"
// device_supports_tilt is in proto_frame.h (doxygen) / proto_frame.cpp (impl)
// ========================================================================================

TEST(ProtocolBoundary, DeviceSupportsTilt_VenetianBlind) {
  EXPECT_TRUE(device_supports_tilt(DeviceType::VENETIAN_BLIND)) << "VENETIAN_BLIND should support tilt";
}

TEST(ProtocolBoundary, DeviceSupportsTilt_Blind) {
  EXPECT_TRUE(device_supports_tilt(DeviceType::BLIND)) << "BLIND should support tilt (covered by doxygen list)";
}

TEST(ProtocolBoundary, DeviceSupportsTilt_ExternalVenetianBlind) {
  EXPECT_TRUE(device_supports_tilt(DeviceType::EXTERNAL_VENETIAN_BLIND))
      << "EXTERNAL_VENETIAN_BLIND should support tilt";
}

TEST(ProtocolBoundary, DeviceSupportsTilt_LouvreBlind) {
  EXPECT_TRUE(device_supports_tilt(DeviceType::LOUVRE_BLIND)) << "LOUVRE_BLIND should support tilt";
}

TEST(ProtocolBoundary, DeviceSupportsTilt_RollerShutterNotTilt) {
  EXPECT_FALSE(device_supports_tilt(DeviceType::ROLLER_SHUTTER)) << "ROLLER_SHUTTER must NOT support tilt";
}

TEST(ProtocolBoundary, DeviceSupportsTilt_AwningNotTilt) {
  EXPECT_FALSE(device_supports_tilt(DeviceType::AWNING)) << "AWNING must NOT support tilt";
}

TEST(ProtocolBoundary, DeviceSupportsTilt_LightNotTilt) {
  EXPECT_FALSE(device_supports_tilt(DeviceType::LIGHT)) << "LIGHT must NOT support tilt";
}

TEST(ProtocolBoundary, DeviceSupportsTilt_UnknownNotTilt) {
  EXPECT_FALSE(device_supports_tilt(DeviceType::UNKNOWN)) << "UNKNOWN must NOT support tilt";
}

// ========================================================================================
// Device capability class: additional types from the doxygen-enumerated DeviceType
// that are in proto_frame.h but not tested in device_profiles_test.cpp
// ========================================================================================

TEST(ProtocolBoundary, DeviceCapabilityClass_WindowOpenerIsCover) {
  EXPECT_EQ(device_capability_class(DeviceType::WINDOW_OPENER), DeviceCapabilityClass::COVER)
      << "WINDOW_OPENER must be classified as COVER";
}

TEST(ProtocolBoundary, DeviceCapabilityClass_GarageOpenerIsCover) {
  EXPECT_EQ(device_capability_class(DeviceType::GARAGE_OPENER), DeviceCapabilityClass::COVER)
      << "GARAGE_OPENER must be classified as COVER";
}

TEST(ProtocolBoundary, DeviceCapabilityClass_GateOpenerIsCover) {
  EXPECT_EQ(device_capability_class(DeviceType::GATE_OPENER), DeviceCapabilityClass::COVER)
      << "GATE_OPENER must be classified as COVER";
}

TEST(ProtocolBoundary, DeviceCapabilityClass_RollingDoorOpenerIsCover) {
  EXPECT_EQ(device_capability_class(DeviceType::ROLLING_DOOR_OPENER), DeviceCapabilityClass::COVER)
      << "ROLLING_DOOR_OPENER must be classified as COVER";
}

TEST(ProtocolBoundary, DeviceCapabilityClass_DualShutterIsCover) {
  EXPECT_EQ(device_capability_class(DeviceType::DUAL_SHUTTER), DeviceCapabilityClass::COVER)
      << "DUAL_SHUTTER must be classified as COVER";
}

TEST(ProtocolBoundary, DeviceCapabilityClass_VentilationPointIsSwitch) {
  EXPECT_EQ(device_capability_class(DeviceType::VENTILATION_POINT), DeviceCapabilityClass::SWITCH)
      << "VENTILATION_POINT must be classified as SWITCH (binary on/off)";
}

TEST(ProtocolBoundary, DeviceCapabilityClass_ExteriorHeatingIsClimate) {
  EXPECT_EQ(device_capability_class(DeviceType::EXTERIOR_HEATING), DeviceCapabilityClass::CLIMATE)
      << "EXTERIOR_HEATING must be classified as CLIMATE";
}

TEST(ProtocolBoundary, DeviceCapabilityClass_HeatPumpIsClimate) {
  EXPECT_EQ(device_capability_class(DeviceType::HEAT_PUMP), DeviceCapabilityClass::CLIMATE)
      << "HEAT_PUMP must be classified as CLIMATE";
}

TEST(ProtocolBoundary, DeviceCapabilityClass_VerticalDescending) {
  // ExternalVenetianBlind is explicitly in the COVER switch arm of device_capability_class
  EXPECT_EQ(device_capability_class(DeviceType::EXTERNAL_VENETIAN_BLIND), DeviceCapabilityClass::COVER)
      << "EXTERNAL_VENETIAN_BLIND must be classified as COVER";
}

TEST(ProtocolBoundary, DeviceCapabilityClass_CurtainTrackIsCover) {
  EXPECT_EQ(device_capability_class(DeviceType::CURTAIN_TRACK), DeviceCapabilityClass::COVER)
      << "CURTAIN_TRACK must be classified as COVER";
}

TEST(ProtocolBoundary, DeviceCapabilityClass_SwingingShutterIsCover) {
  EXPECT_EQ(device_capability_class(DeviceType::SWINGING_SHUTTER), DeviceCapabilityClass::COVER)
      << "SWINGING_SHUTTER must be classified as COVER";
}

// ========================================================================================
// Device operation profile name: LOCK, CLIMATE, SENSOR, BEACON not yet covered
// ========================================================================================

TEST(ProtocolBoundary, DeviceOperationProfileName_Lock) {
  EXPECT_STREQ(device_operation_profile_name(DeviceType::LOCK), "lock")
      << "LOCK device type should have 'lock' operation profile";
}

TEST(ProtocolBoundary, DeviceOperationProfileName_Beacon) {
  EXPECT_STREQ(device_operation_profile_name(DeviceType::BEACON), "beacon")
      << "BEACON device type should have 'beacon' operation profile";
}

TEST(ProtocolBoundary, DeviceOperationProfileName_IntrusionAlarm) {
  EXPECT_STREQ(device_operation_profile_name(DeviceType::INTRUSION_ALARM), "sensor")
      << "INTRUSION_ALARM device type should have 'sensor' operation profile";
}

TEST(ProtocolBoundary, DeviceOperationProfileName_HeatingInterface) {
  EXPECT_STREQ(device_operation_profile_name(DeviceType::HEATING_TEMPERATURE_INTERFACE), "climate")
      << "HEATING_TEMPERATURE_INTERFACE device type should have 'climate' operation profile";
}

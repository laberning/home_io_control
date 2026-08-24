#include "proto_commands.h"
#include "proto_crypto.h"
#include "proto_frame.h"

#include "test_helpers.h"

#include <cstring>

using namespace esphome::home_io_control;

// ============================================================================
// Command builder tests
// ============================================================================
// Verify each command builder produces correct frame structure, payload, and flags.

// ========================================================================================
// Discovery and key exchange
// ========================================================================================

TEST(ProtoCommands, CreateDiscover) {
  IoFrame frame{};
  ASSERT_TRUE(create_discover(frame, test::OWN_ID)) << "create_discover should succeed";
  EXPECT_EQ(frame.cmd, CMD_DISCOVER_REQ) << "discover command should be CMD_DISCOVER_REQ (0x28)";
  EXPECT_EQ(frame.data_len, 0) << "discover frame should have no payload";
  EXPECT_TRUE(is_start(frame) && is_end(frame)) << "discover should be both start and end frame";
}

TEST(ProtoCommands, CreateDiscoverResp) {
  IoFrame frame{};
  ASSERT_TRUE(
      create_discover_resp(frame, test::OWN_ID, test::DST_ID, DeviceType::ROLLER_SHUTTER, 5, MANUFACTURER_SOMFY))
      << "create_discover_resp should succeed";
  EXPECT_EQ(frame.cmd, CMD_DISCOVER_RESP) << "discover-resp command should be CMD_DISCOVER_RESP (0x29)";
  EXPECT_EQ(frame.data_len, DISCOVERY_RESP_FULL_SIZE) << "discover-resp should carry the full 9-byte payload";
  EXPECT_TRUE(is_start(frame) && is_end(frame)) << "discover-resp should be both start and end frame";
  EXPECT_FALSE((frame.ctrl1 & CTRL1_LOW_POWER) != 0) << "discover-resp is device-originated, not low-power targeted";
  EXPECT_EQ(decode_packed_device_type(frame.data[0], frame.data[1]), DeviceType::ROLLER_SHUTTER)
      << "packed type should round-trip through decode_packed_device_type";
  EXPECT_EQ(decode_packed_device_subtype(frame.data[1]), 5) << "packed subtype should round-trip";
  EXPECT_EQ(0, memcmp(&frame.data[DISCOVERY_RESP_BACKBONE_OFFSET], test::OWN_ID, NODE_ID_SIZE))
      << "backbone address should be our own node ID, matching the real-capture cross-check";
  EXPECT_EQ(frame.data[DISCOVERY_RESP_MANUFACTURER_OFFSET], MANUFACTURER_SOMFY)
      << "manufacturer byte should be as supplied";
  EXPECT_EQ(frame.data[DISCOVERY_RESP_FLAGS_OFFSET], 0xDD)
      << "flags byte is a best-effort placeholder (see proto_commands.h @note / "
         "KEY_EXTRACTION_DISCOVER_RESP_FLAGS in proto_commands.cpp) — pinned here so a future change "
         "to it is deliberate, not accidental";
  EXPECT_EQ(frame.data[DISCOVERY_RESP_TIMESTAMP_OFFSET], 0x00);
  EXPECT_EQ(frame.data[DISCOVERY_RESP_TIMESTAMP_OFFSET + 1], 0x0E);
}

TEST(ProtoCommands, CreateKeyConfirm) {
  IoFrame frame{};
  ASSERT_TRUE(create_key_confirm(frame, test::OWN_ID, test::DST_ID)) << "create_key_confirm should succeed";
  EXPECT_EQ(frame.cmd, CMD_KEY_CONFIRM) << "key-confirm command should be CMD_KEY_CONFIRM (0x33)";
  EXPECT_EQ(frame.data_len, 0) << "key-confirm should have no payload";
  EXPECT_FALSE(is_start(frame)) << "key-confirm should not be a start frame";
  EXPECT_TRUE(is_end(frame)) << "key-confirm closes the key exchange CMD_KEY_INIT opened — real devices' 0x33 "
                                "sets END (somfy_dimmer/velux_kux100 pairing_full captures)";
  EXPECT_FALSE((frame.ctrl1 & CTRL1_LOW_POWER) != 0) << "key-confirm is device-originated, not low-power targeted";
}

TEST(ProtoCommands, CreateDiscoverConfirmAck) {
  IoFrame frame{};
  ASSERT_TRUE(create_discover_confirm_ack(frame, test::OWN_ID, test::DST_ID))
      << "create_discover_confirm_ack should succeed";
  EXPECT_EQ(frame.cmd, CMD_DISCOVER_CONFIRM_ACK) << "command should be CMD_DISCOVER_CONFIRM_ACK (0x2D)";
  EXPECT_EQ(frame.data_len, 0) << "discovery-confirm ack should have no payload";
  EXPECT_FALSE(is_start(frame)) << "the hub's 0x2C is the start of this two-frame handshake, not our 0x2D";
  EXPECT_TRUE(is_end(frame)) << "our 0x2D closes it — real devices' 0x2D sets END (velux_kux100 capture)";
  EXPECT_FALSE((frame.ctrl1 & CTRL1_LOW_POWER) != 0)
      << "discovery-confirm ack is device-originated, not low-power targeted";
}

TEST(ProtoCommands, CreateChallengeReqUsesTheFramingObservedOnAir) {
  IoFrame device_role{};
  ASSERT_TRUE(create_challenge_req_device_role(device_role, test::DST_ID, test::OWN_ID, test::TEST_CHALLENGE))
      << "create_challenge_req_device_role should succeed";
  EXPECT_EQ(device_role.cmd, CMD_CHALLENGE_REQ) << "command should be CMD_CHALLENGE_REQ (0x3C)";
  EXPECT_EQ(0, memcmp(device_role.data, test::TEST_CHALLENGE, HMAC_SIZE))
      << "the supplied challenge bytes should appear verbatim in the frame payload";
  // The two bits that distinguish this from the controller-role builder. A real device answering a
  // hub's key-init sets neither (somfy_dimmer/pairing_full.yaml's 0x3C is `0E 00 ...`): the frame
  // continues the hub's already-open exchange, and LOW_POWER describes a controller's *target*.
  EXPECT_FALSE(is_start(device_role)) << "device-role challenge-req should not be a start frame";
  EXPECT_FALSE(is_end(device_role)) << "device-role challenge-req should not be an end frame";
  EXPECT_FALSE((device_role.ctrl1 & CTRL1_LOW_POWER) != 0)
      << "device-role challenge-req is addressed to a mains-powered hub";

  // The controller-role builder now matches. It used to set START and LOW_POWER, but field
  // captures show every 0x3C on a real network is `0E 00` regardless of who sent it, and the
  // hub's own `4E 20` challenges were never once answered. See create_challenge_req().
  IoFrame controller_role{};
  ASSERT_TRUE(create_challenge_req(controller_role, test::DST_ID, test::OWN_ID, test::TEST_CHALLENGE));
  EXPECT_FALSE(is_start(controller_role)) << "no 0x3C observed on air is a start frame";
  EXPECT_FALSE((controller_role.ctrl1 & CTRL1_LOW_POWER) != 0) << "no 0x3C observed on air sets LOW_POWER";
  EXPECT_EQ(controller_role.ctrl0, device_role.ctrl0) << "both roles now use the observed framing";
  EXPECT_EQ(controller_role.ctrl1, device_role.ctrl1);
}

TEST(ProtoCommands, CreateChallengeReqWithSuppliedChallengeMatchesGenerated) {
  IoFrame frame{};
  ASSERT_TRUE(create_challenge_req(frame, test::DST_ID, test::OWN_ID, test::TEST_CHALLENGE))
      << "create_challenge_req with a supplied challenge should succeed";
  EXPECT_EQ(frame.cmd, CMD_CHALLENGE_REQ) << "challenge-req command should be CMD_CHALLENGE_REQ (0x3C)";
  EXPECT_EQ(0, memcmp(frame.data, test::TEST_CHALLENGE, HMAC_SIZE))
      << "the supplied challenge bytes should appear verbatim in the frame payload";
}

TEST(ProtoCommands, CreateKeyInit) {
  IoFrame frame{};
  ASSERT_TRUE(create_key_init(frame, test::OWN_ID, test::DST_ID)) << "create_key_init should succeed";
  EXPECT_EQ(frame.cmd, CMD_KEY_INIT) << "key-init command should be CMD_KEY_INIT (0x31)";
  EXPECT_EQ(frame.data_len, 0) << "key-init should have no payload";
  EXPECT_TRUE(is_start(frame)) << "key-init should be a start frame";
  EXPECT_FALSE(is_end(frame)) << "key-init should not be an end frame";
}

// ========================================================================================
// Challenge and status responses
// ========================================================================================

TEST(ProtoCommands, CreateChallengeReq) {
  IoFrame frame{};
  ASSERT_TRUE(create_challenge_req(frame, test::DST_ID, test::OWN_ID)) << "create_challenge_req should succeed";
  EXPECT_EQ(frame.cmd, CMD_CHALLENGE_REQ) << "challenge-req command should be CMD_CHALLENGE_REQ (0x3C)";
  EXPECT_EQ(frame.data_len, HMAC_SIZE) << "challenge-req should carry 6-byte random challenge";
  // Framing matches every 0x3C seen on a real network (`0E 00 ...`); see create_challenge_req().
  EXPECT_FALSE(is_start(frame)) << "challenge-req should not be a start frame";
  EXPECT_FALSE(is_end(frame)) << "challenge-req should not be an end frame";
  EXPECT_FALSE((frame.ctrl1 & CTRL1_LOW_POWER) != 0) << "challenge-req should not set LOW_POWER";
}

TEST(ProtoCommands, CreateStatusUpdateResp) {
  IoFrame frame{};
  ASSERT_TRUE(create_status_update_resp(frame, test::OWN_ID, test::DST_ID))
      << "create_status_update_resp should succeed";
  EXPECT_EQ(frame.cmd, CMD_STATUS_UPDATE_RESP) << "status-update-resp command should be CMD_STATUS_UPDATE_RESP (0x72)";
  EXPECT_EQ(frame.data_len, 2) << "status-update-resp should have 2-byte payload";
  EXPECT_EQ(frame.data[0], 0x05) << "status-update-resp payload byte 0 should be 0x05";
  EXPECT_EQ(frame.data[1], 0x00) << "status-update-resp payload byte 1 should be 0x00";
  EXPECT_FALSE(is_start(frame)) << "status-update-resp should not be a start frame";
  EXPECT_TRUE(is_end(frame)) << "status-update-resp should be an end frame";
  EXPECT_TRUE((frame.ctrl1 & CTRL1_LOW_POWER) != 0) << "device-targeted frame should set LOW_POWER";
}

// ========================================================================================
// Config and execute commands
// ========================================================================================

TEST(ProtoCommands, CreateSetConfig1) {
  IoFrame frame{};
  ASSERT_TRUE(create_set_config1(frame, test::OWN_ID, test::DST_ID)) << "create_set_config1 should succeed";
  EXPECT_EQ(frame.cmd, CMD_SET_CONFIG1) << "set-config1 command should be CMD_SET_CONFIG1 (0x6F)";
  EXPECT_EQ(frame.data_len, 5) << "set-config1 should have 5-byte payload";
  EXPECT_EQ(frame.data[0], 0xE0) << "set-config1 payload byte 0 should be 0xE0";
  EXPECT_EQ(frame.data[1], 0x10) << "set-config1 payload byte 1 should be 0x10";
  EXPECT_EQ(frame.data[2], 0x0A) << "set-config1 payload byte 2 should be 0x0A";
  EXPECT_EQ(frame.data[3], 0x08) << "set-config1 payload byte 3 should be 0x08";
  EXPECT_EQ(frame.data[4], 0x00) << "set-config1 payload byte 4 should be 0x00";
  EXPECT_TRUE(is_start(frame)) << "set-config1 should be a start frame";
  EXPECT_FALSE(is_end(frame)) << "set-config1 should not be an end frame";
  EXPECT_TRUE((frame.ctrl1 & CTRL1_LOW_POWER) != 0) << "device-targeted frame should set LOW_POWER";
}

TEST(ProtoCommands, CreateGetStatus) {
  IoFrame frame{};
  ASSERT_TRUE(create_get_status(frame, test::OWN_ID, test::DST_ID)) << "create_get_status should succeed";
  EXPECT_EQ(frame.cmd, CMD_PRIVATE) << "get-status command should be CMD_PRIVATE (0x03)";
  EXPECT_EQ(frame.data_len, 3) << "get-status should have 3-byte payload";
  EXPECT_EQ(frame.data[0], 0x03) << "get-status sub-command byte 0 should be 0x03";
  EXPECT_EQ(frame.data[1], 0x00) << "get-status sub-command byte 1 should be 0x00";
  EXPECT_EQ(frame.data[2], 0x00) << "get-status sub-command byte 2 should be 0x00";
  EXPECT_TRUE(is_start(frame)) << "get-status should be a start frame";
  EXPECT_FALSE(is_end(frame)) << "get-status should not be an end frame";
}

TEST(ProtoCommands, CreateGetName) {
  IoFrame frame{};
  ASSERT_TRUE(create_get_name(frame, test::OWN_ID, test::DST_ID, true)) << "create_get_name should succeed";
  EXPECT_EQ(frame.cmd, CMD_GET_NAME) << "get-name command should be CMD_GET_NAME (0x50)";
  EXPECT_EQ(frame.data_len, 0) << "get-name should have no payload";
  EXPECT_TRUE(is_start(frame)) << "get-name should be a start frame";
  EXPECT_FALSE(is_end(frame)) << "get-name should not be an end frame";
}

TEST(ProtoCommands, CreateSetName) {
  IoFrame frame{};
  const uint8_t payload[DEVICE_NAME_WRITE_PAYLOAD_SIZE] = {'P', 'a', 't', 'i', 'o'};

  ASSERT_TRUE(create_set_name(frame, test::OWN_ID, test::DST_ID, payload)) << "create_set_name should succeed";
  EXPECT_EQ(frame.cmd, CMD_SET_NAME) << "set-name command should be CMD_SET_NAME (0x52)";
  EXPECT_EQ(frame.data_len, DEVICE_NAME_WRITE_PAYLOAD_SIZE) << "set-name should use the fixed 16-byte payload";
  EXPECT_EQ(frame.data[0], 'P');
  EXPECT_EQ(frame.data[1], 'a');
  EXPECT_EQ(frame.data[2], 't');
  EXPECT_EQ(frame.data[3], 'i');
  EXPECT_EQ(frame.data[4], 'o');
  for (size_t index = 5; index < DEVICE_NAME_WRITE_PAYLOAD_SIZE; index++)
    EXPECT_EQ(frame.data[index], 0x00) << "set-name payload should preserve zero padding";
  EXPECT_TRUE(is_start(frame)) << "set-name should be a start frame";
  EXPECT_FALSE(is_end(frame)) << "set-name should not be an end frame";
  EXPECT_TRUE((frame.ctrl1 & CTRL1_LOW_POWER) != 0) << "device-targeted frame should set LOW_POWER";
}

TEST(ProtoCommands, CreateIdentify) {
  IoFrame frame{};
  ASSERT_TRUE(create_identify(frame, test::OWN_ID, test::DST_ID)) << "create_identify should succeed";
  EXPECT_EQ(frame.cmd, CMD_IDENTIFY) << "identify command should be CMD_IDENTIFY (0x1E)";
  EXPECT_EQ(frame.data_len, 2) << "identify should carry a 2-byte payload";
  EXPECT_EQ(frame.data[0], 0x01) << "identify payload byte 0 (origin) should be 0x01";
  EXPECT_EQ(frame.data[1], 0xFF) << "identify payload byte 1 (parameter) should be 0xFF";
  EXPECT_TRUE(is_start(frame)) << "identify should be a start frame";
  EXPECT_FALSE(is_end(frame)) << "identify should not be an end frame";
  EXPECT_TRUE((frame.ctrl1 & CTRL1_LOW_POWER) != 0) << "device-targeted frame should set LOW_POWER";
}

TEST(ProtoCommands, CreateGetStatusTilt) {
  IoFrame frame{};
  ASSERT_TRUE(create_get_status_tilt(frame, test::OWN_ID, test::DST_ID)) << "create_get_status_tilt should succeed";
  EXPECT_EQ(frame.cmd, CMD_PRIVATE) << "tilt-aware get-status should still use CMD_PRIVATE (0x03)";
  EXPECT_EQ(frame.data_len, 4) << "tilt-aware get-status should have 4-byte payload";
  EXPECT_EQ(frame.data[0], 0x03);
  EXPECT_EQ(frame.data[1], STATUS_TILT_SELECTOR);
  EXPECT_EQ(frame.data[2], 0x01);
  EXPECT_EQ(frame.data[3], 0x00);
}

TEST(ProtoCommands, CreateExecuteTilt) {
  IoFrame frame{};
  ASSERT_TRUE(create_execute_tilt(frame, test::OWN_ID, test::DST_ID, true, 25)) << "create_execute_tilt should succeed";
  EXPECT_EQ(frame.cmd, CMD_EXECUTE) << "tilt execute should still use CMD_EXECUTE (0x00)";
  EXPECT_EQ(frame.data_len, 8) << "tilt execute should use 8-byte payload";
  EXPECT_EQ(frame.data[0], 0x01) << "tilt originator should be user";
  EXPECT_EQ(frame.data[1], 0x63) << "tilt ACEI should use user_default priority (0x63), as a real 2W hub does";
  EXPECT_EQ(frame.data[2], POS_UNKNOWN) << "tilt execute should keep position unchanged via unknown marker";
  EXPECT_EQ(frame.data[3], 0x00);
  EXPECT_EQ(frame.data[4], STATUS_TILT_SELECTOR) << "tilt execute should set the tilt separator flag";
  EXPECT_EQ(frame.data[5], 0x96) << "25% open corresponds to 75% closed => 0x9600";
  EXPECT_EQ(frame.data[6], 0x00);
  EXPECT_EQ(frame.data[7], 0x00);
}

TEST(ProtoCommands, CreateExecutePositionAndTilt) {
  IoFrame frame{};
  // Position 50% (IO closed) + tilt 75% open
  ASSERT_TRUE(create_execute_position_and_tilt(frame, test::OWN_ID, test::DST_ID, true, 50, 75))
      << "create_execute_position_and_tilt should succeed";
  EXPECT_EQ(frame.cmd, CMD_EXECUTE) << "combined command should use CMD_EXECUTE (0x00)";
  EXPECT_EQ(frame.data_len, 8) << "combined command should use 8-byte payload";
  EXPECT_EQ(frame.data[0], 0x01) << "originator should be user";
  EXPECT_EQ(frame.data[1], 0x63) << "ACEI should be user_default priority (0x63), as a real 2W hub does";
  EXPECT_EQ(frame.data[2], 100) << "position 50% => 2*50 = 100 (0x64)";
  EXPECT_EQ(frame.data[3], 0x00);
  EXPECT_EQ(frame.data[4], STATUS_TILT_SELECTOR) << "tilt separator flag (FP3 bitmap)";
  // tilt 75% open = 25% closed => 25 * 0xC800 / 100 = 0x3200
  EXPECT_EQ(frame.data[5], 0x32) << "tilt MSB for 75% open (25% closed)";
  EXPECT_EQ(frame.data[6], 0x00) << "tilt LSB";
  EXPECT_EQ(frame.data[7], 0x00);
}

TEST(ProtoCommands, CreateExecutePositionAndTilt_FullOpen) {
  IoFrame frame{};
  // Position 0% (fully open) + tilt 100% (fully open)
  ASSERT_TRUE(create_execute_position_and_tilt(frame, test::OWN_ID, test::DST_ID, false, 0, 100));
  EXPECT_EQ(frame.data[2], 0x00) << "position 0% => 2*0 = 0";
  EXPECT_EQ(frame.data[5], 0x00) << "tilt 100% open = 0% closed => 0x0000";
  EXPECT_EQ(frame.data[6], 0x00);
}

TEST(ProtoCommands, CreateExecutePositionAndTilt_FullClosed) {
  IoFrame frame{};
  // Position 100% (fully closed) + tilt 0% (fully closed)
  ASSERT_TRUE(create_execute_position_and_tilt(frame, test::OWN_ID, test::DST_ID, false, 100, 0));
  EXPECT_EQ(frame.data[2], 200) << "position 100% => 2*100 = 200 (0xC8)";
  EXPECT_EQ(frame.data[5], 0xC8) << "tilt 0% open = 100% closed => 0xC800 MSB";
  EXPECT_EQ(frame.data[6], 0x00) << "tilt 0% open = 100% closed => 0xC800 LSB";
}

TEST(ProtoCommands, CreateExecutePositionAndTilt_RejectsOverLimit) {
  IoFrame frame{};
  EXPECT_FALSE(create_execute_position_and_tilt(frame, test::OWN_ID, test::DST_ID, true, 101, 50))
      << "position > 100 should be rejected";
}

// ========================================================================================
// Key transfer and challenge response
// ========================================================================================

TEST(ProtoCommands, CreateKeyTransferSize) {
  IoFrame key_init{};
  ASSERT_TRUE(create_key_init(key_init, test::OWN_ID, test::DST_ID)) << "key-init should be created first";
  IoFrame key_transfer{};
  ASSERT_TRUE(create_key_transfer(key_transfer, key_init, test::DST_ID, test::OWN_ID, test::TEST_SYSTEM_KEY,
                                  test::TEST_CHALLENGE))
      << "create_key_transfer should succeed";
  EXPECT_EQ(key_transfer.cmd, CMD_KEY_TRANSFER) << "key-transfer command should be CMD_KEY_TRANSFER (0x32)";
  EXPECT_EQ(key_transfer.data_len, AES_KEY_SIZE) << "key-transfer payload should be 16 bytes (encrypted system key)";
  EXPECT_FALSE(is_start(key_transfer)) << "key-transfer should not be a start frame";
  EXPECT_FALSE(is_end(key_transfer)) << "key-transfer should not be an end frame (middle frame)";
  EXPECT_TRUE((key_transfer.ctrl1 & CTRL1_LOW_POWER) != 0) << "device-targeted frame should set LOW_POWER";
}

TEST(ProtoCommands, CreateChallengeRespHmacSize) {
  IoFrame origin = test::make_execute(0);
  IoFrame auth_resp{};
  ASSERT_TRUE(
      create_challenge_resp(auth_resp, test::DST_ID, test::OWN_ID, test::TEST_CHALLENGE, origin, test::TEST_SYSTEM_KEY))
      << "create_challenge_resp should succeed";
  EXPECT_EQ(auth_resp.cmd, CMD_CHALLENGE_RESP) << "challenge-resp command should be CMD_CHALLENGE_RESP (0x3D)";
  EXPECT_EQ(auth_resp.data_len, HMAC_SIZE) << "challenge-resp should carry 6-byte HMAC";
  EXPECT_FALSE(is_start(auth_resp)) << "challenge-resp should not be a start frame";
  EXPECT_FALSE(is_end(auth_resp)) << "challenge-resp should not be an end frame (continuation)";
  EXPECT_TRUE((auth_resp.ctrl1 & CTRL1_LOW_POWER) != 0) << "device-targeted frame should set LOW_POWER";
}

TEST(ProtoCommands, CreateDiscoveryRequest_0x28_Defaults) {
  IoFrame frame{};
  ASSERT_TRUE(create_discovery_request(frame, test::OWN_ID, CMD_DISCOVER_REQ, BROADCAST_DISCOVER, false, false, 0,
                                       test::TEST_SYSTEM_KEY))
      << "create_discovery_request should succeed for 0x28";
  EXPECT_EQ(frame.cmd, CMD_DISCOVER_REQ) << "command should be CMD_DISCOVER_REQ";
  EXPECT_EQ(frame.data_len, 0) << "0x28 should have no payload";
  EXPECT_TRUE(is_start(frame) && is_end(frame)) << "discovery should be start+end";
  EXPECT_FALSE((frame.ctrl1 & CTRL1_LOW_POWER) != 0) << "low_power=false should be clear";
}

TEST(ProtoCommands, CreateDiscoveryRequest_0x28_LowPower) {
  IoFrame frame{};
  ASSERT_TRUE(create_discovery_request(frame, test::OWN_ID, CMD_DISCOVER_REQ, BROADCAST_DISCOVER, true, false, 0,
                                       test::TEST_SYSTEM_KEY));
  EXPECT_TRUE((frame.ctrl1 & CTRL1_LOW_POWER) != 0) << "low_power=true should be set";
}

TEST(ProtoCommands, CreateDiscoveryRequest_0x2E_NoPayload) {
  IoFrame frame{};
  ASSERT_TRUE(create_discovery_request(frame, test::OWN_ID, CMD_DISCOVER_ALT_REQ, BROADCAST_DISCOVER_ALT, false, false,
                                       0, test::TEST_SYSTEM_KEY))
      << "create_discovery_request should succeed for 0x2E with no payload";
  EXPECT_EQ(frame.cmd, CMD_DISCOVER_ALT_REQ);
  EXPECT_EQ(frame.data_len, 0);
}

TEST(ProtoCommands, CreateDiscoveryRequest_0x2E_WithPayload) {
  IoFrame frame{};
  ASSERT_TRUE(create_discovery_request(frame, test::OWN_ID, CMD_DISCOVER_ALT_REQ, BROADCAST_DISCOVER_ALT, false, true,
                                       0x00, test::TEST_SYSTEM_KEY))
      << "create_discovery_request should succeed for 0x2E with payload";
  EXPECT_EQ(frame.cmd, CMD_DISCOVER_ALT_REQ);
  EXPECT_EQ(frame.data_len, 1);
  EXPECT_EQ(frame.data[0], 0x00);
}

TEST(ProtoCommands, CreateDiscoveryRequest_0x2A_PayloadSize) {
  IoFrame frame{};
  ASSERT_TRUE(create_discovery_request(frame, test::OWN_ID, CMD_DISCOVER_SPE_REQ, BROADCAST_DISCOVER, false, false, 0,
                                       test::TEST_SYSTEM_KEY))
      << "create_discovery_request should succeed for 0x2A";
  EXPECT_EQ(frame.cmd, CMD_DISCOVER_SPE_REQ);
  EXPECT_EQ(frame.data_len, 12) << "0x2A payload should be 6 random + 6 HMAC bytes";
}

TEST(ProtoCommands, CreateDiscoveryRequest_0x2A_HmacCoversCommandByteOnly) {
  // Pins the transcript, which is the whole point of this payload: the 6-byte HMAC covers the
  // command byte alone, authenticated against the nonce that precedes it in the same frame.
  // Asserting only the length (the test above) let a wrong transcript ship — every emitted 0x2A
  // carried an HMAC no device could verify. Real-hardware ground truth for the rule lives in
  // CorpusCryptoSelfAuthenticated (corpus_crypto_test.cpp).
  IoFrame frame{};
  ASSERT_TRUE(create_discovery_request(frame, test::OWN_ID, CMD_DISCOVER_SPE_REQ, BROADCAST_DISCOVER, false, false, 0,
                                       test::TEST_SYSTEM_KEY));
  ASSERT_EQ(frame.data_len, HMAC_SIZE * 2);
  const uint8_t *nonce = frame.data;
  const uint8_t *hmac = frame.data + HMAC_SIZE;

  const uint8_t cmd = CMD_DISCOVER_SPE_REQ;
  EXPECT_TRUE(crypto::verify_hmac(&cmd, 1, hmac, nonce, test::TEST_SYSTEM_KEY))
      << "0x2A HMAC must verify over the command byte alone, with the nonce as challenge";

  uint8_t cmd_plus_nonce[HMAC_SIZE + 1] = {cmd};
  std::memcpy(cmd_plus_nonce + 1, nonce, HMAC_SIZE);
  EXPECT_FALSE(crypto::verify_hmac(cmd_plus_nonce, sizeof(cmd_plus_nonce), hmac, nonce, test::TEST_SYSTEM_KEY))
      << "the pre-2026-08 [cmd, nonce] transcript must not come back";
}

TEST(ProtoCommands, CreateDiscoveryRequest_0x2A_RequiresSystemKey) {
  IoFrame frame{};
  EXPECT_FALSE(
      create_discovery_request(frame, test::OWN_ID, CMD_DISCOVER_SPE_REQ, BROADCAST_DISCOVER, false, false, 0, nullptr))
      << "0x2A should require a system key";
}

TEST(ProtoCommands, CreateDiscoveryRequest_UnsupportedCommand) {
  IoFrame frame{};
  EXPECT_FALSE(
      create_discovery_request(frame, test::OWN_ID, 0xFF, BROADCAST_DISCOVER, false, false, 0, test::TEST_SYSTEM_KEY))
      << "unsupported command should fail";
}

// ========================================================================================
// Typed execute command builders (create_execute_position / create_execute_command)
// ========================================================================================

TEST(ProtoCommands, CreateExecutePositionZero) {
  IoFrame frame{};
  ASSERT_TRUE(create_execute_position(frame, test::OWN_ID, test::DST_ID, true, 0))
      << "create_execute_position(0) should succeed";
  EXPECT_EQ(frame.cmd, CMD_EXECUTE) << "position execute should use CMD_EXECUTE (0x00)";
  EXPECT_EQ(frame.data_len, 8) << "position execute should use 8-byte payload";
  EXPECT_EQ(frame.data[0], ORIGINATOR_USER_REMOTE) << "originator should be USER_REMOTE";
  EXPECT_EQ(frame.data[1], 0x63) << "ACEI byte should be 0x63 (user_default priority, matching a real 2W hub)";
  EXPECT_EQ(frame.data[2], 0x00) << "position 0 doubles to 0x00";
  EXPECT_EQ(frame.data[3], 0x00) << "position 0 second byte should be 0x00";
  EXPECT_TRUE(is_start(frame)) << "position execute should be a start frame";
  EXPECT_FALSE(is_end(frame)) << "position execute should not be an end frame";
}

TEST(ProtoCommands, CreateExecutePositionHundred) {
  IoFrame frame{};
  ASSERT_TRUE(create_execute_position(frame, test::OWN_ID, test::DST_ID, false, 100))
      << "create_execute_position(100) should succeed";
  EXPECT_EQ(frame.data[2], 0xC8) << "position 100 doubles to 0xC8";
  EXPECT_EQ(frame.data[3], 0x00);
  EXPECT_EQ(frame.data_len, 8);
}

TEST(ProtoCommands, CreateExecutePositionFifty) {
  IoFrame frame{};
  ASSERT_TRUE(create_execute_position(frame, test::OWN_ID, test::DST_ID, true, 50))
      << "create_execute_position(50) should succeed";
  EXPECT_EQ(frame.data[2], 0x64) << "position 50 doubles to 0x64";
  EXPECT_EQ(frame.data[3], 0x00);
}

TEST(ProtoCommands, CreateExecutePositionRejectsAboveHundred) {
  IoFrame frame{};
  EXPECT_FALSE(create_execute_position(frame, test::OWN_ID, test::DST_ID, true, 101))
      << "create_execute_position should reject position > 100";
  EXPECT_FALSE(create_execute_position(frame, test::OWN_ID, test::DST_ID, true, 200))
      << "create_execute_position should reject position > 100";
  EXPECT_FALSE(create_execute_position(frame, test::OWN_ID, test::DST_ID, true, POS_STOP))
      << "create_execute_position should reject POS_STOP (use create_execute_command instead)";
}

TEST(ProtoCommands, CreateExecutePositionDoublesAcrossFullRange) {
  // The wire encoding doubles the 0-100 percent value into a single byte (0-200); checked across
  // the entire valid range rather than a few spot values, since an off-by-one at either end
  // would otherwise slip through.
  for (uint16_t pos = 0; pos <= 100; pos++) {
    IoFrame frame{};
    ASSERT_TRUE(create_execute_position(frame, test::OWN_ID, test::DST_ID, true, static_cast<uint8_t>(pos)))
        << "create_execute_position(" << pos << ") should succeed";
    EXPECT_EQ(frame.cmd, CMD_EXECUTE) << "mismatch at position " << pos;
    EXPECT_EQ(frame.data_len, 8) << "mismatch at position " << pos;
    EXPECT_EQ(frame.data[2], static_cast<uint8_t>(2 * pos)) << "doubled position mismatch at position " << pos;
    EXPECT_EQ(frame.data[3], 0x00) << "second position byte mismatch at position " << pos;
  }
}

TEST(ProtoCommands, CreateExecuteCommandStop) {
  IoFrame frame{};
  ASSERT_TRUE(create_execute_command(frame, test::OWN_ID, test::DST_ID, true, CoverCommand::STOP))
      << "create_execute_command(STOP) should succeed";
  EXPECT_EQ(frame.cmd, CMD_EXECUTE) << "command execute should use CMD_EXECUTE (0x00)";
  EXPECT_EQ(frame.data_len, 6) << "command execute should use 6-byte special payload";
  EXPECT_EQ(frame.data[0], ORIGINATOR_USER_REMOTE) << "originator should be USER_REMOTE";
  EXPECT_EQ(frame.data[1], 0x63) << "ACEI byte should be 0x63 (user_default priority, matching a real 2W hub)";
  EXPECT_EQ(frame.data[2], POS_STOP) << "stop command main byte should be POS_STOP (0xD2)";
  EXPECT_EQ(frame.data[3], 0x00) << "stop command modifier should be 0x00";
  EXPECT_TRUE(is_start(frame));
  EXPECT_FALSE(is_end(frame));
}

TEST(ProtoCommands, CreateExecuteCommandFavorite) {
  IoFrame frame{};
  ASSERT_TRUE(create_execute_command(frame, test::OWN_ID, test::DST_ID, false, CoverCommand::FAVORITE))
      << "create_execute_command(FAVORITE) should succeed";
  EXPECT_EQ(frame.cmd, CMD_EXECUTE);
  EXPECT_EQ(frame.data_len, 6) << "favorite should use 6-byte special payload";
  EXPECT_EQ(frame.data[0], ORIGINATOR_USER_REMOTE) << "originator should be USER_REMOTE";
  EXPECT_EQ(frame.data[1], 0x63) << "ACEI byte should be 0x63 (user_default priority, matching a real 2W hub)";
  EXPECT_EQ(frame.data[2], POS_FAVORITE) << "favorite main byte should be POS_FAVORITE (0xD8)";
  EXPECT_EQ(frame.data[3], 0x00) << "favorite modifier should be 0x00";
  EXPECT_EQ(frame.data[4], 0x00);
  EXPECT_EQ(frame.data[5], 0x00);
}

TEST(ProtoCommands, CreateExecuteCommandVent) {
  IoFrame frame{};
  ASSERT_TRUE(create_execute_command(frame, test::OWN_ID, test::DST_ID, true, CoverCommand::VENT))
      << "create_execute_command(VENT) should succeed";
  EXPECT_EQ(frame.cmd, CMD_EXECUTE);
  EXPECT_EQ(frame.data_len, 6) << "vent should use 6-byte special payload";
  EXPECT_EQ(frame.data[2], POS_FAVORITE) << "vent shares main byte 0xD8 with favorite";
  EXPECT_EQ(frame.data[3], POS_VENT_MODIFIER) << "vent modifier should be POS_VENT_MODIFIER (0x03)";
}

TEST(ProtoCommands, CreateExecuteCommandRejectsForceOpen) {
  // FORCE_OPEN needs a device-specific "fully open" position (see create_force_open()) that
  // this generic dispatch cannot supply, so it must be rejected here rather than silently
  // building a wrong-direction command.
  IoFrame frame{};
  EXPECT_FALSE(create_execute_command(frame, test::OWN_ID, test::DST_ID, false, CoverCommand::FORCE_OPEN));
}

TEST(ProtoCommands, CreateForceOpenNonInverted) {
  IoFrame frame{};
  ASSERT_TRUE(create_force_open(frame, test::OWN_ID, test::DST_ID, false, 0)) << "create_force_open should succeed";
  EXPECT_EQ(frame.cmd, CMD_EXECUTE) << "force open should use CMD_EXECUTE (0x00)";
  EXPECT_EQ(frame.data_len, 8) << "force open should use the ordinary 8-byte position payload, not a special byte";
  EXPECT_EQ(frame.data[0], ORIGINATOR_USER_REMOTE) << "originator should be USER_REMOTE";
  EXPECT_EQ(frame.data[1], 0x03) << "force open ACEI should be elevated priority level 0 (protection_human): 0x03";
  EXPECT_EQ(frame.data[2], 0x00) << "force open position LSB should be 0x00 for a non-inverted device";
  EXPECT_EQ(frame.data[3], 0x00) << "force open position MSB should be 0x00";
  EXPECT_TRUE(is_start(frame));
  EXPECT_FALSE(is_end(frame));
}

TEST(ProtoCommands, CreateForceOpenInvertedDevice) {
  // An inverted device's wire-scale "fully open" is 100, not 0 (see IoDevice::inverted /
  // platform_cover.cpp's ha<->io mapping) — the caller passes that in explicitly.
  IoFrame frame{};
  ASSERT_TRUE(create_force_open(frame, test::OWN_ID, test::DST_ID, false, 100));
  EXPECT_EQ(frame.data[2], 0xC8) << "position 100 doubles to 0x00C8 LSB, matching create_execute_position(100)";
  EXPECT_EQ(frame.data[3], 0x00) << "position 100 doubles to 0x00C8 MSB should be 0x00";
}

TEST(ProtoCommands, CreateForceOpenMatchesOrdinaryOpenExceptAcei) {
  // FORCE_OPEN should be identical to an ordinary create_execute_position() move to the same
  // position except for the elevated ACEI byte — it is not a special-payload command like
  // STOP/FAVORITE/VENT.
  IoFrame force_frame{}, open_frame{};
  ASSERT_TRUE(create_force_open(force_frame, test::OWN_ID, test::DST_ID, true, 0));
  ASSERT_TRUE(create_execute_position(open_frame, test::OWN_ID, test::DST_ID, true, 0));
  EXPECT_EQ(force_frame.data_len, open_frame.data_len);
  EXPECT_NE(force_frame.data[1], open_frame.data[1]) << "ACEI byte should differ (elevated priority)";
  for (uint8_t i = 0; i < force_frame.data_len; i++) {
    if (i == 1)
      continue;
    EXPECT_EQ(force_frame.data[i], open_frame.data[i]) << "data[" << (int) i << "] should match ordinary open";
  }
}

TEST(ProtoCommands, CreateExecuteCommandVentDiffersFromFavoriteInModifier) {
  IoFrame vent_frame{}, fav_frame{};
  ASSERT_TRUE(create_execute_command(vent_frame, test::OWN_ID, test::DST_ID, true, CoverCommand::VENT));
  ASSERT_TRUE(create_execute_command(fav_frame, test::OWN_ID, test::DST_ID, true, CoverCommand::FAVORITE));
  // Both share the same main byte (0xD8) but differ in the modifier (byte 3)
  EXPECT_EQ(vent_frame.data[2], fav_frame.data[2]) << "main byte should be same (POS_FAVORITE/0xD8)";
  EXPECT_NE(vent_frame.data[3], fav_frame.data[3]) << "modifier byte should differ between vent and favorite";
  EXPECT_EQ(vent_frame.data[3], POS_VENT_MODIFIER) << "vent modifier should be 0x03";
  EXPECT_EQ(fav_frame.data[3], 0x00) << "favorite modifier should be 0x00";
}

TEST(ProtoCommands, CoverCommandNameLookup) {
  EXPECT_STREQ(cover_command_name(CoverCommand::STOP), "STOP");
  EXPECT_STREQ(cover_command_name(CoverCommand::FAVORITE), "FAVORITE");
  EXPECT_STREQ(cover_command_name(CoverCommand::VENT), "VENT");
  EXPECT_STREQ(cover_command_name(CoverCommand::FORCE_OPEN), "FORCE_OPEN");
}

// ============================================================================
// Silent operation: the reference hub's slower travel profile, isolated by A/B capture on
// 2026-08-15 with the Velux app's toggle flipped between otherwise identical close commands.
// ============================================================================

TEST(ProtoCommands, CreateExecutePositionSilentOnlyChangesTheProfileByte) {
  // A Somfy hub with silent on sends `01 67 00 00 80 D8 05 00` and with it off `... 80 D8 06 00`,
  // for the same command in the same direction. Only the profile byte moves; our ACEI differs from
  // that hub's by design (see EXECUTE_ACEI), so compare the payload shape rather than all 8 bytes.
  IoFrame silent{};
  ASSERT_TRUE(create_execute_position(silent, test::OWN_ID, test::DST_ID, true, 100, /*silent=*/true));
  ASSERT_EQ(silent.data_len, 8u);
  EXPECT_EQ(silent.data[4], 0x80) << "the extended block is present either way";
  EXPECT_EQ(silent.data[5], POS_FAVORITE) << "silent does not disturb the secondary-target byte";
  EXPECT_EQ(silent.data[6], 0x05) << "0x05 is the silent travel profile";
}

TEST(ProtoCommands, CreateExecutePositionDefaultsToNonSilent) {
  IoFrame normal{};
  ASSERT_TRUE(create_execute_position(normal, test::OWN_ID, test::DST_ID, true, 100));
  IoFrame explicit_off{};
  ASSERT_TRUE(create_execute_position(explicit_off, test::OWN_ID, test::DST_ID, true, 100, /*silent=*/false));
  EXPECT_EQ(memcmp(normal.data, explicit_off.data, normal.data_len), 0) << "omitting the flag must mean not silent";

  IoFrame silent{};
  ASSERT_TRUE(create_execute_position(silent, test::OWN_ID, test::DST_ID, true, 100, /*silent=*/true));
  // Exactly one byte separates the two forms, which is what the controlled capture showed.
  EXPECT_EQ(normal.data_len, silent.data_len);
  for (uint8_t i = 0; i < normal.data_len; i++) {
    if (i == 6)
      continue;
    EXPECT_EQ(normal.data[i], silent.data[i])
        << "silent must only move the profile byte, but byte " << int{i} << " changed too";
  }
  EXPECT_EQ(normal.data[6], 0x06) << "0x06 is the normal profile";
  EXPECT_EQ(silent.data[6], 0x05) << "0x05 is the silent profile";
}

TEST(ProtoCommands, FavoriteHonoursSilentBySwitchingToTheExtendedForm) {
  // Captured pair from a Somfy hub pressing "My" on one RS100:
  //   silent off: `01 67 D8 00 00 00`            6-byte, no extended block
  //   silent on : `01 67 D8 00 80 D8 05 00`      8-byte, silent profile
  IoFrame normal{};
  ASSERT_TRUE(create_execute_command(normal, test::OWN_ID, test::DST_ID, true, CoverCommand::FAVORITE));
  EXPECT_EQ(normal.data_len, 6u) << "a normal My press is the short form";
  EXPECT_EQ(normal.data[2], POS_FAVORITE);

  IoFrame silent{};
  ASSERT_TRUE(create_execute_command(silent, test::OWN_ID, test::DST_ID, true, CoverCommand::FAVORITE,
                                     /*silent=*/true));
  ASSERT_EQ(silent.data_len, 8u) << "silent switches to the extended form";
  EXPECT_EQ(silent.data[2], POS_FAVORITE) << "still a My press";
  EXPECT_EQ(silent.data[4], 0x80);
  EXPECT_EQ(silent.data[5], POS_FAVORITE);
  EXPECT_EQ(silent.data[6], 0x05);
}

TEST(ProtoCommands, SilentDoesNotLeakIntoStopVentOrForceOpen) {
  // STOP has no travel speed. VENT is unobserved, and every extended frame captured so far has
  // byte 3 clear where VENT puts its modifier, so extending it would be a guess. Force-open is an
  // override — slowing it down would work against the point of it.
  for (auto cmd : {CoverCommand::STOP, CoverCommand::VENT}) {
    IoFrame f{};
    ASSERT_TRUE(create_execute_command(f, test::OWN_ID, test::DST_ID, true, cmd, /*silent=*/true));
    EXPECT_EQ(f.data_len, 6u) << "silent must not alter this command's payload";
  }

  IoFrame forced{};
  ASSERT_TRUE(create_force_open(forced, test::OWN_ID, test::DST_ID, true, 0));
  EXPECT_EQ(forced.data[6], 0x06) << "force-open stays on the normal travel profile";
}

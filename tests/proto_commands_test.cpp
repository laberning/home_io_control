#include "proto_commands.h"
#include "proto_frame.h"

#include "test_helpers.h"

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
  EXPECT_TRUE(is_start(frame)) << "challenge-req should be a start frame";
  EXPECT_FALSE(is_end(frame)) << "challenge-req should not be an end frame";
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
}

// ========================================================================================
// Execute command variants
// ========================================================================================

TEST(ProtoCommands, CreateExecutePositionZero) {
  IoFrame frame{};
  ASSERT_TRUE(create_execute(frame, test::OWN_ID, test::DST_ID, true, 0)) << "create_execute(0) should succeed";
  EXPECT_EQ(frame.cmd, CMD_EXECUTE) << "execute command should be CMD_EXECUTE (0x00)";
  EXPECT_EQ(frame.data_len, 8) << "execute position command should use 8-byte payload";
  EXPECT_EQ(frame.data[0], 0x01) << "execute payload byte 0 (origin) should be 0x01";
  EXPECT_EQ(frame.data[1], 0x67) << "execute payload byte 1 (ACEI) should be 0x67";
  EXPECT_EQ(frame.data[2], 0x00) << "execute position LSB should be 0x00 for position 0";
  EXPECT_EQ(frame.data[3], 0x00) << "execute position MSB should be 0x00 for position 0";
  EXPECT_TRUE(is_start(frame)) << "execute should be a start frame";
  EXPECT_FALSE(is_end(frame)) << "execute should not be an end frame";
}

TEST(ProtoCommands, CreateExecutePositionHundred) {
  IoFrame frame{};
  ASSERT_TRUE(create_execute(frame, test::OWN_ID, test::DST_ID, true, 100)) << "create_execute(100) should succeed";
  EXPECT_EQ(frame.data[2], 0xC8) << "position 100 doubles to 0x00C8 LSB";
  EXPECT_EQ(frame.data[3], 0x00) << "position 100 doubles to 0x00C8 MSB should be 0x00";
  EXPECT_EQ(frame.data_len, 8) << "execute position command should use 8-byte payload";
  EXPECT_TRUE(is_start(frame)) << "execute should be a start frame";
  EXPECT_FALSE(is_end(frame)) << "execute should not be an end frame";
}

TEST(ProtoCommands, CreateExecutePositionHalf) {
  IoFrame frame{};
  ASSERT_TRUE(create_execute(frame, test::OWN_ID, test::DST_ID, true, 50)) << "create_execute(50) should succeed";
  EXPECT_EQ(frame.data[2], 0x64) << "position 50 doubles to 0x0064 LSB";
  EXPECT_EQ(frame.data[3], 0x00) << "position 50 doubles to 0x0064 MSB should be 0x00";
  EXPECT_EQ(frame.data_len, 8) << "execute position command should use 8-byte payload";
  EXPECT_TRUE(is_start(frame)) << "execute should be a start frame";
  EXPECT_FALSE(is_end(frame)) << "execute should not be an end frame";
}

TEST(ProtoCommands, CreateExecuteFavoriteUsesSpecialPayload) {
  IoFrame frame{};
  ASSERT_TRUE(create_execute(frame, test::OWN_ID, test::DST_ID, true, POS_FAVORITE))
      << "create_execute(POS_FAVORITE) should succeed";
  EXPECT_EQ(frame.cmd, CMD_EXECUTE) << "favorite command should still use CMD_EXECUTE (0x00)";
  EXPECT_EQ(frame.data_len, 6) << "favorite command should use the short special payload";
  EXPECT_EQ(frame.data[0], 0x01) << "favorite payload byte 0 (origin) should be 0x01";
  EXPECT_EQ(frame.data[1], 0x67) << "favorite payload byte 1 (ACEI) should be 0x67";
  EXPECT_EQ(frame.data[2], POS_FAVORITE) << "favorite payload byte 2 should carry POS_FAVORITE";
  EXPECT_EQ(frame.data[3], 0x00);
  EXPECT_EQ(frame.data[4], 0x00);
  EXPECT_EQ(frame.data[5], 0x00);
  EXPECT_TRUE(is_start(frame)) << "favorite execute should be a start frame";
  EXPECT_FALSE(is_end(frame)) << "favorite execute should not be an end frame";
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
  EXPECT_EQ(frame.data[1], 0xE7) << "tilt ACEI should match observed tilt traffic";
  EXPECT_EQ(frame.data[2], POS_UNKNOWN) << "tilt execute should keep position unchanged via unknown marker";
  EXPECT_EQ(frame.data[3], 0x00);
  EXPECT_EQ(frame.data[4], STATUS_TILT_SELECTOR) << "tilt execute should set the tilt separator flag";
  EXPECT_EQ(frame.data[5], 0x96) << "25% open corresponds to 75% closed => 0x9600";
  EXPECT_EQ(frame.data[6], 0x00);
  EXPECT_EQ(frame.data[7], 0x00);
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
}

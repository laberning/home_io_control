/// @file proto_private_registers_test.cpp
/// @brief Byte-exact tests for the private register transport: header, read request, echo check.

#include "payload_frame.h"
#include "proto_private_registers.h"

#include "proto_constants.h"

#include <gtest/gtest.h>

#include <vector>

using namespace esphome::home_io_control;

TEST(ProtoPrivateRegisters, HeaderCarriesPrefixDirectionAndRegister) {
  uint8_t out[PRIVATE_REGISTER_PAYLOAD_MAX_SIZE] = {0};
  write_private_register_header(PRIVATE_REGISTER_DIRECTION_SET, 0x01, 0x36, out);
  EXPECT_EQ(std::vector<uint8_t>(out, out + PRIVATE_REGISTER_HEADER_SIZE),
            (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x36}));
}

TEST(ProtoPrivateRegisters, BuildsTheReadRequestForBothRegisterFamilies) {
  uint8_t out[PRIVATE_REGISTER_PAYLOAD_MAX_SIZE] = {0};
  ASSERT_EQ(encode_private_register_read(0x00, 0x2A, out), 4U);
  EXPECT_EQ(std::vector<uint8_t>(out, out + 4), (std::vector<uint8_t>{0x0C, 0x60, 0x00, 0x2A}));
  ASSERT_EQ(encode_private_register_read(0x01, 0x15, out), 4U);
  EXPECT_EQ(std::vector<uint8_t>(out, out + 4), (std::vector<uint8_t>{0x0C, 0x60, 0x01, 0x15}));
}

TEST(ProtoPrivateRegisters, NoRegisterNumberEverProducesTheWriteDirection) {
  // The probe's read-only guarantee: the direction byte is the GET constant for every one of the
  // 65536 register numbers, and the GET and SET constants really differ.
  static_assert(PRIVATE_REGISTER_DIRECTION_GET != PRIVATE_REGISTER_DIRECTION_SET);
  for (unsigned reg_hi = 0; reg_hi <= 0xFF; reg_hi++) {
    for (unsigned reg_lo = 0; reg_lo <= 0xFF; reg_lo++) {
      uint8_t out[PRIVATE_REGISTER_PAYLOAD_MAX_SIZE] = {0};
      ASSERT_EQ(encode_private_register_read(static_cast<uint8_t>(reg_hi), static_cast<uint8_t>(reg_lo), out), 4U);
      ASSERT_EQ(out[0], PRIVATE_REGISTER_PREFIX);
      ASSERT_EQ(out[1], PRIVATE_REGISTER_DIRECTION_GET);
      ASSERT_EQ(out[2], reg_hi);
      ASSERT_EQ(out[3], reg_lo);
    }
  }
}

TEST(ProtoPrivateRegisters, ARegisterReplyEchoesItsRegister) {
  EXPECT_TRUE(private_reply_echoes_register(
      test::make_payload_frame(CMD_WRITE_PRIVATE_ACK, {0x0C, 0x60, 0x01, 0x19, 'A'}), 0x01, 0x19));
  EXPECT_TRUE(private_reply_echoes_register(test::make_payload_frame(CMD_WRITE_PRIVATE_ACK, {0x0C, 0x60, 0x01, 0x36}),
                                            0x01, 0x36))
      << "empty is an echo";
  EXPECT_TRUE(private_reply_echoes_register(
      test::make_payload_frame(CMD_WRITE_PRIVATE_ACK, {0x0C, 0x61, 0x01, 0x36, 0x03}), 0x01, 0x36))
      << "a write's ack";
  EXPECT_FALSE(private_reply_echoes_register(
      test::make_payload_frame(CMD_WRITE_PRIVATE_ACK, {0x0C, 0x60, 0x01, 0x18, 0x41}), 0x01, 0x19));
  EXPECT_FALSE(private_reply_echoes_register(test::make_payload_frame(CMD_WRITE_PRIVATE_ACK, {0x0C, 0x60, 0x00, 0x19}),
                                             0x01, 0x19));
  EXPECT_FALSE(private_reply_echoes_register(test::make_payload_frame(CMD_WRITE_PRIVATE_ACK, {0x0C, 0x62, 0x01, 0x19}),
                                             0x01, 0x19));
  EXPECT_FALSE(
      private_reply_echoes_register(test::make_payload_frame(CMD_WRITE_PRIVATE_ACK, {0x0C, 0x60, 0x01}), 0x01, 0x19))
      << "truncated";
  EXPECT_FALSE(
      private_reply_echoes_register(test::make_payload_frame(CMD_WRITE_PRIVATE, {0x0C, 0x60, 0x01, 0x19}), 0x01, 0x19));
}

/// @file transfer_codec_test.cpp
/// @brief Decoders of the data-transport read (0x46/0x47/0x4A/0x4B): the open answer, the chunk
/// arithmetic and the block echo check, on the layout of a real VELUX KLR 200 copy session.

#include "proto_codecs.h"
#include "proto_constants.h"
#include "proto_frame.h"

#include <gtest/gtest.h>

#include <vector>

using namespace esphome::home_io_control;

namespace {

/// A frame with the given command and payload; endpoints are irrelevant to the decode.
IoFrame frame_with(uint8_t cmd, const std::vector<uint8_t> &payload) {
  IoFrame f{};
  init_frame(f, true, false, true, false);
  EXPECT_TRUE(set_cmd(f, cmd, payload.data(), static_cast<uint8_t>(payload.size())));
  return f;
}

}  // namespace

TEST(TransferCodec, DecodesAnAcceptedOpenAnswer) {
  // The copy session's product-name record: handle 0x10, flag 0, 600 bytes.
  TransferOpenReply reply;
  ASSERT_TRUE(decode_transfer_open_reply(frame_with(CMD_TRANSFER_OPEN_RESP, {0x10, 0x00, 0x02, 0x58}), reply));
  EXPECT_EQ(reply.handle, 0x10);
  EXPECT_EQ(reply.status, TRANSFER_STATUS_ACCEPTED);
  EXPECT_EQ(reply.length, 600);
  EXPECT_TRUE(reply.ok());
}

TEST(TransferCodec, AnAdaptedOpenIsOkAndAnythingElseIsAnError) {
  TransferOpenReply reply;
  ASSERT_TRUE(decode_transfer_open_reply(frame_with(CMD_TRANSFER_OPEN_RESP, {0x12, 0x01, 0x00, 0x10}), reply));
  EXPECT_TRUE(reply.ok());
  ASSERT_TRUE(decode_transfer_open_reply(frame_with(CMD_TRANSFER_OPEN_RESP, {0x12, 0x02, 0x00, 0x10}), reply));
  EXPECT_FALSE(reply.ok());
}

TEST(TransferCodec, RefusesAnOpenAnswerOfTheWrongShape) {
  TransferOpenReply reply;
  EXPECT_FALSE(decode_transfer_open_reply(frame_with(CMD_TRANSFER_OPEN_RESP, {0x10, 0x00, 0x02}), reply));
  EXPECT_FALSE(decode_transfer_open_reply(frame_with(CMD_TRANSFER_OPEN_RESP, {0x10, 0x00, 0x02, 0x58, 0x00}), reply));
  EXPECT_FALSE(decode_transfer_open_reply(frame_with(CMD_ERROR_RESP, {0x10, 0x00, 0x02, 0x58}), reply));
}

TEST(TransferCodec, ChunkCountRoundsUpToWholeBlocks) {
  EXPECT_EQ(transfer_chunk_count(0), 0);
  EXPECT_EQ(transfer_chunk_count(1), 1);
  EXPECT_EQ(transfer_chunk_count(18), 1);
  EXPECT_EQ(transfer_chunk_count(19), 2);
  EXPECT_EQ(transfer_chunk_count(600), 34);  // The copy session's 600-byte record took 34 chunks.
  EXPECT_EQ(transfer_chunk_count(80), 5);
}

TEST(TransferCodec, DecodesABlockThatEchoesTheRequest) {
  std::vector<uint8_t> payload = {0x10, 0x00, 0x07, 'K', 'i', 't'};
  const IoFrame f = frame_with(CMD_UNKNOWN4A_RESP, payload);
  const uint8_t *data = nullptr;
  uint8_t len = 0;
  ASSERT_TRUE(decode_transfer_block(f, 0x10, 7, data, len));
  ASSERT_EQ(len, 3);
  EXPECT_EQ(data[0], 'K');
  EXPECT_EQ(data[2], 't');
}

TEST(TransferCodec, TheCloseEchoCarriesNoData) {
  const IoFrame f = frame_with(CMD_UNKNOWN4A_RESP, {0x10, 0x00, 0x00});
  const uint8_t *data = nullptr;
  uint8_t len = 0xFF;
  ASSERT_TRUE(decode_transfer_block(f, 0x10, TRANSFER_CHUNK_CLOSE, data, len));
  EXPECT_EQ(len, 0);
}

TEST(TransferCodec, ABlockForAnotherChunkOrHandleIsRefused) {
  const IoFrame f = frame_with(CMD_UNKNOWN4A_RESP, {0x10, 0x00, 0x07, 0x41});
  const uint8_t *data = nullptr;
  uint8_t len = 0;
  EXPECT_FALSE(decode_transfer_block(f, 0x10, 8, data, len));       // late answer to another chunk
  EXPECT_FALSE(decode_transfer_block(f, 0x11, 7, data, len));       // another transfer
  EXPECT_FALSE(decode_transfer_block(f, 0x10, 0x0107, data, len));  // chunk high byte differs
}

TEST(TransferCodec, TheHandleIsComparedOnItsLowSevenBits) {
  // The vendor stack compares only the low seven bits of the first byte (E-KIZ-054).
  const IoFrame f = frame_with(CMD_UNKNOWN4A_RESP, {0x90, 0x00, 0x01, 0x41});
  const uint8_t *data = nullptr;
  uint8_t len = 0;
  EXPECT_TRUE(decode_transfer_block(f, 0x10, 1, data, len));
}

TEST(TransferCodec, ABlockLongerThanTheBlockSizeIsRefused) {
  std::vector<uint8_t> payload = {0x10, 0x00, 0x01};
  payload.resize(3 + TRANSFER_BLOCK_SIZE + 1, 0x00);
  const IoFrame f = frame_with(CMD_UNKNOWN4A_RESP, payload);
  const uint8_t *data = nullptr;
  uint8_t len = 0;
  EXPECT_FALSE(decode_transfer_block(f, 0x10, 1, data, len));
}

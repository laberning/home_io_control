/// @file corpus_transfer_open_test.cpp
/// @brief The data-transport open reply (CMD_TRANSFER_OPEN_RESP, 0x47), checked against the corpus.
///
/// Every captured 0x47 must decode with decode_transfer_open_reply(), and the RS100 Solar io's answer
/// to the open of its class-2 object 0xA601 (issue #156) must give the handle, status and length the
/// hub then read the object with: 366 bytes, which the block rule covers in 21 chunks.

#include "corpus_test_helpers.h"
#include "proto_codecs.h"
#include "proto_constants.h"
#include "proto_frame.h"

#include <gtest/gtest.h>

#include <string>

using namespace esphome::home_io_control;

namespace {

/// The RS100's open reply for object 0xA601.
constexpr const char *A601_OPEN_CAPTURE = "somfy_rs100_probe_read_object_a601_sx1262";
/// Handle, length and chunk count of that reply, as the hub logged the transfer.
constexpr uint8_t A601_HANDLE = 0x10;
constexpr uint16_t A601_LENGTH = 366;
constexpr uint16_t A601_CHUNKS = 21;  ///< ceil(366 / TRANSFER_BLOCK_SIZE).

}  // namespace

TEST(CorpusTransferOpen, EveryCaptured0x47Decodes) {
  int open_replies = 0;
  for (const corpus::CorpusCapture *capture : corpus_test::all_captures()) {
    for (uint8_t i = 0; i < capture->frame_count; i++) {
      const IoFrame frame = corpus_test::parse_capture_frame(capture->frames[i]);
      if (frame.cmd != CMD_TRANSFER_OPEN_RESP)
        continue;
      SCOPED_TRACE(std::string(capture->id) + " frame " + std::to_string(i));
      open_replies++;
      TransferOpenReply reply;
      EXPECT_TRUE(decode_transfer_open_reply(frame, reply));
    }
  }
  EXPECT_GE(open_replies, 1);
}

TEST(CorpusTransferOpen, Rs100ObjectA601OpenGivesTheLengthTheHubRead) {
  const corpus::CorpusCapture *capture = corpus_test::capture_by_id(A601_OPEN_CAPTURE);
  ASSERT_NE(capture, nullptr);
  const IoFrame frame = corpus_test::parse_capture_frame(capture->frames[0]);
  TransferOpenReply reply;
  ASSERT_TRUE(decode_transfer_open_reply(frame, reply));
  EXPECT_EQ(reply.handle, A601_HANDLE);
  EXPECT_EQ(reply.status, TRANSFER_STATUS_ACCEPTED);
  EXPECT_TRUE(reply.ok());
  EXPECT_EQ(reply.length, A601_LENGTH);
  EXPECT_EQ(transfer_chunk_count(reply.length), A601_CHUNKS);
}

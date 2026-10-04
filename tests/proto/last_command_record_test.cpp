/// @file last_command_record_test.cpp
/// @brief Tests for the last-command record decoder (proto_codecs.h).

#include "proto_codecs.h"
#include "proto_frame.h"

#include <gtest/gtest.h>

#include <cstring>

using namespace esphome::home_io_control;

// ============================================================================
// LastCommandRecord test suite
// ============================================================================
// decode_last_command_record() against both status-bearing payload layouts: offsets, length
// guard (both sides of the boundary), and the all-zero-commander rejection. Includes only protocol
// headers, so it also proves proto_codecs.h is self-contained.

TEST(LastCommandRecord, DecodeReadsPrivateResponseOffsets) {
  IoFrame f{};
  const uint8_t payload[14] = {0x05, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xBE, 0xFE, 0xDB, 0x01, 0x00, 0x00};
  std::memcpy(f.data, payload, sizeof(payload));
  f.data_len = sizeof(payload);

  const auto record = decode_last_command_record(f, PRIVATE_RESPONSE_LAST_COMMAND_OFFSET);

  ASSERT_TRUE(record.valid);
  EXPECT_EQ(std::memcmp(record.commander, "\xBE\xFE\xDB", 3), 0);
  EXPECT_EQ(record.originator, 0x01);
}

TEST(LastCommandRecord, DecodeReadsStatusUpdateOffsets) {
  // Real capture bytes: tests/corpus/captures/statuspoll/somfy_rs100_statuspoll_kig300_sx1276.yaml
  // frame 2 (0x71) — proves the +3 shift relative to 0x04, not a synthetic guess.
  IoFrame f{};
  const uint8_t payload[16] = {0x04, 0x60, 0x10, 0x0A, 0x0B, 0x00, 0x00, 0xAC,
                               0x9E, 0x00, 0x0F, 0xBE, 0xFE, 0xDB, 0x01, 0x00};
  std::memcpy(f.data, payload, sizeof(payload));
  f.data_len = sizeof(payload);

  const auto record = decode_last_command_record(f, STATUS_UPDATE_LAST_COMMAND_OFFSET);

  ASSERT_TRUE(record.valid);
  EXPECT_EQ(std::memcmp(record.commander, "\xBE\xFE\xDB", 3), 0);
  EXPECT_EQ(record.originator, 0x01);
}

TEST(LastCommandRecord, DecodeRejectsShortPrivateResponse) {
  IoFrame f{};
  const uint8_t payload[6] = {0x2C, 0x80, 0x00, 0x00, 0x00, 0x00};
  std::memcpy(f.data, payload, sizeof(payload));
  f.data_len = sizeof(payload);

  EXPECT_FALSE(decode_last_command_record(f, PRIVATE_RESPONSE_LAST_COMMAND_OFFSET).valid);
}

TEST(LastCommandRecord, DecodeRejectsShortStatusUpdate) {
  // 14 bytes: one short of the 15 the record needs at base 11 (3-byte commander + 1-byte
  // originator). A nonzero commander is set so this pins the length guard specifically, not the
  // separate all-zero-commander guard (DecodeRejectsAllZeroCommander below).
  IoFrame f{};
  const uint8_t payload[14] = {0x04, 0x60, 0x10, 0x0A, 0x0B, 0x00, 0x00, 0xAC, 0x9E, 0x00, 0x0F, 0xBE, 0xFE, 0xDB};
  std::memcpy(f.data, payload, sizeof(payload));
  f.data_len = sizeof(payload);

  EXPECT_FALSE(decode_last_command_record(f, STATUS_UPDATE_LAST_COMMAND_OFFSET).valid);
}

TEST(LastCommandRecord, DecodeAcceptsAPayloadEndingExactlyAtTheRecord) {
  // 15 bytes: exactly base 11 + 3-byte commander + 1-byte originator, the shortest 0x71 payload
  // that carries the whole record. Pins the length guard's off-by-one from the accepting side.
  IoFrame f{};
  const uint8_t payload[15] = {0x04, 0x60, 0x10, 0x0A, 0x0B, 0x00, 0x00, 0xAC,
                               0x9E, 0x00, 0x0F, 0xBE, 0xFE, 0xDB, 0x01};
  std::memcpy(f.data, payload, sizeof(payload));
  f.data_len = sizeof(payload);

  const auto record = decode_last_command_record(f, STATUS_UPDATE_LAST_COMMAND_OFFSET);

  ASSERT_TRUE(record.valid);
  EXPECT_EQ(record.originator, 0x01);
}

TEST(LastCommandRecord, DecodeRejectsAllZeroCommander) {
  IoFrame f{};
  const uint8_t payload[14] = {0x05, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00};
  std::memcpy(f.data, payload, sizeof(payload));
  f.data_len = sizeof(payload);

  EXPECT_FALSE(decode_last_command_record(f, PRIVATE_RESPONSE_LAST_COMMAND_OFFSET).valid)
      << "00 00 00 is not a node ID any observed controller uses -- a device padding this field "
         "must not publish a fabricated address";
}

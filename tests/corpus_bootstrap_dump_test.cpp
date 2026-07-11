/// @file corpus_bootstrap_dump_test.cpp
/// @brief DISABLED_ dump utility that prints wire bytes for the corpus bootstrap captures.
///
/// Never runs in CI or as part of `make unit-test` (DISABLED_ prefix, per gtest convention).
/// Run explicitly to (re)generate the bytes hand-transcribed into
/// tests/corpus/captures/_bootstrap/*.yaml — see tests/corpus/README.md:
///
///   ./build/test_home_io_control --gtest_also_run_disabled_tests --gtest_filter='*BootstrapDump*'
///
/// Building the bootstrap fixtures from real serialize()/crc_ccitt()/create_challenge_resp()
/// output (instead of hand-typed hex) guarantees the captures are honest wire bytes, not
/// guesses about frame layout.

#include "proto_commands.h"
#include "proto_constants.h"
#include "proto_crypto.h"
#include "proto_frame.h"

#include "test_helpers.h"

#include <gtest/gtest.h>

#include <cstdio>

using namespace esphome::home_io_control;

namespace {

void print_wire(const char *label, const IoFrame &frame) {
  uint8_t wire[FRAME_MAX_SIZE + 2] = {0};
  const uint8_t len = serialize(frame, wire, FRAME_MAX_SIZE);
  const uint16_t crc = crc_ccitt(wire, len);
  wire[len] = crc & 0xFF;
  wire[len + 1] = (crc >> 8) & 0xFF;
  printf("%s hex:", label);
  for (uint8_t i = 0; i < len + 2; i++)
    printf(" %02X", wire[i]);
  printf("\n");
}

}  // namespace

TEST(BootstrapDump, DISABLED_PrintSyntheticFrames) {
  // --- synthetic_1w_close: typed broadcast (all roller-shutter devices) CLOSE ---
  IoFrame oneway{};
  init_frame(oneway, /*is_2w=*/false, /*start=*/true, /*end=*/true, /*low_power=*/false);
  const uint8_t remote_id[NODE_ID_SIZE] = {0xAA, 0xBB, 0xCC};
  const uint8_t broadcast_roller[NODE_ID_SIZE] = {0x00, 0x00, 0xBF};  // type=ROLLER_SHUTTER, suffix=broadcast
  set_dst(oneway, broadcast_roller);
  set_src(oneway, remote_id);
  // originator=USER_REMOTE, acei level=2 (valid bit set), main0=0xC8 (percent 100 * 2 = CLOSE), main1=0 (no modifier)
  const uint8_t oneway_payload[4] = {ORIGINATOR_USER_REMOTE, 0x41, 0xC8, 0x00};
  ASSERT_TRUE(set_cmd(oneway, CMD_EXECUTE, oneway_payload, sizeof(oneway_payload)));
  print_wire("synthetic_1w_close", oneway);

  OneWayFrameInfo info = decode_1w_frame(oneway);
  printf("synthetic_1w_close decoded: address_class=%d target_type=%d intent=%s originator=%u acei_level=%u\n",
         static_cast<int>(info.address_class), static_cast<int>(info.target_type), info.intent, info.originator,
         info.acei_level);

  // --- synthetic_auth_exchange: execute(50) -> challenge -> challenge-resp -> status(END) ---
  IoFrame exec = test::make_execute(50);
  print_wire("synthetic_auth_exchange exec", exec);

  IoFrame challenge{};
  init_frame(challenge, /*is_2w=*/true, /*start=*/false, /*end=*/false, /*low_power=*/false);
  set_dst(challenge, test::OWN_ID);
  set_src(challenge, test::DST_ID);
  ASSERT_TRUE(set_cmd(challenge, CMD_CHALLENGE_REQ, test::TEST_CHALLENGE, HMAC_SIZE));
  print_wire("synthetic_auth_exchange challenge", challenge);

  // create_challenge_resp(f, dst, src, ...) — dst=device (the exec request's own dst), src=our
  // own id, matching how ExchangeEngine builds it (exchange_engine.cpp: request.dst, node_id_).
  IoFrame resp{};
  ASSERT_TRUE(
      create_challenge_resp(resp, test::DST_ID, test::OWN_ID, test::TEST_CHALLENGE, exec, test::TEST_SYSTEM_KEY));
  print_wire("synthetic_auth_exchange challenge_resp", resp);

  IoFrame status{};
  init_frame(status, /*is_2w=*/true, /*start=*/false, /*end=*/true, /*low_power=*/false);
  set_dst(status, test::OWN_ID);
  set_src(status, test::DST_ID);
  // STATUS_STOPPED, originator=LOCAL_USER, target MSB/LSB=0x64/0x00 (50%), current MSB/LSB=0x64/0x00, hint=0x00
  const uint8_t status_payload[8] = {STATUS_STOPPED, 0x00, 0x64, 0x00, 0x64, 0x00, 0x00, 0x00};
  ASSERT_TRUE(set_cmd(status, CMD_PRIVATE_RESP, status_payload, sizeof(status_payload)));
  print_wire("synthetic_auth_exchange status", status);
}

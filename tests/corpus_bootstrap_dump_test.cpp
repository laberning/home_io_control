/// @file corpus_bootstrap_dump_test.cpp
/// @brief DISABLED_ dump utility that prints wire bytes for the corpus bootstrap captures.
///
/// Never runs in CI or as part of `make unit-test` (DISABLED_ prefix, per gtest convention).
/// Run explicitly to (re)generate the bytes hand-transcribed into
/// the synthetic bootstrap captures (tests/corpus/captures/*/synthetic_*.yaml:
/// oneway/synthetic_oneway_close, exchange/synthetic_exchange_auth, identify/synthetic_identify)
/// — see tests/corpus/README.md:
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
  // Index is size_t, not uint8_t: `len + 2` promotes to int, and a uint8_t counter can never
  // reach it once len exceeds 253 — the comparison would stay true forever. FRAME_MAX_SIZE
  // caps len at 32 today, so this was latent rather than live, but the loop shouldn't depend
  // on that constant staying small.
  const size_t wire_len = static_cast<size_t>(len) + 2;
  for (size_t i = 0; i < wire_len; i++)
    printf(" %02X", wire[i]);
  printf("\n");
}

void print_bytes(const uint8_t *data, uint8_t len) {
  for (uint8_t i = 0; i < len; i++)
    printf("%02X ", data[i]);
}

/// Print one create_hmac() cross-language known-answer vector: data / challenge / key -> hmac.
/// Consumed by scripts/corpus/tests/data/crypto_kat.yaml (Step 5) and pinned as a hardcoded
/// C++ vector alongside it — the two must never be regenerated independently of each other.
void print_hmac_kat(const char *label, const uint8_t *data, uint8_t data_len, const uint8_t challenge[HMAC_SIZE],
                    const uint8_t key[AES_KEY_SIZE]) {
  uint8_t hmac[HMAC_SIZE] = {0};
  ASSERT_TRUE(crypto::create_hmac(data, data_len, challenge, key, hmac));
  printf("%s data:", label);
  print_bytes(data, data_len);
  printf("challenge:");
  print_bytes(challenge, HMAC_SIZE);
  printf("hmac:");
  print_bytes(hmac, HMAC_SIZE);
  printf("\n");
}

}  // namespace

TEST(BootstrapDump, DISABLED_PrintSyntheticFrames) {
  // --- synthetic_oneway_close: typed broadcast (all roller-shutter devices) CLOSE ---
  IoFrame oneway{};
  init_frame(oneway, /*is_2w=*/false, /*start=*/true, /*end=*/true, /*low_power=*/false);
  const uint8_t remote_id[NODE_ID_SIZE] = {0xAA, 0xBB, 0xCC};
  const uint8_t broadcast_roller[NODE_ID_SIZE] = {0x00, 0x00, 0xBF};  // type=ROLLER_SHUTTER, suffix=broadcast
  set_dst(oneway, broadcast_roller);
  set_src(oneway, remote_id);
  // originator=USER_REMOTE, acei level=2 (valid bit set), main0=0xC8 (percent 100 * 2 = CLOSE), main1=0 (no modifier)
  const uint8_t oneway_payload[4] = {ORIGINATOR_USER_REMOTE, 0x41, 0xC8, 0x00};
  ASSERT_TRUE(set_cmd(oneway, CMD_EXECUTE, oneway_payload, sizeof(oneway_payload)));
  print_wire("synthetic_oneway_close", oneway);

  OneWayFrameInfo info = decode_1w_frame(oneway);
  printf("synthetic_oneway_close decoded: address_class=%d target_type=%d intent=%s originator=%u acei_level=%u\n",
         static_cast<int>(info.address_class), static_cast<int>(info.target_type), info.intent, info.originator,
         info.acei_level);

  // --- synthetic_exchange_auth: execute(50) -> challenge -> challenge-resp -> status(END) ---
  IoFrame exec = test::make_execute(50);
  print_wire("synthetic_exchange_auth exec", exec);

  IoFrame challenge{};
  init_frame(challenge, /*is_2w=*/true, /*start=*/false, /*end=*/false, /*low_power=*/false);
  set_dst(challenge, test::OWN_ID);
  set_src(challenge, test::DST_ID);
  ASSERT_TRUE(set_cmd(challenge, CMD_CHALLENGE_REQ, test::TEST_CHALLENGE, HMAC_SIZE));
  print_wire("synthetic_exchange_auth challenge", challenge);

  // create_challenge_resp(f, dst, src, ...) — dst=device (the exec request's own dst), src=our
  // own id, matching how ExchangeEngine builds it (exchange_engine.cpp: request.dst, node_id_).
  IoFrame resp{};
  ASSERT_TRUE(
      create_challenge_resp(resp, test::DST_ID, test::OWN_ID, test::TEST_CHALLENGE, exec, test::TEST_SYSTEM_KEY));
  print_wire("synthetic_exchange_auth challenge_resp", resp);

  IoFrame status{};
  init_frame(status, /*is_2w=*/true, /*start=*/false, /*end=*/true, /*low_power=*/false);
  set_dst(status, test::OWN_ID);
  set_src(status, test::DST_ID);
  // STATUS_STOPPED, originator=LOCAL_USER, target MSB/LSB=0x64/0x00 (50%), current MSB/LSB=0x64/0x00, hint=0x00
  const uint8_t status_payload[8] = {STATUS_STOPPED, 0x00, 0x64, 0x00, 0x64, 0x00, 0x00, 0x00};
  ASSERT_TRUE(set_cmd(status, CMD_PRIVATE_RESP, status_payload, sizeof(status_payload)));
  print_wire("synthetic_exchange_auth status", status);

  // --- synthetic_identify: identify(0x1E) -> challenge -> challenge-resp -> ack(END) ---
  IoFrame identify{};
  ASSERT_TRUE(create_identify(identify, test::OWN_ID, test::DST_ID, /*low_power=*/true));
  print_wire("synthetic_identify identify", identify);

  IoFrame identify_challenge{};
  init_frame(identify_challenge, /*is_2w=*/true, /*start=*/false, /*end=*/false, /*low_power=*/false);
  set_dst(identify_challenge, test::OWN_ID);
  set_src(identify_challenge, test::DST_ID);
  ASSERT_TRUE(set_cmd(identify_challenge, CMD_CHALLENGE_REQ, test::TEST_CHALLENGE, HMAC_SIZE));
  print_wire("synthetic_identify challenge", identify_challenge);

  // create_challenge_resp(f, dst, src, ...) — dst=device (the identify request's own dst), src=our
  // own id, matching how ExchangeEngine builds it (exchange_engine.cpp: request.dst, node_id_).
  IoFrame identify_resp{};
  ASSERT_TRUE(create_challenge_resp(identify_resp, test::DST_ID, test::OWN_ID, test::TEST_CHALLENGE, identify,
                                    test::TEST_SYSTEM_KEY));
  print_wire("synthetic_identify challenge_resp", identify_resp);

  // Final ack: identify_device() treats any endpoint-matched, non-error reply as success (there is
  // no dedicated CMD_IDENTIFY response), so the device echoing CMD_IDENTIFY back is a plausible
  // and minimal stand-in.
  IoFrame identify_ack{};
  init_frame(identify_ack, /*is_2w=*/true, /*start=*/false, /*end=*/true, /*low_power=*/false);
  set_dst(identify_ack, test::OWN_ID);
  set_src(identify_ack, test::DST_ID);
  ASSERT_TRUE(set_cmd(identify_ack, CMD_IDENTIFY, nullptr, 0));
  print_wire("synthetic_identify ack", identify_ack);
}

/// Prints cross-language create_hmac() KAT vectors for scripts/corpus/tests/data/crypto_kat.yaml
/// and the hardcoded pinned vectors in tests/corpus_crypto_test.cpp (Step 5 of the corpus
/// implementation plan). Run explicitly and copy the printed lines by hand — this is not part of
/// any assertion, it's a generator:
///   ./build/test_home_io_control --gtest_also_run_disabled_tests --gtest_filter='*CryptoKat*'
TEST(BootstrapDump, DISABLED_PrintCryptoKatVectors) {
  // Vector 1: single-byte data (shorter than the 8-byte IV prefix, exercises 0x55 padding).
  {
    const uint8_t data[1] = {0x00};
    print_hmac_kat("kat1", data, sizeof(data), test::TEST_CHALLENGE, test::TEST_SYSTEM_KEY);
  }
  // Vector 2: exactly 8 bytes of data (fills the IV prefix with no padding).
  {
    const uint8_t data[8] = {0x00, 0x01, 0x43, 0x64, 0x00, 0x80, 0xD8, 0x06};
    print_hmac_kat("kat2", data, sizeof(data), test::TEST_CHALLENGE, test::TEST_SYSTEM_KEY);
  }
  // Vector 3: 14 bytes of data (longer than the 8-byte IV prefix; the extra bytes only feed the
  // running checksum, not the IV bytes directly — exercises that path independently of vector 2).
  {
    const uint8_t data[14] = {0x20, 0x02, 0x03, 0x05, 0x04, 0x00, 0x08, 0xAF, 0xC0, 0xB8, 0x12, 0x1F, 0x73, 0xA2};
    print_hmac_kat("kat3", data, sizeof(data), test::TEST_CHALLENGE, test::TEST_SYSTEM_KEY);
  }
}

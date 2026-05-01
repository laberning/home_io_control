#include "hub_decisions.h"
#include "proto_commands.h"
#include "proto_crypto.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace esphome::home_io_control;

namespace {

[[noreturn]] void fail(const char *message) {
  std::fprintf(stderr, "protocol_smoke failed: %s\n", message);
  std::exit(1);
}

void expect_true(bool condition, const char *message) {
  if (!condition)
    fail(message);
}

void expect_equal_u8(uint8_t actual, uint8_t expected, const char *message) {
  if (actual != expected)
    fail(message);
}

void expect_equal_str(const char *actual, const char *expected, const char *message) {
  if (std::strcmp(actual, expected) != 0)
    fail(message);
}

void expect_mem_eq(const uint8_t *actual, const uint8_t *expected, size_t len, const char *message) {
  if (std::memcmp(actual, expected, len) != 0)
    fail(message);
}

// Keep frame fabrication tiny and explicit so the decision tests below read like protocol cases
// rather than serializer/parser tests.
IoFrame make_frame(const uint8_t src[NODE_ID_SIZE], const uint8_t dst[NODE_ID_SIZE], uint8_t cmd,
                   uint8_t data_len = 0) {
  IoFrame frame{};
  init_frame(frame, true, false, false, false);
  set_src(frame, src);
  set_dst(frame, dst);
  frame.cmd = cmd;
  frame.data_len = data_len;
  return frame;
}

void test_execute_round_trip() {
  const uint8_t own[NODE_ID_SIZE] = {0xC0, 0xFF, 0xEE};
  const uint8_t dst[NODE_ID_SIZE] = {0x9C, 0xA3, 0x9C};

  IoFrame frame{};
  expect_true(create_execute(frame, own, dst, true, 100), "create_execute should succeed");
  expect_true(is_start(frame), "execute frame should be marked start");
  expect_true(!is_end(frame), "execute frame should remain open for the authenticated response path");
  expect_equal_u8(frame.cmd, CMD_EXECUTE, "execute frame command should match");
  expect_equal_u8(frame.data_len, 8, "execute frame should use 8-byte payload for position commands");

  uint8_t serialized[FRAME_MAX_SIZE] = {0};
  uint8_t serialized_len = serialize(frame, serialized, sizeof(serialized));
  expect_true(serialized_len > 0, "execute frame should serialize");

  IoFrame parsed{};
  expect_true(parse(serialized, serialized_len, parsed), "serialized execute frame should parse");
  expect_equal_u8(parsed.cmd, CMD_EXECUTE, "parsed execute frame command should match");
  expect_equal_u8(parsed.data_len, 8, "parsed execute payload length should match");
  expect_mem_eq(parsed.src, own, NODE_ID_SIZE, "parsed execute src should match");
  expect_mem_eq(parsed.dst, dst, NODE_ID_SIZE, "parsed execute dst should match");
}

void test_status_request_round_trip() {
  const uint8_t own[NODE_ID_SIZE] = {0xC0, 0xFF, 0xEE};
  const uint8_t dst[NODE_ID_SIZE] = {0x9C, 0xA3, 0x9C};

  IoFrame frame{};
  expect_true(create_get_status(frame, own, dst), "create_get_status should succeed");
  expect_equal_u8(frame.cmd, CMD_PRIVATE, "status request command should match");
  expect_equal_u8(frame.data_len, 3, "status request payload length should match");

  uint8_t serialized[FRAME_MAX_SIZE] = {0};
  uint8_t serialized_len = serialize(frame, serialized, sizeof(serialized));
  expect_true(serialized_len > 0, "status request should serialize");

  IoFrame parsed{};
  expect_true(parse(serialized, serialized_len, parsed), "serialized status request should parse");
  expect_equal_u8(parsed.cmd, CMD_PRIVATE, "parsed status request command should match");
}

void test_challenge_response_hmac() {
  const uint8_t own[NODE_ID_SIZE] = {0xC0, 0xFF, 0xEE};
  const uint8_t dst[NODE_ID_SIZE] = {0x9C, 0xA3, 0x9C};
  const uint8_t key[AES_KEY_SIZE] = {0xD1, 0x74, 0x34, 0x93, 0xFA, 0x94, 0x38, 0x45,
                                     0xAC, 0x43, 0x50, 0xEE, 0xFF, 0x34, 0x29, 0x34};
  const uint8_t challenge[HMAC_SIZE] = {0x08, 0xD1, 0xCA, 0x5F, 0xFD, 0x64};

  IoFrame origin{};
  expect_true(create_execute(origin, own, dst, true, 0), "origin execute should be created");

  IoFrame auth_resp{};
  expect_true(create_challenge_resp(auth_resp, dst, own, challenge, origin, key),
              "challenge response should be created");
  expect_equal_u8(auth_resp.cmd, CMD_CHALLENGE_RESP, "challenge response command should match");
  expect_equal_u8(auth_resp.data_len, HMAC_SIZE, "challenge response HMAC length should match");

  uint8_t transcript[FRAME_MAX_SIZE] = {0};
  transcript[0] = origin.cmd;
  std::memcpy(transcript + 1, origin.data, origin.data_len);
  expect_true(crypto::verify_hmac(transcript, origin.data_len + 1, auth_resp.data, challenge, key),
              "challenge response HMAC should verify against the origin transcript");

  uint8_t tampered_hmac[HMAC_SIZE] = {0};
  std::memcpy(tampered_hmac, auth_resp.data, HMAC_SIZE);
  tampered_hmac[0] ^= 0x01;
  expect_true(!crypto::verify_hmac(transcript, origin.data_len + 1, tampered_hmac, challenge, key),
              "tampered challenge response HMAC should fail verification");
}

void test_crypt_key_round_trip() {
  const uint8_t challenge[HMAC_SIZE] = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC};
  const uint8_t plaintext[AES_KEY_SIZE] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                           0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  uint8_t encrypted[AES_KEY_SIZE] = {0};
  uint8_t decrypted[AES_KEY_SIZE] = {0};
  uint8_t iv_seed[1] = {CMD_KEY_INIT};

  expect_true(crypto::crypt_key(iv_seed, sizeof(iv_seed), challenge, plaintext, encrypted),
              "key encryption should succeed");
  expect_true(crypto::crypt_key(iv_seed, sizeof(iv_seed), challenge, encrypted, decrypted),
              "key decryption should succeed");
  expect_mem_eq(decrypted, plaintext, AES_KEY_SIZE, "crypt_key should round-trip");
}

void test_key_transfer_round_trip() {
  const uint8_t own[NODE_ID_SIZE] = {0xC0, 0xFF, 0xEE};
  const uint8_t dst[NODE_ID_SIZE] = {0x9C, 0xA3, 0x9C};
  const uint8_t key[AES_KEY_SIZE] = {0xD1, 0x74, 0x34, 0x93, 0xFA, 0x94, 0x38, 0x45,
                                     0xAC, 0x43, 0x50, 0xEE, 0xFF, 0x34, 0x29, 0x34};
  const uint8_t challenge[HMAC_SIZE] = {0x08, 0xD1, 0xCA, 0x5F, 0xFD, 0x64};

  IoFrame key_init{};
  expect_true(create_key_init(key_init, own, dst), "key init should be created");

  IoFrame key_transfer{};
  expect_true(create_key_transfer(key_transfer, key_init, dst, own, key, challenge), "key transfer should be created");
  expect_equal_u8(key_transfer.cmd, CMD_KEY_TRANSFER, "key transfer command should match");
  expect_equal_u8(key_transfer.data_len, AES_KEY_SIZE, "key transfer payload should match AES key size");

  uint8_t decrypted[AES_KEY_SIZE] = {0};
  uint8_t iv_seed[1] = {key_init.cmd};
  expect_true(crypto::crypt_key(iv_seed, sizeof(iv_seed), challenge, key_transfer.data, decrypted),
              "key transfer payload should decrypt with the same IV seed");
  expect_mem_eq(decrypted, key, AES_KEY_SIZE, "key transfer payload should round-trip to the original system key");
}

void test_device_profiles() {
  expect_true(device_supports_position_control(DeviceType::AWNING), "awning should support position control");
  expect_true(device_supports_binary_control(DeviceType::LIGHT), "light should support binary control");
  expect_true(device_supports_binary_control(DeviceType::ON_OFF_SWITCH), "switch should support binary control");
  expect_true(!device_supports_position_control(DeviceType::LIGHT), "light should not report position control");
  expect_equal_str(device_operation_profile_name(DeviceType::AWNING), "cover_position", "awning profile should match");
  expect_equal_str(device_operation_profile_name(DeviceType::LIGHT), "binary_on_off", "light profile should match");
  expect_equal_str(device_operation_profile_name(DeviceType::UNKNOWN), "unknown", "unknown profile should match");
}

void test_parse_rejects_length_mismatch() {
  const uint8_t own[NODE_ID_SIZE] = {0xC0, 0xFF, 0xEE};
  const uint8_t dst[NODE_ID_SIZE] = {0x9C, 0xA3, 0x9C};

  IoFrame frame{};
  expect_true(create_get_status(frame, own, dst), "status request should be created for malformed-length test");

  uint8_t serialized[FRAME_MAX_SIZE] = {0};
  uint8_t serialized_len = serialize(frame, serialized, sizeof(serialized));
  expect_true(serialized_len > 0, "status request should serialize for malformed-length test");

  serialized[0] =
      static_cast<uint8_t>((serialized[0] & ~CTRL0_LENGTH_MASK) | ((serialized_len - 2) & CTRL0_LENGTH_MASK));

  IoFrame parsed{};
  expect_true(!parse(serialized, serialized_len, parsed), "parse should reject mismatched CTRL0 length metadata");
}

void test_parse_rejects_null_and_truncated_inputs() {
  IoFrame parsed{};
  expect_true(!parse(nullptr, 0, parsed), "parse should reject null input");
  expect_true(!parse(reinterpret_cast<const uint8_t *>(""), 0, parsed), "parse should reject zero-length input");

  const uint8_t own[NODE_ID_SIZE] = {0xC0, 0xFF, 0xEE};
  const uint8_t dst[NODE_ID_SIZE] = {0x9C, 0xA3, 0x9C};
  IoFrame frame{};
  expect_true(create_get_status(frame, own, dst), "status request should be created for truncation test");

  uint8_t serialized[FRAME_MAX_SIZE] = {0};
  uint8_t serialized_len = serialize(frame, serialized, sizeof(serialized));
  expect_true(serialized_len > 0, "status request should serialize for truncation test");
  expect_true(!parse(serialized, serialized_len - 1, parsed), "parse should reject truncated input");
}

void test_max_frame_round_trip() {
  IoFrame frame{};
  init_frame(frame, true, true, true, false);
  const uint8_t own[NODE_ID_SIZE] = {0xC0, 0xFF, 0xEE};
  const uint8_t dst[NODE_ID_SIZE] = {0x9C, 0xA3, 0x9C};
  set_src(frame, own);
  set_dst(frame, dst);
  uint8_t payload[FRAME_MAX_DATA_SIZE] = {0};
  for (uint8_t i = 0; i < FRAME_MAX_DATA_SIZE; i++)
    payload[i] = i;
  expect_true(set_cmd(frame, CMD_PRIVATE_RESP, payload, sizeof(payload)), "max frame should be constructible");

  uint8_t serialized[FRAME_MAX_SIZE] = {0};
  const uint8_t serialized_len = serialize(frame, serialized, sizeof(serialized));
  expect_equal_u8(serialized_len, FRAME_MAX_SIZE, "max frame should serialize to full frame size");

  IoFrame parsed{};
  expect_true(parse(serialized, serialized_len, parsed), "max frame should parse");
  expect_equal_u8(parsed.data_len, FRAME_MAX_DATA_SIZE, "max frame payload length should round-trip");
}

void test_serialize_rejects_inconsistent_metadata() {
  IoFrame frame{};
  init_frame(frame, true, true, false, false);
  frame.cmd = CMD_PRIVATE;
  frame.data_len = 3;
  frame.ctrl0 =
      static_cast<uint8_t>((frame.ctrl0 & ~CTRL0_LENGTH_MASK) | ((FRAME_MIN_SIZE + 1 - 1) & CTRL0_LENGTH_MASK));

  uint8_t serialized[FRAME_MAX_SIZE] = {0};
  expect_equal_u8(serialize(frame, serialized, sizeof(serialized)), 0,
                  "serialize should reject inconsistent CTRL0/data_len metadata");
}

void test_hex_to_bytes_rejects_invalid_input() {
  uint8_t out[NODE_ID_SIZE] = {0xAA, 0xBB, 0xCC};
  expect_true(!hex_to_bytes("C0FFE", out, NODE_ID_SIZE), "odd-length hex should be rejected");
  expect_true(out[0] == 0 && out[1] == 0 && out[2] == 0, "failed hex decode should zero the output buffer");
  expect_true(!hex_to_bytes("GGGGGG", out, NODE_ID_SIZE), "non-hex characters should be rejected");
}

void test_status_position_decoding() {
  float target = UNKNOWN_POSITION;
  float position = UNKNOWN_POSITION;

  decode_position_report(STATUS_POS_MAX, STATUS_POS_MAX / 2, false, target, position);
  expect_true(target == 100.0f, "max target raw value should decode to 100 percent");
  expect_true(position == 50.0f, "half current raw value should decode to 50 percent");

  decode_position_report(STATUS_POS_MAX / 4, STATUS_POS_MAX + 1, true, target, position);
  expect_true(target == 25.0f, "quarter target raw value should decode to 25 percent");
  expect_true(position == 25.0f, "stopped device with invalid current raw value should fall back to target");

  decode_position_report(STATUS_POS_MAX + 1, STATUS_POS_MAX + 1, false, target, position);
  expect_true(target == UNKNOWN_POSITION, "invalid target raw value should stay unknown");
  expect_true(position == UNKNOWN_POSITION, "invalid current raw value should stay unknown while moving");
}

void test_crc_ccitt_known_vector() {
  const uint8_t sample[] = {0x40, 0x20, 0x9C, 0xA3, 0x9C, 0xC0, 0xFF, 0xEE, 0x03, 0x03, 0x00, 0x00};
  expect_true(crc_ccitt(sample, sizeof(sample)) == 0x6E2C, "CRC-CCITT should match the known IO-homecontrol vector");
}

// These cases are the migration-specific regression guard: they pin down which received frames are
// ignored, which complete an exchange immediately, and which must continue into auth or pairing.
void test_exchange_transition_decisions() {
  const uint8_t own[NODE_ID_SIZE] = {0xC0, 0xFF, 0xEE};
  const uint8_t dst[NODE_ID_SIZE] = {0x9C, 0xA3, 0x9C};
  const uint8_t foreign[NODE_ID_SIZE] = {0x11, 0x22, 0x33};

  const IoFrame request = make_frame(own, dst, CMD_EXECUTE, 8);
  const IoFrame direct = make_frame(dst, own, CMD_PRIVATE_RESP, 6);
  const IoFrame challenge = make_frame(dst, own, CMD_CHALLENGE_REQ, HMAC_SIZE);
  const IoFrame unrelated = make_frame(foreign, own, CMD_PRIVATE_RESP, 6);

  expect_true(decisions::classify_exchange_first_response(request, direct) ==
                  decisions::ExchangeFirstResponseDisposition::COMPLETE_DIRECT,
              "matching non-challenge first response should complete directly");
  expect_true(decisions::classify_exchange_first_response(request, challenge) ==
                  decisions::ExchangeFirstResponseDisposition::REQUIRE_AUTH,
              "matching challenge first response should require auth");
  expect_true(decisions::classify_exchange_first_response(request, unrelated) ==
                  decisions::ExchangeFirstResponseDisposition::IGNORE_UNRELATED,
              "unrelated first response should be ignored");

  expect_true(decisions::classify_exchange_final_response(request, direct) ==
                  decisions::ExchangeFinalResponseDisposition::ACCEPT,
              "matching final response should be accepted");
  expect_true(decisions::classify_exchange_final_response(request, unrelated) ==
                  decisions::ExchangeFinalResponseDisposition::IGNORE_UNRELATED,
              "unrelated final response should be ignored");
}

void test_pairing_transition_decisions() {
  const uint8_t controller[NODE_ID_SIZE] = {0xC0, 0xFF, 0xEE};
  const uint8_t device[NODE_ID_SIZE] = {0x9C, 0xA3, 0x9C};
  const uint8_t foreign[NODE_ID_SIZE] = {0x11, 0x22, 0x33};

  const IoFrame discovery = make_frame(device, controller, CMD_DISCOVER_RESP, 2);
  const IoFrame ignored_discovery = make_frame(device, controller, CMD_PRIVATE_RESP, 6);
  const IoFrame key_challenge = make_frame(device, controller, CMD_CHALLENGE_REQ, HMAC_SIZE);
  const IoFrame wrong_cmd = make_frame(device, controller, CMD_PRIVATE_RESP, HMAC_SIZE);
  const IoFrame wrong_len = make_frame(device, controller, CMD_CHALLENGE_REQ, HMAC_SIZE - 1);
  const IoFrame wrong_nodes = make_frame(foreign, controller, CMD_CHALLENGE_REQ, HMAC_SIZE);

  expect_true(
      decisions::classify_pairing_discovery_response(discovery) == decisions::PairingDiscoveryDisposition::ACCEPT,
      "discovery response should be accepted during pairing discovery wait");
  expect_true(decisions::classify_pairing_discovery_response(ignored_discovery) ==
                  decisions::PairingDiscoveryDisposition::IGNORE,
              "non-discovery frame should be ignored during pairing discovery wait");

  expect_true(decisions::classify_pairing_key_challenge(key_challenge, device, controller) ==
                  decisions::PairingKeyChallengeDisposition::ACCEPT,
              "matching key challenge should be accepted during pairing key wait");
  expect_true(decisions::classify_pairing_key_challenge(wrong_cmd, device, controller) ==
                  decisions::PairingKeyChallengeDisposition::IGNORE,
              "wrong command should be ignored during pairing key wait");
  expect_true(decisions::classify_pairing_key_challenge(wrong_len, device, controller) ==
                  decisions::PairingKeyChallengeDisposition::IGNORE,
              "wrong length challenge should be ignored during pairing key wait");
  expect_true(decisions::classify_pairing_key_challenge(wrong_nodes, device, controller) ==
                  decisions::PairingKeyChallengeDisposition::IGNORE,
              "wrong node pairing challenge should be ignored during pairing key wait");
}

}  // namespace

int main() {
  test_execute_round_trip();
  test_status_request_round_trip();
  test_challenge_response_hmac();
  test_crypt_key_round_trip();
  test_key_transfer_round_trip();
  test_device_profiles();
  test_parse_rejects_length_mismatch();
  test_parse_rejects_null_and_truncated_inputs();
  test_max_frame_round_trip();
  test_serialize_rejects_inconsistent_metadata();
  test_hex_to_bytes_rejects_invalid_input();
  test_status_position_decoding();
  test_crc_ccitt_known_vector();
  test_exchange_transition_decisions();
  test_pairing_transition_decisions();
  std::puts("protocol_smoke passed");
  return 0;
}
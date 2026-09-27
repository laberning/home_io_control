/// @file oneway_key_adoption_test.cpp
/// @brief Tests for decode_1w_add_controller() (proto_codecs.cpp) — Phase 3A Step 2: decoding a
/// 1W CMD_ONEWAY_ADD_CONTROLLER (0x30) frame into a recovered controller identity.

#include "proto_codecs.h"
#include "proto_constants.h"
#include "proto_crypto.h"
#include "proto_frame.h"

#include "test_helpers.h"

#include <cstring>

using namespace esphome::home_io_control;

namespace {

const uint8_t VECTOR_SRC[NODE_ID_SIZE] = {0xAB, 0xCD, 0xEF};
const uint8_t VECTOR_DST[NODE_ID_SIZE] = {0x00, 0x00, 0x3F};  // typed broadcast, per the published frame
const uint8_t VECTOR_PLAINTEXT_KEY[AES_KEY_SIZE] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                                    0x09, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16};
const uint8_t VECTOR_ENC_KEY[AES_KEY_SIZE] = {0x7E, 0x60, 0x49, 0x1F, 0x97, 0x6A, 0xDF, 0x65,
                                              0x3D, 0xB0, 0xED, 0x78, 0x5E, 0x49, 0xA2, 0x01};
const uint8_t VECTOR_MAC[HMAC_SIZE] = {0x19, 0xE8, 0x1E, 0xC4, 0x3D, 0x5E};
constexpr uint8_t VECTOR_MANUFACTURER = 0x02;
constexpr uint8_t VECTOR_DATA_BYTE = 0x01;
constexpr uint16_t VECTOR_SEQUENCE = 0x1234;

/// Build the 20-byte declared payload (enc_key + man_id + data + sequence) for an add-controller
/// frame, given an already-wrapped key.
void build_payload(const uint8_t enc_key[AES_KEY_SIZE], uint8_t manufacturer, uint8_t data_byte, uint16_t sequence,
                   uint8_t out[20]) {
  memcpy(out, enc_key, AES_KEY_SIZE);
  out[16] = manufacturer;
  out[17] = data_byte;
  out[18] = static_cast<uint8_t>(sequence >> 8);
  out[19] = static_cast<uint8_t>(sequence & 0xFF);
}

}  // namespace

// ============================================================================
// Published vector, end to end (no hardware needed)
// ============================================================================
// The iown-homecontrol link-layer documentation (CC0-1.0), also captured verbatim as
// tests/corpus/captures/enrollment/reference_1w_enrollment_add_controller_kat.yaml and pinned at the
// frame layer by proto_frame_test.cpp's AddControllerMacTrailerRoundTrip. This is the load-bearing
// test for 1W key adoption: it proves the whole chain (frame model -> key unwrap -> MAC check)
// against a source outside this codebase, not merely against a self-consistent round trip.

TEST(OnewayKeyAdoption, PublishedVectorDecodesAndMacVerifies) {
  // Layout: ctrl0, ctrl1, dst[3], src[3], cmd, enc_key[16], man_id, data, sequence[2], mac[6].
  const uint8_t non_crc[] = {0xFC, 0x00, 0x00, 0x00, 0x3F, 0xAB, 0xCD, 0xEF, 0x30, 0x7E, 0x60, 0x49,
                             0x1F, 0x97, 0x6A, 0xDF, 0x65, 0x3D, 0xB0, 0xED, 0x78, 0x5E, 0x49, 0xA2,
                             0x01, 0x02, 0x01, 0x12, 0x34, 0x19, 0xE8, 0x1E, 0xC4, 0x3D, 0x5E};

  IoFrame frame{};
  ASSERT_TRUE(parse(non_crc, sizeof(non_crc), frame)) << "published vector should parse as a MAC-trailer frame";
  ASSERT_TRUE(frame.has_mac);

  OneWayAdoptedKey out{};
  const auto result = decode_1w_add_controller(frame, out);
  ASSERT_EQ(result, OneWayAddControllerDecodeError::NONE) << "well-formed published vector should decode";

  EXPECT_EQ(0, memcmp(out.system_key, VECTOR_PLAINTEXT_KEY, AES_KEY_SIZE))
      << "recovered key must exactly match the published controller key 0102...1516";
  EXPECT_EQ(out.manufacturer, VECTOR_MANUFACTURER);
  EXPECT_EQ(0, memcmp(out.sender_node, VECTOR_SRC, NODE_ID_SIZE));
  EXPECT_EQ(out.sequence, VECTOR_SEQUENCE);
  EXPECT_EQ(out.mac_status, OneWayMacStatus::VERIFIED)
      << "the published MAC must verify under the recovered key -- the decisive self-check this step exists for";
}

// ============================================================================
// Synthetic round trip (no hardware, no published vector needed)
// ============================================================================

TEST(OnewayKeyAdoption, SyntheticRoundTripRecoversKeyExactly) {
  const uint8_t node[NODE_ID_SIZE] = {0x11, 0x22, 0x33};
  const uint8_t plaintext_key[AES_KEY_SIZE] = {0xDE, 0xCA, 0xFC, 0x0F, 0xFE, 0xE0, 0xFF, 0x1C,
                                               0xE0, 0xFF, 0xEE, 0xBA, 0xBE, 0x01, 0x02, 0x03};
  uint8_t enc_key[AES_KEY_SIZE] = {0};
  ASSERT_TRUE(crypto::crypt_1w_key(node, plaintext_key, enc_key));

  uint8_t payload[20] = {0};
  build_payload(enc_key, 0x07, 0x00, 0x0042, payload);

  IoFrame frame{};
  init_frame(frame, /*is_2w=*/false, /*start=*/true, /*end=*/true, /*low_power=*/false);
  set_src(frame, node);
  set_dst(frame, VECTOR_DST);
  ASSERT_TRUE(set_cmd(frame, CMD_ONEWAY_ADD_CONTROLLER, payload, sizeof(payload)));
  // No MAC on this synthetic frame -- has_mac defaults to false.

  OneWayAdoptedKey out{};
  const auto result = decode_1w_add_controller(frame, out);
  ASSERT_EQ(result, OneWayAddControllerDecodeError::NONE);

  EXPECT_EQ(0, memcmp(out.system_key, plaintext_key, AES_KEY_SIZE)) << "synthetic key must round-trip exactly";
  EXPECT_EQ(out.manufacturer, 0x07);
  EXPECT_EQ(0, memcmp(out.sender_node, node, NODE_ID_SIZE));
  EXPECT_EQ(out.sequence, 0x0042);
  EXPECT_EQ(out.mac_status, OneWayMacStatus::NOT_PRESENT)
      << "a frame with no MAC trailer must decode successfully and report NOT_PRESENT, not an error";
}

// ============================================================================
// Negative cases
// ============================================================================

TEST(OnewayKeyAdoption, RejectsTruncatedPayload) {
  uint8_t payload[20] = {0};
  build_payload(VECTOR_ENC_KEY, VECTOR_MANUFACTURER, VECTOR_DATA_BYTE, VECTOR_SEQUENCE, payload);

  IoFrame frame{};
  init_frame(frame, /*is_2w=*/false, /*start=*/true, /*end=*/true, /*low_power=*/false);
  set_src(frame, VECTOR_SRC);
  set_dst(frame, VECTOR_DST);
  // One byte short of the expected 20-byte declared payload.
  ASSERT_TRUE(set_cmd(frame, CMD_ONEWAY_ADD_CONTROLLER, payload, sizeof(payload) - 1));

  OneWayAdoptedKey out{};
  const auto result = decode_1w_add_controller(frame, out);
  EXPECT_EQ(result, OneWayAddControllerDecodeError::BAD_LENGTH)
      << "a payload short of the expected 20 bytes must be rejected before decryption is attempted";
  EXPECT_EQ(0, memcmp(out.system_key, OneWayAdoptedKey{}.system_key, AES_KEY_SIZE))
      << "rejection must leave `out` unpopulated with real key material";
}

TEST(OnewayKeyAdoption, Rejects2wFrame) {
  uint8_t payload[20] = {0};
  build_payload(VECTOR_ENC_KEY, VECTOR_MANUFACTURER, VECTOR_DATA_BYTE, VECTOR_SEQUENCE, payload);

  IoFrame frame{};
  init_frame(frame, /*is_2w=*/true, /*start=*/true, /*end=*/false, /*low_power=*/false);  // 2W, not 1W
  set_src(frame, VECTOR_SRC);
  set_dst(frame, VECTOR_DST);
  ASSERT_TRUE(set_cmd(frame, CMD_ONEWAY_ADD_CONTROLLER, payload, sizeof(payload)));

  OneWayAdoptedKey out{};
  const auto result = decode_1w_add_controller(frame, out);
  EXPECT_EQ(result, OneWayAddControllerDecodeError::NOT_ONEWAY)
      << "a 2W frame must never be decrypted as an add-controller frame, even with a matching cmd byte";
}

TEST(OnewayKeyAdoption, RejectsWrongCommand) {
  uint8_t payload[20] = {0};
  build_payload(VECTOR_ENC_KEY, VECTOR_MANUFACTURER, VECTOR_DATA_BYTE, VECTOR_SEQUENCE, payload);

  IoFrame frame{};
  init_frame(frame, /*is_2w=*/false, /*start=*/true, /*end=*/true, /*low_power=*/false);
  set_src(frame, VECTOR_SRC);
  set_dst(frame, VECTOR_DST);
  ASSERT_TRUE(set_cmd(frame, CMD_ONEWAY_REMOVE, payload, sizeof(payload)));  // not CMD_ONEWAY_ADD_CONTROLLER

  OneWayAdoptedKey out{};
  const auto result = decode_1w_add_controller(frame, out);
  EXPECT_EQ(result, OneWayAddControllerDecodeError::WRONG_COMMAND);
}

TEST(OnewayKeyAdoption, DecodesButReportsMacFailureWhenMacIsWrong) {
  // Published vector with one MAC byte flipped -- must still decode (the key unwrap is
  // independent of the MAC) but must report the MAC as FAILED, not silently pass.
  uint8_t payload[20] = {0};
  build_payload(VECTOR_ENC_KEY, VECTOR_MANUFACTURER, VECTOR_DATA_BYTE, VECTOR_SEQUENCE, payload);

  IoFrame frame{};
  init_frame(frame, /*is_2w=*/false, /*start=*/true, /*end=*/true, /*low_power=*/false);
  set_src(frame, VECTOR_SRC);
  set_dst(frame, VECTOR_DST);
  ASSERT_TRUE(set_cmd(frame, CMD_ONEWAY_ADD_CONTROLLER, payload, sizeof(payload)));
  frame.has_mac = true;
  memcpy(frame.mac, VECTOR_MAC, HMAC_SIZE);
  frame.mac[0] ^= 0xFF;  // flip one byte

  OneWayAdoptedKey out{};
  const auto result = decode_1w_add_controller(frame, out);
  ASSERT_EQ(result, OneWayAddControllerDecodeError::NONE) << "a wrong MAC is a reported outcome, not a decode error";
  EXPECT_EQ(0, memcmp(out.system_key, VECTOR_PLAINTEXT_KEY, AES_KEY_SIZE))
      << "key unwrap does not depend on the MAC and must still succeed";
  EXPECT_EQ(out.mac_status, OneWayMacStatus::FAILED) << "a corrupted MAC must be reported as FAILED, not VERIFIED";
}

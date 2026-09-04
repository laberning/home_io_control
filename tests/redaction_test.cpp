/// @file redaction_test.cpp
/// @brief Tests for the key-material redaction guarantee (redaction.h, log_frame.h).

#include "hub_core.h"
#include "hub_internal.h"
#include "log_frame.h"
#include "pairing_telemetry.h"
#include "proto_commands.h"
#include "proto_crypto.h"
#include "proto_frame.h"
#include "redaction.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"

#include <cstring>
#include <string>

using namespace esphome::home_io_control;

namespace {

class TestableComponent : public IOHomeControlComponent {
 public:
  using IOHomeControlComponent::discover_and_pair;
  using IOHomeControlComponent::initialized_;
  using IOHomeControlComponent::radio_;
  using IOHomeControlComponent::node_id_;
  using IOHomeControlComponent::system_key_;
};

/// Build a raw RadioRxPacket from a serialised IoFrame.
RadioRxPacket frame_to_rx_packet(const IoFrame &frame, uint32_t freq_hz = FREQ_CH2) {
  RadioRxPacket pkt{};
  pkt.len = serialize(frame, pkt.data, sizeof(pkt.data));
  pkt.freq_hz = freq_hz;
  return pkt;
}

IoFrame build_discovery_response(const uint8_t src[3], const uint8_t dst[3]) {
  IoFrame f{};
  init_frame(f, true, true, true, false);
  set_dst(f, dst);
  set_src(f, src);
  uint8_t payload[DEVICE_METADATA_SIZE] = {0x00, 0x01};  // ROLLER_SHUTTER, subtype 1
  set_cmd(f, CMD_DISCOVER_RESP, payload, sizeof(payload));
  return f;
}

IoFrame build_key_challenge(const uint8_t src[3], const uint8_t dst[3], const uint8_t challenge[6]) {
  IoFrame f{};
  init_frame(f, true, false, false, false);
  set_dst(f, dst);
  set_src(f, src);
  set_cmd(f, CMD_CHALLENGE_REQ, challenge, 6);
  return f;
}

IoFrame build_key_confirm(const uint8_t src[3], const uint8_t dst[3]) {
  IoFrame f{};
  init_frame(f, true, false, false, false);
  set_dst(f, dst);
  set_src(f, src);
  set_cmd(f, CMD_KEY_CONFIRM, nullptr, 0);
  return f;
}

/// Scan `text` for the hex-byte rendering of `key` (space-separated, uppercase — matches
/// bytes_to_hex()'s format) so tests can assert the key never appears in rendered log text.
bool text_contains_key_hex(const std::string &text, const uint8_t *key, size_t key_len) {
  char hex[FRAME_LOG_HEX_BUFFER_SIZE];
  bytes_to_hex(key, static_cast<uint8_t>(key_len), hex, sizeof(hex));
  std::string needle(hex);
  // bytes_to_hex() trails a space; drop it so find() matches a prefix within longer text too.
  if (!needle.empty() && needle.back() == ' ')
    needle.pop_back();
  return text.find(needle) != std::string::npos;
}

}  // namespace

// ============================================================================
// contains_key_material()
// ============================================================================

TEST(Redaction, ContainsKeyMaterial_DetectsExactKey) {
  uint8_t buf[32] = {0};
  memcpy(buf + 5, test::TEST_SYSTEM_KEY, sizeof(test::TEST_SYSTEM_KEY));
  EXPECT_TRUE(contains_key_material(buf, sizeof(buf), test::TEST_SYSTEM_KEY));
}

TEST(Redaction, ContainsKeyMaterial_AbsentForUnrelatedBuffer) {
  uint8_t buf[32];
  for (size_t i = 0; i < sizeof(buf); i++)
    buf[i] = static_cast<uint8_t>(i);
  EXPECT_FALSE(contains_key_material(buf, sizeof(buf), test::TEST_SYSTEM_KEY));
}

TEST(Redaction, ContainsKeyMaterial_ShortBufferNeverMatches) {
  uint8_t buf[8] = {0};
  EXPECT_FALSE(contains_key_material(buf, sizeof(buf), test::TEST_SYSTEM_KEY));
}

// ============================================================================
// contains_key_material() — also covers 1W-recovered keys (crypt_1w_key())
// ============================================================================
// The 1W key-wrap primitive (crypt_1w_key(), Phase 3A Step 1) recovers a plaintext network key
// from an overheard CMD_ONEWAY_ADD_CONTROLLER (0x30) frame. That recovered key is exactly the
// kind of "real key material" contains_key_material() exists to catch — verify the generic
// scanner works on a 1W key with zero 1W-specific code, so a later step that reports/logs the
// recovered key can lean on the existing guarantee rather than inventing a new one.

TEST(Redaction, ContainsKeyMaterial_DetectsRecovered1wKey) {
  const uint8_t node[NODE_ID_SIZE] = {0xAB, 0xCD, 0xEF};
  // Published add-controller vector (the iown-homecontrol link-layer documentation, CC0-1.0;
  // also tests/corpus/captures/enrollment/reference_1w_enrollment_add_controller_kat.yaml).
  const uint8_t wire_ciphertext[AES_KEY_SIZE] = {0x7E, 0x60, 0x49, 0x1F, 0x97, 0x6A, 0xDF, 0x65,
                                                 0x3D, 0xB0, 0xED, 0x78, 0x5E, 0x49, 0xA2, 0x01};

  uint8_t recovered_key[AES_KEY_SIZE] = {0};
  ASSERT_TRUE(crypto::crypt_1w_key(node, wire_ciphertext, recovered_key));

  uint8_t buf[32] = {0};
  memcpy(buf + 3, recovered_key, sizeof(recovered_key));
  EXPECT_TRUE(contains_key_material(buf, sizeof(buf), recovered_key))
      << "the generic key-material scanner must catch a 1W-recovered key exactly as it catches a "
         "2W system key";

  // The ciphertext that actually rides on the wire is not the recovered key — logging the raw
  // 0x30 payload bytes (as any un-redacted frame log already does today) does not by itself leak
  // the plaintext network key, unlike the 2W CMD_KEY_TRANSFER payload.
  EXPECT_FALSE(contains_key_material(wire_ciphertext, sizeof(wire_ciphertext), recovered_key))
      << "the wire ciphertext must not equal the recovered plaintext key";
}

// ============================================================================
// command_carries_key_material()
// ============================================================================

TEST(Redaction, CommandCarriesKeyMaterial_KeyTransferAndChallengePair) {
  EXPECT_TRUE(command_carries_key_material(CMD_KEY_TRANSFER));
  EXPECT_TRUE(command_carries_key_material(CMD_CHALLENGE_REQ));
  EXPECT_TRUE(command_carries_key_material(CMD_CHALLENGE_RESP));
  // The 1W add-controller payload unwraps to the network key using only public inputs (the
  // TRANSFER_KEY and the sender address in the same frame header), so rendering it verbatim is
  // equivalent to printing the key.
  EXPECT_TRUE(command_carries_key_material(CMD_ONEWAY_ADD_CONTROLLER));
  EXPECT_FALSE(command_carries_key_material(CMD_KEY_INIT));
  EXPECT_FALSE(command_carries_key_material(CMD_KEY_CONFIRM));
  EXPECT_FALSE(command_carries_key_material(CMD_ONEWAY_REMOVE));
}

// ============================================================================
// render_frame_hex_redacted()
// ============================================================================

TEST(Redaction, RenderFrameHexRedacted_MasksKeyTransferPayload) {
  IoFrame key_init{};
  ASSERT_TRUE(create_key_init(key_init, test::OWN_ID, test::DST_ID));
  const uint8_t challenge[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};

  IoFrame kt{};
  ASSERT_TRUE(create_key_transfer(kt, key_init, test::DST_ID, test::OWN_ID, test::TEST_SYSTEM_KEY, challenge));

  uint8_t wire[FRAME_MAX_SIZE] = {0};
  uint8_t len = serialize(kt, wire, sizeof(wire));
  ASSERT_EQ(len, FRAME_MIN_SIZE + AES_KEY_SIZE) << "key-transfer frame should carry the full 16-byte payload";

  char out[FRAME_LOG_HEX_BUFFER_SIZE];
  render_frame_hex_redacted(wire, len, out, sizeof(out));
  std::string rendered(out);

  EXPECT_NE(rendered.find("16 bytes masked"), std::string::npos);

  // Header stays visible.
  char header_hex[64];
  bytes_to_hex(wire, FRAME_MIN_SIZE, header_hex, sizeof(header_hex));
  EXPECT_EQ(rendered.rfind(header_hex, 0), 0u) << "header hex should be a prefix of the rendered text";

  // The encrypted payload bytes must never appear as hex, and the plaintext system key
  // must never appear anywhere in the rendered text.
  EXPECT_FALSE(text_contains_key_hex(rendered, wire + FRAME_MIN_SIZE, AES_KEY_SIZE))
      << "masked payload bytes must not leak into the rendered text";
  EXPECT_FALSE(contains_key_material(reinterpret_cast<const uint8_t *>(rendered.data()), rendered.size(),
                                     test::TEST_SYSTEM_KEY));
}

TEST(Redaction, RenderFrameHexRedacted_NonKeyFrameRendersFully) {
  IoFrame f{};
  ASSERT_TRUE(create_key_init(f, test::OWN_ID, test::DST_ID));
  uint8_t wire[FRAME_MAX_SIZE] = {0};
  uint8_t len = serialize(f, wire, sizeof(wire));

  char out[FRAME_LOG_HEX_BUFFER_SIZE];
  render_frame_hex_redacted(wire, len, out, sizeof(out));
  std::string rendered(out);
  EXPECT_EQ(rendered.find("masked"), std::string::npos);

  char full_hex[FRAME_LOG_HEX_BUFFER_SIZE];
  bytes_to_hex(wire, len, full_hex, sizeof(full_hex));
  EXPECT_EQ(rendered, std::string(full_hex));
}

// ============================================================================
// Full mocked pairing exchange — end-to-end redaction guarantee
// ============================================================================

TEST(Redaction, FullPairingExchange_KeyTransferFrameNeverExposesSystemKey) {
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  const uint8_t device_bytes[NODE_ID_SIZE] = {test::DST_ID[0], test::DST_ID[1], test::DST_ID[2]};

  radio.queue_rx(frame_to_rx_packet(build_discovery_response(device_bytes, comp.node_id_)));

  uint8_t challenge[HMAC_SIZE] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
  radio.queue_rx(frame_to_rx_packet(build_key_challenge(device_bytes, comp.node_id_, challenge)));

  radio.queue_rx(frame_to_rx_packet(build_key_confirm(device_bytes, comp.node_id_)));

  ASSERT_TRUE(comp.discover_and_pair()) << "full happy-path pairing flow should succeed";

  // Find the CMD_KEY_TRANSFER (0x32) frame among everything transmitted during pairing and
  // confirm its rendered frame-log text never exposes the plaintext system key.
  bool found_key_transfer = false;
  for (const auto &sent : radio.get_sent_data()) {
    if (sent.size() <= FRAME_CMD_OFFSET || sent[FRAME_CMD_OFFSET] != CMD_KEY_TRANSFER)
      continue;
    found_key_transfer = true;

    // The system key must never appear on the wire in plaintext (sanity: it's AES-encrypted).
    EXPECT_FALSE(contains_key_material(sent.data(), sent.size(), test::TEST_SYSTEM_KEY));

    char out[FRAME_LOG_HEX_BUFFER_SIZE];
    render_frame_hex_redacted(sent.data(), static_cast<uint8_t>(sent.size()), out, sizeof(out));
    std::string rendered(out);

    EXPECT_NE(rendered.find("bytes masked"), std::string::npos) << "key-transfer frame-log text must mask its payload";
    EXPECT_FALSE(contains_key_material(reinterpret_cast<const uint8_t *>(rendered.data()), rendered.size(),
                                       test::TEST_SYSTEM_KEY));
  }
  EXPECT_TRUE(found_key_transfer) << "pairing flow should have transmitted a CMD_KEY_TRANSFER frame";
}

// ============================================================================
// Pairing telemetry — key material cannot appear by construction
// ============================================================================

TEST(Redaction, PairingTelemetryResultStringNeverExposesSystemKey) {
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  const uint8_t device_bytes[NODE_ID_SIZE] = {test::DST_ID[0], test::DST_ID[1], test::DST_ID[2]};
  radio.queue_rx(frame_to_rx_packet(build_discovery_response(device_bytes, comp.node_id_)));
  uint8_t challenge[HMAC_SIZE] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
  radio.queue_rx(frame_to_rx_packet(build_key_challenge(device_bytes, comp.node_id_, challenge)));
  radio.queue_rx(frame_to_rx_packet(build_key_confirm(device_bytes, comp.node_id_)));

  ASSERT_TRUE(comp.discover_and_pair());

  // PairingTelemetry only ever records cmd/src/rssi/phase metadata — never frame payload
  // bytes — so the real system key cannot appear in its output by construction. This test
  // locks that invariant down as a regression guard rather than trusting the design note.
  const std::string result = comp.pairing_telemetry().result_sensor_string();
  EXPECT_FALSE(
      contains_key_material(reinterpret_cast<const uint8_t *>(result.data()), result.size(), test::TEST_SYSTEM_KEY));
  EXPECT_FALSE(text_contains_key_hex(result, test::TEST_SYSTEM_KEY, AES_KEY_SIZE));

  // Every recorded event's fields are limited to cmd/src_node/rssi/aux — none of which can
  // hold key-derived bytes (src_node is a 3-byte node ID, never 16 bytes of key material).
  const PairingTelemetry &telemetry = comp.pairing_telemetry();
  for (uint8_t i = 0; i < telemetry.event_count(); i++) {
    const PairingTelemetryEvent &event = telemetry.events()[i];
    EXPECT_FALSE(contains_key_material(event.src_node, sizeof(event.src_node), test::TEST_SYSTEM_KEY));
    EXPECT_FALSE(contains_key_material(event.dst_node, sizeof(event.dst_node), test::TEST_SYSTEM_KEY));
  }
}

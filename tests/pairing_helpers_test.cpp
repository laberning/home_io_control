#include "hub_core.h"
#include "hub_internal.h"
#include "hub_pairing.h"
#include "proto_frame.h"
#include "radio_interface.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"

#include <array>
#include <cstring>
#include <deque>

using namespace esphome::home_io_control;

// ============================================================================
// Pairing helpers test suite
// ============================================================================
// Tests for IOHomeControlComponent pairing helper methods extracted during
// Phase 4 refactoring of discover_and_pair():
//  - wait_for_discovery_response_
//  - wait_for_key_challenge_
//  - parse_device_from_discovery

namespace {

// --- Testable component exposing protected helpers ---------------------------
class TestableComponent : public IOHomeControlComponent {
 public:
  using IOHomeControlComponent::wait_for_discovery_response_;
  using IOHomeControlComponent::wait_for_key_challenge_;
  using IOHomeControlComponent::parse_device_from_discovery;
  using IOHomeControlComponent::discover_and_pair;
  using IOHomeControlComponent::initialized_;
  using IOHomeControlComponent::busy_;
  using IOHomeControlComponent::radio_;
  using IOHomeControlComponent::node_id_;
  using IOHomeControlComponent::system_key_;
  using IOHomeControlComponent::devices_;
};

// --- Frame builders ---------------------------------------------------------
static std::array<uint8_t, DEVICE_METADATA_SIZE> encode_device_metadata(DeviceType type, uint8_t subtype) {
  return {static_cast<uint8_t>(static_cast<uint8_t>(type) >> DEVICE_TYPE_LOW_BITS_SHIFT),
          static_cast<uint8_t>(subtype | ((static_cast<uint8_t>(type) & ((1U << DEVICE_TYPE_LOW_BITS_SHIFT) - 1U))
                                          << DEVICE_TYPE_HIGH_BITS_SHIFT))};
}

static IoFrame build_discovery_response(const uint8_t src[3], const uint8_t dst[3], DeviceType type, uint8_t subtype) {
  IoFrame f{};
  init_frame(f, true, true, true, false);
  set_dst(f, dst);
  set_src(f, src);
  auto payload = encode_device_metadata(type, subtype);
  set_cmd(f, CMD_DISCOVER_RESP, payload.data(), payload.size());
  return f;
}

static IoFrame build_key_challenge(const uint8_t src[3], const uint8_t dst[3], const uint8_t challenge[6]) {
  IoFrame f{};
  init_frame(f, true, false, false, false);
  set_dst(f, dst);
  set_src(f, src);
  set_cmd(f, CMD_CHALLENGE_REQ, challenge, 6);
  return f;
}

}  // anonymous namespace

// ============================================================================
// wait_for_discovery_response_ tests
// ============================================================================

TEST(PairingHelpers, WaitForDiscoveryResponse_Success) {
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);

  IoFrame resp = build_discovery_response(test::DST_ID, comp.node_id_, DeviceType::ROLLER_SHUTTER, 0x01);
  uint8_t raw[64];
  uint8_t raw_len = serialize(resp, raw, sizeof(raw));
  RadioRxPacket pkt{};
  pkt.len = raw_len;
  memcpy(pkt.data, raw, raw_len);
  pkt.freq_hz = FREQ_CH2;
  radio.queue_rx(pkt);

  RadioRxPacket out_pkt{};
  IoFrame out_frame{};
  auto result = comp.wait_for_discovery_response_(detail::PAIRING_DISCOVERY_RESPONSE_TIMEOUT_MS, out_pkt, out_frame);

  // Expect: valid discovery response yields ACCEPT disposition and populates outputs
  EXPECT_EQ(result, decisions::PairingDiscoveryDisposition::ACCEPT)
      << "valid discovery frame should produce ACCEPT disposition";
  EXPECT_EQ(out_pkt.len, raw_len) << "output packet length should match input raw length";
  EXPECT_EQ(memcmp(out_pkt.data, raw, raw_len), 0) << "output packet data should match input raw bytes";
  EXPECT_EQ(out_frame.cmd, CMD_DISCOVER_RESP) << "parsed frame command should be CMD_DISCOVER_RESP";
  EXPECT_EQ(out_frame.data_len, DEVICE_METADATA_SIZE)
      << "discovery response data length should be 2 bytes (packed type+subtype)";
}

TEST(PairingHelpers, WaitForDiscoveryResponse_TimeoutNoTraffic) {
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);

  RadioRxPacket out_pkt{};
  IoFrame out_frame{};
  auto result = comp.wait_for_discovery_response_(detail::PAIRING_DISCOVERY_RESPONSE_TIMEOUT_MS, out_pkt, out_frame);

  // Expect: no packets seen → NO_RESPONSE disposition
  EXPECT_EQ(result, decisions::PairingDiscoveryDisposition::NO_RESPONSE)
      << "timeout with no radio traffic should yield NO_RESPONSE";
}

TEST(PairingHelpers, WaitForDiscoveryResponse_InvalidFramesIgnoredThenAccept) {
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);

  // Build a frame that's not a discovery response (wrong command)
  IoFrame wrong_cmd{};
  init_frame(wrong_cmd, true, true, true, false);
  set_dst(wrong_cmd, comp.node_id_);
  set_src(wrong_cmd, test::DST_ID);
  set_cmd(wrong_cmd, CMD_PRIVATE_RESP, nullptr, 0);
  uint8_t raw1[64];
  uint8_t len1 = serialize(wrong_cmd, raw1, sizeof(raw1));
  RadioRxPacket pkt1{};
  pkt1.len = len1;
  memcpy(pkt1.data, raw1, len1);
  radio.queue_rx(pkt1);

  // Valid discovery response
  IoFrame valid = build_discovery_response(test::DST_ID, comp.node_id_, DeviceType::ROLLER_SHUTTER, 0x02);
  uint8_t raw2[64];
  uint8_t len2 = serialize(valid, raw2, sizeof(raw2));
  RadioRxPacket pkt2{};
  pkt2.len = len2;
  memcpy(pkt2.data, raw2, len2);
  radio.queue_rx(pkt2);

  RadioRxPacket out_pkt{};
  IoFrame out_frame{};
  auto result = comp.wait_for_discovery_response_(detail::PAIRING_DISCOVERY_RESPONSE_TIMEOUT_MS, out_pkt, out_frame);

  // Expect: invalid frames ignored; first valid discovery response yields ACCEPT
  EXPECT_EQ(result, decisions::PairingDiscoveryDisposition::ACCEPT)
      << "after invalid frames, first valid discovery response should be accepted";
  EXPECT_EQ(decode_packed_device_subtype(out_frame.data[1]), 0x02u)
      << "subtype from valid frame should be preserved in parsed output";
}

TEST(PairingHelpers, WaitForDiscoveryResponse_AllInvalidReturnsInvalid) {
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);

  // Send two invalid frames (wrong command)
  IoFrame wrong1{};
  init_frame(wrong1, true, true, true, false);
  set_dst(wrong1, comp.node_id_);
  set_src(wrong1, test::DST_ID);
  set_cmd(wrong1, CMD_PRIVATE_RESP, nullptr, 0);
  uint8_t raw1[64];
  uint8_t len1 = serialize(wrong1, raw1, sizeof(raw1));
  RadioRxPacket pkt1{};
  pkt1.len = len1;
  memcpy(pkt1.data, raw1, len1);
  radio.queue_rx(pkt1);

  IoFrame wrong2{};
  init_frame(wrong2, true, true, true, false);
  set_dst(wrong2, comp.node_id_);
  set_src(wrong2, test::DST_ID);
  set_cmd(wrong2, CMD_STATUS_UPDATE, nullptr, 0);
  uint8_t raw2[64];
  uint8_t len2 = serialize(wrong2, raw2, sizeof(raw2));
  RadioRxPacket pkt2{};
  pkt2.len = len2;
  memcpy(pkt2.data, raw2, len2);
  radio.queue_rx(pkt2);

  RadioRxPacket out_pkt{};
  IoFrame out_frame{};
  auto result = comp.wait_for_discovery_response_(detail::PAIRING_DISCOVERY_RESPONSE_TIMEOUT_MS, out_pkt, out_frame);

  // Expect: traffic seen but no valid discovery response → INVALID disposition
  EXPECT_EQ(result, decisions::PairingDiscoveryDisposition::INVALID)
      << "when traffic contains only non-discovery frames, INVALID should be returned";
}

TEST(PairingHelpers, WaitForKeyChallenge_Success) {
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);

  uint8_t device_id[3] = {0x44, 0x55, 0x66};
  uint8_t challenge[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};

  IoFrame chal = build_key_challenge(device_id, comp.node_id_, challenge);
  uint8_t raw[64];
  uint8_t raw_len = serialize(chal, raw, sizeof(raw));
  RadioRxPacket pkt{};
  pkt.len = raw_len;
  memcpy(pkt.data, raw, raw_len);
  pkt.freq_hz = FREQ_CH2;
  radio.queue_rx(pkt);

  RadioRxPacket out_pkt{};
  IoFrame out_frame{};
  bool ok = comp.wait_for_key_challenge_(detail::PAIRING_KEY_CHALLENGE_TIMEOUT_MS, out_pkt, out_frame, device_id);

  // Expect: challenge frame accepted and parsed correctly
  EXPECT_TRUE(ok) << "valid key challenge from expected device should be accepted";
  EXPECT_EQ(out_frame.cmd, CMD_CHALLENGE_REQ) << "challenge command should be 0x3C";
  EXPECT_EQ(memcmp(out_frame.src, device_id, NODE_ID_SIZE), 0)
      << "challenge source node ID should match the device we are pairing";
  EXPECT_EQ(memcmp(out_frame.dst, comp.node_id_, NODE_ID_SIZE), 0)
      << "challenge destination should be the controller's node ID";
}

TEST(PairingHelpers, WaitForKeyChallenge_WrongDeviceIgnored) {
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);

  uint8_t expected_device_id[3] = {0x44, 0x55, 0x66};
  uint8_t wrong_device_id[3] = {0x77, 0x88, 0x99};
  uint8_t challenge[6] = {1, 2, 3, 4, 5, 6};

  IoFrame chal = build_key_challenge(wrong_device_id, comp.node_id_, challenge);
  uint8_t raw[64];
  uint8_t raw_len = serialize(chal, raw, sizeof(raw));
  RadioRxPacket pkt{};
  pkt.len = raw_len;
  memcpy(pkt.data, raw, raw_len);
  radio.queue_rx(pkt);

  RadioRxPacket out_pkt{};
  IoFrame out_frame{};
  bool ok =
      comp.wait_for_key_challenge_(detail::PAIRING_KEY_CHALLENGE_TIMEOUT_MS, out_pkt, out_frame, expected_device_id);

  // Expect: frame from wrong device is ignored → failure return
  EXPECT_FALSE(ok) << "challenge from wrong node ID should be ignored and cause failure";
}

TEST(PairingHelpers, WaitForKeyChallenge_NonChallengeIgnored) {
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);

  uint8_t device_id[3] = {0x44, 0x55, 0x66};

  // Non-challenge frame (status response)
  IoFrame resp{};
  init_frame(resp, true, false, true, false);
  set_dst(resp, comp.node_id_);
  set_src(resp, device_id);
  set_cmd(resp, CMD_PRIVATE_RESP, nullptr, 0);

  uint8_t raw[64];
  uint8_t raw_len = serialize(resp, raw, sizeof(raw));
  RadioRxPacket pkt{};
  pkt.len = raw_len;
  memcpy(pkt.data, raw, raw_len);
  radio.queue_rx(pkt);

  RadioRxPacket out_pkt{};
  IoFrame out_frame{};
  bool ok = comp.wait_for_key_challenge_(detail::PAIRING_KEY_CHALLENGE_TIMEOUT_MS, out_pkt, out_frame, device_id);

  // Expect: non‑challenge command is ignored → failure return
  EXPECT_FALSE(ok) << "non‑challenge frame should be ignored during key‑challenge wait";
}

// ============================================================================
// parse_device_from_discovery tests
// ============================================================================

TEST(PairingHelpers, ParseDeviceFromDiscovery_SetsBasicFields) {
  TestableComponent comp;
  IoDevice device{};
  std::string device_id;

  uint8_t node_id[3] = {0x11, 0x22, 0x33};
  IoFrame frame{};
  init_frame(frame, true, true, true, false);
  set_src(frame, node_id);
  set_dst(frame, test::DST_ID);
  auto payload = encode_device_metadata(DeviceType::ROLLER_SHUTTER, 0);
  set_cmd(frame, CMD_DISCOVER_RESP, payload.data(), payload.size());

  comp.parse_device_from_discovery(frame, device, device_id);

  // Verify device metadata populated from discovery payload
  EXPECT_EQ(memcmp(device.node_id, node_id, 3), 0) << "node ID should match source address";
  EXPECT_EQ(device.type, DeviceType::ROLLER_SHUTTER) << "type should decode to ROLLER_SHUTTER (0x02)";
  EXPECT_EQ(device.subtype, 0u) << "subtype should be zero as encoded";
  EXPECT_EQ(device.position, UNKNOWN_POSITION) << "position should start as UNKNOWN_POSITION";
  EXPECT_EQ(device.target, UNKNOWN_POSITION) << "target should start as UNKNOWN_POSITION";
  EXPECT_TRUE(device.is_stopped) << "device should be marked stopped initially";
  EXPECT_FALSE(device.inverted) << "ROLLER_SHUTTER should not be inverted";
  EXPECT_EQ(device_id, "112233") << "device ID string should be hex representation of node ID";
}

TEST(PairingHelpers, ParseDeviceFromDiscovery_InvertedType) {
  TestableComponent comp;
  IoDevice device{};
  std::string device_id;

  uint8_t node_id[3] = {0xAA, 0xBB, 0xCC};
  IoFrame frame{};
  init_frame(frame, true, true, true, false);
  set_src(frame, node_id);
  set_dst(frame, test::DST_ID);
  auto payload = encode_device_metadata(DeviceType::HORIZONTAL_AWNING, 5);
  set_cmd(frame, CMD_DISCOVER_RESP, payload.data(), payload.size());

  comp.parse_device_from_discovery(frame, device, device_id);

  // Verify inverted type properties
  EXPECT_EQ(device.type, DeviceType::HORIZONTAL_AWNING) << "type should decode to HORIZONTAL_AWNING (0x10)";
  EXPECT_EQ(device.subtype, 5u) << "subtype should be 5 as encoded";
  EXPECT_TRUE(device.inverted) << "HORIZONTAL_AWNING should be inverted by default";
  EXPECT_EQ(device_id, "AABBCC") << "device ID string should match node ID hex";
}

TEST(PairingHelpers, ParseDeviceFromDiscovery_ShortPayloadFallsBackToUnknown) {
  TestableComponent comp;
  IoDevice device{};
  std::string device_id;

  uint8_t node_id[3] = {0x11, 0x22, 0x33};
  IoFrame frame{};
  init_frame(frame, true, true, true, false);
  set_src(frame, node_id);
  set_dst(frame, test::DST_ID);
  // Payload length < 2: won't set type/subtype
  uint8_t payload[1] = {0x00};
  set_cmd(frame, CMD_DISCOVER_RESP, payload, 1);

  comp.parse_device_from_discovery(frame, device, device_id);

  // Short payload → type stays UNKNOWN, other fields default
  EXPECT_EQ(device.type, DeviceType::UNKNOWN) << "insufficient payload should leave type as UNKNOWN";
  EXPECT_EQ(device.subtype, 0u) << "subtype should be zero when payload insufficient";
  EXPECT_FALSE(device.inverted) << "inverted flag should be false for UNKNOWN type";
  EXPECT_EQ(device_id, "112233") << "node ID string should still be extracted from source address";
}

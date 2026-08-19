#include "hub_core.h"
#include "hub_pairing.h"
#include "pairing_engine.h"
#include "proto_frame.h"
#include "radio_interface.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <deque>

using namespace esphome::home_io_control;

// ============================================================================
// Pairing helpers test suite
// ============================================================================
// Tests for IOHomeControlComponent pairing helper methods extracted during
// Phase 4 refactoring of discover_and_pair():
//  - wait_for_discovery_response
//  - wait_for_key_challenge
//  - parse_device_from_discovery

namespace {

// --- Testable component exposing protected helpers ---------------------------
class TestableComponent : public IOHomeControlComponent {
 public:
  using IOHomeControlComponent::discover_and_pair;
  using IOHomeControlComponent::queue_discover_and_pair;
  using IOHomeControlComponent::op_queue_;
  using IOHomeControlComponent::initialized_;
  using IOHomeControlComponent::busy_;
  using IOHomeControlComponent::radio_;
  using IOHomeControlComponent::node_id_;
  using IOHomeControlComponent::system_key_;

  // Shadow the base pairing_engine_ with a TestablePairingEngine connected to the
  // same hub state via the same pointers. Tests that script individual phases use
  // this member; discover_and_pair() internally uses the base-class member (both
  // share the same radio/registry/node_id via shared pointers/references).
  test::TestablePairingEngine pairing_engine_{&radio_,          node_id_,  system_key_,       &tuning_,
                                              exchange_engine_, registry_, pairing_telemetry_};
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
// wait_for_discovery_response tests
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
  auto result =
      comp.pairing_engine_.wait_for_discovery_response_(PAIRING_DISCOVERY_RESPONSE_TIMEOUT_MS, out_pkt, out_frame);

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
  auto result =
      comp.pairing_engine_.wait_for_discovery_response_(PAIRING_DISCOVERY_RESPONSE_TIMEOUT_MS, out_pkt, out_frame);

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
  auto result =
      comp.pairing_engine_.wait_for_discovery_response_(PAIRING_DISCOVERY_RESPONSE_TIMEOUT_MS, out_pkt, out_frame);

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
  auto result =
      comp.pairing_engine_.wait_for_discovery_response_(PAIRING_DISCOVERY_RESPONSE_TIMEOUT_MS, out_pkt, out_frame);

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
  bool ok =
      comp.pairing_engine_.wait_for_key_challenge_(PAIRING_KEY_CHALLENGE_TIMEOUT_MS, out_pkt, out_frame, device_id);

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
  bool ok = comp.pairing_engine_.wait_for_key_challenge_(PAIRING_KEY_CHALLENGE_TIMEOUT_MS, out_pkt, out_frame,
                                                         expected_device_id);

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
  bool ok =
      comp.pairing_engine_.wait_for_key_challenge_(PAIRING_KEY_CHALLENGE_TIMEOUT_MS, out_pkt, out_frame, device_id);

  // Expect: non‑challenge command is ignored → failure return
  EXPECT_FALSE(ok) << "non‑challenge frame should be ignored during key‑challenge wait";
}

TEST(PairingHelpers, WaitForKeyChallengeStaysOnRequestChannel) {
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);

  uint8_t device_id[3] = {0x44, 0x55, 0x66};

  // No challenge ever arrives — the wait runs the full window across several empty slices.
  RadioRxPacket out_pkt{};
  IoFrame out_frame{};
  bool ok =
      comp.pairing_engine_.wait_for_key_challenge_(PAIRING_KEY_CHALLENGE_TIMEOUT_MS, out_pkt, out_frame, device_id);

  EXPECT_FALSE(ok);
  EXPECT_TRUE(radio.freq_history().empty())
      << "key challenge wait must never retune: it is a unicast reply and always answers on the request channel";
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

  PairingEngine::parse_device_from_discovery(frame, device, device_id);

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

  PairingEngine::parse_device_from_discovery(frame, device, device_id);

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

  PairingEngine::parse_device_from_discovery(frame, device, device_id);

  // Short payload → type stays UNKNOWN, other fields default
  EXPECT_EQ(device.type, DeviceType::UNKNOWN) << "insufficient payload should leave type as UNKNOWN";
  EXPECT_EQ(device.subtype, 0u) << "subtype should be zero when payload insufficient";
  EXPECT_FALSE(device.inverted) << "inverted flag should be false for UNKNOWN type";
  EXPECT_EQ(device_id, "112233") << "node ID string should still be extracted from source address";
}

// ============================================================================
// build_device_yaml_snippet tests
// ============================================================================

TEST(PairingHelpers, BuildDeviceYamlSnippet_IncompleteMetadata_ContainsDeviceIdAndName) {
  const std::string snippet =
      build_device_yaml_snippet(DeviceType::UNKNOWN, 0, "38B4A1", /*metadata_complete=*/false, /*inverted=*/false);

  EXPECT_NE(snippet.find("io_device_id: \"38B4A1\""), std::string::npos)
      << "the one thing the user can't guess (the device ID) must be present verbatim";
  EXPECT_NE(snippet.find("name: \"My Device\""), std::string::npos)
      << "snippet should still include the standard name placeholder";
  EXPECT_NE(snippet.find("platform: home_io_control"), std::string::npos);
}

TEST(PairingHelpers, BuildDeviceYamlSnippet_IncompleteMetadata_DoesNotGuessAPlatform) {
  const std::string snippet =
      build_device_yaml_snippet(DeviceType::UNKNOWN, 0, "38B4A1", /*metadata_complete=*/false, /*inverted=*/false);

  // A wrong guess (e.g. defaulting to "cover") would be worse than no guess: the user might
  // paste it under the wrong platform's schema without noticing. Only the placeholder token
  // should appear, not a real platform key at the top level.
  EXPECT_NE(snippet.find("<cover|light|switch|lock>:"), std::string::npos)
      << "platform must be left as an explicit placeholder, never guessed";
}

TEST(PairingHelpers, BuildDeviceYamlSnippet_IncompleteMetadata_DoesNotEmitUncommentedDeviceType) {
  const std::string snippet =
      build_device_yaml_snippet(DeviceType::UNKNOWN, 0, "38B4A1", /*metadata_complete=*/false, /*inverted=*/false);

  EXPECT_EQ(snippet.find("\n    io_device_type:"), std::string::npos)
      << "io_device_type is unknown here and must stay commented out, never a bare guessed line";
  EXPECT_NE(snippet.find("# io_device_type:"), std::string::npos)
      << "the commented-out io_device_type line should still explain why it's missing";
}

TEST(PairingHelpers, BuildDeviceYamlSnippet_IncompleteMetadata_DifferentDeviceIdsProduceDifferentSnippets) {
  const std::string snippet_a =
      build_device_yaml_snippet(DeviceType::UNKNOWN, 0, "112233", /*metadata_complete=*/false, /*inverted=*/false);
  const std::string snippet_b =
      build_device_yaml_snippet(DeviceType::UNKNOWN, 0, "AABBCC", /*metadata_complete=*/false, /*inverted=*/false);

  EXPECT_NE(snippet_a, snippet_b);
  EXPECT_NE(snippet_a.find("112233"), std::string::npos);
  EXPECT_NE(snippet_b.find("AABBCC"), std::string::npos);
}

TEST(PairingHelpers, BuildDeviceYamlSnippet_CompleteMetadataInvertedCover) {
  // HORIZONTAL_AWNING is a cover type; inverted=true should emit invert_position.
  const std::string snippet = build_device_yaml_snippet(DeviceType::HORIZONTAL_AWNING, 5, "30E1F2",
                                                        /*metadata_complete=*/true, /*inverted=*/true);

  EXPECT_NE(snippet.find("  cover:\n"), std::string::npos) << "cover-class device should name the cover platform";
  EXPECT_NE(snippet.find("io_device_id: \"30E1F2\""), std::string::npos);
  EXPECT_NE(snippet.find("io_device_type: \"horizontal_awning\""), std::string::npos);
  EXPECT_NE(snippet.find("io_subtype: 5"), std::string::npos);
  EXPECT_NE(snippet.find("invert_position: true"), std::string::npos)
      << "inverted cover should emit invert_position: true";
}

TEST(PairingHelpers, BuildDeviceYamlSnippet_CompleteMetadataNonInvertedLight) {
  const std::string snippet =
      build_device_yaml_snippet(DeviceType::LIGHT, 0, "415CE4", /*metadata_complete=*/true, /*inverted=*/false);

  EXPECT_NE(snippet.find("  light:\n"), std::string::npos) << "light-class device should name the light platform";
  EXPECT_NE(snippet.find("io_device_id: \"415CE4\""), std::string::npos);
  EXPECT_NE(snippet.find("io_device_type: \"light\""), std::string::npos);
  EXPECT_NE(snippet.find("io_subtype: 0"), std::string::npos);
  EXPECT_EQ(snippet.find("invert_position"), std::string::npos) << "non-inverted device must not emit invert_position";
}

TEST(PairingHelpers, BuildDeviceYamlSnippet_CompleteMetadataNoPlatformReturnsEmpty) {
  // HEATING_TEMPERATURE_INTERFACE maps to DeviceCapabilityClass::CLIMATE, which has no
  // ESPHome platform in this repo (no platform_climate.{h,cpp}, unlike cover/light/switch/lock).
  const std::string snippet = build_device_yaml_snippet(DeviceType::HEATING_TEMPERATURE_INTERFACE, 0, "AABBCC",
                                                        /*metadata_complete=*/true, /*inverted=*/false);

  EXPECT_TRUE(snippet.empty()) << "a type with no known ESPHome platform must yield an empty snippet";
}

TEST(PairingHelpers, BuildDeviceYamlSnippet_CompleteMetadataLock) {
  // LOCK maps to DeviceCapabilityClass::LOCK, which has a dedicated ESPHome platform
  // (components/home_io_control/platform_lock.{h,cpp}).
  const std::string snippet =
      build_device_yaml_snippet(DeviceType::LOCK, 0, "AABBCC", /*metadata_complete=*/true, /*inverted=*/false);

  EXPECT_NE(snippet.find("  lock:\n"), std::string::npos) << "lock-class device should name the lock platform";
  EXPECT_NE(snippet.find("io_device_id: \"AABBCC\""), std::string::npos);
  EXPECT_NE(snippet.find("io_device_type: \"lock\""), std::string::npos);
  EXPECT_NE(snippet.find("io_subtype: 0"), std::string::npos);
  EXPECT_EQ(snippet.find("invert_position"), std::string::npos) << "lock must not emit invert_position";
}

// ============================================================================
// Frequency hopping and preamble/sync gating tests
// ============================================================================

namespace {

/// MockRadio variant that simulates preamble/sync detection. Hop counting is MockRadio's own
/// freq_history().size() — no separate counter needed since Step 0 gave every MockRadio that for
/// free.
class SignalDetectRadio : public MockRadio {
 public:
  bool is_sync_detected() override { return sync_detected_; }
  bool is_preamble_detected() override { return preamble_detected_; }

  void set_sync_detected(bool v) { sync_detected_ = v; }
  void set_preamble_detected(bool v) { preamble_detected_ = v; }

 private:
  bool sync_detected_{false};
  bool preamble_detected_{false};
};

}  // anonymous namespace

TEST(PairingHelpers, WaitForDiscoveryResponse_NoHopWhenPreambleDetected) {
  TestableComponent comp;
  comp.initialized_ = true;
  SignalDetectRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);

  // Simulate preamble detected — radio should NOT hop
  radio.set_preamble_detected(true);

  RadioRxPacket out_pkt{};
  IoFrame out_frame{};
  auto result = comp.pairing_engine_.wait_for_discovery_response_(150, out_pkt, out_frame);

  EXPECT_EQ(result, decisions::PairingDiscoveryDisposition::NO_RESPONSE);
  EXPECT_EQ(radio.freq_history().size(), 0u) << "should not hop when preamble is detected (signal arriving)";
}

TEST(PairingHelpers, WaitForDiscoveryResponse_NoHopWhenSyncDetected) {
  TestableComponent comp;
  comp.initialized_ = true;
  SignalDetectRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);

  // Simulate sync word detected — radio should NOT hop
  radio.set_sync_detected(true);

  RadioRxPacket out_pkt{};
  IoFrame out_frame{};
  auto result = comp.pairing_engine_.wait_for_discovery_response_(150, out_pkt, out_frame);

  EXPECT_EQ(result, decisions::PairingDiscoveryDisposition::NO_RESPONSE);
  EXPECT_EQ(radio.freq_history().size(), 0u) << "should not hop when sync word is detected (frame arriving)";
}

TEST(PairingHelpers, WaitForDiscoveryResponseVisitsAllThreeChannels) {
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);

  // No packets queued → hops every slice (SX1276_DISCOVERY_HOP_SLICE_MS = 5 ms) for the whole
  // 150 ms window, unlike the unicast waits above: discovery is a broadcast whose reply channel
  // is unknown, so it is the one wait loop that legitimately visits every channel.
  RadioRxPacket out_pkt{};
  IoFrame out_frame{};
  auto result = comp.pairing_engine_.wait_for_discovery_response_(150, out_pkt, out_frame);

  EXPECT_EQ(result, decisions::PairingDiscoveryDisposition::NO_RESPONSE);
  const auto &history = radio.freq_history();
  EXPECT_NE(std::find(history.begin(), history.end(), FREQ_CH1), history.end());
  EXPECT_NE(std::find(history.begin(), history.end(), FREQ_CH2), history.end());
  EXPECT_NE(std::find(history.begin(), history.end(), FREQ_CH3), history.end());
}

// A preamble/sync detection responds by extending the current dwell a short, fixed amount
// (PREAMBLE_DWELL_MS = 15 ms) rather than by taking another full per-channel hop-slice dwell.
// The two differ in both directions depending on chip (15 vs 5 ms on SX1276, 15 vs 200 ms on
// SX1262/LR1121), so a port that models "preamble detected" as "skip the hop, take another
// full dwell" is wrong on every chip. wait_timeouts() makes the two indistinguishable-by-count
// mistake visible: the sequence must alternate hop-slice, 15, hop-slice, 15, … and never repeat
// the same value twice in a row.
TEST(PairingHelpers, WaitForDiscoveryResponse_PreambleLingerUsesShortExtensionDwell) {
  TestableComponent comp;
  comp.initialized_ = true;
  SignalDetectRadio radio;  // preamble asserted throughout; nothing is ever received
  radio.set_preamble_detected(true);
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);

  RadioRxPacket out_pkt{};
  IoFrame out_frame{};
  auto result = comp.pairing_engine_.wait_for_discovery_response_(150, out_pkt, out_frame);

  EXPECT_EQ(result, decisions::PairingDiscoveryDisposition::NO_RESPONSE);
  EXPECT_TRUE(radio.freq_history().empty()) << "the preamble guard must suppress every hop for the whole window";

  // Walk pairs of (hop-slice, extension) timeouts as long as both come through unclamped. Near
  // the very end of the window std::min() against the shrinking remaining time clamps the last
  // one or two entries to less than their canonical value — that is the window closing, not a
  // different alternation, so it ends the walk without failing the test.
  const auto &timeouts = radio.wait_timeouts();
  size_t canonical_pairs = 0;
  for (size_t i = 0; i + 1 < timeouts.size(); i += 2) {
    if (timeouts[i] != SX1276_DISCOVERY_HOP_SLICE_MS || timeouts[i + 1] != 15u)
      break;
    canonical_pairs++;
  }
  EXPECT_GE(canonical_pairs, 4u) << "need several unclamped hop-slice/extension pairs — alternating "
                                 << SX1276_DISCOVERY_HOP_SLICE_MS << ", 15, " << SX1276_DISCOVERY_HOP_SLICE_MS
                                 << ", 15, … — to prove this is the short extension wait and not a repeat "
                                 << "of the same dwell";
}

// ============================================================================
// Discovery retry tests
// ============================================================================

TEST(PairingHelpers, RunDiscoveryPhase_RetriesOnNoResponse) {
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);

  // No packets queued — all attempts will fail
  pairing::PairingContext context;
  auto result = comp.pairing_engine_.run_discovery_phase_(context);

  EXPECT_NE(result, decisions::PairingDiscoveryDisposition::ACCEPT);
  // Should have sent one discovery TX per attempt (3 attempts)
  EXPECT_EQ(radio.get_send_count(), PAIRING_DISCOVERY_MAX_ATTEMPTS)
      << "should transmit discovery frame once per retry attempt";
}

TEST(PairingHelpers, RunDiscoveryPhase_SucceedsOnFirstAttempt) {
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);

  // Queue a valid discovery response that will be found after the TX
  IoFrame resp = build_discovery_response(test::DST_ID, comp.node_id_, DeviceType::ROLLER_SHUTTER, 0x01);
  uint8_t raw[64];
  uint8_t raw_len = serialize(resp, raw, sizeof(raw));
  RadioRxPacket pkt{};
  pkt.len = raw_len;
  memcpy(pkt.data, raw, raw_len);
  radio.queue_rx(pkt);

  pairing::PairingContext context;
  auto result = comp.pairing_engine_.run_discovery_phase_(context);

  EXPECT_EQ(result, decisions::PairingDiscoveryDisposition::ACCEPT);
  EXPECT_EQ(radio.get_send_count(), 1) << "should stop after first successful discovery";
  EXPECT_EQ(context.device_id, node_id_to_string(test::DST_ID));
}

// ============================================================================
// Queue priority tests
// ============================================================================

TEST(PairingHelpers, QueueDiscoverAndPair_FlushesStatusPolls) {
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;

  // Pre-fill queue with status and name requests
  comp.op_queue_.enqueue_request_status("AABBCC");
  comp.op_queue_.enqueue_request_name("DDEEFF");
  comp.op_queue_.enqueue_set_position("112233", 50);

  comp.queue_discover_and_pair();

  // Status and name polls should be flushed, position command kept
  EXPECT_EQ(comp.op_queue_.size(), 2u) << "status/name polls should be removed, position + discovery remain";
  EXPECT_EQ(comp.op_queue_.front().type, PendingOperationType::DISCOVER_AND_PAIR)
      << "discovery should be at the front of the queue";
  EXPECT_EQ(comp.op_queue_.back().type, PendingOperationType::SET_POSITION) << "position command should be preserved";
}

TEST(PairingHelpers, QueueDiscoverAndPair_NoDuplicates) {
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;

  comp.queue_discover_and_pair();
  comp.queue_discover_and_pair();  // second call should be ignored

  int count = 0;
  for (const auto &op : comp.op_queue_) {
    if (op.type == PendingOperationType::DISCOVER_AND_PAIR)
      count++;
  }
  EXPECT_EQ(count, 1) << "should not queue duplicate discovery requests";
}

// ============================================================================
// discover_and_pair() end-to-end flow (pinning tests)
// ============================================================================

namespace {

/// Build a raw RadioRxPacket from a serialised IoFrame.
static RadioRxPacket frame_to_rx_packet(const IoFrame &frame, uint32_t freq_hz = FREQ_CH2) {
  RadioRxPacket pkt{};
  pkt.len = serialize(frame, pkt.data, sizeof(pkt.data));
  pkt.freq_hz = freq_hz;
  return pkt;
}

/// Build the 0x33 key-confirm frame device→controller with matching endpoints for the 0x32 key-transfer.
static IoFrame build_key_confirm(const uint8_t src[3], const uint8_t dst[3]) {
  IoFrame f{};
  init_frame(f, true, false, false, false);
  set_dst(f, dst);
  set_src(f, src);
  set_cmd(f, CMD_KEY_CONFIRM, nullptr, 0);
  return f;
}

}  // anonymous namespace

TEST(DiscoverAndPair, NotInitializedReturnsFalse) {
  TestableComponent comp;
  // comp.initialized_ stays false
  MockRadio radio;
  comp.radio_ = &radio;
  EXPECT_FALSE(comp.discover_and_pair()) << "should return false when hub is not initialized";
}

TEST(DiscoverAndPair, DiscoveryTimeoutReturnsFalse) {
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  // No packets queued — discovery times out immediately (MockRadio returns false on every wait_for_packet)
  EXPECT_FALSE(comp.discover_and_pair()) << "should return false when discovery receives no response";
}

TEST(DiscoverAndPair, KeyExchangeNoChallenge_ReturnsFalse) {
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);

  const uint8_t device_bytes[NODE_ID_SIZE] = {test::DST_ID[0], test::DST_ID[1], test::DST_ID[2]};

  // Discovery succeeds — then no key challenge arrives
  radio.queue_rx(
      frame_to_rx_packet(build_discovery_response(device_bytes, comp.node_id_, DeviceType::ROLLER_SHUTTER, 1)));

  EXPECT_FALSE(comp.discover_and_pair()) << "should return false when key challenge is never received";
}

TEST(DiscoverAndPair, HappyPath_RegistersDevice) {
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  // system_key_ stays all zeros — AES with a zero key is still a valid operation

  const uint8_t device_bytes[NODE_ID_SIZE] = {test::DST_ID[0], test::DST_ID[1], test::DST_ID[2]};

  // 1. Discovery response (0x29): device → controller
  radio.queue_rx(
      frame_to_rx_packet(build_discovery_response(device_bytes, comp.node_id_, DeviceType::ROLLER_SHUTTER, 1)));

  // 2. Key challenge (0x3C): device → controller
  uint8_t challenge[HMAC_SIZE] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
  radio.queue_rx(frame_to_rx_packet(build_key_challenge(device_bytes, comp.node_id_, challenge)));

  // 3. Key confirm (0x33): device → controller, consumed by send_and_receive_ for the 0x32 key-transfer.
  //    Endpoints must be the reverse of the 0x32 request (src=device, dst=controller).
  radio.queue_rx(frame_to_rx_packet(build_key_confirm(device_bytes, comp.node_id_)));

  // 4. finalize_pairing_configuration (0x6F) ignores its exchange result — no response needed.

  EXPECT_TRUE(comp.discover_and_pair()) << "full happy-path pairing flow should succeed";

  // Device must appear in the registry after successful pairing.
  EXPECT_NE(comp.get_device(node_id_to_string(device_bytes)), nullptr)
      << "paired device should be registered after discover_and_pair";
}

// ============================================================================
// wait_for_key_confirm timeout (SX1262 dedicated path)
// ============================================================================

namespace {
/// A minimal 0x32 key-transfer frame as the wait_for_key_confirm_() request context — real
/// encrypted payload not needed, only the frame endpoints matter for the confirm-wait endpoint
/// check. `own_id` must be the same node ID already copied into `comp.node_id_`.
pairing::PairingContext make_key_transfer_context(const uint8_t own_id[NODE_ID_SIZE],
                                                  const uint8_t device_id[NODE_ID_SIZE]) {
  pairing::PairingContext context;
  init_frame(context.req, true, false, false, false);
  set_dst(context.req, device_id);
  set_src(context.req, own_id);
  set_cmd(context.req, CMD_KEY_TRANSFER, nullptr, 0);
  return context;
}
}  // namespace

TEST(PairingHelpers, WaitForKeyConfirm_TimeoutNoConfirm) {
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  pairing::PairingContext context = make_key_transfer_context(comp.node_id_, test::DST_ID);

  // No 0x33 ever arrives — MockRadio returns false from wait_for_packet immediately.
  bool ok = comp.pairing_engine_.wait_for_key_confirm_(context);

  EXPECT_FALSE(ok) << "timeout without receiving CMD_KEY_CONFIRM (0x33) should return false";
  EXPECT_EQ(radio.get_send_count(), EXCHANGE_RETRY_COUNT)
      << "should attempt the key-transfer TX once per retry before giving up";
}

// ============================================================================
// wait_for_key_confirm channel policy (regression coverage for the removed hop)
// ============================================================================

// MockRadioSX1262 is used throughout this section — not for any dwell it reports (this loop
// holds the request channel and never dwells), but because has_fast_tx_rx_turnaround() == false
// is the only configuration production ever reaches this function in: fast-turnaround radios
// (SX1276) take the standard send_and_receive_() / wait_for_first_response_() exchange path
// instead and never call wait_for_key_confirm_() at all. Testing this loop under the plain
// MockRadio (which reports has_fast_tx_rx_turnaround() == true) would be testing a configuration
// that can't occur.

TEST(PairingHelpers, KeyConfirmWaitStaysOnRequestChannel) {
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadioSX1262 radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  pairing::PairingContext context = make_key_transfer_context(comp.node_id_, test::DST_ID);

  // No 0x33 ever arrives, so the wait runs the full window with nothing received.
  bool ok = comp.pairing_engine_.wait_for_key_confirm_(context);

  EXPECT_FALSE(ok);
  EXPECT_TRUE(radio.freq_history().empty())
      << "key confirm wait must never retune: it is a unicast reply and always answers on the request channel";
}

TEST(PairingHelpers, KeyConfirmWaitAcceptsLateReply) {
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadioSX1262 radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  const uint8_t device_bytes[NODE_ID_SIZE] = {test::DST_ID[0], test::DST_ID[1], test::DST_ID[2]};

  pairing::PairingContext context = make_key_transfer_context(comp.node_id_, device_bytes);

  // Old code hopped on every empty slice, so it would have left the request channel well before
  // this reply arrives. Queue genuine silence first — wait_for_packet() returning false, not a
  // frame that gets rejected for an unrelated reason — since a rejected frame is consumed
  // immediately and never leaves a slice empty; three empty slices is comfortably past where the
  // old 150 ms slicing would have hopped at least once.
  radio.queue_rx_silence(3);
  radio.queue_rx(frame_to_rx_packet(build_key_confirm(device_bytes, comp.node_id_)));

  bool ok = comp.pairing_engine_.wait_for_key_confirm_(context);

  EXPECT_TRUE(ok) << "a reply arriving after several empty slices must still be accepted, since the wait no "
                     "longer hops away from the channel it is on";
  EXPECT_TRUE(radio.freq_history().empty())
      << "confirms the acceptance above wasn't a coincidental hop back onto the right channel";
}

TEST(PairingHelpers, KeyConfirmWaitStillReturnsFalseOnErrorResponse) {
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadioSX1262 radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  const uint8_t device_bytes[NODE_ID_SIZE] = {test::DST_ID[0], test::DST_ID[1], test::DST_ID[2]};

  pairing::PairingContext context = make_key_transfer_context(comp.node_id_, device_bytes);

  IoFrame error_resp{};
  init_frame(error_resp, true, false, false, false);
  set_dst(error_resp, comp.node_id_);
  set_src(error_resp, device_bytes);
  const uint8_t error_code[1] = {0x01};
  set_cmd(error_resp, CMD_ERROR_RESP, error_code, 1);
  radio.queue_rx(frame_to_rx_packet(error_resp));

  bool ok = comp.pairing_engine_.wait_for_key_confirm_(context);

  EXPECT_FALSE(ok) << "an explicit ERROR_RESP must still return false immediately, without exhausting retries";
  EXPECT_EQ(radio.get_send_count(), 1) << "the wait must not retry after an explicit refusal";
}

// ============================================================================
// Key-exchange confirm-wait strategy selection (RadioDriver::has_fast_tx_rx_turnaround)
// ============================================================================

namespace {

// Wire layout: [CTRL0][CTRL1][DST 3B][SRC 3B][CMD ...] — command byte at offset 8.
constexpr size_t WIRE_CMD_OFFSET = 8;

std::vector<uint8_t> sent_command_bytes(const MockRadio &radio) {
  std::vector<uint8_t> cmds;
  for (const auto &frame : radio.get_sent_data()) {
    cmds.push_back(frame.size() > WIRE_CMD_OFFSET ? frame[WIRE_CMD_OFFSET] : 0);
  }
  return cmds;
}

// Shared setup: device answers the key-init with a challenge; the key-confirm
// (0x33) never arrives, so the phase exhausts its confirm strategy and fails.
void run_key_exchange_without_confirm(TestableComponent &comp, MockRadio &radio, pairing::PairingContext &context) {
  comp.initialized_ = true;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  uint8_t device_id[3] = {0x44, 0x55, 0x66};
  uint8_t challenge[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  memcpy(context.device.node_id, device_id, NODE_ID_SIZE);

  IoFrame chal = build_key_challenge(device_id, comp.node_id_, challenge);
  uint8_t raw[64];
  uint8_t raw_len = serialize(chal, raw, sizeof(raw));
  RadioRxPacket pkt{};
  pkt.len = raw_len;
  memcpy(pkt.data, raw, raw_len);
  pkt.freq_hz = FREQ_CH2;
  radio.queue_rx(pkt);

  EXPECT_FALSE(comp.pairing_engine_.run_key_exchange_phase_(context))
      << "key exchange must fail when the key-confirm (0x33) never arrives";
}

}  // anonymous namespace

TEST(PairingHelpers, KeyExchangePhase_FastTurnaroundUsesStandardExchange) {
  TestableComponent comp;
  MockRadio radio;  // fast turnaround: standard send_and_receive path
  pairing::PairingContext context;
  run_key_exchange_without_confirm(comp, radio, context);

  // Expected TX sequence: one 0x31 key-init, then only 0x32 key-transfer retries
  // from send_and_receive — no key-init re-trigger.
  std::vector<uint8_t> expected = {CMD_KEY_INIT};
  for (uint8_t i = 0; i < EXCHANGE_RETRY_COUNT; i++)
    expected.push_back(CMD_KEY_TRANSFER);
  EXPECT_EQ(sent_command_bytes(radio), expected)
      << "fast-turnaround driver should await the 0x33 via the standard exchange";
}

TEST(PairingHelpers, KeyExchangePhase_SlowTurnaroundUsesDedicatedConfirmWait) {
  TestableComponent comp;
  MockRadioSX1262 radio;  // slow turnaround: dedicated confirm wait + key-init re-trigger
  pairing::PairingContext context;
  run_key_exchange_without_confirm(comp, radio, context);

  // Expected TX sequence: one 0x31 key-init, EXCHANGE_RETRY_COUNT × 0x32 from the
  // dedicated confirm wait, then two 0x31 re-sends to trigger the device's auto-confirm.
  std::vector<uint8_t> expected = {CMD_KEY_INIT};
  for (uint8_t i = 0; i < EXCHANGE_RETRY_COUNT; i++)
    expected.push_back(CMD_KEY_TRANSFER);
  expected.push_back(CMD_KEY_INIT);
  expected.push_back(CMD_KEY_INIT);
  EXPECT_EQ(sent_command_bytes(radio), expected)
      << "slow-turnaround driver should use the dedicated confirm wait with key-init re-trigger";
}

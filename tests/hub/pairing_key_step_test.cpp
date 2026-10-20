/// @file pairing_key_step_test.cpp
/// @brief Pairing key step: explicit refusals end the attempt, the key-held check after an
/// unconfirmed exchange, the experimental key pull (0x38), and the advice they produce.

#include "hub_core.h"
#include "hub_pairing.h"
#include "pairing_advisor.h"
#include "pairing_engine.h"
#include "proto_codecs.h"
#include "proto_commands.h"
#include "proto_crypto.h"
#include "proto_frame.h"

#include "esp_random.h"
#include "test_helpers.h"
#include "stubs/radio_test_common.h"

#include <algorithm>
#include <cstring>
#include <vector>

using namespace esphome::home_io_control;

namespace {

class TestableComponent : public IOHomeControlComponent {
 public:
  using IOHomeControlComponent::discover_and_pair;
  using IOHomeControlComponent::initialized_;
  using IOHomeControlComponent::radio_;
  using IOHomeControlComponent::node_id_;
  using IOHomeControlComponent::system_key_;
  using IOHomeControlComponent::tuning_;
  using IOHomeControlComponent::exchange_engine_;
  using IOHomeControlComponent::pairing_telemetry_;

  RecentOneWayPairingSighting recent_oneway_pairing_sighting_{};
  test::TestablePairingEngine pairing_engine_{
      &radio_,          node_id_,  system_key_,        &tuning_,
      exchange_engine_, registry_, pairing_telemetry_, recent_oneway_pairing_sighting_};
};

constexpr uint8_t DEVICE_ID[NODE_ID_SIZE] = {0xB6, 0xBD, 0xFB};
constexpr size_t WIRE_CMD_OFFSET = 8;  // [CTRL0][CTRL1][DST 3B][SRC 3B][CMD ...]

RadioRxPacket to_packet(const IoFrame &frame) {
  RadioRxPacket pkt{};
  pkt.len = serialize(frame, pkt.data, sizeof(pkt.data));
  pkt.freq_hz = FREQ_CH2;
  return pkt;
}

IoFrame device_frame(const uint8_t dst[NODE_ID_SIZE], uint8_t cmd, const uint8_t *data, uint8_t len) {
  IoFrame f{};
  init_frame(f, true, false, false, false);
  set_dst(f, dst);
  set_src(f, DEVICE_ID);
  set_cmd(f, cmd, data, len);
  return f;
}

IoFrame challenge_frame(const uint8_t dst[NODE_ID_SIZE]) {
  static const uint8_t challenge[HMAC_SIZE] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  return device_frame(dst, CMD_CHALLENGE_REQ, challenge, HMAC_SIZE);
}

IoFrame error_frame(const uint8_t dst[NODE_ID_SIZE], uint8_t code) {
  return device_frame(dst, CMD_ERROR_RESP, &code, 1);
}

/// The Atlantic AGILIA discovery reply from tests/corpus/captures/pairing/
/// atlantic_agilia_pairing_key_transfer_command_unknown.yaml, readdressed to `dst`: node type bytes
/// FF C0 (remote controller), manufacturer 12, flags 0xCC.
IoFrame atlantic_discovery_reply(const uint8_t dst[NODE_ID_SIZE]) {
  const uint8_t payload[] = {0xFF, 0xC0, 0xB6, 0xBD, 0xFB, 0x0C, 0xCC, 0x00, 0x00};
  IoFrame f{};
  init_frame(f, true, true, true, false);
  set_dst(f, dst);
  set_src(f, DEVICE_ID);
  set_cmd(f, CMD_DISCOVER_RESP, payload, sizeof(payload));
  return f;
}

std::vector<uint8_t> sent_commands(const MockRadio &radio) {
  std::vector<uint8_t> cmds;
  for (const auto &frame : radio.get_sent_data())
    cmds.push_back(frame.size() > WIRE_CMD_OFFSET ? frame[WIRE_CMD_OFFSET] : 0);
  return cmds;
}

void setup(TestableComponent &comp, MockRadio &radio) {
  comp.initialized_ = true;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);
}

pairing::PairingContext context_for_device() {
  pairing::PairingContext context;
  memcpy(context.device.node_id, DEVICE_ID, NODE_ID_SIZE);
  context.device_id = node_id_to_string(DEVICE_ID);
  return context;
}

}  // namespace

// ============================================================================
// Result-byte decoding
// ============================================================================

TEST(ErrorRespDecoding, PackedCodesUseCodeShiftedByThree) {
  EXPECT_EQ(error_resp_code(0x08), 1u);
  EXPECT_EQ(error_resp_state(0x08), 0u);
  EXPECT_STREQ(command_result_name(0x08), "COMMAND_UNKNOWN");
  EXPECT_STREQ(command_result_name(0x18), "BAD_MAC");
  EXPECT_STREQ(command_result_name(0x28), "PARAMETER_UNKNOWN");
  EXPECT_STREQ(command_result_name(0x38), "PRIORITY_LOCKED");
  EXPECT_STREQ(command_result_name(0x58), "TOTAL_INCOHERENCE");
  EXPECT_STREQ(command_result_name(0x80), "COMMAND_REJECTED");
  EXPECT_STREQ(command_result_name(0x88), "ALREADY_CONFIGURED");
}

TEST(ErrorRespDecoding, BytesWithStateBitsKeepTheRunStatusTable) {
  // A Somfy awning answers an identify with 0x05 (corpus somfy_awning_identify_wrong_systemkey_sx1276).
  EXPECT_EQ(error_resp_state(0x05), 5u);
  EXPECT_STREQ(command_result_name(0x05), "WRONG_SYSTEMKEY");
  EXPECT_STREQ(command_result_name(RESULT_LIMITATION_BY_RAIN), "LIMITATION_BY_RAIN");
}

// ============================================================================
// Node type from discovery
// ============================================================================

TEST(DiscoveryNodeType, RemoteControllerTypeSurvivesInFull) {
  EXPECT_EQ(decode_packed_device_type(0xFF, 0xC0), DeviceType::REMOTE_CONTROLLER);
  IoDevice device{};
  std::string id;
  const DiscoveryResponseInfo info = decode_discovery_response(atlantic_discovery_reply(test::OWN_ID), device, id);
  EXPECT_EQ(device.type, DeviceType::REMOTE_CONTROLLER);
  EXPECT_EQ(info.manufacturer, 12u);
}

TEST(DiscoveryNodeType, ActuatorTypeMatchesDeviceType) {
  // 0x04 0x00 = type 0x10 (horizontal awning), as in the Sunilus corpus capture's family.
  EXPECT_EQ(decode_packed_device_type(0x04, 0x00), DeviceType::HORIZONTAL_AWNING);
}

// ============================================================================
// Explicit refusal of the key transfer
// ============================================================================

TEST(PairingKeyStep, SlowTurnaroundRefusalEndsTheExchangeWithoutKeyInitResend) {
  TestableComponent comp;
  MockRadioSX1262 radio;
  setup(comp, radio);
  pairing::PairingContext context = context_for_device();

  radio.queue_rx(to_packet(challenge_frame(comp.node_id_)));
  radio.queue_rx(to_packet(error_frame(comp.node_id_, RESULT_COMMAND_UNKNOWN)));

  EXPECT_FALSE(comp.pairing_engine_.run_key_exchange_phase_(context));
  EXPECT_TRUE(context.key_refused);
  EXPECT_EQ(context.key_refusal_code, RESULT_COMMAND_UNKNOWN);
  EXPECT_TRUE(context.saw_key_challenge);
  EXPECT_EQ(sent_commands(radio), (std::vector<uint8_t>{CMD_KEY_INIT, CMD_KEY_TRANSFER}))
      << "an explicit refusal is the device's answer: no key-init re-send after it";
}

TEST(PairingKeyStep, FastTurnaroundRefusalIsRecorded) {
  TestableComponent comp;
  MockRadio radio;
  setup(comp, radio);
  pairing::PairingContext context = context_for_device();

  radio.queue_rx(to_packet(challenge_frame(comp.node_id_)));
  radio.queue_rx(to_packet(error_frame(comp.node_id_, RESULT_COMMAND_UNKNOWN)));

  EXPECT_FALSE(comp.pairing_engine_.run_key_exchange_phase_(context));
  EXPECT_TRUE(context.key_refused);
  EXPECT_EQ(context.key_refusal_code, RESULT_COMMAND_UNKNOWN);
}

TEST(PairingKeyStep, AtlanticFlowEndsRefusedWithPullAdvice) {
  TestableComponent comp;
  MockRadioSX1262 radio;
  setup(comp, radio);

  radio.queue_rx(to_packet(atlantic_discovery_reply(comp.node_id_)));
  IoFrame ack{};
  ASSERT_TRUE(create_discover_confirm_ack(ack, DEVICE_ID, comp.node_id_));
  radio.queue_rx(to_packet(ack));
  radio.queue_rx(to_packet(challenge_frame(comp.node_id_)));
  radio.queue_rx(to_packet(error_frame(comp.node_id_, RESULT_COMMAND_UNKNOWN)));
  // The key-held check's name read then gets no answer.

  EXPECT_FALSE(comp.discover_and_pair());

  const PairingTelemetry &telemetry = comp.pairing_telemetry();
  EXPECT_EQ(telemetry.outcome(), PairingOutcome::KEY_TRANSFER_REFUSED);
  EXPECT_EQ(telemetry.discovered_node_type(), DeviceType::REMOTE_CONTROLLER);
  EXPECT_NE(telemetry.result_sensor_string().find("outcome=key_transfer_refused"), std::string::npos);
  EXPECT_NE(telemetry.result_sensor_string().find("key_push_not_accepted"), std::string::npos);

  const std::vector<uint8_t> cmds = sent_commands(radio);
  EXPECT_EQ(std::count(cmds.begin(), cmds.end(), CMD_KEY_INIT), 1) << "one key-init, no retries after the refusal";
  EXPECT_EQ(std::count(cmds.begin(), cmds.end(), CMD_KEY_TRANSFER), 1);
  EXPECT_EQ(std::count(cmds.begin(), cmds.end(), CMD_GET_NAME), PAIRING_KEY_VERIFY_MAX_TRIES)
      << "the key-held check runs once, with its own small try budget";
}

// ============================================================================
// Key-held check after an unconfirmed exchange
// ============================================================================

namespace {

IoFrame name_reply(const uint8_t dst[NODE_ID_SIZE]) {
  uint8_t name[16] = {'R', 'a', 'd', 'i', 'a', 't', 'o', 'r'};
  IoFrame f = device_frame(dst, CMD_GET_NAME_RESP, name, sizeof(name));
  f.ctrl0 |= CTRL0_END;
  return f;
}

}  // namespace

TEST(PairingKeyStep, KeyHeldCheckAcceptsAuthenticatedNameReply) {
  TestableComponent comp;
  MockRadio radio;
  setup(comp, radio);
  pairing::PairingContext context = context_for_device();

  radio.queue_rx(to_packet(challenge_frame(comp.node_id_)));  // challenges our 0x50
  radio.queue_rx(to_packet(name_reply(comp.node_id_)));       // then answers it

  EXPECT_TRUE(comp.pairing_engine_.verify_device_holds_key_(context));
  EXPECT_EQ(sent_commands(radio), (std::vector<uint8_t>{CMD_GET_NAME, CMD_CHALLENGE_RESP}));
}

TEST(PairingKeyStep, KeyHeldCheckRejectsUnchallengedNameReply) {
  TestableComponent comp;
  MockRadio radio;
  setup(comp, radio);
  pairing::PairingContext context = context_for_device();

  radio.queue_rx(to_packet(name_reply(comp.node_id_)));

  EXPECT_FALSE(comp.pairing_engine_.verify_device_holds_key_(context))
      << "a reply without a challenge proves nothing about the device's key";
}

TEST(PairingKeyStep, UnconfirmedKeyExchangeIsRescuedByKeyHeldCheck) {
  TestableComponent comp;
  MockRadio radio;
  setup(comp, radio);
  comp.tuning_.pairing_key_exchange_retries = 1;

  const uint8_t device_bytes[NODE_ID_SIZE] = {DEVICE_ID[0], DEVICE_ID[1], DEVICE_ID[2]};
  IoFrame discovery = atlantic_discovery_reply(comp.node_id_);
  // An ordinary actuator type (roller shutter, 0x02) so the snippet path runs as for any device.
  discovery.data[0] = 0x00;
  discovery.data[1] = 0x80;
  radio.queue_rx(to_packet(discovery));
  IoFrame ack{};
  ASSERT_TRUE(create_discover_confirm_ack(ack, device_bytes, comp.node_id_));
  radio.queue_rx(to_packet(ack));
  radio.queue_rx(to_packet(challenge_frame(comp.node_id_)));  // to our 0x31
  // TX order: 0x28, 0x2C, 0x31, then three 0x32 tries whose transmit fails (so they draw no RX).
  radio.queue_tx_result(true);
  radio.queue_tx_result(true);
  radio.queue_tx_result(true);
  radio.queue_tx_result(false);
  radio.queue_tx_result(false);
  radio.queue_tx_result(false);
  radio.queue_rx(to_packet(challenge_frame(comp.node_id_)));  // to the key-held check's 0x50
  radio.queue_rx(to_packet(name_reply(comp.node_id_)));

  EXPECT_TRUE(comp.discover_and_pair()) << "a device that answers with our key is paired, confirm or not";
  EXPECT_NE(comp.get_device(node_id_to_string(device_bytes)), nullptr);
  EXPECT_EQ(comp.pairing_telemetry().outcome(), PairingOutcome::PAIRED);
}

// ============================================================================
// Key pull (pairing_key_transfer: pull)
// ============================================================================

namespace {

constexpr uint8_t PULL_CHALLENGE[HMAC_SIZE] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
constexpr uint8_t VERIFY_CHALLENGE[HMAC_SIZE] = {0x11, 0x12, 0x13, 0x14, 0x15, 0x16};
constexpr uint8_t OTHER_KEY[AES_KEY_SIZE] = {0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE,
                                             0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};

/// Our 0x38 carrying PULL_CHALLENGE.
IoFrame pull_request(const uint8_t own[NODE_ID_SIZE]) {
  IoFrame request{};
  create_launch_key_transfer(request, own, DEVICE_ID, PULL_CHALLENGE);
  return request;
}

/// The device's 0x32 answering pull_request(), wrapping `key` with the whole-request IV. Encrypting
/// and decrypting are the same XOR, so recover_key_from_pull() also wraps.
IoFrame pulled_key_frame(const uint8_t dst[NODE_ID_SIZE], const uint8_t key[AES_KEY_SIZE]) {
  uint8_t wrapped[AES_KEY_SIZE];
  recover_key_from_pull(pull_request(dst), key, wrapped);
  return device_frame(dst, CMD_KEY_TRANSFER, wrapped, AES_KEY_SIZE);
}

/// The device's 0x3D answering our VERIFY_CHALLENGE, signed with `key` over its own 0x32.
IoFrame proof_frame(const uint8_t dst[NODE_ID_SIZE], const IoFrame &its_key_transfer, const uint8_t key[AES_KEY_SIZE]) {
  uint8_t mac[HMAC_SIZE];
  challenge_resp_hmac(its_key_transfer, VERIFY_CHALLENGE, key, mac);
  return device_frame(dst, CMD_CHALLENGE_RESP, mac, HMAC_SIZE);
}

struct RngGuard {
  RngGuard() {
    test_rng::reset();
    test_rng::enqueue_bytes(PULL_CHALLENGE, HMAC_SIZE);
    test_rng::enqueue_bytes(VERIFY_CHALLENGE, HMAC_SIZE);
  }
  ~RngGuard() { test_rng::reset(); }
};

}  // namespace

TEST(PairingKeyPull, RequestFrameShape) {
  IoFrame f{};
  ASSERT_TRUE(create_launch_key_transfer(f, test::OWN_ID, DEVICE_ID, PULL_CHALLENGE));
  EXPECT_EQ(f.cmd, CMD_LAUNCH_KEY_TRANSFER);
  EXPECT_EQ(f.data_len, HMAC_SIZE);
  EXPECT_EQ(0, memcmp(f.data, PULL_CHALLENGE, HMAC_SIZE));
  EXPECT_TRUE(f.ctrl0 & CTRL0_START);
  EXPECT_FALSE(f.ctrl0 & CTRL0_END);
  EXPECT_TRUE(f.ctrl1 & CTRL1_PRIORITY);
  EXPECT_FALSE(f.ctrl1 & CTRL1_LOW_POWER);
}

TEST(PairingKeyPull, RecoverKeyRoundTrip) {
  const IoFrame reply = pulled_key_frame(test::OWN_ID, OTHER_KEY);
  uint8_t out[AES_KEY_SIZE];
  ASSERT_TRUE(recover_key_from_pull(pull_request(test::OWN_ID), reply.data, out));
  EXPECT_EQ(0, memcmp(out, OTHER_KEY, AES_KEY_SIZE));
}

TEST(PairingKeyPull, DeviceSendingOurKeyIsPaired) {
  RngGuard rng;
  TestableComponent comp;
  MockRadio radio;
  setup(comp, radio);
  pairing::PairingContext context = context_for_device();

  radio.queue_rx(to_packet(pulled_key_frame(comp.node_id_, comp.system_key_)));

  EXPECT_EQ(comp.pairing_engine_.run_key_pull_phase_(context), PairingEngine::KeyPullResult::OUR_KEY);
  EXPECT_EQ(sent_commands(radio), (std::vector<uint8_t>{CMD_LAUNCH_KEY_TRANSFER})) << "our own key needs no challenge";
}

TEST(PairingKeyPull, ProvenForeignKeyIsReportedNotAdopted) {
  RngGuard rng;
  TestableComponent comp;
  MockRadio radio;
  setup(comp, radio);
  pairing::PairingContext context = context_for_device();
  uint8_t key_before[AES_KEY_SIZE];
  memcpy(key_before, comp.system_key_, AES_KEY_SIZE);

  const IoFrame reply = pulled_key_frame(comp.node_id_, OTHER_KEY);
  radio.queue_rx(to_packet(reply));
  radio.queue_rx(to_packet(proof_frame(comp.node_id_, reply, OTHER_KEY)));

  EXPECT_EQ(comp.pairing_engine_.run_key_pull_phase_(context), PairingEngine::KeyPullResult::OTHER_KEY);
  EXPECT_EQ(sent_commands(radio), (std::vector<uint8_t>{CMD_LAUNCH_KEY_TRANSFER, CMD_CHALLENGE_REQ}));
  EXPECT_EQ(0, memcmp(comp.system_key_, key_before, AES_KEY_SIZE)) << "a pulled key is never adopted automatically";
}

TEST(PairingKeyPull, UnprovenForeignKeyFails) {
  RngGuard rng;
  TestableComponent comp;
  MockRadio radio;
  setup(comp, radio);
  pairing::PairingContext context = context_for_device();

  radio.queue_rx(to_packet(pulled_key_frame(comp.node_id_, OTHER_KEY)));
  // No 0x3D follows.

  EXPECT_EQ(comp.pairing_engine_.run_key_pull_phase_(context), PairingEngine::KeyPullResult::FAILED);
}

TEST(PairingKeyPull, WrongChallengeAnswerLeavesTheKeyUnproven) {
  RngGuard rng;
  TestableComponent comp;
  MockRadio radio;
  setup(comp, radio);
  pairing::PairingContext context = context_for_device();
  uint8_t key_before[AES_KEY_SIZE];
  memcpy(key_before, comp.system_key_, AES_KEY_SIZE);

  const IoFrame reply = pulled_key_frame(comp.node_id_, OTHER_KEY);
  const uint8_t unrelated_key[AES_KEY_SIZE] = {0x77};
  radio.queue_rx(to_packet(reply));
  radio.queue_rx(to_packet(proof_frame(comp.node_id_, reply, unrelated_key)));

  EXPECT_EQ(comp.pairing_engine_.run_key_pull_phase_(context), PairingEngine::KeyPullResult::FAILED);
  EXPECT_TRUE(comp.pairing_telemetry_.key_pull_unproven());
  EXPECT_EQ(0, memcmp(comp.system_key_, key_before, AES_KEY_SIZE));
}

TEST(PairingKeyPull, RefusalEndsThePull) {
  TestableComponent comp;
  MockRadio radio;
  setup(comp, radio);
  pairing::PairingContext context = context_for_device();

  radio.queue_rx(to_packet(error_frame(comp.node_id_, RESULT_COMMAND_UNKNOWN)));

  EXPECT_EQ(comp.pairing_engine_.run_key_pull_phase_(context), PairingEngine::KeyPullResult::FAILED);
  EXPECT_TRUE(context.key_refused);
  EXPECT_EQ(sent_commands(radio), (std::vector<uint8_t>{CMD_LAUNCH_KEY_TRANSFER})) << "no retry after a refusal";
}

// ============================================================================
// Advisor
// ============================================================================

namespace {

uint8_t advise(PairingTelemetry &telemetry, advisor::PairingAdvice out[advisor::PAIRING_ADVICE_MAX]) {
  return advisor::analyze_pairing_telemetry(telemetry, test::OWN_ID, out);
}

}  // namespace

TEST(PairingKeyAdvice, CommandUnknownSuggestsPull) {
  PairingTelemetry telemetry;
  telemetry.begin();
  telemetry.set_discovered_device(DEVICE_ID, DeviceType::HORIZONTAL_AWNING);
  telemetry.set_key_refusal(RESULT_COMMAND_UNKNOWN);
  telemetry.set_outcome(PairingOutcome::KEY_TRANSFER_REFUSED);
  advisor::PairingAdvice out[advisor::PAIRING_ADVICE_MAX];
  const uint8_t n = advise(telemetry, out);
  ASSERT_GE(n, 1u);
  EXPECT_EQ(out[n - 1].code, advisor::PairingAdviceCode::KEY_PUSH_NOT_ACCEPTED);
  EXPECT_NE(advisor::pairing_advice_message(out[n - 1]).find("pairing_key_transfer: pull"), std::string::npos);
}

TEST(PairingKeyAdvice, OtherRefusalNamesTheCode) {
  PairingTelemetry telemetry;
  telemetry.begin();
  telemetry.set_discovered_device(DEVICE_ID, DeviceType::HORIZONTAL_AWNING);
  telemetry.set_key_refusal(RESULT_COMMAND_REJECTED);
  telemetry.set_outcome(PairingOutcome::KEY_TRANSFER_REFUSED);
  advisor::PairingAdvice out[advisor::PAIRING_ADVICE_MAX];
  const uint8_t n = advise(telemetry, out);
  ASSERT_GE(n, 1u);
  EXPECT_EQ(out[n - 1].code, advisor::PairingAdviceCode::KEY_TRANSFER_REFUSED);
  EXPECT_NE(advisor::pairing_advice_message(out[n - 1]).find("COMMAND_REJECTED"), std::string::npos);
}

TEST(PairingKeyAdvice, NoPullSuggestionAfterAPull) {
  PairingTelemetry telemetry;
  telemetry.begin();
  telemetry.set_discovered_device(DEVICE_ID, DeviceType::REMOTE_CONTROLLER);
  telemetry.set_key_pull_attempted();
  telemetry.set_outcome(PairingOutcome::KEY_EXCHANGE_FAILED);
  advisor::PairingAdvice out[advisor::PAIRING_ADVICE_MAX];
  const uint8_t n = advise(telemetry, out);
  for (uint8_t i = 0; i < n; i++)
    EXPECT_NE(out[i].code, advisor::PairingAdviceCode::KEY_PUSH_NOT_ACCEPTED);
}

TEST(PairingKeyAdvice, RefusedPullSaysThePullIsNotOffered) {
  PairingTelemetry telemetry;
  telemetry.begin();
  telemetry.set_discovered_device(DEVICE_ID, DeviceType::HORIZONTAL_AWNING);
  telemetry.set_key_pull_attempted();
  telemetry.set_key_refusal(RESULT_COMMAND_UNKNOWN);
  telemetry.set_outcome(PairingOutcome::KEY_TRANSFER_REFUSED);
  advisor::PairingAdvice out[advisor::PAIRING_ADVICE_MAX];
  const uint8_t n = advise(telemetry, out);
  ASSERT_GE(n, 1u);
  EXPECT_EQ(out[n - 1].code, advisor::PairingAdviceCode::KEY_PULL_NOT_OFFERED);
  const std::string message = advisor::pairing_advice_message(out[n - 1]);
  EXPECT_NE(message.find("does not hand out its key"), std::string::npos);
  EXPECT_EQ(message.find("Scan Paired Devices"), std::string::npos) << "push advice does not apply to a pull";
}

TEST(PairingKeyAdvice, UnprovenPullKeyWarnsNotToUseIt) {
  PairingTelemetry telemetry;
  telemetry.begin();
  telemetry.set_discovered_device(DEVICE_ID, DeviceType::REMOTE_CONTROLLER);
  telemetry.set_key_pull_attempted();
  telemetry.set_key_pull_unproven();
  telemetry.set_outcome(PairingOutcome::KEY_EXCHANGE_FAILED);
  advisor::PairingAdvice out[advisor::PAIRING_ADVICE_MAX];
  const uint8_t n = advise(telemetry, out);
  ASSERT_GE(n, 1u);
  EXPECT_EQ(out[n - 1].code, advisor::PairingAdviceCode::KEY_PULL_UNPROVEN);
  EXPECT_STREQ(advisor::pairing_advice_code_name(out[n - 1].code), "key_pull_unproven");
}

TEST(PairingKeyAdvice, ReceivedKeySaysHowToUseIt) {
  PairingTelemetry telemetry;
  telemetry.begin();
  telemetry.set_discovered_device(DEVICE_ID, DeviceType::REMOTE_CONTROLLER);
  telemetry.set_key_pull_attempted();
  telemetry.set_outcome(PairingOutcome::KEY_RECEIVED);
  advisor::PairingAdvice out[advisor::PAIRING_ADVICE_MAX];
  const uint8_t n = advise(telemetry, out);
  ASSERT_GE(n, 1u);
  EXPECT_EQ(out[n - 1].code, advisor::PairingAdviceCode::PULLED_KEY_RECEIVED);
  EXPECT_STREQ(advisor::pairing_advice_code_name(out[n - 1].code), "pulled_key_received");
  const std::string message = advisor::pairing_advice_message(out[n - 1]);
  EXPECT_NE(message.find("system_key:"), std::string::npos);
  EXPECT_NE(message.find("Scan Paired Devices"), std::string::npos);
}

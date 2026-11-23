/// @file pairing_key_step_test.cpp
/// @brief Pairing key step: explicit refusals end the attempt, the key-held check after an
/// unconfirmed exchange, the experimental key pull (0x38), and the advice they produce.

#include "hub_core.h"
#include "hub_pairing.h"
#include "log_helpers.h"
#include "pairing_advisor.h"
#include "pairing_engine.h"
#include "proto_codecs.h"
#include "proto_commands.h"
#include "proto_crypto.h"
#include "proto_frame.h"

#include "esp_random.h"
#include "test_bands.h"
#include "pairing_test_support.h"
#include "test_helpers.h"
#include "stubs/radio_test_common.h"
#include "support/hold_seam.h"
#include "support/log_capture.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <vector>

using namespace esphome::home_io_control;
using namespace pairing_test;

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
  using IOHomeControlComponent::registry_;

  RecentOneWayPairingSighting recent_oneway_pairing_sighting_{};
  test::TestablePairingEngine pairing_engine_{
      &radio_,          node_id_,  system_key_,        &tuning_,
      exchange_engine_, registry_, pairing_telemetry_, recent_oneway_pairing_sighting_};
};

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

TEST(PairingKeyStep, AtlanticFlowWithAForcedPushEndsRefusedWithPullAdvice) {
  TestableComponent comp;
  MockRadioSX1262 radio;
  setup(comp, radio);
  comp.tuning_.pairing_key_transfer = KeyTransferMode::PUSH;  // auto would pull from a remote controller

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
// Which key the transfer carries
// ============================================================================

namespace {

/// Not challenge_frame()'s: recovering the pushed key needs these exact bytes.
constexpr uint8_t PUSH_CHALLENGE[HMAC_SIZE] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};

constexpr uint8_t DEVICE_OWN_KEY[AES_KEY_SIZE] = {0x5E, 0x11, 0xA7, 0x04, 0x29, 0xC3, 0x8B, 0x62,
                                                  0xD0, 0x4F, 0x96, 0x1A, 0x7C, 0xE5, 0x38, 0xB4};

/// The device's side of the exchange: a challenge, then the confirm.
void queue_push_answers(MockRadio &radio, const uint8_t own[NODE_ID_SIZE]) {
  radio.queue_rx(to_packet(device_frame(own, CMD_CHALLENGE_REQ, PUSH_CHALLENGE, HMAC_SIZE)));
  radio.queue_rx(to_packet(device_frame(own, CMD_KEY_CONFIRM, nullptr, 0)));
}

/// The key the one CMD_KEY_TRANSFER we sent carries.
std::vector<uint8_t> pushed_key(const MockRadio &radio) {
  const std::vector<IoFrame> transfers = sent_frames_of(radio, CMD_KEY_TRANSFER);
  if (transfers.size() != 1 || transfers[0].data_len != AES_KEY_SIZE)
    return {};
  uint8_t recovered[AES_KEY_SIZE]{};
  if (!recover_system_key_from_transfer(transfers[0].data, PUSH_CHALLENGE, recovered))
    return {};
  return {recovered, recovered + AES_KEY_SIZE};
}

}  // namespace

TEST(PairingKeyStep, TheTransferCarriesTheHubKeyForADeviceWithNoKeyOfItsOwn) {
  TestableComponent comp;
  MockRadio radio;
  setup(comp, radio);
  pairing::PairingContext context = context_for_device();
  queue_push_answers(radio, comp.node_id_);

  ASSERT_TRUE(comp.pairing_engine_.run_key_exchange_phase_(context));
  EXPECT_EQ(pushed_key(radio), std::vector<uint8_t>(test::TEST_SYSTEM_KEY, test::TEST_SYSTEM_KEY + AES_KEY_SIZE));
}

TEST(PairingKeyStep, TheTransferCarriesTheKeyRegisteredForTheDevice) {
  TestableComponent comp;
  MockRadio radio;
  setup(comp, radio);
  DeviceConfig cfg{};
  cfg.has_system_key = true;
  memcpy(cfg.system_key, DEVICE_OWN_KEY, AES_KEY_SIZE);
  comp.registry_.add(node_id_to_string(DEVICE_ID), cfg);
  comp.exchange_engine_.set_device_key_provider([&comp](const uint8_t *node) -> const uint8_t * {
    const IoDevice *dev = comp.registry_.get(node_id_to_string(node));
    return dev != nullptr && dev->has_system_key ? dev->system_key : nullptr;
  });

  pairing::PairingContext context = context_for_device();
  queue_push_answers(radio, comp.node_id_);

  ASSERT_TRUE(comp.pairing_engine_.run_key_exchange_phase_(context));
  EXPECT_EQ(pushed_key(radio), std::vector<uint8_t>(DEVICE_OWN_KEY, DEVICE_OWN_KEY + AES_KEY_SIZE));
}

// ============================================================================
// Quiet windows of the pairing steps (ADR 0111)
// ============================================================================

TEST(PairingQuietWindow, EveryTransmitAndReplyOfAWholePairingAttemptSitsInAHeldWindow) {
  // Discovery, discover-confirm, key-init, the key transfer's confirm wait and the key-held check's exchange.
  TestableComponent comp;
  MockRadioSX1262 radio;
  setup(comp, radio);
  comp.tuning_.pairing_key_transfer = KeyTransferMode::PUSH;
  test::HoldSeam seam(radio);

  radio.queue_rx(to_packet(atlantic_discovery_reply(comp.node_id_)));
  IoFrame ack{};
  ASSERT_TRUE(create_discover_confirm_ack(ack, DEVICE_ID, comp.node_id_));
  radio.queue_rx(to_packet(ack));
  radio.queue_rx(to_packet(challenge_frame(comp.node_id_)));
  radio.queue_rx(to_packet(error_frame(comp.node_id_, RESULT_COMMAND_UNKNOWN)));

  EXPECT_FALSE(comp.discover_and_pair());

  EXPECT_GE(seam.depth_at_send.size(), 4u) << "0x28, 0x2C, 0x31, 0x32 and the key-held check's reads";
  EXPECT_EQ(seam.depth_at_deliver.size(), 4u) << "discovery reply, 0x2D, 0x3C, the refusal";
  EXPECT_TRUE(seam.held_at_every_send_and_delivery())
      << "nothing may be logged while pairing waits for a reply it provoked";
  EXPECT_TRUE(test::frame_log_released());
}

TEST(PairingQuietWindow, KeyConfirmWaitHoldsAcrossTheAnswerAndLogsTheChallengeLineAfterIt) {
  TestableComponent comp;
  MockRadioSX1262 radio;
  setup(comp, radio);
  pairing::PairingContext context = context_for_device();
  test::LogCapture log;
  std::vector<size_t> at_send;
  std::vector<size_t> at_deliver;
  test::HoldSeam seam(
      radio, [&] { at_send.push_back(log.lines().size()); }, [&] { at_deliver.push_back(log.lines().size()); });

  radio.queue_rx(to_packet(challenge_frame(comp.node_id_)));  // to the 0x31
  radio.queue_rx(to_packet(challenge_frame(comp.node_id_)));  // to the 0x32: a second challenge
  radio.queue_rx(to_packet(device_frame(comp.node_id_, CMD_KEY_CONFIRM, nullptr, 0)));  // to our 0x3D

  ASSERT_TRUE(comp.pairing_engine_.run_key_exchange_phase_(context));

  EXPECT_EQ(sent_commands(radio), (std::vector<uint8_t>{CMD_KEY_INIT, CMD_KEY_TRANSFER, CMD_CHALLENGE_RESP}));
  ASSERT_EQ(at_send.size(), 3u);
  ASSERT_EQ(at_deliver.size(), 3u);
  for (size_t i = 0; i < 3; i++)
    EXPECT_EQ(at_deliver[i], at_send[i]) << "a line was logged between transmit " << i << " and its reply";
  EXPECT_EQ(at_send[1], at_deliver[0]) << "a line was logged between the key-init's challenge and the 0x32 (#163)";
  EXPECT_EQ(at_send[2], at_deliver[1]) << "a line was logged between the challenge and the 0x3D (issue #163)";
  EXPECT_TRUE(seam.quiet_from_delivery_to_next_send(0)) << "the hold was released between the key-init's challenge and "
                                                           "the 0x32, which prints the held lines there (#163)";
  EXPECT_TRUE(seam.quiet_from_delivery_to_next_send(1))
      << "the hold was released between the challenge and the 0x3D, which prints the held lines there (#163)";
  EXPECT_TRUE(seam.held_at_every_send_and_delivery());
  EXPECT_TRUE(test::frame_log_released());

  size_t challenged_line = log.lines().size();
  for (size_t i = 0; i < log.lines().size(); i++) {
    if (log.lines()[i].format.rfind("Key transfer: device challenged the key transfer", 0) == 0)
      challenged_line = i;
  }
  ASSERT_LT(challenged_line, log.lines().size()) << "the line is still logged";
  EXPECT_GE(challenged_line, at_deliver.back()) << "the line follows the wait for the confirm, not the 0x3D";
}

TEST(PairingQuietWindow, KeyInitChallengeLineFollowsTheWaitForTheConfirm) {
  TestableComponent comp;
  MockRadioSX1262 radio;
  setup(comp, radio);
  pairing::PairingContext context = context_for_device();
  test::LogCapture log;
  size_t at_last_deliver = 0;
  test::HoldSeam seam(radio, {}, [&] { at_last_deliver = log.lines().size(); });

  radio.queue_rx(to_packet(challenge_frame(comp.node_id_)));                      // to the 0x31
  radio.queue_rx(to_packet(error_frame(comp.node_id_, RESULT_COMMAND_UNKNOWN)));  // to the 0x32

  EXPECT_FALSE(comp.pairing_engine_.run_key_exchange_phase_(context));

  size_t challenge_line = log.lines().size();
  for (size_t i = 0; i < log.lines().size(); i++) {
    if (log.lines()[i].format.rfind("Challenge (0x3C) received", 0) == 0)
      challenge_line = i;
  }
  ASSERT_LT(challenge_line, log.lines().size()) << "the line is still logged";
  EXPECT_GE(challenge_line, at_last_deliver) << "the line must not sit between the challenge and the 0x32";
  EXPECT_TRUE(test::frame_log_released());
}

TEST(PairingQuietWindow, ResentKeyInitHoldsAcrossTheAnswerToItsChallenge) {
  TestableComponent comp;
  MockRadioSX1262 radio;
  setup(comp, radio);
  pairing::PairingContext context = context_for_device();
  ASSERT_TRUE(create_key_init(context.key_init, comp.node_id_, context.device.node_id));
  test::HoldSeam seam(radio);

  radio.queue_rx(to_packet(challenge_frame(comp.node_id_)));                      // to the re-sent 0x31
  radio.queue_rx(to_packet(error_frame(comp.node_id_, RESULT_COMMAND_UNKNOWN)));  // to the 0x32

  EXPECT_FALSE(comp.pairing_engine_.resend_key_init_(context, 1));

  EXPECT_EQ(sent_commands(radio), (std::vector<uint8_t>{CMD_KEY_INIT, CMD_KEY_TRANSFER}));
  EXPECT_TRUE(seam.quiet_from_delivery_to_next_send(0))
      << "the hold was released between the re-sent key-init's challenge and the 0x32 (#163)";
  EXPECT_TRUE(seam.held_at_every_send_and_delivery());
  EXPECT_TRUE(test::frame_log_released());
}

TEST(PairingQuietWindow, ReleasesTheHoldWhenTheKeyInitNeverGetsAChallenge) {
  TestableComponent comp;
  MockRadioSX1262 radio;
  setup(comp, radio);
  pairing::PairingContext context = context_for_device();

  EXPECT_FALSE(comp.pairing_engine_.run_key_exchange_phase_(context));
  EXPECT_TRUE(test::frame_log_released());
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
// Direction: pairing_key_transfer auto | push | pull
// ============================================================================

TEST(PairingKeyDirection, AutoPushesToAnActuatorAndNeverFallsBackToAPull) {
  TestableComponent comp;
  MockRadio radio;
  setup(comp, radio);

  // A roller shutter (type 2, packed 00 80) answers discovery; the key step then gets a refusal.
  const uint8_t payload[] = {0x00, 0x80, 0xB6, 0xBD, 0xFB, 0x02, 0xCC, 0x00, 0x00};
  IoFrame reply{};
  init_frame(reply, true, true, true, false);
  set_dst(reply, comp.node_id_);
  set_src(reply, DEVICE_ID);
  set_cmd(reply, CMD_DISCOVER_RESP, payload, sizeof(payload));
  radio.queue_rx(to_packet(reply));
  IoFrame ack{};
  ASSERT_TRUE(create_discover_confirm_ack(ack, DEVICE_ID, comp.node_id_));
  radio.queue_rx(to_packet(ack));
  radio.queue_rx(to_packet(challenge_frame(comp.node_id_)));
  radio.queue_rx(to_packet(error_frame(comp.node_id_, RESULT_COMMAND_UNKNOWN)));

  EXPECT_FALSE(comp.discover_and_pair());
  const std::vector<uint8_t> cmds = sent_commands(radio);
  EXPECT_NE(std::find(cmds.begin(), cmds.end(), CMD_KEY_INIT), cmds.end()) << "an actuator is pushed a key";
  EXPECT_EQ(std::find(cmds.begin(), cmds.end(), CMD_LAUNCH_KEY_TRANSFER), cmds.end())
      << "a refusal repeats; there is no switch to the pull inside the attempt";
}

// ============================================================================
// Pull -> roll-call under the pulled key -> registered devices
// ============================================================================

namespace {

/// Queue discovery, discover-confirm ack and a pull of `key` whose sender never answers our challenges
/// (the Atlantic flow up to the key, issue #215's Freebox).
void queue_unproven_pull(MockRadio &radio, const uint8_t dst[NODE_ID_SIZE], const uint8_t key[AES_KEY_SIZE]) {
  radio.queue_rx(to_packet(atlantic_discovery_reply(dst)));
  IoFrame ack{};
  create_discover_confirm_ack(ack, DEVICE_ID, dst);
  radio.queue_rx(to_packet(ack));
  radio.queue_rx(to_packet(pulled_key_frame(dst, key)));
}

/// Queue discovery, discover-confirm ack and a proven pull of `key` (the Atlantic flow up to the key).
void queue_proven_pull(MockRadio &radio, const uint8_t dst[NODE_ID_SIZE], const uint8_t key[AES_KEY_SIZE]) {
  queue_unproven_pull(radio, dst, key);
  radio.queue_rx(to_packet(proof_frame(dst, pulled_key_frame(dst, key), key)));
}

/// Sends before the first roll-call 0x2A of a foreign-key pull whose heater probe finds nothing: 0x28,
/// 0x2C, 0x38, the 0x3C(s) that challenge the key, and three unanswered 0x20 probe tries.
constexpr int SENDS_BEFORE_ROLL_CALL_PROVEN = 7;    // one 0x3C, answered
constexpr int SENDS_BEFORE_ROLL_CALL_UNPROVEN = 9;  // three 0x3C, none answered
/// Sends before the heater probe's 0x20 of an unproven pull: 0x28, 0x2C, 0x38, three 0x3C.
constexpr int SENDS_BEFORE_PROBE_UNPROVEN = 6;

/// Queue `frame` to arrive only once the hub has made its `sends_before + 1`-th send.
void queue_rx_after_sends(MockRadio &radio, int sends_before, const IoFrame &frame) {
  radio.queue_rx_hold_until_sent(sends_before + 1);
  radio.queue_rx(to_packet(frame));
}

std::string key_hex(const uint8_t key[AES_KEY_SIZE]) { return detail::format_key_hex(key); }

/// Queue the heater's roll-call reply behind the direct probe of its address: the radio stays silent
/// until the roll-call's first 0x2A goes out. Before it go 0x28, 0x2C, 0x38, the 0x3C that proves a
/// foreign key, and three unanswered 0x20 probe tries.
void queue_roll_call_reply_after_probe(MockRadio &radio, const uint8_t dst[NODE_ID_SIZE], bool foreign_key) {
  queue_rx_after_sends(radio, foreign_key ? SENDS_BEFORE_ROLL_CALL_PROVEN : SENDS_BEFORE_ROLL_CALL_PROVEN - 1,
                       heater_roll_call_reply(dst));
}

/// Queue `reply` behind the direct probe of a foreign-key pull, as queue_roll_call_reply_after_probe().
void queue_roll_call_reply_after_probe(MockRadio &radio, const IoFrame &reply) {
  queue_rx_after_sends(radio, SENDS_BEFORE_ROLL_CALL_PROVEN, reply);
}

/// A MockRadio that calls `on_probe_read` each time a register read (0x20) goes out, so a test can
/// look at the registry while the heater probe's exchange is running, not only after it.
class ProbeWatchingRadio : public MockRadio {
 public:
  std::function<void()> on_probe_read;

  TxResult send_packet(const uint8_t *data, uint8_t len, const RadioTxConfig &tx_config) override {
    IoFrame frame{};
    if (on_probe_read && parse(data, len, frame) && frame.cmd == CMD_WRITE_PRIVATE)
      on_probe_read();
    return MockRadio::send_packet(data, len, tx_config);
  }
};

}  // namespace

TEST(PairingKeyPullFlow, ARemoteControllerThatRefusesThePullNeverGetsThePushSuggestion) {
  RngGuard rng;
  TestableComponent comp;
  MockRadio radio;
  setup(comp, radio);
  radio.queue_rx(to_packet(atlantic_discovery_reply(comp.node_id_)));
  IoFrame ack{};
  ASSERT_TRUE(create_discover_confirm_ack(ack, DEVICE_ID, comp.node_id_));
  radio.queue_rx(to_packet(ack));
  radio.queue_rx(to_packet(error_frame(comp.node_id_, RESULT_COMMAND_UNKNOWN)));

  EXPECT_FALSE(comp.discover_and_pair());

  const std::string result = comp.pairing_telemetry().result_sensor_string();
  EXPECT_NE(result.find("key_pull_not_offered"), std::string::npos) << result;
  EXPECT_EQ(result.find("key_push_not_accepted"), std::string::npos) << result;
}

TEST(PairingKeyPullFlow, AutoPullsAnAgiliaThenRegistersTheHeaterWithItsKey) {
  RngGuard rng;
  TestableComponent comp;
  MockRadio radio;
  setup(comp, radio);
  queue_proven_pull(radio, comp.node_id_, OTHER_KEY);
  queue_roll_call_reply_after_probe(radio, comp.node_id_, /*foreign_key=*/true);

  EXPECT_TRUE(comp.discover_and_pair());

  const IoDevice *heater = comp.get_device("B6BDFC");
  ASSERT_NE(heater, nullptr) << "the responder, not the pairing node, is registered";
  EXPECT_EQ(heater->type, DeviceType::ELECTRICAL_HEATER);
  EXPECT_EQ(heater->subtype, 1);
  ASSERT_TRUE(heater->has_system_key);
  EXPECT_EQ(0, memcmp(heater->system_key, OTHER_KEY, AES_KEY_SIZE));
  EXPECT_EQ(comp.get_device("B6BDFB"), nullptr) << "the node that handed the key over is not a controllable device";
  EXPECT_EQ(comp.pairing_telemetry().outcome(), PairingOutcome::PAIRED);

  const std::vector<uint8_t> cmds = sent_commands(radio);
  EXPECT_EQ(std::count(cmds.begin(), cmds.end(), CMD_KEY_INIT), 0) << "auto pulled: no push";
  EXPECT_EQ(std::count(cmds.begin(), cmds.end(), CMD_DISCOVER_SPE_REQ), 6)
      << "one roll-call sweep under the pulled key";
  EXPECT_EQ(comp.system_key_[0], test::TEST_SYSTEM_KEY[0]);
  EXPECT_EQ(0, memcmp(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE)) << "the hub key is never replaced";
}

TEST(PairingKeyPullFlow, TheRollCallIsSignedWithThePulledKey) {
  RngGuard rng;
  TestableComponent comp;
  MockRadio radio;
  setup(comp, radio);
  queue_proven_pull(radio, comp.node_id_, OTHER_KEY);

  comp.discover_and_pair();

  size_t checked = 0;
  for (const auto &raw : radio.get_sent_data()) {
    IoFrame frame{};
    if (!parse(raw.data(), static_cast<uint8_t>(raw.size()), frame) || frame.cmd != CMD_DISCOVER_SPE_REQ)
      continue;
    uint8_t expected[HMAC_SIZE];
    const uint8_t command = CMD_DISCOVER_SPE_REQ;
    ASSERT_TRUE(crypto::create_hmac(&command, 1, frame.data, OTHER_KEY, expected));
    EXPECT_EQ(0, memcmp(expected, frame.data + HMAC_SIZE, HMAC_SIZE));
    checked++;
  }
  EXPECT_EQ(checked, 6u);
}

TEST(PairingKeyPullFlow, LastPairingResultCarriesNoKey) {
  RngGuard rng;
  TestableComponent comp;
  MockRadio radio;
  setup(comp, radio);
  queue_proven_pull(radio, comp.node_id_, OTHER_KEY);
  queue_roll_call_reply_after_probe(radio, comp.node_id_, /*foreign_key=*/true);

  ASSERT_TRUE(comp.discover_and_pair());

  const std::string result = comp.pairing_telemetry().result_sensor_string();
  EXPECT_EQ(result.find(key_hex(OTHER_KEY)), std::string::npos) << result;
  EXPECT_NE(result.find("outcome=paired"), std::string::npos) << result;
  EXPECT_NE(result.find("node=B6BDFB"), std::string::npos) << "node= stays the discovered node: " << result;
  const std::string tail = "; check=" + detail::format_key_check_value(OTHER_KEY);
  EXPECT_EQ(result.substr(result.size() - tail.size()), tail) << "a proven pulled key: " << result;
}

TEST(PairingKeyPullFlow, NoResponderEndsAsKeyReceivedAndRegistersNothing) {
  RngGuard rng;
  TestableComponent comp;
  MockRadio radio;
  setup(comp, radio);
  queue_proven_pull(radio, comp.node_id_, OTHER_KEY);

  EXPECT_FALSE(comp.discover_and_pair());

  EXPECT_EQ(comp.pairing_telemetry().outcome(), PairingOutcome::KEY_RECEIVED);
  const std::string result = comp.pairing_telemetry().result_sensor_string();
  EXPECT_NE(result.find("pulled_key_received"), std::string::npos);
  const std::string tail = "; check=" + detail::format_key_check_value(OTHER_KEY);
  EXPECT_EQ(result.substr(result.size() - tail.size()), tail) << result;
  EXPECT_EQ(comp.get_device("B6BDFC"), nullptr);
  EXPECT_EQ(comp.get_device("B6BDFB"), nullptr);
}

TEST(PairingKeyPullFlow, AnUnprovenKeyIsProvenByADeviceThatAnswersTheRollCall) {
  // Issue #215: a giver that sends its key but never answers our 0x3C. A device answers a roll-call
  // only under the key it holds, so its answer proves the key.
  RngGuard rng;
  TestableComponent comp;
  MockRadio radio;
  setup(comp, radio);
  queue_unproven_pull(radio, comp.node_id_, OTHER_KEY);
  queue_rx_after_sends(radio, SENDS_BEFORE_ROLL_CALL_UNPROVEN, heater_roll_call_reply(comp.node_id_));

  EXPECT_TRUE(comp.discover_and_pair());

  EXPECT_EQ(sent_frames_of(radio, CMD_CHALLENGE_REQ).size(), 3u);
  const IoDevice *heater = comp.get_device("B6BDFC");
  ASSERT_NE(heater, nullptr);
  ASSERT_TRUE(heater->has_system_key);
  EXPECT_EQ(0, memcmp(heater->system_key, OTHER_KEY, AES_KEY_SIZE));
  EXPECT_EQ(comp.pairing_telemetry().outcome(), PairingOutcome::PAIRED);
  const std::string result = comp.pairing_telemetry().result_sensor_string();
  EXPECT_EQ(result.find(key_hex(OTHER_KEY)), std::string::npos);
  const std::string tail = "; check=" + detail::format_key_check_value(OTHER_KEY);
  EXPECT_EQ(result.substr(result.size() - tail.size()), tail) << "a device's answer proved the key: " << result;
  EXPECT_EQ(0, memcmp(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE)) << "the hub key is never replaced";
}

TEST(PairingKeyPullFlow, AnUnprovenKeyWithNoDeviceAnsweringFailsAndRegistersNothing) {
  RngGuard rng;
  TestableComponent comp;
  MockRadio radio;
  setup(comp, radio);
  queue_unproven_pull(radio, comp.node_id_, OTHER_KEY);

  EXPECT_FALSE(comp.discover_and_pair());

  EXPECT_EQ(sent_frames_of(radio, CMD_DISCOVER_SPE_REQ).size(), 6u) << "the roll-call ran under the candidate key";
  EXPECT_EQ(comp.pairing_telemetry().outcome(), PairingOutcome::KEY_EXCHANGE_FAILED);
  const std::string result = comp.pairing_telemetry().result_sensor_string();
  EXPECT_NE(result.find("key_pull_unproven"), std::string::npos) << result;
  EXPECT_EQ(result.find(key_hex(OTHER_KEY)), std::string::npos) << result;
  EXPECT_EQ(result.substr(result.size() - 9), "; check=-") << "nothing proved the key: " << result;
  EXPECT_EQ(comp.get_device("B6BDFC"), nullptr);
  EXPECT_EQ(comp.get_device("B6BDFB"), nullptr);
}

TEST(PairingKeyPullFlow, ARollCallAnswerToAnotherControllerDoesNotProveAnUnprovenKey) {
  // The giver may run its own roll-call at the same time; only 0x2B frames addressed to the hub count.
  RngGuard rng;
  TestableComponent comp;
  MockRadio radio;
  setup(comp, radio);
  queue_unproven_pull(radio, comp.node_id_, OTHER_KEY);
  const uint8_t other_controller[NODE_ID_SIZE] = {0x12, 0x34, 0x56};
  queue_rx_after_sends(radio, SENDS_BEFORE_ROLL_CALL_UNPROVEN, heater_roll_call_reply(other_controller));

  EXPECT_FALSE(comp.discover_and_pair());

  EXPECT_EQ(comp.pairing_telemetry().outcome(), PairingOutcome::KEY_EXCHANGE_FAILED);
  EXPECT_EQ(comp.get_device("B6BDFC"), nullptr);
}

TEST(PairingKeyPullFlow, AnUnprovenKeyIsProvenByTheHeaterProbe) {
  // The heater's authenticated read succeeds only under the key it holds, so its answer proves the key
  // as a roll-call reply would.
  RngGuard rng;
  TestableComponent comp;
  MockRadio radio;
  setup(comp, radio);
  queue_unproven_pull(radio, comp.node_id_, OTHER_KEY);
  radio.queue_rx_hold_until_sent(SENDS_BEFORE_PROBE_UNPROVEN + 1);
  queue_heater_answers_probe(radio, comp.node_id_);

  EXPECT_TRUE(comp.discover_and_pair());

  EXPECT_EQ(comp.pairing_telemetry().outcome(), PairingOutcome::PAIRED);
  const IoDevice *heater = comp.get_device("B6BDFC");
  ASSERT_NE(heater, nullptr);
  ASSERT_TRUE(heater->has_system_key);
  EXPECT_EQ(0, memcmp(heater->system_key, OTHER_KEY, AES_KEY_SIZE));
  EXPECT_TRUE(sent_frames_of(radio, CMD_DISCOVER_SPE_REQ).empty()) << "the probe found it: no roll-call";
}

TEST(PairingKeyPullFlow, AnUnprovenKeyAnsweredOnlyByADeviceWithoutAPlatformEndsAsKeyReceived) {
  // The answer proves the key even though nothing can be registered: the report (with the key) is then
  // the result, as after a proven pull.
  RngGuard rng;
  TestableComponent comp;
  MockRadio radio;
  setup(comp, radio);
  queue_unproven_pull(radio, comp.node_id_, OTHER_KEY);
  queue_rx_after_sends(radio, SENDS_BEFORE_ROLL_CALL_UNPROVEN, heater_roll_call_reply(comp.node_id_, 0x05, 0x81));

  EXPECT_FALSE(comp.discover_and_pair());

  EXPECT_EQ(comp.pairing_telemetry().outcome(), PairingOutcome::KEY_RECEIVED);
  EXPECT_EQ(comp.get_device("B6BDFC"), nullptr);
  EXPECT_EQ(comp.pairing_telemetry().result_sensor_string().find(key_hex(OTHER_KEY)), std::string::npos);
}

TEST(PairingKeyPullFlow, AResponderOfATypeWithoutAPlatformIsNotRegistered) {
  RngGuard rng;
  TestableComponent comp;
  MockRadio radio;
  setup(comp, radio);
  queue_proven_pull(radio, comp.node_id_, OTHER_KEY);
  // Type 22 subtype 1 (packed 05 81): the Atlantic heat pump, which no platform drives yet. The probe of
  // the next address finds nothing, so the reply waits for the roll-call.
  queue_rx_after_sends(radio, SENDS_BEFORE_ROLL_CALL_PROVEN, heater_roll_call_reply(comp.node_id_, 0x05, 0x81));

  EXPECT_FALSE(comp.discover_and_pair());
  EXPECT_EQ(comp.pairing_telemetry().outcome(), PairingOutcome::KEY_RECEIVED);
  EXPECT_EQ(comp.get_device("B6BDFC"), nullptr);
}

TEST(PairingKeyPullFlow, AResponderTheRegistryAlreadyKnowsKeepsItsRecord) {
  RngGuard rng;
  TestableComponent comp;
  MockRadio radio;
  setup(comp, radio);
  DeviceConfig cfg;
  cfg.type = DeviceType::ELECTRICAL_HEATER;
  cfg.subtype = 1;
  comp.add_device("B6BDFC", cfg);
  queue_proven_pull(radio, comp.node_id_, OTHER_KEY);
  radio.queue_rx(to_packet(heater_roll_call_reply(comp.node_id_)));

  EXPECT_FALSE(comp.discover_and_pair()) << "nothing new was registered";
  EXPECT_FALSE(comp.get_device("B6BDFC")->has_system_key) << "the YAML record is left alone";
  EXPECT_TRUE(sent_frames_of(radio, CMD_WRITE_PRIVATE).empty()) << "a device the YAML declares is not probed";
}

// ============================================================================
// Pull -> direct read of the heater at the next address (no roll-call)
// ============================================================================

TEST(PairingKeyPullFlow, AnAgiliaHeaterThatAnswersTheProbeIsRegisteredWithoutARollCall) {
  RngGuard rng;
  TestableComponent comp;
  MockRadio radio;
  setup(comp, radio);
  queue_proven_pull(radio, comp.node_id_, OTHER_KEY);
  queue_heater_answers_probe(radio, comp.node_id_);

  EXPECT_TRUE(comp.discover_and_pair());

  EXPECT_EQ(comp.pairing_telemetry().outcome(), PairingOutcome::PAIRED);
  const IoDevice *heater = comp.get_device("B6BDFC");
  ASSERT_NE(heater, nullptr);
  EXPECT_EQ(heater->type, DeviceType::ELECTRICAL_HEATER);
  EXPECT_EQ(heater->subtype, SUBTYPE_HEATER_WITH_SETPOINT);
  ASSERT_TRUE(heater->has_system_key);
  EXPECT_EQ(0, memcmp(heater->system_key, OTHER_KEY, AES_KEY_SIZE));
  EXPECT_TRUE(heater->registered_by_pairing);
  EXPECT_TRUE(sent_frames_of(radio, CMD_DISCOVER_SPE_REQ).empty()) << "the probe found it: no roll-call";

  const std::vector<IoFrame> reads = sent_frames_of(radio, CMD_WRITE_PRIVATE);
  ASSERT_EQ(reads.size(), 1u);
  EXPECT_EQ(0, memcmp(reads[0].dst, HEATER_ID, NODE_ID_SIZE)) << "the address after the pairing node";
  const uint8_t max_setpoint_read[] = {0x0C, 0x60, 0x01, 0x02};
  ASSERT_EQ(reads[0].data_len, sizeof(max_setpoint_read));
  EXPECT_EQ(0, memcmp(reads[0].data, max_setpoint_read, sizeof(max_setpoint_read)));

  const std::vector<IoFrame> answers = sent_frames_of(radio, CMD_CHALLENGE_RESP);
  ASSERT_EQ(answers.size(), 1u);
  uint8_t expected[HMAC_SIZE];
  ASSERT_TRUE(challenge_resp_hmac(reads[0], HEATER_CHALLENGE, OTHER_KEY, expected));
  EXPECT_EQ(0, memcmp(answers[0].data, expected, HMAC_SIZE)) << "the probe is signed with the pulled key";
}

TEST(PairingKeyPullFlow, AHeaterThatDoesNotAnswerTheProbeLeavesTheRegistryUntouched) {
  RngGuard rng;
  TestableComponent comp;
  ProbeWatchingRadio radio;
  setup(comp, radio);
  queue_proven_pull(radio, comp.node_id_, OTHER_KEY);
  ASSERT_EQ(comp.registry_.size(), 0u);
  std::vector<size_t> sizes_during_read;
  radio.on_probe_read = [&]() { sizes_during_read.push_back(comp.registry_.size()); };

  EXPECT_FALSE(comp.discover_and_pair());

  EXPECT_EQ(sent_frames_of(radio, CMD_WRITE_PRIVATE).size(), 3u) << "sanity: the probe ran, three tries, unanswered";
  EXPECT_EQ(sizes_during_read, (std::vector<size_t>{0, 0, 0})) << "no candidate record is planted for the read";
  EXPECT_EQ(comp.registry_.size(), 0u) << "the probe plants no candidate and removes none";
  EXPECT_EQ(comp.get_device("B6BDFC"), nullptr);
}

TEST(PairingKeyPullFlow, AnUnansweredProbeKeepsTheRecordAnEarlierPairingLeft) {
  RngGuard rng;
  TestableComponent comp;
  ProbeWatchingRadio radio;
  setup(comp, radio);
  constexpr uint8_t EARLIER_KEY[AES_KEY_SIZE] = {0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
                                                 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F, 0x50};
  IoDevice earlier{};
  memcpy(earlier.node_id, HEATER_ID, NODE_ID_SIZE);
  earlier.type = DeviceType::ELECTRICAL_HEATER;
  earlier.subtype = 1;
  earlier.has_system_key = true;
  memcpy(earlier.system_key, EARLIER_KEY, AES_KEY_SIZE);
  earlier.registered_by_pairing = true;
  comp.registry_.put("B6BDFC", earlier);
  queue_proven_pull(radio, comp.node_id_, OTHER_KEY);
  uint8_t reads_with_earlier_key = 0;
  radio.on_probe_read = [&]() {
    const IoDevice *during = comp.get_device("B6BDFC");
    if (during != nullptr && memcmp(during->system_key, EARLIER_KEY, AES_KEY_SIZE) == 0)
      reads_with_earlier_key++;
  };

  EXPECT_FALSE(comp.discover_and_pair());

  EXPECT_EQ(sent_frames_of(radio, CMD_WRITE_PRIVATE).size(), 3u) << "sanity: the probe ran, unanswered";
  EXPECT_EQ(reads_with_earlier_key, 3) << "the earlier record is not replaced for the read";
  const IoDevice *kept = comp.get_device("B6BDFC");
  ASSERT_NE(kept, nullptr);
  EXPECT_EQ(0, memcmp(kept->system_key, EARLIER_KEY, AES_KEY_SIZE)) << "the earlier record keeps its own key";
  EXPECT_EQ(comp.registry_.size(), 1u);
}

TEST(PairingKeyPullFlow, AHeaterAwayFromTheNextAddressIsFoundByTheRollCall) {
  // As in tests/corpus/captures/pairing/atlantic_agilia_pairing_key_pull_heater_not_next_address.yaml:
  // after a connection reset the heater answers at an address unrelated to the pairing node.
  RngGuard rng;
  TestableComponent comp;
  MockRadio radio;
  setup(comp, radio);
  queue_proven_pull(radio, comp.node_id_, OTHER_KEY);
  constexpr uint8_t MOVED_HEATER[NODE_ID_SIZE] = {0xF5, 0x19, 0xDD};
  IoFrame reply = heater_roll_call_reply(comp.node_id_);
  set_src(reply, MOVED_HEATER);
  memcpy(reply.data + 2, MOVED_HEATER, NODE_ID_SIZE);  // the backbone repeats the source
  queue_roll_call_reply_after_probe(radio, reply);

  EXPECT_TRUE(comp.discover_and_pair());

  EXPECT_EQ(comp.pairing_telemetry().outcome(), PairingOutcome::PAIRED);
  const IoDevice *heater = comp.get_device("F519DD");
  ASSERT_NE(heater, nullptr);
  EXPECT_EQ(heater->type, DeviceType::ELECTRICAL_HEATER);
  ASSERT_TRUE(heater->has_system_key);
  EXPECT_EQ(0, memcmp(heater->system_key, OTHER_KEY, AES_KEY_SIZE));
  EXPECT_EQ(comp.get_device("B6BDFC"), nullptr) << "the silent next address is not registered";
  EXPECT_EQ(sent_frames_of(radio, CMD_WRITE_PRIVATE).size(), 3u) << "three tries at the next address first";
}

TEST(PairingKeyPullFlow, AHubKeyPullFindsTheHeaterByTheProbe) {
  // The radiator-1 case of issue #165, where the roll-call stayed silent.
  RngGuard rng;
  TestableComponent comp;
  MockRadio radio;
  setup(comp, radio);
  queue_proven_pull(radio, comp.node_id_, test::TEST_SYSTEM_KEY);
  queue_heater_answers_probe(radio, comp.node_id_);

  EXPECT_TRUE(comp.discover_and_pair());

  EXPECT_EQ(comp.pairing_telemetry().outcome(), PairingOutcome::PAIRED);
  const IoDevice *heater = comp.get_device("B6BDFC");
  ASSERT_NE(heater, nullptr);
  EXPECT_FALSE(heater->has_system_key) << "it holds the hub key";
  EXPECT_EQ(comp.get_device("B6BDFB"), nullptr);
  EXPECT_TRUE(sent_frames_of(radio, CMD_DISCOVER_SPE_REQ).empty());
}

TEST(PairingKeyPullFlow, ARemoteControllerOfAnotherMakerIsNotProbed) {
  RngGuard rng;
  TestableComponent comp;
  MockRadio radio;
  setup(comp, radio);
  // The AGILIA discovery reply with manufacturer 2 (Somfy) instead of 12.
  const uint8_t payload[] = {0xFF, 0xC0, 0xB6, 0xBD, 0xFB, 0x02, 0xCC, 0x00, 0x00};
  IoFrame reply{};
  init_frame(reply, true, true, true, false);
  set_dst(reply, comp.node_id_);
  set_src(reply, DEVICE_ID);
  set_cmd(reply, CMD_DISCOVER_RESP, payload, sizeof(payload));
  radio.queue_rx(to_packet(reply));
  IoFrame ack{};
  ASSERT_TRUE(create_discover_confirm_ack(ack, DEVICE_ID, comp.node_id_));
  radio.queue_rx(to_packet(ack));
  const IoFrame key_reply = pulled_key_frame(comp.node_id_, OTHER_KEY);
  radio.queue_rx(to_packet(key_reply));
  radio.queue_rx(to_packet(proof_frame(comp.node_id_, key_reply, OTHER_KEY)));

  comp.discover_and_pair();

  EXPECT_TRUE(sent_frames_of(radio, CMD_WRITE_PRIVATE).empty());
  EXPECT_EQ(sent_frames_of(radio, CMD_DISCOVER_SPE_REQ).size(), 6u) << "the roll-call is the only search";
}

TEST(PairingKeyPullFlow, AHubKeyPullFromARemoteControllerRollCallsForTheDevicesHoldingIt) {
  RngGuard rng;
  TestableComponent comp;
  MockRadio radio;
  setup(comp, radio);
  queue_proven_pull(radio, comp.node_id_, test::TEST_SYSTEM_KEY);
  queue_roll_call_reply_after_probe(radio, comp.node_id_, /*foreign_key=*/false);

  EXPECT_TRUE(comp.discover_and_pair());

  const IoDevice *heater = comp.get_device("B6BDFC");
  ASSERT_NE(heater, nullptr) << "the heater behind the remote-controller node is registered";
  EXPECT_EQ(heater->type, DeviceType::ELECTRICAL_HEATER);
  EXPECT_FALSE(heater->has_system_key) << "it uses the hub key, so it carries none of its own";
  EXPECT_EQ(comp.pairing_telemetry().outcome(), PairingOutcome::PAIRED);

  size_t signed_with_hub_key = 0;
  for (const auto &raw : radio.get_sent_data()) {
    IoFrame frame{};
    if (!parse(raw.data(), static_cast<uint8_t>(raw.size()), frame) || frame.cmd != CMD_DISCOVER_SPE_REQ)
      continue;
    uint8_t expected[HMAC_SIZE];
    const uint8_t command = CMD_DISCOVER_SPE_REQ;
    ASSERT_TRUE(crypto::create_hmac(&command, 1, frame.data, test::TEST_SYSTEM_KEY, expected));
    if (memcmp(expected, frame.data + HMAC_SIZE, HMAC_SIZE) == 0)
      signed_with_hub_key++;
  }
  EXPECT_EQ(signed_with_hub_key, 6u);
}

TEST(PairingKeyPullFlow, AHubKeyPullWithNothingAnsweringRegistersNothingAndSaysToScan) {
  // tests/corpus/captures/pairing/atlantic_agilia_pairing_key_pull_own_key_heater_silent.yaml: the
  // radiator hands back the hub key and its heater stays silent through the roll-call.
  RngGuard rng;
  TestableComponent comp;
  MockRadio radio;
  setup(comp, radio);
  queue_proven_pull(radio, comp.node_id_, test::TEST_SYSTEM_KEY);

  EXPECT_FALSE(comp.discover_and_pair());
  EXPECT_EQ(comp.pairing_telemetry().outcome(), PairingOutcome::HUB_KEY_RETURNED);
  EXPECT_EQ(comp.get_device("B6BDFB"), nullptr) << "the pairing node cannot be controlled, so it is not registered";
  EXPECT_EQ(comp.get_device("B6BDFC"), nullptr);
  const std::vector<uint8_t> cmds = sent_commands(radio);
  EXPECT_EQ(std::count(cmds.begin(), cmds.end(), CMD_SET_CONFIG1), 0) << "the node refuses SetConfig1";
  const std::string result = comp.pairing_telemetry().result_sensor_string();
  EXPECT_NE(result.find("outcome=hub_key_returned"), std::string::npos) << result;
  EXPECT_NE(result.find("hub_key_no_responder"), std::string::npos) << result;
}

TEST(PairingKeyPullFlow, ADeviceAnEarlierPairingRegisteredIsRegisteredAgain) {
  // Radiator 3 in issue #165: an earlier attempt in the same boot had registered the heater, so the
  // next one reported it as known, printed no block and ended as key_received.
  RngGuard rng;
  TestableComponent comp;
  MockRadio radio;
  setup(comp, radio);
  IoDevice earlier{};
  const uint8_t heater_id[NODE_ID_SIZE] = {0xB6, 0xBD, 0xFC};
  memcpy(earlier.node_id, heater_id, NODE_ID_SIZE);
  earlier.type = DeviceType::ELECTRICAL_HEATER;
  earlier.subtype = 1;
  earlier.registered_by_pairing = true;
  comp.registry_.put("B6BDFC", earlier);
  queue_proven_pull(radio, comp.node_id_, OTHER_KEY);
  queue_roll_call_reply_after_probe(radio, comp.node_id_, /*foreign_key=*/true);

  EXPECT_TRUE(comp.discover_and_pair());
  EXPECT_EQ(comp.pairing_telemetry().outcome(), PairingOutcome::PAIRED);
  const IoDevice *heater = comp.get_device("B6BDFC");
  ASSERT_NE(heater, nullptr);
  ASSERT_TRUE(heater->has_system_key);
  EXPECT_EQ(0, memcmp(heater->system_key, OTHER_KEY, AES_KEY_SIZE));
  EXPECT_TRUE(heater->registered_by_pairing);
}

TEST(PairingKeyPullFlow, AHubKeyPullFromAnActuatorTakesThePairedPathWithoutARollCall) {
  RngGuard rng;
  TestableComponent comp;
  MockRadio radio;
  setup(comp, radio);
  // Forced pull of a roller shutter (type 2, packed 00 80) that sends the hub's own key: it has a platform
  // itself, so there is nothing to look for behind it.
  comp.tuning_.pairing_key_transfer = KeyTransferMode::PULL;
  const uint8_t payload[] = {0x00, 0x80, 0xB6, 0xBD, 0xFB, 0x02, 0xCC, 0x00, 0x00};
  IoFrame reply{};
  init_frame(reply, true, true, true, false);
  set_dst(reply, comp.node_id_);
  set_src(reply, DEVICE_ID);
  set_cmd(reply, CMD_DISCOVER_RESP, payload, sizeof(payload));
  radio.queue_rx(to_packet(reply));
  IoFrame ack{};
  ASSERT_TRUE(create_discover_confirm_ack(ack, DEVICE_ID, comp.node_id_));
  radio.queue_rx(to_packet(ack));
  radio.queue_rx(to_packet(pulled_key_frame(comp.node_id_, test::TEST_SYSTEM_KEY)));

  EXPECT_TRUE(comp.discover_and_pair());
  const std::vector<uint8_t> cmds = sent_commands(radio);
  EXPECT_EQ(std::count(cmds.begin(), cmds.end(), CMD_DISCOVER_SPE_REQ), 0);
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
  EXPECT_NE(message.find("pairing_key_transfer: push"), std::string::npos) << "the direction can be forced";
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
  EXPECT_NE(message.find("found no device"), std::string::npos);
  EXPECT_NE(message.find("Scan Paired Devices"), std::string::npos);
}

// ============================================================================
// Standalone key pull by node id (PairingEngine::pull_key_from(), ADR 0130)
// ============================================================================

namespace {

/// Sends before a standalone pull's roll-call: the 0x38 and the three unanswered 0x3C. There is no heater probe,
/// since nothing was discovered, so the first 0x2A follows at once.
constexpr int SENDS_BEFORE_PULL_ROLL_CALL = 4;

/// The rig for the standalone pull: a hub with a scripted radio, pulling from DEVICE_ID.
struct PullKeyRig {
  PullKeyRig() { setup(comp, radio); }

  PullKeyResult pull() { return comp.pairing_engine_.pull_key_from(DEVICE_ID); }

  RngGuard rng;
  TestableComponent comp;
  MockRadio radio;
};

}  // namespace

TEST(PullKeyFromEngine, AProvenKeyEndsTheAttemptWithNoDiscoveryAndNoSearch) {
  PullKeyRig rig;
  const IoFrame reply = pulled_key_frame(rig.comp.node_id_, OTHER_KEY);
  rig.radio.queue_rx(to_packet(reply));
  rig.radio.queue_rx(to_packet(proof_frame(rig.comp.node_id_, reply, OTHER_KEY)));

  const PullKeyResult result = rig.pull();

  ASSERT_TRUE(result.attempted());
  EXPECT_EQ(result.outcome, PullKeyOutcome::PROVEN);
  EXPECT_FALSE(result.searched);
  EXPECT_EQ(result.check_value, detail::format_key_check_value(OTHER_KEY));
  EXPECT_EQ(sent_commands(rig.radio), (std::vector<uint8_t>{CMD_LAUNCH_KEY_TRANSFER, CMD_CHALLENGE_REQ}))
      << "no discovery, and nothing sent after the proof";
  EXPECT_NE(result.log_report.find(key_hex(OTHER_KEY)), std::string::npos) << "the device log gets the key once";
  EXPECT_NE(result.log_report.find(detail::format_key_check_value(OTHER_KEY)), std::string::npos);
  EXPECT_NE(result.log_report.find("No search for devices was made"), std::string::npos);
  EXPECT_EQ(result.log_report.find("A roll-call under that key found no device"), std::string::npos)
      << "the report does not claim a roll-call that was not made";
  EXPECT_EQ(result.advice.find(key_hex(OTHER_KEY)), std::string::npos) << "the advice carries no key";
  EXPECT_NE(result.advice.find("proved"), std::string::npos) << result.advice;
  EXPECT_EQ(rig.comp.registry_.size(), 0u) << "nothing is registered";
  EXPECT_EQ(0, memcmp(rig.comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE)) << "the hub key is never replaced";
}

TEST(PullKeyFromEngine, TheLastPairingResultCarriesNoKeyAndTheAdviceCode) {
  PullKeyRig rig;
  const IoFrame reply = pulled_key_frame(rig.comp.node_id_, OTHER_KEY);
  rig.radio.queue_rx(to_packet(reply));
  rig.radio.queue_rx(to_packet(proof_frame(rig.comp.node_id_, reply, OTHER_KEY)));

  rig.pull();

  const std::string summary = rig.comp.pairing_telemetry().result_sensor_string();
  EXPECT_EQ(summary.find(key_hex(OTHER_KEY)), std::string::npos) << summary;
  EXPECT_NE(summary.find("outcome=key_received"), std::string::npos) << summary;
  EXPECT_NE(summary.find("pull_key_proven"), std::string::npos) << summary;
}

TEST(PullKeyFromEngine, HoldsTheFrameLogAcrossTheRequestAndTheProof) {
  PullKeyRig rig;
  test::HoldSeam seam(rig.radio);
  const IoFrame reply = pulled_key_frame(rig.comp.node_id_, OTHER_KEY);
  rig.radio.queue_rx(to_packet(reply));
  rig.radio.queue_rx(to_packet(proof_frame(rig.comp.node_id_, reply, OTHER_KEY)));

  ASSERT_EQ(rig.pull().outcome, PullKeyOutcome::PROVEN);

  EXPECT_TRUE(seam.held_at_every_send_and_delivery()) << "ADR 0111: nothing is logged while the hub waits";
  EXPECT_TRUE(test::frame_log_released());
}

TEST(PullKeyFromEngine, TheHubsOwnKeyIsReportedAsAlreadyShared) {
  PullKeyRig rig;
  rig.radio.queue_rx(to_packet(pulled_key_frame(rig.comp.node_id_, rig.comp.system_key_)));

  const PullKeyResult result = rig.pull();

  EXPECT_EQ(result.outcome, PullKeyOutcome::OWN_KEY);
  EXPECT_TRUE(result.log_report.empty());
  EXPECT_EQ(sent_commands(rig.radio), (std::vector<uint8_t>{CMD_LAUNCH_KEY_TRANSFER}));
  EXPECT_NE(result.advice.find("already shares this hub's key"), std::string::npos) << result.advice;
  EXPECT_EQ(rig.comp.pairing_telemetry().outcome(), PairingOutcome::HUB_KEY_RETURNED);
}

TEST(PullKeyFromEngine, ARefusalCarriesTheControllersCodeAndSendsNoSecondRequest) {
  PullKeyRig rig;
  rig.radio.queue_rx(to_packet(error_frame(rig.comp.node_id_, RESULT_COMMAND_UNKNOWN)));

  const PullKeyResult result = rig.pull();

  EXPECT_EQ(result.outcome, PullKeyOutcome::REFUSED);
  EXPECT_TRUE(result.has_refusal_code);
  EXPECT_EQ(result.refusal_code, RESULT_COMMAND_UNKNOWN);
  EXPECT_EQ(sent_commands(rig.radio), (std::vector<uint8_t>{CMD_LAUNCH_KEY_TRANSFER})) << "a refusal ends the tries";
  EXPECT_EQ(rig.comp.pairing_telemetry().outcome(), PairingOutcome::KEY_TRANSFER_REFUSED);
  EXPECT_NE(result.advice.find("key-sharing mode"), std::string::npos) << result.advice;
}

TEST(PullKeyFromEngine, ASilentControllerIsReportedAsNoKeyAndNothingIsSearched) {
  PullKeyRig rig;

  const PullKeyResult result = rig.pull();

  EXPECT_EQ(result.outcome, PullKeyOutcome::SILENT);
  EXPECT_FALSE(result.has_refusal_code);
  const std::vector<uint8_t> cmds = sent_commands(rig.radio);
  EXPECT_EQ(std::count(cmds.begin(), cmds.end(), CMD_LAUNCH_KEY_TRANSFER), static_cast<long>(cmds.size()))
      << "only 0x38 tries: no discovery, no roll-call";
  EXPECT_EQ(rig.comp.pairing_telemetry().outcome(), PairingOutcome::KEY_EXCHANGE_FAILED);
  EXPECT_NE(rig.comp.pairing_telemetry().result_sensor_string().find("pull_key_unanswered"), std::string::npos);
  EXPECT_TRUE(test::frame_log_released());
}

TEST(PullKeyFromEngine, AWrongProofIsNotProvenAndNotSearched) {
  PullKeyRig rig;
  const IoFrame reply = pulled_key_frame(rig.comp.node_id_, OTHER_KEY);
  rig.radio.queue_rx(to_packet(reply));
  rig.radio.queue_rx(
      to_packet(proof_frame(rig.comp.node_id_, reply, rig.comp.system_key_)));  // signed with another key

  const PullKeyResult result = rig.pull();

  EXPECT_EQ(result.outcome, PullKeyOutcome::UNPROVEN);
  EXPECT_FALSE(result.searched) << "a 0x3D that does not match is final";
  EXPECT_TRUE(sent_frames_of(rig.radio, CMD_DISCOVER_SPE_REQ).empty());
  EXPECT_TRUE(result.log_report.empty()) << "the key is not printed";
}

TEST(PullKeyFromEngine, AnUnprovenKeyIsProvenByADeviceThatAnswersTheRollCall) {
  // ADR 0071: the same key-holder search as Discover & Pair, and only for a key the controller did not prove.
  PullKeyRig rig;
  rig.radio.queue_rx(to_packet(pulled_key_frame(rig.comp.node_id_, OTHER_KEY)));
  queue_rx_after_sends(rig.radio, SENDS_BEFORE_PULL_ROLL_CALL, heater_roll_call_reply(rig.comp.node_id_));

  const PullKeyResult result = rig.pull();

  EXPECT_EQ(result.outcome, PullKeyOutcome::PROVEN);
  EXPECT_TRUE(result.searched);
  EXPECT_EQ(result.check_value, detail::format_key_check_value(OTHER_KEY)) << "proven by the devices that answered";
  EXPECT_TRUE(result.log_report.empty()) << "the search printed the report; the result adds none";
  const IoDevice *heater = rig.comp.get_device("B6BDFC");
  ASSERT_NE(heater, nullptr);
  ASSERT_TRUE(heater->has_system_key);
  EXPECT_EQ(0, memcmp(heater->system_key, OTHER_KEY, AES_KEY_SIZE));
  EXPECT_EQ(rig.comp.pairing_telemetry().outcome(), PairingOutcome::PAIRED);
  EXPECT_EQ(result.advice.find(key_hex(OTHER_KEY)), std::string::npos);
  EXPECT_EQ(rig.comp.pairing_telemetry().result_sensor_string().find(key_hex(OTHER_KEY)), std::string::npos);
  EXPECT_EQ(0, memcmp(rig.comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE));
}

TEST(PullKeyFromEngine, AnUnprovenKeyNobodyAnswersUnderIsNotPrintedAndRegistersNothing) {
  PullKeyRig rig;
  rig.radio.queue_rx(to_packet(pulled_key_frame(rig.comp.node_id_, OTHER_KEY)));

  const PullKeyResult result = rig.pull();

  EXPECT_EQ(result.outcome, PullKeyOutcome::UNPROVEN);
  EXPECT_TRUE(result.searched);
  EXPECT_TRUE(result.check_value.empty()) << "an unproven key gets no check value in the result";
  EXPECT_EQ(sent_frames_of(rig.radio, CMD_DISCOVER_SPE_REQ).size(), 6u) << "one roll-call sweep under the key";
  EXPECT_TRUE(result.log_report.empty());
  EXPECT_EQ(rig.comp.registry_.size(), 0u);
  EXPECT_EQ(rig.comp.pairing_telemetry().outcome(), PairingOutcome::KEY_EXCHANGE_FAILED);
  EXPECT_NE(result.advice.find("did not prove"), std::string::npos) << result.advice;
  EXPECT_EQ(result.advice.find(key_hex(OTHER_KEY)), std::string::npos);
}

TEST(PullKeyFromEngine, TheHubsOwnNodeIdIsRefusedBeforeAnythingIsSent) {
  PullKeyRig rig;

  const PullKeyResult result = rig.comp.pairing_engine_.pull_key_from(rig.comp.node_id_);

  EXPECT_FALSE(result.attempted());
  ASSERT_NE(result.refusal, nullptr);
  EXPECT_TRUE(rig.radio.get_sent_data().empty());
  EXPECT_EQ(rig.comp.pairing_telemetry().outcome(), PairingOutcome::NONE) << "no attempt was recorded";
}

// ============================================================================
// Channel access of the session-opening steps (ADR 0133)
// ============================================================================

namespace {

/// The test radio on the invented second band, with the hub's Wi-Fi on its CH2: the set of a step is {CH1, CH3}.
void use_second_band_with_wifi_on_ch2(TestableComponent &comp, MockRadio &radio) {
  radio.set_band(test_bands::second_band());
  comp.exchange_engine_.set_avoid_frequency_provider([]() -> uint32_t { return test_bands::SECOND_BAND_CH2_HZ; });
}

}  // namespace

TEST(PairingChannelAccess, TheKeyTransferFollowsTheKeyInitsChannelOnAFastTurnaroundRadio) {
  esphome::test_clock::ManualClock clock;
  TestableComponent comp;
  MockRadio radio;
  setup(comp, radio);
  use_second_band_with_wifi_on_ch2(comp, radio);
  pairing::PairingContext context = context_for_device();
  radio.queue_rx(to_packet(challenge_frame(comp.node_id_)));
  radio.queue_rx(to_packet(error_frame(comp.node_id_, RESULT_COMMAND_UNKNOWN)));

  EXPECT_FALSE(comp.pairing_engine_.run_key_exchange_phase_(context));

  const std::vector<uint32_t> opener = tx_channels_of(radio, CMD_KEY_INIT);
  ASSERT_EQ(opener.size(), 1u);
  EXPECT_NE(opener[0], FREQ_CH2) << "the Wi-Fi channel is excluded";
  EXPECT_EQ(tx_channels_of(radio, CMD_KEY_TRANSFER), opener) << "the 0x32 stays on the session's channel";
  EXPECT_EQ(context.session_freq, opener[0]);
}

TEST(PairingChannelAccess, TheKeyTransferAndItsChallengeAnswerFollowTheKeyInitsChannelOnASlowTurnaroundRadio) {
  esphome::test_clock::ManualClock clock;
  TestableComponent comp;
  MockRadioSX1262 radio;
  setup(comp, radio);
  use_second_band_with_wifi_on_ch2(comp, radio);
  pairing::PairingContext context = context_for_device();
  radio.queue_rx(to_packet(challenge_frame(comp.node_id_)));  // to the 0x31
  radio.queue_rx(to_packet(challenge_frame(comp.node_id_)));  // to the 0x32, answered by a 0x3D
  radio.queue_rx(to_packet(error_frame(comp.node_id_, RESULT_COMMAND_UNKNOWN)));

  EXPECT_FALSE(comp.pairing_engine_.run_key_exchange_phase_(context));

  const std::vector<uint32_t> opener = tx_channels_of(radio, CMD_KEY_INIT);
  ASSERT_EQ(opener.size(), 1u);
  EXPECT_EQ(tx_channels_of(radio, CMD_KEY_TRANSFER), (std::vector<uint32_t>{opener[0]}));
  EXPECT_EQ(tx_channels_of(radio, CMD_CHALLENGE_RESP), (std::vector<uint32_t>{opener[0]}));
}

TEST(PairingChannelAccess, AResentKeyInitTakesAChannelOfTheSetAndItsTransferFollowsIt) {
  esphome::test_clock::ManualClock clock;
  TestableComponent comp;
  MockRadioSX1262 radio;
  setup(comp, radio);
  use_second_band_with_wifi_on_ch2(comp, radio);
  pairing::PairingContext context = context_for_device();
  ASSERT_TRUE(create_key_init(context.key_init, comp.node_id_, context.device.node_id));
  radio.queue_rx(to_packet(challenge_frame(comp.node_id_)));
  radio.queue_rx(to_packet(error_frame(comp.node_id_, RESULT_COMMAND_UNKNOWN)));

  EXPECT_FALSE(comp.pairing_engine_.resend_key_init_(context, 1));

  const std::vector<uint32_t> opener = tx_channels_of(radio, CMD_KEY_INIT);
  ASSERT_EQ(opener.size(), 1u);
  EXPECT_NE(opener[0], FREQ_CH2);
  EXPECT_EQ(tx_channels_of(radio, CMD_KEY_TRANSFER), opener);
}

TEST(PairingChannelAccess, ABusyChannel2SendsNoKeyInitAndEndsTheStepAsAFailedTry) {
  esphome::test_clock::ManualClock clock;
  TestableComponent comp;
  MockRadio radio;
  setup(comp, radio);
  radio.set_channel_rssi(FREQ_CH2, -50);
  pairing::PairingContext context = context_for_device();

  EXPECT_FALSE(comp.pairing_engine_.run_key_exchange_phase_(context));

  EXPECT_EQ(radio.get_send_count(), 0) << "never sent onto a channel measured busy";
  EXPECT_EQ(comp.exchange_engine_.counters().channel_busy_attempts, 1u);
  EXPECT_TRUE(test::frame_log_released());
}

TEST(PairingChannelAccess, ABusyChannel2KeepsTheDiscoverConfirmSilentUntilItsTriesAreSpent) {
  esphome::test_clock::ManualClock clock;
  TestableComponent comp;
  MockRadio radio;
  setup(comp, radio);
  radio.set_channel_rssi(FREQ_CH2, -50);
  pairing::PairingContext context = context_for_device();

  EXPECT_EQ(comp.pairing_engine_.run_discover_confirm_step_(context), pairing::DiscoverConfirmResult::NO_REPLY);

  EXPECT_EQ(radio.get_send_count(), 0);
  EXPECT_EQ(comp.exchange_engine_.counters().channel_busy_attempts,
            static_cast<uint32_t>(PAIRING_DISCOVER_CONFIRM_TRIES));
}

TEST(PairingChannelAccess, TheDiscoverConfirmLeavesTheChannelNearTheWifiAndEveryTryDrawsFromTheSet) {
  esphome::test_clock::ManualClock clock;
  TestableComponent comp;
  MockRadio radio;
  setup(comp, radio);
  use_second_band_with_wifi_on_ch2(comp, radio);
  pairing::PairingContext context = context_for_device();

  EXPECT_EQ(comp.pairing_engine_.run_discover_confirm_step_(context), pairing::DiscoverConfirmResult::NO_REPLY);

  const std::vector<uint32_t> channels = tx_channels_of(radio, CMD_DISCOVER_CONFIRM);
  ASSERT_EQ(channels.size(), static_cast<size_t>(PAIRING_DISCOVER_CONFIRM_TRIES));
  for (const uint32_t freq : channels)
    EXPECT_NE(freq, FREQ_CH2);
}

TEST(PairingChannelAccess, TheOptionalSetConfigAndTheKeyHeldCheckWaitForAFreeChannelToo) {
  esphome::test_clock::ManualClock clock;
  TestableComponent comp;
  MockRadio radio;
  setup(comp, radio);
  radio.set_channel_rssi(FREQ_CH2, -50);
  pairing::PairingContext context = context_for_device();

  comp.pairing_engine_.finalize_pairing_configuration_(context);
  EXPECT_FALSE(comp.pairing_engine_.verify_device_holds_key_(context));

  EXPECT_EQ(radio.get_send_count(), 0) << "no 0x6F and no 0x50 onto a busy channel";
}

TEST(PairingChannelAccess, TheKeyHeldCheckLeavesTheChannelNearTheWifi) {
  esphome::test_clock::ManualClock clock;
  TestableComponent comp;
  MockRadio radio;
  setup(comp, radio);
  use_second_band_with_wifi_on_ch2(comp, radio);
  pairing::PairingContext context = context_for_device();

  comp.pairing_engine_.verify_device_holds_key_(context);

  const std::vector<uint32_t> channels = tx_channels_of(radio, CMD_GET_NAME);
  ASSERT_FALSE(channels.empty());
  for (const uint32_t freq : channels)
    EXPECT_NE(freq, FREQ_CH2);
}

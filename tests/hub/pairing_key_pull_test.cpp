/// @file pairing_key_pull_test.cpp
/// @brief KeyPull on its own: the 0x38 request and its tries, the decrypted key, the proof challenges,
/// and the direction a pairing attempt moves the key in. No PairingEngine is built.

#include "hub_pairing.h"
#include "pairing_key_pull.h"
#include "pairing_test_support.h"
#include "proto_codecs.h"
#include "proto_commands.h"
#include "proto_crypto.h"
#include "proto_frame.h"

#include "esp_random.h"
#include "test_helpers.h"
#include "stubs/radio_test_common.h"
#include "support/hold_seam.h"

#include <cstring>
#include <vector>

using namespace esphome::home_io_control;
using namespace pairing_test;

namespace {

/// A KeyPull standing alone on the rig's exchange engine and telemetry.
struct KeyPullRig : PairingPartsRig {
  using PairingPartsRig::PairingPartsRig;
  KeyPull key_pull{&radio_ptr, node_id, system_key, &tuning, engine, telemetry};
};

pairing::PairingContext context_for_device() {
  pairing::PairingContext context;
  memcpy(context.device.node_id, DEVICE_ID, NODE_ID_SIZE);
  context.device_id = node_id_to_string(DEVICE_ID);
  return context;
}

}  // namespace

// ============================================================================
// Key pull (pairing_key_transfer: pull)
// ============================================================================

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
  MockRadio radio;
  KeyPullRig rig(radio);
  pairing::PairingContext context = context_for_device();

  radio.queue_rx(to_packet(pulled_key_frame(rig.node_id, rig.system_key)));

  EXPECT_EQ(rig.key_pull.pull(context), KeyPull::Result::OUR_KEY);
  EXPECT_EQ(sent_commands(radio), (std::vector<uint8_t>{CMD_LAUNCH_KEY_TRANSFER})) << "our own key needs no challenge";
}

TEST(PairingKeyPull, ProvenForeignKeyIsReportedNotAdopted) {
  RngGuard rng;
  MockRadio radio;
  KeyPullRig rig(radio);
  pairing::PairingContext context = context_for_device();
  uint8_t key_before[AES_KEY_SIZE];
  memcpy(key_before, rig.system_key, AES_KEY_SIZE);

  const IoFrame reply = pulled_key_frame(rig.node_id, OTHER_KEY);
  radio.queue_rx(to_packet(reply));
  radio.queue_rx(to_packet(proof_frame(rig.node_id, reply, OTHER_KEY)));

  EXPECT_EQ(rig.key_pull.pull(context), KeyPull::Result::OTHER_KEY);
  EXPECT_EQ(sent_commands(radio), (std::vector<uint8_t>{CMD_LAUNCH_KEY_TRANSFER, CMD_CHALLENGE_REQ}));
  EXPECT_EQ(0, memcmp(rig.system_key, key_before, AES_KEY_SIZE)) << "a pulled key is never adopted automatically";
}

TEST(PairingKeyPull, HoldsTheFrameLogAcrossTheRequestAndTheProofChallenge) {
  RngGuard rng;
  MockRadio radio;
  KeyPullRig rig(radio);
  pairing::PairingContext context = context_for_device();
  test::HoldSeam seam(radio);

  const IoFrame reply = pulled_key_frame(rig.node_id, OTHER_KEY);
  radio.queue_rx(to_packet(reply));
  radio.queue_rx(to_packet(proof_frame(rig.node_id, reply, OTHER_KEY)));

  ASSERT_EQ(rig.key_pull.pull(context), KeyPull::Result::OTHER_KEY);
  EXPECT_EQ(seam.depth_at_send.size(), 2u) << "the 0x38 and the proof challenge";
  EXPECT_EQ(seam.depth_at_deliver.size(), 2u) << "the 0x32 and the device's 0x3D";
  EXPECT_TRUE(seam.held_at_every_send_and_delivery())
      << "nothing may be logged while the hub waits for the 0x32 or for the proof (ADR 0111)";
  EXPECT_TRUE(test::frame_log_released());
}

TEST(PairingKeyPull, ReleasesTheFrameLogWhenNothingAnswers) {
  RngGuard rng;
  MockRadio radio;
  KeyPullRig rig(radio);
  pairing::PairingContext context = context_for_device();

  EXPECT_EQ(rig.key_pull.pull(context), KeyPull::Result::FAILED);
  EXPECT_TRUE(test::frame_log_released()) << "every silent try ends its quiet window";
}

TEST(PairingKeyPull, AKeyWithNoProofIsKeptUnprovenForARollCall) {
  RngGuard rng;
  MockRadio radio;
  KeyPullRig rig(radio);
  pairing::PairingContext context = context_for_device();

  radio.queue_rx(to_packet(pulled_key_frame(rig.node_id, OTHER_KEY)));
  // No 0x3D follows.

  EXPECT_EQ(rig.key_pull.pull(context), KeyPull::Result::UNPROVEN);
  EXPECT_TRUE(context.has_pulled_key);
  EXPECT_EQ(0, memcmp(context.pulled_key, OTHER_KEY, AES_KEY_SIZE)) << "kept for the roll-call, never printed";
  EXPECT_TRUE(rig.telemetry.key_pull_unproven());
}

TEST(PairingKeyPull, A0x32ThatAnswersAnEarlierTryLateStillDecrypts) {
  // Issue #215: a slow giver's 0x32 can land in the next try's window. Every 0x38 carries the same
  // challenge, so the IV the hub decrypts with is the one the device encrypted with.
  RngGuard rng;
  MockRadio radio;
  KeyPullRig rig(radio);
  pairing::PairingContext context = context_for_device();

  const IoFrame reply = pulled_key_frame(rig.node_id, OTHER_KEY);  // answers a request with PULL_CHALLENGE
  radio.queue_rx_hold_until_sent(2);                               // nothing during try 1
  radio.queue_rx(to_packet(reply));
  radio.queue_rx(to_packet(proof_frame(rig.node_id, reply, OTHER_KEY)));

  EXPECT_EQ(rig.key_pull.pull(context), KeyPull::Result::OTHER_KEY);
  const std::vector<IoFrame> pulls = sent_frames_of(radio, CMD_LAUNCH_KEY_TRANSFER);
  ASSERT_EQ(pulls.size(), 2u);
  EXPECT_EQ(0, memcmp(pulls[0].data, PULL_CHALLENGE, HMAC_SIZE));
  EXPECT_EQ(0, memcmp(pulls[1].data, PULL_CHALLENGE, HMAC_SIZE)) << "the retry repeats the challenge";
}

TEST(PairingKeyPull, A0x3DThatAnswersAnEarlierChallengeLateStillVerifies) {
  // Without repeated bytes, a 0x3D to challenge 1 arriving in challenge 2's window would read as a
  // wrong answer, which ends the pull for good.
  test_rng::reset();
  test_rng::enqueue_bytes(PULL_CHALLENGE, HMAC_SIZE);
  test_rng::enqueue_bytes(VERIFY_CHALLENGE, HMAC_SIZE);
  const uint8_t other_challenge[HMAC_SIZE] = {0x31, 0x32, 0x33, 0x34, 0x35, 0x36};
  test_rng::enqueue_bytes(other_challenge, HMAC_SIZE);
  MockRadio radio;
  KeyPullRig rig(radio);
  pairing::PairingContext context = context_for_device();

  const IoFrame reply = pulled_key_frame(rig.node_id, OTHER_KEY);
  radio.queue_rx(to_packet(reply));
  radio.queue_rx_hold_until_sent(3);                                      // challenge 1 gets no answer in its window
  radio.queue_rx(to_packet(proof_frame(rig.node_id, reply, OTHER_KEY)));  // signed over VERIFY_CHALLENGE

  EXPECT_EQ(rig.key_pull.pull(context), KeyPull::Result::OTHER_KEY);
  const std::vector<IoFrame> challenges = sent_frames_of(radio, CMD_CHALLENGE_REQ);
  ASSERT_EQ(challenges.size(), 2u);
  EXPECT_EQ(0, memcmp(challenges[0].data, VERIFY_CHALLENGE, HMAC_SIZE));
  EXPECT_EQ(0, memcmp(challenges[1].data, VERIFY_CHALLENGE, HMAC_SIZE));
  test_rng::reset();
}

TEST(PairingKeyPull, ARepeatedChallengeGetsTheStartPreamble) {
  RngGuard rng;
  MockRadio radio;
  KeyPullRig rig(radio);
  pairing::PairingContext context = context_for_device();

  radio.queue_rx(to_packet(pulled_key_frame(rig.node_id, OTHER_KEY)));
  // No 0x3D: three challenges.

  rig.key_pull.pull(context);

  const auto &sent = radio.get_sent_data();
  const auto &configs = radio.get_tx_configs();
  ASSERT_EQ(sent.size(), configs.size());
  std::vector<uint16_t> challenge_preambles;
  uint16_t pull_preamble = 0;
  for (size_t i = 0; i < sent.size(); i++) {
    IoFrame frame{};
    if (!parse(sent[i].data(), static_cast<uint8_t>(sent[i].size()), frame))
      continue;
    if (frame.cmd == CMD_LAUNCH_KEY_TRANSFER)
      pull_preamble = configs[i].preamble_len;
    if (frame.cmd == CMD_CHALLENGE_REQ)
      challenge_preambles.push_back(configs[i].preamble_len);
  }
  ASSERT_EQ(challenge_preambles.size(), 3u);
  EXPECT_EQ(challenge_preambles[0], radio.response_preamble()) << "the first answers the 0x32 at once";
  EXPECT_GT(pull_preamble, radio.response_preamble());
  EXPECT_EQ(challenge_preambles[1], pull_preamble) << "a repeat follows a pause";
  EXPECT_EQ(challenge_preambles[2], pull_preamble);
}

TEST(PairingKeyPull, WrongChallengeAnswerLeavesTheKeyUnproven) {
  RngGuard rng;
  MockRadio radio;
  KeyPullRig rig(radio);
  pairing::PairingContext context = context_for_device();
  uint8_t key_before[AES_KEY_SIZE];
  memcpy(key_before, rig.system_key, AES_KEY_SIZE);

  const IoFrame reply = pulled_key_frame(rig.node_id, OTHER_KEY);
  const uint8_t unrelated_key[AES_KEY_SIZE] = {0x77};
  radio.queue_rx(to_packet(reply));
  radio.queue_rx(to_packet(proof_frame(rig.node_id, reply, unrelated_key)));

  EXPECT_EQ(rig.key_pull.pull(context), KeyPull::Result::FAILED);
  EXPECT_TRUE(rig.telemetry.key_pull_unproven());
  EXPECT_EQ(0, memcmp(rig.system_key, key_before, AES_KEY_SIZE));
}

TEST(PairingKeyPull, ALostProofIsChallengedAgain) {
  // Radiator 3 in issue #165: one 0x3D lost on air failed the attempt; the next press paired.
  test_rng::reset();
  test_rng::enqueue_bytes(PULL_CHALLENGE, HMAC_SIZE);
  test_rng::enqueue_bytes(VERIFY_CHALLENGE, HMAC_SIZE);
  test_rng::enqueue_bytes(VERIFY_CHALLENGE, HMAC_SIZE);
  MockRadio radio;
  KeyPullRig rig(radio);
  pairing::PairingContext context = context_for_device();

  const IoFrame reply = pulled_key_frame(rig.node_id, OTHER_KEY);
  radio.queue_rx(to_packet(reply));
  // The first challenge's wait is silent for its whole window; the proof arrives after the second one.
  radio.queue_rx_hold_until_sent(3);
  radio.queue_rx(to_packet(proof_frame(rig.node_id, reply, OTHER_KEY)));

  EXPECT_EQ(rig.key_pull.pull(context), KeyPull::Result::OTHER_KEY);
  EXPECT_EQ(sent_commands(radio),
            (std::vector<uint8_t>{CMD_LAUNCH_KEY_TRANSFER, CMD_CHALLENGE_REQ, CMD_CHALLENGE_REQ}));
  test_rng::reset();
}

TEST(PairingKeyPull, AWrongProofIsNotChallengedAgain) {
  RngGuard rng;
  MockRadio radio;
  KeyPullRig rig(radio);
  pairing::PairingContext context = context_for_device();

  const IoFrame reply = pulled_key_frame(rig.node_id, OTHER_KEY);
  const uint8_t unrelated_key[AES_KEY_SIZE] = {0x77};
  radio.queue_rx(to_packet(reply));
  radio.queue_rx(to_packet(proof_frame(rig.node_id, reply, unrelated_key)));

  EXPECT_EQ(rig.key_pull.pull(context), KeyPull::Result::FAILED);
  EXPECT_EQ(sent_commands(radio), (std::vector<uint8_t>{CMD_LAUNCH_KEY_TRANSFER, CMD_CHALLENGE_REQ}))
      << "a 0x3D that does not verify proves the device lacks the key; asking again cannot change that";
}

TEST(PairingKeyPull, ASilentMainsDeviceIsAskedNineTimesListeningShortOnAllButTheLastTry) {
  esphome::test_clock::ManualClock clock;
  RngGuard rng;
  MockRadio radio;
  KeyPullRig rig(radio);
  pairing::PairingContext context = context_for_device();

  EXPECT_EQ(rig.key_pull.pull(context), KeyPull::Result::FAILED);

  EXPECT_EQ(sent_frames_of(radio, CMD_LAUNCH_KEY_TRANSFER).size(), static_cast<size_t>(EXCHANGE_MAX_ATTEMPTS));
  const auto &waits = radio.wait_timeouts();
  ASSERT_FALSE(waits.empty());
  EXPECT_EQ(waits.front(), rig.tuning.exchange_early_response_wait_ms) << "an early try listens the short window";
  EXPECT_EQ(waits.back(), PAIRING_KEY_REPLY_TIMEOUT_MS) << "the last try listens the whole reply window";
}

TEST(PairingKeyPull, ALowPowerDeviceIsAskedThreeTimesWithTheRetryGap) {
  esphome::test_clock::ManualClock clock;
  RngGuard rng;
  MockRadio radio;
  KeyPullRig rig(radio);
  pairing::PairingContext context = context_for_device();
  context.discovery_low_power = true;

  EXPECT_EQ(rig.key_pull.pull(context), KeyPull::Result::FAILED);

  EXPECT_EQ(sent_frames_of(radio, CMD_LAUNCH_KEY_TRANSFER).size(), static_cast<size_t>(EXCHANGE_RETRY_COUNT));
  const auto &times = radio.send_times_ms();
  ASSERT_EQ(times.size(), static_cast<size_t>(EXCHANGE_RETRY_COUNT));
  EXPECT_EQ(times[1] - times[0], PAIRING_KEY_REPLY_TIMEOUT_MS + EXCHANGE_RETRY_DELAY_MS)
      << "one reply window and the retry gap between tries";
}

TEST(PairingKeyPull, TheKeyPullNeverRunsPastTheExchangeBudget) {
  for (const uint16_t budget_ms : {600, 1000, 2500, 5000}) {
    esphome::test_clock::ManualClock clock;
    RngGuard rng;
    MockRadio radio;
    KeyPullRig rig(radio);
    rig.tuning.exchange_total_budget_ms = budget_ms;
    pairing::PairingContext context = context_for_device();
    const uint32_t start = esphome::test_clock::peek_ms();

    EXPECT_EQ(rig.key_pull.pull(context), KeyPull::Result::FAILED);

    EXPECT_LT(esphome::test_clock::peek_ms() - start, budget_ms) << "budget " << budget_ms;
    EXPECT_GE(sent_frames_of(radio, CMD_LAUNCH_KEY_TRANSFER).size(), 1u);
  }
}

TEST(PairingKeyPull, AReplyToALaterTryStopsTheTries) {
  esphome::test_clock::ManualClock clock;
  RngGuard rng;
  MockRadio radio;
  KeyPullRig rig(radio);
  pairing::PairingContext context = context_for_device();
  radio.queue_rx_hold_until_sent(5);  // silent for four tries, then the key arrives
  radio.queue_rx(to_packet(pulled_key_frame(rig.node_id, test::TEST_SYSTEM_KEY)));

  EXPECT_EQ(rig.key_pull.pull(context), KeyPull::Result::OUR_KEY);

  EXPECT_EQ(sent_frames_of(radio, CMD_LAUNCH_KEY_TRANSFER).size(), 5u);
}

TEST(PairingKeyPull, RefusalEndsThePull) {
  MockRadio radio;
  KeyPullRig rig(radio);
  pairing::PairingContext context = context_for_device();

  radio.queue_rx(to_packet(error_frame(rig.node_id, RESULT_COMMAND_UNKNOWN)));

  EXPECT_EQ(rig.key_pull.pull(context), KeyPull::Result::FAILED);
  EXPECT_TRUE(context.key_refused);
  EXPECT_EQ(sent_commands(radio), (std::vector<uint8_t>{CMD_LAUNCH_KEY_TRANSFER})) << "no retry after a refusal";
}

// ============================================================================
// Direction: pairing_key_transfer auto | push | pull
// ============================================================================

namespace {

pairing::PairingContext context_of_type(DeviceType type) {
  pairing::PairingContext context = context_for_device();
  context.device.type = type;
  return context;
}

}  // namespace

TEST(PairingKeyDirection, AutoPullsFromARemoteControllerAndPushesToAnythingElse) {
  MockRadio radio;
  KeyPullRig rig(radio);
  ASSERT_EQ(rig.tuning.pairing_key_transfer, KeyTransferMode::AUTO) << "auto is the default";

  EXPECT_TRUE(rig.key_pull.selected(context_of_type(DeviceType::REMOTE_CONTROLLER)));
  EXPECT_FALSE(rig.key_pull.selected(context_of_type(DeviceType::ROLLER_SHUTTER)));
  EXPECT_FALSE(rig.key_pull.selected(context_of_type(DeviceType::UNKNOWN)));
}

TEST(PairingKeyDirection, ForcedModesIgnoreTheDiscoveryType) {
  MockRadio radio;
  KeyPullRig rig(radio);

  rig.tuning.pairing_key_transfer = KeyTransferMode::PUSH;
  EXPECT_FALSE(rig.key_pull.selected(context_of_type(DeviceType::REMOTE_CONTROLLER)));
  rig.tuning.pairing_key_transfer = KeyTransferMode::PULL;
  EXPECT_TRUE(rig.key_pull.selected(context_of_type(DeviceType::ROLLER_SHUTTER)));
}

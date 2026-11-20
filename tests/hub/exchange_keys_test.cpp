/// @file exchange_keys_test.cpp
/// @brief Tests for the keys of an exchange: the explicit key of one exchange, the per-device system keys and the wait
/// for a challenge answer.

#include "hub_exchange.h"
#include "hub_core.h"
#include "proto_frame.h"
#include "proto_commands.h"
#include "proto_crypto.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"
#include "support/hold_seam.h"

#include <cstring>

#include "exchange_fixture.h"

using namespace esphome::home_io_control;
using namespace exchange_test;

namespace {
constexpr uint8_t EXPLICIT_KEY[AES_KEY_SIZE] = {0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8,
                                                0xE9, 0xEA, 0xEB, 0xEC, 0xED, 0xEE, 0xEF, 0xF0};
constexpr uint8_t PROVIDER_KEY[AES_KEY_SIZE] = {0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8,
                                                0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF, 0xD0};
}  // namespace

// --- An explicit key replaces the registry's choice for one exchange ----------------------------

namespace {

/// Run one exchange to a node whose challenge the device answers, with the engine's key provider
/// returning PROVIDER_KEY for every node, and return the HMAC of the 0x3D the engine sent.
void exchange_and_read_0x3d(const uint8_t *explicit_key, uint8_t out_hmac[HMAC_SIZE], const uint8_t *challenge_bytes) {
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning;
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);
  engine.set_device_key_provider([](const uint8_t *) -> const uint8_t * { return PROVIDER_KEY; });

  IoFrame request{};
  create_execute_position(request, test::OWN_ID, test::DST_ID, false, 100);
  radio.queue_rx(to_rx_packet(build_challenge(test::DST_ID, test::OWN_ID, challenge_bytes)));
  radio.queue_rx(to_rx_packet(build_status_response(test::DST_ID, test::OWN_ID)));

  IoFrame response{};
  ASSERT_EQ(engine.send_and_receive(request, response, FREQ_CH2, ExchangeOptions{}.with_key(explicit_key)),
            ExchangeOutcome::SUCCESS_WITH_RESPONSE);
  ASSERT_GE(radio.get_sent_data().size(), 2u);
  IoFrame answer{};
  const auto &raw = radio.get_sent_data()[1];
  ASSERT_TRUE(parse(raw.data(), static_cast<uint8_t>(raw.size()), answer));
  ASSERT_EQ(answer.cmd, CMD_CHALLENGE_RESP);
  memcpy(out_hmac, answer.data, HMAC_SIZE);
}

}  // namespace

TEST(ExchangeExplicitKey, TheChallengeIsAnsweredUnderTheGivenKeyNotTheRegistrysOne) {
  const uint8_t challenge[HMAC_SIZE] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  IoFrame request{};
  create_execute_position(request, test::OWN_ID, test::DST_ID, false, 100);
  uint8_t frame_data[FRAME_MAX_SIZE];
  frame_data[0] = request.cmd;
  memcpy(frame_data + 1, request.data, request.data_len);
  uint8_t under_explicit[HMAC_SIZE];
  uint8_t under_provider[HMAC_SIZE];
  ASSERT_TRUE(crypto::create_hmac(frame_data, request.data_len + 1, challenge, EXPLICIT_KEY, under_explicit));
  ASSERT_TRUE(crypto::create_hmac(frame_data, request.data_len + 1, challenge, PROVIDER_KEY, under_provider));

  uint8_t sent[HMAC_SIZE]{};
  exchange_and_read_0x3d(EXPLICIT_KEY, sent, challenge);
  EXPECT_EQ(0, memcmp(sent, under_explicit, HMAC_SIZE));
  EXPECT_NE(0, memcmp(sent, under_provider, HMAC_SIZE));

  exchange_and_read_0x3d(nullptr, sent, challenge);
  EXPECT_EQ(0, memcmp(sent, under_provider, HMAC_SIZE)) << "without a key the provider's key is used";
}

// ============================================================================
// Per-device system keys (ADR 0053)
// ============================================================================

namespace {

/// A second key for a device that keeps one of its own (the same test value the key-pull tests use).
constexpr uint8_t DEVICE_OWN_KEY[AES_KEY_SIZE] = {0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE,
                                                  0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};

/// Register `test::DST_ID` on `comp`, optionally with a key of its own.
void register_destination(TestableComponent &comp, const uint8_t *own_key) {
  DeviceConfig cfg;
  if (own_key != nullptr) {
    cfg.has_system_key = true;
    memcpy(cfg.system_key, own_key, AES_KEY_SIZE);
  }
  comp.add_device(node_id_to_string(test::DST_ID), cfg);
}

/// Run a challenge exchange against `test::DST_ID` and return the HMAC the hub put in its 0x3D.
bool run_challenge_exchange(TestableComponent &comp, MockRadio &radio, IoFrame &request, uint8_t (&challenge)[6],
                            uint8_t (&sent_hmac)[HMAC_SIZE]) {
  comp.initialized_ = true;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  create_execute_position(request, comp.node_id_, test::DST_ID, false, 100);
  const IoFrame chal = build_challenge(test::DST_ID, comp.node_id_, challenge);
  uint8_t raw[64];
  RadioRxPacket pkt{};
  pkt.len = serialize(chal, raw, sizeof(raw));
  memcpy(pkt.data, raw, pkt.len);
  radio.queue_rx(pkt);
  const IoFrame final_resp = build_status_response(test::DST_ID, comp.node_id_);
  RadioRxPacket final_pkt{};
  final_pkt.len = serialize(final_resp, raw, sizeof(raw));
  memcpy(final_pkt.data, raw, final_pkt.len);
  radio.queue_rx(final_pkt);

  IoFrame response{};
  if (comp.send_and_receive_(request, response, FREQ_CH2) != ExchangeOutcome::SUCCESS_WITH_RESPONSE)
    return false;
  IoFrame auth{};
  const auto &auth_raw = radio.get_sent_data()[1];
  if (!parse(auth_raw.data(), static_cast<uint8_t>(auth_raw.size()), auth) || auth.cmd != CMD_CHALLENGE_RESP)
    return false;
  memcpy(sent_hmac, auth.data, HMAC_SIZE);
  return true;
}

/// The HMAC a 0x3D for `request` carries under `key`.
void expected_hmac(const IoFrame &request, const uint8_t (&challenge)[6], const uint8_t *key,
                   uint8_t (&out)[HMAC_SIZE]) {
  uint8_t frame_data[FRAME_MAX_SIZE];
  frame_data[0] = request.cmd;
  memcpy(frame_data + 1, request.data, request.data_len);
  ASSERT_TRUE(crypto::create_hmac(frame_data, request.data_len + 1, challenge, key, out));
}

}  // namespace

TEST(DeviceKey, ChallengeResponseIsSignedWithTheDevicesOwnKey) {
  TestableComponent comp;
  MockRadio radio;
  register_destination(comp, DEVICE_OWN_KEY);
  IoFrame request{};
  uint8_t challenge[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  uint8_t sent[HMAC_SIZE];
  ASSERT_TRUE(run_challenge_exchange(comp, radio, request, challenge, sent));

  uint8_t with_device_key[HMAC_SIZE];
  uint8_t with_hub_key[HMAC_SIZE];
  expected_hmac(request, challenge, DEVICE_OWN_KEY, with_device_key);
  expected_hmac(request, challenge, test::TEST_SYSTEM_KEY, with_hub_key);
  EXPECT_EQ(0, memcmp(sent, with_device_key, HMAC_SIZE)) << "the 0x3D must use the destination's own key";
  EXPECT_NE(0, memcmp(sent, with_hub_key, HMAC_SIZE));
}

TEST(DeviceKey, DeviceWithoutAKeyOfItsOwnUsesTheHubKey) {
  TestableComponent comp;
  MockRadio radio;
  register_destination(comp, nullptr);
  IoFrame request{};
  uint8_t challenge[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  uint8_t sent[HMAC_SIZE];
  ASSERT_TRUE(run_challenge_exchange(comp, radio, request, challenge, sent));

  uint8_t with_hub_key[HMAC_SIZE];
  expected_hmac(request, challenge, test::TEST_SYSTEM_KEY, with_hub_key);
  EXPECT_EQ(0, memcmp(sent, with_hub_key, HMAC_SIZE));
}

TEST(DeviceKey, UnregisteredDestinationUsesTheHubKey) {
  TestableComponent comp;
  MockRadio radio;
  // A different device carries a key of its own; the destination is not registered at all.
  comp.add_device("AABBCC", [] {
    DeviceConfig cfg;
    cfg.has_system_key = true;
    memcpy(cfg.system_key, DEVICE_OWN_KEY, AES_KEY_SIZE);
    return cfg;
  }());
  IoFrame request{};
  uint8_t challenge[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  uint8_t sent[HMAC_SIZE];
  ASSERT_TRUE(run_challenge_exchange(comp, radio, request, challenge, sent));

  uint8_t with_hub_key[HMAC_SIZE];
  expected_hmac(request, challenge, test::TEST_SYSTEM_KEY, with_hub_key);
  EXPECT_EQ(0, memcmp(sent, with_hub_key, HMAC_SIZE));
}

TEST(DeviceKey, ADeviceKeyEqualToTheHubKeyChangesNothing) {
  TestableComponent comp;
  MockRadio radio;
  register_destination(comp, test::TEST_SYSTEM_KEY);
  IoFrame request{};
  uint8_t challenge[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  uint8_t sent[HMAC_SIZE];
  ASSERT_TRUE(run_challenge_exchange(comp, radio, request, challenge, sent));

  uint8_t with_hub_key[HMAC_SIZE];
  expected_hmac(request, challenge, test::TEST_SYSTEM_KEY, with_hub_key);
  EXPECT_EQ(0, memcmp(sent, with_hub_key, HMAC_SIZE));
}

TEST(DeviceKey, InboundAuthenticationVerifiesAgainstTheSendersOwnKey) {
  TestableComponent comp;
  comp.initialized_ = true;
  RespondOnChallengeMockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);
  register_destination(comp, DEVICE_OWN_KEY);

  IoFrame status_update = build_status_update_from_device(test::DST_ID, comp.node_id_);
  radio.arm(status_update, DEVICE_OWN_KEY, /*valid=*/true);
  EXPECT_TRUE(comp.authenticate_request_(status_update, FREQ_CH2))
      << "a 0x3D signed with the device's own key verifies";
}

TEST(DeviceKey, InboundAuthenticationRejectsTheHubKeyFromAKeyedDevice) {
  TestableComponent comp;
  comp.initialized_ = true;
  RespondOnChallengeMockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);
  register_destination(comp, DEVICE_OWN_KEY);

  IoFrame status_update = build_status_update_from_device(test::DST_ID, comp.node_id_);
  radio.arm(status_update, test::TEST_SYSTEM_KEY, /*valid=*/true);
  EXPECT_FALSE(comp.authenticate_request_(status_update, FREQ_CH2))
      << "a device that keeps its own key is not authenticated by the hub key";
}

// ============================================================================
// The wait for a challenge answer listens for the whole response window and judges only the
// challenged device's 0x3D: a frame from anyone else neither ends the wait nor counts as a wrong key.
// ============================================================================

namespace {

/// Everything one challenge-wait test needs: an engine on a ManualClock, the request being
/// authenticated (a status update from DST_ID) and the fixed challenge bytes the hub will send,
/// so the answers can be built and queued before the call.
struct ChallengeWaitFixture {
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
  RadioDriver *radio_ptr{&radio};
  TuningConfig tuning;
  ExchangeEngine engine{&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning};
  IoFrame request{build_status_update_from_device(test::DST_ID, test::OWN_ID)};
  uint8_t challenge[HMAC_SIZE]{0x11, 0x22, 0x33, 0x44, 0x55, 0x66};

  /// Queue `frame` as the next received packet.
  void queue(const IoFrame &frame) {
    RadioRxPacket pkt{};
    pkt.len = serialize(frame, pkt.data, sizeof(pkt.data));
    radio.queue_rx(pkt);
  }

  /// A 0x3D from `from` to the hub, signed with `key` over `request`; garbage bytes when `key` is null.
  IoFrame answer(const uint8_t from[NODE_ID_SIZE], const uint8_t *key) const {
    IoFrame frame{};
    if (key != nullptr) {
      EXPECT_TRUE(create_challenge_resp_device_role(frame, test::OWN_ID, from, challenge, request, key));
      return frame;
    }
    uint8_t garbage[HMAC_SIZE];
    memset(garbage, 0xAB, sizeof(garbage));
    init_frame(frame);
    set_dst(frame, test::OWN_ID);
    set_src(frame, from);
    set_cmd(frame, CMD_CHALLENGE_RESP, garbage, HMAC_SIZE);
    return frame;
  }

  ExchangeEngine::ChallengeResult run() {
    return engine.challenge_sender(request, FREQ_CH2, test::TEST_SYSTEM_KEY, challenge, 0);
  }
};

}  // namespace

TEST(ChallengeWait, AnUnrelatedFrameBeforeTheGenuineAnswerStillVerifies) {
  ChallengeWaitFixture f;
  f.queue(build_status_update_from_device(test::FOREIGN_ID, test::OWN_ID));
  f.queue(f.answer(test::DST_ID, test::TEST_SYSTEM_KEY));

  EXPECT_EQ(f.run(), ExchangeEngine::ChallengeResult::VERIFIED)
      << "a frame from another party must not end the wait before the device answers";
}

TEST(ChallengeWait, HoldsTheFrameLogFromTheChallengeToTheAnswer) {
  ChallengeWaitFixture f;
  test::HoldSeam seam(f.radio);
  f.queue(f.answer(test::DST_ID, test::TEST_SYSTEM_KEY));

  ASSERT_EQ(f.run(), ExchangeEngine::ChallengeResult::VERIFIED);
  EXPECT_EQ(seam.depth_at_send.size(), 1u);
  EXPECT_TRUE(seam.held_at_every_send_and_delivery())
      << "nothing may be logged while the hub waits for the answer to its challenge (ADR 0111)";
  EXPECT_TRUE(test::frame_log_released());
}

TEST(ChallengeWait, ReleasesTheFrameLogWhenNoAnswerComes) {
  ChallengeWaitFixture f;

  ASSERT_EQ(f.run(), ExchangeEngine::ChallengeResult::NO_ANSWER);
  EXPECT_TRUE(test::frame_log_released());
}

TEST(ChallengeWait, AnswerFromAnotherDeviceAloneIsNoAnswerNotAMismatch) {
  ChallengeWaitFixture f;
  f.queue(f.answer(test::FOREIGN_ID, nullptr));

  EXPECT_EQ(f.run(), ExchangeEngine::ChallengeResult::NO_ANSWER)
      << "a 0x3D from a neighbouring device says nothing about the challenged device's key";
}

TEST(ChallengeWait, AnswerFromTheChallengedDeviceThatDoesNotVerifyIsAMismatch) {
  ChallengeWaitFixture f;
  f.queue(f.answer(test::DST_ID, nullptr));

  EXPECT_EQ(f.run(), ExchangeEngine::ChallengeResult::MISMATCH);
}

TEST(ChallengeWait, TheChallengedDevicesOtherFrameIsIgnoredAndTheAnswerStillCounts) {
  ChallengeWaitFixture f;
  f.queue(build_status_update_from_device(test::DST_ID, test::OWN_ID));
  f.queue(f.answer(test::DST_ID, test::TEST_SYSTEM_KEY));

  EXPECT_EQ(f.run(), ExchangeEngine::ChallengeResult::VERIFIED);
}

TEST(ChallengeWait, FramesIgnoredDuringTheWaitAreCountedInTheDebugSnapshot) {
  ChallengeWaitFixture f;
  f.radio.queue_rx_failed_reception(0x0084);
  f.queue(build_status_update_from_device(test::FOREIGN_ID, test::OWN_ID));

  ASSERT_EQ(f.run(), ExchangeEngine::ChallengeResult::NO_ANSWER);
  const auto &d = f.engine.get_debug();
  EXPECT_EQ(d.final_waits, 1u) << "the challenge wait is counted as one wait for a closing frame";
  EXPECT_EQ(d.final_rx_ignored, 1u) << "the other device's frame was heard and not taken as the answer";
  EXPECT_EQ(d.final_rx_failed, 1u);
  EXPECT_EQ(d.final_rx_irq, 0x0084u);
}

TEST(ChallengeWait, SilenceEndsAsNoAnswerAfterTheWholeWindow) {
  ChallengeWaitFixture f;
  f.tuning.exchange_response_wait_ms = 250;

  EXPECT_EQ(f.run(), ExchangeEngine::ChallengeResult::NO_ANSWER);
  ASSERT_FALSE(f.radio.wait_timeouts().empty());
  EXPECT_EQ(f.radio.wait_timeouts().front(), 250u) << "the wait spends the configured response window";
}

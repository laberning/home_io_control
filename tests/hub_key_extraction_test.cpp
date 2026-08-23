#include "hub_core.h"
#include "hub_internal.h"
#include "pairing_responder.h"
#include "proto_commands.h"
#include "proto_crypto.h"
#include "proto_frame.h"

#include "corpus_generated.h"
#include "corpus_test_helpers.h"
#include "test_helpers.h"
#include "stubs/radio_test_common.h"

#include <esp_random.h>

#include <cstring>

using namespace esphome::home_io_control;
using test::TestableHubComponent;

// ============================================================================
// HubKeyExtraction test suite
// ============================================================================
// "Recover System Key" (key extraction) hub wiring: arm/disarm, the 0x28/0x2C/0x31/0x32 RX
// branches, address disambiguation against the real node_id_ and against other devices' traffic,
// auto-off timeout scheduling, and the disarm-on-extraction guarantee.

namespace {

constexpr uint8_t OUR_NODE_ID[NODE_ID_SIZE] = {0xC0, 0xFF, 0xEE};
constexpr uint8_t FOREIGN_HUB_ID[NODE_ID_SIZE] = {0xAA, 0xBB, 0xCC};

void setup_component(TestableHubComponent &comp, MockRadio &radio) {
  std::memcpy(comp.node_id_, OUR_NODE_ID, NODE_ID_SIZE);
  static const uint8_t key[AES_KEY_SIZE] = {0xD1, 0x74, 0x34, 0x93, 0xFA, 0x94, 0x38, 0x45,
                                            0xAC, 0x43, 0x50, 0xEE, 0xFF, 0x34, 0x29, 0x34};
  std::memcpy(comp.system_key_, key, AES_KEY_SIZE);
  comp.initialized_ = true;
  comp.radio_ = &radio;
}

RadioRxPacket make_rx_packet(const IoFrame &frame) {
  RadioRxPacket pkt{};
  pkt.len = serialize(frame, pkt.data, sizeof(pkt.data));
  pkt.freq_hz = FREQ_CH2;
  return pkt;
}

/// Count how many transmitted frames have the given command byte (cmd is the 9th wire byte,
/// index 8, per proto_frame.h's [CTRL0][CTRL1][DST3][SRC3][CMD] layout).
int count_sent_cmd(const MockRadio &radio, uint8_t cmd) {
  int count = 0;
  for (const auto &pkt : radio.get_sent_data()) {
    if (pkt.size() > 8 && pkt[8] == cmd)
      count++;
  }
  return count;
}

/// The one frame in `cap` transmitted in direction `tx` carrying command `cmd`, parsed. Fails the
/// test loudly (rather than silently returning a default-constructed frame) if the capture doesn't
/// have exactly one such frame -- a capture edit that removes or duplicates the frame a test
/// depends on should break that test immediately, not pass with the wrong bytes.
IoFrame find_capture_frame(const corpus::CorpusCapture *cap, bool tx, uint8_t cmd) {
  const corpus::CorpusFrame *match = nullptr;
  for (uint8_t i = 0; i < cap->frame_count; i++) {
    const corpus::CorpusFrame &cf = cap->frames[i];
    if (cf.tx != tx)
      continue;
    const IoFrame parsed = corpus_test::parse_capture_frame(cf);
    if (parsed.cmd != cmd)
      continue;
    EXPECT_EQ(match, nullptr) << cap->id << " has more than one tx=" << tx << " cmd=0x" << std::hex
                              << static_cast<int>(cmd) << " frame";
    match = &cf;
  }
  if (match == nullptr) {
    ADD_FAILURE() << cap->id << " has no tx=" << tx << " cmd=0x" << std::hex << static_cast<int>(cmd) << " frame";
    return IoFrame{};
  }
  return corpus_test::parse_capture_frame(*match);
}

}  // namespace

// ========================================================================================
// Arm / disarm
// ========================================================================================

TEST(HubKeyExtraction, ArmSetsArmedIdleAndSchedulesAutoOffTimeout) {
  TestableHubComponent comp;
  MockRadio radio;
  setup_component(comp, radio);

  comp.set_key_extraction_armed(true);

  EXPECT_EQ(comp.key_extraction_ctx_.state, pairing_responder::ResponderState::ARMED_IDLE);
  EXPECT_EQ(comp.last_timeout_name_, "key_extraction_auto_off");
  EXPECT_EQ(comp.last_timeout_ms_, 10u * 60u * 1000u) << "auto-off window should be 10 minutes";
  EXPECT_TRUE(stored_node_id_is_valid(comp.key_extraction_ctx_.throwaway_id))
      << "throwaway ID should be a structurally valid node ID";
}

TEST(HubKeyExtraction, ArmedStateCallbackFiresOnManualArmAndDisarm) {
  TestableHubComponent comp;
  MockRadio radio;
  setup_component(comp, radio);

  std::vector<bool> observed;
  comp.set_key_extraction_armed_callback([&](bool armed) { observed.push_back(armed); });

  comp.set_key_extraction_armed(true);
  comp.set_key_extraction_armed(false);

  ASSERT_EQ(observed.size(), 2u);
  EXPECT_TRUE(observed[0]);
  EXPECT_FALSE(observed[1]);
  EXPECT_EQ(comp.key_extraction_ctx_.state, pairing_responder::ResponderState::DISARMED);
}

TEST(HubKeyExtraction, DisarmWhenAlreadyDisarmedIsNoop) {
  TestableHubComponent comp;
  MockRadio radio;
  setup_component(comp, radio);

  int callback_calls = 0;
  comp.set_key_extraction_armed_callback([&](bool) { callback_calls++; });
  comp.set_key_extraction_armed(false);
  EXPECT_EQ(callback_calls, 0) << "disarming an already-disarmed responder should not fire the callback";
}

TEST(HubKeyExtraction, ThrowawayIdAvoidsRealNodeIdAndBroadcastAddresses) {
  TestableHubComponent comp;
  MockRadio radio;
  setup_component(comp, radio);

  // Script the RNG to first "collide" with our real node ID, then land on a valid value.
  test_rng::reset();
  test_rng::enqueue_bytes(OUR_NODE_ID, NODE_ID_SIZE);
  const uint8_t valid_candidate[NODE_ID_SIZE] = {0x11, 0x22, 0x33};
  test_rng::enqueue_bytes(valid_candidate, NODE_ID_SIZE);

  uint8_t out[NODE_ID_SIZE];
  comp.generate_key_extraction_throwaway_id_(out);
  test_rng::reset();

  EXPECT_EQ(0, memcmp(out, valid_candidate, NODE_ID_SIZE)) << "generator should skip the colliding candidate";
  EXPECT_NE(0, memcmp(out, OUR_NODE_ID, NODE_ID_SIZE));
  EXPECT_NE(0, memcmp(out, BROADCAST_DISCOVER, NODE_ID_SIZE));
  EXPECT_NE(0, memcmp(out, BROADCAST_DISCOVER_ALT, NODE_ID_SIZE));
}

TEST(HubKeyExtraction, ThrowawayIdGenerationFallsBackWhenEveryCandidateCollides) {
  TestableHubComponent comp;
  MockRadio radio;
  setup_component(comp, radio);

  // Script every candidate the generator could possibly try (comfortably more than its internal
  // collision-retry budget) to collide with our real node ID. Using the *same* colliding value
  // throughout keeps this test independent of that exact retry-count constant: no matter how many
  // attempts the generator makes before giving up, the last one it tried is still OUR_NODE_ID.
  test_rng::reset();
  for (int i = 0; i < 64; i++)
    test_rng::enqueue_bytes(OUR_NODE_ID, NODE_ID_SIZE);

  uint8_t out[NODE_ID_SIZE];
  comp.generate_key_extraction_throwaway_id_(out);
  test_rng::reset();

  EXPECT_EQ(0, memcmp(out, OUR_NODE_ID, NODE_ID_SIZE))
      << "exhausting the collision-retry budget should fall back to the last-generated candidate, "
         "not leave the output buffer in some other state";
}

// ========================================================================================
// Preamble regression: replies must use the driver's chip-tuned response_preamble(), not a
// fixed SHORT_PREAMBLE/LONG_PREAMBLE constant. Hardware-confirmed 2026-08-02: both fixed choices
// broke real exchanges — SHORT_PREAMBLE(8) was too short for a hopping receiver to reliably catch
// from a slower-turnaround chip, and LONG_PREAMBLE(1024) blocked the main loop long enough to
// blow through the hub's tight per-try wait windows on both chips.
//
// SX1262_RESPONSE_PREAMBLE == SHORT_PREAMBLE (8 bytes): both are byte-denominated, and the
// hardware-validated SX1262 response preamble turns out to equal the protocol's nominal short
// preamble. So MockRadioSX1262 no longer discriminates "used response_preamble()" from
// "hardcoded SHORT_PREAMBLE" on that constant alone — only the LONG_PREAMBLE check still catches
// a regression to a fixed constant.
// ========================================================================================

TEST(HubKeyExtraction, DiscoveryReplyUsesRadioResponsePreambleNotFixedConstant) {
  TestableHubComponent comp;
  MockRadioSX1262 radio;
  setup_component(comp, radio);
  comp.set_key_extraction_armed(true);

  IoFrame discover{};
  create_discover(discover, FOREIGN_HUB_ID);
  comp.process_received_packet_(make_rx_packet(discover));

  ASSERT_EQ(radio.get_tx_configs().size(), 3u) << "0x29 should be sent on all 3 channels";
  for (const auto &tx_config : radio.get_tx_configs()) {
    EXPECT_EQ(tx_config.preamble_len, radio.response_preamble())
        << "discovery reply must use the driver's response_preamble(), not a fixed constant";
    EXPECT_NE(tx_config.preamble_len, LONG_PREAMBLE);
  }
}

// ========================================================================================
// Full RX flow: 0x28 -> 0x29, 0x2C -> 0x2D, 0x31 -> 0x3C, 0x32 -> 0x33 + extraction, then the
// post-extraction grace window (not an immediate disarm any more -- see arm_post_extraction_grace_())
// ========================================================================================

TEST(HubKeyExtraction, FullExchangeReachesExtractedThenGraceDisarms) {
  TestableHubComponent comp;
  MockRadio radio;
  setup_component(comp, radio);
  comp.set_key_extraction_armed(true);
  uint8_t throwaway_id[NODE_ID_SIZE];
  memcpy(throwaway_id, comp.key_extraction_ctx_.throwaway_id, NODE_ID_SIZE);

  // Hub broadcasts discovery.
  IoFrame discover{};
  create_discover(discover, FOREIGN_HUB_ID);
  comp.process_received_packet_(make_rx_packet(discover));
  EXPECT_EQ(comp.key_extraction_ctx_.state, pairing_responder::ResponderState::SENT_DISCOVER_RESP);
  EXPECT_EQ(count_sent_cmd(radio, CMD_DISCOVER_RESP), 3) << "0x29 should be sent on all 3 channels";

  // Hub confirms the discovery directly to us; most hubs will not start the key exchange until
  // this is acknowledged. (A hub that skips it is covered by the pure-responder suite.)
  IoFrame discover_confirm{};
  init_frame(discover_confirm, true, true, false, false);
  set_dst(discover_confirm, throwaway_id);
  set_src(discover_confirm, FOREIGN_HUB_ID);
  ASSERT_TRUE(set_cmd(discover_confirm, CMD_DISCOVER_CONFIRM));
  comp.process_received_packet_(make_rx_packet(discover_confirm));
  EXPECT_EQ(comp.key_extraction_ctx_.state, pairing_responder::ResponderState::SENT_CONFIRM_ACK);
  EXPECT_EQ(count_sent_cmd(radio, CMD_DISCOVER_CONFIRM_ACK), 3) << "0x2D should be sent on all 3 channels";

  // Hub sends key-init to our throwaway ID.
  IoFrame key_init{};
  create_key_init(key_init, FOREIGN_HUB_ID, throwaway_id);
  comp.process_received_packet_(make_rx_packet(key_init));
  EXPECT_EQ(comp.key_extraction_ctx_.state, pairing_responder::ResponderState::SENT_CHALLENGE);
  EXPECT_EQ(count_sent_cmd(radio, CMD_CHALLENGE_REQ), 3) << "0x3C should be sent on all 3 channels";
  EXPECT_EQ(0, memcmp(comp.key_extraction_ctx_.hub_node_id, FOREIGN_HUB_ID, NODE_ID_SIZE));

  // Hub sends the real key-transfer, encrypted the way create_key_transfer() does.
  IoFrame key_init_frame_for_iv{};
  create_key_init(key_init_frame_for_iv, FOREIGN_HUB_ID, throwaway_id);
  IoFrame key_transfer{};
  const uint8_t foreign_system_key[AES_KEY_SIZE] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                                    0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10};
  ASSERT_TRUE(create_key_transfer(key_transfer, key_init_frame_for_iv, throwaway_id, FOREIGN_HUB_ID, foreign_system_key,
                                  comp.key_extraction_ctx_.challenge));
  comp.process_received_packet_(make_rx_packet(key_transfer));

  EXPECT_EQ(count_sent_cmd(radio, CMD_KEY_CONFIRM), 3) << "0x33 should be sent on all 3 channels";
  EXPECT_EQ(comp.key_extraction_ctx_.state, pairing_responder::ResponderState::EXTRACTED)
      << "responder should stay armed (not disarm immediately) in case the hub follows up with an "
         "address request";
  EXPECT_EQ(comp.last_timeout_name_, "key_extraction_post_extract_grace")
      << "extraction should arm the post-extraction grace timer, not disarm on the spot";

  ASSERT_TRUE(static_cast<bool>(comp.last_timeout_callback_));
  comp.last_timeout_callback_();
  EXPECT_EQ(comp.key_extraction_ctx_.state, pairing_responder::ResponderState::DISARMED)
      << "the grace timer firing with no further hub progress should disarm";
}

/// Not every hub waits for our CMD_DISCOVER_CONFIRM_ACK before starting the key exchange, so a
/// hub that goes straight from discovery to key-init must still reach a completed extraction —
/// reaching EXTRACTED here is that proof, since the responder only leaves SENT_CHALLENGE after
/// emitting the recovered-key log block. It no longer disarms on the spot: see
/// arm_post_extraction_grace_() for why the responder now stays armed for a grace window in case
/// the hub follows up with an address request (0x36).
TEST(HubKeyExtraction, ExchangeWithoutDiscoverConfirmStillExtractsKey) {
  TestableHubComponent comp;
  MockRadio radio;
  setup_component(comp, radio);
  comp.set_key_extraction_armed(true);
  uint8_t throwaway_id[NODE_ID_SIZE];
  memcpy(throwaway_id, comp.key_extraction_ctx_.throwaway_id, NODE_ID_SIZE);

  IoFrame discover{};
  create_discover(discover, FOREIGN_HUB_ID);
  comp.process_received_packet_(make_rx_packet(discover));
  ASSERT_EQ(comp.key_extraction_ctx_.state, pairing_responder::ResponderState::SENT_DISCOVER_RESP);

  // No 0x2C at all — straight to key-init.
  IoFrame key_init{};
  create_key_init(key_init, FOREIGN_HUB_ID, throwaway_id);
  comp.process_received_packet_(make_rx_packet(key_init));
  ASSERT_EQ(comp.key_extraction_ctx_.state, pairing_responder::ResponderState::SENT_CHALLENGE);
  EXPECT_EQ(count_sent_cmd(radio, CMD_CHALLENGE_REQ), 3);

  IoFrame key_transfer{};
  const uint8_t foreign_system_key[AES_KEY_SIZE] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
                                                    0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00};
  ASSERT_TRUE(create_key_transfer(key_transfer, key_init, throwaway_id, FOREIGN_HUB_ID, foreign_system_key,
                                  comp.key_extraction_ctx_.challenge));
  comp.process_received_packet_(make_rx_packet(key_transfer));

  EXPECT_EQ(count_sent_cmd(radio, CMD_KEY_CONFIRM), 3) << "0x33 should still be sent on all 3 channels";
  EXPECT_EQ(comp.key_extraction_ctx_.state, pairing_responder::ResponderState::EXTRACTED)
      << "a hub that skips the discovery-confirm step must still complete the extraction";
}

/// The literal real-world counterpart to the two tests above, closing the gap their own use of a
/// freshly-generated throwaway ID and a freshly-random challenge necessarily leaves: this test
/// scripts esp_random() (test_rng, tests/include/esp_random.h -- built for exactly this purpose,
/// per its own doxygen) with the exact bytes
/// tests/corpus/captures/issues/issue_45_velux_kig300_key_extraction_success.yaml records for a
/// real Velux KIG300 pairing session, so generate_key_extraction_throwaway_id_() and
/// crypto::generate_challenge() reproduce that session's real values, then feeds the session's
/// literal captured 0x28/0x2C/0x31 hub frames through the real dispatch path and checks the
/// transmitted 0x29/0x2D/0x3C are byte-identical to what this project's own firmware actually
/// transmitted on real hardware against a real hub. That capture's device side *is* this project's
/// own code (issue #45's hardware retest, not a third-party device) -- unlike
/// LiteralKlr200CaptureReplayThroughRealDispatchAnswersCorrectly above, this is a direct
/// reproducibility check: does today's code still produce the exact bytes it produced then? The
/// capture's own 0x32 (key transfer) is deliberately withheld from the corpus -- it would decrypt
/// to that real installation's real secret system key -- so this test cannot replay past the
/// challenge step with real bytes; a synthetic key transfer under a test key closes out the state
/// machine afterward, the same way every other full-exchange test here already does.
TEST(HubKeyExtraction, LiteralKig300CaptureReplayThroughRealDispatchReproducesHistoricalBytes) {
  const corpus::CorpusCapture *cap = corpus_test::capture_by_id("issue_45_velux_kig300_key_extraction_success");
  ASSERT_NE(cap, nullptr);

  const IoFrame hub_discover = find_capture_frame(cap, /*tx=*/true, CMD_DISCOVER_REQ);
  const IoFrame expected_discover_resp = find_capture_frame(cap, /*tx=*/false, CMD_DISCOVER_RESP);
  const IoFrame hub_discover_confirm = find_capture_frame(cap, /*tx=*/true, CMD_DISCOVER_CONFIRM);
  const IoFrame expected_discover_confirm_ack = find_capture_frame(cap, /*tx=*/false, CMD_DISCOVER_CONFIRM_ACK);
  // Not const: create_key_transfer() below takes its IV-source frame by mutable reference.
  IoFrame hub_key_init = find_capture_frame(cap, /*tx=*/true, CMD_KEY_INIT);
  const IoFrame expected_challenge_req = find_capture_frame(cap, /*tx=*/false, CMD_CHALLENGE_REQ);

  TestableHubComponent comp;
  MockRadio radio;
  setup_component(comp, radio);

  test_rng::reset();
  test_rng::enqueue_bytes(expected_discover_resp.src, NODE_ID_SIZE);
  comp.set_key_extraction_armed(true);
  ASSERT_EQ(memcmp(comp.key_extraction_ctx_.throwaway_id, expected_discover_resp.src, NODE_ID_SIZE), 0)
      << "RNG script did not land on the real session's throwaway ID -- check whether "
         "generate_key_extraction_throwaway_id_()'s retry/validity logic now consumes a different "
         "number of esp_random() draws";

  comp.process_received_packet_(make_rx_packet(hub_discover));
  {
    bool found = false;
    for (const auto &pkt : radio.get_sent_data()) {
      IoFrame sent{};
      if (!parse(pkt.data(), static_cast<uint8_t>(pkt.size()), sent) || sent.cmd != CMD_DISCOVER_RESP)
        continue;
      found = true;
      ASSERT_EQ(sent.data_len, expected_discover_resp.data_len);
      EXPECT_EQ(memcmp(sent.data, expected_discover_resp.data, sent.data_len), 0)
          << "0x29 payload must reproduce this real KIG300 session's captured 0x29";
    }
    EXPECT_TRUE(found);
  }
  radio.clear();

  comp.process_received_packet_(make_rx_packet(hub_discover_confirm));
  {
    bool found = false;
    for (const auto &pkt : radio.get_sent_data()) {
      IoFrame sent{};
      if (!parse(pkt.data(), static_cast<uint8_t>(pkt.size()), sent) || sent.cmd != CMD_DISCOVER_CONFIRM_ACK)
        continue;
      found = true;
      EXPECT_EQ(sent.data_len, expected_discover_confirm_ack.data_len) << "0x2D is a bare ack, no payload expected";
    }
    EXPECT_TRUE(found);
  }
  radio.clear();

  test_rng::enqueue_bytes(expected_challenge_req.data, HMAC_SIZE);
  comp.process_received_packet_(make_rx_packet(hub_key_init));
  test_rng::reset();
  {
    bool found = false;
    for (const auto &pkt : radio.get_sent_data()) {
      IoFrame sent{};
      if (!parse(pkt.data(), static_cast<uint8_t>(pkt.size()), sent) || sent.cmd != CMD_CHALLENGE_REQ)
        continue;
      found = true;
      ASSERT_EQ(sent.data_len, HMAC_SIZE);
      EXPECT_EQ(memcmp(sent.data, expected_challenge_req.data, HMAC_SIZE), 0)
          << "0x3C challenge must reproduce this real KIG300 session's captured challenge byte-for-byte";
    }
    EXPECT_TRUE(found);
  }
  EXPECT_EQ(memcmp(comp.key_extraction_ctx_.challenge, expected_challenge_req.data, HMAC_SIZE), 0);
  radio.clear();

  // The capture's own 0x32 is withheld (see this test's doxygen above) -- close out the state
  // machine with a synthetic key transfer under a test key, same as every other full-exchange test.
  const uint8_t test_key[AES_KEY_SIZE] = {0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28,
                                          0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F, 0x30};
  IoFrame key_transfer{};
  ASSERT_TRUE(create_key_transfer(key_transfer, hub_key_init, comp.key_extraction_ctx_.throwaway_id,
                                  comp.key_extraction_ctx_.hub_node_id, test_key, comp.key_extraction_ctx_.challenge));
  comp.process_received_packet_(make_rx_packet(key_transfer));
  EXPECT_EQ(comp.key_extraction_ctx_.state, pairing_responder::ResponderState::EXTRACTED);
  EXPECT_EQ(count_sent_cmd(radio, CMD_KEY_CONFIRM), 3);
}

TEST(HubKeyExtraction, SecondExtractionAttemptMidWindowIsIgnoredAfterFirstSucceeds) {
  TestableHubComponent comp;
  MockRadio radio;
  setup_component(comp, radio);
  comp.set_key_extraction_armed(true);
  uint8_t throwaway_id[NODE_ID_SIZE];
  memcpy(throwaway_id, comp.key_extraction_ctx_.throwaway_id, NODE_ID_SIZE);

  IoFrame discover{};
  create_discover(discover, FOREIGN_HUB_ID);
  comp.process_received_packet_(make_rx_packet(discover));
  IoFrame key_init{};
  create_key_init(key_init, FOREIGN_HUB_ID, throwaway_id);
  comp.process_received_packet_(make_rx_packet(key_init));
  IoFrame key_transfer{};
  const uint8_t foreign_system_key[AES_KEY_SIZE] = {0};
  ASSERT_TRUE(create_key_transfer(key_transfer, key_init, throwaway_id, FOREIGN_HUB_ID, foreign_system_key,
                                  comp.key_extraction_ctx_.challenge));
  comp.process_received_packet_(make_rx_packet(key_transfer));
  ASSERT_EQ(comp.key_extraction_ctx_.state, pairing_responder::ResponderState::EXTRACTED);
  radio.clear();

  // A second hub's discovery broadcast arrives in the same window, after the first extraction
  // already succeeded — must not respond. Still armed at this point (the grace window replaced the
  // immediate disarm), so this is on_discover_request()'s EXTRACTED guard doing the work, not the
  // DISARMED early-return in try_handle_key_extraction_frame_().
  IoFrame second_discover{};
  create_discover(second_discover, FOREIGN_HUB_ID);
  comp.process_received_packet_(make_rx_packet(second_discover));
  EXPECT_EQ(radio.get_send_count(), 0)
      << "a responder that already extracted a key must not reply to further discovery";
}

// ========================================================================================
// Disarmed: 0x28/0x2C/0x31/0x32 fall through unchanged, no reply
// ========================================================================================

TEST(HubKeyExtraction, DisarmedDoesNotRespondToDiscoveryOrKeyFrames) {
  TestableHubComponent comp;
  MockRadio radio;
  setup_component(comp, radio);
  // Never armed.

  const uint8_t some_id[NODE_ID_SIZE] = {0x11, 0x22, 0x33};

  IoFrame discover{};
  create_discover(discover, FOREIGN_HUB_ID);
  comp.process_received_packet_(make_rx_packet(discover));

  IoFrame discover_confirm{};
  init_frame(discover_confirm, true, true, false, false);
  set_dst(discover_confirm, some_id);
  set_src(discover_confirm, FOREIGN_HUB_ID);
  ASSERT_TRUE(set_cmd(discover_confirm, CMD_DISCOVER_CONFIRM));
  comp.process_received_packet_(make_rx_packet(discover_confirm));

  IoFrame key_init{};
  create_key_init(key_init, FOREIGN_HUB_ID, some_id);
  comp.process_received_packet_(make_rx_packet(key_init));

  EXPECT_EQ(radio.get_send_count(), 0) << "disarmed responder must never transmit";
}

// ========================================================================================
// Address disambiguation: 0x31 to our REAL node_id_ is not part of this flow
// ========================================================================================

/// A hub in pairing mode sends CMD_DISCOVER_CONFIRM to each of its *own* already-paired devices
/// as well, repeatedly and on every channel. Answering one would impersonate that device and put
/// a burst of bogus 0x2D frames on the air, so the responder must only ever answer a 0x2C
/// addressed to its throwaway ID.
TEST(HubKeyExtraction, DiscoverConfirmToAnotherDeviceIsIgnored) {
  TestableHubComponent comp;
  MockRadio radio;
  setup_component(comp, radio);
  comp.set_key_extraction_armed(true);

  IoFrame discover{};
  create_discover(discover, FOREIGN_HUB_ID);
  comp.process_received_packet_(make_rx_packet(discover));
  radio.clear();
  const auto state_before = comp.key_extraction_ctx_.state;

  const uint8_t other_device_id[NODE_ID_SIZE] = {0x58, 0x6E, 0x35};
  IoFrame discover_confirm{};
  init_frame(discover_confirm, true, true, false, false);
  set_dst(discover_confirm, other_device_id);
  set_src(discover_confirm, FOREIGN_HUB_ID);
  ASSERT_TRUE(set_cmd(discover_confirm, CMD_DISCOVER_CONFIRM));
  comp.process_received_packet_(make_rx_packet(discover_confirm));

  EXPECT_EQ(comp.key_extraction_ctx_.state, state_before) << "responder state must be untouched";
  EXPECT_EQ(radio.get_send_count(), 0) << "responder must not answer a 0x2C addressed to another device";
}

/// The 0x36 twin of DiscoverConfirmToAnotherDeviceIsIgnored above: pins that the same dst guard
/// (hub_key_extraction.cpp) covers the new CMD_ADDRESS_REQ branch too.
TEST(HubKeyExtraction, AddressReqToAnotherDeviceIsIgnored) {
  TestableHubComponent comp;
  MockRadio radio;
  setup_component(comp, radio);
  comp.set_key_extraction_armed(true);
  comp.key_extraction_ctx_.state = pairing_responder::ResponderState::EXTRACTED;  // simulate a completed key exchange
  radio.clear();

  const uint8_t other_device_id[NODE_ID_SIZE] = {0x58, 0x6E, 0x35};
  IoFrame address_req{};
  init_frame(address_req, true, true, false, false);
  set_dst(address_req, other_device_id);
  set_src(address_req, FOREIGN_HUB_ID);
  ASSERT_TRUE(set_cmd(address_req, CMD_ADDRESS_REQ));
  comp.process_received_packet_(make_rx_packet(address_req));

  EXPECT_EQ(comp.key_extraction_ctx_.state, pairing_responder::ResponderState::EXTRACTED)
      << "responder state must be untouched";
  EXPECT_EQ(radio.get_send_count(), 0) << "responder must not answer a 0x36 addressed to another device";
}

/// Our throwaway ID goes out in clear in our own 0x29/0x37, so the dst guard alone (exercised
/// above) doesn't establish a 0x36 actually came from the hub we exchanged keys with. This pins the
/// added src check in handle_key_extraction_address_req_(): a frame correctly addressed to our
/// throwaway ID but from anyone other than key_extraction_ctx_.hub_node_id must still be ignored.
TEST(HubKeyExtraction, AddressReqFromWrongHubIsIgnored) {
  TestableHubComponent comp;
  MockRadio radio;
  setup_component(comp, radio);
  comp.set_key_extraction_armed(true);
  comp.key_extraction_ctx_.state = pairing_responder::ResponderState::EXTRACTED;  // simulate a completed key exchange
  memcpy(comp.key_extraction_ctx_.hub_node_id, FOREIGN_HUB_ID, NODE_ID_SIZE);
  uint8_t throwaway_id[NODE_ID_SIZE];
  memcpy(throwaway_id, comp.key_extraction_ctx_.throwaway_id, NODE_ID_SIZE);
  radio.clear();

  const uint8_t attacker_id[NODE_ID_SIZE] = {0x58, 0x6E, 0x35};
  IoFrame address_req{};
  init_frame(address_req, true, true, false, false);
  set_dst(address_req, throwaway_id);
  set_src(address_req, attacker_id);
  ASSERT_TRUE(set_cmd(address_req, CMD_ADDRESS_REQ));
  comp.process_received_packet_(make_rx_packet(address_req));

  EXPECT_EQ(comp.key_extraction_ctx_.state, pairing_responder::ResponderState::EXTRACTED)
      << "responder state must be untouched";
  EXPECT_EQ(radio.get_send_count(), 0)
      << "responder must not answer a 0x36 from anyone but the hub it actually exchanged keys with";
}

/// The 0x3C twin of AddressReqFromWrongHubIsIgnored above: pins the same src guard in
/// handle_key_extraction_address_challenge_().
TEST(HubKeyExtraction, AddressChallengeFromWrongHubIsIgnored) {
  TestableHubComponent comp;
  MockRadio radio;
  setup_component(comp, radio);
  comp.set_key_extraction_armed(true);
  comp.key_extraction_ctx_.state = pairing_responder::ResponderState::SENT_ADDRESS_RESP;
  memcpy(comp.key_extraction_ctx_.hub_node_id, FOREIGN_HUB_ID, NODE_ID_SIZE);
  uint8_t throwaway_id[NODE_ID_SIZE];
  memcpy(throwaway_id, comp.key_extraction_ctx_.throwaway_id, NODE_ID_SIZE);
  radio.clear();

  const uint8_t attacker_id[NODE_ID_SIZE] = {0x58, 0x6E, 0x35};
  IoFrame address_challenge{};
  ASSERT_TRUE(create_challenge_req(address_challenge, throwaway_id, attacker_id, test::TEST_CHALLENGE));
  comp.process_received_packet_(make_rx_packet(address_challenge));

  EXPECT_EQ(comp.key_extraction_ctx_.state, pairing_responder::ResponderState::SENT_ADDRESS_RESP)
      << "responder state must be untouched";
  EXPECT_EQ(radio.get_send_count(), 0)
      << "responder must not answer a hub-issued 0x3C from anyone but the hub it actually exchanged keys with";
}

TEST(HubKeyExtraction, KeyInitToRealNodeIdIsNotHijackedByResponder) {
  TestableHubComponent comp;
  MockRadio radio;
  setup_component(comp, radio);
  comp.set_key_extraction_armed(true);
  const auto state_before = comp.key_extraction_ctx_.state;

  // A key-init addressed to our REAL node_id_ (not the throwaway ID) — e.g. another
  // controller's own pairing traffic overheard on the shared channel.
  IoFrame key_init{};
  create_key_init(key_init, FOREIGN_HUB_ID, OUR_NODE_ID);
  comp.process_received_packet_(make_rx_packet(key_init));

  EXPECT_EQ(comp.key_extraction_ctx_.state, state_before) << "responder state must be untouched";
  EXPECT_EQ(radio.get_send_count(), 0) << "responder must not reply to a key-init addressed to our real node ID";
}

// ========================================================================================
// Re-arming after a completed attempt
// ========================================================================================

TEST(HubKeyExtraction, RearmAfterExtractionStartsFreshAttempt) {
  TestableHubComponent comp;
  MockRadio radio;
  setup_component(comp, radio);
  comp.set_key_extraction_armed(true);
  comp.key_extraction_ctx_.state = pairing_responder::ResponderState::EXTRACTED;  // simulate a completed attempt

  comp.set_key_extraction_armed(false);
  comp.set_key_extraction_armed(true);

  EXPECT_EQ(comp.key_extraction_ctx_.state, pairing_responder::ResponderState::ARMED_IDLE);
}

/// Regression guard for the bug the state-set guard inside arm_post_extraction_grace_()'s callback
/// fixes: a grace timer armed against a *previous* extraction must not disarm a *new* arm cycle
/// that happens to be re-armed inside the same window. The host Component stub holds exactly one
/// timeout slot regardless of name (tests/include/esphome/core/component.h), which is what makes
/// this constructible: capturing the callback before disarm/re-arm and firing it afterward
/// exercises a stricter situation than production can produce (ESPHome's real named-timer
/// replacement would have destroyed the stale callback the moment the new cycle reached
/// EXTRACTED/SENT_ADDRESS_RESP) -- but the guard is written to be correct regardless, and this is
/// the strongest test the host harness can offer for it.
TEST(HubKeyExtraction, StaleGraceTimeoutDoesNotDisarmARearmedWindow) {
  TestableHubComponent comp;
  MockRadio radio;
  setup_component(comp, radio);
  comp.set_key_extraction_armed(true);
  comp.key_extraction_ctx_.state = pairing_responder::ResponderState::EXTRACTED;  // simulate a completed attempt
  comp.arm_post_extraction_grace_();
  auto stale_grace_callback = comp.last_timeout_callback_;

  comp.set_key_extraction_armed(false);
  comp.set_key_extraction_armed(true);
  ASSERT_EQ(comp.key_extraction_ctx_.state, pairing_responder::ResponderState::ARMED_IDLE);

  stale_grace_callback();

  EXPECT_EQ(comp.key_extraction_ctx_.state, pairing_responder::ResponderState::ARMED_IDLE)
      << "a grace timer from the previous arm cycle must not disarm the new one";
}

/// At the 60-second grace window this is the likely path, not a corner case: a user watching the
/// log sees the key and turns the switch off before the window elapses. Manual disarm must be
/// clean -- no double-firing of the armed callback, and the (now stale) grace callback firing
/// afterward must be a no-op.
TEST(HubKeyExtraction, ManualDisarmDuringGraceWindowIsClean) {
  TestableHubComponent comp;
  MockRadio radio;
  setup_component(comp, radio);
  int armed_false_count = 0;
  comp.set_key_extraction_armed_callback([&](bool armed) {
    if (!armed)
      armed_false_count++;
  });
  comp.set_key_extraction_armed(true);
  comp.key_extraction_ctx_.state = pairing_responder::ResponderState::EXTRACTED;  // simulate a completed attempt
  comp.arm_post_extraction_grace_();
  auto stale_grace_callback = comp.last_timeout_callback_;

  comp.set_key_extraction_armed(false);
  EXPECT_EQ(armed_false_count, 1);
  EXPECT_EQ(comp.key_extraction_ctx_.state, pairing_responder::ResponderState::DISARMED);

  stale_grace_callback();

  EXPECT_EQ(comp.key_extraction_ctx_.state, pairing_responder::ResponderState::DISARMED)
      << "a stale grace callback after a manual disarm must be a no-op";
  EXPECT_EQ(armed_false_count, 1) << "the armed-false callback must not fire a second time";
}

// ========================================================================================
// Auto-off timeout: partial-progress and no-attempt phrasing (behavioral smoke test — the
// message text itself is exercised via the callback, not asserted verbatim here).
// ========================================================================================

TEST(HubKeyExtraction, AutoOffTimeoutCallbackDisarms) {
  TestableHubComponent comp;
  MockRadio radio;
  setup_component(comp, radio);
  comp.set_key_extraction_armed(true);
  ASSERT_TRUE(static_cast<bool>(comp.last_timeout_callback_));

  auto timeout_cb = comp.last_timeout_callback_;
  timeout_cb();

  EXPECT_EQ(comp.key_extraction_ctx_.state, pairing_responder::ResponderState::DISARMED);
}

// ========================================================================================
// Address verification: 0x36 -> 0x37, hub-issued 0x3C -> 0x3D. Some hubs (Velux KLR200) run this
// round after the key exchange to verify the backbone address they were handed; others (Velux
// KIG300) never send it. Both must work through the real dispatch path in hub_status.cpp, not just
// the pure pairing_responder.cpp guards.
// ========================================================================================

/// Regression guard for the ordering fix in process_received_packet_() (hub_status.cpp): an
/// inbound 0x3C addressed to our throwaway ID must reach the key-extraction responder before
/// is_exchange_internal_command()'s 0x3C/0x3D early-drop gets a chance to discard it. Without that
/// ordering, this test fails with "0 sent 0x3D" -- the handler is never even called -- and it is
/// the only test in this suite that would catch a future tidy-up that re-merges the two early
/// returns in hub_status.cpp.
TEST(HubKeyExtraction, ChallengeReqReachesResponderBeforeExchangeInternalDrop) {
  TestableHubComponent comp;
  MockRadio radio;
  setup_component(comp, radio);
  comp.set_key_extraction_armed(true);
  comp.key_extraction_ctx_.state = pairing_responder::ResponderState::SENT_ADDRESS_RESP;
  // Simulating a mid-flow state directly (rather than driving it through on_key_init()) skips the
  // step that would normally capture this -- set it explicitly so the src guard in
  // handle_key_extraction_address_challenge_() doesn't reject FOREIGN_HUB_ID below.
  memcpy(comp.key_extraction_ctx_.hub_node_id, FOREIGN_HUB_ID, NODE_ID_SIZE);

  IoFrame challenge_req{};
  ASSERT_TRUE(
      create_challenge_req(challenge_req, comp.key_extraction_ctx_.throwaway_id, FOREIGN_HUB_ID, test::TEST_CHALLENGE));
  comp.process_received_packet_(make_rx_packet(challenge_req));

  EXPECT_EQ(count_sent_cmd(radio, CMD_CHALLENGE_RESP), 3)
      << "a hub-issued 0x3C addressed to our throwaway ID while SENT_ADDRESS_RESP must draw a 0x3D "
         "-- if this is 0, the reorder in process_received_packet_() regressed";
}

/// The full KLR200 sequence this feature exists for: discovery through the address-verification
/// round, driven through the real dispatch path with the project's own builders (not raw capture
/// bytes -- the capture's own node IDs/key/challenge don't match this harness's generated
/// throwaway ID or scripted key, so the frames are built the way the other full-exchange tests in
/// this file already do; the literal capture bytes are pinned separately by
/// corpus_crypto_test.cpp::ChallengeRespDeviceRoleReproducesKlr200AddressProof and by
/// corpus_device_role_builder_test.cpp's framing pins). Asserts the responder answers a 0x36 and a
/// hub-issued 0x3C, and does NOT disarm until the grace timer actually fires.
TEST(HubKeyExtraction, FullExchangeThroughAddressVerificationDisarmsAfterChallenge) {
  TestableHubComponent comp;
  MockRadio radio;
  setup_component(comp, radio);
  comp.set_key_extraction_armed(true);
  uint8_t throwaway_id[NODE_ID_SIZE];
  memcpy(throwaway_id, comp.key_extraction_ctx_.throwaway_id, NODE_ID_SIZE);

  IoFrame discover{};
  create_discover(discover, FOREIGN_HUB_ID);
  comp.process_received_packet_(make_rx_packet(discover));

  IoFrame key_init{};
  create_key_init(key_init, FOREIGN_HUB_ID, throwaway_id);
  comp.process_received_packet_(make_rx_packet(key_init));

  IoFrame key_transfer{};
  const uint8_t foreign_system_key[AES_KEY_SIZE] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                                    0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10};
  ASSERT_TRUE(create_key_transfer(key_transfer, key_init, throwaway_id, FOREIGN_HUB_ID, foreign_system_key,
                                  comp.key_extraction_ctx_.challenge));
  comp.process_received_packet_(make_rx_packet(key_transfer));
  ASSERT_EQ(comp.key_extraction_ctx_.state, pairing_responder::ResponderState::EXTRACTED);
  radio.clear();

  // Hub verifies the address it was handed: CMD_ADDRESS_REQ (0x36).
  IoFrame address_req{};
  init_frame(address_req, true, true, false, false);
  set_dst(address_req, throwaway_id);
  set_src(address_req, FOREIGN_HUB_ID);
  ASSERT_TRUE(set_cmd(address_req, CMD_ADDRESS_REQ));
  comp.process_received_packet_(make_rx_packet(address_req));
  EXPECT_EQ(count_sent_cmd(radio, CMD_ADDRESS_RESP), 3) << "0x37 should be sent on all 3 channels";
  EXPECT_EQ(comp.key_extraction_ctx_.state, pairing_responder::ResponderState::SENT_ADDRESS_RESP);

  // Hub challenges the 0x37 it got back: CMD_CHALLENGE_REQ (0x3C), controller-issued this time.
  IoFrame address_challenge{};
  ASSERT_TRUE(create_challenge_req(address_challenge, throwaway_id, FOREIGN_HUB_ID, test::TEST_CHALLENGE));
  comp.process_received_packet_(make_rx_packet(address_challenge));
  EXPECT_EQ(count_sent_cmd(radio, CMD_CHALLENGE_RESP), 3) << "0x3D should be sent on all 3 channels";
  EXPECT_EQ(comp.key_extraction_ctx_.state, pairing_responder::ResponderState::SENT_ADDRESS_RESP)
      << "must not disarm on the spot -- the grace timer does that";

  // The honest payload assertion (per corpus_crypto_test.cpp's doxygen): equal to
  // create_hmac() over our own 0x37's [cmd, data...] using the hub's challenge and the key this
  // harness actually recovered -- not the KLR200 capture's literal bytes, which were produced
  // under a different node ID/key/challenge triple.
  IoFrame our_address_resp{};
  ASSERT_TRUE(create_address_resp_device_role(our_address_resp, throwaway_id, FOREIGN_HUB_ID));
  uint8_t transcript[HMAC_SIZE + 1] = {0};
  transcript[0] = our_address_resp.cmd;
  memcpy(transcript + 1, our_address_resp.data, our_address_resp.data_len);
  const uint8_t transcript_len = static_cast<uint8_t>(our_address_resp.data_len + 1);
  uint8_t expected_hmac[HMAC_SIZE] = {0};
  ASSERT_TRUE(crypto::create_hmac(transcript, transcript_len, test::TEST_CHALLENGE,
                                  comp.key_extraction_ctx_.recovered_key, expected_hmac));

  const auto &sent = radio.get_sent_data();
  bool found_3d = false;
  for (const auto &pkt : sent) {
    IoFrame sent_frame{};
    if (!parse(pkt.data(), static_cast<uint8_t>(pkt.size()), sent_frame))
      continue;
    if (sent_frame.cmd != CMD_CHALLENGE_RESP)
      continue;
    found_3d = true;
    EXPECT_EQ(sent_frame.data_len, HMAC_SIZE);
    EXPECT_EQ(memcmp(sent_frame.data, expected_hmac, HMAC_SIZE), 0)
        << "0x3D payload should equal create_hmac() over our own 0x37 under the recovered key";
  }
  EXPECT_TRUE(found_3d);

  ASSERT_TRUE(static_cast<bool>(comp.last_timeout_callback_));
  comp.last_timeout_callback_();
  EXPECT_EQ(comp.key_extraction_ctx_.state, pairing_responder::ResponderState::DISARMED);
}

/// The literal real-world counterpart to the test above, and the reason it does *not* assert a
/// byte-exact match against the capture's own 0x37/0x3D: seeding the responder to this capture's
/// exact node ID / hub ID / key and feeding its literal 0x36 through the real dispatch path
/// revealed that the captured 0x37 payload (`B6 2B BB`) is NOT this device's own node ID
/// (`38 ED A1`, the address the whole rest of the session — including the 0x36 that provoked this
/// 0x37 — addresses it by). tests/corpus/captures/velux_kux100/pairing_full.yaml's own note says
/// so explicitly: 0x37's payload is that device's separately-tracked "backbone" identity, not its
/// node/session address. create_address_resp_device_role() reports our one and only identity for
/// both (see its doxygen in proto_commands.h), which this capture's own bytes prove is not what at
/// least one real device does — so this test checks what our builder can actually be held to: the
/// dispatch path answers the literal captured 0x36 with our own real node ID, and the literal
/// captured hub challenge from this session's own 0x3C authenticates correctly against whatever we
/// actually sent. corpus_crypto_test.cpp::ChallengeRespDeviceRoleReproducesKlr200AddressProof
/// separately pins the capture's *own* 0x3D bytes via a direct create_hmac() call over the
/// capture's own (not our) 0x37 data — proving the crypto primitive reproduces a real device's
/// output, distinct from what this test proves about our dispatch path.
TEST(HubKeyExtraction, LiteralKlr200CaptureReplayThroughRealDispatchAnswersCorrectly) {
  const corpus::CorpusCapture *cap = corpus_test::capture_by_id("velux_kux100_pairing_full");
  ASSERT_NE(cap, nullptr);

  const IoFrame address_req = find_capture_frame(cap, /*tx=*/true, CMD_ADDRESS_REQ);
  const IoFrame address_challenge = find_capture_frame(cap, /*tx=*/true, CMD_CHALLENGE_REQ);
  // Sanity checks on the capture itself, so a future edit that shrinks/grows either payload fails
  // here with a clear message instead of a confusing mismatch below.
  ASSERT_EQ(find_capture_frame(cap, /*tx=*/false, CMD_ADDRESS_RESP).data_len, NODE_ID_SIZE);
  ASSERT_EQ(address_challenge.data_len, HMAC_SIZE);

  TestableHubComponent comp;
  MockRadio radio;
  setup_component(comp, radio);
  comp.set_key_extraction_armed(true);
  comp.key_extraction_ctx_.state = pairing_responder::ResponderState::EXTRACTED;
  // Our own address and the hub's, exactly as this real session used them -- the captured 0x36's
  // dst/src pair, not a value this harness generated. test::TEST_SYSTEM_KEY is byte-identical to
  // this capture's `key: corpus` value (both trace to scripts/corpus/protolib.py's
  // CORPUS_SYSTEM_KEY), so it decrypts/authenticates exactly as the real session's key did.
  memcpy(comp.key_extraction_ctx_.throwaway_id, address_req.dst, NODE_ID_SIZE);
  memcpy(comp.key_extraction_ctx_.hub_node_id, address_req.src, NODE_ID_SIZE);
  memcpy(comp.key_extraction_ctx_.recovered_key, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);
  radio.clear();

  comp.process_received_packet_(make_rx_packet(address_req));
  EXPECT_EQ(comp.key_extraction_ctx_.state, pairing_responder::ResponderState::SENT_ADDRESS_RESP);
  IoFrame our_address_resp{};
  bool have_our_address_resp = false;
  for (const auto &pkt : radio.get_sent_data()) {
    IoFrame sent{};
    if (!parse(pkt.data(), static_cast<uint8_t>(pkt.size()), sent) || sent.cmd != CMD_ADDRESS_RESP)
      continue;
    have_our_address_resp = true;
    our_address_resp = sent;
    ASSERT_EQ(sent.data_len, NODE_ID_SIZE);
    EXPECT_EQ(memcmp(sent.data, comp.key_extraction_ctx_.throwaway_id, NODE_ID_SIZE), 0)
        << "0x37 payload must be our own advertised node ID";
  }
  EXPECT_TRUE(have_our_address_resp);
  radio.clear();

  comp.process_received_packet_(make_rx_packet(address_challenge));
  uint8_t transcript[HMAC_SIZE + 1] = {0};
  transcript[0] = our_address_resp.cmd;
  memcpy(transcript + 1, our_address_resp.data, our_address_resp.data_len);
  uint8_t expected_hmac[HMAC_SIZE] = {0};
  ASSERT_TRUE(crypto::create_hmac(transcript, static_cast<uint8_t>(our_address_resp.data_len + 1),
                                  address_challenge.data, comp.key_extraction_ctx_.recovered_key, expected_hmac));
  bool found_0x3d = false;
  for (const auto &pkt : radio.get_sent_data()) {
    IoFrame sent{};
    if (!parse(pkt.data(), static_cast<uint8_t>(pkt.size()), sent) || sent.cmd != CMD_CHALLENGE_RESP)
      continue;
    found_0x3d = true;
    ASSERT_EQ(sent.data_len, HMAC_SIZE);
    EXPECT_EQ(memcmp(sent.data, expected_hmac, HMAC_SIZE), 0)
        << "0x3D payload must authenticate our own 0x37 under this real session's own captured "
           "hub challenge and key";
  }
  EXPECT_TRUE(found_0x3d);
}

/// Regression guard for the KIG300 family (issue #45): a hub that completes the key exchange and
/// sends nothing further must still disarm, just after the grace window instead of immediately.
TEST(HubKeyExtraction, GraceWindowExpiresWithoutAddressRequest) {
  TestableHubComponent comp;
  MockRadio radio;
  setup_component(comp, radio);
  comp.set_key_extraction_armed(true);
  uint8_t throwaway_id[NODE_ID_SIZE];
  memcpy(throwaway_id, comp.key_extraction_ctx_.throwaway_id, NODE_ID_SIZE);

  IoFrame discover{};
  create_discover(discover, FOREIGN_HUB_ID);
  comp.process_received_packet_(make_rx_packet(discover));
  IoFrame key_init{};
  create_key_init(key_init, FOREIGN_HUB_ID, throwaway_id);
  comp.process_received_packet_(make_rx_packet(key_init));
  IoFrame key_transfer{};
  const uint8_t foreign_system_key[AES_KEY_SIZE] = {0};
  ASSERT_TRUE(create_key_transfer(key_transfer, key_init, throwaway_id, FOREIGN_HUB_ID, foreign_system_key,
                                  comp.key_extraction_ctx_.challenge));
  comp.process_received_packet_(make_rx_packet(key_transfer));
  ASSERT_EQ(comp.key_extraction_ctx_.state, pairing_responder::ResponderState::EXTRACTED);

  EXPECT_EQ(comp.last_timeout_name_, "key_extraction_post_extract_grace");
  EXPECT_EQ(comp.last_timeout_ms_, 60000u) << "grace window should be one minute";

  ASSERT_TRUE(static_cast<bool>(comp.last_timeout_callback_));
  comp.last_timeout_callback_();
  EXPECT_EQ(comp.key_extraction_ctx_.state, pairing_responder::ResponderState::DISARMED)
      << "a hub that never follows up with 0x36 should still disarm once the grace window elapses";
}

/// A 0x36 arriving after extraction is itself a sign of hub progress and should push the disarm
/// back out, the same way the initial extraction armed it. The host Component stub holds one
/// timeout slot regardless of name, so this can only observe re-registration, not cancellation of
/// the timer it replaced -- real cancellation semantics come from ESPHome's named-timer
/// replacement and are not covered here (see StaleGraceTimeoutDoesNotDisarmARearmedWindow above).
///
/// The stub's recorded last_timeout_name_/last_timeout_ms_ are reset immediately before the 0x36
/// is fed in, not just asserted afterward: arm_post_extraction_grace_() already ran once during the
/// key-transfer step above, so those fields already show the grace timer's name/duration *before*
/// handle_key_extraction_address_req_() runs. Without the reset, this test would pass even if the
/// 0x36 handler's arm_post_extraction_grace_() call were deleted entirely -- it did, verified by
/// deliberately removing that call and confirming this test failed, then restoring it.
TEST(HubKeyExtraction, AddressRequestExtendsGraceWindow) {
  TestableHubComponent comp;
  MockRadio radio;
  setup_component(comp, radio);
  comp.set_key_extraction_armed(true);
  uint8_t throwaway_id[NODE_ID_SIZE];
  memcpy(throwaway_id, comp.key_extraction_ctx_.throwaway_id, NODE_ID_SIZE);

  IoFrame discover{};
  create_discover(discover, FOREIGN_HUB_ID);
  comp.process_received_packet_(make_rx_packet(discover));
  IoFrame key_init{};
  create_key_init(key_init, FOREIGN_HUB_ID, throwaway_id);
  comp.process_received_packet_(make_rx_packet(key_init));
  IoFrame key_transfer{};
  const uint8_t foreign_system_key[AES_KEY_SIZE] = {0};
  ASSERT_TRUE(create_key_transfer(key_transfer, key_init, throwaway_id, FOREIGN_HUB_ID, foreign_system_key,
                                  comp.key_extraction_ctx_.challenge));
  comp.process_received_packet_(make_rx_packet(key_transfer));
  ASSERT_EQ(comp.key_extraction_ctx_.state, pairing_responder::ResponderState::EXTRACTED);

  // Clear the grace timer's own prior registration so re-registration by the 0x36 handler below is
  // actually observable -- see the doxygen above.
  comp.last_timeout_name_.clear();
  comp.last_timeout_ms_ = 0;

  IoFrame address_req{};
  init_frame(address_req, true, true, false, false);
  set_dst(address_req, throwaway_id);
  set_src(address_req, FOREIGN_HUB_ID);
  ASSERT_TRUE(set_cmd(address_req, CMD_ADDRESS_REQ));
  comp.process_received_packet_(make_rx_packet(address_req));

  EXPECT_EQ(comp.last_timeout_name_, "key_extraction_post_extract_grace")
      << "after the 0x36, the recorded timer should be the grace timer again";
  EXPECT_EQ(comp.last_timeout_ms_, 60000u);
}

/// Mirror of AddressRequestExtendsGraceWindow above for the other re-arm call site: a hub-issued
/// 0x3C challenging our 0x37 is also a sign of progress and must push the disarm back out too. Same
/// reset-before-stimulus technique, for the same reason -- arm_post_extraction_grace_() already ran
/// once when SENT_ADDRESS_RESP was reached via the 0x36 above, so the stub's recorded fields already
/// show the grace timer before the 0x3C handler runs.
TEST(HubKeyExtraction, AddressChallengeExtendsGraceWindow) {
  TestableHubComponent comp;
  MockRadio radio;
  setup_component(comp, radio);
  comp.set_key_extraction_armed(true);
  uint8_t throwaway_id[NODE_ID_SIZE];
  memcpy(throwaway_id, comp.key_extraction_ctx_.throwaway_id, NODE_ID_SIZE);

  IoFrame discover{};
  create_discover(discover, FOREIGN_HUB_ID);
  comp.process_received_packet_(make_rx_packet(discover));
  IoFrame key_init{};
  create_key_init(key_init, FOREIGN_HUB_ID, throwaway_id);
  comp.process_received_packet_(make_rx_packet(key_init));
  IoFrame key_transfer{};
  const uint8_t foreign_system_key[AES_KEY_SIZE] = {0};
  ASSERT_TRUE(create_key_transfer(key_transfer, key_init, throwaway_id, FOREIGN_HUB_ID, foreign_system_key,
                                  comp.key_extraction_ctx_.challenge));
  comp.process_received_packet_(make_rx_packet(key_transfer));
  ASSERT_EQ(comp.key_extraction_ctx_.state, pairing_responder::ResponderState::EXTRACTED);

  IoFrame address_req{};
  init_frame(address_req, true, true, false, false);
  set_dst(address_req, throwaway_id);
  set_src(address_req, FOREIGN_HUB_ID);
  ASSERT_TRUE(set_cmd(address_req, CMD_ADDRESS_REQ));
  comp.process_received_packet_(make_rx_packet(address_req));
  ASSERT_EQ(comp.key_extraction_ctx_.state, pairing_responder::ResponderState::SENT_ADDRESS_RESP);

  // Clear the grace timer's own prior registration (from the 0x36 above) so re-registration by the
  // 0x3C handler below is actually observable.
  comp.last_timeout_name_.clear();
  comp.last_timeout_ms_ = 0;

  IoFrame address_challenge{};
  ASSERT_TRUE(create_challenge_req(address_challenge, throwaway_id, FOREIGN_HUB_ID, test::TEST_CHALLENGE));
  comp.process_received_packet_(make_rx_packet(address_challenge));

  EXPECT_EQ(comp.last_timeout_name_, "key_extraction_post_extract_grace")
      << "after the hub-issued 0x3C, the recorded timer should be the grace timer again";
  EXPECT_EQ(comp.last_timeout_ms_, 60000u);
}

// ========================================================================================
// The ready-to-paste extraction report
// ========================================================================================
// build_key_extraction_report() is pure, so these assert the exact text a user will be asked to
// copy — mirroring HubOneWayKeyAdoption's report tests (tests/hub_oneway_key_adoption_test.cpp),
// the host ESP_LOG stub discards its arguments, so testing the builder directly is the only way
// to pin the report's contents.

TEST(HubKeyExtraction, ReportContainsPasteableBlockAndKeyExactlyOnce) {
  constexpr uint8_t node_id[NODE_ID_SIZE] = {0xAB, 0xCD, 0xEF};
  constexpr uint8_t key[AES_KEY_SIZE] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                         0x09, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16};
  const std::string report = detail::build_key_extraction_report(node_id, key);

  EXPECT_NE(report.find("home_io_control:"), std::string::npos);
  EXPECT_NE(report.find("node_id: \"ABCDEF\""), std::string::npos);
  EXPECT_NE(report.find("system_key: \"01020304050607080910111213141516\""), std::string::npos);

  const std::string key_hex = "01020304050607080910111213141516";
  const size_t first = report.find(key_hex);
  ASSERT_NE(first, std::string::npos) << "the recovered key must be present for the user to copy";
  EXPECT_EQ(report.find(key_hex, first + 1), std::string::npos) << "and must appear exactly once";
}

TEST(HubKeyExtraction, ReportNeverClaimsTheKeyIsConfirmed) {
  // This exchange is never independently confirmed against the specific hub it came from (see
  // log_key_extraction_result_()'s file-level @warning) -- wording that implied otherwise would
  // be a claim this feature cannot support.
  constexpr uint8_t node_id[NODE_ID_SIZE] = {0xAB, 0xCD, 0xEF};
  constexpr uint8_t key[AES_KEY_SIZE] = {0};
  const std::string report = detail::build_key_extraction_report(node_id, key);

  EXPECT_NE(report.find("has not been independently confirmed"), std::string::npos);
  for (const char *forbidden : {"success", "confirmed key", "verified key"})
    EXPECT_EQ(report.find(forbidden), std::string::npos) << "the report must not contain '" << forbidden << "'";
}

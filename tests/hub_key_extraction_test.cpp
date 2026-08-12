#include "hub_core.h"
#include "hub_internal.h"
#include "pairing_responder.h"
#include "proto_commands.h"
#include "proto_crypto.h"
#include "proto_frame.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"

#include <esp_random.h>

#include <cstring>

using namespace esphome::home_io_control;
using test::TestableHubComponent;

// ============================================================================
// HubKeyExtraction test suite
// ============================================================================
// "Accept Foreign Pairing (Key Extraction)" hub wiring: arm/disarm, the 0x28/0x31/0x32 RX
// branches, address disambiguation against the real node_id_, auto-off timeout scheduling, and
// the disarm-on-extraction guarantee.

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
// Full RX flow: 0x28 -> 0x29, 0x31 -> 0x3C, 0x32 -> 0x33 + extraction + auto-disarm
// ========================================================================================

TEST(HubKeyExtraction, FullExchangeExtractsKeyAndAutoDisarms) {
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
  EXPECT_EQ(comp.key_extraction_ctx_.state, pairing_responder::ResponderState::DISARMED)
      << "responder should auto-disarm immediately after extraction";
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
  ASSERT_EQ(comp.key_extraction_ctx_.state, pairing_responder::ResponderState::DISARMED);
  radio.clear();

  // A second hub's discovery broadcast arrives in the same 10-minute window, after we already
  // disarmed — must not respond.
  IoFrame second_discover{};
  create_discover(second_discover, FOREIGN_HUB_ID);
  comp.process_received_packet_(make_rx_packet(second_discover));
  EXPECT_EQ(radio.get_send_count(), 0) << "a disarmed responder must not reply to further discovery";
}

// ========================================================================================
// Disarmed: 0x28/0x31/0x32 fall through unchanged, no reply
// ========================================================================================

TEST(HubKeyExtraction, DisarmedDoesNotRespondToDiscoveryOrKeyFrames) {
  TestableHubComponent comp;
  MockRadio radio;
  setup_component(comp, radio);
  // Never armed.

  IoFrame discover{};
  create_discover(discover, FOREIGN_HUB_ID);
  comp.process_received_packet_(make_rx_packet(discover));

  const uint8_t some_id[NODE_ID_SIZE] = {0x11, 0x22, 0x33};
  IoFrame key_init{};
  create_key_init(key_init, FOREIGN_HUB_ID, some_id);
  comp.process_received_packet_(make_rx_packet(key_init));

  EXPECT_EQ(radio.get_send_count(), 0) << "disarmed responder must never transmit";
}

// ========================================================================================
// Address disambiguation: 0x31 to our REAL node_id_ is not part of this flow
// ========================================================================================

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

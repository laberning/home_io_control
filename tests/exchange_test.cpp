#include "hub_exchange.h"
#include "hub_core.h"
#include "proto_frame.h"
#include "proto_commands.h"
#include "proto_crypto.h"
#include "radio_sx1276.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"

#include <cstring>
#include <deque>
#include <vector>

using namespace esphome::home_io_control;

// ============================================================================
// Exchange test suite
// ============================================================================
// Outbound exchange state machine tests (hub_exchange.cpp) and inbound auth
// state transitions. Focus on state-machine transitions and context struct validity.

// Helper to build exchange context
exchange::OutboundExchangeContext make_outbound_context() {
  exchange::OutboundExchangeContext ctx;
  ctx.state = exchange::OutboundExchangeState::IDLE;
  ctx.try_index = 0;
  ctx.saw_challenge = false;
  ctx.exchange_start_ms = 0;
  ctx.wait_ms = 0;
  ctx.first_response_ms = 0;
  return ctx;
}

// ========================================================================================
// Outbound exchange state machine
// ========================================================================================

TEST(Exchange, OutboundStateTransitions) {
  // This tests the state transition logic in hub_exchange.cpp (simplified here).
  // The actual implementation is in send_and_receive_ but the decisions are data-driven.
  // We'll test the decision helpers from hub_decisions.h which we already have.
  // Additionally test the context structs are well-defined.
  exchange::OutboundExchangeContext ctx = make_outbound_context();
  EXPECT_EQ(ctx.state, exchange::OutboundExchangeState::IDLE) << "initial state should be IDLE";
  EXPECT_EQ(ctx.try_index, 0u) << "initial try_index should be 0";
  EXPECT_FALSE(ctx.saw_challenge) << "initial saw_challenge should be false";
}

TEST(Exchange, InboundAuthStateTransitions) {
  exchange::InboundAuthContext ctx;
  EXPECT_EQ(ctx.state, exchange::InboundAuthState::IDLE) << "initial inbound auth state should be IDLE";

  ctx.state = exchange::InboundAuthState::TX_CHALLENGE;
  EXPECT_EQ(ctx.state, exchange::InboundAuthState::TX_CHALLENGE) << "state should transition to TX_CHALLENGE";

  ctx.state = exchange::InboundAuthState::WAIT_CHALLENGE_RESPONSE;
  EXPECT_EQ(ctx.state, exchange::InboundAuthState::WAIT_CHALLENGE_RESPONSE)
      << "state should transition to WAIT_CHALLENGE_RESPONSE";

  ctx.state = exchange::InboundAuthState::VERIFIED;
  EXPECT_EQ(ctx.state, exchange::InboundAuthState::VERIFIED) << "state should transition to VERIFIED";

  ctx.state = exchange::InboundAuthState::FAILED;
  EXPECT_EQ(ctx.state, exchange::InboundAuthState::FAILED) << "state should transition to FAILED";
}

// ============================================================================
// Outbound exchange (send_and_receive_) unit tests
// These tests exercise IOHomeControlComponent::send_and_receive_ using a mock
// radio driver. They validate the full retry loop, first-response handling,
// authentication challenge flow, and final response processing.

namespace {

// --- Testable component exposing protected exchange internals -----------------
class TestableComponent : public IOHomeControlComponent {
 public:
  using IOHomeControlComponent::send_and_receive_;
  using IOHomeControlComponent::authenticate_request_;
  using IOHomeControlComponent::initialized_;
  using IOHomeControlComponent::radio_;
  using IOHomeControlComponent::node_id_;
  using IOHomeControlComponent::system_key_;
};

// --- Frame builders ---------------------------------------------------------

static IoFrame build_status_response(const uint8_t src[3], const uint8_t dst[3], uint8_t data_len = 6) {
  IoFrame f{};
  init_frame(f, true, false, true, false);  // 2W, end frame
  set_dst(f, dst);
  set_src(f, src);
  uint8_t payload[6] = {0};
  set_cmd(f, CMD_PRIVATE_RESP, payload, data_len);
  return f;
}

static IoFrame build_challenge(const uint8_t src[3], const uint8_t dst[3], const uint8_t challenge[6]) {
  IoFrame f{};
  init_frame(f, true, false, false, false);
  set_dst(f, dst);
  set_src(f, src);
  set_cmd(f, CMD_CHALLENGE_REQ, challenge, 6);
  return f;
}

static IoFrame build_error_response(const uint8_t src[3], const uint8_t dst[3], uint8_t result) {
  IoFrame f{};
  init_frame(f, true, false, true, false);
  set_dst(f, dst);
  set_src(f, src);
  set_cmd(f, CMD_ERROR_RESP, &result, 1);
  return f;
}

}  // anonymous namespace

// ============================================================================
// Test cases
// ============================================================================

TEST(Exchange, SendAndReceive_DirectSuccess) {
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  IoFrame request{};
  create_execute_position(request, comp.node_id_, test::DST_ID, false, 100);

  IoFrame resp = build_status_response(test::DST_ID, comp.node_id_);
  uint8_t raw[64];
  uint8_t raw_len = serialize(resp, raw, sizeof(raw));
  RadioRxPacket pkt{};
  pkt.len = raw_len;
  memcpy(pkt.data, raw, raw_len);
  pkt.freq_hz = FREQ_CH2;
  radio.queue_rx(pkt);

  IoFrame response{};
  bool ok = comp.send_and_receive_(request, response, FREQ_CH2) == ExchangeOutcome::SUCCESS_WITH_RESPONSE;

  // Direct response should succeed without authentication
  EXPECT_TRUE(ok) << "direct status response should succeed without challenge";
  EXPECT_EQ(response.cmd, CMD_PRIVATE_RESP) << "response command should be CMD_PRIVATE_RESP (status)";
  EXPECT_EQ(memcmp(response.src, test::DST_ID, NODE_ID_SIZE), 0) << "response source should be the device we commanded";
  EXPECT_EQ(memcmp(response.dst, comp.node_id_, NODE_ID_SIZE), 0) << "response destination should be our node ID";
}

TEST(Exchange, SendAndReceive_AllTransmitFails) {
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  for (int i = 0; i < EXCHANGE_RETRY_COUNT; ++i) {
    radio.queue_tx_result(false);
  }

  IoFrame request{};
  create_execute_position(request, comp.node_id_, test::DST_ID, false, 100);

  IoFrame response{};
  bool ok = comp.send_and_receive_(request, response, FREQ_CH2) == ExchangeOutcome::SUCCESS_WITH_RESPONSE;

  // All transmit attempts fail — should exhaust retries and return false
  EXPECT_FALSE(ok) << "if every transmit attempt fails, exchange should return false";
  EXPECT_EQ(radio.get_send_count(), EXCHANGE_RETRY_COUNT)
      << "should perform exactly EXCHANGE_RETRY_COUNT transmit attempts before giving up";
}

TEST(Exchange, SendAndReceive_FirstResponseIgnoredThenDirectSuccess) {
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  IoFrame request{};
  create_execute_position(request, comp.node_id_, test::DST_ID, false, 100);

  // Unrelated packet (wrong source)
  IoFrame unrelated = build_status_response(test::FOREIGN_ID, comp.node_id_);
  uint8_t raw1[64];
  uint8_t len1 = serialize(unrelated, raw1, sizeof(raw1));
  RadioRxPacket pkt1{};
  pkt1.len = len1;
  memcpy(pkt1.data, raw1, len1);
  radio.queue_rx(pkt1);

  // Correct direct response
  IoFrame correct = build_status_response(test::DST_ID, comp.node_id_);
  uint8_t raw2[64];
  uint8_t len2 = serialize(correct, raw2, sizeof(raw2));
  RadioRxPacket pkt2{};
  pkt2.len = len2;
  memcpy(pkt2.data, raw2, len2);
  radio.queue_rx(pkt2);

  IoFrame response{};
  bool ok = comp.send_and_receive_(request, response, FREQ_CH2) == ExchangeOutcome::SUCCESS_WITH_RESPONSE;

  // Direct response should succeed without authentication
  EXPECT_TRUE(ok) << "direct status response should succeed without challenge";
  EXPECT_EQ(response.cmd, CMD_PRIVATE_RESP) << "response command should be CMD_PRIVATE_RESP (status)";
  EXPECT_EQ(memcmp(response.src, test::DST_ID, NODE_ID_SIZE), 0) << "response source should be the device we commanded";
  EXPECT_EQ(memcmp(response.dst, comp.node_id_, NODE_ID_SIZE), 0) << "response destination should be our node ID";
}

TEST(Exchange, SendAndReceive_ChallengeSuccess) {
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  IoFrame request{};
  create_execute_position(request, comp.node_id_, test::DST_ID, false, 100);

  // Challenge packet from device
  uint8_t chal_data[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  IoFrame challenge = build_challenge(test::DST_ID, comp.node_id_, chal_data);
  uint8_t raw_chal[64];
  uint8_t len_chal = serialize(challenge, raw_chal, sizeof(raw_chal));
  RadioRxPacket chal_pkt{};
  chal_pkt.len = len_chal;
  memcpy(chal_pkt.data, raw_chal, len_chal);
  radio.queue_rx(chal_pkt);

  // Final status response after auth
  IoFrame final_resp = build_status_response(test::DST_ID, comp.node_id_);
  uint8_t raw_final[64];
  uint8_t len_final = serialize(final_resp, raw_final, sizeof(raw_final));
  RadioRxPacket final_pkt{};
  final_pkt.len = len_final;
  memcpy(final_pkt.data, raw_final, len_final);
  radio.queue_rx(final_pkt);

  IoFrame response{};
  bool ok = comp.send_and_receive_(request, response, FREQ_CH2) == ExchangeOutcome::SUCCESS_WITH_RESPONSE;

  EXPECT_TRUE(ok);
  EXPECT_EQ(response.cmd, CMD_PRIVATE_RESP);
  EXPECT_GE(radio.get_send_count(), 2);  // request + auth response
}

TEST(Exchange, SendAndReceive_DirectErrorResponseIsAccepted) {
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  IoFrame request{};
  create_execute_position(request, comp.node_id_, test::DST_ID, false, 100);

  IoFrame resp = build_error_response(test::DST_ID, comp.node_id_, RESULT_LIMITATION_BY_RAIN);
  uint8_t raw[64];
  uint8_t raw_len = serialize(resp, raw, sizeof(raw));
  RadioRxPacket pkt{};
  pkt.len = raw_len;
  memcpy(pkt.data, raw, raw_len);
  pkt.freq_hz = FREQ_CH2;
  radio.queue_rx(pkt);

  IoFrame response{};
  bool ok = comp.send_and_receive_(request, response, FREQ_CH2) == ExchangeOutcome::SUCCESS_WITH_RESPONSE;

  EXPECT_TRUE(ok) << "transport layer should surface explicit device refusals to the caller";
  EXPECT_EQ(response.cmd, CMD_ERROR_RESP);
  ASSERT_EQ(response.data_len, 1u);
  EXPECT_EQ(response.data[0], RESULT_LIMITATION_BY_RAIN);
}

TEST(Exchange, SendAndReceive_FinalErrorResponseAfterChallengeIsAccepted) {
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  IoFrame request{};
  create_execute_position(request, comp.node_id_, test::DST_ID, false, 100);

  uint8_t chal_data[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  IoFrame challenge = build_challenge(test::DST_ID, comp.node_id_, chal_data);
  uint8_t raw_chal[64];
  uint8_t len_chal = serialize(challenge, raw_chal, sizeof(raw_chal));
  RadioRxPacket chal_pkt{};
  chal_pkt.len = len_chal;
  memcpy(chal_pkt.data, raw_chal, len_chal);
  radio.queue_rx(chal_pkt);

  IoFrame final_resp = build_error_response(test::DST_ID, comp.node_id_, RESULT_THERMAL_PROTECTION);
  uint8_t raw_final[64];
  uint8_t len_final = serialize(final_resp, raw_final, sizeof(raw_final));
  RadioRxPacket final_pkt{};
  final_pkt.len = len_final;
  memcpy(final_pkt.data, raw_final, len_final);
  radio.queue_rx(final_pkt);

  IoFrame response{};
  bool ok = comp.send_and_receive_(request, response, FREQ_CH2) == ExchangeOutcome::SUCCESS_WITH_RESPONSE;

  EXPECT_TRUE(ok) << "authenticated exchanges should also surface explicit device refusals";
  EXPECT_EQ(response.cmd, CMD_ERROR_RESP);
  ASSERT_EQ(response.data_len, 1u);
  EXPECT_EQ(response.data[0], RESULT_THERMAL_PROTECTION);
}

TEST(Exchange, SendAndReceive_AuthTransmitFailure) {
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  IoFrame request{};
  create_execute_position(request, comp.node_id_, test::DST_ID, false, 100);

  // Challenge packet
  uint8_t chal_data[6] = {1, 2, 3, 4, 5, 6};
  IoFrame challenge = build_challenge(test::DST_ID, comp.node_id_, chal_data);
  uint8_t raw_chal[64];
  uint8_t len_chal = serialize(challenge, raw_chal, sizeof(raw_chal));
  RadioRxPacket chal_pkt{};
  chal_pkt.len = len_chal;
  memcpy(chal_pkt.data, raw_chal, len_chal);
  radio.queue_rx(chal_pkt);

  // TX sequence: request success, auth failure, then further request failures
  radio.queue_tx_result(true);   // request transmit OK
  radio.queue_tx_result(false);  // auth response TX fails
  radio.queue_tx_result(false);  // try 1 request fails
  radio.queue_tx_result(false);  // try 2 request fails
  radio.queue_tx_result(false);  // try 3 request fails

  IoFrame response{};
  bool ok = comp.send_and_receive_(request, response, FREQ_CH2) == ExchangeOutcome::SUCCESS_WITH_RESPONSE;

  // All transmit attempts fail — should exhaust retries and return false
  EXPECT_FALSE(ok) << "if every transmit attempt fails, exchange should return false";
  // Request sent EXCHANGE_RETRY_COUNT times, plus one auth response attempt
  EXPECT_EQ(radio.get_send_count(), EXCHANGE_RETRY_COUNT + 1)
      << "should perform EXCHANGE_RETRY_COUNT request transmits plus one auth response attempt before giving up";
}

TEST(Exchange, SendAndReceive_MissingFinalResponseIsUnconfirmedSuccessNotFailure) {
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  IoFrame request{};
  create_execute_position(request, comp.node_id_, test::DST_ID, false, 100);

  // Challenge packet
  uint8_t chal_data[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  IoFrame challenge = build_challenge(test::DST_ID, comp.node_id_, chal_data);
  uint8_t raw_chal[64];
  uint8_t len_chal = serialize(challenge, raw_chal, sizeof(raw_chal));
  RadioRxPacket chal_pkt{};
  chal_pkt.len = len_chal;
  memcpy(chal_pkt.data, raw_chal, len_chal);
  radio.queue_rx(chal_pkt);

  // No final packet queued

  // TX success for request and auth
  radio.queue_tx_result(true);
  radio.queue_tx_result(true);

  IoFrame response{};
  const ExchangeOutcome outcome = comp.send_and_receive_(request, response, FREQ_CH2);

  // A challenge we answered proves the device received and accepted the request, so this is not a
  // failure — it is an acceptance we could not confirm. Some devices never close the exchange at
  // all: a Somfy RS100's next transmission after our 0x3D was measured at 3.4-12 s, or never,
  // against a 500 ms window, while a Somfy awning acks synchronously with 0x04. See
  // ExchangeOutcome.
  EXPECT_EQ(outcome, ExchangeOutcome::SUCCESS_UNCONFIRMED)
      << "an authenticated request with no final response is accepted, not failed";
  EXPECT_EQ(response.cmd, 0) << "there was no response frame, so none should be handed back";
  // The retry is the part that actively hurt: the command is already executing, so re-sending it
  // twice more only reaches a device that has acted and now ignores duplicates.
  EXPECT_EQ(radio.get_send_count(), 2) << "expected exactly the request plus the auth response, with no retries";
}

// ============================================================================
// response_preamble() behavior tests
// ============================================================================
// Validate that RadioDriver::response_preamble() correctly influences the
// preamble length used by send_and_receive_ for both START and non-START frames,
// and that the SX1262 override returns the longer preamble while the default
// (SX1276-like) returns SHORT_PREAMBLE.

TEST(Exchange, ResponsePreamble_DefaultReturnsShortPreamble) {
  MockRadio radio;
  EXPECT_EQ(radio.response_preamble(), SHORT_PREAMBLE)
      << "base RadioDriver (SX1276-like) should return SHORT_PREAMBLE for response frames";
}

TEST(Exchange, ResponsePreamble_SX1262MatchesConfiguredValue) {
  MockRadioSX1262 radio;
  EXPECT_EQ(radio.response_preamble(), SX1262_RESPONSE_PREAMBLE)
      << "SX1262 should return the configured response preamble";
  // SX1262_RESPONSE_PREAMBLE is byte-denominated, same as SHORT_PREAMBLE (set_packet_params_()
  // converts to the chip's bit-denominated register at the SPI boundary), so the hardware-
  // validated response preamble equals the protocol's nominal short preamble, not something
  // longer than it.
  EXPECT_EQ(radio.response_preamble(), SHORT_PREAMBLE)
      << "SX1262's hardware-validated response preamble is the protocol's nominal 8 bytes";
}

TEST(Exchange, TxRxTurnaround_RealDriversDeclareExpectedCapability) {
  // Pins the per-chip capability the pairing engine uses to select its key-confirm
  // wait strategy. Instantiation only — no init(), no SPI traffic.
  MockSpi spi;
  MockPin rst, dio0, dio4, dio1, busy;
  RadioSX1276 sx1276(&spi, &rst, &dio0, &dio4, 17, 0x80);
  RadioSX1262 sx1262(&spi, &rst, &dio1, &busy, 17, 0x03);
  EXPECT_TRUE(sx1276.has_fast_tx_rx_turnaround())
      << "SX1276 (IoHomeOn) catches immediate replies via the standard exchange wait";
  EXPECT_FALSE(sx1262.has_fast_tx_rx_turnaround())
      << "SX1262 needs the dedicated key-confirm wait (slow TX->RX transition)";
}

TEST(Exchange, RadioSX1276_ApplyTuningRoutesRadioParameters) {
  // Drives the real SX1276 apply_tuning() path (no init(); register writes go to the no-op
  // MockSpi). Verifies the default matches the pre-tunable fixed value and that a tuning
  // override propagates to the observable response_preamble(), and that set_rx_bandwidth_
  // accepts every enum option without touching hardware.
  MockSpi spi;
  MockPin rst, dio0, dio4;
  RadioSX1276 sx1276(&spi, &rst, &dio0, &dio4, 17, 0x80);

  // Default (no tuning applied): the SX1276 default response preamble (12).
  EXPECT_EQ(sx1276.response_preamble(), SX1276_RESPONSE_PREAMBLE);

  TuningConfig cfg{};
  cfg.sx1276_response_preamble = 40;
  for (auto bw : {SX1276RxBandwidth::BW_20_8_KHZ, SX1276RxBandwidth::BW_41_7_KHZ, SX1276RxBandwidth::BW_125_0_KHZ}) {
    cfg.sx1276_rx_bandwidth = bw;
    sx1276.apply_tuning(cfg);  // set_rx_bandwidth_ + set_response_preamble_ (no crash / no SPI dependency)
  }
  EXPECT_EQ(sx1276.response_preamble(), 40) << "apply_tuning must route sx1276_response_preamble to the driver";

  // Applying defaults again restores the default preamble.
  sx1276.apply_tuning(TuningConfig{});
  EXPECT_EQ(sx1276.response_preamble(), SX1276_RESPONSE_PREAMBLE);
}

TEST(Exchange, SendAndReceive_StartFrameUsesLongPreamble) {
  // START frames must always use LONG_PREAMBLE regardless of chip type.
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  IoFrame request{};
  create_execute_position(request, comp.node_id_, test::DST_ID, false, 100);
  // create_execute_position sets START flag — verify it
  ASSERT_TRUE(is_start(request)) << "execute command should have START flag set";

  // No RX → times out after retries, but we can inspect the TX config
  IoFrame response{};
  comp.send_and_receive_(request, response, FREQ_CH2);

  ASSERT_GE(radio.get_tx_configs().size(), 1u) << "at least one TX should have been attempted";
  EXPECT_EQ(radio.get_tx_configs()[0].preamble_len, LONG_PREAMBLE)
      << "START frame should always use LONG_PREAMBLE regardless of radio type";
}

TEST(Exchange, SendAndReceive_NonStartFrameUsesResponsePreamble_Default) {
  // Non-START frames on the default radio (SX1276-like) should use SHORT_PREAMBLE.
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  // Build a non-START frame (key_transfer style: 2W, no start, no end)
  IoFrame request{};
  init_frame(request, true, false, false, false);
  set_dst(request, test::DST_ID);
  set_src(request, comp.node_id_);
  uint8_t payload[16] = {0};
  set_cmd(request, CMD_KEY_TRANSFER, payload, sizeof(payload));
  ASSERT_FALSE(is_start(request)) << "key_transfer should NOT have START flag";

  IoFrame response{};
  comp.send_and_receive_(request, response, FREQ_CH2);

  ASSERT_GE(radio.get_tx_configs().size(), 1u);
  EXPECT_EQ(radio.get_tx_configs()[0].preamble_len, SHORT_PREAMBLE)
      << "non-START frame on default radio should use SHORT_PREAMBLE";
}

TEST(Exchange, SendAndReceive_NonStartFrameUsesResponsePreamble_SX1262) {
  // Non-START frames on SX1262 should use SX1262_RESPONSE_PREAMBLE, not LONG_PREAMBLE.
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadioSX1262 radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  // Build a non-START frame (key_transfer style)
  IoFrame request{};
  init_frame(request, true, false, false, false);
  set_dst(request, test::DST_ID);
  set_src(request, comp.node_id_);
  uint8_t payload[16] = {0};
  set_cmd(request, CMD_KEY_TRANSFER, payload, sizeof(payload));
  ASSERT_FALSE(is_start(request));

  IoFrame response{};
  comp.send_and_receive_(request, response, FREQ_CH2);

  ASSERT_GE(radio.get_tx_configs().size(), 1u);
  EXPECT_EQ(radio.get_tx_configs()[0].preamble_len, SX1262_RESPONSE_PREAMBLE)
      << "non-START frame on SX1262 should use SX1262_RESPONSE_PREAMBLE";
}

TEST(Exchange, SendAndReceive_AuthResponseUsesSX1262Preamble) {
  // When SX1262 radio handles a challenge, the 0x3D auth response should use the longer preamble.
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadioSX1262 radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  IoFrame request{};
  create_execute_position(request, comp.node_id_, test::DST_ID, false, 100);

  // Queue a challenge from device
  uint8_t chal_data[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  IoFrame challenge = build_challenge(test::DST_ID, comp.node_id_, chal_data);
  uint8_t raw_chal[64];
  uint8_t len_chal = serialize(challenge, raw_chal, sizeof(raw_chal));
  RadioRxPacket chal_pkt{};
  chal_pkt.len = len_chal;
  memcpy(chal_pkt.data, raw_chal, len_chal);
  radio.queue_rx(chal_pkt);

  // No final response — will timeout, but we can inspect the auth response TX config
  IoFrame response{};
  comp.send_and_receive_(request, response, FREQ_CH2);

  // TX 0 = request (LONG_PREAMBLE, start frame)
  // TX 1 = auth response (should use SX1262_RESPONSE_PREAMBLE)
  ASSERT_GE(radio.get_tx_configs().size(), 2u) << "should have sent at least request + auth response";
  EXPECT_EQ(radio.get_tx_configs()[0].preamble_len, LONG_PREAMBLE)
      << "initial request (START) should use LONG_PREAMBLE";
  EXPECT_EQ(radio.get_tx_configs()[1].preamble_len, SX1262_RESPONSE_PREAMBLE)
      << "auth response on SX1262 should use SX1262_RESPONSE_PREAMBLE";
}

// ============================================================================
// Step 11a — pinning tests: verify exchange HMAC content, retry exhaustion,
// unrelated-frame filtering during auth wait, and inbound authenticate_request_.
// These tests must pass before and after the ExchangeEngine extraction.
// ============================================================================

namespace {

/// @brief Mock radio that auto-responds to outbound 0x3C with a 0x3D built
/// from a captured request frame, letting tests verify inbound auth without
/// knowing the RNG-generated challenge in advance.
class RespondOnChallengeMockRadio : public MockRadio {
 public:
  /// Arm the responder. On the next 0x3C transmitted by the hub, it will
  /// build a 0x3D using @p system_key and queue it. If @p valid is false the
  /// HMAC bytes are intentionally wrong (to exercise rejection).
  void arm(const IoFrame &request, const uint8_t *system_key, bool valid) {
    request_ = request;
    system_key_ = system_key;
    valid_ = valid;
    armed_ = true;
  }

  bool send_packet(const uint8_t *data, uint8_t len, const RadioTxConfig &tx) override {
    bool result = MockRadio::send_packet(data, len, tx);
    if (!armed_)
      return result;
    IoFrame frame;
    if (!parse(data, len, frame) || frame.cmd != CMD_CHALLENGE_REQ)
      return result;
    armed_ = false;

    // Build a 0x3D response: src=device, dst=controller (reverse of the 0x3C).
    uint8_t hmac[HMAC_SIZE];
    if (valid_) {
      uint8_t frame_data[FRAME_MAX_SIZE];
      frame_data[0] = request_.cmd;
      memcpy(frame_data + 1, request_.data, request_.data_len);
      // frame.data holds the 6 challenge bytes the hub put in its 0x3C.
      crypto::create_hmac(frame_data, request_.data_len + 1, frame.data, system_key_, hmac);
    } else {
      memset(hmac, 0xAB, HMAC_SIZE);
    }
    IoFrame resp;
    init_frame(resp);
    set_dst(resp, frame.src);  // hub's node_id → controller is dst
    set_src(resp, frame.dst);  // device is src
    set_cmd(resp, CMD_CHALLENGE_RESP, hmac, HMAC_SIZE);

    uint8_t raw[RADIO_PACKET_BUFFER_SIZE];
    uint8_t raw_len = serialize(resp, raw, sizeof(raw));
    RadioRxPacket pkt{};
    pkt.len = raw_len;
    memcpy(pkt.data, raw, raw_len);
    queue_rx(pkt);
    return result;
  }

 private:
  IoFrame request_{};
  const uint8_t *system_key_{nullptr};
  bool valid_{false};
  bool armed_{false};
};

/// Build a CMD_STATUS_UPDATE frame arriving from a device (src=device, dst=controller).
/// Used to exercise authenticate_request_() without going through process_received_packet_().
static IoFrame build_status_update_from_device(const uint8_t device_id[NODE_ID_SIZE],
                                               const uint8_t controller_id[NODE_ID_SIZE]) {
  IoFrame f{};
  init_frame(f, true, true, false, false);  // 2W, start
  set_dst(f, controller_id);
  set_src(f, device_id);
  uint8_t payload[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  set_cmd(f, CMD_STATUS_UPDATE, payload, sizeof(payload));
  return f;
}

}  // namespace

// --- Pinning test 1: HMAC content of the transmitted 0x3D is correct ---------

TEST(Exchange, SendAndReceive_ChallengeResponseCarriesCorrectHmac) {
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  IoFrame request{};
  create_execute_position(request, comp.node_id_, test::DST_ID, false, 100);

  // Challenge from device with known bytes.
  uint8_t chal_data[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  IoFrame challenge = build_challenge(test::DST_ID, comp.node_id_, chal_data);
  uint8_t raw_chal[64];
  uint8_t len_chal = serialize(challenge, raw_chal, sizeof(raw_chal));
  RadioRxPacket chal_pkt{};
  chal_pkt.len = len_chal;
  memcpy(chal_pkt.data, raw_chal, len_chal);
  radio.queue_rx(chal_pkt);

  // Final response after challenge.
  IoFrame final_resp = build_status_response(test::DST_ID, comp.node_id_);
  uint8_t raw_final[64];
  uint8_t len_final = serialize(final_resp, raw_final, sizeof(raw_final));
  RadioRxPacket final_pkt{};
  final_pkt.len = len_final;
  memcpy(final_pkt.data, raw_final, len_final);
  radio.queue_rx(final_pkt);

  IoFrame response{};
  bool ok = comp.send_and_receive_(request, response, FREQ_CH2) == ExchangeOutcome::SUCCESS_WITH_RESPONSE;
  ASSERT_TRUE(ok) << "challenge+response exchange should succeed";

  // TX 0 = initial request, TX 1 = 0x3D auth response.
  ASSERT_GE(radio.get_sent_data().size(), 2u) << "at least request + auth response should have been transmitted";
  const auto &auth_raw = radio.get_sent_data()[1];

  IoFrame auth_resp{};
  ASSERT_TRUE(parse(auth_raw.data(), static_cast<uint8_t>(auth_raw.size()), auth_resp))
      << "auth response frame must parse cleanly";
  EXPECT_EQ(auth_resp.cmd, CMD_CHALLENGE_RESP) << "transmitted frame must be a 0x3D challenge response";
  ASSERT_EQ(auth_resp.data_len, HMAC_SIZE) << "0x3D payload must be exactly HMAC_SIZE bytes";

  // Recompute expected HMAC: [request.cmd, request.data...] + challenge + system_key.
  uint8_t frame_data[FRAME_MAX_SIZE];
  frame_data[0] = request.cmd;
  memcpy(frame_data + 1, request.data, request.data_len);
  uint8_t expected_hmac[HMAC_SIZE];
  ASSERT_TRUE(crypto::create_hmac(frame_data, request.data_len + 1, chal_data, test::TEST_SYSTEM_KEY, expected_hmac));
  EXPECT_EQ(memcmp(auth_resp.data, expected_hmac, HMAC_SIZE), 0)
      << "transmitted HMAC in 0x3D must match create_hmac([cmd+payload], challenge, system_key)";
}

// --- Pinning test 2: retry exhaustion when TX succeeds but no response -------

TEST(Exchange, SendAndReceive_RetryExhaustion_NoResponse) {
  // TX always succeeds (no queued results → MockRadio defaults to true),
  // but no RX packets are queued so wait_for_first_response_ times out each try.
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  IoFrame request{};
  create_execute_position(request, comp.node_id_, test::DST_ID, false, 50);

  IoFrame response{};
  bool ok = comp.send_and_receive_(request, response, FREQ_CH2) == ExchangeOutcome::SUCCESS_WITH_RESPONSE;

  EXPECT_FALSE(ok) << "exchange with no device response should fail after exhausting all retries";
  EXPECT_EQ(radio.get_send_count(), EXCHANGE_RETRY_COUNT)
      << "should attempt exactly EXCHANGE_RETRY_COUNT transmissions before giving up";
}

// --- Pinning test 3: unrelated frame ignored during the auth-wait window -----

TEST(Exchange, SendAndReceive_UnrelatedFrameIgnoredDuringFinalWait) {
  // After sending the 0x3D, an unrelated frame from a foreign device arrives
  // before the legitimate final response. The exchange should ignore it and
  // ultimately succeed when the correct frame arrives.
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  IoFrame request{};
  create_execute_position(request, comp.node_id_, test::DST_ID, false, 100);

  // 1. Challenge from the correct device.
  uint8_t chal_data[6] = {0xCA, 0xFE, 0xBA, 0xBE, 0x00, 0x01};
  IoFrame challenge = build_challenge(test::DST_ID, comp.node_id_, chal_data);
  uint8_t raw_chal[64];
  uint8_t len_chal = serialize(challenge, raw_chal, sizeof(raw_chal));
  RadioRxPacket chal_pkt{};
  chal_pkt.len = len_chal;
  memcpy(chal_pkt.data, raw_chal, len_chal);
  radio.queue_rx(chal_pkt);

  // 2. Unrelated frame (wrong src) that arrives during the final-response wait.
  IoFrame noise = build_status_response(test::FOREIGN_ID, comp.node_id_);
  uint8_t raw_noise[64];
  uint8_t len_noise = serialize(noise, raw_noise, sizeof(raw_noise));
  RadioRxPacket noise_pkt{};
  noise_pkt.len = len_noise;
  memcpy(noise_pkt.data, raw_noise, len_noise);
  radio.queue_rx(noise_pkt);

  // 3. Correct final response from the actual device.
  IoFrame final_resp = build_status_response(test::DST_ID, comp.node_id_);
  uint8_t raw_final[64];
  uint8_t len_final = serialize(final_resp, raw_final, sizeof(raw_final));
  RadioRxPacket final_pkt{};
  final_pkt.len = len_final;
  memcpy(final_pkt.data, raw_final, len_final);
  radio.queue_rx(final_pkt);

  IoFrame response{};
  bool ok = comp.send_and_receive_(request, response, FREQ_CH2) == ExchangeOutcome::SUCCESS_WITH_RESPONSE;

  EXPECT_TRUE(ok) << "unrelated frames in the auth-wait window must be ignored; exchange should still succeed";
  EXPECT_EQ(response.cmd, CMD_PRIVATE_RESP) << "final accepted response must be the legitimate device reply";
  EXPECT_EQ(memcmp(response.src, test::DST_ID, NODE_ID_SIZE), 0)
      << "accepted response must originate from the correct device";
}

// --- Pinning test 4: inbound authenticate_request_ with valid HMAC -----------

TEST(Exchange, AuthenticateRequest_ValidHmacAccepted) {
  // Device sends a CMD_STATUS_UPDATE. Hub challenges with 0x3C; the mock radio
  // intercepts the 0x3C, computes the correct HMAC, and queues the 0x3D reply.
  TestableComponent comp;
  comp.initialized_ = true;
  RespondOnChallengeMockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  IoFrame status_update = build_status_update_from_device(test::DST_ID, comp.node_id_);
  radio.arm(status_update, test::TEST_SYSTEM_KEY, /*valid=*/true);

  bool ok = comp.authenticate_request_(status_update, FREQ_CH2);

  EXPECT_TRUE(ok) << "authenticate_request_ with correct HMAC must return true";
}

// --- Pinning test 5: inbound authenticate_request_ rejects wrong HMAC --------

TEST(Exchange, AuthenticateRequest_InvalidHmacRejected) {
  TestableComponent comp;
  comp.initialized_ = true;
  RespondOnChallengeMockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  IoFrame status_update = build_status_update_from_device(test::DST_ID, comp.node_id_);
  radio.arm(status_update, test::TEST_SYSTEM_KEY, /*valid=*/false);

  bool ok = comp.authenticate_request_(status_update, FREQ_CH2);

  EXPECT_FALSE(ok) << "authenticate_request_ with wrong HMAC must return false";
}

// ============================================================================
// collect_broadcast_responses tests
// ============================================================================
// Exercises ExchangeEngine directly (not through IOHomeControlComponent) to isolate this
// primitive from the action layer that drives it (ManagementActions::scan_paired_devices(),
// covered separately in tests/hub_management_test.cpp). A small pairing_discovery_wait_ms keeps
// host-test iteration counts meaningful: in host tests millis() advances by exactly 1 per call,
// and MockRadio::wait_for_packet() returns false immediately once its queue is empty rather than
// honouring the timeout.

namespace {

TuningConfig make_broadcast_test_tuning() {
  TuningConfig tuning;
  tuning.pairing_discovery_wait_ms = 20;
  return tuning;
}

IoFrame build_spe_response(const uint8_t src[3], const uint8_t dst[3]) {
  IoFrame f{};
  init_frame(f, true, true, true, false);
  set_dst(f, dst);
  set_src(f, src);
  set_cmd(f, CMD_DISCOVER_SPE_RESP, nullptr, 0);
  return f;
}

RadioRxPacket to_rx_packet(const IoFrame &frame) {
  RadioRxPacket pkt{};
  uint8_t raw[64];
  pkt.len = serialize(frame, raw, sizeof(raw));
  memcpy(pkt.data, raw, pkt.len);
  return pkt;
}

IoFrame build_spe_request(const uint8_t own[3]) {
  IoFrame f{};
  create_discovery_request(f, own, CMD_DISCOVER_SPE_REQ, BROADCAST_DISCOVER, false, false, 0, test::TEST_SYSTEM_KEY);
  return f;
}

/// Records every callback delivery so tests can assert on count, addresses, and RSSI.
/// collect_broadcast_responses() neither stores nor deduplicates replies, so this is also what
/// proves it hands over duplicates rather than filtering them.
struct CollectedReplies {
  std::vector<IoFrame> frames;
  std::vector<int16_t> rssi_dbm;

  ExchangeEngine::BroadcastReplyHandler handler() {
    return [this](const IoFrame &frame, int16_t rssi) {
      this->frames.push_back(frame);
      this->rssi_dbm.push_back(rssi);
    };
  }
};

}  // namespace

TEST(Exchange, CollectBroadcastResponses_ZeroReplies) {
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  IoFrame request = build_spe_request(test::OWN_ID);
  CollectedReplies collected;
  uint8_t count = engine.collect_broadcast_responses(request, FREQ_CH2, CMD_DISCOVER_SPE_RESP,
                                                     tuning.pairing_discovery_wait_ms, collected.handler());

  EXPECT_EQ(count, 0u) << "no queued replies should yield zero collected";
  EXPECT_TRUE(collected.frames.empty()) << "the handler must not be invoked when nothing arrives";
}

TEST(Exchange, CollectBroadcastResponses_WindowIsHonouredNotIgnored) {
  // Every production caller passes the same tuning value, so without this the window_ms parameter
  // could be ignored entirely and no other test would notice. A zero-length window must expire
  // before the first receive: host millis() advances one tick per call, so the deadline is already
  // in the past when the loop is first evaluated.
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  radio.queue_rx(to_rx_packet(build_spe_response(test::DST_ID, test::OWN_ID)));

  IoFrame request = build_spe_request(test::OWN_ID);
  CollectedReplies collected;
  uint8_t count = engine.collect_broadcast_responses(request, FREQ_CH2, CMD_DISCOVER_SPE_RESP, /*window_ms=*/0,
                                                     collected.handler());

  EXPECT_EQ(count, 0u) << "a zero-length window must collect nothing even with a reply already queued";
  EXPECT_TRUE(collected.frames.empty()) << "the handler must not run after the window has expired";
  EXPECT_EQ(radio.get_send_count(), 1) << "the request is still transmitted; only the listen window is empty";
}

TEST(Exchange, CollectBroadcastResponses_OneReply) {
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);
  radio.set_last_capture_rssi(-42);

  IoFrame reply = build_spe_response(test::DST_ID, test::OWN_ID);
  radio.queue_rx(to_rx_packet(reply));

  IoFrame request = build_spe_request(test::OWN_ID);
  CollectedReplies collected;
  uint8_t count = engine.collect_broadcast_responses(request, FREQ_CH2, CMD_DISCOVER_SPE_RESP,
                                                     tuning.pairing_discovery_wait_ms, collected.handler());

  ASSERT_EQ(count, 1u);
  ASSERT_EQ(collected.frames.size(), 1u);
  EXPECT_EQ(memcmp(collected.frames[0].src, test::DST_ID, NODE_ID_SIZE), 0)
      << "delivered reply should carry the responder's node ID";
  EXPECT_EQ(collected.rssi_dbm[0], -42) << "delivered reply should carry the RSSI of the captured packet";
}

TEST(Exchange, CollectBroadcastResponses_TwoDistinctSourcesBothCollected) {
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  radio.queue_rx(to_rx_packet(build_spe_response(test::DST_ID, test::OWN_ID)));
  radio.queue_rx(to_rx_packet(build_spe_response(test::FOREIGN_ID, test::OWN_ID)));

  IoFrame request = build_spe_request(test::OWN_ID);
  CollectedReplies collected;
  uint8_t count = engine.collect_broadcast_responses(request, FREQ_CH2, CMD_DISCOVER_SPE_RESP,
                                                     tuning.pairing_discovery_wait_ms, collected.handler());

  EXPECT_EQ(count, 2u) << "two distinct responders should both be collected";
}

TEST(Exchange, CollectBroadcastResponses_SameSourceTwiceBothDelivered) {
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  radio.queue_rx(to_rx_packet(build_spe_response(test::DST_ID, test::OWN_ID)));
  radio.queue_rx(to_rx_packet(build_spe_response(test::DST_ID, test::OWN_ID)));

  IoFrame request = build_spe_request(test::OWN_ID);
  CollectedReplies collected;
  uint8_t count = engine.collect_broadcast_responses(request, FREQ_CH2, CMD_DISCOVER_SPE_RESP,
                                                     tuning.pairing_discovery_wait_ms, collected.handler());

  EXPECT_EQ(count, 2u) << "the engine does not deduplicate; both deliveries reach the handler";
  EXPECT_EQ(collected.frames.size(), 2u)
      << "deduplication belongs to the caller (see ScanPairedDevicesDedupsSameSourceThroughActionLayer)";
}

TEST(Exchange, CollectBroadcastResponses_WrongCommandIgnoredWithoutAbortingCollection) {
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  // Wrong-command frame from the same src the valid reply will use.
  IoFrame wrong_cmd{};
  init_frame(wrong_cmd, true, true, true, false);
  set_dst(wrong_cmd, test::OWN_ID);
  set_src(wrong_cmd, test::DST_ID);
  set_cmd(wrong_cmd, CMD_PRIVATE_RESP, nullptr, 0);
  radio.queue_rx(to_rx_packet(wrong_cmd));

  radio.queue_rx(to_rx_packet(build_spe_response(test::DST_ID, test::OWN_ID)));

  IoFrame request = build_spe_request(test::OWN_ID);
  CollectedReplies collected;
  uint8_t count = engine.collect_broadcast_responses(request, FREQ_CH2, CMD_DISCOVER_SPE_RESP,
                                                     tuning.pairing_discovery_wait_ms, collected.handler());

  ASSERT_EQ(count, 1u) << "the wrong-command frame must be ignored, not counted or fatal";
  ASSERT_EQ(collected.frames.size(), 1u);
  EXPECT_EQ(collected.frames[0].cmd, CMD_DISCOVER_SPE_RESP) << "the delivered reply must be the valid one";
}

TEST(Exchange, CollectBroadcastResponses_WrongDestinationIgnored) {
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  // Addressed to some other hub, not us.
  radio.queue_rx(to_rx_packet(build_spe_response(test::DST_ID, test::FOREIGN_ID)));

  IoFrame request = build_spe_request(test::OWN_ID);
  CollectedReplies collected;
  uint8_t count = engine.collect_broadcast_responses(request, FREQ_CH2, CMD_DISCOVER_SPE_RESP,
                                                     tuning.pairing_discovery_wait_ms, collected.handler());

  EXPECT_EQ(count, 0u) << "a reply not addressed to us must be ignored";
}

TEST(Exchange, CollectBroadcastResponses_NoCapacityLimitAtEngineLayer) {
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  // Three distinct responders: the engine stores nothing, so all three reach the handler and any
  // limit is the caller's to impose.
  const uint8_t src_a[3] = {0x01, 0x01, 0x01};
  const uint8_t src_b[3] = {0x02, 0x02, 0x02};
  const uint8_t src_c[3] = {0x03, 0x03, 0x03};
  radio.queue_rx(to_rx_packet(build_spe_response(src_a, test::OWN_ID)));
  radio.queue_rx(to_rx_packet(build_spe_response(src_b, test::OWN_ID)));
  radio.queue_rx(to_rx_packet(build_spe_response(src_c, test::OWN_ID)));

  IoFrame request = build_spe_request(test::OWN_ID);
  CollectedReplies collected;
  uint8_t count = engine.collect_broadcast_responses(request, FREQ_CH2, CMD_DISCOVER_SPE_RESP,
                                                     tuning.pairing_discovery_wait_ms, collected.handler());

  EXPECT_EQ(count, 3u) << "the engine imposes no cap of its own";
  EXPECT_EQ(collected.frames.size(), 3u) << "every distinct responder must reach the handler";
}

TEST(Exchange, CollectBroadcastResponses_TransmitFailureReturnsZero) {
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);
  radio.queue_tx_result(false);

  // Even a queued reply must not be collected if the initial transmit never went out.
  radio.queue_rx(to_rx_packet(build_spe_response(test::DST_ID, test::OWN_ID)));

  IoFrame request = build_spe_request(test::OWN_ID);
  CollectedReplies collected;
  uint8_t count = engine.collect_broadcast_responses(request, FREQ_CH2, CMD_DISCOVER_SPE_RESP,
                                                     tuning.pairing_discovery_wait_ms, collected.handler());

  EXPECT_EQ(count, 0u) << "a failed initial transmit must short-circuit with zero replies";
  EXPECT_TRUE(collected.frames.empty()) << "the handler must not be invoked when the transmit failed";
  EXPECT_EQ(radio.get_send_count(), 1) << "exactly one transmit attempt, no retry";
}

// ============================================================================
// Response windows: a start frame wakes a sleeping device, so it gets the *longer* budget.
//
// These were fixed constants, and RESPONSE_START_WAIT_MS (300 ms) was shorter than
// RESPONSE_WAIT_MS (500 ms) despite its own comment promising "longer" — backwards for the one
// case where the target may have been asleep until the 213 ms wake-up preamble reached it. Field
// captures of a solar RS100 measured replies from 29 ms to 3052 ms on the same device; see
// RESPONSE_START_WAIT_MS.
// ============================================================================

namespace {

// The host clock stubs advance millis() one unit per call, and the engine reads it a couple of
// times between stamping the deadline and slicing it, so the first slice lands a tick or two under
// the configured window. Assert the window, not the exact tick.
void expect_window_near(uint32_t actual, uint32_t expected) {
  EXPECT_LE(actual, expected) << "slice must never exceed the configured window";
  EXPECT_GE(actual + 10u, expected) << "slice must be the configured window, not a different budget";
}

}  // namespace

TEST(Exchange, StartFrameBudgetsTheStartResponseWindow) {
  MockRadio radio;
  // A slice larger than the window keeps per-channel hopping from masking the budget under test.
  radio.set_exchange_wait_slice_ms(1000000);
  RadioDriver *radio_ptr = &radio;

  TuningConfig tuning;
  tuning.exchange_start_response_wait_ms = 1750;
  tuning.exchange_response_wait_ms = 250;
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  IoFrame request{};
  create_execute_position(request, test::OWN_ID, test::DST_ID, true, 100);
  ASSERT_TRUE(is_start(request)) << "an execute command is a start frame";

  IoFrame response{};
  engine.send_and_receive(request, response, FREQ_CH2);  // nothing queued → runs out its retries

  ASSERT_FALSE(radio.wait_timeouts().empty());
  expect_window_near(radio.wait_timeouts().front(), 1750);
}

TEST(Exchange, ContinuationFrameBudgetsTheShorterResponseWindow) {
  MockRadio radio;
  radio.set_exchange_wait_slice_ms(1000000);
  RadioDriver *radio_ptr = &radio;

  TuningConfig tuning;
  tuning.exchange_start_response_wait_ms = 1750;
  tuning.exchange_response_wait_ms = 250;
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  IoFrame request{};
  init_frame(request, true, false, false, false);
  set_dst(request, test::DST_ID);
  set_src(request, test::OWN_ID);
  uint8_t payload[16] = {0};
  set_cmd(request, CMD_KEY_TRANSFER, payload, sizeof(payload));
  ASSERT_FALSE(is_start(request));

  IoFrame response{};
  engine.send_and_receive(request, response, FREQ_CH2);

  ASSERT_FALSE(radio.wait_timeouts().empty());
  expect_window_near(radio.wait_timeouts().front(), 250);
}

TEST(Exchange, ResponseWindowDefaultsComeFromTheProtocolConstants) {
  // No ordering constraint between the two windows is asserted on purpose. An earlier version of
  // this test required the start window to be the longer of the two, on the theory that a
  // just-woken device is the slowest to answer. Measurement disproved that: the device replies
  // within milliseconds of the carrier dropping or not at all (see RESPONSE_START_WAIT_MS), so
  // both windows only need to clear a few tens of milliseconds and their relative order carries
  // no meaning worth pinning.
  TuningConfig tuning;
  EXPECT_EQ(tuning.exchange_start_response_wait_ms, RESPONSE_START_WAIT_MS);
  EXPECT_EQ(tuning.exchange_response_wait_ms, RESPONSE_WAIT_MS);
  EXPECT_EQ(tuning.exchange_total_budget_ms, EXCHANGE_TOTAL_BUDGET_MS);
}

// ============================================================================
// Retry budget: EXCHANGE_RETRY_COUNT is a maximum, not a promise. Three tries at a window sized
// for the slowest device on the network blocks the ESPHome loop past its own warning threshold
// (ADR 0013), so a try only starts if the exchange still has budget. See EXCHANGE_TOTAL_BUDGET_MS.
// ============================================================================

TEST(Exchange, RetriesStopOnceTheTotalBudgetIsSpent) {
  MockRadio radio;
  radio.set_exchange_wait_slice_ms(1000000);  // don't let per-channel slicing mask the window
  RadioDriver *radio_ptr = &radio;

  TuningConfig tuning;
  // One try fits; a second would overrun. The host clock stubs advance millis() per call, so the
  // budget is expressed relative to the window rather than in real milliseconds.
  tuning.exchange_start_response_wait_ms = 400;
  tuning.exchange_total_budget_ms = 200;
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  IoFrame request{};
  create_execute_position(request, test::OWN_ID, test::DST_ID, true, 100);
  IoFrame response{};
  engine.send_and_receive(request, response, FREQ_CH2);  // nothing queued -> every try fails

  EXPECT_EQ(radio.get_send_count(), 1) << "a second try must not start once the budget is spent";
  EXPECT_STREQ(engine.get_debug().stage, "retry_budget_exhausted");
}

TEST(Exchange, GenerousBudgetStillAllowsEveryRetry) {
  MockRadio radio;
  radio.set_exchange_wait_slice_ms(1000000);
  RadioDriver *radio_ptr = &radio;

  TuningConfig tuning;
  tuning.exchange_start_response_wait_ms = 200;
  tuning.exchange_total_budget_ms = 60000;  // far more than the stub clock can consume
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  IoFrame request{};
  create_execute_position(request, test::OWN_ID, test::DST_ID, true, 100);
  IoFrame response{};
  engine.send_and_receive(request, response, FREQ_CH2);

  EXPECT_EQ(radio.get_send_count(), EXCHANGE_RETRY_COUNT) << "with budget to spare the retry count is unchanged";
}

TEST(Exchange, ExecuteUsesUserDefaultAceiPriority) {
  // A real 2W hub (Velux KIG300, 2026-08-14 capture) sends ACEI 0x63 -- level 3, user_default.
  // Claiming a higher priority than the reference controller is what this hub used to do.
  IoFrame f{};
  ASSERT_TRUE(create_execute_position(f, test::OWN_ID, test::DST_ID, true, 100));
  EXPECT_EQ(f.data[1], 0x63);
  EXPECT_EQ((f.data[1] & ACEI_LEVEL_MASK) >> ACEI_LEVEL_SHIFT, ACEI_LEVEL_USER_DEFAULT);
}

// ============================================================================
// A failure report has to say which kind of failure it was. Every wait_for_packet() clears the
// radio's capture before listening, so recording "the latest" meant the report always described
// the final timed-out wait — making cap_valid=0 tautological and hiding whether the radio heard
// nothing or heard something this layer discarded.
// ============================================================================

TEST(Exchange, FailureReportKeepsTheInformativeCaptureNotTheLastEmptyOne) {
  MockRadio radio;
  radio.set_emulate_capture_lifecycle(true);
  radio.set_exchange_wait_slice_ms(1000000);
  RadioDriver *radio_ptr = &radio;

  TuningConfig tuning;
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  // One frame arrives and is correctly ignored (wrong endpoints), then nothing for the rest of the
  // exchange. The radio demonstrably heard something.
  const uint8_t other_device[3] = {0x55, 0x66, 0x77};
  IoFrame unrelated = build_status_response(other_device, test::OWN_ID);
  uint8_t raw[64];
  uint8_t raw_len = serialize(unrelated, raw, sizeof(raw));
  RadioRxPacket pkt{};
  pkt.len = raw_len;
  memcpy(pkt.data, raw, raw_len);
  pkt.freq_hz = FREQ_CH2;
  radio.queue_rx(pkt);

  IoFrame request{};
  create_execute_position(request, test::OWN_ID, test::DST_ID, true, 100);
  IoFrame response{};
  ASSERT_EQ(engine.send_and_receive(request, response, FREQ_CH2), ExchangeOutcome::FAILED);

  EXPECT_TRUE(engine.get_debug().capture_valid)
      << "the exchange received a frame, so its report must not claim the radio heard nothing";
  EXPECT_EQ(engine.get_debug().capture_freq_hz, FREQ_CH2) << "and it should describe the frame actually heard";
}

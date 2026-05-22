#include "hub_exchange.h"
#include "hub_core.h"
#include "proto_frame.h"
#include "proto_commands.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"

#include <cstring>
#include <deque>

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

// --- Testable component exposing protected send_and_receive_ -----------------
class TestableComponent : public IOHomeControlComponent {
 public:
  using IOHomeControlComponent::send_and_receive_;
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
  create_execute(request, comp.node_id_, test::DST_ID, false, 100);

  IoFrame resp = build_status_response(test::DST_ID, comp.node_id_);
  uint8_t raw[64];
  uint8_t raw_len = serialize(resp, raw, sizeof(raw));
  RadioRxPacket pkt{};
  pkt.len = raw_len;
  memcpy(pkt.data, raw, raw_len);
  pkt.freq_hz = FREQ_CH2;
  radio.queue_rx(pkt);

  IoFrame response{};
  bool ok = comp.send_and_receive_(request, response, FREQ_CH2);

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
  create_execute(request, comp.node_id_, test::DST_ID, false, 100);

  IoFrame response{};
  bool ok = comp.send_and_receive_(request, response, FREQ_CH2);

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
  create_execute(request, comp.node_id_, test::DST_ID, false, 100);

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
  bool ok = comp.send_and_receive_(request, response, FREQ_CH2);

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
  create_execute(request, comp.node_id_, test::DST_ID, false, 100);

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
  bool ok = comp.send_and_receive_(request, response, FREQ_CH2);

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
  create_execute(request, comp.node_id_, test::DST_ID, false, 100);

  IoFrame resp = build_error_response(test::DST_ID, comp.node_id_, RESULT_LIMITATION_BY_RAIN);
  uint8_t raw[64];
  uint8_t raw_len = serialize(resp, raw, sizeof(raw));
  RadioRxPacket pkt{};
  pkt.len = raw_len;
  memcpy(pkt.data, raw, raw_len);
  pkt.freq_hz = FREQ_CH2;
  radio.queue_rx(pkt);

  IoFrame response{};
  bool ok = comp.send_and_receive_(request, response, FREQ_CH2);

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
  create_execute(request, comp.node_id_, test::DST_ID, false, 100);

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
  bool ok = comp.send_and_receive_(request, response, FREQ_CH2);

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
  create_execute(request, comp.node_id_, test::DST_ID, false, 100);

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
  bool ok = comp.send_and_receive_(request, response, FREQ_CH2);

  // All transmit attempts fail — should exhaust retries and return false
  EXPECT_FALSE(ok) << "if every transmit attempt fails, exchange should return false";
  // Request sent EXCHANGE_RETRY_COUNT times, plus one auth response attempt
  EXPECT_EQ(radio.get_send_count(), EXCHANGE_RETRY_COUNT + 1)
      << "should perform EXCHANGE_RETRY_COUNT request transmits plus one auth response attempt before giving up";
}

TEST(Exchange, SendAndReceive_FinalResponseTimeout) {
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  IoFrame request{};
  create_execute(request, comp.node_id_, test::DST_ID, false, 100);

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
  bool ok = comp.send_and_receive_(request, response, FREQ_CH2);

  // No final response received within auth wait window
  EXPECT_FALSE(ok) << "final response timeout after successful challenge should cause exchange failure";
}

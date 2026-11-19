/// @file exchange_test.cpp
/// @brief Tests for the outbound exchange state machine of hub_exchange.cpp: the context structs, send_and_receive_
/// through a mock radio, and the pinned HMAC content and retry exhaustion cases.
///
/// The other subjects of the exchange engine are in the exchange_*_test.cpp files.

#include "hub_exchange.h"
#include "hub_core.h"
#include "proto_frame.h"
#include "proto_commands.h"
#include "proto_crypto.h"
#include "radio_sx1276.h"
#include "radio_sx1262.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"

#include <cstring>
#include <vector>

#include "exchange_fixture.h"

using namespace esphome::home_io_control;
using namespace exchange_test;

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
  ctx.try_start_ms = 0;
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
  // We'll test the decision helpers from the decisions_*.h headers.
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

  // A challenge we answered proves the device received the request, so this is not treated as a
  // failure — it is an acceptance we could not confirm (a challenge says nothing about whether our
  // 0x3D answer itself arrived). Some devices never close the exchange at all: a Somfy RS100's next
  // transmission after our 0x3D was measured at 3.4-12 s, or never, against a 500 ms window, while
  // a Somfy awning acks synchronously with 0x04. See ExchangeOutcome.
  EXPECT_EQ(outcome, ExchangeOutcome::SUCCESS_UNCONFIRMED)
      << "an authenticated request with no final response is accepted, not failed";
  EXPECT_EQ(response.cmd, 0) << "there was no response frame, so none should be handed back";
  // No re-send here: this hub has never seen the device close an EXECUTE, and a device that never
  // does (it reports through a later status update) is already acting on the first copy. Re-sending
  // to a device that does normally confirm is covered by the ResendRig tests below.
  EXPECT_EQ(radio.get_send_count(), 2) << "expected exactly the request plus the auth response, with no retries";
}

TEST(Exchange, SendAndReceive_ExecuteStopsAfterOneUnconfirmedAccept) {
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  IoFrame request{};
  create_execute_position(request, comp.node_id_, test::DST_ID, false, 100);

  uint8_t chal_data[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
  IoFrame challenge = build_challenge(test::DST_ID, comp.node_id_, chal_data);
  uint8_t raw_chal[64];
  uint8_t len_chal = serialize(challenge, raw_chal, sizeof(raw_chal));
  RadioRxPacket chal_pkt{};
  chal_pkt.len = len_chal;
  memcpy(chal_pkt.data, raw_chal, len_chal);
  radio.queue_rx(chal_pkt);
  // No final response queued.

  IoFrame response{};
  const ExchangeOutcome outcome = comp.send_and_receive_(request, response, FREQ_CH2);

  EXPECT_EQ(outcome, ExchangeOutcome::SUCCESS_UNCONFIRMED)
      << "CMD_EXECUTE authenticated without a final reply must still count as accepted";
  EXPECT_EQ(radio.get_send_count(), 2)
      << "CMD_EXECUTE to a device never seen to confirm must not be re-sent: it may already be acting on it";
}

// --- Re-sending an unconfirmed EXECUTE -----------------------------------------------------
// A CMD_EXECUTE the target accepted without a closing reply is sent once more, but only to a target
// that has confirmed an EXECUTE before (decisions::retry_after_unconfirmed_accept_is_safe()). A
// standalone engine with a scripted evidence provider decides "confirms" per test; a ManualClock
// makes each silent final wait expire at its deadline, as on a real radio.

TEST(Exchange, AcceptedSubscribeEventIsResentOnceAndNoMore) {
  // A subscription's answer carries the reading: a try that was challenged (accepted) but whose
  // answer never came is sent once more, then the exchange ends.
  ResendRig rig;
  rig.queue_challenge();                  // try 1: challenged, then the final wait stays silent
  rig.radio.queue_rx_hold_until_sent(3);  // silent until the re-sent request (send #3) is out
  rig.queue_challenge();                  // try 2: challenged again, final silent again
  IoFrame request{};
  ASSERT_TRUE(create_subscribe_event(request, test::OWN_ID, test::DST_ID, EVENT_LOCAL_SENSOR, false));

  EXPECT_EQ(rig.send(request), ExchangeOutcome::SUCCESS_UNCONFIRMED);
  ASSERT_EQ(rig.radio.get_send_count(), 4) << "request, 0x3D, the same request again, 0x3D; no third copy";
  EXPECT_EQ(rig.radio.get_sent_data()[0], rig.radio.get_sent_data()[2]) << "the re-send is the same subscription";
}

TEST(Exchange, UnansweredSubscribeEventKeepsTheNormalRetryBudget) {
  ResendRig rig;  // nothing queued: the device never answers at all
  IoFrame request{};
  ASSERT_TRUE(create_subscribe_event(request, test::OWN_ID, test::DST_ID, EVENT_LOCAL_SENSOR, false));

  EXPECT_EQ(rig.send(request), ExchangeOutcome::FAILED);
  EXPECT_EQ(rig.radio.get_send_count(), EXCHANGE_RETRY_COUNT);
}

TEST(Exchange, UnconfirmedExecuteToAConfirmingDeviceIsResentAndCanSucceed) {
  ResendRig rig;
  rig.queue_challenge();                  // try 1: challenged, then the final wait stays silent
  rig.radio.queue_rx_hold_until_sent(3);  // silent until the re-sent request (send #3) is out
  rig.queue_challenge();                  // try 2: challenged again
  rig.radio.queue_rx_hold_until_sent(4);  // ... and closed after our second 0x3D (send #4)
  rig.queue_final_reply();

  EXPECT_EQ(rig.send(stop_request()), ExchangeOutcome::SUCCESS_WITH_RESPONSE);
  ASSERT_EQ(rig.radio.get_send_count(), 4) << "request, 0x3D, the same request again, 0x3D";
  EXPECT_EQ(rig.radio.get_sent_data()[0], rig.radio.get_sent_data()[2]) << "the re-send is the same command";
}

TEST(Exchange, UnconfirmedExecuteResendWaitsTheLongerGap) {
  ResendRig rig;
  rig.queue_challenge();
  rig.radio.queue_rx_hold_until_sent(3);
  rig.queue_challenge();
  rig.radio.queue_rx_hold_until_sent(4);
  rig.queue_final_reply();

  ASSERT_EQ(rig.send(stop_request()), ExchangeOutcome::SUCCESS_WITH_RESPONSE);
  const auto &t = rig.radio.send_times_ms();
  ASSERT_EQ(t.size(), 4u);
  // From our first 0x3D to the re-sent command: the whole final-reply window, then the re-send gap
  // in place of the ordinary retry gap, so a device still acting on the first copy has finished.
  EXPECT_EQ(t[2] - t[1],
            rig.tuning.exchange_response_wait_ms + UNCONFIRMED_EXECUTE_RESEND_DELAY_MS + CHANNEL_CHECK_TIME_MS)
      << "the re-sent request's channel check comes after the gap";
}

TEST(Exchange, UnconfirmedExecuteIsResentAtMostOnce) {
  ResendRig rig;
  rig.queue_challenge();
  rig.radio.queue_rx_hold_until_sent(3);
  rig.queue_challenge();  // the re-send is challenged too, and again never closed

  EXPECT_EQ(rig.send(stop_request()), ExchangeOutcome::SUCCESS_UNCONFIRMED);
  EXPECT_EQ(rig.radio.get_send_count(), 4) << "one re-send, never a third copy of the command";
}

TEST(Exchange, UnansweredResendIsNotFollowedByAThirdCopy) {
  ResendRig rig;
  rig.queue_challenge();  // try 1 is challenged and never closed; the re-send then draws nothing at all

  EXPECT_EQ(rig.send(stop_request()), ExchangeOutcome::SUCCESS_UNCONFIRMED)
      << "the device did accept the first copy, so this is still an unconfirmed acceptance";
  EXPECT_EQ(rig.radio.get_send_count(), 3)
      << "request, 0x3D, the re-send; the ordinary failure retry must not add a third copy";
}

TEST(Exchange, UnconfirmedExecuteIsNotResentToADeviceNeverSeenToConfirm) {
  ResendRig rig;
  rig.confirms = false;  // e.g. a device that always reports through a later status update instead
  rig.queue_challenge();

  EXPECT_EQ(rig.send(stop_request()), ExchangeOutcome::SUCCESS_UNCONFIRMED);
  EXPECT_EQ(rig.radio.get_send_count(), 2);
}

TEST(Exchange, UnconfirmedFavoriteIsNotResentEvenToAConfirmingDevice) {
  ResendRig rig;
  rig.queue_challenge();
  IoFrame favorite{};
  create_execute_command(favorite, test::OWN_ID, test::DST_ID, false, CoverCommand::FAVORITE);

  EXPECT_EQ(rig.send(favorite), ExchangeOutcome::SUCCESS_UNCONFIRMED);
  EXPECT_EQ(rig.radio.get_send_count(), 2) << "a second \"My\" could stop the move the first one started";
}

TEST(Exchange, UnconfirmedExecuteResendStaysInsideTheExchangeBudget) {
  ResendRig rig;
  // The first try's final wait (500 ms) plus the re-send gap (750 ms) would start the re-send after
  // a 1000 ms budget, so it is not attempted, and not waited for either.
  rig.tuning.exchange_total_budget_ms = 1000;
  rig.queue_challenge();
  const uint32_t start = esphome::millis();

  EXPECT_EQ(rig.send(stop_request()), ExchangeOutcome::SUCCESS_UNCONFIRMED);
  EXPECT_EQ(rig.radio.get_send_count(), 2);
  EXPECT_LT(esphome::millis() - start, rig.tuning.exchange_total_budget_ms)
      << "a re-send that cannot start inside the budget must not be waited for";
  EXPECT_STREQ(rig.engine.get_debug().stage, "retry_budget_exhausted")
      << "the exchange loop's budget guard refused the re-send, like any other try";
}

TEST(Exchange, SendAndReceive_StatusPollRetriesAfterUnconfirmedAccept) {
  // Same radio script as SendAndReceive_ExecuteStopsAfterOneUnconfirmedAccept: one challenge
  // answered, then silence for the rest of the exchange. Unlike CMD_EXECUTE, CMD_PRIVATE has no
  // side effect to repeat, so an unconfirmed accept on try 1 must not stop the retry loop.
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  IoFrame request{};
  create_get_status(request, comp.node_id_, test::DST_ID, /*low_power=*/false);

  uint8_t chal_data[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
  IoFrame challenge = build_challenge(test::DST_ID, comp.node_id_, chal_data);
  uint8_t raw_chal[64];
  uint8_t len_chal = serialize(challenge, raw_chal, sizeof(raw_chal));
  RadioRxPacket chal_pkt{};
  chal_pkt.len = len_chal;
  memcpy(chal_pkt.data, raw_chal, len_chal);
  radio.queue_rx(chal_pkt);
  // No final response, and no more challenges: tries 2 and 3 see nothing at all.

  IoFrame response{};
  const ExchangeOutcome outcome = comp.send_and_receive_(request, response, FREQ_CH2);

  EXPECT_EQ(outcome, ExchangeOutcome::SUCCESS_UNCONFIRMED)
      << "a status poll that never got a reply is still an unconfirmed accept, not a failure";
  EXPECT_EQ(comp.exchange_engine_.get_debug().tries, EXCHANGE_RETRY_COUNT)
      << "every one of the EXCHANGE_RETRY_COUNT tries must actually run, not stop after the first accept";
  EXPECT_EQ(radio.get_send_count(), 4)
      << "try 1 sends the request plus the auth response (2); tries 2 and 3 each send only the "
         "request, since no challenge arrives to answer (1 + 1)";
}

namespace {

/// @brief Reactive mock: answers every transmitted request with a fresh challenge, but only closes
/// the exchange with a real final response on the second auth response — modelling a device that
/// accepted the first try silently (challenge answered, no reply) and only replied on retry.
///
/// A pre-queued script doesn't work here: MockRadio's RX queue is strict FIFO across the whole
/// exchange, so a final response queued ahead of time gets dequeued during try 1's final-response
/// wait instead of try 2's — classify_exchange_final_response() only checks endpoints, and a 0x3C
/// challenge matches them just as well as a 0x04 reply. Reacting to each outbound frame as it is
/// sent keeps every queued reply aligned with the wait it's meant for.
class UnconfirmedThenFinalOnRetryMockRadio : public MockRadio {
 public:
  TxResult send_packet(const uint8_t *data, uint8_t len, const RadioTxConfig &tx) override {
    TxResult result = MockRadio::send_packet(data, len, tx);
    IoFrame frame;
    if (!parse(data, len, frame))
      return result;
    if (frame.cmd == CMD_PRIVATE) {
      this->auth_count_this_try_ = 0;
      this->queue_rx(build_challenge_packet(frame));
    } else if (frame.cmd == CMD_CHALLENGE_RESP) {
      if (++this->auth_count_this_try_ == 1 && ++this->try_count_ == 2)
        this->queue_rx(build_final_packet(frame));
    }
    return result;
  }

 private:
  static RadioRxPacket build_challenge_packet(const IoFrame &request) {
    uint8_t chal_data[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    IoFrame challenge{};
    init_frame(challenge, true, false, false, false);
    set_dst(challenge, request.src);
    set_src(challenge, request.dst);
    set_cmd(challenge, CMD_CHALLENGE_REQ, chal_data, sizeof(chal_data));
    RadioRxPacket pkt{};
    uint8_t raw[64];
    pkt.len = serialize(challenge, raw, sizeof(raw));
    memcpy(pkt.data, raw, pkt.len);
    return pkt;
  }

  static RadioRxPacket build_final_packet(const IoFrame &auth_response) {
    IoFrame resp{};
    init_frame(resp, true, false, true, false);
    set_dst(resp, auth_response.src);
    set_src(resp, auth_response.dst);
    uint8_t payload[6] = {0};
    set_cmd(resp, CMD_PRIVATE_RESP, payload, sizeof(payload));
    RadioRxPacket pkt{};
    uint8_t raw[64];
    pkt.len = serialize(resp, raw, sizeof(raw));
    memcpy(pkt.data, raw, pkt.len);
    return pkt;
  }

  uint8_t auth_count_this_try_{0};
  uint8_t try_count_{0};
};

}  // namespace

TEST(Exchange, SendAndReceive_StatusPollUnconfirmedThenAnsweredReturnsResponse) {
  TestableComponent comp;
  comp.initialized_ = true;
  UnconfirmedThenFinalOnRetryMockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  IoFrame request{};
  create_get_status(request, comp.node_id_, test::DST_ID, /*low_power=*/false);

  IoFrame response{};
  const ExchangeOutcome outcome = comp.send_and_receive_(request, response, FREQ_CH2);

  EXPECT_EQ(outcome, ExchangeOutcome::SUCCESS_WITH_RESPONSE)
      << "a retried status poll must still return the response once one arrives";
  EXPECT_EQ(response.cmd, CMD_PRIVATE_RESP);
}

// ============================================================================
// Step 11a — pinning tests: verify exchange HMAC content, retry exhaustion,
// unrelated-frame filtering during auth wait, and inbound authenticate_request_.
// These tests must pass before and after the ExchangeEngine extraction.
// ============================================================================

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

// --- Pinning test: wait_for_first_response_() stays on the request channel ------------------
//
// Step B: a unicast reply comes back on the channel the request went out on (0 of 300 unicast RX
// events measured off-channel across SX1276/SX1262/LR1121), so this wait now holds the request
// channel for its whole window rather than rotating. That subsumes what used to be two separate
// pins — "still hops on a genuinely empty dwell" and "does not hop after a rejected frame" — into
// one behaviour: HOLD_REQUEST_CHANNEL never calls change_frequency() at all, so the two stimuli
// are no longer distinguishable at this call site. Both stimuli are kept, back to back, so this
// test still exercises the same two code paths the two old tests did.

TEST(Exchange, SendAndReceive_FirstResponseWaitStaysOnRequestChannel) {
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  IoFrame request{};
  create_execute_position(request, comp.node_id_, test::DST_ID, false, 50);

  // An unrelated frame (wrong src) lands on the very first listen (the rejected-frame stimulus),
  // then silence for the rest of the window (the idle-dwell stimulus).
  IoFrame noise = build_status_response(test::FOREIGN_ID, comp.node_id_);
  uint8_t raw_noise[64];
  uint8_t len_noise = serialize(noise, raw_noise, sizeof(raw_noise));
  RadioRxPacket noise_pkt{};
  noise_pkt.len = len_noise;
  memcpy(noise_pkt.data, raw_noise, len_noise);
  radio.queue_rx(noise_pkt);
  radio.queue_rx_silence(5);

  IoFrame response{};
  comp.send_and_receive_(request, response, FREQ_CH2);

  // The only retunes are the channel checks of the later attempts (each leaves the previous attempt's
  // channel); a wait itself never retunes, whether the dwell is genuinely empty or a frame arrived.
  EXPECT_EQ(radio.freq_history().size(), static_cast<size_t>(radio.get_send_count() - 1))
      << "HOLD_REQUEST_CHANNEL must never retune, whether the dwell is genuinely empty or a "
         "rejected frame arrived";
  ASSERT_GE(radio.call_log().size(), 2u) << "need the reception plus at least one following slice";
  EXPECT_EQ(radio.call_log()[0], MockRadio::CallKind::kWait) << "the first listen is the one that receives the noise";
  EXPECT_EQ(radio.call_log()[1], MockRadio::CallKind::kWait)
      << "no hop between the rejected reception and the next slice";
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

// --- Pinning test: wait_for_final_response_() stays on the request channel -----------------
//
// Every wait loop's channel policy is pinned by its own test — wait_for_first_response_ (above),
// wait_for_key_challenge_, wait_for_key_confirm_, wait_for_discovery_response_, and
// collect_broadcast_responses each already have one; this is wait_for_final_response_'s. Same
// reasoning as the first-response case: HOLD_REQUEST_CHANNEL, so a unicast final reply (0 of 300
// measured off-channel) never needs a hop.

TEST(Exchange, SendAndReceive_FinalResponseWaitStaysOnRequestChannel) {
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  IoFrame request{};
  create_execute_position(request, comp.node_id_, test::DST_ID, false, 100);

  // Challenge from the correct device, so the exchange proceeds into the final-response wait.
  uint8_t chal_data[6] = {0xCA, 0xFE, 0xBA, 0xBE, 0x00, 0x01};
  IoFrame challenge = build_challenge(test::DST_ID, comp.node_id_, chal_data);
  uint8_t raw_chal[64];
  uint8_t len_chal = serialize(challenge, raw_chal, sizeof(raw_chal));
  RadioRxPacket chal_pkt{};
  chal_pkt.len = len_chal;
  memcpy(chal_pkt.data, raw_chal, len_chal);
  radio.queue_rx(chal_pkt);

  // An unrelated frame during the final-response wait (rejected-frame stimulus), then silence
  // (idle-dwell stimulus) — same two-stimulus shape as the first-response pin above.
  IoFrame noise = build_status_response(test::FOREIGN_ID, comp.node_id_);
  uint8_t raw_noise[64];
  uint8_t len_noise = serialize(noise, raw_noise, sizeof(raw_noise));
  RadioRxPacket noise_pkt{};
  noise_pkt.len = len_noise;
  memcpy(noise_pkt.data, raw_noise, len_noise);
  radio.queue_rx(noise_pkt);
  radio.queue_rx_silence(5);

  IoFrame response{};
  comp.send_and_receive_(request, response, FREQ_CH2);

  EXPECT_TRUE(radio.freq_history().empty())
      << "HOLD_REQUEST_CHANNEL must never retune during the final-response wait, whether the "
         "dwell is genuinely empty or a rejected frame arrived";
  EXPECT_GE(radio.get_send_count(), 2) << "request + auth response — proves the exchange actually "
                                          "reached wait_for_final_response_() rather than dying earlier";
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

// --- Inbound continuation frames follow the driver's response preamble ------

namespace {
/// A challenge-answering radio whose driver asks for a response preamble distinct from
/// SHORT_PREAMBLE, so a test can tell "follows the driver" apart from "hard-coded protocol floor".
class LongResponsePreambleMockRadio : public RespondOnChallengeMockRadio {
 public:
  static constexpr uint16_t RESPONSE_PREAMBLE = 12;
  uint16_t response_preamble() const override { return RESPONSE_PREAMBLE; }
};
static_assert(LongResponsePreambleMockRadio::RESPONSE_PREAMBLE != SHORT_PREAMBLE,
              "the test value must differ from the protocol floor to prove anything");
}  // namespace

TEST(Exchange, AuthenticateRequest_ChallengeUsesDriverResponsePreamble) {
  TestableComponent comp;
  comp.initialized_ = true;
  LongResponsePreambleMockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  IoFrame status_update = build_status_update_from_device(test::DST_ID, comp.node_id_);
  radio.arm(status_update, test::TEST_SYSTEM_KEY, /*valid=*/true);

  ASSERT_TRUE(comp.authenticate_request_(status_update, FREQ_CH2));

  ASSERT_GE(radio.get_tx_configs().size(), 1u);
  EXPECT_EQ(radio.get_tx_configs()[0].preamble_len, LongResponsePreambleMockRadio::RESPONSE_PREAMBLE)
      << "our inbound 0x3C is a continuation frame and must use the driver's response preamble";
}

TEST(Exchange, StatusUpdateAckUsesDriverResponsePreambleOnAllChannels) {
  TestableComponent comp;
  comp.initialized_ = true;
  LongResponsePreambleMockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  IoFrame status_update = build_status_update_from_device(test::DST_ID, comp.node_id_);
  radio.arm(status_update, test::TEST_SYSTEM_KEY, /*valid=*/true);

  uint8_t raw[RADIO_PACKET_BUFFER_SIZE];
  RadioRxPacket pkt{};
  pkt.len = serialize(status_update, raw, sizeof(raw));
  ASSERT_GT(pkt.len, 0);
  memcpy(pkt.data, raw, pkt.len);
  pkt.freq_hz = FREQ_CH2;
  comp.process_received_packet_(pkt);

  // 0x3C challenge, then the 0x72 ACK once per channel.
  const auto &tx = radio.get_tx_configs();
  ASSERT_EQ(tx.size(), 4u) << "expected the challenge plus three ACK copies";
  for (size_t i = 0; i < tx.size(); ++i) {
    EXPECT_EQ(tx[i].preamble_len, LongResponsePreambleMockRadio::RESPONSE_PREAMBLE)
        << "tx #" << i << " is a continuation frame and must use the driver's response preamble";
  }
  // The receive channel (CH2) goes first, then the others in their usual order.
  EXPECT_EQ(tx[1].freq_hz, FREQ_CH2);
  EXPECT_EQ(tx[2].freq_hz, FREQ_CH1);
  EXPECT_EQ(tx[3].freq_hz, FREQ_CH3);
}

TEST(Exchange, StatusUpdateAckStartsOnTheChannelTheExchangeRanOn) {
  TestableComponent comp;
  comp.initialized_ = true;
  LongResponsePreambleMockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  IoFrame status_update = build_status_update_from_device(test::DST_ID, comp.node_id_);
  radio.arm(status_update, test::TEST_SYSTEM_KEY, /*valid=*/true);

  RadioRxPacket pkt{};
  pkt.len = serialize(status_update, pkt.data, sizeof(pkt.data));
  ASSERT_GT(pkt.len, 0);
  pkt.freq_hz = FREQ_CH3;
  comp.process_received_packet_(pkt);

  const auto &tx = radio.get_tx_configs();
  ASSERT_EQ(tx.size(), 4u);
  EXPECT_EQ(tx[1].freq_hz, FREQ_CH3);
  EXPECT_EQ(tx[2].freq_hz, FREQ_CH1);
  EXPECT_EQ(tx[3].freq_hz, FREQ_CH2);
}

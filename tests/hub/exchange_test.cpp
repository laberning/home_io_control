#include "hub_exchange.h"
#include "hub_core.h"
#include "proto_frame.h"
#include "proto_commands.h"
#include "proto_crypto.h"
#include "radio_sx1276.h"
#include "radio_sx1262.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"

#include <algorithm>
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
  ctx.try_start_ms = 0;
  ctx.wait_ms = 0;
  ctx.first_response_ms = 0;
  return ctx;
}

namespace {

/// The plan the request path builds for a read of @p request: decisions::attempt_plan() with the
/// transmit time the engine estimates for the request's start preamble on FREQ_CH2. What a test
/// compares the attempts of a run against; the exchange itself is always started from a read kind.
/// @param with_short_lead True for a settling read of a low-power receiver in an unknown state.
decisions::AttemptPlan expected_plan(const ExchangeEngine &engine, const TuningConfig &tuning, decisions::ReadKind kind,
                                     const IoFrame &request, bool with_short_lead = false) {
  const bool low_power = (request.ctrl1 & CTRL1_LOW_POWER) != 0;
  return decisions::attempt_plan(kind, low_power, tuning,
                                 engine.tx_time_ms(request, FREQ_CH2, engine.request_preamble_for(request)), 0,
                                 with_short_lead);
}

/// Options that run an exchange as a read of @p kind.
ExchangeOptions read_of(decisions::ReadKind kind) { return decisions::AttemptRequest::of_kind(kind); }

}  // namespace

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

namespace {

// --- Testable component exposing protected exchange internals -----------------
class TestableComponent : public IOHomeControlComponent {
 public:
  using IOHomeControlComponent::send_and_receive_;
  using IOHomeControlComponent::authenticate_request_;
  using IOHomeControlComponent::process_received_packet_;
  using IOHomeControlComponent::initialized_;
  using IOHomeControlComponent::radio_;
  using IOHomeControlComponent::node_id_;
  using IOHomeControlComponent::system_key_;
  using IOHomeControlComponent::exchange_engine_;
  using IOHomeControlComponent::tuning_;
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

namespace {

struct ResendRig {
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
  RadioDriver *radio_ptr{&radio};
  TuningConfig tuning{};
  ExchangeEngine engine{&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning};
  bool confirms{true};
  int lookups{0};  ///< Target-evidence lookups of the exchange so far.

  ResendRig() {
    engine.set_target_evidence_provider([this](const uint8_t *, decisions::TargetEvidence &out) {
      lookups++;
      out = decisions::TargetEvidence{};
      out.confirms_execute = confirms;
      return true;
    });
  }

  void queue(const IoFrame &frame) {
    uint8_t raw[64];
    RadioRxPacket pkt{};
    pkt.len = serialize(frame, raw, sizeof(raw));
    memcpy(pkt.data, raw, pkt.len);
    radio.queue_rx(pkt);
  }
  void queue_challenge() {
    const uint8_t chal[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    queue(build_challenge(test::DST_ID, test::OWN_ID, chal));
  }
  void queue_final_reply() { queue(build_status_response(test::DST_ID, test::OWN_ID)); }

  ExchangeOutcome send(const IoFrame &request) {
    IoFrame response{};
    return engine.send_and_receive(request, response, FREQ_CH2);
  }
};

IoFrame stop_request() {
  IoFrame f{};
  create_execute_command(f, test::OWN_ID, test::DST_ID, false, CoverCommand::STOP);
  return f;
}

}  // namespace

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
  EXPECT_EQ(radio.response_preamble(), MOCK_RESPONSE_PREAMBLE)
      << "SX1262 should return the configured response preamble";
  // Byte-denominated like SHORT_PREAMBLE (set_packet_params_() converts to bits at the SPI
  // boundary); the 3.5 ms default is 17 bytes at 8 bit periods each, above the protocol's nominal
  // 8-byte floor.
  EXPECT_EQ(radio.response_preamble(), 17u);
  EXPECT_GE(radio.response_preamble(), SHORT_PREAMBLE);
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
  // MockSpi). Verifies the default is the 3.5 ms duration at 10 bit periods per byte and that a
  // tuning override propagates to the observable response_preamble(), and that set_rx_bandwidth_
  // accepts every enum option without touching hardware.
  MockSpi spi;
  MockPin rst, dio0, dio4;
  RadioSX1276 sx1276(&spi, &rst, &dio0, &dio4, 17, 0x80);

  // Default (no tuning applied): 3.5 ms is 13 bytes at 10 bit periods each.
  EXPECT_EQ(sx1276.response_preamble(), 13u);

  TuningConfig cfg{};
  cfg.response_preamble_ms = 10.0F;  // 384 bit periods: 38 bytes at 10 bit periods each
  for (auto bw : {SX1276RxBandwidth::BW_20_8_KHZ, SX1276RxBandwidth::BW_41_7_KHZ, SX1276RxBandwidth::BW_125_0_KHZ}) {
    cfg.sx1276_rx_bandwidth = bw;
    sx1276.apply_tuning(cfg);  // set_rx_bandwidth_ + set_response_preamble_ (no crash / no SPI dependency)
  }
  EXPECT_EQ(sx1276.response_preamble(), 38) << "apply_tuning must route response_preamble_ms to the driver";

  // Applying defaults again restores the default preamble.
  sx1276.apply_tuning(TuningConfig{});
  EXPECT_EQ(sx1276.response_preamble(), 13u);
}

TEST(Exchange, SendAndReceive_LowPowerStartFrameUsesLongPreamble) {
  // A START frame whose target is a low-power device (CTRL1_LOW_POWER set) keeps MOCK_WAKE_PREAMBLE:
  // the long preamble is the wake-up burst for a duty-cycled receiver.
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  IoFrame request{};
  create_execute_position(request, comp.node_id_, test::DST_ID, /*low_power=*/true, 100);
  ASSERT_TRUE(is_start(request)) << "execute command should have START flag set";
  ASSERT_NE(request.ctrl1 & CTRL1_LOW_POWER, 0) << "low_power=true must set CTRL1_LOW_POWER";

  // No RX → times out after retries, but we can inspect the TX config
  IoFrame response{};
  comp.send_and_receive_(request, response, FREQ_CH2);

  ASSERT_GE(radio.get_tx_configs().size(), 1u) << "at least one TX should have been attempted";
  EXPECT_EQ(radio.get_tx_configs()[0].preamble_len, MOCK_WAKE_PREAMBLE)
      << "a low-power START frame must use MOCK_WAKE_PREAMBLE to wake the target";
}

TEST(Exchange, SendAndReceive_NormalStartFrameUsesNormalStartPreamble) {
  // A START frame to a non-low-power (always-alive) target uses the runtime-tunable
  // `start_preamble_ms`, not the 1024-byte wake-up burst.
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  IoFrame request{};
  create_execute_position(request, comp.node_id_, test::DST_ID, /*low_power=*/false, 100);
  ASSERT_TRUE(is_start(request)) << "execute command should have START flag set";
  ASSERT_EQ(request.ctrl1 & CTRL1_LOW_POWER, 0) << "low_power=false must leave CTRL1_LOW_POWER clear";

  // No RX → times out after retries, but we can inspect the TX config
  IoFrame response{};
  comp.send_and_receive_(request, response, FREQ_CH2);

  ASSERT_GE(radio.get_tx_configs().size(), 1u) << "at least one TX should have been attempted";
  EXPECT_EQ(radio.get_tx_configs()[0].preamble_len, mock_preamble_bytes(comp.tuning_.start_preamble_ms))
      << "a normal START frame must use `start_preamble_ms`, not MOCK_WAKE_PREAMBLE";
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
  // Non-START frames on SX1262 should use MOCK_RESPONSE_PREAMBLE, not MOCK_WAKE_PREAMBLE.
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
  EXPECT_EQ(radio.get_tx_configs()[0].preamble_len, MOCK_RESPONSE_PREAMBLE)
      << "non-START frame on SX1262 should use MOCK_RESPONSE_PREAMBLE";
}

namespace {

/// The preamble the first transmit of `request` carries, on a hub driving `radio`.
uint16_t first_preamble_on(MockRadio &radio, const IoFrame &request) {
  TestableComponent comp;
  comp.initialized_ = true;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);
  IoFrame response{};
  comp.send_and_receive_(request, response, FREQ_CH2);
  return radio.get_tx_configs().empty() ? 0 : radio.get_tx_configs()[0].preamble_len;
}

}  // namespace

// ADR 0097: every kind of frame carries its preamble as a duration, converted by the radio, so the
// same setting is fewer programmed bytes on a radio that sends 10 bit periods per byte and lasts
// the same on air.
TEST(Exchange, EveryFrameKindCarriesTheSameDurationOnATenBitRadioAsOnARawStreamRadio) {
  IoFrame normal{};
  create_execute_position(normal, test::OWN_ID, test::DST_ID, /*low_power=*/false, 100);
  IoFrame wake{};
  create_execute_position(wake, test::OWN_ID, test::DST_ID, /*low_power=*/true, 100);
  IoFrame continuation{};
  init_frame(continuation, true, false, false, false);
  set_dst(continuation, test::DST_ID);
  set_src(continuation, test::OWN_ID);
  uint8_t payload[16] = {0};
  set_cmd(continuation, CMD_KEY_TRANSFER, payload, sizeof(payload));

  struct Kind {
    const char *name;
    const IoFrame *frame;
    uint16_t raw_stream_bytes;
    uint16_t ten_bit_bytes;
    float ms;
  };
  const Kind kinds[] = {
      {"start frame to a mains target", &normal, 48, 38, START_PREAMBLE_MS},
      {"start frame to a low-power target", &wake, 2880, 2304, LOW_POWER_WAKE_MS},
      {"continuation frame", &continuation, 17, 13, RESPONSE_PREAMBLE_MS},
  };
  for (const Kind &kind : kinds) {
    MockRadioSX1262 raw_stream;
    TenBitPreambleRadio ten_bit;
    EXPECT_EQ(first_preamble_on(raw_stream, *kind.frame), kind.raw_stream_bytes) << kind.name << " (8 bits/byte)";
    EXPECT_EQ(first_preamble_on(ten_bit, *kind.frame), kind.ten_bit_bytes) << kind.name << " (10 bits/byte)";
    RadioTxConfig raw_cfg;
    raw_cfg.preamble_len = kind.raw_stream_bytes;
    RadioTxConfig ten_cfg;
    ten_cfg.preamble_len = kind.ten_bit_bytes;
    EXPECT_NEAR(raw_stream.tx_air_time_us(13, raw_cfg) / 1000.0, ten_bit.tx_air_time_us(13, ten_cfg) / 1000.0, 1.0)
        << kind.name << ": the same time on air";
  }
}

TEST(Exchange, ATunedStartDurationMovesEveryRadioToItsOwnByteCount) {
  IoFrame normal{};
  create_execute_position(normal, test::OWN_ID, test::DST_ID, /*low_power=*/false, 100);
  // start_preamble_ms is read at send time, so a live change (the Home Assistant number) applies to the next frame.
  for (const float ms : {5.0F, 20.0F}) {
    MockRadioSX1262 raw_stream;
    TenBitPreambleRadio ten_bit;
    TestableComponent comp;
    comp.initialized_ = true;
    comp.tuning_.start_preamble_ms = ms;
    memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
    memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);
    IoFrame response{};
    comp.radio_ = &raw_stream;
    comp.send_and_receive_(normal, response, FREQ_CH2);
    comp.radio_ = &ten_bit;
    comp.send_and_receive_(normal, response, FREQ_CH2);
    ASSERT_FALSE(raw_stream.get_tx_configs().empty());
    ASSERT_FALSE(ten_bit.get_tx_configs().empty());
    EXPECT_EQ(raw_stream.get_tx_configs()[0].preamble_len, raw_stream.preamble_bytes_for_ms(ms)) << ms;
    EXPECT_EQ(ten_bit.get_tx_configs()[0].preamble_len, ten_bit.preamble_bytes_for_ms(ms)) << ms;
    EXPECT_GT(raw_stream.get_tx_configs()[0].preamble_len, ten_bit.get_tx_configs()[0].preamble_len) << ms;
  }
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

  // TX 0 = request (`start_preamble_ms` — start frame to a non-low-power target)
  // TX 1 = auth response (should use MOCK_RESPONSE_PREAMBLE)
  ASSERT_GE(radio.get_tx_configs().size(), 2u) << "should have sent at least request + auth response";
  EXPECT_EQ(radio.get_tx_configs()[0].preamble_len, mock_preamble_bytes(comp.tuning_.start_preamble_ms))
      << "initial request (START, low_power=false) should use `start_preamble_ms`";
  EXPECT_EQ(radio.get_tx_configs()[1].preamble_len, MOCK_RESPONSE_PREAMBLE)
      << "auth response on SX1262 should use MOCK_RESPONSE_PREAMBLE";
}

TEST(Exchange, SendAndReceive_RequestPreambleOverrideAppliesToRequestOnly) {
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadioSX1262 radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  IoFrame request{};
  create_execute_position(request, comp.node_id_, test::DST_ID, /*low_power=*/true, 100);

  uint8_t chal_data[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  IoFrame challenge = build_challenge(test::DST_ID, comp.node_id_, chal_data);
  uint8_t raw_chal[64];
  RadioRxPacket chal_pkt{};
  chal_pkt.len = serialize(challenge, raw_chal, sizeof(raw_chal));
  memcpy(chal_pkt.data, raw_chal, chal_pkt.len);
  radio.queue_rx(chal_pkt);

  IoFrame response{};
  comp.exchange_engine_.send_and_receive(request, response, FREQ_CH2, ExchangeOptions{}.with_preamble(32));

  ASSERT_GE(radio.get_tx_configs().size(), 2u);
  EXPECT_EQ(radio.get_tx_configs()[0].preamble_len, 32u)
      << "the override replaces the rule's MOCK_WAKE_PREAMBLE for a low-power start frame";
  EXPECT_EQ(radio.get_tx_configs()[1].preamble_len, MOCK_RESPONSE_PREAMBLE)
      << "the 0x3D challenge response keeps the driver's response preamble";
}

// answer_challenge() is the one place that builds and sends a 0x3D, shared by the normal
// inbound-challenge path (handle_authentication_()) and pairing's post-0x32 challenge answer
// (wait_for_key_confirm_(), pairing_engine.cpp), so both send the exact same shape. This test
// drives it directly, bypassing send_and_receive_() entirely, to pin its own contract independent
// of the outbound retry loop around it.
TEST(Exchange, AnswerChallengeSendsExactly0x3DOverTheRequestTranscript) {
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadioSX1262 radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  IoFrame request{};
  create_execute_position(request, comp.node_id_, test::DST_ID, false, 100);

  uint8_t chal_data[HMAC_SIZE] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  IoFrame challenge = build_challenge(test::DST_ID, comp.node_id_, chal_data);

  ASSERT_TRUE(comp.exchange_engine_.answer_challenge(request, challenge, FREQ_CH2))
      << "answer_challenge should build and transmit the 0x3D";

  ASSERT_EQ(radio.get_sent_data().size(), 1u) << "answer_challenge should transmit exactly one frame";
  IoFrame sent{};
  const auto &raw = radio.get_sent_data()[0];
  ASSERT_TRUE(parse(raw.data(), static_cast<uint8_t>(raw.size()), sent)) << "transmitted frame must parse cleanly";

  IoFrame expected{};
  ASSERT_TRUE(create_challenge_resp(expected, request.dst, comp.node_id_, chal_data, request, comp.system_key_))
      << "reference create_challenge_resp() call should succeed";
  uint8_t expected_raw[64];
  const uint8_t expected_len = serialize(expected, expected_raw, sizeof(expected_raw));
  ASSERT_EQ(raw.size(), expected_len) << "answer_challenge must send exactly create_challenge_resp()'s bytes";
  EXPECT_EQ(0, memcmp(raw.data(), expected_raw, expected_len))
      << "answer_challenge must send exactly create_challenge_resp()'s bytes";

  ASSERT_EQ(radio.get_tx_configs().size(), 1u);
  EXPECT_EQ(radio.get_tx_configs()[0].preamble_len, MOCK_RESPONSE_PREAMBLE)
      << "answer_challenge should use the driver's response_preamble(), like handle_authentication_() did";
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

  TxResult send_packet(const uint8_t *data, uint8_t len, const RadioTxConfig &tx) override {
    TxResult result = MockRadio::send_packet(data, len, tx);
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

// ============================================================================
// collect_broadcast_responses tests
// ============================================================================
// Exercises ExchangeEngine directly (not through IOHomeControlComponent) to isolate this
// primitive from the action layer that drives it (ManagementActions::scan_paired_devices(),
// covered separately in tests/hub/hub_management_test.cpp). A small pairing_discovery_wait_ms keeps
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
  create_discovery_request(f, own, CMD_DISCOVER_SPE_REQ, BROADCAST_DISCOVER, /*low_power=*/false,
                           /*ack_capable=*/false, /*payload_enabled=*/false, /*payload=*/0, test::TEST_SYSTEM_KEY);
  return f;
}

/// Records every callback delivery so tests can assert on count, addresses, RSSI, receive
/// channel, and post-transmit latency. collect_broadcast_responses() neither stores nor
/// deduplicates replies, so this is also what proves it hands over duplicates rather than
/// filtering them.
struct CollectedReplies {
  std::vector<IoFrame> frames;
  std::vector<int16_t> rssi_dbm;
  std::vector<uint32_t> rx_freq_hz;
  std::vector<uint32_t> after_tx_ms;

  ExchangeEngine::BroadcastReplyHandler handler() {
    return [this](const IoFrame &frame, const ExchangeEngine::BroadcastReplyInfo &info) {
      this->frames.push_back(frame);
      this->rssi_dbm.push_back(info.rssi_dbm);
      this->rx_freq_hz.push_back(info.rx_freq_hz);
      this->after_tx_ms.push_back(info.after_tx_ms);
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
// Roll-call channel policy: collect_broadcast_responses()'s default policy
// (ROTATE_SKIPPING_REQUEST) leaves the request channel before its first listen, because a Somfy
// always-alive reply is seen there only 1 of 149 times. The two tests below call without the new
// `policy` argument, so they pin that default rather than a fixed universal behaviour — a caller
// that passes ROTATE_ALL_CHANNELS instead (see below) gets the opposite.
// ============================================================================

TEST(Exchange, CollectBroadcastResponses_LeavesRequestChannelImmediately) {
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  // A reply queued for the very first wait_for_packet() call is what this test needs to isolate
  // "before the first listen" — but collect_broadcast_responses() keeps collecting for the whole
  // window even after a match (it doesn't return on the first reply), so old code eventually hops
  // too once the queue runs dry, and freq_history() alone can't tell "hopped before listening
  // ever started" from "hopped after the queued reply was consumed". call_log() can: it's the
  // interleaved order of wait/hop calls, so the very first entry reveals whether a hop preceded
  // the first listen (new code, unconditional pre-listen hop) or a listen came first (old code,
  // which only ever hops after a wait_for_packet() call already returned false).
  radio.queue_rx(to_rx_packet(build_spe_response(test::DST_ID, test::OWN_ID)));

  IoFrame request = build_spe_request(test::OWN_ID);
  CollectedReplies collected;
  engine.collect_broadcast_responses(request, FREQ_CH2, CMD_DISCOVER_SPE_RESP, tuning.pairing_discovery_wait_ms,
                                     collected.handler());

  ASSERT_FALSE(radio.call_log().empty());
  EXPECT_EQ(radio.call_log().front(), MockRadio::CallKind::kHop)
      << "the engine must retune before its first listen, not after it";
  ASSERT_FALSE(radio.freq_history().empty());
  EXPECT_NE(radio.freq_history().front(), FREQ_CH2) << "the request channel must not be the first channel listened on";
}

TEST(Exchange, CollectBroadcastResponses_NeverListensOnRequestChannel) {
  const uint32_t request_channels[] = {FREQ_CH1, FREQ_CH2, FREQ_CH3};
  for (uint32_t request_freq : request_channels) {
    MockRadio radio;
    RadioDriver *radio_ptr = &radio;
    TuningConfig tuning = make_broadcast_test_tuning();
    tuning.pairing_discovery_wait_ms = 20;
    // collect_broadcast_responses() leaves spec.dwell_ms at 0, so listen() asks MockRadio's
    // hop_dwell_ms() (which reads this field) — force many iterations within the window.
    tuning.sx1276_discovery_hop_slice_ms = 1;
    ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

    IoFrame request = build_spe_request(test::OWN_ID);
    CollectedReplies collected;
    engine.collect_broadcast_responses(request, request_freq, CMD_DISCOVER_SPE_RESP, tuning.pairing_discovery_wait_ms,
                                       collected.handler());

    for (uint32_t visited : radio.freq_history()) {
      EXPECT_NE(visited, request_freq) << "request_freq=" << request_freq
                                       << ": the rotation must never land back on the request channel";
    }
    const auto &history = radio.freq_history();
    const bool saw_both_others =
        std::find(history.begin(), history.end(), request_freq == FREQ_CH1 ? FREQ_CH2 : FREQ_CH1) != history.end() &&
        std::find(history.begin(), history.end(), request_freq == FREQ_CH3 ? FREQ_CH2 : FREQ_CH3) != history.end();
    EXPECT_TRUE(saw_both_others) << "request_freq=" << request_freq
                                 << ": both non-request channels should appear over several hops";
  }
}

// Real VELUX low-power roll-call replies have been observed landing on the request channel, so a
// caller whose responders may do that passes ROTATE_ALL_CHANNELS instead of relying on the
// skipping default.
TEST(Exchange, CollectBroadcastResponses_RotateAllListensOnTheRequestChannel) {
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  // A reply on the very first wait_for_packet() call: only reachable if the first listen stays on
  // the request channel instead of hopping away from it first.
  RadioRxPacket reply_pkt = to_rx_packet(build_spe_response(test::DST_ID, test::OWN_ID));
  reply_pkt.freq_hz = FREQ_CH2;
  radio.queue_rx(reply_pkt);

  IoFrame request = build_spe_request(test::OWN_ID);
  CollectedReplies collected;
  uint8_t count =
      engine.collect_broadcast_responses(request, FREQ_CH2, CMD_DISCOVER_SPE_RESP, tuning.pairing_discovery_wait_ms,
                                         collected.handler(), ListenPolicy::ROTATE_ALL_CHANNELS);

  EXPECT_EQ(count, 1u) << "ROTATE_ALL_CHANNELS must catch a reply on the request channel";
  ASSERT_FALSE(radio.call_log().empty());
  EXPECT_EQ(radio.call_log().front(), MockRadio::CallKind::kWait)
      << "ROTATE_ALL_CHANNELS starts listening immediately, unlike the skipping default";
  EXPECT_NE(std::find(radio.freq_history().begin(), radio.freq_history().end(), FREQ_CH2), radio.freq_history().end())
      << "the request frequency must still appear in the rotation";
}

TEST(Exchange, CollectBroadcastResponses_ReportsReceiveChannel) {
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  // Transmit on CH2, but the queued reply reports arriving on CH1.
  RadioRxPacket reply_pkt = to_rx_packet(build_spe_response(test::DST_ID, test::OWN_ID));
  reply_pkt.freq_hz = FREQ_CH1;
  radio.queue_rx(reply_pkt);

  IoFrame request = build_spe_request(test::OWN_ID);
  CollectedReplies collected;
  uint8_t count = engine.collect_broadcast_responses(request, FREQ_CH2, CMD_DISCOVER_SPE_RESP,
                                                     tuning.pairing_discovery_wait_ms, collected.handler());

  ASSERT_EQ(count, 1u);
  ASSERT_EQ(collected.rx_freq_hz.size(), 1u);
  EXPECT_EQ(collected.rx_freq_hz[0], FREQ_CH1) << "the reply's own receive channel must be reported, not the TX one";
}

TEST(Exchange, CollectBroadcastResponses_ReportsLatencyAfterTransmit) {
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  // One silent slice, then a reply: host millis() advances one tick per call, so after_tx_ms must
  // be strictly positive by the time the reply is delivered.
  radio.queue_rx_silence(1);
  radio.queue_rx(to_rx_packet(build_spe_response(test::DST_ID, test::OWN_ID)));

  IoFrame request = build_spe_request(test::OWN_ID);
  CollectedReplies collected;
  uint8_t count = engine.collect_broadcast_responses(request, FREQ_CH2, CMD_DISCOVER_SPE_RESP,
                                                     tuning.pairing_discovery_wait_ms, collected.handler());

  ASSERT_EQ(count, 1u);
  ASSERT_EQ(collected.after_tx_ms.size(), 1u);
  EXPECT_GT(collected.after_tx_ms[0], 0u) << "latency must be measured from the transmit completing, not read as 0";
}

// Pins a property no existing test asserts directly: staying put after a reception is
// deliberate — the hub is demonstrably on a channel this device population uses, and replies
// arrive spread across the whole window — not merely "the mock happened to still be on the right
// channel". call_log() (not freq_history()) is what can show this: it is the interleaved
// wait/hop call order, so it can distinguish "no hop between this reception and the next slice"
// from "a hop happened and the rotation later landed back here".
TEST(Exchange, CollectBroadcastResponses_DoesNotHopAfterAReception) {
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  tuning.pairing_discovery_wait_ms = 20;
  // collect_broadcast_responses() leaves spec.dwell_ms at 0, so listen() asks MockRadio's
  // hop_dwell_ms() (which reads this field) — force several slices within the window.
  tuning.sx1276_discovery_hop_slice_ms = 1;
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  // One matching reply lands on the very first listen after the pre-loop hop, then silence for
  // the rest of the window.
  radio.queue_rx(to_rx_packet(build_spe_response(test::DST_ID, test::OWN_ID)));
  radio.queue_rx_silence(5);

  IoFrame request = build_spe_request(test::OWN_ID);
  CollectedReplies collected;
  engine.collect_broadcast_responses(request, FREQ_CH2, CMD_DISCOVER_SPE_RESP, tuning.pairing_discovery_wait_ms,
                                     collected.handler());

  ASSERT_EQ(collected.frames.size(), 1u) << "the reply must have been collected for this test to say anything";
  ASSERT_GE(radio.call_log().size(), 3u) << "need the reception plus at least one following slice";
  EXPECT_EQ(radio.call_log()[0], MockRadio::CallKind::kHop) << "exactly one hop precedes the first listen";
  EXPECT_EQ(radio.call_log()[1], MockRadio::CallKind::kWait) << "the first listen is the one that receives the reply";
  EXPECT_EQ(radio.call_log()[2], MockRadio::CallKind::kWait)
      << "no hop between the reception and the next slice: the roll-call stays on a channel that just answered";
}

// ============================================================================
// hop_frequency(): the bare CH1→CH2→CH3→CH1 rotation, asserted directly so the skip_freq default
// argument (used above) can be trusted to reproduce it exactly.
// ============================================================================

TEST(Exchange, HopFrequencyWithoutSkipIsUnchanged) {
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  radio.change_frequency(FREQ_CH1);
  engine.hop_frequency();
  EXPECT_EQ(radio.get_current_freq(), FREQ_CH2);

  engine.hop_frequency();
  EXPECT_EQ(radio.get_current_freq(), FREQ_CH3);

  engine.hop_frequency();
  EXPECT_EQ(radio.get_current_freq(), FREQ_CH1);
}

// ============================================================================
// maybe_hop(): the idle-path hop. maybe_hop() is time-gated; RadioDriver::reception_in_progress()
// additionally gates it so a hop cannot retune under an arriving frame and destroy it via
// change_frequency()'s IRQ/DIO-latch clear (issue #81).
// ============================================================================

TEST(Exchange, MaybeHopDoesNotHopWhileTheDwellHasNotYetElapsed) {
  // Boundary case for MaybeHopHopsOnceTheDwellElapses below: exchange_engine.cpp's maybe_hop()
  // compares with <=, so exactly HOP_TIME_US elapsed must not be enough on its own.
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  engine.reset_hop_timestamp();
  esphome::test_clock::advance_us(HOP_TIME_US);
  engine.maybe_hop();

  EXPECT_TRUE(radio.freq_history().empty()) << "exactly HOP_TIME_US elapsed must not be past the dwell yet";
}

TEST(Exchange, MaybeHopHopsOnceTheDwellElapses) {
  // Baseline: pins that the reception_in_progress() guard did not break the ordinary path.
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  engine.reset_hop_timestamp();
  esphome::test_clock::advance_us(HOP_TIME_US + 1);
  engine.maybe_hop();

  EXPECT_EQ(radio.freq_history().size(), 1u) << "the dwell elapsed and nothing was arriving, so the hop must fire";
}

TEST(Exchange, MaybeHopDoesNotHopWhileAFrameIsArriving) {
  // The core assertion of the whole step: a reception in progress must outrank the dwell timer.
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  engine.reset_hop_timestamp();
  esphome::test_clock::advance_us(HOP_TIME_US + 1);
  radio.note_reception_from_test();
  engine.maybe_hop();

  EXPECT_TRUE(radio.freq_history().empty()) << "a frame arriving on this channel must suppress the hop entirely";
}

TEST(Exchange, MaybeHopHopsAsSoonAsTheReceptionClears) {
  // Pins both that the holdoff expires on its own and that the deferred hop is not lost — it
  // happens on the very next call, not never.
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  engine.reset_hop_timestamp();
  esphome::test_clock::advance_us(HOP_TIME_US + 1);
  radio.note_reception_from_test();
  engine.maybe_hop();
  ASSERT_TRUE(radio.freq_history().empty()) << "sanity: the first call must still have been suppressed";

  esphome::test_clock::advance_us(RX_HOP_HOLDOFF_US + 1);
  engine.maybe_hop();

  EXPECT_EQ(radio.freq_history().size(), 1u) << "the holdoff expired, so the deferred hop must now fire";
}

TEST(Exchange, DeferredHopDoesNotRestartTheDwellTimer) {
  // Pins the "last_hop_us_ deliberately left alone" decision (exchange_engine.cpp maybe_hop()).
  // A wrong implementation that stamped last_hop_us_ = micros() on the suppressed call too (e.g.
  // by moving the update above the reception check) would still pass
  // MaybeHopHopsAsSoonAsTheReceptionClears above, because RX_HOP_HOLDOFF_US already exceeds
  // HOP_TIME_US on its own. Clearing the holdoff directly rather than waiting out its own timer
  // isolates the property: once the dwell has genuinely elapsed, a single further microsecond
  // must be enough to hop.
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  engine.reset_hop_timestamp();
  esphome::test_clock::advance_us(HOP_TIME_US + 1);
  radio.note_reception_from_test();
  engine.maybe_hop();
  ASSERT_TRUE(radio.freq_history().empty()) << "sanity: the first call must still have been suppressed";

  radio.clear_reception_from_test();
  esphome::test_clock::advance_us(1);
  engine.maybe_hop();

  EXPECT_EQ(radio.freq_history().size(), 1u)
      << "the dwell had already elapsed before the suppressed call; one more microsecond after the "
         "reception clears must be enough to hop, proving last_hop_us_ was not touched while the "
         "hop was held off";
}

// ============================================================================
// Response windows: a start frame wakes a sleeping device, so it gets the *longer* budget.
//
// These were fixed constants, and RESPONSE_START_WAIT_MS (300 ms) was shorter than
// RESPONSE_WAIT_MS (500 ms) despite its own comment promising "longer" — backwards for the one
// case where a low-power target may have been asleep until the 213 ms wake-up preamble reached it.
// Field captures of a solar RS100 measured replies from 29 ms to 3052 ms on the same device; see
// RESPONSE_START_WAIT_MS.
// ============================================================================

namespace {

// The host clock stubs advance millis() one unit per call, and the engine reads it a couple of
// times between stamping the deadline and slicing it, so the first slice lands a tick or two under
// the configured window. Assert the window, not the exact tick. Still needed for legacy-mode
// callers (e.g. Listen_HoldNeverRetunesAndUsesTheWholeWindowAsOneTimeout below); a ManualClock
// caller can assert the exact value instead -- see StartFrameBudgetsTheStartResponseWindow.
void expect_window_near(uint32_t actual, uint32_t expected) {
  EXPECT_LE(actual, expected) << "slice must never exceed the configured window";
  EXPECT_GE(actual + 10u, expected) << "slice must be the configured window, not a different budget";
}

}  // namespace

TEST(Exchange, StartFrameBudgetsTheStartResponseWindow) {
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
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
  EXPECT_EQ(radio.wait_timeouts().front(), 1750u) << "the first slice must be exactly the configured window";
}

TEST(Exchange, ContinuationFrameBudgetsTheShorterResponseWindow) {
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
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
  EXPECT_EQ(radio.wait_timeouts().front(), 250u) << "the first slice must be exactly the configured window";
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
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;

  TuningConfig tuning;
  // A try starts only if it ends inside the budget: elapsed + retry delay (250) + the try's air time
  // (a mains START frame's short preamble, ~16 ms) + its overhead and window (250). Try 2's check runs
  // at ~276 ms (try 1's end) and ends at ~802 < 1000; try 3's runs at ~802 and would end at ~1328 --
  // exactly two tries, in real milliseconds rather than relative to the window.
  tuning.exchange_start_response_wait_ms = 250;
  tuning.exchange_total_budget_ms = 1000;
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  IoFrame request{};
  create_execute_position(request, test::OWN_ID, test::DST_ID, /*low_power=*/false, 100);
  IoFrame response{};
  engine.send_and_receive(request, response, FREQ_CH2);  // nothing queued -> every try fails

  EXPECT_EQ(radio.get_send_count(), 2) << "a third try must not start once the budget is spent";
  EXPECT_STREQ(engine.get_debug().stage, "retry_budget_exhausted");
}

TEST(Exchange, ALowPowerTargetKeepsItsThreeTriesWhateverTheBudget) {
  // The same exchange to a low-power target: the budget would stop it after two, the attempt count
  // alone bounds it, so the third runs (ADR 0095).
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;

  TuningConfig tuning;
  tuning.exchange_start_response_wait_ms = 250;
  tuning.exchange_total_budget_ms = 1000;
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  IoFrame request{};
  create_execute_position(request, test::OWN_ID, test::DST_ID, /*low_power=*/true, 100);
  IoFrame response{};
  engine.send_and_receive(request, response, FREQ_CH2);

  EXPECT_EQ(radio.get_send_count(), EXCHANGE_RETRY_COUNT);
}

TEST(Exchange, GenerousBudgetStillAllowsEveryRetry) {
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;

  TuningConfig tuning;
  tuning.exchange_start_response_wait_ms = 200;
  tuning.exchange_total_budget_ms = 60000;  // far more than three tries plus their retry delays can spend
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  IoFrame request{};
  create_execute_position(request, test::OWN_ID, test::DST_ID, true, 100);
  IoFrame response{};
  engine.send_and_receive(request, response, FREQ_CH2);

  EXPECT_EQ(radio.get_send_count(), EXCHANGE_RETRY_COUNT) << "with budget to spare the retry count is unchanged";
}

// --- run_attempts(): the loop pairing's retries run through ------------------------------------

namespace {
constexpr uint8_t EXPLICIT_KEY[AES_KEY_SIZE] = {0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8,
                                                0xE9, 0xEA, 0xEB, 0xEC, 0xED, 0xEE, 0xEF, 0xF0};
constexpr uint8_t PROVIDER_KEY[AES_KEY_SIZE] = {0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8,
                                                0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF, 0xD0};
}  // namespace

TEST(ExchangeRunAttempts, RunsEveryTryWithTheGapBeforeEachFurtherOne) {
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning;
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  std::vector<uint32_t> starts;
  std::vector<uint8_t> numbers;
  const uint32_t begin_ms = esphome::test_clock::peek_ms();
  const AttemptRunResult result =
      engine.run_attempts(ExchangeEngine::fixed_attempts(3, 250, 500), [&](uint8_t number, uint16_t window_ms) {
        starts.push_back(esphome::test_clock::peek_ms() - begin_ms);
        numbers.push_back(number);
        EXPECT_EQ(window_ms, 500);
        return AttemptVerdict::AGAIN;
      });

  EXPECT_EQ(result, AttemptRunResult::TRIES_USED);
  EXPECT_EQ(numbers, (std::vector<uint8_t>{1, 2, 3}));
  EXPECT_EQ(starts, (std::vector<uint32_t>{0, 250, 500})) << "no gap before the first try, one before each other";
  EXPECT_EQ(engine.counters().retransmits, 2u) << "tries beyond the first count as retransmits";
}

TEST(ExchangeRunAttempts, DoneAndStopEndTheRunAtOnce) {
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning;
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  uint8_t calls = 0;
  EXPECT_EQ(engine.run_attempts(ExchangeEngine::fixed_attempts(3, 250),
                                [&](uint8_t number, uint16_t) {
                                  calls++;
                                  return number == 2 ? AttemptVerdict::DONE : AttemptVerdict::AGAIN;
                                }),
            AttemptRunResult::DONE);
  EXPECT_EQ(calls, 2);

  calls = 0;
  EXPECT_EQ(engine.run_attempts(ExchangeEngine::fixed_attempts(3, 250),
                                [&](uint8_t, uint16_t) {
                                  calls++;
                                  return AttemptVerdict::STOP;
                                }),
            AttemptRunResult::STOPPED);
  EXPECT_EQ(calls, 1);
}

TEST(ExchangeRunAttempts, ACountOfZeroRunsNoTry) {
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning;
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  bool called = false;
  EXPECT_EQ(engine.run_attempts(ExchangeEngine::fixed_attempts(0, 250),
                                [&](uint8_t, uint16_t) {
                                  called = true;
                                  return AttemptVerdict::DONE;
                                }),
            AttemptRunResult::TRIES_USED);
  EXPECT_FALSE(called);
}

TEST(ExchangeRunAttempts, ARunWithoutABudgetIgnoresTheExchangeBudget) {
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning;
  tuning.exchange_total_budget_ms = 100;  // far less than the gaps alone
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  uint8_t calls = 0;
  EXPECT_EQ(engine.run_attempts(ExchangeEngine::fixed_attempts(3, 250),
                                [&](uint8_t, uint16_t) {
                                  calls++;
                                  return AttemptVerdict::AGAIN;
                                }),
            AttemptRunResult::TRIES_USED);
  EXPECT_EQ(calls, 3);
}

TEST(ExchangeRunAttempts, ABudgetedRunStopsStartingTriesThatCannotFinish) {
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning;
  tuning.exchange_total_budget_ms = 1000;
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  // Try 1 spends its 200 ms window. Try 2 starts: 200 + 250 (gap) + 100 (air) + overhead + 200
  // ends before 1000, and the run is then at 650 ms. Try 3 would end at 650 + 250 + 100 + overhead
  // + 200 > 1000, so it is refused.
  ExchangeEngine::AttemptRun run = ExchangeEngine::fixed_attempts(9, 250, 200);
  run.budgeted_tx_ms = 100;
  uint8_t calls = 0;
  const AttemptRunResult result = engine.run_attempts(run, [&](uint8_t, uint16_t window_ms) {
    calls++;
    esphome::delay(window_ms);  // the try listens its whole window
    return AttemptVerdict::AGAIN;
  });

  EXPECT_EQ(result, AttemptRunResult::BUDGET_EXHAUSTED);
  EXPECT_EQ(calls, 2) << "the third try would end past the budget, so it is not started";
}

TEST(ExchangeRunAttempts, ABudgetedRunStillStartsItsFirstTryWhateverItCosts) {
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning;
  tuning.exchange_total_budget_ms = 1;
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  ExchangeEngine::AttemptRun run = ExchangeEngine::fixed_attempts(3, 250, 400);
  run.budgeted_tx_ms = 100;
  uint8_t calls = 0;
  EXPECT_EQ(engine.run_attempts(run,
                                [&](uint8_t, uint16_t) {
                                  calls++;
                                  return AttemptVerdict::AGAIN;
                                }),
            AttemptRunResult::BUDGET_EXHAUSTED);
  EXPECT_EQ(calls, 1);
}

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

// --- A try's transmission counts against the budget -------------------------------------------
// A try starts only if its transmission ends inside the budget (ExchangeEngine::try_fits_budget_()),
// with the transmit time taken from the driver's tx_air_time_us(). Every case models air time on a
// ManualClock, so the clock the budget is checked against includes it.

namespace {

/// A radio whose wake-up is a 500 ms transmission for a LONG start frame, e.g. one that wakes
/// receivers with a train of frames instead of a long preamble.
class SlowWakeRadio : public MockRadio {
 public:
  static constexpr uint32_t LONG_WAKE_US = 500000;
  uint32_t tx_air_time_us(uint8_t len, const RadioTxConfig &cfg) const override {
    return cfg.wake == TxWake::LONG ? LONG_WAKE_US : MockRadio::tx_air_time_us(len, cfg);
  }
};

/// Queue a challenge for every try of a status poll: each try is challenged and then never closed.
void queue_challenges(MockRadio &radio, int tries) {
  const uint8_t chal[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
  const IoFrame challenge = build_challenge(test::DST_ID, test::OWN_ID, chal);
  uint8_t raw[64];
  RadioRxPacket pkt{};
  pkt.len = serialize(challenge, raw, sizeof(raw));
  memcpy(pkt.data, raw, pkt.len);
  for (int i = 0; i < tries; i++) {
    // Try N's request is send 2N-1 (request, 0x3D per earlier try): hold until it is out.
    radio.queue_rx_hold_until_sent(2 * i + 1);
    radio.queue_rx(pkt);
  }
}

/// Run a status poll that is challenged and never closed on every try, with @p budget_ms.
/// @return The radio's send count.
int run_challenged_status_poll(uint16_t budget_ms, std::vector<uint32_t> *send_times = nullptr,
                               const char **stage = nullptr) {
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
  radio.set_model_tx_airtime(true);
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning;
  tuning.exchange_total_budget_ms = budget_ms;
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);
  queue_challenges(radio, EXCHANGE_RETRY_COUNT);

  IoFrame request{};
  create_get_status(request, test::OWN_ID, test::DST_ID, /*low_power=*/false);
  IoFrame response{};
  EXPECT_EQ(engine.send_and_receive(request, response, FREQ_CH2), ExchangeOutcome::SUCCESS_UNCONFIRMED);
  if (send_times != nullptr)
    *send_times = radio.send_times_ms();
  if (stage != nullptr)
    *stage = engine.get_debug().stage;
  return radio.get_send_count();
}

}  // namespace

TEST(Exchange, ThreeUnansweredSlowWakeTriesAllRunPastTheBudget) {
  esphome::test_clock::ManualClock clock;
  SlowWakeRadio radio;
  radio.set_model_tx_airtime(true);
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning;  // defaults: 400 ms start window, 250 ms retry gap, 2500 ms budget
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  IoFrame request{};
  create_execute_position(request, test::OWN_ID, test::DST_ID, /*low_power=*/true, 100);
  IoFrame response{};
  const uint32_t start = esphome::test_clock::peek_ms();
  EXPECT_EQ(engine.send_and_receive(request, response, FREQ_CH2), ExchangeOutcome::FAILED);
  const uint32_t blocked_ms = esphome::test_clock::peek_ms() - start;

  // Three tries of a 500 ms wake-up, a 400 ms window and the 250 ms gap end at 3 x 910 + 2 x 250 =
  // 3230 ms, past the 2500 ms budget: a low-power target keeps its three tries, bounded by their count
  // (ADR 0095). The exchange runs no longer than they need.
  EXPECT_EQ(radio.get_send_count(), 3);
  EXPECT_STREQ(engine.get_debug().stage, "wait_first_timeout");
  EXPECT_GT(blocked_ms, tuning.exchange_total_budget_ms);
  EXPECT_LT(blocked_ms, 3230u);
  for (const auto &cfg : radio.get_tx_configs())
    EXPECT_EQ(cfg.wake, TxWake::LONG);
}

TEST(Exchange, SettlingReadRunsThePlanTheGuardAgreesWith) {
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
  radio.set_model_tx_airtime(true);
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning;
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);
  IoFrame request{};
  create_get_status(request, test::OWN_ID, test::DST_ID, /*low_power=*/false);

  const decisions::AttemptPlan plan = expected_plan(engine, tuning, decisions::ReadKind::SETTLING, request);

  EXPECT_EQ(plan.gap_ms, 0u);
  // At the defaults one attempt is the short start frame on air (~10 ms) plus its overhead and the
  // 200 ms early window, and the last one listens the whole 400 ms: nine fit below the 2500 ms
  // budget (ADR 0075).
  EXPECT_EQ(plan.max_attempts, EXCHANGE_MAX_ATTEMPTS) << "a mains settling read makes nine attempts at the defaults";
  // A silent target gets exactly the planned attempts: the plan equals what the guard admits.
  IoFrame response{};
  const uint32_t start = esphome::test_clock::peek_ms();
  EXPECT_EQ(engine.send_and_receive(request, response, FREQ_CH2, read_of(decisions::ReadKind::SETTLING)),
            ExchangeOutcome::FAILED);
  EXPECT_EQ(radio.get_send_count(), plan.max_attempts);
  EXPECT_LT(esphome::test_clock::peek_ms() - start, tuning.exchange_total_budget_ms);
}

TEST(Exchange, SettlingReadAgreesWithTheGuardWhenTheBudgetLimitsTheAttempts) {
  // At a 1000 ms budget the plan is 3 (1 + (999 - 422) / 222) and a silent target gets exactly those.
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
  radio.set_model_tx_airtime(true);
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning;
  tuning.exchange_total_budget_ms = 1000;
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);
  IoFrame request{};
  create_get_status(request, test::OWN_ID, test::DST_ID, /*low_power=*/false);

  const decisions::AttemptPlan plan = expected_plan(engine, tuning, decisions::ReadKind::SETTLING, request);
  IoFrame response{};
  EXPECT_EQ(engine.send_and_receive(request, response, FREQ_CH2,
                                    decisions::AttemptRequest::of_kind(decisions::ReadKind::SETTLING)),
            ExchangeOutcome::FAILED);

  EXPECT_EQ(plan.max_attempts, 3u);
  EXPECT_EQ(radio.get_send_count(), plan.max_attempts) << "the plan promises what the guard admits, no more, no fewer";
}

TEST(Exchange, SettlingReadOfASilentLowPowerTargetUsesTheLowPowerWindowAndThreeAttempts) {
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
  radio.set_model_tx_airtime(true);
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning;
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);
  IoFrame request{};
  create_get_status(request, test::OWN_ID, test::DST_ID, /*low_power=*/true);

  IoFrame response{};
  EXPECT_EQ(engine.send_and_receive(request, response, FREQ_CH2,
                                    decisions::AttemptRequest::of_kind(decisions::ReadKind::SETTLING)),
            ExchangeOutcome::FAILED);

  EXPECT_EQ(radio.get_send_count(), LOW_POWER_SETTLING_ATTEMPTS)
      << "wake-up preamble plus 550 ms window: three fit the default budget, the cap";
  ASSERT_EQ(radio.wait_timeouts().size(), static_cast<size_t>(LOW_POWER_SETTLING_ATTEMPTS));
  for (const auto wait : radio.wait_timeouts())
    EXPECT_EQ(wait, LOW_POWER_RESPONSE_WAIT_MS);
}

TEST(Exchange, ProgressLookToASilentTargetIsASingleAttempt) {
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning;
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);
  IoFrame request{};
  create_get_status(request, test::OWN_ID, test::DST_ID, /*low_power=*/false);

  IoFrame response{};
  EXPECT_EQ(engine.send_and_receive(request, response, FREQ_CH2,
                                    decisions::AttemptRequest::of_kind(decisions::ReadKind::PROGRESS)),
            ExchangeOutcome::FAILED);

  EXPECT_EQ(radio.get_send_count(), SCHEDULED_POLL_MAX_TRIES);
}

TEST(Exchange, ATryThatWouldEndAtTheBudgetIsNotStarted) {
  // Measure the third try's check time with budget to spare: its request goes out one retry gap
  // after the check.
  std::vector<uint32_t> t;
  ASSERT_EQ(run_challenged_status_poll(60000, &t), 2 * EXCHANGE_RETRY_COUNT);
  ASSERT_EQ(t.size(), 6u);
  const uint32_t third_check_ms = t[4] - EXCHANGE_RETRY_DELAY_MS - t[0];
  IoFrame request{};
  create_get_status(request, test::OWN_ID, test::DST_ID, false);
  uint8_t raw[64];
  const uint8_t len = serialize(request, raw, sizeof(raw));
  const uint32_t tx_ms = (io868_tx_air_time_us(mock_preamble_bytes(TuningConfig{}.start_preamble_ms), len,
                                               SOFT_PHY_PREAMBLE_BITS_PER_BYTE) +
                          999) /
                         1000;
  ASSERT_GT(tx_ms, 0u);

  // The elapsed time alone is well inside the budget, but the third try would end exactly at it:
  // its transmission and overhead, its first-reply window and, the target having challenged, its
  // final-reply window with the 0x3D. That try does not start.
  // The tail is what a challenged try adds: the final-reply window, the 0x3D's air time (the request
  // stands in for its size) and the overhead.
  const TuningConfig defaults;
  MockRadio probe;
  const uint32_t answer_tx_ms =
      (io868_tx_air_time_us(probe.response_preamble(), len, SOFT_PHY_PREAMBLE_BITS_PER_BYTE) + 999) / 1000;
  const uint32_t tail_ms = defaults.exchange_response_wait_ms + answer_tx_ms + EXCHANGE_ATTEMPT_OVERHEAD_MS;
  const auto edge =
      static_cast<uint16_t>(third_check_ms + EXCHANGE_RETRY_DELAY_MS + tx_ms + EXCHANGE_ATTEMPT_OVERHEAD_MS +
                            defaults.exchange_start_response_wait_ms + tail_ms);
  const char *stage = nullptr;
  EXPECT_EQ(run_challenged_status_poll(edge, nullptr, &stage), 4);
  EXPECT_STREQ(stage, "retry_budget_exhausted");
  // One millisecond more, and it ends inside the budget.
  EXPECT_EQ(run_challenged_status_poll(static_cast<uint16_t>(edge + 1)), 6);
}

TEST(Exchange, RetryCadenceMatchesTheRecordedWaitSliceForEachTryPlusTheFixedRetryDelay) {
  // Computed from the recorded slices rather than a hardcoded expected gap, so a future change to
  // RESPONSE_START_WAIT_MS or EXCHANGE_RETRY_DELAY_MS can't silently break this without the
  // assertion catching it. Note the per-slice half of that gap is partly self-referential --
  // MockRadio advances the manual clock by exactly wait_timeouts[i] on an empty slice, so that
  // arithmetic isn't independently verified here. What this test does add: exactly one wait per
  // try (not several smaller slices), and exactly one EXCHANGE_RETRY_DELAY_MS gap on top, with no
  // extra time unaccounted for.
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning;  // defaults: EXCHANGE_RETRY_COUNT tries, a total budget generous enough for all of them
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  IoFrame request{};
  create_execute_position(request, test::OWN_ID, test::DST_ID, true, 100);
  IoFrame response{};
  engine.send_and_receive(request, response, FREQ_CH2);  // nothing queued -> every try fails

  ASSERT_EQ(radio.get_send_count(), EXCHANGE_RETRY_COUNT)
      << "sanity: every try must have run for this to be meaningful";
  const auto &send_times = radio.send_times_ms();
  const auto &wait_timeouts = radio.wait_timeouts();
  ASSERT_EQ(send_times.size(), static_cast<size_t>(EXCHANGE_RETRY_COUNT));
  ASSERT_EQ(wait_timeouts.size(), static_cast<size_t>(EXCHANGE_RETRY_COUNT))
      << "HOLD_REQUEST_CHANNEL (wait_for_first_response_) issues exactly one wait per try";

  for (size_t i = 0; i + 1 < send_times.size(); i++) {
    EXPECT_EQ(send_times[i + 1] - send_times[i], wait_timeouts[i] + EXCHANGE_RETRY_DELAY_MS + CHANNEL_CHECK_TIME_MS)
        << "try " << i
        << "'s gap to the next send must be its own wait slice plus the fixed retry delay and the "
           "next request's channel check";
  }
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

// The radio only clears its capture when it begins a listen, so without an explicit reset a
// fully-silent exchange would inherit the *previous* exchange's capture and claim "we heard a
// frame". This is the exact misread that sent the issue #95 analysis down a wrong path.
TEST(Exchange, FailureReportDoesNotInheritThePreviousExchangesCapture) {
  MockRadio radio;
  radio.set_emulate_capture_lifecycle(true);
  RadioDriver *radio_ptr = &radio;

  TuningConfig tuning;
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  // Exchange 1: a direct status reply arrives and succeeds, leaving a populated radio capture.
  IoFrame reply = build_status_response(test::DST_ID, test::OWN_ID);
  uint8_t raw[64];
  RadioRxPacket pkt{};
  pkt.len = serialize(reply, raw, sizeof(raw));
  memcpy(pkt.data, raw, pkt.len);
  pkt.freq_hz = FREQ_CH2;
  radio.queue_rx(pkt);

  IoFrame request{};
  create_get_status(request, test::OWN_ID, test::DST_ID, /*low_power=*/false);
  IoFrame response{};
  ASSERT_EQ(engine.send_and_receive(request, response, FREQ_CH2), ExchangeOutcome::SUCCESS_WITH_RESPONSE);
  ASSERT_TRUE(engine.get_debug().capture_valid) << "sanity: exchange 1 heard a frame";

  // Exchange 2: nothing on air at all. Its report must describe *this* exchange. This runs the
  // realistic listen path (default budget, no queued TX failures) and only checks FAILED + capture
  // fields on purpose — asserting a try count here would be fragile against the host millis() stub
  // and the total budget (see CappedExchangeDoesProportionallyLessBlockingListening for that).
  IoFrame request2{};
  create_get_status(request2, test::OWN_ID, test::DST_ID, /*low_power=*/false);
  IoFrame response2{};
  ASSERT_EQ(engine.send_and_receive(request2, response2, FREQ_CH2), ExchangeOutcome::FAILED);

  EXPECT_FALSE(engine.get_debug().capture_valid)
      << "a fully-silent exchange must not report the previous exchange's radio capture";
  EXPECT_EQ(engine.get_debug().capture_frame_len, 0);
  EXPECT_EQ(engine.get_debug().capture_rssi_dbm, 0);
}

// ============================================================================
// max_tries: a scheduler-owned status poll caps its transmit attempts at SCHEDULED_POLL_MAX_TRIES
// so a dead device does not block loop() for the full EXCHANGE_RETRY_COUNT product. Everything
// else keeps the default.
// ============================================================================

TEST(Exchange, DebugLineRendersEveryFieldADiagnosisNeeds) {
  // The failure line and the accepted-without-reply line share this renderer, so one test covers
  // both. The fields are the whole point of the lines: a field report has to be able to tell
  // "the radio saw nothing" from "it received something unusable" without the reporter re-running.
  ExchangeEngine::DebugInfo d;
  d.stage = "success_auth_unconfirmed";
  d.tries = 2;
  d.max_tries = 3;
  d.request_cmd = CMD_EXECUTE;
  d.saw_challenge = true;
  d.capture_valid = true;
  d.capture_rx_done = false;
  d.capture_crc_error = true;
  d.capture_freq_hz = FREQ_CH2;
  d.capture_irq_status = 0x000C;
  d.capture_packet_status = 0x20;
  d.capture_reported_len = 34;
  d.capture_frame_len = 23;
  d.capture_rssi_dbm = -47;
  d.final_waits = 3;
  d.final_rx_ignored = 2;
  d.final_rx_failed = 1;
  d.final_rx_irq = 0x0084;

  char buf[EXCHANGE_DEBUG_LINE_SIZE];
  const int written = render_exchange_debug(buf, sizeof(buf), "DA88B6", d);

  ASSERT_GT(written, 0);
  EXPECT_LT(static_cast<size_t>(written), sizeof(buf))
      << "EXCHANGE_DEBUG_LINE_SIZE must hold the whole line: a truncated diagnostic loses exactly "
         "the trailing capture fields it exists to carry";
  const std::string line(buf);
  EXPECT_NE(line.find("device=DA88B6"), std::string::npos);
  EXPECT_NE(line.find("stage=success_auth_unconfirmed"), std::string::npos);
  EXPECT_NE(line.find("tries=2 max_tries=3"), std::string::npos);
  EXPECT_NE(line.find("saw_challenge=1"), std::string::npos);
  EXPECT_NE(line.find("cap_valid=1 cap_rx_done=0 cap_crc_err=1"), std::string::npos)
      << "the three capture flags are what separates a lost challenge answer from a lost reply";
  EXPECT_NE(line.find("cap_irq=0x000C"), std::string::npos);
  EXPECT_NE(line.find("cap_reported_len=34 cap_frame_len=23"), std::string::npos);
  EXPECT_NE(line.find("cap_rssi=-47"), std::string::npos);
  EXPECT_NE(line.find("final_waits=3 final_rx_ignored=2 final_rx_failed=1 final_rx_irq=0x0084"), std::string::npos)
      << "the final_rx_* fields are what separates a reply lost on this side from one never sent";
}

// --- Final-wait reception counters -------------------------------------------
// The cap_* fields keep the first informative reception, which in an authenticated exchange is the
// device's challenge, and every re-arm inside the final wait clears the capture. So what the final
// wait heard is counted per event instead. A ManualClock makes a silent wait expire at its deadline
// the way a real radio's does; under the legacy +1 ms clock every empty-queue return would look
// like an early failed reception.

namespace {
/// Queue a valid 0x3C challenge from the device to the hub.
void queue_device_challenge(MockRadio &radio) {
  const uint8_t chal_data[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
  IoFrame challenge = build_challenge(test::DST_ID, test::OWN_ID, chal_data);
  uint8_t raw[64];
  RadioRxPacket pkt{};
  pkt.len = serialize(challenge, raw, sizeof(raw));
  memcpy(pkt.data, raw, pkt.len);
  radio.queue_rx(pkt);
}
}  // namespace

TEST(Exchange, FinalWaitCountsFailedAndIgnoredReceptions) {
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
  radio.set_emulate_capture_lifecycle(true);  // real drivers clear and refill the capture per wait
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning;
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  IoFrame request{};
  create_execute_position(request, test::OWN_ID, test::DST_ID, false, 100);

  queue_device_challenge(radio);
  // During the final wait: one reception that fails to decode, one frame from another device.
  radio.queue_rx_failed_reception(0x0084);
  const uint8_t other_device[3] = {0x11, 0x22, 0x33};
  IoFrame unrelated = build_status_response(other_device, test::OWN_ID);
  uint8_t raw[64];
  RadioRxPacket unrelated_pkt{};
  unrelated_pkt.len = serialize(unrelated, raw, sizeof(raw));
  memcpy(unrelated_pkt.data, raw, unrelated_pkt.len);
  radio.queue_rx(unrelated_pkt);

  IoFrame response{};
  ASSERT_EQ(engine.send_and_receive(request, response, FREQ_CH2), ExchangeOutcome::SUCCESS_UNCONFIRMED);

  const auto &d = engine.get_debug();
  EXPECT_EQ(d.final_waits, 1u);
  EXPECT_EQ(d.final_rx_failed, 1u) << "the failed reception must be counted before the re-arm clears it";
  EXPECT_EQ(d.final_rx_irq, 0x0084u);
  EXPECT_EQ(d.final_rx_ignored, 1u) << "the other device's frame arrived during the wait and was not the reply";
  EXPECT_EQ(d.capture_frame_len, 15u) << "cap_* still describes the challenge, which is why final_rx_* exists";
}

TEST(Exchange, SilentFinalWaitCountsNothing) {
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning;
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  IoFrame request{};
  create_execute_position(request, test::OWN_ID, test::DST_ID, false, 100);
  queue_device_challenge(radio);  // then nothing: the device never closes the exchange

  IoFrame response{};
  ASSERT_EQ(engine.send_and_receive(request, response, FREQ_CH2), ExchangeOutcome::SUCCESS_UNCONFIRMED);

  const auto &d = engine.get_debug();
  EXPECT_EQ(d.final_waits, 1u);
  EXPECT_EQ(d.final_rx_failed, 0u) << "a wait that simply expires is silence, not a failed reception";
  EXPECT_EQ(d.final_rx_ignored, 0u);
}

TEST(Exchange, FirstResponseWaitDoesNotFeedTheFinalCounters) {
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning;
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  IoFrame request{};
  create_get_status(request, test::OWN_ID, test::DST_ID, /*low_power=*/false);
  radio.queue_rx_failed_reception(0x0084);  // lands in the first-response wait; nothing answers

  IoFrame response{};
  engine.send_and_receive(request, response, FREQ_CH2, /*max_tries=*/1);

  const auto &d = engine.get_debug();
  EXPECT_EQ(d.final_waits, 0u) << "no challenge, so no final wait ever ran";
  EXPECT_EQ(d.final_rx_failed, 0u);
}

TEST(Exchange, UnconfirmedAcceptKeepsTheFinalWaitCaptureForTheLogLine) {
  // An EXECUTE that draws a challenge and then silence ends as SUCCESS_UNCONFIRMED, which prints
  // the accepted-without-reply line rather than a failure. That line is only worth printing if the
  // snapshot behind it still describes the final wait, so assert the snapshot, which is what the
  // host log macros cannot show.
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
  const uint8_t len_chal = serialize(challenge, raw_chal, sizeof(raw_chal));
  RadioRxPacket chal_pkt{};
  chal_pkt.len = len_chal;
  memcpy(chal_pkt.data, raw_chal, len_chal);
  radio.queue_rx(chal_pkt);
  // Nothing queued for the final wait.

  IoFrame response{};
  ASSERT_EQ(comp.send_and_receive_(request, response, FREQ_CH2), ExchangeOutcome::SUCCESS_UNCONFIRMED);

  const auto &d = comp.exchange_engine_.get_debug();
  EXPECT_STREQ(d.stage, "success_auth_unconfirmed");
  EXPECT_TRUE(d.saw_challenge) << "the challenge is the reason this counts as accepted at all";
  EXPECT_EQ(d.request_cmd, CMD_EXECUTE);

  char buf[EXCHANGE_DEBUG_LINE_SIZE];
  const int written = render_exchange_debug(buf, sizeof(buf), "DEADBE", d);
  EXPECT_LT(static_cast<size_t>(written), sizeof(buf));
  EXPECT_NE(std::string(buf).find("stage=success_auth_unconfirmed"), std::string::npos);
}

TEST(Exchange, MaxTriesOfOneTransmitsExactlyOnce) {
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning;
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  for (int i = 0; i < EXCHANGE_RETRY_COUNT; ++i)
    radio.queue_tx_result(false);

  IoFrame request{};
  create_get_status(request, test::OWN_ID, test::DST_ID, /*low_power=*/false);
  IoFrame response{};
  engine.send_and_receive(request, response, FREQ_CH2, /*max_tries=*/1);

  EXPECT_EQ(radio.get_send_count(), 1) << "max_tries=1 must transmit once, not EXCHANGE_RETRY_COUNT times";
  EXPECT_EQ(engine.get_debug().tries, 1u);
  EXPECT_EQ(engine.get_debug().max_tries, 1u) << "the budgeted cap is surfaced for the failure log";
}

TEST(Exchange, MaxTriesDefaultsToTheFullRetryCount) {
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning;
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  for (int i = 0; i < EXCHANGE_RETRY_COUNT; ++i)
    radio.queue_tx_result(false);

  IoFrame request{};
  create_get_status(request, test::OWN_ID, test::DST_ID, /*low_power=*/false);
  IoFrame response{};
  engine.send_and_receive(request, response, FREQ_CH2);  // no max_tries argument

  EXPECT_EQ(radio.get_send_count(), EXCHANGE_RETRY_COUNT) << "the default is still the full retry count";
  EXPECT_EQ(engine.get_debug().max_tries, EXCHANGE_RETRY_COUNT);
}

TEST(Exchange, MaxTriesIsClampedToTheValidRange) {
  {  // 0 must not mean "transmit nothing".
    MockRadio radio;
    RadioDriver *radio_ptr = &radio;
    TuningConfig tuning;
    ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);
    for (int i = 0; i < EXCHANGE_RETRY_COUNT; ++i)
      radio.queue_tx_result(false);
    IoFrame request{};
    create_get_status(request, test::OWN_ID, test::DST_ID, /*low_power=*/false);
    IoFrame response{};
    engine.send_and_receive(request, response, FREQ_CH2, /*max_tries=*/0);
    EXPECT_EQ(radio.get_send_count(), 1) << "max_tries=0 clamps up to one transmit";
  }
  {  // A too-large value must not exceed the budgeted ceiling.
    MockRadio radio;
    RadioDriver *radio_ptr = &radio;
    TuningConfig tuning;
    ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);
    for (int i = 0; i < EXCHANGE_RETRY_COUNT; ++i)
      radio.queue_tx_result(false);
    IoFrame request{};
    create_get_status(request, test::OWN_ID, test::DST_ID, /*low_power=*/false);
    IoFrame response{};
    engine.send_and_receive(request, response, FREQ_CH2, /*max_tries=*/99);
    EXPECT_EQ(radio.get_send_count(), EXCHANGE_RETRY_COUNT) << "max_tries clamps down to EXCHANGE_RETRY_COUNT";
  }
}

// The tests above pin the outer loop bound via TX failures, which never reach a listen. This one
// pins what the change is actually for: a capped exchange spends proportionally less time in the
// blocking first-response wait. Realistic path (TX succeeds, nothing replies) with the budget
// lifted and the window tiny so the host millis() stub can't trip the budget on its own. The
// first-response wait slices internally, so the count per try is an implementation detail — assert
// the ratio, which is exact because each try re-runs the identical listen.
TEST(Exchange, CappedExchangeDoesProportionallyLessBlockingListening) {
  TuningConfig tuning;
  tuning.exchange_start_response_wait_ms = 50;
  tuning.exchange_total_budget_ms = 60000;

  auto listen_slices_for = [&](uint8_t max_tries) {
    MockRadio radio;
    RadioDriver *radio_ptr = &radio;
    ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);
    IoFrame request{};
    create_get_status(request, test::OWN_ID, test::DST_ID, /*low_power=*/false);
    IoFrame response{};
    engine.send_and_receive(request, response, FREQ_CH2, max_tries);
    return std::make_pair(radio.get_send_count(), radio.wait_timeouts().size());
  };

  const auto one = listen_slices_for(1);
  const auto full = listen_slices_for(EXCHANGE_RETRY_COUNT);

  EXPECT_EQ(one.first, 1) << "max_tries=1 transmits once";
  EXPECT_EQ(full.first, EXCHANGE_RETRY_COUNT);
  EXPECT_GE(one.second, 1u) << "a single try still performs a blocking listen (not just a TX)";
  EXPECT_EQ(full.second, one.second * EXCHANGE_RETRY_COUNT)
      << "the default blocks in the first-response wait EXCHANGE_RETRY_COUNT times as long";
}

// ============================================================================
// ExchangeEngine::listen() — the shared listen primitive, exercised directly through MockRadio.
// Not yet called by any of the six wait loops (that porting happens loop by loop in later steps);
// these tests pin the primitive's own behaviour so a later port can be bisected from it.
// ============================================================================

namespace {

/// Radio double that reports a controllable, constant preamble/sync state — used to test
/// linger_on_preamble without depending on a real chip's IRQ timing.
class SignalDetectRadio : public MockRadio {
 public:
  bool is_preamble_detected() override { return preamble_detected_; }
  void set_preamble_detected(bool v) { preamble_detected_ = v; }

 private:
  bool preamble_detected_{false};
};

}  // namespace

// Pins the roll-call's own call to spec.linger_on_preamble / spec.linger_dwell_ms in
// collect_broadcast_responses() -- not just the listen() primitive those fields feed. A hardware
// capture found the two unguarded: SX1276's 5 ms per-channel dwell is shorter than a
// DISCOVER_SPE_RESP's ~10 ms air time, so the radio retuned mid-frame on nearly every reception
// and the measured found-rate collapsed from 83% to 12%; hardware-measured 2026-08-19 confirmed
// 100% once the guard was added. If either field is dropped from that call site, this test must
// fail even though CollectBroadcastResponses_DoesNotHopAfterAReception and
// Listen_LingerOnPreambleUsesLingerDwellAndSuppressesHopping both still pass.
TEST(Exchange, CollectBroadcastResponses_LingersOnPreambleInsteadOfHoppingMidFrame) {
  constexpr uint32_t WINDOW_MS = 150;

  SignalDetectRadio radio;
  radio.set_preamble_detected(true);  // asserted throughout; nothing is ever received
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  // Matches the hardware SX1276 dwell that exposed the bug: 5 ms dwell vs ~10 ms reply air time.
  tuning.sx1276_discovery_hop_slice_ms = 5;
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  IoFrame request = build_spe_request(test::OWN_ID);
  CollectedReplies collected;
  uint8_t count =
      engine.collect_broadcast_responses(request, FREQ_CH2, CMD_DISCOVER_SPE_RESP, WINDOW_MS, collected.handler());

  EXPECT_EQ(count, 0u) << "nothing was ever queued; this test only cares about the wait/hop cadence";

  // Exactly one hop precedes the whole listen -- leaving the request channel, per
  // CollectBroadcastResponses_LeavesRequestChannelImmediately. With the dwell (5 ms) shorter than
  // a reply's air time, an unguarded rotation would then hop roughly every 5 ms for the rest of
  // the 150 ms window (dozens of hops); the preamble/sync guard must suppress every one of them.
  EXPECT_EQ(radio.freq_history().size(), 1u)
      << "asserted preamble/sync must suppress hopping for the whole window, not just the pre-loop skip";

  // wait_timeouts() must alternate the per-channel dwell with a PREAMBLE_LINGER_DWELL_MS-sized
  // linger extension, mirroring Listen_LingerOnPreambleUsesLingerDwellAndSuppressesHopping's
  // canonical-pairs check but exercised through the roll-call call site instead of listen()
  // directly.
  const auto &timeouts = radio.wait_timeouts();
  size_t canonical_pairs = 0;
  for (size_t i = 0; i + 1 < timeouts.size(); i += 2) {
    if (timeouts[i] != tuning.sx1276_discovery_hop_slice_ms || timeouts[i + 1] != PREAMBLE_LINGER_DWELL_MS)
      break;
    canonical_pairs++;
  }
  EXPECT_GE(canonical_pairs, 4u) << "need several unclamped dwell/linger pairs to prove the alternation";
}

TEST(Exchange, Listen_HoldNeverRetunesAndUsesTheWholeWindowAsOneTimeout) {
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  radio.queue_rx(to_rx_packet(build_spe_response(test::DST_ID, test::OWN_ID)));

  ListenSpec spec;
  spec.window_ms = 300;
  spec.policy = ListenPolicy::HOLD_REQUEST_CHANNEL;

  RadioRxPacket packet{};
  IoFrame frame{};
  auto outcome = engine.listen(spec, packet, frame,
                               [](const IoFrame *, const RadioRxPacket &) { return ReplyDisposition::ACCEPT; });

  EXPECT_EQ(outcome, ListenOutcome::ACCEPTED);
  EXPECT_TRUE(radio.freq_history().empty()) << "HOLD_REQUEST_CHANNEL must never retune";
  ASSERT_EQ(radio.wait_timeouts().size(), 1u) << "HOLD waits the whole remaining window in one call, not slices";
  expect_window_near(radio.wait_timeouts().front(), spec.window_ms);
}

TEST(Exchange, Listen_RotateAllChannelsVisitsAllThreeChannels) {
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  ListenSpec spec;
  spec.window_ms = 150;
  spec.policy = ListenPolicy::ROTATE_ALL_CHANNELS;
  spec.dwell_ms = 5;

  RadioRxPacket packet{};
  IoFrame frame{};
  // Nothing queued: every slice times out, so the loop hops for the whole window.
  auto outcome = engine.listen(spec, packet, frame,
                               [](const IoFrame *, const RadioRxPacket &) { return ReplyDisposition::IGNORE; });

  EXPECT_EQ(outcome, ListenOutcome::TIMED_OUT);
  const auto &history = radio.freq_history();
  EXPECT_NE(std::find(history.begin(), history.end(), FREQ_CH1), history.end());
  EXPECT_NE(std::find(history.begin(), history.end(), FREQ_CH2), history.end());
  EXPECT_NE(std::find(history.begin(), history.end(), FREQ_CH3), history.end());
}

TEST(Exchange, Listen_RotateSkippingRequestHopsFirstAndNeverLandsOnRequestChannel) {
  const uint32_t request_channels[] = {FREQ_CH1, FREQ_CH2, FREQ_CH3};
  for (uint32_t request_freq : request_channels) {
    MockRadio radio;
    RadioDriver *radio_ptr = &radio;
    TuningConfig tuning = make_broadcast_test_tuning();
    ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

    ListenSpec spec;
    spec.window_ms = 20;
    spec.policy = ListenPolicy::ROTATE_SKIPPING_REQUEST;
    spec.request_freq = request_freq;
    spec.dwell_ms = 1;  // force several iterations within the window
    spec.hop_after_ignored_frame = false;

    RadioRxPacket packet{};
    IoFrame frame{};
    engine.listen(spec, packet, frame, [](const IoFrame *, const RadioRxPacket &) { return ReplyDisposition::IGNORE; });

    ASSERT_FALSE(radio.call_log().empty());
    EXPECT_EQ(radio.call_log().front(), MockRadio::CallKind::kHop)
        << "request_freq=" << request_freq << ": must retune before the first listen";
    for (uint32_t visited : radio.freq_history()) {
      EXPECT_NE(visited, request_freq) << "request_freq=" << request_freq << ": must never land on it";
    }
  }
}

TEST(Exchange, Listen_AcceptStopsImmediately) {
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  radio.queue_rx(to_rx_packet(build_spe_response(test::DST_ID, test::OWN_ID)));
  radio.queue_rx(to_rx_packet(build_spe_response(test::DST_ID, test::OWN_ID)));  // must never be consumed

  ListenSpec spec;
  spec.window_ms = 300;
  spec.policy = ListenPolicy::HOLD_REQUEST_CHANNEL;

  int calls = 0;
  RadioRxPacket packet{};
  IoFrame frame{};
  auto outcome = engine.listen(spec, packet, frame, [&](const IoFrame *, const RadioRxPacket &) {
    calls++;
    return ReplyDisposition::ACCEPT;
  });

  EXPECT_EQ(outcome, ListenOutcome::ACCEPTED);
  EXPECT_EQ(calls, 1) << "ACCEPT on the first packet must stop the listen before the second is ever read";
  EXPECT_EQ(frame.cmd, CMD_DISCOVER_SPE_RESP) << "the caller's frame must hold the accepted frame";
}

TEST(Exchange, Listen_AbortStopsImmediatelyAndIsDistinguishableFromAccept) {
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  radio.queue_rx(to_rx_packet(build_spe_response(test::DST_ID, test::OWN_ID)));

  ListenSpec spec;
  spec.window_ms = 300;
  spec.policy = ListenPolicy::HOLD_REQUEST_CHANNEL;

  RadioRxPacket packet{};
  IoFrame frame{};
  auto outcome = engine.listen(spec, packet, frame,
                               [](const IoFrame *, const RadioRxPacket &) { return ReplyDisposition::ABORT; });

  EXPECT_EQ(outcome, ListenOutcome::ABORTED);
  EXPECT_NE(outcome, ListenOutcome::ACCEPTED);
}

TEST(Exchange, Listen_IgnoreRunsToDeadline) {
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  radio.queue_rx(to_rx_packet(build_spe_response(test::DST_ID, test::OWN_ID)));

  ListenSpec spec;
  spec.window_ms = 300;
  spec.policy = ListenPolicy::HOLD_REQUEST_CHANNEL;

  int calls = 0;
  RadioRxPacket packet{};
  IoFrame frame{};
  auto outcome = engine.listen(spec, packet, frame, [&](const IoFrame *, const RadioRxPacket &) {
    calls++;
    return ReplyDisposition::IGNORE;
  });

  EXPECT_EQ(outcome, ListenOutcome::TIMED_OUT) << "an always-IGNORE handler must run the listen to the deadline";
  EXPECT_EQ(calls, 1) << "the one queued packet was still handed to the handler on the way to timing out";
}

TEST(Exchange, Listen_UnparsablePacketReachesHandlerWithNullParsed) {
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  // Shorter than FRAME_MIN_SIZE: parse() must reject it outright.
  RadioRxPacket garbage{};
  garbage.len = 4;
  radio.queue_rx(garbage);

  ListenSpec spec;
  spec.window_ms = 300;
  spec.policy = ListenPolicy::HOLD_REQUEST_CHANNEL;

  bool saw_null_parsed = false;
  RadioRxPacket packet{};
  IoFrame frame{};
  auto outcome = engine.listen(spec, packet, frame, [&](const IoFrame *parsed, const RadioRxPacket &pkt) {
    saw_null_parsed = parsed == nullptr;
    EXPECT_EQ(pkt.len, 4u) << "the raw packet must still reach the handler even though it didn't parse";
    return ReplyDisposition::ACCEPT;
  });

  EXPECT_EQ(outcome, ListenOutcome::ACCEPTED);
  EXPECT_TRUE(saw_null_parsed) << "a packet that fails parse() must reach the handler with parsed == nullptr";
}

TEST(Exchange, Listen_HopAfterIgnoredFrameFalseStaysPutAfterAReception) {
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  radio.queue_rx(to_rx_packet(build_spe_response(test::DST_ID, test::OWN_ID)));
  radio.queue_rx_silence(5);

  ListenSpec spec;
  spec.window_ms = 20;
  spec.policy = ListenPolicy::ROTATE_ALL_CHANNELS;
  spec.dwell_ms = 1;
  spec.hop_after_ignored_frame = false;

  RadioRxPacket packet{};
  IoFrame frame{};
  engine.listen(spec, packet, frame, [](const IoFrame *, const RadioRxPacket &) { return ReplyDisposition::IGNORE; });

  ASSERT_GE(radio.call_log().size(), 2u) << "need the reception plus at least one following slice";
  EXPECT_EQ(radio.call_log()[0], MockRadio::CallKind::kWait) << "ROTATE_ALL_CHANNELS starts listening immediately";
  EXPECT_EQ(radio.call_log()[1], MockRadio::CallKind::kWait)
      << "hop_after_ignored_frame=false: no hop between the reception and the next slice";
}

TEST(Exchange, Listen_LingerOnPreambleUsesLingerDwellAndSuppressesHopping) {
  SignalDetectRadio radio;
  radio.set_preamble_detected(true);  // asserted throughout; nothing is ever received
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  ListenSpec spec;
  spec.window_ms = 150;
  spec.policy = ListenPolicy::ROTATE_ALL_CHANNELS;
  spec.dwell_ms = 5;
  spec.linger_on_preamble = true;
  spec.linger_dwell_ms = 15;

  RadioRxPacket packet{};
  IoFrame frame{};
  auto outcome = engine.listen(spec, packet, frame,
                               [](const IoFrame *, const RadioRxPacket &) { return ReplyDisposition::IGNORE; });

  EXPECT_EQ(outcome, ListenOutcome::TIMED_OUT);
  EXPECT_TRUE(radio.freq_history().empty()) << "the preamble guard must suppress every hop for the whole window";

  // Same clamped-tail caveat as the pairing-engine version of this property: walk pairs only as
  // long as both timeouts come through unclamped by the window's edge.
  const auto &timeouts = radio.wait_timeouts();
  size_t canonical_pairs = 0;
  for (size_t i = 0; i + 1 < timeouts.size(); i += 2) {
    if (timeouts[i] != spec.dwell_ms || timeouts[i + 1] != spec.linger_dwell_ms)
      break;
    canonical_pairs++;
  }
  EXPECT_GE(canonical_pairs, 4u) << "need several unclamped dwell/extension pairs to prove the alternation";
}

TEST(Exchange, Listen_OnHopFiresExactlyOncePerHop) {
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  ListenSpec spec;
  spec.window_ms = 150;
  spec.policy = ListenPolicy::ROTATE_ALL_CHANNELS;
  spec.dwell_ms = 5;
  int hop_calls = 0;
  spec.on_hop = [&]() { hop_calls++; };

  RadioRxPacket packet{};
  IoFrame frame{};
  engine.listen(spec, packet, frame, [](const IoFrame *, const RadioRxPacket &) { return ReplyDisposition::IGNORE; });

  ASSERT_FALSE(radio.freq_history().empty());
  EXPECT_EQ(static_cast<size_t>(hop_calls), radio.freq_history().size())
      << "on_hop must fire exactly once per change_frequency() call, no more and no less";
}

// ============================================================================
// Exchange-engine counters. Pure additive telemetry: free-running, engine-wide, no behavior
// change. Internal-only for now — see the Counters doc comment in exchange_engine.h for why
// there's no sensor/log consumer yet. One test per increment site.
// ============================================================================

TEST(Exchange, CountersTrackLbtRetries) {
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  IoFrame request{};
  create_execute_position(request, comp.node_id_, test::DST_ID, false, 100);

  // Two busy reads (>= LBT_RSSI_THRESHOLD_DBM) force two retries; the queue then runs dry and
  // read_rssi() falls back to MockRadio's default (-120 dBm, well clear), so the third read lets
  // transmit_frame() proceed without a third retry being counted.
  radio.queue_rssi(-50);
  radio.queue_rssi(-50);

  IoFrame correct = build_status_response(test::DST_ID, comp.node_id_);
  radio.queue_rx(to_rx_packet(correct));

  IoFrame response{};
  comp.send_and_receive_(request, response, FREQ_CH2);

  EXPECT_EQ(comp.exchange_engine_.counters().lbt_retries, 2u);
}

TEST(Exchange, CountersTrackRetransmits) {
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  IoFrame request{};
  create_execute_position(request, comp.node_id_, test::DST_ID, false, 100);

  // Try 1's request TX itself fails (mirrors SendAndReceive_AllTransmitFails' pattern), which
  // forces a retry without needing to simulate an entire wait-window timeout via the RX queue —
  // MockRadio's HOLD-policy busy-loop drains queued RX entries near-instantly regardless of which
  // logical "try" is current, so queue_rx_silence() can't stand in for a whole-window timeout here.
  radio.queue_tx_result(false);  // try 1: request TX fails.

  IoFrame correct = build_status_response(test::DST_ID, comp.node_id_);
  radio.queue_rx(to_rx_packet(correct));  // try 2: TX defaults to success, and this is the response.

  IoFrame response{};
  bool ok = comp.send_and_receive_(request, response, FREQ_CH2) == ExchangeOutcome::SUCCESS_WITH_RESPONSE;

  EXPECT_TRUE(ok) << "sanity: the second try must be the one that succeeds";
  EXPECT_EQ(comp.exchange_engine_.counters().retransmits, 1u) << "one retry beyond the first attempt";
}

TEST(Exchange, CountersTrackChallengeRoundTripsBothDirections) {
  // Outbound direction: a device challenges our command; handle_authentication_() answers it.
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
  radio.queue_rx(to_rx_packet(challenge));

  IoFrame final_resp = build_status_response(test::DST_ID, comp.node_id_);
  radio.queue_rx(to_rx_packet(final_resp));

  IoFrame response{};
  bool outbound_ok = comp.send_and_receive_(request, response, FREQ_CH2) == ExchangeOutcome::SUCCESS_WITH_RESPONSE;
  ASSERT_TRUE(outbound_ok);
  EXPECT_EQ(comp.exchange_engine_.counters().challenge_round_trips, 1u)
      << "a device challenging our outbound command must count as one round trip";

  // Inbound direction: we challenge a device's unsolicited command; authenticate_request_()
  // verifies it. Same TestableComponent/counters — this is the second increment site, not a
  // second engine, so the count accumulates on top of the outbound one above.
  RespondOnChallengeMockRadio inbound_radio;
  comp.radio_ = &inbound_radio;
  IoFrame status_update = build_status_update_from_device(test::DST_ID, comp.node_id_);
  inbound_radio.arm(status_update, test::TEST_SYSTEM_KEY, /*valid=*/true);

  bool inbound_ok = comp.authenticate_request_(status_update, FREQ_CH2);
  ASSERT_TRUE(inbound_ok);
  EXPECT_EQ(comp.exchange_engine_.counters().challenge_round_trips, 2u)
      << "both challenge directions must feed the same counter";
}

TEST(Exchange, CountersTrackParseFailures) {
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  IoFrame request{};
  create_execute_position(request, comp.node_id_, test::DST_ID, false, 100);

  // Shorter than FRAME_MIN_SIZE: parse() must reject it outright (mirrors
  // Listen_UnparsablePacketReachesHandlerWithNullParsed's stimulus).
  RadioRxPacket garbage{};
  garbage.len = 4;
  radio.queue_rx(garbage);

  IoFrame response{};
  comp.send_and_receive_(request, response, FREQ_CH2);  // expected to fail overall; only the counter matters here

  EXPECT_EQ(comp.exchange_engine_.counters().parse_failures, 1u);
}

TEST(Exchange, CountersResetZeroesEveryField) {
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  IoFrame request{};
  create_execute_position(request, comp.node_id_, test::DST_ID, false, 100);

  // Drive all four counters nonzero before reset, mirroring each field's own CountersTrack* test
  // above: two busy RSSI reads for lbt_retries, a failed request TX for retransmits, a queued
  // challenge for challenge_round_trips, and a garbage packet for parse_failures.
  radio.queue_rssi(-50);
  radio.queue_rssi(-50);
  radio.queue_tx_result(false);  // try 1: request TX fails, forcing a retry.

  uint8_t chal_data[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  IoFrame challenge = build_challenge(test::DST_ID, comp.node_id_, chal_data);
  radio.queue_rx(to_rx_packet(challenge));

  RadioRxPacket garbage{};
  garbage.len = 4;
  radio.queue_rx(garbage);

  IoFrame response{};
  comp.send_and_receive_(request, response, FREQ_CH2);

  ASSERT_GT(comp.exchange_engine_.counters().lbt_retries, 0u) << "sanity: lbt_retries must be nonzero before reset";
  ASSERT_GT(comp.exchange_engine_.counters().retransmits, 0u) << "sanity: retransmits must be nonzero before reset";
  ASSERT_GT(comp.exchange_engine_.counters().challenge_round_trips, 0u)
      << "sanity: challenge_round_trips must be nonzero before reset";
  ASSERT_GT(comp.exchange_engine_.counters().parse_failures, 0u)
      << "sanity: parse_failures must be nonzero before reset";

  comp.exchange_engine_.reset_counters();

  const auto &c = comp.exchange_engine_.counters();
  EXPECT_EQ(c.lbt_retries, 0u);
  EXPECT_EQ(c.retransmits, 0u);
  EXPECT_EQ(c.challenge_round_trips, 0u);
  EXPECT_EQ(c.parse_failures, 0u);
}

// ============================================================================
// Low-power wake belief: per-try request preamble in send_and_receive()
// ============================================================================
// A low-power START request orders its tries by the target's wake belief (see
// decisions::low_power_try_preamble()). MockRadio has no queued reply unless a test adds one, so
// every try transmits and the preamble of each is read back from get_tx_configs().

namespace {

/// Standalone engine on a manual clock with a scripted target-evidence provider, so the belief is
/// driven by explicit "N ms ago" stamps rather than by a hub.
struct WakeBeliefRig {
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
  RadioDriver *radio_ptr{&radio};
  TuningConfig tuning{};
  ExchangeEngine engine{&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning};
  decisions::TargetEvidence evidence{};  // all zero = never moved, never heard from
  bool evidence_known{true};
  int provider_calls{0};

  WakeBeliefRig() {
    engine.set_target_evidence_provider([this](const uint8_t *, decisions::TargetEvidence &out) {
      provider_calls++;
      out = evidence;
      return evidence_known;
    });
  }

  void moved_ago(uint32_t ms) { evidence.last_moving_evidence_ms = esphome::millis() - ms; }
  void heard_ago(uint32_t ms) { evidence.last_seen_ms = esphome::millis() - ms; }

  /// Run one exchange to a silent device and return the preamble of every transmit, in order.
  std::vector<uint16_t> send(const IoFrame &request, uint8_t max_tries = EXCHANGE_RETRY_COUNT,
                             uint16_t override_preamble = 0) {
    IoFrame response{};
    engine.send_and_receive(request, response, FREQ_CH2, ExchangeOptions{max_tries}.with_preamble(override_preamble));
    std::vector<uint16_t> preambles;
    for (const auto &config : radio.get_tx_configs())
      preambles.push_back(config.preamble_len);
    return preambles;
  }
};

IoFrame low_power_position_request(bool low_power = true) {
  IoFrame request{};
  create_execute_position(request, test::OWN_ID, test::DST_ID, low_power, 40);
  return request;
}

IoFrame low_power_stop_request() {
  IoFrame request{};
  create_execute_command(request, test::OWN_ID, test::DST_ID, /*low_power=*/true, CoverCommand::STOP, false);
  return request;
}

using Preambles = std::vector<uint16_t>;

}  // namespace

TEST(WakeBelief, NoProviderKeepsTheWakeUpPreambleOnEveryTry) {
  WakeBeliefRig rig;
  rig.engine.set_target_evidence_provider({});
  EXPECT_EQ(rig.send(low_power_position_request()),
            (Preambles{MOCK_WAKE_PREAMBLE, MOCK_WAKE_PREAMBLE, MOCK_WAKE_PREAMBLE}));
}

TEST(WakeBelief, AwakeTargetGetsShortThenWakeUpTwice) {
  WakeBeliefRig rig;
  rig.moved_ago(1000);
  const uint16_t short_preamble = mock_preamble_bytes(rig.tuning.start_preamble_ms);
  EXPECT_EQ(rig.send(low_power_position_request()),
            (Preambles{short_preamble, MOCK_WAKE_PREAMBLE, MOCK_WAKE_PREAMBLE}));
  EXPECT_EQ(rig.engine.get_debug().wake_belief, decisions::WakeBelief::AWAKE);
  EXPECT_EQ(rig.engine.get_debug().last_try_preamble, MOCK_WAKE_PREAMBLE);
}

TEST(WakeBelief, RecentlyHeardTargetWithoutMovingEvidenceIsAsleep) {
  // A resting VELUX SSL ignored the short preamble even right after it had answered (ADR 0040,
  // amendment): having heard from the device is no reason to lead short.
  WakeBeliefRig rig;
  rig.heard_ago(34);
  EXPECT_EQ(rig.send(low_power_position_request()),
            (Preambles{MOCK_WAKE_PREAMBLE, MOCK_WAKE_PREAMBLE, MOCK_WAKE_PREAMBLE}));
  EXPECT_EQ(rig.engine.get_debug().wake_belief, decisions::WakeBelief::ASLEEP);
}

TEST(WakeBelief, AsleepTargetGetsTheWakeUpPreambleOnEveryTry) {
  WakeBeliefRig rig;
  rig.heard_ago(60000);
  rig.moved_ago(LOW_POWER_MAX_TRAVEL_MS + 1000);
  EXPECT_EQ(rig.send(low_power_position_request()),
            (Preambles{MOCK_WAKE_PREAMBLE, MOCK_WAKE_PREAMBLE, MOCK_WAKE_PREAMBLE}));
  EXPECT_EQ(rig.engine.get_debug().wake_belief, decisions::WakeBelief::ASLEEP);
}

TEST(WakeBelief, StopIsAwakeEvenWithoutEvidence) {
  WakeBeliefRig rig;
  const uint16_t short_preamble = mock_preamble_bytes(rig.tuning.start_preamble_ms);
  EXPECT_EQ(rig.send(low_power_stop_request()), (Preambles{short_preamble, MOCK_WAKE_PREAMBLE, MOCK_WAKE_PREAMBLE}));
}

TEST(WakeBelief, UnknownDestinationIsAsleep) {
  WakeBeliefRig rig;
  rig.moved_ago(1000);  // would be AWAKE, but the provider says it has never heard of this node
  rig.evidence_known = false;
  EXPECT_EQ(rig.send(low_power_position_request()),
            (Preambles{MOCK_WAKE_PREAMBLE, MOCK_WAKE_PREAMBLE, MOCK_WAKE_PREAMBLE}));
  EXPECT_EQ(rig.engine.get_debug().wake_belief, decisions::WakeBelief::ASLEEP);
}

TEST(WakeBelief, KillSwitchRestoresTheWakeUpPreambleOnEveryTry) {
  WakeBeliefRig rig;
  rig.moved_ago(1000);
  rig.tuning.low_power_wake_belief = false;
  EXPECT_EQ(rig.send(low_power_position_request()),
            (Preambles{MOCK_WAKE_PREAMBLE, MOCK_WAKE_PREAMBLE, MOCK_WAKE_PREAMBLE}));
  EXPECT_EQ(rig.provider_calls, 0) << "with the switch off the evidence is never even looked up";
}

TEST(WakeBelief, ExplicitPreambleOverrideWinsOnEveryTry) {
  WakeBeliefRig rig;
  rig.moved_ago(1000);
  EXPECT_EQ(rig.send(low_power_position_request(), EXCHANGE_RETRY_COUNT, /*override_preamble=*/48),
            (Preambles{48, 48, 48}));
  EXPECT_EQ(rig.provider_calls, 0) << "an override is the caller's choice and is never second-guessed";
}

TEST(WakeBelief, DebugSnapshotNamesWhyNoBeliefApplied) {
  // The exchange-failure log line prints this reason in its belief= field, so a posted log says
  // whether the switch was off, the frame was not low-power, and so on — one "n/a" could not.
  using Use = ExchangeEngine::WakeBeliefUse;
  WakeBeliefRig rig;
  rig.moved_ago(1000);

  rig.send(low_power_position_request(/*low_power=*/false));
  EXPECT_EQ(rig.engine.get_debug().wake_belief_use, Use::NOT_LOW_POWER);

  rig.send(low_power_position_request(), EXCHANGE_RETRY_COUNT, /*override_preamble=*/48);
  EXPECT_EQ(rig.engine.get_debug().wake_belief_use, Use::OVERRIDE);

  rig.send(low_power_position_request(), /*max_tries=*/1);
  EXPECT_EQ(rig.engine.get_debug().wake_belief_use, Use::APPLIED) << "the try count is never a reason to skip";

  rig.tuning.low_power_wake_belief = false;
  rig.send(low_power_position_request());
  EXPECT_EQ(rig.engine.get_debug().wake_belief_use, Use::SWITCHED_OFF);
  rig.tuning.low_power_wake_belief = true;

  rig.send(low_power_position_request());
  EXPECT_EQ(rig.engine.get_debug().wake_belief_use, Use::APPLIED);

  rig.engine.set_target_evidence_provider({});
  rig.send(low_power_position_request());
  EXPECT_EQ(rig.engine.get_debug().wake_belief_use, Use::NO_PROVIDER);
}

TEST(WakeBelief, SwitchOffKeepsASingleTryExchangeOnTheWakeUpPreamble) {
  WakeBeliefRig rig;
  rig.moved_ago(1000);
  rig.tuning.low_power_wake_belief = false;
  EXPECT_EQ(rig.send(low_power_position_request(), /*max_tries=*/1), (Preambles{MOCK_WAKE_PREAMBLE}));
  EXPECT_EQ(rig.engine.get_debug().wake_belief_use, ExchangeEngine::WakeBeliefUse::SWITCHED_OFF);
}

TEST(WakeBelief, SkipReasonLabelsAreDistinctAndReadable) {
  using Use = ExchangeEngine::WakeBeliefUse;
  EXPECT_STREQ(ExchangeEngine::wake_belief_use_name(Use::NOT_LOW_POWER), "not_low_power");
  EXPECT_STREQ(ExchangeEngine::wake_belief_use_name(Use::OVERRIDE), "override");
  EXPECT_STREQ(ExchangeEngine::wake_belief_use_name(Use::SWITCHED_OFF), "off");
  EXPECT_STREQ(ExchangeEngine::wake_belief_use_name(Use::NO_PROVIDER), "no_provider");
  EXPECT_STREQ(ExchangeEngine::wake_belief_use_name(Use::APPLIED), "applied");
}

TEST(WakeBelief, AlwaysAliveTargetKeepsTheNormalStartPreambleOnEveryTry) {
  WakeBeliefRig rig;
  rig.moved_ago(1000);
  const uint16_t short_preamble = mock_preamble_bytes(rig.tuning.start_preamble_ms);
  EXPECT_EQ(rig.send(low_power_position_request(/*low_power=*/false)),
            (Preambles{short_preamble, short_preamble, short_preamble}));
  EXPECT_EQ(rig.provider_calls, 0) << "only a low-power start frame has a wake-up preamble to reorder";
}

TEST(WakeBelief, NonStartFrameKeepsTheResponsePreamble) {
  // Key-transfer-shaped frame: a non-start frame that still carries CTRL1_LOW_POWER.
  WakeBeliefRig rig;
  rig.moved_ago(1000);
  IoFrame request{};
  init_frame(request, true, false, false, /*low_power=*/true);
  set_dst(request, test::DST_ID);
  set_src(request, test::OWN_ID);
  uint8_t payload[16] = {0};
  set_cmd(request, CMD_KEY_TRANSFER, payload, sizeof(payload));
  ASSERT_FALSE(is_start(request));
  ASSERT_NE(request.ctrl1 & CTRL1_LOW_POWER, 0);

  const Preambles preambles = rig.send(request);

  ASSERT_FALSE(preambles.empty());
  for (const uint16_t preamble : preambles)
    EXPECT_EQ(preamble, SHORT_PREAMBLE) << "the driver's response preamble, never a wake belief";
  EXPECT_EQ(rig.provider_calls, 0);
}

TEST(WakeBelief, SingleTryExchangeSendsTheBeliefsFirstTry) {
  // A scheduler-owned poll is allowed one try at most ladder slots. Its likeliest moment is seconds
  // after a command or STOP, when the receiver is travelling or has just answered and ignores the
  // wake-up preamble, so it leads with the same preamble a multi-try exchange would.
  WakeBeliefRig rig;
  rig.moved_ago(1000);
  EXPECT_EQ(rig.send(low_power_position_request(), /*max_tries=*/1),
            (Preambles{mock_preamble_bytes(rig.tuning.start_preamble_ms)}));
  EXPECT_EQ(rig.provider_calls, 1);
  EXPECT_EQ(rig.engine.get_debug().wake_belief_use, ExchangeEngine::WakeBeliefUse::APPLIED);
  EXPECT_EQ(rig.engine.get_debug().wake_belief, decisions::WakeBelief::AWAKE);
}

TEST(WakeBelief, SingleTryExchangeToARecentlyHeardTargetKeepsTheWakeUpPreamble) {
  // The settle poll after an accepted STOP: moving evidence was cleared and the STOP's reply was
  // heard a second ago. The receiver is at rest, where only the wake-up preamble reaches it.
  WakeBeliefRig rig;
  rig.heard_ago(1000);
  EXPECT_EQ(rig.send(low_power_position_request(), /*max_tries=*/1), (Preambles{MOCK_WAKE_PREAMBLE}));
  EXPECT_EQ(rig.engine.get_debug().wake_belief, decisions::WakeBelief::ASLEEP);
}

TEST(WakeBelief, TwoTryExchangeStillFollowsThePlan) {
  WakeBeliefRig rig;
  rig.moved_ago(1000);
  EXPECT_EQ(rig.send(low_power_position_request(), /*max_tries=*/2),
            (Preambles{mock_preamble_bytes(rig.tuning.start_preamble_ms), MOCK_WAKE_PREAMBLE}));
}

TEST(WakeBelief, TunedWakePreambleReplacesLongPreamble) {
  WakeBeliefRig rig;
  rig.tuning.low_power_wake_ms = mock_preamble_ms(2048);
  EXPECT_EQ(rig.send(low_power_position_request()), (Preambles{2048, 2048, 2048}));
}

TEST(WakeBelief, TunedWakePreambleAppliesWithTheSwitchOff) {
  WakeBeliefRig rig;
  rig.tuning.low_power_wake_belief = false;
  rig.tuning.low_power_wake_ms = mock_preamble_ms(1536);
  EXPECT_EQ(rig.send(low_power_position_request()), (Preambles{1536, 1536, 1536}));
}

TEST(WakeBelief, SilentAwakeExchangeSpendsTheMovingEvidence) {
  WakeBeliefRig rig;
  rig.moved_ago(1000);
  int spent = 0;
  rig.engine.set_wake_evidence_spent_handler([&spent](const uint8_t *dst) {
    EXPECT_EQ(memcmp(dst, test::DST_ID, NODE_ID_SIZE), 0);
    spent++;
  });
  rig.send(low_power_position_request(), /*max_tries=*/1);
  EXPECT_EQ(spent, 1) << "a receiver believed moving that answers nothing has most likely come to rest";
}

TEST(WakeBelief, SilentAwakeExchangeKeepsTheEvidenceWhileTheReportedRemainingTimeHasNotRunOut) {
  WakeBeliefRig rig;
  rig.moved_ago(1000);
  rig.evidence.moving_until_ms = esphome::millis() + 5000;
  int spent = 0;
  rig.engine.set_wake_evidence_spent_handler([&spent](const uint8_t *) { spent++; });

  rig.send(low_power_position_request(), /*max_tries=*/1);
  EXPECT_EQ(spent, 0) << "by its own count it is still travelling: one missed frame says nothing";

  esphome::test_clock::advance_ms(5000);
  rig.moved_ago(1000);
  rig.send(low_power_position_request(), /*max_tries=*/1);
  EXPECT_EQ(spent, 1) << "once the count has run out a silent short try does spend the evidence";
}

TEST(WakeBelief, SilentStopLeavesTheEvidenceAlone) {
  WakeBeliefRig rig;
  rig.moved_ago(1000);
  int spent = 0;
  rig.engine.set_wake_evidence_spent_handler([&spent](const uint8_t *) { spent++; });
  rig.send(low_power_stop_request());
  EXPECT_EQ(spent, 0) << "an unanswered STOP stopped nothing: the receiver may still be travelling";
}

TEST(WakeBelief, SilentAsleepExchangeLeavesTheEvidenceAlone) {
  WakeBeliefRig rig;
  int spent = 0;
  rig.engine.set_wake_evidence_spent_handler([&spent](const uint8_t *) { spent++; });
  rig.send(low_power_position_request());
  EXPECT_EQ(spent, 0) << "only an AWAKE belief has moving evidence to spend";

  rig.moved_ago(1000);
  rig.send(low_power_position_request(/*low_power=*/false));
  EXPECT_EQ(spent, 0) << "an always-alive target has no belief at all";
}

TEST(WakeBelief, EvidenceIsLookedUpOncePerExchange) {
  WakeBeliefRig rig;
  rig.moved_ago(1000);
  ASSERT_EQ(rig.send(low_power_position_request()).size(), 3u);
  EXPECT_EQ(rig.provider_calls, 1) << "a belief is resolved once, not re-derived (and re-clocked) every try";
}

TEST(WakeBelief, OnlyTheClockChangesBetweenTries_FrameBytesAreIdentical) {
  WakeBeliefRig rig;
  rig.moved_ago(1000);
  ASSERT_EQ(rig.send(low_power_position_request()).size(), 3u);
  const auto &sent = rig.radio.get_sent_data();
  ASSERT_EQ(sent.size(), 3u);
  EXPECT_EQ(sent[0], sent[1]);
  EXPECT_EQ(sent[0], sent[2]);
}

TEST(WakeBelief, ReplyOnTheWakeUpTryEndsTheExchange) {
  // The belief was wrong (the target was not listening for the short preamble): try 2's wake-up
  // preamble reaches it. The exchange succeeds after exactly two transmits.
  WakeBeliefRig rig;
  rig.moved_ago(1000);
  rig.radio.queue_rx_hold_until_sent(2);
  rig.radio.queue_rx(test::make_rx_packet(build_status_response(test::DST_ID, test::OWN_ID)));

  IoFrame response{};
  const ExchangeOutcome outcome =
      rig.engine.send_and_receive(low_power_position_request(), response, FREQ_CH2, EXCHANGE_RETRY_COUNT);

  EXPECT_EQ(outcome, ExchangeOutcome::SUCCESS_WITH_RESPONSE);
  ASSERT_EQ(rig.radio.get_tx_configs().size(), 2u);
  EXPECT_EQ(rig.radio.get_tx_configs()[0].preamble_len, mock_preamble_bytes(rig.tuning.start_preamble_ms));
  EXPECT_EQ(rig.radio.get_tx_configs()[1].preamble_len, MOCK_WAKE_PREAMBLE);
}

TEST(WakeBelief, DiscoverConfirmStyleCallersStillGetTheAsleepRule) {
  // request_preamble_for() is the single-shot rule (pairing's discover-confirm uses it directly):
  // it never consults a belief, whatever the evidence says.
  WakeBeliefRig rig;
  rig.moved_ago(1000);
  EXPECT_EQ(rig.engine.request_preamble_for(low_power_position_request()), MOCK_WAKE_PREAMBLE);
  EXPECT_EQ(rig.provider_calls, 0);
}

// ============================================================================
// Transmit observer — transmit_frame() reports LBT deferrals and sent frames to the attached
// TransmitObserver (PairingTelemetry during pairing; any future TX accounting the same way).
// ============================================================================

namespace {

/// Records every TransmitObserver callback it receives.
struct RecordingTransmitObserver : TransmitObserver {
  struct Sent {
    uint8_t cmd;
    RadioTxConfig config;
    uint8_t wire_len;
  };
  std::vector<int16_t> lbt_defers;
  std::vector<Sent> sent;

  void on_lbt_defer(int16_t rssi_dbm) override { this->lbt_defers.push_back(rssi_dbm); }
  void on_transmit(const IoFrame &frame, const RadioTxConfig &config, uint8_t wire_len) override {
    this->sent.push_back({frame.cmd, config, wire_len});
  }
};

/// A hub whose engine talks to `radio`, with the node ID set so frames serialize.
void wire_observer_rig(TestableComponent &comp, MockRadio &radio) {
  comp.initialized_ = true;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);
}

}  // namespace

TEST(Exchange, TransmitObserverSeesSentFrameWithItsTxShape) {
  TestableComponent comp;
  MockRadio radio;
  wire_observer_rig(comp, radio);
  RecordingTransmitObserver observer;
  comp.exchange_engine_.set_transmit_observer(&observer);

  IoFrame request{};
  create_execute_position(request, comp.node_id_, test::DST_ID, false, 100);
  ASSERT_TRUE(comp.exchange_engine_.transmit_frame(request, FREQ_CH3, LONG_PREAMBLE));

  ASSERT_EQ(observer.sent.size(), 1u);
  EXPECT_EQ(observer.sent[0].cmd, request.cmd);
  EXPECT_EQ(observer.sent[0].config.freq_hz, FREQ_CH3);
  EXPECT_EQ(observer.sent[0].config.preamble_len, LONG_PREAMBLE);
  // The length the observer is told is the one the radio was handed.
  ASSERT_EQ(radio.get_sent_data().size(), 1u);
  EXPECT_EQ(observer.sent[0].wire_len, radio.get_sent_data()[0].size());
  EXPECT_TRUE(observer.lbt_defers.empty());
}

TEST(Exchange, TransmitObserverSeesEachLbtDeferralWithItsRssi) {
  TestableComponent comp;
  MockRadio radio;
  wire_observer_rig(comp, radio);
  RecordingTransmitObserver observer;
  comp.exchange_engine_.set_transmit_observer(&observer);

  // Two busy reads, then MockRadio's clear default: two deferrals, then the frame goes out.
  radio.queue_rssi(-50);
  radio.queue_rssi(-60);

  IoFrame request{};
  create_execute_position(request, comp.node_id_, test::DST_ID, false, 100);
  ASSERT_TRUE(comp.exchange_engine_.transmit_frame(request, FREQ_CH2, SHORT_PREAMBLE));

  EXPECT_EQ(observer.lbt_defers, (std::vector<int16_t>{-50, -60}));
  EXPECT_EQ(observer.sent.size(), 1u);
}

TEST(Exchange, TransmitObserverNotToldAboutAFailedSend) {
  TestableComponent comp;
  MockRadio radio;
  wire_observer_rig(comp, radio);
  RecordingTransmitObserver observer;
  comp.exchange_engine_.set_transmit_observer(&observer);

  radio.queue_tx_result(false);
  IoFrame request{};
  create_execute_position(request, comp.node_id_, test::DST_ID, false, 100);
  EXPECT_FALSE(comp.exchange_engine_.transmit_frame(request, FREQ_CH2, SHORT_PREAMBLE));

  EXPECT_TRUE(observer.sent.empty());
}

TEST(Exchange, DetachedTransmitObserverHearsNothing) {
  TestableComponent comp;
  MockRadio radio;
  wire_observer_rig(comp, radio);
  RecordingTransmitObserver observer;
  comp.exchange_engine_.set_transmit_observer(&observer);
  comp.exchange_engine_.set_transmit_observer(nullptr);

  radio.queue_rssi(-50);
  IoFrame request{};
  create_execute_position(request, comp.node_id_, test::DST_ID, false, 100);
  ASSERT_TRUE(comp.exchange_engine_.transmit_frame(request, FREQ_CH2, SHORT_PREAMBLE));

  EXPECT_TRUE(observer.lbt_defers.empty());
  EXPECT_TRUE(observer.sent.empty());
}

// ============================================================================
// Wake level — transmit_frame() stamps every transmission with tx_wake_for(START, preamble), so a
// radio that wakes receivers some other way than by preamble length reads the same decision.
// ============================================================================

TEST(Exchange, TransmitFrameStampsTheWakeLevel) {
  TestableComponent comp;
  MockRadio radio;
  wire_observer_rig(comp, radio);

  IoFrame start{};
  create_execute_position(start, comp.node_id_, test::DST_ID, false, 100);
  ASSERT_TRUE(is_start(start));
  IoFrame continuation = start;
  continuation.ctrl0 &= static_cast<uint8_t>(~CTRL0_START);

  ASSERT_TRUE(comp.exchange_engine_.transmit_frame(start, FREQ_CH2, LONG_PREAMBLE));
  ASSERT_TRUE(comp.exchange_engine_.transmit_frame(start, FREQ_CH2, MOCK_START_PREAMBLE));
  ASSERT_TRUE(comp.exchange_engine_.transmit_frame(continuation, FREQ_CH2, LONG_PREAMBLE));

  ASSERT_EQ(radio.get_tx_configs().size(), 3u);
  EXPECT_EQ(radio.get_tx_configs()[0].wake, TxWake::LONG);
  EXPECT_EQ(radio.get_tx_configs()[1].wake, TxWake::SHORT);
  EXPECT_EQ(radio.get_tx_configs()[2].wake, TxWake::NONE);
}

// ============================================================================
// Listen before talk — transmit_frame() asks the radio to send only on a clear TX channel, up to
// LBT_MAX_RETRIES times, then sends once without the check; a failed send ends it at once. This is the
// access of the senders that keep a fixed channel; a request from send_and_receive() takes a free channel.
// ============================================================================

TEST(Exchange, LbtBusyThenClearSendsOnTheSecondCheck) {
  TestableComponent comp;
  MockRadio radio;
  wire_observer_rig(comp, radio);
  radio.queue_rssi(-50);  // busy once, then MockRadio's clear default

  IoFrame request{};
  create_execute_position(request, comp.node_id_, test::DST_ID, false, 100);
  ASSERT_TRUE(comp.exchange_engine_.transmit_frame(request, FREQ_CH2, SHORT_PREAMBLE));

  EXPECT_EQ(radio.get_send_count(), 1);
  EXPECT_EQ(comp.exchange_engine_.counters().lbt_retries, 1u);
  ASSERT_EQ(radio.get_tx_configs().size(), 1u);
  EXPECT_EQ(radio.get_tx_configs()[0].cca_threshold_dbm, comp.tuning_.lbt_rssi_threshold_dbm)
      << "the frame that went out was sent with the check";
}

TEST(Exchange, LbtBusyOnEveryCheckSendsOnceWithoutIt) {
  TestableComponent comp;
  MockRadio radio;
  wire_observer_rig(comp, radio);
  RecordingTransmitObserver observer;
  comp.exchange_engine_.set_transmit_observer(&observer);
  radio.set_rssi_default(-50);

  IoFrame request{};
  create_execute_position(request, comp.node_id_, test::DST_ID, false, 100);
  ASSERT_TRUE(comp.exchange_engine_.transmit_frame(request, FREQ_CH2, SHORT_PREAMBLE));

  const uint8_t checks = LBT_MAX_RETRIES;
  EXPECT_EQ(comp.exchange_engine_.counters().lbt_retries, checks);
  EXPECT_EQ(observer.lbt_defers.size(), checks);
  EXPECT_EQ(radio.rssi_read_freqs().size(), checks) << "the forced send takes no reading";
  ASSERT_EQ(radio.get_tx_configs().size(), 1u);
  EXPECT_FALSE(radio.get_tx_configs()[0].cca_threshold_dbm.has_value()) << "the last send is forced";
}

TEST(Exchange, LbtFailedSendIsNotRetried) {
  TestableComponent comp;
  MockRadio radio;
  wire_observer_rig(comp, radio);
  radio.queue_tx_result(false);

  IoFrame request{};
  create_execute_position(request, comp.node_id_, test::DST_ID, false, 100);
  EXPECT_FALSE(comp.exchange_engine_.transmit_frame(request, FREQ_CH2, SHORT_PREAMBLE));

  EXPECT_EQ(radio.get_send_count(), 1) << "FAILED is not CHANNEL_BUSY: no further attempt";
  EXPECT_EQ(comp.exchange_engine_.counters().lbt_retries, 0u);
}

// Only a frame that opens a session (CTRL0 START) is checked; one without START answers within a
// session and goes out at once (ADR 0045, issue #178: a single 5 ms back-off before a 0x3D lost the
// exchange on an OXIMO 50 io).
TEST(Exchange, LbtSkippedForAFrameWithoutStart) {
  TestableComponent comp;
  MockRadio radio;
  wire_observer_rig(comp, radio);
  RecordingTransmitObserver observer;
  comp.exchange_engine_.set_transmit_observer(&observer);
  radio.set_rssi_default(-50);  // busy on every check

  IoFrame continuation{};
  create_execute_position(continuation, comp.node_id_, test::DST_ID, false, 100);
  continuation.ctrl0 &= static_cast<uint8_t>(~CTRL0_START);
  ASSERT_FALSE(is_start(continuation));
  ASSERT_TRUE(comp.exchange_engine_.transmit_frame(continuation, FREQ_CH2, SHORT_PREAMBLE));

  EXPECT_EQ(radio.get_send_count(), 1);
  EXPECT_TRUE(radio.rssi_read_freqs().empty()) << "no channel reading for a frame without START";
  EXPECT_EQ(comp.exchange_engine_.counters().lbt_retries, 0u);
  EXPECT_TRUE(observer.lbt_defers.empty());
  ASSERT_EQ(radio.get_tx_configs().size(), 1u);
  EXPECT_FALSE(radio.get_tx_configs()[0].cca_threshold_dbm.has_value());
}

TEST(Exchange, TheRequestsChannelIsCheckedButNotThe0x3DOfAnAuthenticatedExchange) {
  TestableComponent comp;
  MockRadio radio;
  wire_observer_rig(comp, radio);
  radio.set_channel_rssi(FREQ_CH2, -50);  // the first channel is busy; the others are free

  IoFrame request{};
  create_execute_position(request, comp.node_id_, test::DST_ID, false, 100);
  uint8_t chal_data[HMAC_SIZE] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  radio.queue_rx(to_rx_packet(build_challenge(test::DST_ID, comp.node_id_, chal_data)));
  radio.queue_rx(to_rx_packet(build_status_response(test::DST_ID, comp.node_id_)));

  IoFrame response{};
  ASSERT_EQ(comp.send_and_receive_(request, response, FREQ_CH2), ExchangeOutcome::SUCCESS_WITH_RESPONSE);

  EXPECT_EQ(comp.exchange_engine_.counters().lbt_retries, 1u) << "only the request's checks are counted";
  ASSERT_EQ(radio.get_sent_data().size(), 2u) << "request + 0x3D";
  IoFrame sent{};
  const auto &raw = radio.get_sent_data()[1];
  ASSERT_TRUE(parse(raw.data(), static_cast<uint8_t>(raw.size()), sent));
  EXPECT_EQ(sent.cmd, CMD_CHALLENGE_RESP);
  ASSERT_EQ(radio.get_tx_configs().size(), 2u);
  EXPECT_TRUE(radio.get_tx_configs()[0].cca_threshold_dbm.has_value());
  EXPECT_FALSE(radio.get_tx_configs()[1].cca_threshold_dbm.has_value()) << "the 0x3D is never checked";
  EXPECT_EQ(radio.get_tx_configs()[1].freq_hz, radio.get_tx_configs()[0].freq_hz)
      << "the answer goes out on the channel the request took";
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

// ============================================================================
// Attempt plans and the window-aware budget guard. A try starts only if its gap, transmission,
// first-reply window and, once the target has challenged, the final-reply window all end inside
// exchange_total_budget_ms. Every case runs on a ManualClock with modelled air time.
// ============================================================================

namespace {

/// An exchange engine on a ManualClock with modelled air time and a configurable tuning.
struct PlanRig {
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
  RadioDriver *radio_ptr{&radio};
  TuningConfig tuning{};
  ExchangeEngine engine{&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning};

  PlanRig() { radio.set_model_tx_airtime(true); }

  /// One attempt's transmit time for a status poll to a mains or low-power target, as the engine
  /// estimates it.
  uint32_t attempt_tx_ms(bool low_power) const {
    IoFrame request{};
    create_get_status(request, test::OWN_ID, test::DST_ID, low_power);
    uint8_t raw[64];
    const uint8_t len = serialize(request, raw, sizeof(raw));
    const uint16_t preamble =
        low_power ? mock_preamble_bytes(tuning.low_power_wake_ms) : mock_preamble_bytes(tuning.start_preamble_ms);
    return (io868_tx_air_time_us(preamble, len, SOFT_PHY_PREAMBLE_BITS_PER_BYTE) + 999) / 1000;
  }

  /// Run a status poll to a target that never answers, with @p options.
  /// @return The wall-clock time the exchange blocked.
  uint32_t run_silent_poll(const ExchangeOptions &options, bool low_power) {
    IoFrame request{};
    create_get_status(request, test::OWN_ID, test::DST_ID, low_power);
    IoFrame response{};
    const uint32_t start = esphome::test_clock::peek_ms();
    EXPECT_EQ(engine.send_and_receive(request, response, FREQ_CH2, options), ExchangeOutcome::FAILED);
    return esphome::test_clock::peek_ms() - start;
  }
};

/// An upper bound on the time @p attempts attempts to a silent target take, worked out from the attempts alone
/// (transmission, overhead, listen window and gap), independent of the guard and the budget it applies.
uint32_t silent_attempts_bound_ms(const PlanRig &rig, uint32_t attempts, uint32_t gap_ms, uint32_t window_ms,
                                  bool low_power) {
  return attempts * (rig.attempt_tx_ms(low_power) + EXCHANGE_ATTEMPT_OVERHEAD_MS + window_ms) +
         (attempts - 1U) * gap_ms;
}

}  // namespace

TEST(Exchange, SettlingReadToASilentMainsTargetMakesTheFittingNumberOfAttemptsWithoutAGap) {
  PlanRig rig;
  IoFrame status{};
  create_get_status(status, test::OWN_ID, test::DST_ID, false);
  const auto plan = expected_plan(rig.engine, rig.tuning, decisions::ReadKind::SETTLING, status);
  ASSERT_GT(plan.max_attempts, EXCHANGE_RETRY_COUNT) << "sanity: the plan is more than today's three";

  const uint32_t blocked_ms = rig.run_silent_poll(read_of(decisions::ReadKind::SETTLING), false);

  EXPECT_EQ(rig.radio.get_send_count(), plan.max_attempts);
  EXPECT_LT(blocked_ms, rig.tuning.exchange_total_budget_ms);
  const auto &t = rig.radio.send_times_ms();
  for (size_t i = 0; i + 1 < t.size(); i++)
    EXPECT_LE(t[i + 1] - t[i], rig.attempt_tx_ms(false) + rig.radio.wait_timeouts()[i] + CHANNEL_CHECK_TIME_MS)
        << "no gap: the next attempt follows the window and its channel check (the estimate rounds the air time up)";
}

TEST(Exchange, SettlingReadIsCappedAtTheMostAttemptsWhateverTheBudget) {
  PlanRig rig;
  rig.tuning.exchange_total_budget_ms = 12000;

  rig.run_silent_poll(read_of(decisions::ReadKind::SETTLING), false);

  EXPECT_EQ(rig.radio.get_send_count(), EXCHANGE_MAX_ATTEMPTS);
}

TEST(Exchange, SettlingReadToASilentLowPowerTargetListensTheLowPowerWindowOnEveryAttempt) {
  PlanRig rig;
  IoFrame status{};
  create_get_status(status, test::OWN_ID, test::DST_ID, true);
  const auto plan = expected_plan(rig.engine, rig.tuning, decisions::ReadKind::SETTLING, status);
  const uint32_t blocked_ms = rig.run_silent_poll(read_of(decisions::ReadKind::SETTLING), true);

  EXPECT_EQ(rig.radio.get_send_count(), LOW_POWER_SETTLING_ATTEMPTS);
  ASSERT_EQ(rig.radio.wait_timeouts().size(), static_cast<size_t>(LOW_POWER_SETTLING_ATTEMPTS));
  for (const auto wait : rig.radio.wait_timeouts())
    EXPECT_EQ(wait, LOW_POWER_RESPONSE_WAIT_MS);
  EXPECT_LT(blocked_ms, silent_attempts_bound_ms(rig, plan.max_attempts, plan.gap_ms, plan.window_ms, true));
}

TEST(Exchange, ASettlingReadToASilentLowPowerTargetRunsThreeWakeUpAttemptsPastTheBudgetAtTheDefaultWakeUp) {
  // ADR 0095, ADR 0097: at the 600 ms default one wake-up attempt and its window take a little over
  // a second, so three of them run past the 2500 ms budget, and the exchange ends after the third.
  PlanRig rig;
  ASSERT_EQ(mock_preamble_bytes(rig.tuning.low_power_wake_ms), MOCK_WAKE_PREAMBLE);
  const uint32_t blocked_ms = rig.run_silent_poll(read_of(decisions::ReadKind::SETTLING), true);

  EXPECT_EQ(rig.radio.get_send_count(), LOW_POWER_SETTLING_ATTEMPTS);
  EXPECT_GT(blocked_ms, rig.tuning.exchange_total_budget_ms) << "the exchange does run past the budget";
  EXPECT_LT(blocked_ms, 3600u) << "about 3.5 s, the three attempts and nothing more";
}

TEST(Exchange, ACommandToASilentLowPowerTargetKeepsThreeAttemptsAtEveryWakeUpLength) {
  for (const uint16_t wake_preamble : {1024, 2048, 4096}) {
    PlanRig rig;
    rig.tuning.low_power_wake_ms = mock_preamble_ms(wake_preamble);
    rig.run_silent_poll(read_of(decisions::ReadKind::COMMAND), true);
    EXPECT_EQ(rig.radio.get_send_count(), EXCHANGE_RETRY_COUNT) << "wake-up " << wake_preamble << " B";
  }
}

TEST(Exchange, AMainsTargetOrAMidTravelLookKeepsTheBudgetAtTheDefaultWakeUp) {
  PlanRig rig;
  const uint32_t blocked_ms = rig.run_silent_poll(read_of(decisions::ReadKind::SETTLING), false);
  EXPECT_LT(blocked_ms, rig.tuning.exchange_total_budget_ms);
}

TEST(Exchange, NoRequestLetsASilentTargetBlockPastTheBudget) {
  // Windows and budgets from short to long, for a settling read (gapless) and for the count form
  // (the default retry gap): the guard keeps the blocked time under the budget when the target
  // never answers. A low-power target's wake-up exchange is bounded by its attempt count instead
  // (ADR 0095): it makes all its attempts and takes no longer than they do.
  for (const bool counted : {false, true}) {
    for (const bool low_power : {false, true}) {
      for (const uint16_t window_ms : {200, 400, 550, 1000}) {
        for (const uint16_t budget_ms : {1000, 2500, 5000, 12000}) {
          PlanRig rig;
          rig.tuning.exchange_start_response_wait_ms = window_ms;
          rig.tuning.exchange_total_budget_ms = budget_ms;
          const ExchangeOptions options =
              counted ? ExchangeOptions{EXCHANGE_RETRY_COUNT} : read_of(decisions::ReadKind::SETTLING);
          IoFrame probe{};
          create_get_status(probe, test::OWN_ID, test::DST_ID, low_power);
          const uint32_t blocked_ms = rig.run_silent_poll(options, low_power);
          if (low_power) {
            const uint32_t attempts = counted ? EXCHANGE_RETRY_COUNT : LOW_POWER_SETTLING_ATTEMPTS;
            const uint32_t gap_ms = counted ? EXCHANGE_RETRY_DELAY_MS : 0;
            const uint32_t listen_ms = counted ? window_ms : LOW_POWER_RESPONSE_WAIT_MS;
            EXPECT_EQ(rig.radio.get_send_count(), static_cast<int>(attempts));
            EXPECT_LT(blocked_ms, silent_attempts_bound_ms(rig, attempts, gap_ms, listen_ms, true))
                << "counted=" << counted << " window=" << window_ms << " budget=" << budget_ms;
          } else if (window_ms < budget_ms) {
            // The first attempt always runs, so a window longer than the budget is the one exception.
            EXPECT_LT(blocked_ms, budget_ms)
                << "counted=" << counted << " window=" << window_ms << " budget=" << budget_ms;
          }
        }
      }
    }
  }
}

TEST(Exchange, ChallengedLowPowerPollKeepsItsThirdTryBecauseItsAttemptCountBoundsIt) {
  // Each try is challenged and its final reply never comes: a try costs its wake-up transmission
  // plus the 500 ms final window, so after two the budget rule would refuse a third. A low-power
  // target's wake-up exchange is bounded by its attempt count instead (ADR 0095): all three run.
  PlanRig rig;
  queue_challenges(rig.radio, EXCHANGE_RETRY_COUNT);
  IoFrame request{};
  create_get_status(request, test::OWN_ID, test::DST_ID, /*low_power=*/true);
  IoFrame response{};

  EXPECT_EQ(rig.engine.send_and_receive(request, response, FREQ_CH2), ExchangeOutcome::SUCCESS_UNCONFIRMED);

  EXPECT_EQ(rig.radio.get_send_count(), 2 * EXCHANGE_RETRY_COUNT) << "three tries, each a request and a 0x3D";
  EXPECT_STRNE(rig.engine.get_debug().stage, "retry_budget_exhausted");
}

TEST(Exchange, ChallengedMainsPollStillMakesItsThirdTry) {
  PlanRig rig;
  queue_challenges(rig.radio, EXCHANGE_RETRY_COUNT);
  IoFrame request{};
  create_get_status(request, test::OWN_ID, test::DST_ID, /*low_power=*/false);
  IoFrame response{};

  EXPECT_EQ(rig.engine.send_and_receive(request, response, FREQ_CH2), ExchangeOutcome::SUCCESS_UNCONFIRMED);

  EXPECT_EQ(rig.radio.get_send_count(), 2 * EXCHANGE_RETRY_COUNT);
}

TEST(Exchange, CommandToASilentMainsTargetMakesNineAttemptsListeningShortOnAllButTheLast) {
  PlanRig rig;
  IoFrame request{};
  create_execute_position(request, test::OWN_ID, test::DST_ID, /*low_power=*/false, 40);
  IoFrame response{};
  const uint32_t start = esphome::test_clock::peek_ms();

  EXPECT_EQ(rig.engine.send_and_receive(request, response, FREQ_CH2, read_of(decisions::ReadKind::COMMAND)),
            ExchangeOutcome::FAILED);

  EXPECT_EQ(rig.radio.get_send_count(), EXCHANGE_MAX_ATTEMPTS) << "more than today's three for a silent mains target";
  const auto &waits = rig.radio.wait_timeouts();
  ASSERT_EQ(waits.size(), static_cast<size_t>(EXCHANGE_MAX_ATTEMPTS));
  for (size_t i = 0; i + 1 < waits.size(); i++)
    EXPECT_EQ(waits[i], rig.tuning.exchange_early_response_wait_ms) << "attempt " << i + 1;
  EXPECT_EQ(waits.back(), rig.tuning.exchange_start_response_wait_ms) << "the last attempt listens the whole window";
  EXPECT_LT(esphome::test_clock::peek_ms() - start, rig.tuning.exchange_total_budget_ms);
}

TEST(Exchange, CommandPlanNeverBlocksPastTheBudgetForAnyEarlyAndStartWindow) {
  for (const uint16_t early_ms : {150, 200, 400, 1000}) {
    for (const uint16_t start_ms : {200, 400, 1000}) {
      for (const uint16_t budget_ms : {500, 1000, 2500, 12000}) {
        PlanRig rig;
        rig.tuning.exchange_early_response_wait_ms = early_ms;
        rig.tuning.exchange_start_response_wait_ms = start_ms;
        rig.tuning.exchange_total_budget_ms = budget_ms;
        IoFrame status{};
        create_get_status(status, test::OWN_ID, test::DST_ID, false);
        const auto plan = expected_plan(rig.engine, rig.tuning, decisions::ReadKind::COMMAND, status);
        const uint32_t blocked_ms = rig.run_silent_poll(read_of(decisions::ReadKind::COMMAND), false);
        const std::string where = "early=" + std::to_string(early_ms) + " start=" + std::to_string(start_ms) +
                                  " budget=" + std::to_string(budget_ms);
        EXPECT_EQ(rig.radio.get_send_count(), plan.max_attempts) << where << ": the plan is what the guard admits";
        // The first attempt always runs, so a start window longer than the budget is the one exception.
        if (start_ms < budget_ms) {
          EXPECT_LT(blocked_ms, budget_ms) << where;
        }
      }
    }
  }
}

TEST(Exchange, ChallengedWriteIsNeverCopiedMoreThanTodaysThreeTimesWhateverThePlan) {
  // The device challenged every copy, so it heard the write; a larger plan must not become up to
  // nine copies of a register write it already holds.
  PlanRig rig;
  queue_challenges(rig.radio, EXCHANGE_MAX_ATTEMPTS);
  const uint8_t payload[] = {0x01, 0x02, 0x03};
  IoFrame request{};
  ASSERT_TRUE(create_write_private(request, test::OWN_ID, test::DST_ID, /*low_power=*/false, payload,
                                   static_cast<uint8_t>(sizeof(payload))));
  ASSERT_EQ(expected_plan(rig.engine, rig.tuning, decisions::ReadKind::COMMAND, request).max_attempts,
            EXCHANGE_MAX_ATTEMPTS)
      << "sanity: the command's plan is the largest, gapless";
  IoFrame response{};

  EXPECT_EQ(rig.engine.send_and_receive(request, response, FREQ_CH2, read_of(decisions::ReadKind::COMMAND)),
            ExchangeOutcome::SUCCESS_UNCONFIRMED);

  EXPECT_EQ(rig.radio.get_send_count(), 2 * EXCHANGE_RETRY_COUNT) << "three requests, each with its 0x3D";
}

TEST(Exchange, ChallengedWriteAfterSilentAttemptsIsNotCopiedAgain) {
  // Three silent attempts spend today's three copies; the fourth is the first the device challenges,
  // and the exchange ends there instead of sending up to five more.
  ResendRig rig;
  rig.radio.queue_rx_hold_until_sent(4);  // nothing is heard until the fourth request is out
  rig.queue_challenge();
  const uint8_t payload[] = {0x01, 0x02, 0x03};
  IoFrame request{};
  ASSERT_TRUE(create_write_private(request, test::OWN_ID, test::DST_ID, /*low_power=*/false, payload,
                                   static_cast<uint8_t>(sizeof(payload))));
  // The command's plan has short early listens, which leave the budget room for further copies.
  ASSERT_EQ(expected_plan(rig.engine, rig.tuning, decisions::ReadKind::COMMAND, request).max_attempts,
            EXCHANGE_MAX_ATTEMPTS);
  IoFrame response{};

  EXPECT_EQ(rig.engine.send_and_receive(request, response, FREQ_CH2, read_of(decisions::ReadKind::COMMAND)),
            ExchangeOutcome::SUCCESS_UNCONFIRMED);

  EXPECT_EQ(rig.radio.get_send_count(), 5) << "four requests and the 0x3D answering the fourth's challenge";
}

TEST(Exchange, ExecuteChallengedWithoutAClosingReplyIsResentOnceWhateverThePlan) {
  ResendRig rig;
  rig.queue_challenge();
  rig.radio.queue_rx_hold_until_sent(3);
  rig.queue_challenge();
  ASSERT_EQ(expected_plan(rig.engine, rig.tuning, decisions::ReadKind::COMMAND, stop_request()).max_attempts,
            EXCHANGE_MAX_ATTEMPTS);
  IoFrame response{};

  EXPECT_EQ(rig.engine.send_and_receive(stop_request(), response, FREQ_CH2, read_of(decisions::ReadKind::COMMAND)),
            ExchangeOutcome::SUCCESS_UNCONFIRMED);

  EXPECT_EQ(rig.radio.get_send_count(), 4) << "request, 0x3D, one re-send, 0x3D; no third copy";
  const auto &waits = rig.radio.wait_timeouts();
  ASSERT_GE(waits.size(), 3u);
  EXPECT_EQ(waits[0], rig.tuning.exchange_early_response_wait_ms) << "the first attempt is an early one";
}

TEST(Exchange, FavouriteExecuteWhoseReplyIsLostAfterItWasChallengedIsNeverSentAgain) {
  // 'My' stops a moving cover: a copy after a challenged one could undo what the first started.
  ResendRig rig;
  rig.queue_challenge();  // the device challenges, we answer, its closing reply never comes
  IoFrame request{};
  create_execute_command(request, test::OWN_ID, test::DST_ID, false, CoverCommand::FAVORITE);

  EXPECT_EQ(rig.send(request), ExchangeOutcome::SUCCESS_UNCONFIRMED);

  EXPECT_EQ(rig.radio.get_send_count(), 2) << "one request and its 0x3D";
}

TEST(Exchange, FavouriteExecuteToASilentTargetKeepsTodaysThreeAttempts) {
  // Not repeat-safe, so the hub gives it no plan: the count-based exchange stays at three.
  PlanRig rig;
  IoFrame request{};
  create_execute_command(request, test::OWN_ID, test::DST_ID, false, CoverCommand::FAVORITE);
  IoFrame response{};

  EXPECT_EQ(rig.engine.send_and_receive(request, response, FREQ_CH2), ExchangeOutcome::FAILED);

  EXPECT_EQ(rig.radio.get_send_count(), EXCHANGE_RETRY_COUNT);
}

TEST(Exchange, ResendKeepsItsLongGapWhenThePlanHasNoGap) {
  ResendRig rig;
  rig.queue_challenge();
  rig.radio.queue_rx_hold_until_sent(3);
  rig.queue_challenge();
  rig.radio.queue_rx_hold_until_sent(4);
  rig.queue_final_reply();
  IoFrame response{};

  ASSERT_EQ(rig.engine.send_and_receive(stop_request(), response, FREQ_CH2, read_of(decisions::ReadKind::COMMAND)),
            ExchangeOutcome::SUCCESS_WITH_RESPONSE);

  const auto &t = rig.radio.send_times_ms();
  ASSERT_EQ(t.size(), 4u);
  EXPECT_EQ(t[2] - t[1],
            rig.tuning.exchange_response_wait_ms + UNCONFIRMED_EXECUTE_RESEND_DELAY_MS + CHANNEL_CHECK_TIME_MS)
      << "the re-send waits 750 ms in total whether or not the plan has a gap of its own, then its channel check";
}

TEST(Exchange, ResendToALowPowerTargetIsSentBehindTheWakeUpPreamble) {
  // A low-power EXECUTE accepted without a closing reply is still re-sent after the 750 ms gap; its
  // exchange is bounded by its attempt count, not the budget (ADR 0095).
  ResendRig rig;
  rig.radio.set_model_tx_airtime(true);
  rig.queue_challenge();
  rig.radio.queue_rx_hold_until_sent(3);
  rig.queue_challenge();
  IoFrame request{};
  create_execute_command(request, test::OWN_ID, test::DST_ID, /*low_power=*/true, CoverCommand::STOP);

  EXPECT_EQ(rig.send(request), ExchangeOutcome::SUCCESS_UNCONFIRMED);
  EXPECT_EQ(rig.radio.get_send_count(), 4) << "request, 0x3D, the re-send, 0x3D";
}

TEST(Exchange, ResendThatFitsTheBudgetIsSentOnceItsGapIsWaited) {
  // The re-send's own check (gap, transmission, window) admits it; the guard ahead of the re-sent try
  // must not then refuse it for a final-reply window the re-send's check leaves out, after the long
  // gap has already been waited. A mains target, since a low-power one is bounded by its attempt count
  // and skips the guard (ADR 0095). The budget sits between what the re-send needs and what it would
  // need with the final-reply window counted ahead of it.
  ResendRig rig;
  rig.radio.set_model_tx_airtime(true);
  rig.queue_challenge();
  rig.radio.queue_rx_hold_until_sent(3);
  rig.queue_challenge();
  IoFrame request = stop_request();
  // Try 1 ends with its 500 ms final wait at about 500 ms; the re-send then needs the 750 ms gap, its
  // transmission, the overhead and the 400 ms start window (about 1680 ms in all), and the final-reply
  // window would add about 520 ms more.
  rig.tuning.exchange_total_budget_ms = 2000;
  const uint32_t start = esphome::test_clock::peek_ms();

  EXPECT_EQ(rig.send(request), ExchangeOutcome::SUCCESS_UNCONFIRMED);

  EXPECT_EQ(rig.radio.get_send_count(), 4) << "request, 0x3D, the re-send, 0x3D";
  EXPECT_STRNE(rig.engine.get_debug().stage, "retry_budget_exhausted");
  EXPECT_LT(esphome::test_clock::peek_ms() - start, rig.tuning.exchange_total_budget_ms);
}

TEST(Exchange, UnconfirmedExecuteOnItsLastAllowedTryIsNotWaitedForAResend) {
  // The challenged try is the last one the exchange allows, so no re-send can follow: the long
  // re-send gap must not be waited and no re-send announced. The budget has room for a re-send,
  // so only the attempt count stops it.
  ResendRig rig;
  rig.queue_challenge();
  IoFrame response{};
  const uint32_t start = esphome::test_clock::peek_ms();

  EXPECT_EQ(rig.engine.send_and_receive(stop_request(), response, FREQ_CH2, /*max_tries=*/1),
            ExchangeOutcome::SUCCESS_UNCONFIRMED);

  EXPECT_EQ(rig.radio.get_send_count(), 2) << "request, 0x3D; no re-send on a one-try exchange";
  EXPECT_LT(esphome::test_clock::peek_ms() - start,
            rig.tuning.exchange_start_response_wait_ms + rig.tuning.exchange_response_wait_ms)
      << "the exchange ends with the final wait, not after the re-send gap";
}

namespace {

/// Make every channel read busy for every clear-channel check, and let a request go out anyway once
/// its access time is spent (lbt_force_when_busy), so each start frame waits out its whole access
/// time before it is sent.
void make_channels_always_busy_but_forced(PlanRig &rig) {
  rig.radio.set_rssi_default(0);
  rig.tuning.lbt_force_when_busy = true;
}

}  // namespace

TEST(Exchange, ALowPowerWakeUpExchangeKeepsAllItsAttemptsWhenABusyChannelDelaysEachOne) {
  // Size the budget so the three attempts fit it with 1 ms to spare when nothing delays them. Each
  // attempt then waits the low-power access time (600 ms) for a free channel, so the budget alone
  // would drop the second and third attempts. The attempt count bounds the exchange instead (ADR 0095).
  for (const auto kind : {decisions::ReadKind::SETTLING, decisions::ReadKind::COMMAND}) {
    PlanRig rig;
    make_channels_always_busy_but_forced(rig);
    const uint32_t gap_ms = kind == decisions::ReadKind::COMMAND ? EXCHANGE_RETRY_DELAY_MS : 0;
    const uint32_t window_ms =
        kind == decisions::ReadKind::COMMAND ? rig.tuning.exchange_start_response_wait_ms : LOW_POWER_RESPONSE_WAIT_MS;
    rig.tuning.exchange_total_budget_ms =
        silent_attempts_bound_ms(rig, LOW_POWER_SETTLING_ATTEMPTS, gap_ms, window_ms, true) + 1;

    const uint32_t blocked_ms = rig.run_silent_poll(read_of(kind), true);

    EXPECT_EQ(rig.radio.get_send_count(), static_cast<int>(LOW_POWER_SETTLING_ATTEMPTS))
        << "kind=" << static_cast<int>(kind);
    EXPECT_GT(blocked_ms, rig.tuning.exchange_total_budget_ms) << "the delays pushed the exchange past the budget";
    EXPECT_LT(blocked_ms,
              silent_attempts_bound_ms(rig, LOW_POWER_SETTLING_ATTEMPTS, gap_ms, window_ms, true) +
                  LOW_POWER_SETTLING_ATTEMPTS * (LOW_POWER_CHANNEL_ACCESS_BUDGET_MS + 3 * CHANNEL_CHECK_READINGS))
        << "no longer than the attempts and their channel access time";
  }
}

TEST(Exchange, ALowPowerExchangeRunsNoMoreThanItsAttemptsWhateverTheBudget) {
  // The count bound is also a ceiling: a huge budget adds no attempt to the planned ones (the count
  // form is capped at EXCHANGE_RETRY_COUNT, a settling read at the three wake-ups).
  PlanRig rig;
  rig.tuning.exchange_total_budget_ms = 12000;

  rig.run_silent_poll(ExchangeOptions{EXCHANGE_RETRY_COUNT}, true);
  EXPECT_EQ(rig.radio.get_send_count(), static_cast<int>(EXCHANGE_RETRY_COUNT));

  rig.radio.clear();
  rig.run_silent_poll(read_of(decisions::ReadKind::SETTLING), true);
  EXPECT_EQ(rig.radio.get_send_count(), static_cast<int>(LOW_POWER_SETTLING_ATTEMPTS));
}

TEST(Exchange, AUserBudgetOfOneSecondStillGivesALowPowerTargetThreeAttempts) {
  for (const bool counted : {false, true}) {
    PlanRig rig;
    rig.tuning.exchange_total_budget_ms = 1000;
    rig.run_silent_poll(counted ? ExchangeOptions{EXCHANGE_RETRY_COUNT} : read_of(decisions::ReadKind::SETTLING), true);
    EXPECT_EQ(rig.radio.get_send_count(), static_cast<int>(LOW_POWER_SETTLING_ATTEMPTS)) << "counted=" << counted;
  }
}

TEST(Exchange, AMainsTargetStillStopsAtTheBudgetOnABusyChannel) {
  PlanRig rig;
  make_channels_always_busy_but_forced(rig);
  rig.tuning.exchange_total_budget_ms = 1000;

  const uint32_t blocked_ms = rig.run_silent_poll(ExchangeOptions{EXCHANGE_RETRY_COUNT}, false);

  EXPECT_LT(rig.radio.get_send_count(), static_cast<int>(EXCHANGE_RETRY_COUNT)) << "the budget drops a later attempt";
  EXPECT_LT(blocked_ms, rig.tuning.exchange_total_budget_ms);
}

// ============================================================================
// A settling read of a low-power receiver in an unknown state leads with one short attempt
// (decisions::WakeBelief::UNKNOWN, ADR 0079). The exchange runs on a ManualClock with modelled air
// time, to a MockRadio that is silent unless a test queues a reply for a given send.
// ============================================================================

namespace {

/// A PlanRig whose target-evidence provider is scripted: all-zero stamps are a receiver the hub has
/// no news of.
struct ShortLeadRig : PlanRig {
  decisions::TargetEvidence evidence{};
  int evidence_spent{0};

  ShortLeadRig() {
    engine.set_target_evidence_provider([this](const uint8_t *, decisions::TargetEvidence &out) {
      out = evidence;
      return true;
    });
    engine.set_wake_evidence_spent_handler([this](const uint8_t *) { evidence_spent++; });
  }

  /// A status read of the low-power target.
  static IoFrame status_read() {
    IoFrame request{};
    create_get_status(request, test::OWN_ID, test::DST_ID, /*low_power=*/true);
    return request;
  }

  /// The target's answer to a status read.
  static RadioRxPacket status_reply() {
    const IoFrame reply = build_status_response(test::DST_ID, test::OWN_ID);
    uint8_t raw[64];
    RadioRxPacket pkt{};
    pkt.len = serialize(reply, raw, sizeof(raw));
    memcpy(pkt.data, raw, pkt.len);
    return pkt;
  }

  /// The transmit time the engine plans for the short lead of status_read().
  uint32_t short_tx_ms() const {
    IoFrame request = status_read();
    uint8_t raw[64];
    const uint8_t len = serialize(request, raw, sizeof(raw));
    return (io868_tx_air_time_us(mock_preamble_bytes(tuning.start_preamble_ms), len, SOFT_PHY_PREAMBLE_BITS_PER_BYTE) +
            999) /
           1000;
  }

  std::vector<uint16_t> preambles() const {
    std::vector<uint16_t> result;
    for (const auto &config : radio.get_tx_configs())
      result.push_back(config.preamble_len);
    return result;
  }

  /// The plan the request path builds for a settling read of status_read() that leads short.
  decisions::AttemptPlan leading_plan() const {
    return expected_plan(engine, tuning, decisions::ReadKind::SETTLING, status_read(), /*with_short_lead=*/true);
  }

  /// Run a settling read of status_read() against the rig's scripted radio.
  ExchangeOutcome settling_read(bool state_unknown = false, uint16_t preamble_override = 0) {
    IoFrame response{};
    decisions::AttemptRequest attempts = decisions::AttemptRequest::of_kind(decisions::ReadKind::SETTLING);
    attempts.state_unknown = state_unknown;
    return engine.send_and_receive(status_read(), response, FREQ_CH2,
                                   ExchangeOptions{attempts}.with_preamble(preamble_override));
  }
};

}  // namespace

TEST(ShortLead, AnUnknownLowPowerSettlingReadLeadsWithOneShortAttemptThenTheWakeUpAttempts) {
  ShortLeadRig rig;
  const auto plan = rig.leading_plan();
  ASSERT_TRUE(plan.short_lead);
  EXPECT_EQ(plan.max_attempts, LOW_POWER_SETTLING_ATTEMPTS + 1u) << "the three wake-up attempts are kept";
  EXPECT_TRUE(plan.bounded_by_attempts) << "three wake-ups behind the 2048-byte default outlast the budget";

  EXPECT_EQ(rig.settling_read(), ExchangeOutcome::FAILED);

  EXPECT_EQ(rig.preambles(), (std::vector<uint16_t>{mock_preamble_bytes(rig.tuning.start_preamble_ms),
                                                    MOCK_WAKE_PREAMBLE, MOCK_WAKE_PREAMBLE, MOCK_WAKE_PREAMBLE}));
  const auto &waits = rig.radio.wait_timeouts();
  ASSERT_EQ(waits.size(), plan.max_attempts);
  EXPECT_EQ(waits[0], plan.window_for_attempt(1, plan.max_attempts));
  EXPECT_EQ(waits[0], LOW_POWER_SHORT_LEAD_WINDOW_MS);
  EXPECT_EQ(waits[1], LOW_POWER_RESPONSE_WAIT_MS);
}

TEST(ShortLead, AReadMarkedStateUnknownLeadsShortDespiteARecentStoppedReport) {
  ShortLeadRig rig;
  // The target reported a second ago (a "stopped" report clears the moving evidence): unmarked, the
  // read believes it asleep and sends only wake-up attempts.
  rig.evidence.last_status_ms = esphome::test_clock::peek_ms() - 1000;

  EXPECT_EQ(rig.settling_read(), ExchangeOutcome::FAILED);
  EXPECT_EQ(rig.preambles(), (std::vector<uint16_t>{MOCK_WAKE_PREAMBLE, MOCK_WAKE_PREAMBLE, MOCK_WAKE_PREAMBLE}))
      << "unmarked, the read believes the target asleep and leads with no short attempt";

  rig.radio.clear();
  EXPECT_EQ(rig.settling_read(/*state_unknown=*/true), ExchangeOutcome::FAILED);
  EXPECT_EQ(rig.preambles(), (std::vector<uint16_t>{mock_preamble_bytes(rig.tuning.start_preamble_ms),
                                                    MOCK_WAKE_PREAMBLE, MOCK_WAKE_PREAMBLE, MOCK_WAKE_PREAMBLE}));
  EXPECT_EQ(rig.engine.get_debug().wake_belief, decisions::WakeBelief::UNKNOWN)
      << "the exchange resolves the belief the mark asks for";
}

TEST(ShortLead, FourAttemptsToASilentTargetRunPastTheDefaultBudgetAndNoFurther) {
  ShortLeadRig rig;
  const auto plan = rig.leading_plan();

  const uint32_t start = esphome::test_clock::peek_ms();
  EXPECT_EQ(rig.settling_read(), ExchangeOutcome::FAILED);
  const uint32_t blocked_ms = esphome::test_clock::peek_ms() - start;

  EXPECT_EQ(rig.radio.get_send_count(), static_cast<int>(plan.max_attempts))
      << "the guard admits every attempt the plan promised, the wake-up attempts included";
  EXPECT_EQ(rig.preambles(), (std::vector<uint16_t>{mock_preamble_bytes(rig.tuning.start_preamble_ms),
                                                    MOCK_WAKE_PREAMBLE, MOCK_WAKE_PREAMBLE, MOCK_WAKE_PREAMBLE}));
  const auto &waits = rig.radio.wait_timeouts();
  ASSERT_EQ(waits.size(), 4u);
  EXPECT_EQ(waits[0], LOW_POWER_SHORT_LEAD_WINDOW_MS) << "the lead listens briefly";
  EXPECT_EQ(waits[1], LOW_POWER_RESPONSE_WAIT_MS);
  EXPECT_GT(blocked_ms, rig.tuning.exchange_total_budget_ms) << "the wake-up attempts run past the budget";
  EXPECT_LT(blocked_ms,
            rig.short_tx_ms() + EXCHANGE_ATTEMPT_OVERHEAD_MS + LOW_POWER_SHORT_LEAD_WINDOW_MS +
                silent_attempts_bound_ms(rig, LOW_POWER_SETTLING_ATTEMPTS, 0, LOW_POWER_RESPONSE_WAIT_MS, true))
      << "the lead and the three wake-up attempts take no longer than they need";
}

TEST(ShortLead, AUserBudgetOfOneSecondStillRunsTheLeadAndEveryWakeUpAttempt) {
  // The budget admits none of the attempts after the first; the attempt count alone bounds the exchange
  // (ADR 0095).
  ShortLeadRig rig;
  rig.tuning.exchange_total_budget_ms = 1000;
  const auto plan = rig.leading_plan();
  ASSERT_EQ(plan.max_attempts, LOW_POWER_SETTLING_ATTEMPTS + 1u);

  const uint32_t start = esphome::test_clock::peek_ms();
  EXPECT_EQ(rig.settling_read(), ExchangeOutcome::FAILED);
  const uint32_t blocked_ms = esphome::test_clock::peek_ms() - start;

  EXPECT_EQ(rig.radio.get_send_count(), static_cast<int>(plan.max_attempts))
      << "no wake-up attempt is dropped for the lead";
  EXPECT_GT(blocked_ms, rig.tuning.exchange_total_budget_ms) << "the attempts run past the budget";
}

TEST(ShortLead, ACoverAnsweringOnlyTheShortPreambleIsReachedOnTheLead) {
  // A moving VELUX screen answers the short start frame and ignores the wake-up one.
  ShortLeadRig rig;
  rig.radio.queue_rx(ShortLeadRig::status_reply());

  EXPECT_EQ(rig.settling_read(), ExchangeOutcome::SUCCESS_WITH_RESPONSE);

  EXPECT_EQ(rig.radio.get_send_count(), 1);
  EXPECT_EQ(rig.preambles(), (std::vector<uint16_t>{mock_preamble_bytes(rig.tuning.start_preamble_ms)}));
}

TEST(ShortLead, ACoverAnsweringOnlyTheWakeUpPreambleIsReachedOnTheSecondAttempt) {
  // A resting receiver ignores the short start frame and hears the wake-up one.
  ShortLeadRig rig;
  rig.radio.queue_rx_hold_until_sent(2);
  rig.radio.queue_rx(ShortLeadRig::status_reply());

  EXPECT_EQ(rig.settling_read(), ExchangeOutcome::SUCCESS_WITH_RESPONSE);

  EXPECT_EQ(rig.radio.get_send_count(), 2);
  EXPECT_EQ(rig.preambles(),
            (std::vector<uint16_t>{mock_preamble_bytes(rig.tuning.start_preamble_ms), MOCK_WAKE_PREAMBLE}));
}

TEST(ShortLead, ASilentLeadSpendsNoEvidence) {
  // Only an exchange planned on AWAKE spends the moving evidence when it goes unanswered; the lead of an
  // unknown state has none to spend.
  ShortLeadRig rig;

  EXPECT_EQ(rig.settling_read(), ExchangeOutcome::FAILED);

  EXPECT_EQ(rig.evidence_spent, 0);
}

TEST(ShortLead, ASingleTryReadKeepsTheWakeUpPreamble) {
  // The lead never replaces the only try: a progress read goes to a receiver whose state is unknown
  // with the preamble that reaches a resting one.
  ShortLeadRig rig;
  IoFrame response{};

  EXPECT_EQ(rig.engine.send_and_receive(ShortLeadRig::status_read(), response, FREQ_CH2,
                                        read_of(decisions::ReadKind::PROGRESS)),
            ExchangeOutcome::FAILED);

  EXPECT_EQ(rig.preambles(), (std::vector<uint16_t>{MOCK_WAKE_PREAMBLE}));
  EXPECT_EQ(rig.engine.get_debug().wake_belief, decisions::WakeBelief::UNKNOWN);
}

TEST(ShortLead, ACountedStatusReadKeepsTheWakeUpPreambleOnEveryTry) {
  ShortLeadRig rig;
  IoFrame response{};

  EXPECT_EQ(rig.engine.send_and_receive(ShortLeadRig::status_read(), response, FREQ_CH2, EXCHANGE_RETRY_COUNT),
            ExchangeOutcome::FAILED);

  EXPECT_EQ(rig.preambles(), (std::vector<uint16_t>{MOCK_WAKE_PREAMBLE, MOCK_WAKE_PREAMBLE, MOCK_WAKE_PREAMBLE}))
      << "a caller that names only a number of tries gets no lead";
}

TEST(ShortLead, NewsOfTheReceiversStateLeavesNoLead) {
  ShortLeadRig rig;

  rig.evidence.last_status_ms = esphome::millis() - 1000;  // a status report a second ago: at rest, known
  EXPECT_EQ(rig.settling_read(), ExchangeOutcome::FAILED);
  EXPECT_EQ(rig.preambles(), (std::vector<uint16_t>{MOCK_WAKE_PREAMBLE, MOCK_WAKE_PREAMBLE, MOCK_WAKE_PREAMBLE}));

  rig.radio.clear();
  rig.evidence.last_status_ms = 0;
  rig.evidence.last_moving_evidence_ms = esphome::millis() - 1000;  // believed moving: the belief itself leads short
  EXPECT_EQ(rig.settling_read(), ExchangeOutcome::FAILED);
  EXPECT_EQ(rig.radio.get_send_count(), LOW_POWER_SETTLING_ATTEMPTS)
      << "the wake-up attempts only, none added as a lead";
  EXPECT_EQ(rig.radio.wait_timeouts().front(), LOW_POWER_RESPONSE_WAIT_MS);
}

TEST(ShortLead, AFixedPreambleOrACommandHasNoLead) {
  ShortLeadRig rig;

  EXPECT_EQ(rig.settling_read(/*state_unknown=*/false, MOCK_WAKE_PREAMBLE), ExchangeOutcome::FAILED);
  EXPECT_EQ(rig.preambles(), (std::vector<uint16_t>{MOCK_WAKE_PREAMBLE, MOCK_WAKE_PREAMBLE, MOCK_WAKE_PREAMBLE}))
      << "the read at a cover's predicted end fixes the wake-up preamble";

  rig.radio.clear();
  IoFrame move{};
  create_execute_position(move, test::OWN_ID, test::DST_ID, /*low_power=*/true, 40);
  IoFrame response{};
  EXPECT_EQ(rig.engine.send_and_receive(move, response, FREQ_CH2, read_of(decisions::ReadKind::COMMAND)),
            ExchangeOutcome::FAILED);
  EXPECT_EQ(rig.radio.get_send_count(), EXCHANGE_RETRY_COUNT);
  for (const auto &config : rig.radio.get_tx_configs())
    EXPECT_EQ(config.preamble_len, MOCK_WAKE_PREAMBLE);
}

TEST(ShortLead, ANonLowPowerTargetHasNoLead) {
  ShortLeadRig rig;
  IoFrame request{};
  create_get_status(request, test::OWN_ID, test::DST_ID, /*low_power=*/false);
  IoFrame response{};

  EXPECT_EQ(rig.engine.send_and_receive(request, response, FREQ_CH2, read_of(decisions::ReadKind::SETTLING)),
            ExchangeOutcome::FAILED);

  for (const auto &config : rig.radio.get_tx_configs())
    EXPECT_EQ(config.preamble_len, mock_preamble_bytes(rig.tuning.start_preamble_ms));
}

TEST(ShortLead, SwitchedOffWithTheWakeBeliefTuning) {
  ShortLeadRig rig;
  rig.tuning.low_power_wake_belief = false;

  EXPECT_EQ(rig.settling_read(), ExchangeOutcome::FAILED);

  EXPECT_EQ(rig.preambles(), (std::vector<uint16_t>{MOCK_WAKE_PREAMBLE, MOCK_WAKE_PREAMBLE, MOCK_WAKE_PREAMBLE}))
      << "with the belief off every try keeps the wake-up preamble";
}

// ============================================================================
// One options value; each exchange is planned once
// ============================================================================

TEST(ExchangeOptions, SettlingReadOfALowPowerTargetLooksTheTargetUpOnce) {
  WakeBeliefRig rig;
  IoFrame request{};
  create_get_status(request, test::OWN_ID, test::DST_ID, /*low_power=*/true);
  IoFrame response{};

  rig.engine.send_and_receive(request, response, FREQ_CH2,
                              decisions::AttemptRequest::of_kind(decisions::ReadKind::SETTLING));

  EXPECT_EQ(rig.provider_calls, 1) << "the preamble plan and the attempt plan share one lookup";
}

TEST(ExchangeOptions, EveryWayOfNamingTheAttemptsLooksTheTargetUpOnce) {
  const IoFrame request = low_power_position_request();
  IoFrame response{};
  {
    WakeBeliefRig rig;
    rig.engine.send_and_receive(request, response, FREQ_CH2, ExchangeOptions{2});
    EXPECT_EQ(rig.provider_calls, 1) << "a count";
  }
  {
    WakeBeliefRig rig;
    rig.engine.send_and_receive(request, response, FREQ_CH2,
                                decisions::AttemptRequest::of_kind(decisions::ReadKind::COMMAND));
    EXPECT_EQ(rig.provider_calls, 1) << "a command read kind";
  }
  {
    WakeBeliefRig rig;
    rig.engine.send_and_receive(request, response, FREQ_CH2);
    EXPECT_EQ(rig.provider_calls, 1) << "the default options";
  }
}

TEST(ExchangeOptions, OptionsKeepEachFieldWhenAnotherIsSet) {
  const uint8_t key[AES_KEY_SIZE] = {};
  const ExchangeOptions options = ExchangeOptions{3}.with_preamble(40).with_key(key);
  EXPECT_EQ(options.attempts.max_tries, 3);
  EXPECT_EQ(options.preamble_override, 40);
  EXPECT_EQ(options.key, key);
}

TEST(ExchangeOptions, OnlyAnExecuteLooksTheTargetUpWhenItsTryIsUnconfirmed) {
  {
    ResendRig rig;
    rig.queue_challenge();  // challenged, never closed
    EXPECT_EQ(rig.send(stop_request()), ExchangeOutcome::SUCCESS_UNCONFIRMED);
    EXPECT_EQ(rig.lookups, 1) << "an EXECUTE asks whether the device confirms";
  }
  {
    ResendRig rig;
    rig.queue_challenge();
    IoFrame request{};
    ASSERT_TRUE(create_subscribe_event(request, test::OWN_ID, test::DST_ID, EVENT_LOCAL_SENSOR, false));
    EXPECT_EQ(rig.send(request), ExchangeOutcome::SUCCESS_UNCONFIRMED);
    EXPECT_EQ(rig.lookups, 0) << "a subscription's answer does not depend on the target";
  }
  {
    ResendRig rig;
    rig.queue_challenge();
    IoFrame request{};
    create_get_status(request, test::OWN_ID, test::DST_ID, /*low_power=*/false);
    EXPECT_EQ(rig.send(request), ExchangeOutcome::SUCCESS_UNCONFIRMED);
    EXPECT_EQ(rig.lookups, 0);
  }
}

TEST(ExchangeOptions, UnconfirmedStatusReadKeepsItsPlanGapAndIsNotGivenTheLongResendGap) {
  ResendRig rig;
  rig.queue_challenge();
  IoFrame request{};
  create_get_status(request, test::OWN_ID, test::DST_ID, /*low_power=*/false);

  ASSERT_EQ(rig.send(request), ExchangeOutcome::SUCCESS_UNCONFIRMED);

  const auto &t = rig.radio.send_times_ms();
  ASSERT_GE(t.size(), 3u);
  EXPECT_EQ(t[2] - t[1], rig.tuning.exchange_response_wait_ms + EXCHANGE_RETRY_DELAY_MS + CHANNEL_CHECK_TIME_MS);
}

// ============================================================================
// Overheard-frame observer: a frame the first-response or final-response wait ignores as unrelated
// is reported to the attached OverheardFrameObserver, and the exchange itself runs as without it.
// ============================================================================

namespace {

/// What MockRadio's capture-lifecycle emulation stamps on every delivered frame.
constexpr int16_t OVERHEARD_TEST_RSSI_DBM = -55;

struct RecordingOverheardObserver : OverheardFrameObserver {
  std::vector<IoFrame> frames;
  std::vector<uint32_t> times;
  std::vector<int16_t> rssis;
  void on_overheard_frame(const IoFrame &frame, const RadioRxPacket &, int16_t rssi_dbm, uint32_t now) override {
    this->frames.push_back(frame);
    this->rssis.push_back(rssi_dbm);
    this->times.push_back(now);
  }
};

/// A wall remote's CLOSE press: a 1W frame to a device class.
IoFrame remote_press_frame() {
  const uint8_t remote[NODE_ID_SIZE] = {0x9D, 0x60, 0x85};
  const uint8_t roller_class[NODE_ID_SIZE] = {0x00, 0x01, 0xBF};
  const uint8_t close[3] = {0x00, 0x00, 0xC8};
  IoFrame f{};
  init_frame(f, /*is_2w=*/false, /*start=*/true, /*end=*/true, /*low_power=*/false);
  set_src(f, remote);
  set_dst(f, roller_class);
  set_cmd(f, CMD_EXECUTE, close, sizeof(close));
  return f;
}

/// What an exchange did, for comparing a run with and without an overheard frame.
struct ExchangeRun {
  ExchangeOutcome outcome;
  size_t send_count;
  std::vector<uint32_t> send_times_ms;
  uint32_t ended_ms;
  bool operator==(const ExchangeRun &o) const {
    return outcome == o.outcome && send_count == o.send_count && send_times_ms == o.send_times_ms &&
           ended_ms == o.ended_ms;
  }
};

/// One exchange where `noise` (if any) is queued at the given point: before the first reply (a direct
/// answer follows) or between the challenge and the final reply.
ExchangeRun run_with_noise(const IoFrame *noise, bool challenged, RecordingOverheardObserver &observer) {
  ResendRig rig;
  rig.radio.set_emulate_capture_lifecycle(true);
  rig.engine.set_overheard_frame_observer(&observer);
  if (challenged) {
    rig.queue_challenge();
    if (noise != nullptr)
      rig.queue(*noise);
    rig.queue_final_reply();
  } else {
    if (noise != nullptr)
      rig.queue(*noise);
    rig.queue(build_status_response(test::DST_ID, test::OWN_ID));
  }
  const ExchangeOutcome outcome = rig.send(stop_request());
  return {outcome, rig.radio.get_sent_data().size(), rig.radio.send_times_ms(), esphome::millis()};
}

}  // namespace

TEST(ExchangeOverheard, RemotePressInTheFirstResponseWaitReachesTheObserverOnce) {
  const IoFrame press = remote_press_frame();
  RecordingOverheardObserver quiet;
  RecordingOverheardObserver heard;
  const ExchangeRun without = run_with_noise(nullptr, false, quiet);
  const ExchangeRun with = run_with_noise(&press, false, heard);

  ASSERT_EQ(heard.frames.size(), 1u);
  EXPECT_EQ(heard.frames[0].cmd, CMD_EXECUTE);
  EXPECT_EQ(heard.rssis[0], OVERHEARD_TEST_RSSI_DBM) << "the observer gets the RSSI of the reception";
  EXPECT_EQ(memcmp(heard.frames[0].src, press.src, NODE_ID_SIZE), 0);
  EXPECT_TRUE(quiet.frames.empty());
  EXPECT_EQ(with.outcome, ExchangeOutcome::SUCCESS_WITH_RESPONSE);
  EXPECT_TRUE(with == without) << "the exchange's outcome, sends and timing do not depend on the frame";
}

TEST(ExchangeOverheard, RemotePressInTheFinalResponseWaitReachesTheObserverOnce) {
  const IoFrame press = remote_press_frame();
  RecordingOverheardObserver quiet;
  RecordingOverheardObserver heard;
  const ExchangeRun without = run_with_noise(nullptr, true, quiet);
  const ExchangeRun with = run_with_noise(&press, true, heard);

  ASSERT_EQ(heard.frames.size(), 1u);
  EXPECT_EQ(heard.frames[0].cmd, CMD_EXECUTE);
  EXPECT_TRUE(quiet.frames.empty());
  EXPECT_TRUE(with == without);
}

TEST(ExchangeOverheard, ForeignTwoWayFrameAlsoReachesTheObserverBecauseTheEngineDoesNotFilter) {
  const IoFrame foreign = build_status_response(test::FOREIGN_ID, test::OWN_ID);
  RecordingOverheardObserver heard;
  const ExchangeRun run = run_with_noise(&foreign, false, heard);

  ASSERT_EQ(heard.frames.size(), 1u);
  EXPECT_EQ(heard.frames[0].cmd, CMD_PRIVATE_RESP);
  EXPECT_EQ(run.outcome, ExchangeOutcome::SUCCESS_WITH_RESPONSE);
}

TEST(ExchangeOverheard, FramesTheExchangeAcceptsAreNotReported) {
  RecordingOverheardObserver heard;
  const ExchangeRun run = run_with_noise(nullptr, true, heard);

  EXPECT_EQ(run.outcome, ExchangeOutcome::SUCCESS_WITH_RESPONSE);
  EXPECT_TRUE(heard.frames.empty());
}

TEST(ExchangeOverheard, InboundAuthenticationWaitDoesNotReport) {
  // challenge_sender() serves a device-initiated command and the pairing key pull; its wait is
  // not an exchange wait, so a remote press heard there is not handed to the observer.
  ResendRig rig;
  RecordingOverheardObserver heard;
  rig.engine.set_overheard_frame_observer(&heard);
  rig.queue(remote_press_frame());

  IoFrame request{};
  create_execute_position(request, test::DST_ID, test::OWN_ID, false, 100);
  EXPECT_NE(rig.engine.challenge_sender(request, FREQ_CH2, test::TEST_SYSTEM_KEY),
            ExchangeEngine::ChallengeResult::VERIFIED);
  EXPECT_TRUE(heard.frames.empty());
}

// ============================================================================
// Channel access of a request (ADR 0096): a request from send_and_receive() is sent only onto a
// channel its check found free; on a busy result it moves to another channel after a random, growing
// back-off, and when its access time is spent the attempt ends as "channel busy" with nothing sent.
// ============================================================================

namespace {

/// A standalone engine on a manual clock whose random source is scripted: the values are consumed in
/// order, then 0. The draws of one attempt are the first channel, then per busy check the back-off and
/// the next channel.
struct AccessRig {
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
  RadioDriver *radio_ptr{&radio};
  TuningConfig tuning{};
  ExchangeEngine engine{&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning};
  std::deque<uint32_t> random_values;
  RecordingTransmitObserver transmitted;
  RecordingOverheardObserver heard;

  AccessRig() {
    engine.set_random_source([this]() -> uint32_t {
      if (random_values.empty())
        return 0;
      const uint32_t value = random_values.front();
      random_values.pop_front();
      return value;
    });
    engine.set_transmit_observer(&transmitted);
    engine.set_overheard_frame_observer(&heard);
  }

  /// A status read of a mains or low-power target.
  static IoFrame status_read(bool low_power) {
    IoFrame request{};
    create_get_status(request, test::OWN_ID, test::DST_ID, low_power);
    return request;
  }

  /// One exchange with @p options to a target that never answers.
  ExchangeOutcome run(bool low_power, const ExchangeOptions &options) {
    IoFrame response{};
    return engine.send_and_receive(status_read(low_power), response, FREQ_CH2, options);
  }

  /// The channel of every frame sent, in order.
  std::vector<uint32_t> sent_channels() const {
    std::vector<uint32_t> channels;
    for (const auto &config : radio.get_tx_configs())
      channels.push_back(config.freq_hz);
    return channels;
  }
};

using Channels = std::vector<uint32_t>;

}  // namespace

TEST(ChannelAccess, AFreeChannelIsSentOnAtOnceAfterTheFiveReadings) {
  AccessRig rig;
  rig.run(false, ExchangeOptions{1});

  EXPECT_EQ(rig.sent_channels(), (Channels{FREQ_CH2}));
  EXPECT_EQ(rig.radio.rssi_read_freqs().size(), static_cast<size_t>(CHANNEL_CHECK_READINGS))
      << "four readings from the engine and the radio's own right before the first symbol";
  EXPECT_EQ(rig.engine.counters().lbt_retries, 0u);
  EXPECT_EQ(rig.engine.counters().channel_busy_attempts, 0u);
  EXPECT_TRUE(rig.radio.freq_history().empty()) << "no retune: the radio was already on the chosen channel";
}

TEST(ChannelAccess, TheFirstChannelOfARequestIsTheRandomSourcesChoice) {
  for (const uint32_t random : {0u, 1u, 2u}) {
    AccessRig rig;
    rig.random_values = {random};
    rig.run(false, ExchangeOptions{1});
    ASSERT_EQ(rig.sent_channels().size(), 1u);
    EXPECT_EQ(rig.sent_channels()[0], decisions::REQUEST_CHANNELS[random]) << "random=" << random;
  }
}

TEST(ChannelAccess, EveryAttemptOfARequestTakesADifferentChannelThanTheOneBefore) {
  AccessRig rig;
  rig.random_values = {0, 0, 1};  // attempt 1: CH2; attempts 2 and 3 draw among the two channels they may take
  rig.run(false, ExchangeOptions{3});

  const Channels sent = rig.sent_channels();
  ASSERT_EQ(sent.size(), 3u);
  EXPECT_NE(sent[0], sent[1]);
  EXPECT_NE(sent[1], sent[2]);
  EXPECT_EQ(sent, (Channels{FREQ_CH2, FREQ_CH1, FREQ_CH3})) << "the draws 0, 0 and 1 (modulo the two candidates)";
}

TEST(ChannelAccess, ABusyChannelMovesTheRequestToAnotherOneAndTheReplyIsHeardThere) {
  AccessRig rig;
  rig.radio.set_channel_rssi(FREQ_CH2, -50);  // the first channel is busy, the others are free
  rig.radio.queue_rx_silence(1);              // what the back-off hears
  const IoFrame reply = build_status_response(test::DST_ID, test::OWN_ID);
  uint8_t raw[64];
  RadioRxPacket pkt{};
  pkt.len = serialize(reply, raw, sizeof(raw));
  memcpy(pkt.data, raw, pkt.len);
  pkt.freq_hz = FREQ_CH1;
  rig.radio.queue_rx(pkt);

  IoFrame response{};
  ASSERT_EQ(rig.engine.send_and_receive(AccessRig::status_read(false), response, FREQ_CH2, ExchangeOptions{1}),
            ExchangeOutcome::SUCCESS_WITH_RESPONSE);

  EXPECT_EQ(rig.sent_channels(), (Channels{FREQ_CH1})) << "nothing was sent onto the busy channel";
  EXPECT_EQ(rig.engine.counters().lbt_retries, 1u);
  EXPECT_EQ(rig.transmitted.lbt_defers, std::vector<int16_t>{-50});
  EXPECT_EQ(rig.radio.get_current_freq(), FREQ_CH1) << "the reply wait stayed on the channel the request used";
  EXPECT_EQ(rig.radio.freq_history(), (Channels{FREQ_CH1})) << "one retune, to the new channel, before the send";
}

TEST(ChannelAccess, TheBackOffIsRandomAndTheHubKeepsReceivingThroughIt) {
  AccessRig rig;
  rig.radio.set_channel_rssi(FREQ_CH2, -50);
  rig.random_values = {0, 2, 0};  // first channel CH2; back-off 1 + 2 % 4 = 3 ms; next channel CH1
  rig.run(false, ExchangeOptions{1});

  ASSERT_FALSE(rig.radio.wait_timeouts().empty());
  EXPECT_EQ(rig.radio.wait_timeouts()[0], 3u) << "the first wait of the exchange is the back-off, through listen()";
  EXPECT_EQ(rig.sent_channels(), (Channels{FREQ_CH1}));
}

TEST(ChannelAccess, ARemoteFrameHeardDuringTheBackOffReachesTheRemoteHandling) {
  AccessRig rig;
  rig.radio.set_emulate_capture_lifecycle(true);
  rig.radio.set_channel_rssi(FREQ_CH2, -50);  // the remote's burst keeps channel 2 busy
  rig.radio.queue_rx(to_rx_packet(remote_press_frame()));

  rig.run(false, ExchangeOptions{1});

  ASSERT_EQ(rig.heard.frames.size(), 1u) << "the press heard while backing off is handed over once";
  EXPECT_EQ(rig.heard.frames[0].cmd, CMD_EXECUTE);
  EXPECT_EQ(rig.heard.rssis[0], OVERHEARD_TEST_RSSI_DBM);
  EXPECT_EQ(rig.sent_channels(), (Channels{FREQ_CH1})) << "and the request still goes out, on another channel";
}

TEST(ChannelAccess, AChannelThatNeverClearsEndsTheAttemptAsBusyAndSendsNothing) {
  AccessRig rig;
  rig.radio.set_rssi_default(-50);  // every channel is busy
  const uint32_t begin_ms = esphome::test_clock::peek_ms();

  EXPECT_EQ(rig.run(false, ExchangeOptions{1}), ExchangeOutcome::FAILED);

  EXPECT_EQ(rig.radio.get_send_count(), 0) << "never onto a channel measured busy";
  EXPECT_EQ(rig.engine.counters().channel_busy_attempts, 1u);
  EXPECT_GT(rig.engine.counters().lbt_retries, 1u) << "it kept checking, on other channels, until the time was spent";
  EXPECT_STREQ(rig.engine.get_debug().stage, "channel_busy");
  const uint32_t spent_ms = esphome::test_clock::peek_ms() - begin_ms;
  EXPECT_GE(spent_ms, CHANNEL_ACCESS_BUDGET_MS);
  EXPECT_LE(spent_ms, CHANNEL_ACCESS_BUDGET_MS + CHANNEL_CHECK_TIME_MS + 2) << "and no longer than about 100 ms";
  const Channels &measured = rig.radio.rssi_read_freqs();
  EXPECT_GE(std::set<uint32_t>(measured.begin(), measured.end()).size(), 2u) << "it moved on to other channels";
}

TEST(ChannelAccess, ABusyAttemptCountsAsAFailedAttemptOfItsPlanAndTheNextOneStartsAfterTheGap) {
  AccessRig rig;
  rig.radio.set_rssi_default(-50);

  EXPECT_EQ(rig.run(false, ExchangeOptions{EXCHANGE_RETRY_COUNT}), ExchangeOutcome::FAILED);

  EXPECT_EQ(rig.engine.counters().channel_busy_attempts, static_cast<uint32_t>(EXCHANGE_RETRY_COUNT));
  EXPECT_EQ(rig.engine.counters().retransmits, static_cast<uint32_t>(EXCHANGE_RETRY_COUNT - 1));
  EXPECT_EQ(rig.radio.get_send_count(), 0);
}

TEST(ChannelAccess, ALowPowerRequestStaysOnChannel2AndWaitsLongerForItToClear) {
  AccessRig rig;
  rig.random_values = {1, 1, 1, 1, 1, 1, 1, 1};  // would move an ordinary request
  rig.radio.set_channel_rssi(FREQ_CH2, -50);
  const uint32_t begin_ms = esphome::test_clock::peek_ms();

  EXPECT_EQ(rig.run(true, ExchangeOptions{1}), ExchangeOutcome::FAILED);

  EXPECT_EQ(rig.radio.get_send_count(), 0);
  const Channels &measured = rig.radio.rssi_read_freqs();
  EXPECT_EQ(std::set<uint32_t>(measured.begin(), measured.end()), std::set<uint32_t>{FREQ_CH2});
  EXPECT_TRUE(rig.radio.freq_history().empty()) << "it never leaves channel 2";
  const uint32_t spent_ms = esphome::test_clock::peek_ms() - begin_ms;
  EXPECT_GE(spent_ms, LOW_POWER_CHANNEL_ACCESS_BUDGET_MS);
  EXPECT_LE(spent_ms, LOW_POWER_CHANNEL_ACCESS_BUDGET_MS + CHANNEL_CHECK_TIME_MS + 2);
}

TEST(ChannelAccess, ALowPowerRequestSendsOnChannel2OnceItClears) {
  AccessRig rig;
  rig.random_values = {1, 1, 1, 1};
  rig.run(true, ExchangeOptions{1});
  EXPECT_EQ(rig.sent_channels(), (Channels{FREQ_CH2}));
}

TEST(ChannelAccess, ABusyAttemptSpendsNoWakeEvidence) {
  AccessRig rig;
  decisions::TargetEvidence evidence{};
  evidence.last_moving_evidence_ms = esphome::millis() - 1000;  // believed moving: AWAKE
  rig.engine.set_target_evidence_provider([&evidence](const uint8_t *, decisions::TargetEvidence &out) {
    out = evidence;
    return true;
  });
  int spent = 0;
  rig.engine.set_wake_evidence_spent_handler([&spent](const uint8_t *) { spent++; });
  rig.radio.set_rssi_default(-50);

  rig.run(true, ExchangeOptions{1});
  EXPECT_EQ(rig.engine.counters().channel_busy_attempts, 1u);
  EXPECT_EQ(spent, 0) << "a request that was never sent says nothing about the receiver";

  rig.radio.set_rssi_default(-120);
  rig.run(true, ExchangeOptions{1});
  EXPECT_EQ(spent, 1) << "the same silence after a send onto a free channel does spend it";
}

TEST(ChannelAccess, AForcedSendGoesOntoTheQuietestChannelAndIsNoEvidenceWhenSilent) {
  AccessRig rig;
  rig.tuning.lbt_force_when_busy = true;
  rig.radio.set_channel_rssi(FREQ_CH2, -50);
  rig.radio.set_channel_rssi(FREQ_CH1, -70);
  rig.radio.set_channel_rssi(FREQ_CH3, -60);
  decisions::TargetEvidence evidence{};
  evidence.last_moving_evidence_ms = esphome::millis() - 1000;
  rig.engine.set_target_evidence_provider([&evidence](const uint8_t *, decisions::TargetEvidence &out) {
    out = evidence;
    return true;
  });
  int spent = 0;
  rig.engine.set_wake_evidence_spent_handler([&spent](const uint8_t *) { spent++; });

  rig.run(false, ExchangeOptions{1});

  EXPECT_EQ(rig.sent_channels(), (Channels{FREQ_CH1})) << "every channel read busy; CH1 was the quietest";
  ASSERT_EQ(rig.radio.get_tx_configs().size(), 1u);
  EXPECT_FALSE(rig.radio.get_tx_configs()[0].cca_threshold_dbm.has_value()) << "forced: no check";
  EXPECT_EQ(rig.engine.counters().channel_busy_attempts, 0u) << "it was sent, so it is not a busy attempt";
  EXPECT_EQ(spent, 0) << "a collision on a busy channel explains the silence";
}

TEST(ChannelAccess, AForcedLowPowerSendStaysOnChannel2) {
  AccessRig rig;
  rig.tuning.lbt_force_when_busy = true;
  rig.radio.set_channel_rssi(FREQ_CH2, -50);
  rig.radio.set_channel_rssi(FREQ_CH1, -110);
  rig.radio.set_channel_rssi(FREQ_CH3, -110);

  rig.run(true, ExchangeOptions{1});

  EXPECT_EQ(rig.sent_channels(), (Channels{FREQ_CH2}));
  const Channels &measured = rig.radio.rssi_read_freqs();
  EXPECT_EQ(std::set<uint32_t>(measured.begin(), measured.end()), std::set<uint32_t>{FREQ_CH2})
      << "the other channels are never measured: a low-power request may not use them";
}

TEST(ChannelAccess, ACallerThatNeedsItsChannelKeepsTheFixedChannelAccess) {
  AccessRig rig;
  rig.random_values = {1, 1, 1};
  rig.radio.set_rssi_default(-50);  // busy on every check

  rig.run(false, ExchangeOptions{1}.on_fixed_channel());

  EXPECT_EQ(rig.sent_channels(), (Channels{FREQ_CH2})) << "the caller's channel, whatever the random source says";
  EXPECT_EQ(rig.engine.counters().lbt_retries, static_cast<uint32_t>(LBT_MAX_RETRIES)) << "five checks, 5 ms apart";
  ASSERT_EQ(rig.radio.get_tx_configs().size(), 1u);
  EXPECT_FALSE(rig.radio.get_tx_configs()[0].cca_threshold_dbm.has_value()) << "then one send without a check";
}

TEST(ChannelAccess, TheHubInstallsItsRandomSourceSoARequestFollowsIt) {
  TestableComponent comp;
  MockRadio radio;
  wire_observer_rig(comp, radio);
  esphome::test_random::set(2);

  IoFrame request{};
  create_get_status(request, comp.node_id_, test::DST_ID, false);
  IoFrame response{};
  comp.send_and_receive_(request, response, FREQ_CH2, 1);
  esphome::test_random::set(0);

  ASSERT_EQ(radio.get_tx_configs().size(), 1u);
  EXPECT_EQ(radio.get_tx_configs()[0].freq_hz, FREQ_CH3);
}

TEST(ChannelAccess, ABusyExchangeFailsThroughTheHubWithTheChannelBusyStage) {
  TestableComponent comp;
  MockRadio radio;
  wire_observer_rig(comp, radio);
  radio.set_rssi_default(-50);

  IoFrame request{};
  create_get_status(request, comp.node_id_, test::DST_ID, false);
  IoFrame response{};
  EXPECT_EQ(comp.send_and_receive_(request, response, FREQ_CH2, 1), ExchangeOutcome::FAILED);

  EXPECT_EQ(radio.get_send_count(), 0);
  EXPECT_STREQ(comp.exchange_engine_.get_debug().stage, "channel_busy");
}

#pragma once

/// @file exchange_fixture.h
/// @brief Shared helpers of the exchange engine tests: a component with its protected exchange members opened, the
/// frame builders for the replies a device sends, the plan and options helpers, and the rigs and radios that more
/// than one exchange test file drives.

#include "hub_core.h"
#include "hub_exchange.h"
#include "proto_frame.h"
#include "proto_commands.h"
#include "proto_crypto.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"

#include <cstring>
#include <vector>

namespace exchange_test {

using namespace esphome::home_io_control;

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

inline IoFrame build_status_response(const uint8_t src[3], const uint8_t dst[3], uint8_t data_len = 6) {
  IoFrame f{};
  init_frame(f, true, false, true, false);  // 2W, end frame
  set_dst(f, dst);
  set_src(f, src);
  uint8_t payload[6] = {0};
  set_cmd(f, CMD_PRIVATE_RESP, payload, data_len);
  return f;
}

inline IoFrame build_challenge(const uint8_t src[3], const uint8_t dst[3], const uint8_t challenge[6]) {
  IoFrame f{};
  init_frame(f, true, false, false, false);
  set_dst(f, dst);
  set_src(f, src);
  set_cmd(f, CMD_CHALLENGE_REQ, challenge, 6);
  return f;
}

inline IoFrame build_error_response(const uint8_t src[3], const uint8_t dst[3], uint8_t result) {
  IoFrame f{};
  init_frame(f, true, false, true, false);
  set_dst(f, dst);
  set_src(f, src);
  set_cmd(f, CMD_ERROR_RESP, &result, 1);
  return f;
}

/// The plan the request path builds for a read of @p request: decisions::attempt_plan() with the
/// transmit time the engine estimates for the request's start preamble on FREQ_CH2. What a test
/// compares the attempts of a run against; the exchange itself is always started from a read kind.
/// @param with_short_lead True for a settling read of a low-power receiver in an unknown state.
inline decisions::AttemptPlan expected_plan(const ExchangeEngine &engine, const TuningConfig &tuning,
                                            decisions::ReadKind kind, const IoFrame &request,
                                            bool with_short_lead = false) {
  const bool low_power = (request.ctrl1 & CTRL1_LOW_POWER) != 0;
  return decisions::attempt_plan(kind, low_power, tuning, engine.band(),
                                 engine.tx_time_ms(request, FREQ_CH2, engine.request_preamble_for(request)), 0,
                                 with_short_lead);
}

/// Options that run an exchange as a read of @p kind.
inline ExchangeOptions read_of(decisions::ReadKind kind) { return decisions::AttemptRequest::of_kind(kind); }

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

inline IoFrame stop_request() {
  IoFrame f{};
  create_execute_command(f, test::OWN_ID, test::DST_ID, false, CoverCommand::STOP);
  return f;
}

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
inline IoFrame build_status_update_from_device(const uint8_t device_id[NODE_ID_SIZE],
                                               const uint8_t controller_id[NODE_ID_SIZE]) {
  IoFrame f{};
  init_frame(f, true, true, false, false);  // 2W, start
  set_dst(f, controller_id);
  set_src(f, device_id);
  uint8_t payload[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  set_cmd(f, CMD_STATUS_UPDATE, payload, sizeof(payload));
  return f;
}

inline TuningConfig make_broadcast_test_tuning() {
  TuningConfig tuning;
  tuning.pairing_discovery_wait_ms = 20;
  return tuning;
}

inline IoFrame build_spe_response(const uint8_t src[3], const uint8_t dst[3]) {
  IoFrame f{};
  init_frame(f, true, true, true, false);
  set_dst(f, dst);
  set_src(f, src);
  set_cmd(f, CMD_DISCOVER_SPE_RESP, nullptr, 0);
  return f;
}

inline RadioRxPacket to_rx_packet(const IoFrame &frame) {
  RadioRxPacket pkt{};
  uint8_t raw[64];
  pkt.len = serialize(frame, raw, sizeof(raw));
  memcpy(pkt.data, raw, pkt.len);
  return pkt;
}

inline IoFrame build_spe_request(const uint8_t own[3]) {
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

// The host clock stubs advance millis() one unit per call, and the engine reads it a couple of
// times between stamping the deadline and slicing it, so the first slice lands a tick or two under
// the configured window. Assert the window, not the exact tick. Still needed for legacy-mode
// callers (e.g. Listen_HoldNeverRetunesAndUsesTheWholeWindowAsOneTimeout below); a ManualClock
// caller can assert the exact value instead -- see StartFrameBudgetsTheStartResponseWindow.
inline void expect_window_near(uint32_t actual, uint32_t expected) {
  EXPECT_LE(actual, expected) << "slice must never exceed the configured window";
  EXPECT_GE(actual + 10u, expected) << "slice must be the configured window, not a different budget";
}

/// Queue a challenge for every try of a status poll: each try is challenged and then never closed.
inline void queue_challenges(MockRadio &radio, int tries) {
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

inline IoFrame low_power_position_request(bool low_power = true) {
  IoFrame request{};
  create_execute_position(request, test::OWN_ID, test::DST_ID, low_power, 40);
  return request;
}

inline IoFrame low_power_stop_request() {
  IoFrame request{};
  create_execute_command(request, test::OWN_ID, test::DST_ID, /*low_power=*/true, CoverCommand::STOP, false);
  return request;
}

using Preambles = std::vector<uint16_t>;

}  // namespace exchange_test

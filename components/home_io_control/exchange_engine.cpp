/// @file exchange_engine.cpp
/// @brief Authenticated exchange engine — outbound and inbound protocol flows.
/// @ingroup hioc_hub
///
/// Implements ExchangeEngine: the retry loop, challenge-response
/// authentication, final-response wait, listen-before-talk transmit, and
/// frequency-hopping. Debug-snapshot helpers are also here.

#include "exchange_engine.h"

#include "hub_decisions.h"
#include "log_frame.h"
#include "proto_commands.h"
#include "proto_constants.h"
#include "proto_crypto.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cinttypes>
#include <cstring>

namespace esphome {
namespace home_io_control {

static const char *const TAG = "home_io_control.exchange";

// ============================================================================
// Construction
// ============================================================================

ExchangeEngine::ExchangeEngine(RadioDriver **radio_ptr, const uint8_t *node_id, const uint8_t *system_key,
                               const TuningConfig *tuning)
    : radio_ptr_(radio_ptr), node_id_(node_id), system_key_(system_key), tuning_(tuning) {}

// ============================================================================
// Debug snapshot helpers
// ============================================================================

void ExchangeEngine::reset_debug(uint8_t request_cmd) {
  this->debug_ = DebugInfo{};
  this->debug_.request_cmd = request_cmd;
}

void ExchangeEngine::record_debug(const char *stage, uint8_t tries, bool saw_challenge) {
  this->debug_.stage = stage;
  this->debug_.tries = tries;
  this->debug_.saw_challenge = this->debug_.saw_challenge || saw_challenge;

  const RadioCaptureInfo &capture = (*this->radio_ptr_)->get_last_capture();
  // wait_for_packet() clears the radio's capture before it starts listening, so the *last* call
  // recorded here is always the final, timed-out wait — which by construction saw nothing. Keep the
  // first informative capture of the exchange instead: it's the only one that can distinguish "the
  // radio never detected a frame" from "it received one and this layer discarded it", which is the
  // question a failure report actually needs to answer.
  if (!capture.valid && this->debug_.capture_valid)
    return;
  this->debug_.capture_valid = capture.valid;
  this->debug_.capture_rx_done = capture.rx_done;
  this->debug_.capture_crc_error = capture.crc_error;
  this->debug_.capture_freq_hz = capture.freq_hz;
  this->debug_.capture_irq_status = capture.irq_status;
  this->debug_.capture_packet_status = capture.packet_status;
  this->debug_.capture_reported_len = capture.reported_len;
  this->debug_.capture_frame_len = capture.frame_len;
  this->debug_.capture_rssi_dbm = capture.rssi_dbm;
}

void ExchangeEngine::log_debug(const char *device_id) const {
  const auto &d = this->debug_;
  ESP_LOGW(TAG,
           "Exchange failed: device=%s cmd=%s(0x%02X) stage=%s tries=%u saw_challenge=%u cap_valid=%u cap_rx_done=%u "
           "cap_crc_err=%u cap_freq=%" PRIu32
           " cap_irq=0x%04X cap_pkt=0x%02X cap_reported_len=%u cap_frame_len=%u cap_rssi=%d",
           device_id, command_name(d.request_cmd), d.request_cmd, d.stage, d.tries, d.saw_challenge, d.capture_valid,
           d.capture_rx_done, d.capture_crc_error, d.capture_freq_hz, d.capture_irq_status, d.capture_packet_status,
           d.capture_reported_len, d.capture_frame_len, d.capture_rssi_dbm);
}

// ============================================================================
// Frequency hopping
// ============================================================================

void ExchangeEngine::reset_hop_timestamp() { this->last_hop_us_ = micros(); }

void ExchangeEngine::hop_frequency(uint32_t skip_freq) {
  RadioDriver *radio = *this->radio_ptr_;
  uint32_t next = radio->get_current_freq();
  // At most two iterations: the rotation cycles through all three channels and only one of them
  // can be skipped, so a channel that is not skip_freq is always one or two steps away.
  do {
    switch (next) {
      case FREQ_CH1:
        next = FREQ_CH2;
        break;
      case FREQ_CH3:
        next = FREQ_CH1;
        break;
      default:
        next = FREQ_CH3;
        break;
    }
  } while (next == skip_freq);
  radio->change_frequency(next);
  this->last_hop_us_ = micros();
}

void ExchangeEngine::maybe_hop() {
  if ((micros() - this->last_hop_us_) <= HOP_TIME_US)
    return;
  // A frame arriving on this channel outranks the dwell timer: change_frequency() retunes under a
  // running demodulator and, on the software-PHY chips, also clears the IRQ word and the DIO
  // latch, so hopping here destroys the frame rather than deferring it (issue #81).
  // last_hop_us_ is deliberately left alone: the dwell has already been served, so the hop should
  // happen on the very next pass once the reception clears, not a further HOP_TIME_US later.
  if ((*this->radio_ptr_)->reception_in_progress())
    return;
  this->hop_frequency();
}

// ============================================================================
// Transmit with LBT
// ============================================================================

bool ExchangeEngine::transmit_frame(const IoFrame &frame, uint32_t freq, uint16_t preamble) {
  RadioDriver *radio = *this->radio_ptr_;
  // FRAME_MAX_WIRE_SIZE, not FRAME_MAX_SIZE: a frame with an out-of-length MAC trailer
  // (IoFrame::has_mac, e.g. CMD_ONEWAY_ADD_CONTROLLER) serializes to more than
  // FRAME_MAX_SIZE/FRAME_MAX_DECLARED_SIZE bytes, and serialize() rejects a buffer too small to
  // hold its actual output rather than truncating into it.
  uint8_t buf[FRAME_MAX_WIRE_SIZE];
  uint8_t const len = serialize(frame, buf, sizeof(buf));
  if (len == 0) {
    ESP_LOGW(TAG, "tx: serialize_failed cmd=0x%02X", frame.cmd);
    return false;
  }
  for (uint8_t lbt = 0; lbt < this->tuning_->lbt_max_retries; lbt++) {
    int16_t const rssi = radio->read_rssi();
    if (rssi < this->tuning_->lbt_rssi_threshold_dbm)
      break;
    ESP_LOGD(TAG, "LBT: channel busy (RSSI %d dBm), retry %u/%u", rssi, lbt + 1, this->tuning_->lbt_max_retries);
    this->counters_.lbt_retries++;
    if (this->pairing_telemetry_ != nullptr)
      this->pairing_telemetry_->record_lbt_defer(rssi);
    delay(LBT_RETRY_DELAY_MS);
  }
  RadioTxConfig tx_config{};
  tx_config.freq_hz = freq;
  tx_config.preamble_len = preamble;
  if (!radio->send_packet(buf, len, tx_config)) {
    ESP_LOGW(TAG, "tx: send_failed cmd=0x%02X", frame.cmd);
    return false;
  }
  if (this->pairing_telemetry_ != nullptr)
    this->pairing_telemetry_->record_tx(frame.cmd);
  return true;
}

// ============================================================================
// Outbound exchange — main entry point
// ============================================================================

namespace {

/// @brief Map OutboundExchangeState to a short string for debug logging.
///
/// OutboundExchangeState is written at each step for debug capture but is
/// never read back for control-flow decisions — all branching is driven by
/// return values and disposition enums.
const char *outbound_stage_name(exchange::OutboundExchangeState state) {
  switch (state) {
    case exchange::OutboundExchangeState::IDLE:
      return "idle";
    case exchange::OutboundExchangeState::TX_REQUEST:
      return "tx_request";
    case exchange::OutboundExchangeState::WAIT_FIRST_RESPONSE:
      return "wait_first_response";
    case exchange::OutboundExchangeState::BUILD_AUTH_RESPONSE:
      return "build_auth_response";
    case exchange::OutboundExchangeState::TX_AUTH_RESPONSE:
      return "tx_auth_response";
    case exchange::OutboundExchangeState::WAIT_FINAL_RESPONSE:
      return "wait_final_response";
    case exchange::OutboundExchangeState::SUCCESS:
      return "success";
    case exchange::OutboundExchangeState::FAILED:
    default:
      return "failed";
  }
}

/// @brief Map InboundAuthState to a short string for debug logging.
const char *inbound_stage_name(exchange::InboundAuthState state) {
  switch (state) {
    case exchange::InboundAuthState::IDLE:
      return "idle";
    case exchange::InboundAuthState::TX_CHALLENGE:
      return "tx_challenge";
    case exchange::InboundAuthState::WAIT_CHALLENGE_RESPONSE:
      return "wait_challenge_response";
    case exchange::InboundAuthState::VERIFIED:
      return "verified";
    case exchange::InboundAuthState::FAILED:
    default:
      return "failed";
  }
}

/// Check if frame is a 0x3D challenge response.
bool frame_is_challenge_response(const IoFrame &frame) { return frame.cmd == CMD_CHALLENGE_RESP; }

/// Log a frame that arrived but could not be parsed. Printing it distinguishes "the radio heard
/// nothing" from "we heard something and rejected it" in a failure report — the two need opposite
/// fixes: a device that never transmitted needs a longer wait or a link check, while one that
/// transmits noise this layer can't decode needs the RX bandwidth or framing looked at. Redacted
/// through the same helper as every other frame log, so an unparsable frame can't leak key material
/// by being unrecognisable (see ADR 0011).
void log_unparsable_frame(const char *stage, int tries, const RadioRxPacket &packet) {
  // The *fact* that a frame failed to parse stays unconditional — it is a real fault worth
  // surfacing to someone who never enables a debug flag. The raw bytes only help someone already
  // debugging the PHY, and a noisy channel can produce several of these a minute, so they sit
  // behind the frame-log flag with the rest of that detail.
  ESP_LOGW(TAG, "%s try=%d: %u bytes did not parse as a frame on %" PRIu32 " Hz", stage, tries, packet.len,
           packet.freq_hz);
#ifdef IOHOME_FRAME_LOG
  char hex[FRAME_LOG_HEX_BUFFER_SIZE];
  render_frame_hex_redacted(packet.data, packet.len, hex, sizeof(hex));
  ESP_LOGD(TAG, "  raw: %s", hex);
#endif
}

/// Log an exchanged frame with context (stage, try index, length).
void log_exchange_frame(const char *stage, int tries, const IoFrame &frame, uint8_t len) {
  ESP_LOGD(TAG, "%s try=%d cmd=0x%02X src=%02X%02X%02X dst=%02X%02X%02X len=%u", stage, tries, frame.cmd, frame.src[0],
           frame.src[1], frame.src[2], frame.dst[0], frame.dst[1], frame.dst[2], len);
}

/// Determine if a candidate frame is a valid final response for the request.
bool is_valid_final_response(const IoFrame &candidate, const IoFrame &request) {
  return decisions::classify_exchange_final_response(request, candidate) ==
         decisions::ExchangeFinalResponseDisposition::ACCEPT;
}

}  // namespace

ExchangeOutcome ExchangeEngine::send_and_receive(const IoFrame &request, IoFrame &response, uint32_t freq) {
  this->reset_debug(request.cmd);
  const uint16_t request_preamble = this->request_preamble_for_(request);
  const uint32_t exchange_begin_ms = millis();
  bool accepted_without_reply = false;

  for (uint8_t tries = 0; tries < EXCHANGE_RETRY_COUNT; tries++) {
    exchange::OutboundExchangeContext context;
    context.try_index = tries + 1;
    context.exchange_start_ms = millis();
    context.wait_ms =
        is_start(request) ? this->tuning_->exchange_start_response_wait_ms : this->tuning_->exchange_response_wait_ms;
    context.state = exchange::OutboundExchangeState::TX_REQUEST;

    if (tries > 0) {
      // The retry count is a maximum, not a promise: don't start a try the exchange has no budget
      // left for. See EXCHANGE_TOTAL_BUDGET_MS -- this is what keeps a failing command from
      // blocking the ESPHome loop for the full retries x response-window product.
      if (millis() - exchange_begin_ms >= this->tuning_->exchange_total_budget_ms) {
        this->record_debug("retry_budget_exhausted", tries, false);
        ESP_LOGI(TAG, "Exchange budget exhausted after %u tries for cmd=%s(0x%02X) (%" PRIu32 " of %u ms)", tries,
                 command_name(request.cmd), request.cmd, millis() - exchange_begin_ms,
                 this->tuning_->exchange_total_budget_ms);
        break;
      }
      App.feed_wdt();
      delay(EXCHANGE_RETRY_DELAY_MS);
      this->counters_.retransmits++;
    }

    if (!this->transmit_request_(request, freq, request_preamble, context))
      continue;

    context.state = exchange::OutboundExchangeState::WAIT_FIRST_RESPONSE;
    this->record_debug(outbound_stage_name(context.state), context.try_index, false);
    auto first_disp = this->wait_for_first_response_(request, context);
    if (first_disp == decisions::ExchangeFirstResponseDisposition::IGNORE_UNRELATED)
      continue;
    if (first_disp == decisions::ExchangeFirstResponseDisposition::COMPLETE_DIRECT) {
      context.state = exchange::OutboundExchangeState::SUCCESS;
      this->record_debug("success_direct", context.try_index, false);
      response = context.rx;
      return ExchangeOutcome::SUCCESS_WITH_RESPONSE;
    }

    if (!this->handle_authentication_(request, freq, context))
      continue;

    context.state = exchange::OutboundExchangeState::WAIT_FINAL_RESPONSE;
    this->record_debug(outbound_stage_name(context.state), context.try_index, true);
    auto final_disp = this->wait_for_final_response_(request, context);
    if (final_disp != decisions::ExchangeFinalResponseDisposition::ACCEPT) {
      // The device challenged us and accepted our answer, so it demonstrably received the request.
      // Not every device closes the exchange with a synchronous reply (see ExchangeOutcome). A
      // retry is safe only for a request with no side effect to repeat — CMD_EXECUTE is already
      // acting on the first copy, so it stops here; everything else spends its full retry budget.
      context.state = exchange::OutboundExchangeState::SUCCESS;
      this->record_debug("success_auth_unconfirmed", context.try_index, true);
      accepted_without_reply = true;
      if (!decisions::retry_after_unconfirmed_accept_is_safe(request.cmd))
        return ExchangeOutcome::SUCCESS_UNCONFIRMED;
      continue;
    }

    context.state = exchange::OutboundExchangeState::SUCCESS;
    this->record_debug("success_auth", context.try_index, true);
    response = context.rx;
    return ExchangeOutcome::SUCCESS_WITH_RESPONSE;
  }

  // An exchange that authenticated on some try but never got a reply is not the same as one the
  // device never answered at all: callers that only need "the request landed" can act on it, and
  // callers that need the payload still cannot.
  return accepted_without_reply ? ExchangeOutcome::SUCCESS_UNCONFIRMED : ExchangeOutcome::FAILED;
}

// ============================================================================
// Outbound exchange step helpers
// ============================================================================

uint16_t ExchangeEngine::request_preamble_for_(const IoFrame &request) const {
  // Gate on is_start() first: several device-role / continuation builders (key transfer,
  // status-update response) set CTRL1_LOW_POWER on a non-start frame, and those must keep the
  // short response preamble, not be lengthened.
  if (!is_start(request))
    return (*this->radio_ptr_)->response_preamble();
  return (request.ctrl1 & CTRL1_LOW_POWER) != 0 ? LONG_PREAMBLE : this->tuning_->normal_start_preamble;
}

bool ExchangeEngine::transmit_request_(const IoFrame &request, uint32_t freq, uint16_t preamble,
                                       exchange::OutboundExchangeContext &ctx) {
  if (!this->transmit_frame(request, freq, preamble)) {
    ctx.state = exchange::OutboundExchangeState::FAILED;
    this->record_debug("tx_request_failed", ctx.try_index, false);
    return false;
  }
  return true;
}

decisions::ExchangeFirstResponseDisposition ExchangeEngine::wait_for_first_response_(
    const IoFrame &request, exchange::OutboundExchangeContext &ctx) {
  ListenSpec spec;
  spec.window_ms = ctx.wait_ms;
  // A unicast reply comes back on the channel the request went out on: 0 of 300 unicast RX
  // events (CHALLENGE_REQ + PRIVATE_RESP, 50 cycles each on SX1276/SX1262/LR1121) arrived off
  // the request channel. Holding the channel needs no dwell and no hop-after-timeout guard —
  // there is nowhere else a reply could come from.
  spec.policy = ListenPolicy::HOLD_REQUEST_CHANNEL;

  auto disp = decisions::ExchangeFirstResponseDisposition::IGNORE_UNRELATED;
  RadioRxPacket packet{};
  auto outcome = this->listen(spec, packet, ctx.rx, [&](const IoFrame *parsed, const RadioRxPacket &pkt) {
    if (parsed == nullptr) {
      this->record_debug("first_parse_fail", ctx.try_index, false);
      this->counters_.parse_failures++;
      log_unparsable_frame("Unparsable first response", ctx.try_index, pkt);
      return ReplyDisposition::IGNORE;
    }
    disp = decisions::classify_exchange_first_response(request, *parsed);
    if (disp == decisions::ExchangeFirstResponseDisposition::IGNORE_UNRELATED) {
      this->record_debug("first_wrong_exchange", ctx.try_index, false);
      log_exchange_frame("Ignored first response", ctx.try_index, *parsed, pkt.len);
      return ReplyDisposition::IGNORE;
    }
    ctx.first_response_ms = millis();
    return ReplyDisposition::ACCEPT;
  });

  if (outcome == ListenOutcome::ACCEPTED)
    return disp;

  ctx.state = exchange::OutboundExchangeState::FAILED;
  this->record_debug("wait_first_timeout", ctx.try_index, false);
  ESP_LOGI(TAG, "Try %d ended: no first response for cmd=%s(0x%02X) within %" PRIu32 " ms", ctx.try_index,
           command_name(request.cmd), request.cmd, ctx.wait_ms);
  return decisions::ExchangeFirstResponseDisposition::IGNORE_UNRELATED;
}

bool ExchangeEngine::handle_authentication_(const IoFrame &request, uint32_t freq,
                                            exchange::OutboundExchangeContext &ctx) {
  ctx.saw_challenge = true;
  ctx.state = exchange::OutboundExchangeState::BUILD_AUTH_RESPONSE;
  this->record_debug(outbound_stage_name(ctx.state), ctx.try_index, true);

  IoFrame auth_resp;
  if (!create_challenge_resp(auth_resp, request.dst, this->node_id_, ctx.rx.data, request, this->system_key_)) {
    ctx.state = exchange::OutboundExchangeState::FAILED;
    this->record_debug("auth_build_failed", ctx.try_index, true);
    return false;
  }

  // No challenge bytes here: the raw 0x3C payload plus the 0x3D response it provokes is a
  // known-plaintext/known-ciphertext pair under the system key (see redaction.h). The generic
  // frame-log helpers (log_frame()/log_component_capture()) already mask both commands.
  ESP_LOGI(TAG, "Auth challenge try=%d wait_ms=%" PRIu32 " req_cmd=0x%02X req_len=%u", ctx.try_index,
           ctx.first_response_ms - ctx.exchange_start_ms, request.cmd, request.data_len);

  ctx.state = exchange::OutboundExchangeState::TX_AUTH_RESPONSE;
  this->record_debug(outbound_stage_name(ctx.state), ctx.try_index, true);
  if (!this->transmit_frame(auth_resp, freq, (*this->radio_ptr_)->response_preamble())) {
    ctx.state = exchange::OutboundExchangeState::FAILED;
    this->record_debug("tx_auth_failed", ctx.try_index, true);
    return false;
  }
  this->counters_.challenge_round_trips++;
  return true;
}

decisions::ExchangeFinalResponseDisposition ExchangeEngine::wait_for_final_response_(
    const IoFrame &request, exchange::OutboundExchangeContext &ctx) {
  // Same budget as any other continuation frame — RESPONSE_AUTH_WAIT_MS was always an alias for
  // RESPONSE_WAIT_MS, so the two share one knob rather than inventing a third.
  const uint32_t auth_wait_ms = this->tuning_->exchange_response_wait_ms;

  ListenSpec spec;
  spec.window_ms = auth_wait_ms;
  // Same reasoning as wait_for_first_response_(): a unicast reply comes back on the request
  // channel (0 of 300 unicast RX events measured off-channel across all three chips), so holding
  // the channel for the whole wait is strictly correct and needs no dwell.
  spec.policy = ListenPolicy::HOLD_REQUEST_CHANNEL;

  RadioRxPacket packet{};
  auto outcome = this->listen(spec, packet, ctx.rx, [&](const IoFrame *parsed, const RadioRxPacket &pkt) {
    if (parsed == nullptr) {
      this->record_debug("final_parse_fail", ctx.try_index, true);
      this->counters_.parse_failures++;
      log_unparsable_frame("Unparsable final response", ctx.try_index, pkt);
      return ReplyDisposition::IGNORE;
    }
    if (is_valid_final_response(*parsed, request))
      return ReplyDisposition::ACCEPT;
    this->record_debug("final_wrong_exchange", ctx.try_index, true);
    log_exchange_frame("Ignored final response", ctx.try_index, *parsed, pkt.len);
    return ReplyDisposition::IGNORE;
  });

  if (outcome == ListenOutcome::ACCEPTED)
    return decisions::ExchangeFinalResponseDisposition::ACCEPT;

  ctx.state = exchange::OutboundExchangeState::FAILED;
  this->record_debug("wait_final_timeout", ctx.try_index, true);
  ESP_LOGI(TAG, "Try %d ended: no matching final response for cmd=%s(0x%02X) within %" PRIu32 " ms", ctx.try_index,
           command_name(request.cmd), request.cmd, auth_wait_ms);
  return decisions::ExchangeFinalResponseDisposition::IGNORE_UNRELATED;
}

// ============================================================================
// Broadcast roll-call
// ============================================================================

uint8_t ExchangeEngine::collect_broadcast_responses(const IoFrame &request, uint32_t freq, uint8_t expected_cmd,
                                                    uint32_t window_ms, const BroadcastReplyHandler &on_reply) {
  this->reset_debug(request.cmd);

  if (!this->transmit_frame(request, freq, this->request_preamble_for_(request))) {
    this->record_debug("broadcast_tx_failed", 1, false);
    return 0;
  }

  RadioDriver *radio = *this->radio_ptr_;
  uint8_t count = 0;

  ListenSpec spec;
  spec.window_ms = window_ms;
  // A roll-call reply almost never returns on the channel that asked for it (see ListenPolicy's
  // own doc comment for why), so dwelling there is wasted listening time: with three channels
  // split evenly across the window, one of them going unused for replies costs a third of it.
  spec.policy = ListenPolicy::ROTATE_SKIPPING_REQUEST;
  spec.request_freq = freq;
  // dwell_ms is left at 0: no measured reason to dwell differently from discovery, so listen()
  // asks the driver via hop_dwell_ms() instead of hardcoding a value here.
  // A reception proves this channel carries responders and replies arrive spread across the whole
  // window, so staying on it costs nothing; hopping away would only shrink the time spent where
  // responders already are.
  spec.hop_after_ignored_frame = false;
  spec.linger_on_preamble = true;
  spec.linger_dwell_ms = PREAMBLE_LINGER_DWELL_MS;

  RadioRxPacket packet{};
  IoFrame frame{};
  this->listen(spec, packet, frame, [&](const IoFrame *parsed, const RadioRxPacket & /*packet*/) {
    if (parsed == nullptr || parsed->cmd != expected_cmd || memcmp(parsed->dst, this->node_id_, NODE_ID_SIZE) != 0)
      return ReplyDisposition::IGNORE;

    on_reply(*parsed, radio->get_last_capture().rssi_dbm);
    if (count < UINT8_MAX)
      ++count;
    // Collection always runs to the deadline: a roll-call has no single "the" reply, so nothing
    // this loop can see is ever a reason to stop early.
    return ReplyDisposition::IGNORE;
  });

  this->record_debug("broadcast_collect_done", 1, false);
  return count;
}

// ============================================================================
// Shared listen primitive
//
// The one listen loop every radio wait in this project runs through: send_and_receive()'s
// first/final-response waits, collect_broadcast_responses(), and PairingEngine's discovery/
// key-transfer/key-confirm waits all call listen() below with a ListenSpec that picks one of
// the three ListenPolicy values (hold the request channel, rotate all three, or rotate skipping
// the request channel). Per-loop behavior — what counts as a match, what aborts, what gets
// logged — lives entirely in the caller's ReplyHandler; this function owns only the
// slice/hop/deadline mechanics common to all of them.
// ============================================================================

namespace {

/// Parse one received packet, hand it to `on_frame`, and translate an ACCEPT/ABORT disposition
/// into `outcome`. Factored out of listen()'s two reception sites purely to keep that function's
/// cognitive complexity under the clang-tidy threshold — no behavior beyond the parse/dispatch.
/// @return true if the listen should stop (ACCEPT or ABORT was returned); false to keep waiting.
bool dispatch_received_packet(const ReplyHandler &on_frame, const RadioRxPacket &packet, IoFrame &frame,
                              ListenOutcome &outcome) {
  const bool parsed = parse(packet.data, packet.len, frame);
  switch (on_frame(parsed ? &frame : nullptr, packet)) {
    case ReplyDisposition::ACCEPT:
      outcome = ListenOutcome::ACCEPTED;
      return true;
    case ReplyDisposition::ABORT:
      outcome = ListenOutcome::ABORTED;
      return true;
    case ReplyDisposition::IGNORE:
      return false;
  }
  return false;
}

/// A frame is arriving: hopping now would cut it off mid-reception. Both halves are live on every
/// current chip.
bool preamble_or_sync_incoming(RadioDriver *radio, const ListenSpec &spec) {
  return spec.linger_on_preamble && (radio->is_preamble_detected() || radio->is_sync_detected());
}

/// Per-channel dwell for a rotating listen: `spec.dwell_ms` if the caller set one, otherwise the
/// driver's own answer to "how long must this radio sit on a channel after retuning before it can
/// hear anything at all" (see RadioDriver::hop_dwell_ms()). Unused (returns 0) for a holding
/// listen — HOLD never slices, so it never asks. Factored out of listen() purely to avoid a
/// nested conditional operator and keep that function's cognitive complexity under the clang-tidy
/// threshold — no behavior beyond the two-way fallback.
uint32_t resolve_dwell_ms(const ListenSpec &spec, bool rotating, RadioDriver *radio, const TuningConfig &tuning) {
  if (!rotating)
    return 0;
  if (spec.dwell_ms != 0)
    return spec.dwell_ms;
  return radio->hop_dwell_ms(tuning);
}

}  // namespace

void ExchangeEngine::listen_hop_(uint32_t skip, const ListenSpec &spec) {
  this->hop_frequency(skip);
  if (spec.on_hop)
    spec.on_hop();
}

ListenOutcome ExchangeEngine::listen(const ListenSpec &spec, RadioRxPacket &packet, IoFrame &frame,
                                     const ReplyHandler &on_frame) {
  RadioDriver *radio = *this->radio_ptr_;
  const uint32_t deadline = millis() + spec.window_ms;
  const bool rotating = spec.policy != ListenPolicy::HOLD_REQUEST_CHANNEL;
  const uint32_t skip = spec.policy == ListenPolicy::ROTATE_SKIPPING_REQUEST ? spec.request_freq : 0;
  // 0 means "ask the driver": neither rotating call site in this project has a measured reason to
  // dwell differently from the chip's own retune-cost answer (RadioDriver::hop_dwell_ms()), so
  // both leave spec.dwell_ms at 0 and share one chip-specific knob instead of inventing a second.
  const uint32_t dwell = resolve_dwell_ms(spec, rotating, radio, *this->tuning_);

  // A broadcast reply almost never returns on the requesting channel (see ListenPolicy's own doc
  // comment for why), so a skipping listen leaves that channel before its first dwell rather than
  // spending one there.
  if (spec.policy == ListenPolicy::ROTATE_SKIPPING_REQUEST)
    this->listen_hop_(skip, spec);

  while ((int32_t) (deadline - millis()) > 0) {
    const uint32_t remaining = deadline - millis();
    // A holding listen waits the whole remaining window in one call: it has nothing to do between
    // slices, wait_for_packet() feeds the watchdog while it blocks, and every expired slice costs
    // an RX re-arm. Only a rotating listen needs a per-channel dwell.
    const uint32_t slice = rotating ? std::min(remaining, dwell) : remaining;

    if (radio->wait_for_packet(packet, slice)) {
      ListenOutcome outcome = ListenOutcome::TIMED_OUT;
      if (dispatch_received_packet(on_frame, packet, frame, outcome))
        return outcome;
      // The roll-call leaves this false because a reception proves responders are on this
      // channel (see the field doc in hub_exchange.h); discovery is the only listen that hops
      // after an ignored frame.
      if (rotating && spec.hop_after_ignored_frame && (int32_t) (deadline - millis()) > 0 &&
          !preamble_or_sync_incoming(radio, spec))
        this->listen_hop_(skip, spec);
      continue;
    }

    if ((int32_t) (deadline - millis()) <= 0)
      break;
    if (!rotating)
      continue;  // HOLD: an early false is a failed reception, not a timeout.
    if (!preamble_or_sync_incoming(radio, spec)) {
      this->listen_hop_(skip, spec);
      continue;
    }
    // Preamble/sync seen at the dwell boundary: extend by linger_dwell_ms and give the frame its
    // air time instead of hopping — a short extension wait, not another full per-channel dwell.
    const uint32_t ext = std::min((uint32_t) (deadline - millis()), spec.linger_dwell_ms);
    ListenOutcome outcome = ListenOutcome::TIMED_OUT;
    if (radio->wait_for_packet(packet, ext) && dispatch_received_packet(on_frame, packet, frame, outcome))
      return outcome;
  }
  return ListenOutcome::TIMED_OUT;
}

// ============================================================================
// Inbound authentication
// ============================================================================

bool ExchangeEngine::authenticate_request(const IoFrame &request, uint32_t freq) {
  RadioDriver *radio = *this->radio_ptr_;
  exchange::InboundAuthContext context;
  context.state = exchange::InboundAuthState::TX_CHALLENGE;
  this->record_debug(inbound_stage_name(context.state), 1, true);

  if (!create_challenge_req(context.challenge, request.src, this->node_id_)) {
    context.state = exchange::InboundAuthState::FAILED;
    this->record_debug(inbound_stage_name(context.state), 1, true);
    return false;
  }
  if (!this->transmit_frame(context.challenge, freq, SHORT_PREAMBLE)) {
    context.state = exchange::InboundAuthState::FAILED;
    this->record_debug(inbound_stage_name(context.state), 1, true);
    return false;
  }

  context.state = exchange::InboundAuthState::WAIT_CHALLENGE_RESPONSE;
  this->record_debug(inbound_stage_name(context.state), 1, true);

  RadioRxPacket packet{};
  if (!radio->wait_for_packet(packet, this->tuning_->exchange_response_wait_ms)) {
    context.state = exchange::InboundAuthState::FAILED;
    this->record_debug(inbound_stage_name(context.state), 1, true);
    return false;
  }

  IoFrame rx;
  if (!parse(packet.data, packet.len, rx) || !frame_is_challenge_response(rx)) {
    context.state = exchange::InboundAuthState::FAILED;
    this->record_debug(inbound_stage_name(context.state), 1, true);
    return false;
  }

  // Transcript is the *device's* own frame (cmd + data), not our challenge — the challenged
  // party authenticates what it said. Long assumed by symmetry with our outbound direction;
  // confirmed against real hardware bytes by the device-side 0x3D in
  // tests/corpus/captures/pairing/velux_kux100_pairing_full.yaml.
  uint8_t frame_data[FRAME_MAX_SIZE];
  frame_data[0] = request.cmd;
  memcpy(frame_data + 1, request.data, request.data_len);
  if (!crypto::verify_hmac(frame_data, request.data_len + 1, rx.data, context.challenge.data, this->system_key_)) {
    context.state = exchange::InboundAuthState::FAILED;
    this->record_debug(inbound_stage_name(context.state), 1, true);
    return false;
  }

  context.state = exchange::InboundAuthState::VERIFIED;
  this->record_debug(inbound_stage_name(context.state), 1, true);
  this->counters_.challenge_round_trips++;
  return true;
}

}  // namespace home_io_control
}  // namespace esphome

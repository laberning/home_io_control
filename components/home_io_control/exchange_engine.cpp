/// @file exchange_engine.cpp
/// @brief Authenticated exchange engine — outbound and inbound protocol flows.
/// @ingroup hioc_hub
///
/// Implements ExchangeEngine: the retry loop, challenge-response
/// authentication, final-response wait, the channel access of a request and the
/// listen-before-talk transmit of the fixed-channel senders, and frequency-hopping. Debug-snapshot helpers are also
/// here.

#include "exchange_engine.h"

#include "decisions_exchange.h"
#include "decisions_attempts.h"
#include "log_frame.h"
#include "proto_commands.h"
#include "proto_constants.h"
#include "proto_crypto.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>

namespace esphome {
namespace home_io_control {

static const char *const TAG = "home_io_control.exchange";

/// Microseconds per millisecond, for rounding a driver's transmit-time estimate up to whole ms.
static constexpr uint32_t US_PER_MS = 1000;

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

void ExchangeEngine::note_final_wait_(const ListenStats &stats) {
  DebugInfo &d = this->debug_;
  const auto add = [](uint8_t &total, uint8_t n) {
    total = static_cast<uint8_t>(std::min<unsigned>(total + n, UINT8_MAX));
  };
  add(d.final_waits, 1);
  add(d.final_rx_ignored, stats.frames_ignored);
  add(d.final_rx_failed, stats.failed_receptions);
  if (stats.failed_receptions != 0)
    d.final_rx_irq = stats.last_failed_irq;
}

void ExchangeEngine::record_debug(const char *stage, uint8_t tries, bool saw_challenge) {
  this->debug_.stage = stage;
  this->debug_.tries = tries;
  this->debug_.saw_challenge = this->debug_.saw_challenge || saw_challenge;

  const RadioCaptureInfo &capture = (*this->radio_ptr_)->get_last_capture();
  // send_and_receive() blanks the radio capture at the start of each exchange (the path this
  // snapshot mainly serves), so a valid capture here belongs to the current exchange rather than a
  // previous one. wait_for_packet() re-clears the capture before each listen, so the *last*
  // record_debug() call is always the final timed-out wait, which saw nothing. Keep the first
  // informative capture instead: it distinguishes "the radio never detected a frame" from "it
  // received one and this layer discarded it" — the question a failure report has to answer.
  // Known gap: record_debug() calls outside that blanking — the bare ones in pairing_engine.cpp,
  // authenticate_request(), and collect_broadcast_responses()'s "broadcast_tx_failed" branch — can
  // still carry a capture from before their flow began. None of those snapshots reaches log_debug().
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

int render_exchange_debug(char *buf, size_t buf_size, const char *device_id, const ExchangeEngine::DebugInfo &d) {
  return snprintf(buf, buf_size,
                  "device=%s cmd=%s(0x%02X) stage=%s tries=%u max_tries=%u saw_challenge=%u cap_valid=%u "
                  "cap_rx_done=%u cap_crc_err=%u cap_freq=%" PRIu32
                  " cap_irq=0x%04X cap_pkt=0x%02X cap_reported_len=%u cap_frame_len=%u cap_rssi=%d belief=%s "
                  "last_preamble=%u final_waits=%u final_rx_ignored=%u final_rx_failed=%u final_rx_irq=0x%04X",
                  device_id, command_name(d.request_cmd), d.request_cmd, d.stage, d.tries, d.max_tries,
                  // Rendered as 0/1: these are flags in a field list, not prose, and a caller greps them.
                  static_cast<unsigned>(d.saw_challenge), static_cast<unsigned>(d.capture_valid),
                  static_cast<unsigned>(d.capture_rx_done), static_cast<unsigned>(d.capture_crc_error),
                  d.capture_freq_hz, d.capture_irq_status, d.capture_packet_status, d.capture_reported_len,
                  d.capture_frame_len, d.capture_rssi_dbm,
                  d.wake_belief_use == ExchangeEngine::WakeBeliefUse::APPLIED
                      ? decisions::wake_belief_name(d.wake_belief)
                      : ExchangeEngine::wake_belief_use_name(d.wake_belief_use),
                  d.last_try_preamble, d.final_waits, d.final_rx_ignored, d.final_rx_failed, d.final_rx_irq);
}

void ExchangeEngine::log_debug(const char *device_id) const {
  char fields[EXCHANGE_DEBUG_LINE_SIZE];
  render_exchange_debug(fields, sizeof(fields), device_id, this->debug_);
  ESP_LOGW(TAG, "Exchange failed: %s", fields);
}

void ExchangeEngine::log_debug_unconfirmed(const char *device_id) const {
  char fields[EXCHANGE_DEBUG_LINE_SIZE];
  render_exchange_debug(fields, sizeof(fields), device_id, this->debug_);
  // "accepted" describes what the device did with the request, not what it did with our challenge
  // answer — see the WAIT_FINAL_RESPONSE branch in send_and_receive() for why silence here has two
  // possible causes. The final_rx_* fields are what tells them apart: a reception during the final
  // wait means something came back and was lost here. The cap_* fields cannot, because they keep
  // the first informative reception, which is the device's challenge.
  ESP_LOGI(TAG, "Exchange accepted without a closing reply: %s", fields);
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

namespace {

/// The RadioTxConfig for `frame` on `freq` with `preamble`: the one place a transmission's shape
/// (channel, preamble, wake-up level) is decided, for transmit_frame() and for the budget estimate.
RadioTxConfig tx_config_for(const IoFrame &frame, uint32_t freq, uint16_t preamble) {
  RadioTxConfig tx_config{};
  tx_config.freq_hz = freq;
  tx_config.preamble_len = preamble;
  tx_config.wake = tx_wake_for(is_start(frame), preamble);
  return tx_config;
}

/// Serialize @p frame for the air into @p buf, the first step of every send.
/// @return The wire length, or 0 (logged) when the frame does not serialize.
uint8_t serialize_for_tx(const IoFrame &frame, uint8_t (&buf)[FRAME_MAX_WIRE_SIZE]) {
  // FRAME_MAX_WIRE_SIZE, not FRAME_MAX_SIZE: a frame with an out-of-length MAC trailer
  // (IoFrame::has_mac, e.g. CMD_ONEWAY_ADD_CONTROLLER) serializes to more than
  // FRAME_MAX_SIZE/FRAME_MAX_DECLARED_SIZE bytes, and serialize() rejects a buffer too small to
  // hold its actual output rather than truncating into it.
  uint8_t const len = serialize(frame, buf, sizeof(buf));
  if (len == 0)
    ESP_LOGW(TAG, "tx: serialize_failed cmd=0x%02X", frame.cmd);
  return len;
}

}  // namespace

bool ExchangeEngine::transmit_frame(const IoFrame &frame, uint32_t freq, uint16_t preamble) {
  RadioDriver *radio = *this->radio_ptr_;
  uint8_t buf[FRAME_MAX_WIRE_SIZE];
  uint8_t const len = serialize_for_tx(frame, buf);
  if (len == 0)
    return false;
  // Listen before talk: up to LBT_MAX_RETRIES sends that the radio makes only if the TX channel is
  // clear, measured by the driver on that channel right before transmitting; after the last busy
  // result, one send without the check, so a noisy channel delays a frame but never drops it.
  // Only a frame that opens a session (CTRL0 START) is checked. A frame without START answers one
  // the peer has just sent and is listening for on this channel for only a few tens of ms, so it
  // goes out at once: a 5 ms back-off already loses the exchange on some receivers (issue #178),
  // and the frame that opened the session was checked by whoever sent it (ADR 0045). The requests
  // of send_and_receive() do not come through here for their check: they take a free channel
  // (transmit_with_channel_access_(), ADR 0096).
  const uint8_t lbt_checks = is_start(frame) ? LBT_MAX_RETRIES : 0;
  RadioTxConfig tx_config = tx_config_for(frame, freq, preamble);
  TxResult result;
  for (uint8_t lbt = 0;; lbt++) {
    if (lbt < lbt_checks) {
      tx_config.cca_threshold_dbm = this->tuning_->lbt_rssi_threshold_dbm;
    } else {
      tx_config.cca_threshold_dbm.reset();
    }
    result = radio->send_packet(buf, len, tx_config);
    if (result.status != TxResult::Status::CHANNEL_BUSY)
      break;
    ESP_LOGD(TAG, "LBT: channel busy (RSSI %d dBm on %" PRIu32 " Hz), retry %u/%u", result.cca_level_dbm, freq, lbt + 1,
             lbt_checks);
    this->note_busy_check_(result.cca_level_dbm);
    delay(LBT_RETRY_DELAY_MS);
  }
  return this->conclude_send_(frame, tx_config, len, result);
}

// === Shared steps of every send ===

void ExchangeEngine::note_busy_check_(int16_t level_dbm) {
  this->counters_.lbt_retries++;
  if (this->transmit_observer_ != nullptr)
    this->transmit_observer_->on_lbt_defer(level_dbm);
}

bool ExchangeEngine::conclude_send_(const IoFrame &frame, const RadioTxConfig &tx_config, uint8_t len,
                                    const TxResult &result) {
  if (!result.sent()) {
    ESP_LOGW(TAG, "tx: send_failed cmd=0x%02X", frame.cmd);
    return false;
  }
  if (this->transmit_observer_ != nullptr)
    this->transmit_observer_->on_transmit(frame, tx_config, len);
  return true;
}

// === Channel access of a request (ADR 0096) ===

TxResult ExchangeEngine::check_channel_and_send_(const uint8_t *buf, uint8_t len, RadioTxConfig &tx_config) {
  RadioDriver *radio = *this->radio_ptr_;
  const int16_t threshold_dbm = this->tuning_->lbt_rssi_threshold_dbm;
  // All readings but the last are taken here; the last is the radio's own, right before the first
  // symbol, so a channel that turns busy between the check and the send is still caught.
  for (uint8_t reading = 1; reading < CHANNEL_CHECK_READINGS; reading++) {
    const int16_t level_dbm = radio->measure_channel_level(tx_config.freq_hz);
    if (decisions::channel_is_busy(level_dbm, threshold_dbm))
      return TxResult::channel_busy(level_dbm);
    // Too short to receive anything meaningful and not a wait for a reply: the readings are the
    // point. Waiting through listen() would also hand the radio's queued frames to the wrong handler.
    delay(CHANNEL_CHECK_READING_GAP_MS);
  }
  tx_config.cca_threshold_dbm = threshold_dbm;
  return radio->send_packet(buf, len, tx_config);
}

void ExchangeEngine::hear_channel_for_(uint32_t ms) {
  ListenSpec spec;
  spec.window_ms = ms;
  spec.policy = ListenPolicy::HOLD_REQUEST_CHANNEL;
  RadioRxPacket packet{};
  IoFrame frame{};
  this->listen(spec, packet, frame, [&](const IoFrame *parsed, const RadioRxPacket &pkt) {
    // Whatever is on the channel is someone else's traffic: a remote's press is what the hub backed
    // off from, and it is handled like one heard inside an exchange's wait.
    if (parsed != nullptr)
      this->report_overheard_(*parsed, pkt);
    return ReplyDisposition::IGNORE;
  });
}

ExchangeEngine::RequestTransmit ExchangeEngine::transmit_with_channel_access_(const IoFrame &frame, uint32_t own_freq,
                                                                              uint16_t preamble,
                                                                              uint32_t previous_freq) {
  RadioDriver *radio = *this->radio_ptr_;
  uint8_t buf[FRAME_MAX_WIRE_SIZE];
  const uint8_t len = serialize_for_tx(frame, buf);
  if (len == 0)
    return {decisions::ChannelAccessStatus::FAILED, own_freq};
  // A low-power receiver is woken on channel 2 and stays there; any other request leaves the
  // previous attempt's channel and, on a busy result, the one it just measured.
  const bool low_power = (frame.ctrl1 & CTRL1_LOW_POWER) != 0;
  uint32_t freq = low_power ? own_freq : decisions::request_channel(previous_freq, this->draw_random_());
  const uint32_t begin_ms = millis();
  for (uint8_t busy_checks = 0;; busy_checks++) {
    RadioTxConfig tx_config = tx_config_for(frame, freq, preamble);
    const TxResult result = this->check_channel_and_send_(buf, len, tx_config);
    if (result.status != TxResult::Status::CHANNEL_BUSY) {
      const bool sent = this->conclude_send_(frame, tx_config, len, result);
      return {sent ? decisions::ChannelAccessStatus::SENT : decisions::ChannelAccessStatus::FAILED, freq};
    }
    ESP_LOGD(TAG, "Channel busy (RSSI %d dBm on %" PRIu32 " Hz), check %u", result.cca_level_dbm, freq,
             busy_checks + 1);
    this->note_busy_check_(result.cca_level_dbm);
    const std::optional<uint32_t> backoff_ms =
        decisions::next_backoff_ms(millis() - begin_ms, busy_checks, low_power, this->draw_random_());
    if (!backoff_ms.has_value())
      break;
    // The back-off is spent listening on the busy channel: that is where the traffic the hub backs
    // off from is, and it keeps the loop alive (listen() feeds the watchdog while it blocks).
    this->hear_channel_for_(*backoff_ms);
    if (!low_power)
      freq = decisions::request_channel(freq, this->draw_random_());
  }
  if (!this->tuning_->lbt_force_when_busy)
    return {decisions::ChannelAccessStatus::CHANNEL_BUSY, freq};

  // Opted in: every channel the request may use stayed busy, so send on the quietest of the three. A
  // low-power request may use only its own channel, so there is nothing to compare.
  std::array<int16_t, decisions::REQUEST_CHANNELS.size()> levels{};
  if (!low_power) {
    for (size_t i = 0; i < levels.size(); i++)
      levels[i] = radio->measure_channel_level(decisions::REQUEST_CHANNELS[i]);
  }
  const uint32_t forced_freq = decisions::forced_send_channel(low_power, own_freq, levels);
  const RadioTxConfig tx_config = tx_config_for(frame, forced_freq, preamble);
  ESP_LOGW(TAG, "Channel busy for %" PRIu32 " ms: sending cmd=0x%02X on %" PRIu32 " Hz anyway (lbt_force_when_busy)",
           millis() - begin_ms, frame.cmd, forced_freq);
  const bool sent = this->conclude_send_(frame, tx_config, len, radio->send_packet(buf, len, tx_config));
  return {sent ? decisions::ChannelAccessStatus::SENT_FORCED : decisions::ChannelAccessStatus::FAILED, forced_freq};
}

uint32_t ExchangeEngine::final_reply_tail_ms_(const IoFrame &request, uint32_t freq) const {
  // The 0x3D answer is not built before the guard runs, so the request stands in for its size (a
  // 0x3D is no longer than a request); the overhead covers the answer's channel check and SPI.
  return this->tuning_->exchange_response_wait_ms +
         this->tx_time_ms(request, freq, (*this->radio_ptr_)->response_preamble()) + EXCHANGE_ATTEMPT_OVERHEAD_MS;
}

uint32_t ExchangeEngine::tx_time_ms(const IoFrame &frame, uint32_t freq, uint16_t preamble) const {
  uint8_t buf[FRAME_MAX_WIRE_SIZE];
  uint8_t const len = serialize(frame, buf, sizeof(buf));
  if (len == 0)
    return 0;
  uint32_t const tx_us = (*this->radio_ptr_)->tx_air_time_us(len, tx_config_for(frame, freq, preamble));
  return (tx_us + US_PER_MS - 1) / US_PER_MS;
}

decisions::AttemptPlan ExchangeEngine::plan_for_(const IoFrame &request, uint32_t freq, const ExchangeOptions &options,
                                                 const PreamblePlan &preamble_plan) const {
  const decisions::AttemptRequest &attempts = options.attempts;
  if (!attempts.planned) {
    // Never below 1 (a caller passing 0 must not silently transmit nothing) and never above
    // EXCHANGE_RETRY_COUNT: a caller that wants more attempts passes a read kind.
    const uint16_t window_ms =
        is_start(request) ? this->tuning_->exchange_start_response_wait_ms : this->tuning_->exchange_response_wait_ms;
    decisions::AttemptPlan counted = decisions::attempt_plan_for_tries(attempts.max_tries, window_ms);
    // A low-power target keeps the tries the caller named behind the wake-up (ADR 0095).
    counted.bounded_by_attempts = is_start(request) && (request.ctrl1 & CTRL1_LOW_POWER) != 0;
    return counted;
  }
  const bool low_power = (request.ctrl1 & CTRL1_LOW_POWER) != 0;
  // A settling read of a low-power receiver the hub has no news of leads with one short attempt
  // (WakeBelief::UNKNOWN). Every other read, and any read whose preamble the caller fixed, has none.
  const bool short_lead = attempts.kind == decisions::ReadKind::SETTLING && low_power &&
                          preamble_plan.use == WakeBeliefUse::APPLIED &&
                          preamble_plan.belief == decisions::WakeBelief::UNKNOWN;
  decisions::AttemptPlan plan =
      this->plan_attempts(attempts.kind, low_power, request, freq, this->request_preamble_for(request), 0, short_lead);
  plan.state_unknown = attempts.state_unknown;
  return plan;
}

decisions::AttemptPlan ExchangeEngine::plan_attempts(decisions::ReadKind kind, bool low_power, const IoFrame &request,
                                                     uint32_t freq, uint16_t preamble, uint16_t last_window_ms,
                                                     bool with_short_lead) const {
  return decisions::attempt_plan(kind, low_power, *this->tuning_, this->tx_time_ms(request, freq, preamble),
                                 last_window_ms, with_short_lead);
}

bool ExchangeEngine::begin_try_(uint8_t tries_started, uint32_t elapsed_ms, uint32_t gap_ms, uint32_t window_ms,
                                const std::optional<TryBudget> &budget) {
  if (tries_started == 0)
    return true;
  if (budget.has_value() && !this->try_fits_budget_(elapsed_ms, gap_ms, budget->tx_ms, window_ms, budget->auth_tail_ms))
    return false;
  App.feed_wdt();
  delay(gap_ms);  // 0 for a run without a gap: a no-op that still yields
  this->counters_.retransmits++;
  return true;
}

ExchangeEngine::AttemptRun ExchangeEngine::fixed_attempts(uint8_t count, uint16_t gap_ms, uint16_t window_ms) {
  AttemptRun run;
  run.plan.max_attempts = count;
  run.plan.gap_ms = gap_ms;
  run.plan.window_ms = window_ms;
  return run;
}

AttemptRunResult ExchangeEngine::run_attempts(const AttemptRun &run, const TryHandler &on_try) {
  const decisions::AttemptPlan &plan = run.plan;
  const uint32_t begin_ms = millis();
  for (uint8_t tries = 0; tries < plan.max_attempts; tries++) {
    const uint16_t window_ms = plan.window_for_attempt(tries + 1, plan.max_attempts);
    std::optional<TryBudget> budget;
    if (run.budgeted_tx_ms.has_value() && tries > 0 && decisions::obeys_time_budget(plan))
      budget = TryBudget{*run.budgeted_tx_ms, 0};
    if (!this->begin_try_(tries, millis() - begin_ms, plan.gap_ms, window_ms, budget))
      return AttemptRunResult::BUDGET_EXHAUSTED;
    switch (on_try(static_cast<uint8_t>(tries + 1), window_ms)) {
      case AttemptVerdict::DONE:
        return AttemptRunResult::DONE;
      case AttemptVerdict::STOP:
        return AttemptRunResult::STOPPED;
      case AttemptVerdict::AGAIN:
        break;
    }
  }
  return AttemptRunResult::TRIES_USED;
}

bool ExchangeEngine::try_fits_budget_(uint32_t elapsed_ms, uint32_t gap_ms, uint32_t tx_ms, uint32_t window_ms,
                                      uint32_t auth_tail_ms) const {
  return decisions::attempt_fits(elapsed_ms, gap_ms, tx_ms, window_ms, auth_tail_ms,
                                 this->tuning_->exchange_total_budget_ms);
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

/// Buffer for format_try_age(): "n/a", or a uint32_t's ten decimal digits, plus the terminator.
constexpr size_t TRY_AGE_BUFFER_SIZE = 12;

/// Render a try's `age_ms=` field: how long before this try the target was last heard, or "n/a"
/// when there is no stamp (never heard, or the target evidence was not looked up). Every try line
/// carries it next to the try's preamble, so field logs show which preamble reaches a low-power
/// receiver how long after it last spoke — the data the wake-belief windows are sized from. The
/// stamp is read once, before the exchange, so a challenge heard on an earlier try of the same
/// exchange does not reset it.
void format_try_age(const exchange::OutboundExchangeContext &ctx, char (&buf)[TRY_AGE_BUFFER_SIZE]) {
  if (ctx.target_last_seen_ms == 0) {
    snprintf(buf, sizeof(buf), "n/a");
    return;
  }
  snprintf(buf, sizeof(buf), "%" PRIu32, ctx.try_start_ms - ctx.target_last_seen_ms);
}

/// True for a start frame addressed to a duty-cycled receiver — the only frame that has a wake-up
/// preamble to choose. One definition, shared by the fixed rule and the per-try wake belief.
bool is_low_power_start(const IoFrame &request) { return is_start(request) && (request.ctrl1 & CTRL1_LOW_POWER) != 0; }

/// Determine if a candidate frame is a valid final response for the request.
bool is_valid_final_response(const IoFrame &candidate, const IoFrame &request) {
  return decisions::classify_exchange_final_response(request, candidate) ==
         decisions::ExchangeFinalResponseDisposition::ACCEPT;
}

/// The final-reply window the budget guard counts ahead of the try that follows an unconfirmed one.
/// None before a CMD_EXECUTE re-send: it is the last copy, so what a silent final wait costs is
/// already the unconfirmed outcome the exchange ends in, and counting that window would refuse a
/// re-send that fits. The full window before any other request's next try.
uint32_t auth_tail_after_unconfirmed(const IoFrame &request, uint32_t final_window_ms) {
  return request.cmd == CMD_EXECUTE ? 0 : final_window_ms;
}

}  // namespace

ExchangeOutcome ExchangeEngine::send_and_receive(const IoFrame &request, IoFrame &response, uint32_t freq,
                                                 const ExchangeOptions &options) {
  this->reset_debug(request.cmd);
  // Blank the radio's diagnostic capture at the start of the exchange. The radio only clears it
  // when it actually begins a listen, so an exchange that transmits and then hears nothing at all
  // would otherwise inherit the *previous* exchange's capture and claim "we heard a frame" when
  // nothing was on air. Done here rather than in reset_debug() because collect_broadcast_responses()
  // shares that helper but does not need this: its only capture reader runs per delivered reply
  // (with the listen repopulating the capture first), and its DebugInfo is never passed to log_debug().
  (*this->radio_ptr_)->clear_last_capture();
  // The exchange is planned here, once: the preamble plan first (it consults the target evidence),
  // then the attempt plan, which a read's short lead builds on.
  const PreamblePlan preamble_plan =
      this->plan_request_preamble_(request, options.preamble_override, options.state_unknown());
  const decisions::AttemptPlan plan = this->plan_for_(request, freq, options, preamble_plan);
  // The plan builders own the range [1, EXCHANGE_MAX_ATTEMPTS] of the attempt count.
  uint8_t tries_allowed = plan.max_attempts;
  this->debug_.max_tries = tries_allowed;
  this->debug_.wake_belief_use = preamble_plan.use;
  if (preamble_plan.use == WakeBeliefUse::APPLIED) {
    this->debug_.wake_belief = preamble_plan.belief;
    if (preamble_plan.belief == decisions::WakeBelief::AWAKE) {
      ESP_LOGD(TAG, "Low-power target %s believed %s: short preamble first", node_id_to_string(request.dst).c_str(),
               decisions::wake_belief_name(preamble_plan.belief));
    }
  }
  const uint32_t exchange_begin_ms = millis();
  uint8_t unconfirmed_tries = 0;  // tries that ended challenged but never closed
  uint32_t previous_freq = 0;  // channel of the previous try, which a free-channel request leaves; 0 before the first
  bool evidence_try = false;   // some try was sent onto a channel measured free (or failed to send); see below
  uint32_t auth_tail_ms = 0;   // the final-reply window a challenged try adds; 0 until the target challenges
  uint32_t follow_up_gap_ms = 0;  // least wait before the next try, set by the unconfirmed rule
  bool announce_resend = false;   // the next try repeats an EXECUTE whose first copy went unconfirmed

  for (uint8_t tries = 0; tries < tries_allowed; tries++) {
    exchange::OutboundExchangeContext context;
    context.try_index = tries + 1;
    // Every attempt but the last listens the early window; the last (and a re-send, which is always
    // the last) the whole one.
    const uint16_t window_ms = plan.window_for_attempt(context.try_index, tries_allowed);
    context.wait_ms = window_ms;
    context.state = exchange::OutboundExchangeState::TX_REQUEST;

    // The attempt count is a maximum, not a promise: begin_try_() does not start a try that could not
    // finish inside the budget, counting the window it listens and, once the target has challenged,
    // the final-reply window. See EXCHANGE_TOTAL_BUDGET_MS -- this is what keeps a failing command
    // from blocking the ESPHome loop for the full attempts x (transmission + window) product.
    std::optional<TryBudget> budget;
    if (tries > 0 && decisions::obeys_time_budget(plan)) {
      budget = TryBudget{this->tx_time_ms(request, freq, this->try_preamble_(plan, preamble_plan, context.try_index)),
                         auth_tail_ms};
    }
    const uint32_t gap_ms = std::max<uint32_t>(plan.gap_ms, follow_up_gap_ms);
    if (!this->begin_try_(tries, millis() - exchange_begin_ms, gap_ms, window_ms, budget)) {
      this->record_debug("retry_budget_exhausted", tries, false);
      ESP_LOGI(TAG, "Exchange budget exhausted after %u tries for cmd=%s(0x%02X) (%" PRIu32 " of %u ms)", tries,
               command_name(request.cmd), request.cmd, millis() - exchange_begin_ms,
               this->tuning_->exchange_total_budget_ms);
      break;
    }

    // The one record that an EXECUTE's first copy went unanswered: the re-send may well succeed, and
    // then no exchange-failure or unconfirmed line is printed at all.
    if (announce_resend) {
      ESP_LOGI(TAG,
               "Try %u accepted without a closing reply for cmd=%s(0x%02X): re-sent, the device normally "
               "confirms (final_rx_ignored=%u final_rx_failed=%u)",
               tries, command_name(request.cmd), request.cmd, this->debug_.final_rx_ignored,
               this->debug_.final_rx_failed);
      announce_resend = false;
    }

    // Stamped after the retry gap, so a try's wait_ms and age_ms measure from its own transmit and
    // not from the end of the previous try.
    context.try_start_ms = millis();
    context.request_preamble = this->try_preamble_(plan, preamble_plan, context.try_index);
    context.target_last_seen_ms = preamble_plan.last_seen_ms;
    this->debug_.last_try_preamble = context.request_preamble;
    const decisions::ChannelAccessStatus access = this->transmit_request_(
        request, freq, context.request_preamble, options.channel_access, previous_freq, context);
    previous_freq = context.request_freq;
    evidence_try |= decisions::silence_is_evidence(access);
    // A request that was never sent (channel busy) or failed to send ends the try; a forced send
    // onto a busy channel listens like any other.
    if (!decisions::request_was_sent(access))
      continue;

    context.state = exchange::OutboundExchangeState::WAIT_FIRST_RESPONSE;
    this->record_debug(outbound_stage_name(context.state), context.try_index, false);
    auto first_disp = this->wait_for_first_response_(request, context);
    if (first_disp == decisions::ExchangeFirstResponseDisposition::IGNORE_UNRELATED)
      continue;
    if (first_disp == decisions::ExchangeFirstResponseDisposition::COMPLETE_DIRECT) {
      context.state = exchange::OutboundExchangeState::SUCCESS;
      this->record_debug("success_direct", context.try_index, false);
      // The hit counterpart of "Try N ended": an unauthenticated reply (a status poll's answer)
      // prints no challenge line, so without this a log would only ever show which preambles missed.
      char age[TRY_AGE_BUFFER_SIZE];
      format_try_age(context, age);
      ESP_LOGI(TAG, "Try %d answered: cmd=%s(0x%02X) wait_ms=%" PRIu32 " preamble=%u age_ms=%s", context.try_index,
               command_name(request.cmd), request.cmd, context.first_response_ms - context.try_start_ms,
               context.request_preamble, age);
      response = context.rx;
      return ExchangeOutcome::SUCCESS_WITH_RESPONSE;
    }

    auth_tail_ms = this->final_reply_tail_ms_(request, context.request_freq);
    if (!this->handle_authentication_(request, context.request_freq, context, options.key))
      continue;

    context.state = exchange::OutboundExchangeState::WAIT_FINAL_RESPONSE;
    this->record_debug(outbound_stage_name(context.state), context.try_index, true);
    auto final_disp = this->wait_for_final_response_(request, context);
    if (final_disp != decisions::ExchangeFinalResponseDisposition::ACCEPT) {
      // The device challenged us, so it demonstrably received the request. It does not follow that
      // it acted on it: a challenge says nothing about whether our 0x3D answer arrived and
      // verified, and a device that never got that answer never executes. Silence here therefore
      // has two causes that look identical from this side — our answer was lost, or the device
      // replied and this side lost the reply — on top of the devices that simply never close an
      // exchange with a synchronous reply (see ExchangeOutcome). log_debug_unconfirmed()'s final_rx_*
      // fields are what separates them after the fact: a reception during the final wait means a
      // reply came back and was lost on this side.
      // How many more tries to spend is after_unconfirmed_(): a CMD_EXECUTE may already be
      // acting on this copy, so it is repeated only where that is harmless and the silence is an
      // anomaly, and then only once; a CMD_SUBSCRIBE_EVENT is sent once more, since its answer is the
      // reading; everything else keeps its full retry budget.
      context.state = exchange::OutboundExchangeState::SUCCESS;
      this->record_debug("success_auth_unconfirmed", context.try_index, true);
      unconfirmed_tries++;
      const UnconfirmedFollowUp follow_up = this->after_unconfirmed_(
          request, unconfirmed_tries, context.try_index, static_cast<uint8_t>(tries_allowed - context.try_index));
      if (follow_up.further_tries == 0)
        return ExchangeOutcome::SUCCESS_UNCONFIRMED;
      follow_up_gap_ms = follow_up.gap_ms;
      announce_resend = follow_up.announce;
      auth_tail_ms = auth_tail_after_unconfirmed(request, auth_tail_ms);
      tries_allowed = std::min<uint8_t>(tries_allowed, tries + 1 + follow_up.further_tries);
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
  if (unconfirmed_tries > 0)
    return ExchangeOutcome::SUCCESS_UNCONFIRMED;
  this->spend_wake_evidence_if_silent_(request, preamble_plan, evidence_try);
  return ExchangeOutcome::FAILED;
}

uint16_t ExchangeEngine::try_preamble_(const decisions::AttemptPlan &plan, const PreamblePlan &preamble_plan,
                                       uint8_t try_index) const {
  // The plan's short lead is try 1 (a receiver in an unknown state, WakeBelief::UNKNOWN); every later
  // try follows the belief.
  if (plan.short_lead && try_index == 1)
    return this->start_preamble();
  return preamble_plan.for_try(try_index);
}

void ExchangeEngine::spend_wake_evidence_if_silent_(const IoFrame &request, const PreamblePlan &plan,
                                                    bool some_try_is_evidence) {
  // Believed moving, yet silent to the short preamble a moving receiver hears and to the wake-up
  // preamble alike: it has most likely come to rest. Spend the moving evidence so the next exchange
  // leads with the wake-up preamble instead of losing its first try (or, for a single-try poll, its
  // only one) to the short preamble again. A challenge means it heard us, so that keeps the evidence.
  // So does a silent STOP: nothing was stopped, and the receiver may well still be travelling.
  // Nor does silence while the remaining time it reported has not run out: a moving receiver that
  // missed one frame is still moving, and spending the evidence would send the next try with the
  // wake-up preamble it ignores (ADR 0077).
  // Nor does silence after tries that were never sent, or went onto a channel measured busy: a
  // collision explains it as well (ADR 0096).
  if (some_try_is_evidence && plan.use == WakeBeliefUse::APPLIED && plan.belief == decisions::WakeBelief::AWAKE &&
      !this->debug_.saw_challenge && !decisions::is_stop_request(request) && this->wake_evidence_spent_handler_ &&
      !this->still_travelling_by_its_own_count_(request)) {
    ESP_LOGD(TAG, "Low-power target %s silent while believed awake: next exchange leads with the wake-up preamble",
             node_id_to_string(request.dst).c_str());
    this->wake_evidence_spent_handler_(request.dst);
  }
}

bool ExchangeEngine::still_travelling_by_its_own_count_(const IoFrame &request) const {
  decisions::TargetEvidence evidence{};
  return this->target_evidence_provider_ && this->target_evidence_provider_(request.dst, evidence) &&
         decisions::moving_until_holds(evidence, millis());
}

// ============================================================================
// Outbound exchange step helpers
// ============================================================================

uint16_t ExchangeEngine::request_preamble_for(const IoFrame &request) const {
  // Gate on the start flag first: several device-role / continuation builders (key transfer,
  // status-update response) set CTRL1_LOW_POWER on a non-start frame, and those must keep the
  // short response preamble, not be lengthened.
  if (!is_start(request))
    return (*this->radio_ptr_)->response_preamble();
  return is_low_power_start(request) ? this->wake_preamble() : this->start_preamble();
}

uint16_t ExchangeEngine::preamble_bytes_for_ms(float ms) const {
  return (*this->radio_ptr_)->preamble_bytes_for_ms(ms);
}

uint16_t ExchangeEngine::start_preamble() const {
  return this->preamble_bytes_for_ms(this->tuning_->start_preamble_ms);
}

uint16_t ExchangeEngine::wake_preamble() const { return this->preamble_bytes_for_ms(this->tuning_->low_power_wake_ms); }

const char *ExchangeEngine::wake_belief_use_name(WakeBeliefUse use) {
  switch (use) {
    case WakeBeliefUse::OVERRIDE:
      return "override";
    case WakeBeliefUse::SWITCHED_OFF:
      return "off";
    case WakeBeliefUse::NO_PROVIDER:
      return "no_provider";
    case WakeBeliefUse::APPLIED:
      return "applied";
    case WakeBeliefUse::NOT_LOW_POWER:
    default:
      return "not_low_power";
  }
}

ExchangeEngine::PreamblePlan ExchangeEngine::plan_request_preamble_(const IoFrame &request, uint16_t override_preamble,
                                                                    bool state_unknown) const {
  PreamblePlan plan;
  plan.fixed = override_preamble != 0 ? override_preamble : this->request_preamble_for(request);
  // Every reason not to apply the belief keeps the fixed preamble and is recorded, so the
  // exchange-failure log names it. The override is the caller's explicit choice (pairing), so it
  // is never second-guessed. Only a low-power *start* frame has a wake-up preamble to reorder.
  // The try count is deliberately not a reason: a single-try exchange (most scheduler-owned status
  // polls) follows the belief's first try like any other. Its likeliest moment is the settle poll
  // seconds after a command or STOP, when the receiver is travelling or has just answered — the
  // state in which a moving VELUX solar receiver ignores the wake-up preamble — so a fixed wake-up
  // preamble there loses the poll and a backoff slot instead of guarding against anything.
  if (override_preamble != 0) {
    plan.use = WakeBeliefUse::OVERRIDE;
  } else if (!is_low_power_start(request)) {
    plan.use = WakeBeliefUse::NOT_LOW_POWER;
  } else if (!this->tuning_->low_power_wake_belief) {
    plan.use = WakeBeliefUse::SWITCHED_OFF;
  } else if (!this->target_evidence_provider_) {
    plan.use = WakeBeliefUse::NO_PROVIDER;
  } else {
    plan.use = WakeBeliefUse::APPLIED;
  }
  if (plan.use != WakeBeliefUse::APPLIED)
    return plan;

  // A destination the hub has no record of has no evidence: treat it as asleep, the safe default.
  decisions::TargetEvidence evidence{};
  const bool known = this->target_evidence_provider_(request.dst, evidence);
  plan.short_preamble = this->start_preamble();
  plan.wake_preamble = this->wake_preamble();
  plan.last_seen_ms = known ? evidence.last_seen_ms : 0;
  plan.belief = known ? decisions::wake_belief(evidence, millis(), decisions::is_stop_request(request),
                                               request.cmd == CMD_PRIVATE, state_unknown)
                      : decisions::WakeBelief::ASLEEP;
  return plan;
}

decisions::ChannelAccessStatus ExchangeEngine::transmit_request_(const IoFrame &request, uint32_t freq,
                                                                 uint16_t preamble, ChannelAccess access,
                                                                 uint32_t previous_freq,
                                                                 exchange::OutboundExchangeContext &ctx) {
  RequestTransmit sent;
  if (access == ChannelAccess::FREE_CHANNEL && is_start(request)) {
    sent = this->transmit_with_channel_access_(request, freq, preamble, previous_freq);
  } else {
    sent.status = this->transmit_frame(request, freq, preamble) ? decisions::ChannelAccessStatus::SENT
                                                                : decisions::ChannelAccessStatus::FAILED;
    sent.freq_hz = freq;
  }
  ctx.request_freq = sent.freq_hz;
  if (sent.status == decisions::ChannelAccessStatus::CHANNEL_BUSY) {
    ctx.state = exchange::OutboundExchangeState::FAILED;
    this->record_debug("channel_busy", ctx.try_index, false);
    this->counters_.channel_busy_attempts++;
    ESP_LOGI(TAG,
             "Try %d ended: channel busy for cmd=%s(0x%02X), nothing sent, last checked %" PRIu32 " Hz preamble=%u",
             ctx.try_index, command_name(request.cmd), request.cmd, sent.freq_hz, preamble);
  } else if (sent.status == decisions::ChannelAccessStatus::FAILED) {
    ctx.state = exchange::OutboundExchangeState::FAILED;
    this->record_debug("tx_request_failed", ctx.try_index, false);
  }
  return sent.status;
}

void ExchangeEngine::report_overheard_(const IoFrame &frame, const RadioRxPacket &packet) {
  if (this->overheard_frame_observer_ == nullptr)
    return;
  const int16_t rssi_dbm = (*this->radio_ptr_)->get_last_capture().rssi_dbm;
  this->overheard_frame_observer_->on_overheard_frame(frame, packet, rssi_dbm, millis());
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
  // The frame log holds its lines from here until the 0x3D has gone out (handle_authentication_()),
  // so a challenge's RX line and the 0x3D's TX line print after the answer instead of delaying it:
  // some receivers drop the command when the answer is late (issue #163). A rejected frame's lines
  // are printed as soon as it is rejected, and any other outcome releases the hold below. No-ops
  // without IOHOME_FRAME_LOG.
  frame_log_hold();
  auto outcome = this->listen(spec, packet, ctx.rx, [&](const IoFrame *parsed, const RadioRxPacket &pkt) {
    if (parsed == nullptr) {
      frame_log_flush();
      this->record_debug("first_parse_fail", ctx.try_index, false);
      this->counters_.parse_failures++;
      log_unparsable_frame("Unparsable first response", ctx.try_index, pkt);
      return ReplyDisposition::IGNORE;
    }
    disp = decisions::classify_exchange_first_response(request, *parsed);
    if (disp == decisions::ExchangeFirstResponseDisposition::IGNORE_UNRELATED) {
      frame_log_flush();
      this->record_debug("first_wrong_exchange", ctx.try_index, false);
      log_exchange_frame("Ignored first response", ctx.try_index, *parsed, pkt.len);
      this->report_overheard_(*parsed, pkt);
      return ReplyDisposition::IGNORE;
    }
    ctx.first_response_ms = millis();
    ctx.first_response_us = micros();
    return ReplyDisposition::ACCEPT;
  });

  // Only a challenge keeps the hold: send_and_receive_() hands it straight to handle_authentication_(),
  // which releases it after the 0x3D.
  if (outcome != ListenOutcome::ACCEPTED || disp != decisions::ExchangeFirstResponseDisposition::REQUIRE_AUTH)
    frame_log_release();
  if (outcome == ListenOutcome::ACCEPTED)
    return disp;

  ctx.state = exchange::OutboundExchangeState::FAILED;
  this->record_debug("wait_first_timeout", ctx.try_index, false);
  char age[TRY_AGE_BUFFER_SIZE];
  format_try_age(ctx, age);
  ESP_LOGI(TAG, "Try %d ended: no first response for cmd=%s(0x%02X) within %" PRIu32 " ms preamble=%u age_ms=%s",
           ctx.try_index, command_name(request.cmd), request.cmd, ctx.wait_ms, ctx.request_preamble, age);
  return decisions::ExchangeFirstResponseDisposition::IGNORE_UNRELATED;
}

bool ExchangeEngine::handle_authentication_(const IoFrame &request, uint32_t freq,
                                            exchange::OutboundExchangeContext &ctx, const uint8_t *key) {
  ctx.saw_challenge = true;
  ctx.state = exchange::OutboundExchangeState::BUILD_AUTH_RESPONSE;
  this->record_debug(outbound_stage_name(ctx.state), ctx.try_index, true);

  // No challenge bytes here: the raw 0x3C payload plus the 0x3D response it provokes is a
  // known-plaintext/known-ciphertext pair under the system key (see redaction.h). The generic
  // frame-log helpers (log_frame()/log_component_capture()) already mask both commands.
  ctx.state = exchange::OutboundExchangeState::TX_AUTH_RESPONSE;
  this->record_debug(outbound_stage_name(ctx.state), ctx.try_index, true);
  // Nothing is logged before the 0x3D: some receivers drop the command when the answer comes late,
  // and a log line here sits between challenge and answer (issue #163); the frame log is held for
  // the same reason (wait_for_first_response_()). The line below is printed after the send; every driver re-arms RX
  // inside send_packet(), so the final reply is still received while it prints. answer_challenge() covers both the 0x3D
  // build and its transmit as one step, so a failure here does not distinguish which of the two failed.
  const bool answered = this->answer_challenge(request, ctx.rx, freq, key);
  const uint32_t answer_us = micros() - ctx.first_response_us;
  // The challenge's and the 0x3D's frame-log lines, held since wait_for_first_response_().
  frame_log_release();

  char age[TRY_AGE_BUFFER_SIZE];
  format_try_age(ctx, age);
  // answer_us: from the challenge being accepted until answer_challenge() returned: the 0x3D's
  // build, its airtime up to TxDone, and the driver's RX re-arm and post-TX settle.
  ESP_LOGI(TAG,
           "Auth challenge try=%d wait_ms=%" PRIu32
           " req_cmd=0x%02X req_len=%u preamble=%u age_ms=%s answer_us=%" PRIu32,
           ctx.try_index, ctx.first_response_ms - ctx.try_start_ms, request.cmd, request.data_len, ctx.request_preamble,
           age, answer_us);

  if (!answered) {
    ctx.state = exchange::OutboundExchangeState::FAILED;
    this->record_debug("auth_response_failed", ctx.try_index, true);
    return false;
  }
  this->counters_.challenge_round_trips++;
  return true;
}

bool ExchangeEngine::answer_challenge(const IoFrame &request, const IoFrame &challenge, uint32_t freq,
                                      const uint8_t *key) {
  IoFrame auth_resp;
  if (!create_challenge_resp(auth_resp, request.dst, this->node_id_, challenge.data, request,
                             key != nullptr ? key : this->key_for(request.dst)))
    return false;
  return this->transmit_frame(auth_resp, freq, (*this->radio_ptr_)->response_preamble());
}

ExchangeEngine::UnconfirmedFollowUp ExchangeEngine::after_unconfirmed_(const IoFrame &request,
                                                                       uint8_t unconfirmed_tries, uint8_t try_index,
                                                                       uint8_t tries_left) const {
  // The last try the exchange allows: no further copy can follow, so none is planned or announced.
  if (tries_left == 0)
    return {};
  // Only an EXECUTE's answer depends on the target, so only an EXECUTE pays for the lookup. A
  // destination the hub has no record of has never confirmed anything.
  decisions::TargetEvidence evidence{};
  if (request.cmd == CMD_EXECUTE && this->target_evidence_provider_)
    this->target_evidence_provider_(request.dst, evidence);
  if (!decisions::retry_after_unconfirmed_accept_is_safe(request, evidence.confirms_execute, unconfirmed_tries))
    return {};
  // A subscription's answer carries the reading, so its one re-send goes out at the ordinary retry
  // gap (nothing moves), and it is also the last copy: no ordinary failure retry after it.
  if (request.cmd == CMD_SUBSCRIBE_EVENT)
    return {UNCONFIRMED_SUBSCRIBE_MAX_RESENDS, 0, false};
  // Any other request is repeated at the ordinary gap, but never into more copies than
  // EXCHANGE_RETRY_COUNT in all, a plan's larger attempt count notwithstanding: the target has
  // heard this request, so more copies than that would only repeat a write it already holds.
  if (request.cmd != CMD_EXECUTE)
    return {static_cast<uint8_t>(EXCHANGE_RETRY_COUNT - std::min<uint8_t>(try_index, EXCHANGE_RETRY_COUNT)), 0, false};
  // The re-send waits the longer gap, so a device still busy acting on the first copy has finished
  // before it arrives. It is the only further copy of a CMD_EXECUTE: if it goes unanswered
  // altogether, the ordinary failure retry must not add a third one to a device that already has
  // the command.
  return {UNCONFIRMED_EXECUTE_MAX_RESENDS, UNCONFIRMED_EXECUTE_RESEND_DELAY_MS, true};
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

  ListenStats stats;
  spec.stats = &stats;

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
    this->report_overheard_(*parsed, pkt);
    return ReplyDisposition::IGNORE;
  });

  this->note_final_wait_(stats);
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
                                                    uint32_t window_ms, const BroadcastReplyHandler &on_reply,
                                                    ListenPolicy policy) {
  this->reset_debug(request.cmd);

  if (!this->transmit_frame(request, freq, this->request_preamble_for(request))) {
    this->record_debug("broadcast_tx_failed", 1, false);
    return 0;
  }
  const uint32_t tx_done_ms = millis();

  RadioDriver *radio = *this->radio_ptr_;
  uint8_t count = 0;

  // policy is the caller's choice: ROTATE_SKIPPING_REQUEST (the default) leaves the request
  // channel unattended because so few replies land there for that caller's population; a caller
  // whose replies do land there passes ROTATE_ALL_CHANNELS instead. request_freq is set
  // unconditionally — harmless for ROTATE_ALL_CHANNELS, which never reads it.
  // A reception proves this channel carries responders and replies arrive spread across the whole
  // window, so staying on it costs nothing; hopping away would only shrink the time spent where
  // responders already are.
  const ListenSpec spec = rotating_broadcast_listen_spec(window_ms, policy, freq, /*hop_after_ignored_frame=*/false);

  RadioRxPacket packet{};
  IoFrame frame{};
  this->listen(spec, packet, frame, [&](const IoFrame *parsed, const RadioRxPacket &pkt) {
    if (parsed == nullptr || !decisions::is_broadcast_reply_for(*parsed, expected_cmd, this->node_id_))
      return ReplyDisposition::IGNORE;

    BroadcastReplyInfo info{};
    info.rssi_dbm = radio->get_last_capture().rssi_dbm;
    info.rx_freq_hz = pkt.freq_hz;
    info.after_tx_ms = millis() - tx_done_ms;
    on_reply(*parsed, info);
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
// first/final-response waits, challenge_sender()'s wait for the 0x3D, collect_broadcast_responses(),
// and PairingEngine's discovery/key-transfer/key-confirm waits all call listen() below with a
// ListenSpec that picks one of the three ListenPolicy values (hold the request channel, rotate all
// three, or rotate skipping the request channel). Per-loop behavior — what counts as a match, what
// aborts, what gets logged — lives entirely in the caller's ReplyHandler; this function owns only the
// slice/hop/deadline mechanics common to all of them.
// ============================================================================

namespace {

/// Count one event in a ListenStats field without wrapping past 255.
void count_saturating(uint8_t &counter) {
  if (counter < UINT8_MAX)
    counter++;
}

/// Parse one received packet, hand it to `on_frame`, and translate an ACCEPT/ABORT disposition
/// into `outcome`; a frame the handler ignores is counted in `stats` when the caller asked for
/// counts. Factored out of listen()'s two reception sites to keep that function's cognitive
/// complexity under the clang-tidy threshold.
/// @return true if the listen should stop (ACCEPT or ABORT was returned); false to keep waiting.
bool dispatch_received_packet(const ReplyHandler &on_frame, const RadioRxPacket &packet, IoFrame &frame,
                              ListenOutcome &outcome, ListenStats *stats) {
  const bool parsed = parse(packet.data, packet.len, frame);
  switch (on_frame(parsed ? &frame : nullptr, packet)) {
    case ReplyDisposition::ACCEPT:
      outcome = ListenOutcome::ACCEPTED;
      return true;
    case ReplyDisposition::ABORT:
      outcome = ListenOutcome::ABORTED;
      return true;
    case ReplyDisposition::IGNORE:
      break;
  }
  if (stats != nullptr)
    count_saturating(stats->frames_ignored);
  return false;
}

/// Record a holding listen's failed reception. Called at the moment it happens, because the re-arm
/// that follows clears the radio capture that describes it.
void count_failed_reception(ListenStats *stats, const RadioDriver *radio) {
  if (stats == nullptr)
    return;
  count_saturating(stats->failed_receptions);
  stats->last_failed_irq = radio->get_last_capture().irq_status;
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
  // 0 means "ask the driver": no rotating call site in this project has a measured reason to
  // dwell differently from the chip's own retune-cost answer (RadioDriver::hop_dwell_ms()), so
  // all of them leave spec.dwell_ms at 0 and share one chip-specific knob instead of inventing a
  // second.
  const uint32_t dwell = resolve_dwell_ms(spec, rotating, radio, *this->tuning_);

  // A skipping listen's replies almost never return on the requesting channel (see ListenPolicy's
  // own doc comment for why), so it leaves that channel before its first dwell rather than
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
      if (dispatch_received_packet(on_frame, packet, frame, outcome, spec.stats))
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
    if (!rotating) {
      count_failed_reception(spec.stats, radio);  // HOLD: an early false is a failed reception, not a timeout.
      continue;
    }
    if (!preamble_or_sync_incoming(radio, spec)) {
      this->listen_hop_(skip, spec);
      continue;
    }
    // Preamble/sync seen at the dwell boundary: extend by linger_dwell_ms and give the frame its
    // air time instead of hopping — a short extension wait, not another full per-channel dwell.
    const uint32_t ext = std::min((uint32_t) (deadline - millis()), spec.linger_dwell_ms);
    ListenOutcome outcome = ListenOutcome::TIMED_OUT;
    if (radio->wait_for_packet(packet, ext) && dispatch_received_packet(on_frame, packet, frame, outcome, spec.stats))
      return outcome;
  }
  return ListenOutcome::TIMED_OUT;
}

// ============================================================================
// Inbound authentication
// ============================================================================

bool ExchangeEngine::authenticate_request(const IoFrame &request, uint32_t freq) {
  return this->authenticate_request(request, freq, this->key_for(request.src));
}

const uint8_t *ExchangeEngine::key_for(const uint8_t *node) const {
  if (this->device_key_provider_) {
    if (const uint8_t *device_key = this->device_key_provider_(node); device_key != nullptr)
      return device_key;
  }
  return this->system_key_;
}

bool ExchangeEngine::authenticate_request(const IoFrame &request, uint32_t freq, const uint8_t *key) {
  return this->challenge_sender(request, freq, key) == ChallengeResult::VERIFIED;
}

ExchangeEngine::ChallengeResult ExchangeEngine::challenge_sender(const IoFrame &request, uint32_t freq,
                                                                 const uint8_t *key) {
  uint8_t challenge[HMAC_SIZE];
  crypto::generate_challenge(challenge);
  return this->challenge_sender(request, freq, key, challenge, (*this->radio_ptr_)->response_preamble());
}

ExchangeEngine::ChallengeResult ExchangeEngine::challenge_sender(const IoFrame &request, uint32_t freq,
                                                                 const uint8_t *key, const uint8_t challenge[HMAC_SIZE],
                                                                 uint16_t preamble) {
  exchange::InboundAuthContext context;
  context.state = exchange::InboundAuthState::TX_CHALLENGE;
  this->record_debug(inbound_stage_name(context.state), 1, true);

  if (!create_challenge_req(context.challenge, request.src, this->node_id_, challenge)) {
    context.state = exchange::InboundAuthState::FAILED;
    this->record_debug(inbound_stage_name(context.state), 1, true);
    return ChallengeResult::NO_ANSWER;
  }
  if (!this->transmit_frame(context.challenge, freq, preamble)) {
    context.state = exchange::InboundAuthState::FAILED;
    this->record_debug(inbound_stage_name(context.state), 1, true);
    return ChallengeResult::NO_ANSWER;
  }

  context.state = exchange::InboundAuthState::WAIT_CHALLENGE_RESPONSE;
  this->record_debug(inbound_stage_name(context.state), 1, true);

  // A holding listen on the challenge channel: the device answers on it, and a frame from anyone
  // else (a neighbouring installation, the device repeating its own report) must not end the wait.
  ListenSpec spec;
  spec.window_ms = this->tuning_->exchange_response_wait_ms;
  spec.policy = ListenPolicy::HOLD_REQUEST_CHANNEL;
  ListenStats stats;
  spec.stats = &stats;

  RadioRxPacket packet{};
  IoFrame rx{};
  const ListenOutcome outcome = this->listen(spec, packet, rx, [&](const IoFrame *parsed, const RadioRxPacket &pkt) {
    // Each frame that does not end the wait is logged like one in a final-reply wait, so a log
    // shows what the wait heard when the answer never came.
    if (parsed == nullptr) {
      log_unparsable_frame("Unparsable challenge answer", 1, pkt);
      return ReplyDisposition::IGNORE;
    }
    if (decisions::classify_challenge_answer(*parsed, request.src, this->node_id_) ==
        decisions::ChallengeAnswerDisposition::ACCEPT)
      return ReplyDisposition::ACCEPT;
    log_exchange_frame("Ignored challenge answer", 1, *parsed, pkt.len);
    return ReplyDisposition::IGNORE;
  });
  this->note_final_wait_(stats);
  if (outcome != ListenOutcome::ACCEPTED) {
    context.state = exchange::InboundAuthState::FAILED;
    this->record_debug(inbound_stage_name(context.state), 1, true);
    return ChallengeResult::NO_ANSWER;
  }

  // Transcript is the *device's* own frame (cmd + data), not our challenge — the challenged
  // party authenticates what it said. Confirmed against real hardware bytes by the device-side
  // 0x3D in tests/corpus/captures/pairing/velux_kux100_pairing_full.yaml.
  // A 0x3D from the challenged device that does not verify is a wrong answer.
  if (!verify_challenge_resp(request, context.challenge.data, rx.data, key)) {
    context.state = exchange::InboundAuthState::FAILED;
    this->record_debug(inbound_stage_name(context.state), 1, true);
    return ChallengeResult::MISMATCH;
  }

  context.state = exchange::InboundAuthState::VERIFIED;
  this->record_debug(inbound_stage_name(context.state), 1, true);
  this->counters_.challenge_round_trips++;
  return ChallengeResult::VERIFIED;
}

}  // namespace home_io_control
}  // namespace esphome

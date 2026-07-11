/// @file exchange_engine.cpp
/// @brief Authenticated exchange engine — outbound and inbound protocol flows.
/// @ingroup hioc_hub
///
/// Implements ExchangeEngine: the retry loop, challenge-response
/// authentication, final-response wait, listen-before-talk transmit, and
/// frequency-hopping. Debug-snapshot helpers are also here.

#include "exchange_engine.h"

#include "hub_decisions.h"
#include "proto_commands.h"
#include "proto_crypto.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <algorithm>
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
           "cap_crc_err=%u cap_freq=%u cap_irq=0x%04X cap_pkt=0x%02X cap_reported_len=%u cap_frame_len=%u cap_rssi=%d",
           device_id, command_name(d.request_cmd), d.request_cmd, d.stage, d.tries, d.saw_challenge, d.capture_valid,
           d.capture_rx_done, d.capture_crc_error, d.capture_freq_hz, d.capture_irq_status, d.capture_packet_status,
           d.capture_reported_len, d.capture_frame_len, d.capture_rssi_dbm);
}

// ============================================================================
// Frequency hopping
// ============================================================================

void ExchangeEngine::reset_hop_timestamp() { this->last_hop_us_ = micros(); }

void ExchangeEngine::hop_frequency() {
  RadioDriver *radio = *this->radio_ptr_;
  uint32_t const cur = radio->get_current_freq();
  uint32_t next;
  switch (cur) {
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
  radio->change_frequency(next);
  this->last_hop_us_ = micros();
}

void ExchangeEngine::maybe_hop() {
  if ((micros() - this->last_hop_us_) > HOP_TIME_US)
    this->hop_frequency();
}

// ============================================================================
// Transmit with LBT
// ============================================================================

bool ExchangeEngine::transmit_frame(const IoFrame &frame, uint32_t freq, uint16_t preamble) {
  RadioDriver *radio = *this->radio_ptr_;
  uint8_t buf[FRAME_MAX_SIZE];
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

bool ExchangeEngine::send_and_receive(const IoFrame &request, IoFrame &response, uint32_t freq) {
  this->reset_debug(request.cmd);
  const uint16_t request_preamble = is_start(request) ? LONG_PREAMBLE : (*this->radio_ptr_)->response_preamble();

  for (uint8_t tries = 0; tries < EXCHANGE_RETRY_COUNT; tries++) {
    exchange::OutboundExchangeContext context;
    context.try_index = tries + 1;
    context.exchange_start_ms = millis();
    context.wait_ms = is_start(request) ? RESPONSE_START_WAIT_MS : RESPONSE_WAIT_MS;
    context.state = exchange::OutboundExchangeState::TX_REQUEST;

    if (tries > 0) {
      App.feed_wdt();
      delay(EXCHANGE_RETRY_DELAY_MS);
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
      return true;
    }

    if (!this->handle_authentication_(request, freq, context))
      continue;

    context.state = exchange::OutboundExchangeState::WAIT_FINAL_RESPONSE;
    this->record_debug(outbound_stage_name(context.state), context.try_index, true);
    auto final_disp = this->wait_for_final_response_(request, context);
    if (final_disp != decisions::ExchangeFinalResponseDisposition::ACCEPT)
      continue;

    context.state = exchange::OutboundExchangeState::SUCCESS;
    this->record_debug("success_auth", context.try_index, true);
    response = context.rx;
    return true;
  }

  return false;
}

// ============================================================================
// Outbound exchange step helpers
// ============================================================================

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
  RadioDriver *radio = *this->radio_ptr_;
  RadioRxPacket packet{};
  const uint32_t deadline = millis() + ctx.wait_ms;
  while ((int32_t) (deadline - millis()) > 0) {
    const uint32_t remaining = deadline - millis();
    const uint32_t slice = std::min<uint32_t>(remaining, radio->exchange_wait_slice_ms());
    if (!radio->wait_for_packet(packet, slice)) {
      if ((int32_t) (deadline - millis()) > 0)
        this->hop_frequency();
      continue;
    }
    if (!parse(packet.data, packet.len, ctx.rx)) {
      this->record_debug("first_parse_fail", ctx.try_index, false);
      continue;
    }
    auto disp = decisions::classify_exchange_first_response(request, ctx.rx);
    if (disp == decisions::ExchangeFirstResponseDisposition::IGNORE_UNRELATED) {
      this->record_debug("first_wrong_exchange", ctx.try_index, false);
      log_exchange_frame("Ignored first response", ctx.try_index, ctx.rx, packet.len);
      continue;
    }
    ctx.first_response_ms = millis();
    return disp;
  }
  ctx.state = exchange::OutboundExchangeState::FAILED;
  this->record_debug("wait_first_timeout", ctx.try_index, false);
  ESP_LOGI(TAG, "Try %d ended: no first response for cmd=%s(0x%02X) within %u ms", ctx.try_index,
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

  ESP_LOGI(TAG, "Auth challenge try=%d wait_ms=%u challenge=%02X%02X%02X%02X%02X%02X req_cmd=0x%02X req_len=%u",
           ctx.try_index, ctx.first_response_ms - ctx.exchange_start_ms, ctx.rx.data[0], ctx.rx.data[1], ctx.rx.data[2],
           ctx.rx.data[3], ctx.rx.data[4], ctx.rx.data[5], request.cmd, request.data_len);

  ctx.state = exchange::OutboundExchangeState::TX_AUTH_RESPONSE;
  this->record_debug(outbound_stage_name(ctx.state), ctx.try_index, true);
  if (!this->transmit_frame(auth_resp, freq, (*this->radio_ptr_)->response_preamble())) {
    ctx.state = exchange::OutboundExchangeState::FAILED;
    this->record_debug("tx_auth_failed", ctx.try_index, true);
    return false;
  }
  return true;
}

decisions::ExchangeFinalResponseDisposition ExchangeEngine::wait_for_final_response_(
    const IoFrame &request, exchange::OutboundExchangeContext &ctx) {
  RadioDriver *radio = *this->radio_ptr_;
  RadioRxPacket packet{};
  const uint32_t deadline = millis() + RESPONSE_AUTH_WAIT_MS;
  while ((int32_t) (deadline - millis()) > 0) {
    const uint32_t remaining = deadline - millis();
    const uint32_t slice = std::min<uint32_t>(remaining, radio->exchange_wait_slice_ms());
    if (!radio->wait_for_packet(packet, slice)) {
      if ((int32_t) (deadline - millis()) > 0)
        this->hop_frequency();
      continue;
    }
    if (!parse(packet.data, packet.len, ctx.rx)) {
      this->record_debug("final_parse_fail", ctx.try_index, true);
      continue;
    }
    if (is_valid_final_response(ctx.rx, request))
      return decisions::ExchangeFinalResponseDisposition::ACCEPT;
    this->record_debug("final_wrong_exchange", ctx.try_index, true);
    log_exchange_frame("Ignored final response", ctx.try_index, ctx.rx, packet.len);
  }
  ctx.state = exchange::OutboundExchangeState::FAILED;
  this->record_debug("wait_final_timeout", ctx.try_index, true);
  ESP_LOGI(TAG, "Try %d ended: no matching final response for cmd=%s(0x%02X) within %u ms", ctx.try_index,
           command_name(request.cmd), request.cmd, RESPONSE_AUTH_WAIT_MS);
  return decisions::ExchangeFinalResponseDisposition::IGNORE_UNRELATED;
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
  if (!radio->wait_for_packet(packet, RESPONSE_WAIT_MS)) {
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
  return true;
}

}  // namespace home_io_control
}  // namespace esphome

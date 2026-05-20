#include "hub_exchange.h"

#include "hub_decisions.h"
#include "hub_core.h"
#include "proto_commands.h"
#include "proto_crypto.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cstring>

namespace esphome {
namespace home_io_control {

static const char *const TAG = "home_io_control.exchange";

/// @file hub_exchange.cpp
/// @brief Outbound authenticated exchange state machine (non-pairing flows).
/// @ingroup hioc_hub
///
/// Implements IOHomeControlComponent::send_and_receive_() and its stepwise
/// helpers: transmit_request_(), wait_for_first_response_(),
/// handle_authentication_(), wait_for_final_response_(). These functions
/// encapsulate the retry loop, challenge-response authentication, and final
/// response handling for commands sent to paired devices.
///
/// The exchange logic is separated from hub_core.cpp to keep the main loop
/// and device-management concerns distinct from the protocol state machine.
/// Pairing flows (discovery/key exchange) live in hub_pairing.cpp.

namespace {

/// @brief Map OutboundExchangeState enum to a short string for debug logging.
/// @param state State value.
/// @return Null‑terminated string label.
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

/// @brief Map InboundAuthState enum to a short string for debug logging.
/// @param state State value.
/// @return Null‑terminated string label.
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

// response_wait_slice_ms provided by decisions namespace (hub_decisions.h)

/// Return preamble length for authenticated challenge response (0x3D).
///
/// SX1262 requires a longer preamble for the challenge response to improve
/// lock-on reliability in the RX->TX turn-around after receiving 0x3C. For
/// SX1276 we keep the short preamble because its IoHomeOn hardware path already
/// matches the baseline waveform.
///
/// @param radio Radio driver instance (used to query chip name).
/// @return Preamble length in symbol periods.
uint16_t auth_response_preamble(const RadioDriver *radio) {
  // SX1276 is the baseline waveform. The longer 0x3D preamble stays scoped to SX1262 so the radio-
  // specific lock-on workaround does not silently perturb the SX1276 behavior.
  return strcmp(radio->chip_name(), "sx1262") == 0 ? SX1262_AUTH_RESPONSE_PREAMBLE : SHORT_PREAMBLE;
}

/// Return the per-channel response dwell to use while waiting for exchange packets.
///
/// The generic 50 ms slice remains correct for the baseline protocol flow and for pairing.
/// SX1262 authenticated exchanges are the special case: after we send 0x3D, some devices reply
/// slightly later than 50 ms on the same channel. Keeping the receiver parked for 90 ms avoids
/// hopping away just before that final response arrives.
///
/// @param radio Radio driver instance (used to scope the workaround to SX1262 only).
/// @param remaining_ms Total time left in the current wait window.
/// @return Slice length in milliseconds, capped by the remaining wait budget.
uint32_t exchange_response_wait_slice_ms(const RadioDriver *radio, uint32_t remaining_ms) {
  const uint32_t max_slice =
      strcmp(radio->chip_name(), "sx1262") == 0 ? SX1262_EXCHANGE_RESPONSE_WAIT_SLICE_MS : RESPONSE_CHANNEL_WAIT_MS;
  return std::min<uint32_t>(remaining_ms, max_slice);
}

/// Check if frame is a 0x3D challenge response.
bool frame_is_challenge_response(const IoFrame &frame) { return frame.cmd == CMD_CHALLENGE_RESP; }

/// Log an exchanged frame with context (stage, try index, length).
///
/// Used to trace both first responses and final responses. Intended for
/// debugging packet flows where unrelated frames are ignored.
///
/// @param stage  String label for the current stage (e.g., "first_response").
/// @param tries  Attempt number (1‑based).
/// @param frame Parsed IoFrame to log.
/// @param len   Length of raw packet (for correlation with capture info).
void log_exchange_frame(const char *stage, int tries, const IoFrame &frame, uint8_t len) {
  ESP_LOGD(TAG, "%s try=%d cmd=0x%02X src=%02X%02X%02X dst=%02X%02X%02X len=%u", stage, tries, frame.cmd, frame.src[0],
           frame.src[1], frame.src[2], frame.dst[0], frame.dst[1], frame.dst[2], len);
}

/// Determine if a candidate frame is a valid final response for the request.
///
/// Used by `wait_for_final_response_()` to accept a frame. The check validates
/// endpoint matching (dst == request.src, src == request.dst) via the decisions
/// namespace.
///
/// @param candidate Parsed IoFrame from the device.
/// @param request  Original outbound request.
/// @return true if candidate is acceptable as final response; false otherwise.
bool is_valid_final_response(const IoFrame &candidate, const IoFrame &request) {
  return decisions::classify_exchange_final_response(request, candidate) ==
         decisions::ExchangeFinalResponseDisposition::ACCEPT;
}

}  // namespace

bool IOHomeControlComponent::send_and_receive_(const IoFrame &request, IoFrame &response, uint32_t freq) {
  this->busy_ = true;
  this->reset_exchange_debug_(request.cmd);
  const uint16_t request_preamble = is_start(request) ? LONG_PREAMBLE : SHORT_PREAMBLE;

  for (int tries = 0; tries < EXCHANGE_RETRY_COUNT; tries++) {
    exchange::OutboundExchangeContext context;
    context.try_index = tries + 1;
    context.exchange_start_ms = millis();
    context.wait_ms = is_start(request) ? RESPONSE_START_WAIT_MS : RESPONSE_WAIT_MS;
    context.state = exchange::OutboundExchangeState::TX_REQUEST;

    if (tries > 0) {
      App.feed_wdt();
      delay(EXCHANGE_RETRY_DELAY_MS);
    }

    // Step 1: Transmit request
    if (!this->transmit_request_(request, freq, request_preamble, context)) {
      continue;  // state already set to FAILED by helper
    }

    // Step 2: Wait for first response
    context.state = exchange::OutboundExchangeState::WAIT_FIRST_RESPONSE;
    this->record_exchange_debug_(outbound_stage_name(context.state), context.try_index, false);
    auto first_disp = this->wait_for_first_response_(request, context);
    if (first_disp == decisions::ExchangeFirstResponseDisposition::IGNORE_UNRELATED) {
      continue;  // timeout or no valid response
    }
    if (first_disp == decisions::ExchangeFirstResponseDisposition::COMPLETE_DIRECT) {
      context.state = exchange::OutboundExchangeState::SUCCESS;
      this->record_exchange_debug_("success_direct", context.try_index, false);
      response = context.rx;
      this->busy_ = false;
      return true;
    }

    // Step 3: Handle authentication challenge
    if (!this->handle_authentication_(request, freq, context)) {
      continue;
    }

    // Step 4: Wait for final authenticated response
    context.state = exchange::OutboundExchangeState::WAIT_FINAL_RESPONSE;
    this->record_exchange_debug_(outbound_stage_name(context.state), context.try_index, true);
    auto final_disp = this->wait_for_final_response_(request, context);
    if (final_disp != decisions::ExchangeFinalResponseDisposition::ACCEPT) {
      continue;
    }

    // Success
    context.state = exchange::OutboundExchangeState::SUCCESS;
    this->record_exchange_debug_("success_auth", context.try_index, true);
    response = context.rx;
    this->busy_ = false;
    return true;
  }

  this->busy_ = false;
  return false;
}

// --- Outbound exchange helper implementations ---

/// Transmit the initial request frame and update exchange context on failure.
///
/// This helper isolates the one-shot TX operation from the retry loop. It does
/// NOT implement retries itself — the orchestrator (`send_and_receive_`) calls
/// this repeatedly until success or retry exhaustion. On failure we mark the
/// context state as FAILED and record debug info; success returns true.
bool IOHomeControlComponent::transmit_request_(const IoFrame &request, uint32_t freq, uint16_t preamble,
                                               exchange::OutboundExchangeContext &ctx) {
  if (!this->transmit_frame_(request, freq, preamble)) {
    ctx.state = exchange::OutboundExchangeState::FAILED;
    this->record_exchange_debug_("tx_request_failed", ctx.try_index, false);
    return false;
  }
  return true;
}

/// Wait for the first response packet within the configured timeout window.
///
/// Listens on the current RF channel, hopping to the next channel after each
/// slice if no packet arrives. Parses incoming frames and classifies them via
/// `decisions::classify_exchange_first_response()`:
///   - COMPLETE_DIRECT  → matching non‑challenge frame (operation complete, no auth)
///   - REQUIRE_AUTH     → matching 0x3C challenge (device demands authentication)
///   - IGNORE_UNRELATED  → all others (timeout, wrong endpoints, unparsable)
decisions::ExchangeFirstResponseDisposition IOHomeControlComponent::wait_for_first_response_(
    const IoFrame &request, exchange::OutboundExchangeContext &ctx) {
  RadioRxPacket packet{};
  const uint32_t deadline = millis() + ctx.wait_ms;
  while ((int32_t) (deadline - millis()) > 0) {
    const uint32_t remaining = deadline - millis();
    const uint32_t slice = exchange_response_wait_slice_ms(this->radio_, remaining);
    if (!this->radio_->wait_for_packet(packet, slice)) {
      if ((int32_t) (deadline - millis()) > 0)
        this->hop_frequency_();
      continue;
    }
    if (!parse(packet.data, packet.len, ctx.rx)) {
      this->record_exchange_debug_("first_parse_fail", ctx.try_index, false);
      continue;
    }
    auto disp = decisions::classify_exchange_first_response(request, ctx.rx);
    if (disp == decisions::ExchangeFirstResponseDisposition::IGNORE_UNRELATED) {
      this->record_exchange_debug_("first_wrong_exchange", ctx.try_index, false);
      log_exchange_frame("Ignored first response", ctx.try_index, ctx.rx, packet.len);
      continue;
    }
    ctx.first_response_ms = millis();
    return disp;
  }
  ctx.state = exchange::OutboundExchangeState::FAILED;
  this->record_exchange_debug_("wait_first_timeout", ctx.try_index, false);
  ESP_LOGI(TAG, "Try %d ended: no first response for cmd=0x%02X within %u ms", ctx.try_index, request.cmd, ctx.wait_ms);
  return decisions::ExchangeFirstResponseDisposition::IGNORE_UNRELATED;
}

/// Handle device authentication challenge (0x3C → 0x3D exchange).
///
/// When the first response is a challenge request (0x3C), this helper builds
/// the HMAC challenge response using `create_challenge_resp()` and transmits
/// it with the SX1262‑specific longer preamble. The exchange context is
/// updated with the BUILD_AUTH_RESPONSE and TX_AUTH_RESPONSE states.
bool IOHomeControlComponent::handle_authentication_(const IoFrame &request, uint32_t freq,
                                                    exchange::OutboundExchangeContext &ctx) {
  ctx.saw_challenge = true;
  ctx.state = exchange::OutboundExchangeState::BUILD_AUTH_RESPONSE;
  this->record_exchange_debug_(outbound_stage_name(ctx.state), ctx.try_index, true);

  IoFrame auth_resp;
  if (!create_challenge_resp(auth_resp, request.dst, this->node_id_, ctx.rx.data, request, this->system_key_)) {
    ctx.state = exchange::OutboundExchangeState::FAILED;
    this->record_exchange_debug_("auth_build_failed", ctx.try_index, true);
    return false;
  }

  ESP_LOGI(TAG, "Auth challenge try=%d wait_ms=%u challenge=%02X%02X%02X%02X%02X%02X req_cmd=0x%02X req_len=%u",
           ctx.try_index, ctx.first_response_ms - ctx.exchange_start_ms, ctx.rx.data[0], ctx.rx.data[1], ctx.rx.data[2],
           ctx.rx.data[3], ctx.rx.data[4], ctx.rx.data[5], request.cmd, request.data_len);

  ctx.state = exchange::OutboundExchangeState::TX_AUTH_RESPONSE;
  this->record_exchange_debug_(outbound_stage_name(ctx.state), ctx.try_index, true);
  if (!this->transmit_frame_(auth_resp, freq, auth_response_preamble(this->radio_))) {
    ctx.state = exchange::OutboundExchangeState::FAILED;
    this->record_exchange_debug_("tx_auth_failed", ctx.try_index, true);
    return false;
  }
  return true;
}

/// Wait for the final (authenticated) response after challenge has been answered.
///
/// After sending the 0x3D challenge response, the device will reply with the
/// actual command response (e.g. 0x04 status) signed using the shared system
/// key. This helper loops within `RESPONSE_AUTH_WAIT_MS`, hopping channels on
/// each slice, parsing and validating that the frame matches the original
/// request endpoints. Non‑matching frames are logged and ignored.
decisions::ExchangeFinalResponseDisposition IOHomeControlComponent::wait_for_final_response_(
    const IoFrame &request, exchange::OutboundExchangeContext &ctx) {
  RadioRxPacket packet{};
  const uint32_t deadline = millis() + RESPONSE_AUTH_WAIT_MS;
  while ((int32_t) (deadline - millis()) > 0) {
    const uint32_t remaining = deadline - millis();
    const uint32_t slice = exchange_response_wait_slice_ms(this->radio_, remaining);
    if (!this->radio_->wait_for_packet(packet, slice)) {
      if ((int32_t) (deadline - millis()) > 0)
        this->hop_frequency_();
      continue;
    }
    if (!parse(packet.data, packet.len, ctx.rx)) {
      this->record_exchange_debug_("final_parse_fail", ctx.try_index, true);
      continue;
    }
    if (is_valid_final_response(ctx.rx, request)) {
      return decisions::ExchangeFinalResponseDisposition::ACCEPT;
    }
    this->record_exchange_debug_("final_wrong_exchange", ctx.try_index, true);
    log_exchange_frame("Ignored final response", ctx.try_index, ctx.rx, packet.len);
  }
  ctx.state = exchange::OutboundExchangeState::FAILED;
  this->record_exchange_debug_("wait_final_timeout", ctx.try_index, true);
  ESP_LOGI(TAG, "Try %d ended: no matching final response for cmd=0x%02X within %u ms", ctx.try_index, request.cmd,
           RESPONSE_AUTH_WAIT_MS);
  return decisions::ExchangeFinalResponseDisposition::IGNORE_UNRELATED;
}

bool IOHomeControlComponent::authenticate_request_(const IoFrame &request, uint32_t freq) {
  exchange::InboundAuthContext context;
  context.state = exchange::InboundAuthState::TX_CHALLENGE;
  this->record_exchange_debug_(inbound_stage_name(context.state), 1, true);

  // Inbound authentication: send a 0x3C challenge and verify the device's 0x3D HMAC response
  // against the original command byte plus payload.
  if (!create_challenge_req(context.challenge, request.src, this->node_id_)) {
    context.state = exchange::InboundAuthState::FAILED;
    this->record_exchange_debug_(inbound_stage_name(context.state), 1, true);
    return false;
  }
  if (!this->transmit_frame_(context.challenge, freq, SHORT_PREAMBLE)) {
    context.state = exchange::InboundAuthState::FAILED;
    this->record_exchange_debug_(inbound_stage_name(context.state), 1, true);
    return false;
  }

  context.state = exchange::InboundAuthState::WAIT_CHALLENGE_RESPONSE;
  this->record_exchange_debug_(inbound_stage_name(context.state), 1, true);

  RadioRxPacket packet{};
  if (!this->radio_->wait_for_packet(packet, RESPONSE_WAIT_MS)) {
    context.state = exchange::InboundAuthState::FAILED;
    this->record_exchange_debug_(inbound_stage_name(context.state), 1, true);
    return false;
  }

  IoFrame rx;
  if (!parse(packet.data, packet.len, rx) || !frame_is_challenge_response(rx)) {
    context.state = exchange::InboundAuthState::FAILED;
    this->record_exchange_debug_(inbound_stage_name(context.state), 1, true);
    return false;
  }

  uint8_t frame_data[FRAME_MAX_SIZE];
  frame_data[0] = request.cmd;
  memcpy(frame_data + 1, request.data, request.data_len);
  if (!crypto::verify_hmac(frame_data, request.data_len + 1, rx.data, context.challenge.data, this->system_key_)) {
    context.state = exchange::InboundAuthState::FAILED;
    this->record_exchange_debug_(inbound_stage_name(context.state), 1, true);
    return false;
  }

  context.state = exchange::InboundAuthState::VERIFIED;
  this->record_exchange_debug_(inbound_stage_name(context.state), 1, true);
  return true;
}

}  // namespace home_io_control
}  // namespace esphome
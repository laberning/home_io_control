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

namespace {

const char *const TAG = "home_io_control";

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

uint32_t response_wait_slice_ms(uint32_t remaining_ms) {
  return std::min<uint32_t>(remaining_ms, RESPONSE_CHANNEL_WAIT_MS);
}

uint16_t auth_response_preamble(const RadioDriver *radio) {
  // SX1276 is the baseline waveform. The longer 0x3D preamble stays scoped to SX1262 so the radio-
  // specific lock-on workaround does not silently perturb the SX1276 behavior.
  return strcmp(radio->chip_name(), "sx1262") == 0 ? SX1262_AUTH_RESPONSE_PREAMBLE : SHORT_PREAMBLE;
}

bool frame_is_challenge_response(const IoFrame &frame) { return frame.cmd == CMD_CHALLENGE_RESP; }

void log_exchange_frame(const char *stage, int tries, const IoFrame &frame, uint8_t len) {
  ESP_LOGD(TAG, "%s try=%d cmd=0x%02X src=%02X%02X%02X dst=%02X%02X%02X len=%u", stage, tries, frame.cmd, frame.src[0],
           frame.src[1], frame.src[2], frame.dst[0], frame.dst[1], frame.dst[2], len);
}

}  // namespace

bool IOHomeControlComponent::send_and_receive_(const IoFrame &request, IoFrame &response, uint32_t freq) {
  this->busy_ = true;
  this->reset_exchange_debug_(request.cmd);
  const uint16_t request_preamble = is_start(request) ? LONG_PREAMBLE : SHORT_PREAMBLE;

  // Retry loop with explicit state context. Each iteration is self-contained: transmit request,
  // wait for first response, optionally authenticate, then wait for the final response.
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

    if (!this->transmit_frame_(request, freq, request_preamble)) {
      context.state = exchange::OutboundExchangeState::FAILED;
      this->record_exchange_debug_("tx_request_failed", context.try_index, false);
      continue;
    }

    context.state = exchange::OutboundExchangeState::WAIT_FIRST_RESPONSE;
    this->record_exchange_debug_(outbound_stage_name(context.state), context.try_index, false);

    RadioRxPacket packet{};
    const uint32_t first_deadline_ms = millis() + context.wait_ms;
    bool got_first_response = false;
    while ((int32_t) (first_deadline_ms - millis()) > 0) {
      uint32_t remaining_ms = first_deadline_ms - millis();
      if (!this->radio_->wait_for_packet(packet, response_wait_slice_ms(remaining_ms))) {
        if ((int32_t) (first_deadline_ms - millis()) > 0)
          this->hop_frequency_();
        continue;
      }
      if (!parse(packet.data, packet.len, context.rx)) {
        this->record_exchange_debug_("first_parse_fail", context.try_index, false);
        continue;
      }
      // Ignore foreign traffic and keep waiting rather than aborting the exchange.
      const auto first_disposition = decisions::classify_exchange_first_response(request, context.rx);
      if (first_disposition == decisions::ExchangeFirstResponseDisposition::IGNORE_UNRELATED) {
        this->record_exchange_debug_("first_wrong_exchange", context.try_index, false);
        log_exchange_frame("Ignored first response", tries + 1, context.rx, packet.len);
        continue;
      }
      context.first_response_ms = millis();
      got_first_response = true;
      break;
    }
    if (!got_first_response) {
      context.state = exchange::OutboundExchangeState::FAILED;
      this->record_exchange_debug_("wait_first_timeout", context.try_index, false);
      ESP_LOGI(TAG, "Try %d ended: no first response for cmd=0x%02X within %u ms", tries + 1, request.cmd,
               context.wait_ms);
      continue;
    }

    const auto first_disposition = decisions::classify_exchange_first_response(request, context.rx);
    if (first_disposition == decisions::ExchangeFirstResponseDisposition::COMPLETE_DIRECT) {
      context.state = exchange::OutboundExchangeState::SUCCESS;
      this->record_exchange_debug_("success_direct", context.try_index, false);
      memcpy(&response, &context.rx, sizeof(IoFrame));
      this->busy_ = false;
      return true;
    }

    context.saw_challenge = true;
    context.state = exchange::OutboundExchangeState::BUILD_AUTH_RESPONSE;
    this->record_exchange_debug_(outbound_stage_name(context.state), context.try_index, true);

    IoFrame auth_resp;
    if (!create_challenge_resp(auth_resp, request.dst, this->node_id_, context.rx.data, request, this->system_key_)) {
      context.state = exchange::OutboundExchangeState::FAILED;
      this->record_exchange_debug_("auth_build_failed", context.try_index, true);
      continue;
    }

    ESP_LOGI(TAG, "Auth challenge try=%d wait_ms=%u challenge=%02X%02X%02X%02X%02X%02X req_cmd=0x%02X req_len=%u",
             tries + 1, context.first_response_ms - context.exchange_start_ms, context.rx.data[0], context.rx.data[1],
             context.rx.data[2], context.rx.data[3], context.rx.data[4], context.rx.data[5], request.cmd,
             request.data_len);

    context.state = exchange::OutboundExchangeState::TX_AUTH_RESPONSE;
    this->record_exchange_debug_(outbound_stage_name(context.state), context.try_index, true);
    if (!this->transmit_frame_(auth_resp, freq, auth_response_preamble(this->radio_))) {
      context.state = exchange::OutboundExchangeState::FAILED;
      this->record_exchange_debug_("tx_auth_failed", context.try_index, true);
      continue;
    }

    context.state = exchange::OutboundExchangeState::WAIT_FINAL_RESPONSE;
    this->record_exchange_debug_(outbound_stage_name(context.state), context.try_index, true);
    const uint32_t final_deadline_ms = millis() + RESPONSE_AUTH_WAIT_MS;
    bool got_final_response = false;
    while ((int32_t) (final_deadline_ms - millis()) > 0) {
      uint32_t remaining_ms = final_deadline_ms - millis();
      if (!this->radio_->wait_for_packet(packet, response_wait_slice_ms(remaining_ms))) {
        if ((int32_t) (final_deadline_ms - millis()) > 0)
          this->hop_frequency_();
        continue;
      }
      if (!parse(packet.data, packet.len, context.rx)) {
        this->record_exchange_debug_("final_parse_fail", context.try_index, true);
        continue;
      }
      // Ignore unrelated traffic while waiting for the authenticated reply.
      if (decisions::classify_exchange_final_response(request, context.rx) !=
          decisions::ExchangeFinalResponseDisposition::ACCEPT) {
        this->record_exchange_debug_("final_wrong_exchange", context.try_index, true);
        log_exchange_frame("Ignored final response", tries + 1, context.rx, packet.len);
        continue;
      }
      got_final_response = true;
      break;
    }
    if (!got_final_response) {
      context.state = exchange::OutboundExchangeState::FAILED;
      this->record_exchange_debug_("wait_final_timeout", context.try_index, true);
      ESP_LOGI(TAG, "Try %d ended: no matching final response for cmd=0x%02X within %u ms", tries + 1, request.cmd,
               RESPONSE_AUTH_WAIT_MS);
      continue;
    }

    context.state = exchange::OutboundExchangeState::SUCCESS;
    this->record_exchange_debug_("success_auth", context.try_index, true);
    memcpy(&response, &context.rx, sizeof(IoFrame));
    this->busy_ = false;
    return true;
  }

  this->busy_ = false;
  return false;
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
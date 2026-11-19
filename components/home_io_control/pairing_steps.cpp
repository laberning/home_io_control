/// @file pairing_steps.cpp
/// @brief Steps the pairing flow's parts share: entering a state, the start preamble, noting a refusal.
/// @ingroup hioc_hub

#include "pairing_steps.h"

#include "proto_commands.h"
#include "esphome/core/log.h"

#include <algorithm>

namespace esphome {
namespace home_io_control {

namespace {
const char *const TAG = "home_io_control";
}  // namespace

void enter_pairing_state(ExchangeEngine &engine, PairingTelemetry &telemetry, pairing::PairingContext &context,
                         pairing::PairingState state, uint8_t try_number, bool saw_challenge) {
  context.state = state;
  engine.record_debug(pairing_stage_name(state), try_number, saw_challenge);
  telemetry.set_phase(state);
}

uint16_t pairing_start_preamble(const ExchangeEngine &engine, const TuningConfig &tuning, const IoFrame &frame) {
  return std::min(engine.request_preamble_for(frame),
                  engine.preamble_bytes_for_ms(effective_discovery_preamble_ms(tuning)));
}

void note_key_refusal(PairingTelemetry &telemetry, pairing::PairingContext &context, const IoFrame &reply,
                      const char *step) {
  const uint8_t code = reply.data_len > 0 ? reply.data[0] : 0;
  context.key_refused = true;
  context.key_refusal_code = code;
  telemetry.set_key_refusal(code);
  ESP_LOGW(TAG, "%s: device %s refused with 0x%02X %s (%s; code %u, state %u)", step, context.device_id.c_str(), code,
           command_result_name(code), command_result_description(code), error_resp_code(code), error_resp_state(code));
}

}  // namespace home_io_control
}  // namespace esphome

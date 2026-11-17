/// @file pairing_key_holders.cpp
/// @brief Key-holder search: find the devices that hold a key and register the ones that can be controlled.
/// @ingroup hioc_hub

#include "pairing_key_holders.h"

#include "decisions_pairing.h"
#include "log_helpers.h"
#include "pairing_steps.h"
#include "proto_codecs.h"
#include "proto_commands.h"
#include "proto_device_types.h"
#include "proto_heating.h"
#include "proto_private_registers.h"
#include "esphome/core/log.h"

#include <cmath>
#include <cstring>
#include <optional>
#include <string>

namespace esphome {
namespace home_io_control {

namespace {
const char *const TAG = "home_io_control";
}  // namespace

KeyHolderSearch::KeyHolderSearch(RadioDriver **radio_ptr, const uint8_t *node_id, const uint8_t *system_key,
                                 const TuningConfig *tuning, ExchangeEngine &engine, DeviceRegistry &registry,
                                 PairingTelemetry &telemetry)
    : radio_ptr_(radio_ptr),
      node_id_(node_id),
      system_key_(system_key),
      tuning_(tuning),
      engine_(engine),
      registry_(registry),
      telemetry_(telemetry) {}

KeyHolderSearch::Result KeyHolderSearch::search_and_register(pairing::PairingContext &context, const uint8_t *key,
                                                             bool device_key, bool key_proven) {
  // Every device that holds the key answers a roll-call built with it. For an Atlantic AGILIA radiator
  // that is the heater at the next address up, not the node that handed the key over.
  enter_pairing_state(engine_, telemetry_, context, pairing::PairingState::SCAN_PULLED_KEY, 1, true);
  ScanState state{};
  // An Atlantic radiator's heater answers a direct read every time but a roll-call only now and then
  // (about one request in four), so it is asked first; the roll-call is the fallback, and the only
  // search for any other device.
  if (!probe_heater_at_next_address_(context, key, device_key, state)) {
    uint8_t attempt = 0;
    const uint8_t total_attempts = count_enabled_passes(tuning_->scan_power_classes) * SCAN_CHANNEL_COUNT;
    ESP_LOGI(TAG, "Roll-call with key check value %s (%s %s handed over)", detail::format_key_check_value(key).c_str(),
             device_key ? "the key" : "the hub key, which", context.device_id.c_str());
    if (run_roll_call_sweep(engine_, node_id_, key, /*key_slot=*/device_key ? 1 : 0, *tuning_, state, attempt,
                            total_attempts) == RollCallAttemptResult::BUILD_FAILED) {
      ESP_LOGW(TAG, "Could not build the roll-call under the pulled key");
    }
  }

  // One report for the device log only: with a device key it carries the key, so the pairing summary, the
  // Last Pairing Result sensor and every Home Assistant event keep to the key check value. A hub-key report
  // carries no key and has no banner.
  Result found{};
  found.answered = state.count;
  // An unproven key that no device answered under proves nothing: no report, so it is never printed.
  if (!key_proven && found.answered == 0)
    return found;

  const PulledKeyReportInfo info{context.device_id, context.device.type, context.device.subtype, key,
                                 system_key_,       key_proven};
  if (device_key) {
    ESP_LOGW(TAG, "========================================");
    detail::log_multiline_result(TAG, /*is_warning=*/true, /*prefix=*/"",
                                 build_pulled_key_report(info, state, registry_));
    ESP_LOGW(TAG, "========================================");
  } else if (state.count != 0) {
    detail::log_multiline_result(TAG, /*is_warning=*/false, /*prefix=*/"",
                                 build_hub_key_pull_report(info, state, registry_));
  }

  // Register each device the report offers a block for, so it can be controlled until the YAML is
  // flashed: with the key it answered under when that is a device key, with the hub key otherwise. A
  // device the YAML declares keeps its record; one an earlier pairing in this boot added is replaced, so
  // pairing it again counts as paired. One of a type no platform drives yet is reported but not registered.
  for (uint8_t i = 0; i < state.count; i++) {
    const ScanResponder &responder = state.responders[i];
    const std::string responder_id = node_id_to_string(responder.src);
    if (registry_.get_declared(responder_id) != nullptr || !device_has_platform(responder.type, responder.subtype))
      continue;
    IoDevice device{};
    memcpy(device.node_id, responder.src, NODE_ID_SIZE);
    device.type = responder.type;
    device.subtype = responder.subtype;
    device.low_power = responder.has_extended && discovery_power_save_mode(responder.flags) == POWER_SAVE_LOW_POWER;
    device.has_system_key = device_key;
    if (device_key)
      memcpy(device.system_key, key, AES_KEY_SIZE);
    device.last_seen_ms = millis();
    device.registered_by_pairing = true;
    registry_.put(responder_id, device);
    found.registered++;
    ESP_LOGI(TAG, "Pairing: registered %s (%s) until reboot; paste its block above to keep it", responder_id.c_str(),
             format_device_kind(responder.type, responder.subtype).c_str());
  }
  return found;
}

bool KeyHolderSearch::probe_heater_at_next_address_(const pairing::PairingContext &context, const uint8_t *key,
                                                    bool device_key, ScanState &state) {
  uint8_t heater[NODE_ID_SIZE];
  const std::optional<decisions::DeviceKind> heater_kind =
      decisions::heater_behind_pairing_node(context.device.type, context.discovery_manufacturer);
  if (!heater_kind || !decisions::next_node_id(context.device.node_id, heater))
    return false;
  const std::string heater_id = node_id_to_string(heater);
  // A YAML entry keeps its own key; the roll-call reports it against that.
  if (registry_.get_declared(heater_id) != nullptr)
    return false;

  // The maximum setpoint is a read-only register every such heater holds, so reading it changes nothing.
  uint8_t payload[PRIVATE_REGISTER_PAYLOAD_MAX_SIZE];
  const size_t payload_len = encode_heating_read(HeatingFunction::MAX_SETPOINT, payload);
  IoFrame request{};
  IoFrame response{};
  float max_setpoint_c = NAN;
  ESP_LOGI(TAG, "Heater probe: reading %s, the address after %s, under key check value %s", heater_id.c_str(),
           context.device_id.c_str(), detail::format_key_check_value(key).c_str());
  const bool answered =
      payload_len != 0 && create_write_private(request, node_id_, heater, /*low_power=*/false, payload, payload_len) &&
      // The candidate has no registry record yet (the caller registers it if it answered), so the
      // exchange is given the key to answer its challenge under.
      engine_.send_and_receive(request, response, FREQ_CH2, ExchangeOptions{}.with_key(key).on_fixed_channel()) ==
          ExchangeOutcome::SUCCESS_WITH_RESPONSE &&
      decode_heating_reply(response, HeatingFunction::MAX_SETPOINT, max_setpoint_c);
  const int16_t rssi = radio_()->get_last_capture().rssi_dbm;
  if (!answered) {
    ESP_LOGI(TAG, "Heater probe: %s did not answer; looking for devices with a roll-call instead", heater_id.c_str());
    return false;
  }
  ESP_LOGI(TAG, "Heater probe: %s answered (maximum setpoint %.1f C); no roll-call needed", heater_id.c_str(),
           max_setpoint_c);

  // Only the register protocol answers this read, and only the heater with an adjustable setpoint
  // speaks it; manufacturer and flags come from no reply of its own, so the report omits them.
  ScanResponder &responder = state.responders[0];
  memcpy(responder.src, heater, NODE_ID_SIZE);
  responder.type = heater_kind->type;
  responder.subtype = heater_kind->subtype;
  responder.rssi_dbm = rssi;
  responder.manufacturer = context.discovery_manufacturer;
  responder.flags = 0;
  responder.has_extended = false;
  responder.metadata_complete = true;
  responder.key_slot = device_key ? 1 : 0;
  state.count = 1;
  state.truncated = false;
  return true;
}

}  // namespace home_io_control
}  // namespace esphome

#include "hub_pairing.h"

#include "hub_decisions.h"
#include "hub_core.h"
#include "proto_commands.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cstring>

namespace esphome {
namespace home_io_control {

namespace {

const char *const TAG = "home_io_control";

const char *pairing_stage_name(pairing::PairingState state) {
  switch (state) {
    case pairing::PairingState::IDLE:
      return "idle";
    case pairing::PairingState::TX_DISCOVER:
      return "tx_discover";
    case pairing::PairingState::WAIT_DISCOVER_RESPONSE:
      return "wait_discover_response";
    case pairing::PairingState::TX_KEY_INIT:
      return "tx_key_init";
    case pairing::PairingState::WAIT_KEY_CHALLENGE:
      return "wait_key_challenge";
    case pairing::PairingState::TX_KEY_TRANSFER:
      return "tx_key_transfer";
    case pairing::PairingState::WAIT_KEY_CONFIRM:
      return "wait_key_confirm";
    case pairing::PairingState::PERSIST_DEVICE:
      return "persist_device";
    case pairing::PairingState::COMPLETE:
      return "complete";
    case pairing::PairingState::FAILED:
    default:
      return "failed";
  }
}

uint32_t response_wait_slice_ms(uint32_t remaining_ms) {
  return std::min<uint32_t>(remaining_ms, RESPONSE_CHANNEL_WAIT_MS);
}

bool frame_is_key_confirm(const IoFrame &frame) { return frame.cmd == CMD_KEY_CONFIRM; }

}  // namespace

bool IOHomeControlComponent::discover_and_pair() {
  if (!this->initialized_)
    return false;
  ESP_LOGI(TAG, "Starting device discovery...");

  this->busy_ = true;
  pairing::PairingContext context;

  context.state = pairing::PairingState::TX_DISCOVER;
  this->record_exchange_debug_(pairing_stage_name(context.state), 1, false);
  if (!create_discover(context.req, this->node_id_) || !this->transmit_frame_(context.req, FREQ_CH2, LONG_PREAMBLE)) {
    this->busy_ = false;
    return false;
  }

  context.state = pairing::PairingState::WAIT_DISCOVER_RESPONSE;
  this->record_exchange_debug_(pairing_stage_name(context.state), 1, false);
  bool saw_discovery_traffic = false;
  bool got_discovery_response = false;
  const uint32_t discovery_deadline_ms = millis() + 2000;
  while ((int32_t) (discovery_deadline_ms - millis()) > 0) {
    const uint32_t remaining_ms = discovery_deadline_ms - millis();
    if (!this->radio_->wait_for_packet(context.packet, response_wait_slice_ms(remaining_ms)))
      continue;
    saw_discovery_traffic = true;
    if (!parse(context.packet.data, context.packet.len, context.rx))
      continue;
    if (decisions::classify_pairing_discovery_response(context.rx) != decisions::PairingDiscoveryDisposition::ACCEPT)
      continue;
    got_discovery_response = true;
    break;
  }
  if (!got_discovery_response) {
    ESP_LOGW(TAG, saw_discovery_traffic ? "No valid discovery response received" : "No device responded to discovery");
    this->busy_ = false;
    return false;
  }

  memcpy(context.device.node_id, context.rx.src, NODE_ID_SIZE);
  if (context.rx.data_len >= 2) {
    context.device.type = static_cast<DeviceType>(context.rx.data[0] << 2 | context.rx.data[1] >> 6);
    context.device.subtype = context.rx.data[1] & 0x3F;
    if (default_inverted_for_type(context.device.type))
      context.device.inverted = true;
  }
  context.device.position = UNKNOWN_POSITION;
  context.device.target = UNKNOWN_POSITION;
  context.device.is_stopped = true;
  context.device_id = node_id_to_string(context.device.node_id);

  ESP_LOGI(TAG, "Discovered device %s (type=%s/%u class=%s profile=%s), starting key exchange...",
           context.device_id.c_str(), device_type_name(context.device.type), (uint8_t) context.device.type,
           device_capability_class_name(context.device.type), device_operation_profile_name(context.device.type));

  context.state = pairing::PairingState::TX_KEY_INIT;
  this->record_exchange_debug_(pairing_stage_name(context.state), 1, false);
  if (!create_key_init(context.key_init, this->node_id_, context.device.node_id) ||
      !this->transmit_frame_(context.key_init, FREQ_CH2, LONG_PREAMBLE)) {
    this->busy_ = false;
    return false;
  }

  context.state = pairing::PairingState::WAIT_KEY_CHALLENGE;
  this->record_exchange_debug_(pairing_stage_name(context.state), 1, true);
  bool saw_key_traffic = false;
  bool got_key_challenge = false;
  const uint32_t key_deadline_ms = millis() + 500;
  while ((int32_t) (key_deadline_ms - millis()) > 0) {
    const uint32_t remaining_ms = key_deadline_ms - millis();
    if (!this->radio_->wait_for_packet(context.packet, response_wait_slice_ms(remaining_ms)))
      continue;
    saw_key_traffic = true;
    if (!parse(context.packet.data, context.packet.len, context.rx))
      continue;
    if (decisions::classify_pairing_key_challenge(context.rx, context.device.node_id, this->node_id_) !=
        decisions::PairingKeyChallengeDisposition::ACCEPT)
      continue;
    got_key_challenge = true;
    break;
  }
  if (!got_key_challenge) {
    ESP_LOGW(TAG,
             saw_key_traffic ? "Key exchange: no valid challenge received" : "Key exchange: no challenge received");
    this->busy_ = false;
    return false;
  }

  context.state = pairing::PairingState::TX_KEY_TRANSFER;
  this->record_exchange_debug_(pairing_stage_name(context.state), 1, true);
  if (!create_key_transfer(context.req, context.key_init, context.device.node_id, this->node_id_, this->system_key_,
                           context.rx.data)) {
    this->busy_ = false;
    return false;
  }

  context.state = pairing::PairingState::WAIT_KEY_CONFIRM;
  this->record_exchange_debug_(pairing_stage_name(context.state), 1, true);
  // Reuse the normal authenticated exchange so pairing inherits the same retry logic.
  if (!this->send_and_receive_(context.req, context.resp, FREQ_CH2) || !frame_is_key_confirm(context.resp)) {
    ESP_LOGW(TAG, "Key exchange failed");
    this->busy_ = false;
    return false;
  }

  context.state = pairing::PairingState::PERSIST_DEVICE;
  this->record_exchange_debug_(pairing_stage_name(context.state), 1, true);
  this->devices_[context.device_id] = context.device;
  this->save_devices_();
  ESP_LOGI(TAG, "Device %s paired successfully!", context.device_id.c_str());

  if (create_set_config1(context.req, this->node_id_, context.device.node_id))
    this->send_and_receive_(context.req, context.resp, FREQ_CH2);

  context.state = pairing::PairingState::COMPLETE;
  this->record_exchange_debug_(pairing_stage_name(context.state), 1, true);
  this->busy_ = false;
  return true;
}

}  // namespace home_io_control
}  // namespace esphome
#include "hub_internal.h"

#include "proto_commands.h"

/// @file hub_operations.cpp
/// @brief High-level command execution and queued operation dispatch.
///
/// This file owns the outbound user-facing operations on the hub:
/// - cover position and tilt commands,
/// - explicit status requests,
/// - light/switch semantic wrappers,
/// - queued dispatch on the main loop.
///
/// Keeping these methods out of hub_core.cpp makes it easier to reason about the
/// difference between lifecycle/polling logic and the explicit actions initiated
/// by Home Assistant entities.

namespace esphome {
namespace home_io_control {

// Execute an authenticated request on the standard command channel and, on success, feed the
// device's reply back through the normal inbound status parser so all state normalization stays
// in one place.
bool IOHomeControlComponent::execute_request_and_update_(const std::string &device_id, const IoFrame &request,
                                                         bool warn_on_no_response, uint32_t retry_after_fail_ms) {
  IoFrame response;
  if (!this->send_and_receive_(request, response, FREQ_CH2)) {
    if (retry_after_fail_ms != 0) {
      if (auto *dev = this->get_device(device_id); dev != nullptr)
        dev->next_update = millis() + retry_after_fail_ms;
    }
    this->log_exchange_debug_(device_id.c_str());
    if (warn_on_no_response) {
      ESP_LOGW(detail::TAG, "No response from device %s", device_id.c_str());
    }
    return false;
  }

  this->update_device_status_(response);
  return true;
}

bool IOHomeControlComponent::set_device_position(const std::string &device_id, uint8_t position) {
  auto *dev = this->get_device(device_id);
  if (dev == nullptr || !this->initialized_)
    return false;

  const char *action = "set position";
  if (position == 0) {
    action = "open";
  } else if (position == 100) {
    action = "close";
  } else if (position == POS_STOP) {
    action = "stop";
  }

  // Once a device family is known, use the profile helpers to reject YAML/entity mismatches
  // before they hit the radio path. Unknown types still pass through so discovery and imported
  // devices keep working as before.
  if (!detail::known_device_accepts_execute_position(*dev, position)) {
    detail::log_rejected_operation(
        device_id, *dev, action,
        detail::is_binary_entity_position(position) ? "cover_position or binary_on_off" : "cover_position");
    return false;
  }

  ESP_LOGI(detail::TAG, "Sending %s to device %s (profile=%s)", action, device_id.c_str(),
           device_operation_profile_name(dev->type));

  IoFrame request;
  if (!create_execute(request, this->node_id_, dev->node_id, true, position))
    return false;
  return this->execute_request_and_update_(device_id, request, true, 0);
}

bool IOHomeControlComponent::set_device_tilt(const std::string &device_id, uint8_t tilt_percent) {
  auto *dev = this->get_device(device_id);
  if (dev == nullptr || !this->initialized_)
    return false;

  if (!detail::known_device_accepts_execute_tilt(*dev)) {
    detail::log_rejected_operation(device_id, *dev, "set tilt", "tilt-capable cover");
    return false;
  }

  ESP_LOGI(detail::TAG, "Sending tilt=%u%% to device %s (profile=%s)", tilt_percent, device_id.c_str(),
           device_operation_profile_name(dev->type));

  IoFrame request;
  if (!create_execute_tilt(request, this->node_id_, dev->node_id, true, tilt_percent))
    return false;
  return this->execute_request_and_update_(device_id, request, true, 0);
}

bool IOHomeControlComponent::request_device_status(const std::string &device_id) {
  auto *dev = this->get_device(device_id);
  if (dev == nullptr || !this->initialized_)
    return false;

  if (!detail::known_device_supports_status_requests(*dev)) {
    detail::log_rejected_operation(device_id, *dev, "status request", "status-capable actuator");
    return false;
  }

  IoFrame request;
  // Tilt-capable covers need the extended 0x03200100 status request so the response includes
  // the reliable 16-byte tilt block. Other devices stay on the shorter generic request.
  bool const request_ok = device_supports_tilt(dev->type)
                              ? create_get_status_tilt(request, this->node_id_, dev->node_id)
                              : create_get_status(request, this->node_id_, dev->node_id);
  if (!request_ok)
    return false;
  return this->execute_request_and_update_(device_id, request, false, detail::STATUS_RETRY_AFTER_FAIL_MS);
}

bool IOHomeControlComponent::set_light_state(const std::string &device_id, bool on) {
  auto *dev = this->get_device(device_id);
  if (dev == nullptr || !this->initialized_)
    return false;

  if (!detail::known_device_matches_entity_class(*dev, DeviceCapabilityClass::LIGHT)) {
    detail::log_rejected_operation(device_id, *dev, "light command", "light entity");
    return false;
  }

  // Light entities are binary-only for now, so they intentionally reuse the controller's
  // existing execute path with the proven on/off position encoding.
  return this->set_device_position(device_id,
                                   on ? detail::BINARY_ENTITY_ON_POSITION : detail::BINARY_ENTITY_OFF_POSITION);
}

bool IOHomeControlComponent::set_switch_state(const std::string &device_id, bool on) {
  auto *dev = this->get_device(device_id);
  if (dev == nullptr || !this->initialized_)
    return false;

  if (!detail::known_device_matches_entity_class(*dev, DeviceCapabilityClass::SWITCH)) {
    detail::log_rejected_operation(device_id, *dev, "switch command", "switch entity");
    return false;
  }

  // Switches share the same transport-level representation as binary lights.
  return this->set_device_position(device_id,
                                   on ? detail::BINARY_ENTITY_ON_POSITION : detail::BINARY_ENTITY_OFF_POSITION);
}

void IOHomeControlComponent::queue_set_device_position(const std::string &device_id, uint8_t position) {
  IoDevice *dev = this->get_device(device_id);
  if (dev != nullptr && !detail::known_device_matches_entity_class(*dev, DeviceCapabilityClass::COVER)) {
    detail::log_rejected_operation(device_id, *dev, "queued cover command", "cover entity");
    return;
  }
  this->pending_operations_.push_back({PendingOperationType::SET_POSITION, device_id, position});
}

void IOHomeControlComponent::queue_set_device_tilt(const std::string &device_id, uint8_t tilt_percent) {
  IoDevice *dev = this->get_device(device_id);
  if (dev != nullptr && !detail::known_device_accepts_execute_tilt(*dev)) {
    detail::log_rejected_operation(device_id, *dev, "queued tilt command", "tilt-capable cover");
    return;
  }
  this->pending_operations_.push_back({PendingOperationType::SET_TILT, device_id, tilt_percent});
}

void IOHomeControlComponent::queue_request_device_status(const std::string &device_id) {
  IoDevice *dev = this->get_device(device_id);
  if (dev != nullptr && !detail::known_device_supports_status_requests(*dev)) {
    detail::log_rejected_operation(device_id, *dev, "queued status request", "status-capable actuator");
    return;
  }
  this->pending_operations_.push_back({PendingOperationType::REQUEST_STATUS, device_id, 0});
}

void IOHomeControlComponent::queue_discover_and_pair() {
  // Pairing is globally exclusive work. Keep at most one queued request so repeated button presses
  // while the radio is busy do not stack duplicate discovery attempts.
  for (const auto &operation : this->pending_operations_) {
    if (operation.type == PendingOperationType::DISCOVER_AND_PAIR)
      return;
  }
  this->pending_operations_.push_back({PendingOperationType::DISCOVER_AND_PAIR, {}, 0});
}

void IOHomeControlComponent::queue_set_light_state(const std::string &device_id, bool on) {
  IoDevice *dev = this->get_device(device_id);
  if (dev != nullptr && !detail::known_device_matches_entity_class(*dev, DeviceCapabilityClass::LIGHT)) {
    detail::log_rejected_operation(device_id, *dev, "queued light command", "light entity");
    return;
  }

  // Queue through the same scheduler as covers so radio work stays serialized while still keeping
  // the light-vs-switch semantics available for capability checks at dispatch time.
  this->pending_operations_.push_back({PendingOperationType::SET_LIGHT_STATE, device_id,
                                       on ? detail::BINARY_ENTITY_ON_POSITION : detail::BINARY_ENTITY_OFF_POSITION});
}

void IOHomeControlComponent::queue_set_switch_state(const std::string &device_id, bool on) {
  IoDevice *dev = this->get_device(device_id);
  if (dev != nullptr && !detail::known_device_matches_entity_class(*dev, DeviceCapabilityClass::SWITCH)) {
    detail::log_rejected_operation(device_id, *dev, "queued switch command", "switch entity");
    return;
  }

  // Queue through the same scheduler as covers so radio work stays serialized while still keeping
  // the light-vs-switch semantics available for capability checks at dispatch time.
  this->pending_operations_.push_back({PendingOperationType::SET_SWITCH_STATE, device_id,
                                       on ? detail::BINARY_ENTITY_ON_POSITION : detail::BINARY_ENTITY_OFF_POSITION});
}

void IOHomeControlComponent::process_pending_operation_() {
  if (this->busy_ || this->pending_operations_.empty())
    return;

  // Pop before dispatch so any handler that re-queues follow-up work sees the queue in its
  // post-consumption state and cannot accidentally execute the same operation twice.
  PendingOperation const operation = std::move(this->pending_operations_.front());
  this->pending_operations_.pop_front();

  switch (operation.type) {
    case PendingOperationType::SET_POSITION:
      this->set_device_position(operation.device_id, operation.position);
      break;
    case PendingOperationType::SET_TILT:
      this->set_device_tilt(operation.device_id, operation.position);
      break;
    case PendingOperationType::SET_LIGHT_STATE:
      this->set_light_state(operation.device_id, operation.position == detail::BINARY_ENTITY_ON_POSITION);
      break;
    case PendingOperationType::SET_SWITCH_STATE:
      this->set_switch_state(operation.device_id, operation.position == detail::BINARY_ENTITY_ON_POSITION);
      break;
    case PendingOperationType::REQUEST_STATUS:
      this->request_device_status(operation.device_id);
      break;
    case PendingOperationType::DISCOVER_AND_PAIR:
      this->discover_and_pair();
      break;
  }
}

}  // namespace home_io_control
}  // namespace esphome
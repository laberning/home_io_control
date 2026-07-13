#include "hub_internal.h"

#include "proto_commands.h"

#include <algorithm>

/// @file hub_operations.cpp
/// @brief High-level command execution and queued operation dispatch.
/// @ingroup hioc_hub
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

namespace {

/// @brief Return the human-readable verb for a position-style command.
/// @param dev Device receiving the command.
/// @param position Requested execute position.
/// @return Log-friendly action string such as "open", "turn on", or "lock".
const char *position_command_action(const IoDevice &dev, uint8_t position) {
  if (position == POS_STOP)
    return "stop";

  if (!detail::is_binary_entity_position(position))
    return "set position";

  bool const active_state = position == BINARY_ENTITY_ON_POSITION;
  switch (device_capability_class(dev.type)) {
    case DeviceCapabilityClass::LIGHT:
    case DeviceCapabilityClass::SWITCH:
      return active_state ? "turn on" : "turn off";
    case DeviceCapabilityClass::LOCK:
      return active_state ? "unlock" : "lock";
    case DeviceCapabilityClass::UNKNOWN:
    case DeviceCapabilityClass::COVER:
    case DeviceCapabilityClass::CLIMATE:
    case DeviceCapabilityClass::SENSOR:
    case DeviceCapabilityClass::BEACON:
    default:
      return active_state ? "open" : "close";
  }
}

/// @brief Return the accepted entity/profile label for rejected execute-position logs.
/// @param position Requested execute position.
/// @return Expected profile label for detail::log_rejected_operation().
const char *position_rejection_profile(uint8_t position) {
  return detail::is_binary_entity_position(position) ? "cover_position or binary_on_off" : "cover_position";
}

}  // namespace

// Execute an authenticated request on the standard command channel and, on success, feed the
// device's reply back through the normal inbound status parser so all state normalization stays
// in one place.
bool IOHomeControlComponent::execute_request_and_update_(const std::string &device_id, const IoFrame &request,
                                                         bool warn_on_no_response, uint32_t retry_after_fail_ms) {
  IoFrame response;
  if (!this->send_and_receive_(request, response, FREQ_CH2)) {
    const auto &dbg = this->exchange_engine_.get_debug();
    if (retry_after_fail_ms != 0)
      this->schedule_background_poll_backoff_(device_id, dbg.saw_challenge);
    this->log_exchange_debug_(device_id.c_str());
    if (warn_on_no_response) {
      ESP_LOGW(detail::TAG, "Command 0x%02X failed for device %s: no valid response (stage=%s tries=%u)", request.cmd,
               device_id.c_str(), dbg.stage, dbg.tries);
    }
    return false;
  }

  if (response.cmd == CMD_ERROR_RESP) {
    if (response.data_len == 0) {
      detail::log_frame_issue(this, "rx", "unsupported_payload", response, frame_length(response));
    } else if (IoDevice *dev = this->registry_.get(device_id); dev != nullptr) {
      detail::record_command_result(*dev, device_id, response.data[0], request.cmd, true);
      this->notify_device_update_(device_id);
    } else {
      detail::log_command_result(device_id, response.data[0], request.cmd, true);
    }
    if (retry_after_fail_ms != 0)
      this->schedule_background_poll_backoff_(device_id, this->exchange_engine_.get_debug().saw_challenge);
    return false;
  }

  if (retry_after_fail_ms != 0)
    this->poll_policy_.clear_failure_streaks(device_id);

  // The immediate reply to our own CMD_EXECUTE (position/tilt/stop/favorite/vent) is not
  // trustworthy for target/current position on at least some devices — see
  // update_device_status_()'s trust_position doc comment. Every other request we send
  // (status poll, get name, ...) keeps trusting its reply as before.
  this->update_device_status_(response, request.cmd != CMD_EXECUTE);
  return true;
}

bool IOHomeControlComponent::set_device_position(const std::string &device_id, uint8_t position) {
  auto *dev = this->get_device(device_id);
  if (dev == nullptr || !this->initialized_)
    return false;

  const char *action = position_command_action(*dev, position);

  // Once a device family is known, use the profile helpers to reject YAML/entity mismatches
  // before they hit the radio path. Unknown types still pass through so discovery and imported
  // devices keep working as before.
  if (!detail::known_device_accepts_execute_position(*dev, position)) {
    detail::log_rejected_operation(device_id, *dev, action, position_rejection_profile(position));
    return false;
  }

  this->begin_status_poll_tracking_(device_id, this->poll_policy_.get_interval(device_id));

  ESP_LOGI(detail::TAG, "Sending %s to device %s (profile=%s)", action, device_id.c_str(),
           device_operation_profile_name(dev->type));

  IoFrame request;
  if (!create_execute(request, this->node_id_, dev->node_id, true, position)) {
    this->poll_policy_.clear(device_id);
    return false;
  }
  bool const ok = this->execute_request_and_update_(device_id, request, true, 0);
  if (!ok) {
    this->schedule_background_poll_backoff_(device_id, this->exchange_engine_.get_debug().saw_challenge);
    return false;
  }
  if (position != POS_STOP && this->poll_policy_.get_interval(device_id) != 0 &&
      this->poll_policy_.get_next_update(device_id) == 0)
    this->begin_status_poll_tracking_(device_id, this->poll_policy_.get_interval(device_id));
  return true;
}

bool IOHomeControlComponent::execute_device_command_(const std::string &device_id, CoverCommand cmd) {
  auto *dev = this->get_device(device_id);
  if (dev == nullptr || !this->initialized_)
    return false;

  if (!detail::known_device_matches_entity_class(*dev, DeviceCapabilityClass::COVER)) {
    detail::log_rejected_operation(device_id, *dev, cover_command_name(cmd), "cover entity");
    return false;
  }

  this->begin_status_poll_tracking_(device_id, this->poll_policy_.get_interval(device_id));

  ESP_LOGI(detail::TAG, "Sending %s to device %s (profile=%s)", cover_command_name(cmd), device_id.c_str(),
           device_operation_profile_name(dev->type));

  IoFrame request;
  if (!create_execute_command(request, this->node_id_, dev->node_id, true, cmd)) {
    this->poll_policy_.clear(device_id);
    return false;
  }
  bool const ok = this->execute_request_and_update_(device_id, request, true, 0);
  if (!ok) {
    this->schedule_background_poll_backoff_(device_id, this->exchange_engine_.get_debug().saw_challenge);
    return false;
  }
  if (cmd != CoverCommand::STOP && this->poll_policy_.get_interval(device_id) != 0 &&
      this->poll_policy_.get_next_update(device_id) == 0)
    this->begin_status_poll_tracking_(device_id, this->poll_policy_.get_interval(device_id));
  // STOP: if the device is still moving (decelerating or settling to a rest position), cap the
  // settle poll to STOP_SETTLE_POLL_CAP_MS. The private response is the shared reply to both polls
  // and commands, so it cannot mark itself as a STOP; this is the one place that knows a STOP was
  // sent and shortens the settle the response handler scheduled (which already folded in any hint).
  if (cmd == CoverCommand::STOP && dev != nullptr && !dev->is_stopped) {
    uint32_t const cap = millis() + STOP_SETTLE_POLL_CAP_MS;
    uint32_t const existing = this->poll_policy_.get_next_update(device_id);
    if (existing == 0 || cap < existing)
      this->poll_policy_.set_next_update(device_id, cap);
  }
  return true;
}

bool IOHomeControlComponent::set_device_tilt(const std::string &device_id, uint8_t tilt_percent) {
  auto *dev = this->get_device(device_id);
  if (dev == nullptr || !this->initialized_)
    return false;

  if (!detail::known_device_accepts_execute_tilt(*dev)) {
    detail::log_rejected_operation(device_id, *dev, "set tilt", "tilt-capable cover");
    return false;
  }

  this->begin_status_poll_tracking_(device_id, this->poll_policy_.get_interval(device_id));

  ESP_LOGI(detail::TAG, "Sending tilt=%u%% to device %s (profile=%s)", tilt_percent, device_id.c_str(),
           device_operation_profile_name(dev->type));

  IoFrame request;
  if (!create_execute_tilt(request, this->node_id_, dev->node_id, true, tilt_percent)) {
    this->poll_policy_.clear(device_id);
    return false;
  }
  bool const ok = this->execute_request_and_update_(device_id, request, true, 0);
  if (!ok) {
    this->schedule_background_poll_backoff_(device_id, this->exchange_engine_.get_debug().saw_challenge);
    return false;
  }
  if (this->poll_policy_.get_interval(device_id) != 0 && this->poll_policy_.get_next_update(device_id) == 0)
    this->begin_status_poll_tracking_(device_id, this->poll_policy_.get_interval(device_id));
  return true;
}

bool IOHomeControlComponent::set_device_position_and_tilt(const std::string &device_id, uint8_t position,
                                                          uint8_t tilt_percent) {
  auto *dev = this->get_device(device_id);
  if (dev == nullptr || !this->initialized_)
    return false;

  if (!detail::known_device_accepts_execute_tilt(*dev)) {
    detail::log_rejected_operation(device_id, *dev, "set position+tilt", "tilt-capable cover");
    return false;
  }

  this->begin_status_poll_tracking_(device_id, this->poll_policy_.get_interval(device_id));

  ESP_LOGI(detail::TAG, "Sending position=%u%% tilt=%u%% to device %s (profile=%s)", position, tilt_percent,
           device_id.c_str(), device_operation_profile_name(dev->type));

  IoFrame request;
  if (!create_execute_position_and_tilt(request, this->node_id_, dev->node_id, true, position, tilt_percent)) {
    this->poll_policy_.clear(device_id);
    return false;
  }
  bool const ok = this->execute_request_and_update_(device_id, request, true, 0);
  if (!ok) {
    this->schedule_background_poll_backoff_(device_id, this->exchange_engine_.get_debug().saw_challenge);
    return false;
  }
  if (this->poll_policy_.get_interval(device_id) != 0 && this->poll_policy_.get_next_update(device_id) == 0)
    this->begin_status_poll_tracking_(device_id, this->poll_policy_.get_interval(device_id));
  return true;
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
  uint32_t const retry_after_fail_ms =
      this->poll_policy_.is_tracking_active(device_id, millis()) ? STATUS_RETRY_AFTER_FAIL_MS : 0;
  return this->execute_request_and_update_(device_id, request, false, retry_after_fail_ms);
}

bool IOHomeControlComponent::request_device_name(const std::string &device_id) {
  auto *dev = this->get_device(device_id);
  if (dev == nullptr || !this->initialized_)
    return false;

  IoFrame request;
  if (!create_get_name(request, this->node_id_, dev->node_id, true))
    return false;
  return this->execute_request_and_update_(device_id, request, false, 0);
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
  return this->set_device_position(device_id, on ? BINARY_ENTITY_ON_POSITION : BINARY_ENTITY_OFF_POSITION);
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
  return this->set_device_position(device_id, on ? BINARY_ENTITY_ON_POSITION : BINARY_ENTITY_OFF_POSITION);
}

bool IOHomeControlComponent::set_lock_state(const std::string &device_id, bool locked) {
  auto *dev = this->get_device(device_id);
  if (dev == nullptr || !this->initialized_)
    return false;

  if (!detail::known_device_matches_entity_class(*dev, DeviceCapabilityClass::LOCK)) {
    detail::log_rejected_operation(device_id, *dev, "lock command", "lock entity");
    return false;
  }

  // Lock entities currently reuse the protocol's proven binary execute encoding:
  // unlock maps to 0 and lock maps to 100.
  return this->set_device_position(device_id, locked ? BINARY_ENTITY_OFF_POSITION : BINARY_ENTITY_ON_POSITION);
}

void IOHomeControlComponent::queue_set_device_position(const std::string &device_id, uint8_t position) {
  const IoDevice *dev = this->get_device(device_id);
  if (dev != nullptr && !detail::known_device_matches_entity_class(*dev, DeviceCapabilityClass::COVER)) {
    detail::log_rejected_operation(device_id, *dev, "queued cover command", "cover entity");
    return;
  }

  // Pre-scan for a pending SET_TILT so we can log its value if coalescing happens.
  uint8_t pending_tilt = 0;
  for (const auto &op : this->op_queue_) {
    if (op.type == PendingOperationType::SET_TILT && op.device_id == device_id) {
      pending_tilt = op.position;  // SET_TILT stores tilt in op.position
      break;
    }
  }
  if (this->op_queue_.enqueue_set_position(device_id, position)) {
    ESP_LOGI(detail::TAG,
             "Coalesced SET_POSITION (pos=%u) + pending SET_TILT (tilt=%u) → SET_POSITION_AND_TILT for "
             "device %s",
             position, pending_tilt, device_id.c_str());
  }
}

void IOHomeControlComponent::queue_device_command(const std::string &device_id, CoverCommand cmd) {
  const IoDevice *dev = this->get_device(device_id);
  if (dev != nullptr && !detail::known_device_matches_entity_class(*dev, DeviceCapabilityClass::COVER)) {
    detail::log_rejected_operation(device_id, *dev, cover_command_name(cmd), "cover entity");
    return;
  }
  this->op_queue_.enqueue_device_command(device_id, cmd);
}

void IOHomeControlComponent::queue_set_device_tilt(const std::string &device_id, uint8_t tilt_percent) {
  const IoDevice *dev = this->get_device(device_id);
  if (dev != nullptr && !detail::known_device_accepts_execute_tilt(*dev)) {
    detail::log_rejected_operation(device_id, *dev, "queued tilt command", "tilt-capable cover");
    return;
  }

  // Pre-scan for a pending SET_POSITION so we can log its value if coalescing happens.
  uint8_t pending_pos = 0;
  for (const auto &op : this->op_queue_) {
    if (op.type == PendingOperationType::SET_POSITION && op.device_id == device_id) {
      pending_pos = op.position;
      break;
    }
  }
  if (this->op_queue_.enqueue_set_tilt(device_id, tilt_percent)) {
    ESP_LOGI(detail::TAG,
             "Coalesced pending SET_POSITION (pos=%u) + SET_TILT (tilt=%u) → "
             "SET_POSITION_AND_TILT for device %s",
             pending_pos, tilt_percent, device_id.c_str());
  }
}

void IOHomeControlComponent::queue_set_device_position_and_tilt(const std::string &device_id, uint8_t position,
                                                                uint8_t tilt_percent) {
  const IoDevice *dev = this->get_device(device_id);
  if (dev != nullptr && !detail::known_device_accepts_execute_tilt(*dev)) {
    detail::log_rejected_operation(device_id, *dev, "queued position+tilt command", "tilt-capable cover");
    return;
  }
  this->op_queue_.enqueue_set_position_and_tilt(device_id, position, tilt_percent);
}

void IOHomeControlComponent::queue_request_device_status(const std::string &device_id) {
  const IoDevice *dev = this->get_device(device_id);
  if (dev != nullptr && !detail::known_device_supports_status_requests(*dev)) {
    detail::log_rejected_operation(device_id, *dev, "queued status request", "status-capable actuator");
    return;
  }
  // Keep at most one pending status poll per device. Without this, an overdue next_update can add
  // the same poll on every main-loop iteration until the first queued request is finally processed.
  this->op_queue_.enqueue_request_status(device_id);
}

void IOHomeControlComponent::queue_request_device_name(const std::string &device_id) {
  if (this->get_device(device_id) == nullptr)
    return;
  this->op_queue_.enqueue_request_name(device_id);
}

/// Queue a discovery-and-pair request with elevated priority.
///
/// Flushes any pending status/name poll operations (which would consume time
/// during the device's limited pairing window) and pushes discovery to the
/// front of the queue. Duplicate requests are suppressed.
void IOHomeControlComponent::queue_discover_and_pair() { this->op_queue_.enqueue_discover_and_pair(); }

void IOHomeControlComponent::queue_set_light_state(const std::string &device_id, bool on) {
  const IoDevice *dev = this->get_device(device_id);
  if (dev != nullptr && !detail::known_device_matches_entity_class(*dev, DeviceCapabilityClass::LIGHT)) {
    detail::log_rejected_operation(device_id, *dev, "queued light command", "light entity");
    return;
  }
  this->op_queue_.enqueue_set_light_state(device_id, on);
}

void IOHomeControlComponent::queue_set_lock_state(const std::string &device_id, bool locked) {
  const IoDevice *dev = this->get_device(device_id);
  if (dev != nullptr && !detail::known_device_matches_entity_class(*dev, DeviceCapabilityClass::LOCK)) {
    detail::log_rejected_operation(device_id, *dev, "queued lock command", "lock entity");
    return;
  }
  this->op_queue_.enqueue_set_lock_state(device_id, locked);
}

void IOHomeControlComponent::queue_set_switch_state(const std::string &device_id, bool on) {
  const IoDevice *dev = this->get_device(device_id);
  if (dev != nullptr && !detail::known_device_matches_entity_class(*dev, DeviceCapabilityClass::SWITCH)) {
    detail::log_rejected_operation(device_id, *dev, "queued switch command", "switch entity");
    return;
  }
  this->op_queue_.enqueue_set_switch_state(device_id, on);
}

void IOHomeControlComponent::process_pending_operation_() {
  if (this->busy_ || this->op_queue_.empty())
    return;

  // Pop before dispatch so any handler that re-queues follow-up work sees the queue in its
  // post-consumption state and cannot accidentally execute the same operation twice.
  auto opt = this->op_queue_.pop();
  if (!opt.has_value())
    return;
  const PendingOperation &operation = *opt;

  switch (operation.type) {
    case PendingOperationType::SET_POSITION:
      this->set_device_position(operation.device_id, operation.position);
      break;
    case PendingOperationType::SET_TILT:
      this->set_device_tilt(operation.device_id, operation.position);
      break;
    case PendingOperationType::SET_POSITION_AND_TILT:
      this->set_device_position_and_tilt(operation.device_id, operation.position, operation.tilt);
      break;
    case PendingOperationType::DEVICE_COMMAND:
      this->execute_device_command_(operation.device_id, operation.command);
      break;
    case PendingOperationType::SET_LIGHT_STATE:
      this->set_light_state(operation.device_id, operation.position == BINARY_ENTITY_ON_POSITION);
      break;
    case PendingOperationType::SET_LOCK_STATE:
      this->set_lock_state(operation.device_id, operation.position == BINARY_ENTITY_OFF_POSITION);
      break;
    case PendingOperationType::SET_SWITCH_STATE:
      this->set_switch_state(operation.device_id, operation.position == BINARY_ENTITY_ON_POSITION);
      break;
    case PendingOperationType::REQUEST_STATUS:
      this->request_device_status(operation.device_id);
      break;
    case PendingOperationType::REQUEST_NAME:
      this->request_device_name(operation.device_id);
      break;
    case PendingOperationType::DISCOVER_AND_PAIR:
      this->discover_and_pair();
      break;
  }
}

}  // namespace home_io_control
}  // namespace esphome
#include "hub_internal.h"

#include "proto_commands.h"

#include <algorithm>
#include <cstdio>

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

/// Stack-buffer size for a pre-formatted execute action phrase such as
/// "position=100%% tilt=100%%".
constexpr size_t EXECUTE_ACTION_BUF_SIZE = 40;

/// Wire-scale "fully open" position for a FORCE_OPEN. A normal actuator reads fully open as 0;
/// an IoDevice::inverted actuator (a horizontal awning, say) reads it as 100.
constexpr uint8_t FORCE_OPEN_WIRE_POSITION = 0;
constexpr uint8_t FORCE_OPEN_WIRE_POSITION_INVERTED = 100;

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

/// @brief Return the effective profile label for a device's "Sending ... (profile=...)" logs.
/// @param dev Device receiving the command.
/// @return "dimmable_light" for a LIGHT-class device with IoDevice::dimmable set (a YAML choice
///         the wire protocol has no signal for, so device_operation_profile_name() alone can't
///         know it); device_operation_profile_name(dev.type) for every other device.
const char *operation_profile_name(const IoDevice &dev) {
  if (dev.dimmable && device_capability_class(dev.type) == DeviceCapabilityClass::LIGHT)
    return "dimmable_light";
  return device_operation_profile_name(dev.type);
}

/// @brief Return the accepted entity/profile label for rejected execute-position logs.
/// @param dev Device the command was rejected for.
/// @param position Requested execute position.
/// @return Expected profile label for detail::log_rejected_operation().
const char *position_rejection_profile(const IoDevice &dev, uint8_t position) {
  // A LIGHT-class device only reaches rejection for a genuinely out-of-range value (dimmable
  // lights already accept the full 0-100 span in known_device_accepts_execute_position()) — the
  // fix there isn't "needs cover_position", it's "needs a value in 0-100".
  if (device_capability_class(dev.type) == DeviceCapabilityClass::LIGHT)
    return "0-100";
  return detail::is_binary_entity_position(position) ? "cover_position or binary_on_off" : "cover_position";
}

/// @brief The queue-time capability guard for one family of queued operation.
///
/// Every `queue_*` method applies the same early-reject as its execute-time counterpart, for fast
/// user feedback, with a "queued ..." rejection noun. This table is the one place those pairings
/// are recorded, so a deliberate asymmetry is a visible row rather than an accident. The lone
/// `queue_*` method with a guard that is NOT a row here is `queue_device_command`: it returns
/// bool and uses the command name as its rejection noun, so it keeps its guard inline (see there).
/// NOTE the asymmetry `queue_set_device_position` carries a COVER guard that `set_device_position`
/// deliberately does *not* — light/switch/lock all funnel through `set_device_position`, so an
/// entity-class guard there would break them; at queue time each entity has its own method.
struct QueueGuard {
  bool (*accepts)(const IoDevice &dev);  ///< false → reject this operation for this device.
  const char *rejection_noun;            ///< e.g. "queued cover command".
  const char *expected;                  ///< e.g. "cover entity".
};

constexpr QueueGuard QUEUE_GUARD_COVER{
    [](const IoDevice &d) { return detail::known_device_matches_entity_class(d, DeviceCapabilityClass::COVER); },
    "queued cover command", "cover entity"};
constexpr QueueGuard QUEUE_GUARD_TILT{[](const IoDevice &d) { return detail::known_device_accepts_execute_tilt(d); },
                                      "queued tilt command", "tilt-capable cover"};
constexpr QueueGuard QUEUE_GUARD_POSITION_AND_TILT{
    [](const IoDevice &d) { return detail::known_device_accepts_execute_tilt(d); }, "queued position+tilt command",
    "tilt-capable cover"};
constexpr QueueGuard QUEUE_GUARD_LIGHT{
    [](const IoDevice &d) { return detail::known_device_matches_entity_class(d, DeviceCapabilityClass::LIGHT); },
    "queued light command", "light entity"};
constexpr QueueGuard QUEUE_GUARD_LOCK{
    [](const IoDevice &d) { return detail::known_device_matches_entity_class(d, DeviceCapabilityClass::LOCK); },
    "queued lock command", "lock entity"};
constexpr QueueGuard QUEUE_GUARD_SWITCH{
    [](const IoDevice &d) { return detail::known_device_matches_entity_class(d, DeviceCapabilityClass::SWITCH); },
    "queued switch command", "switch entity"};
constexpr QueueGuard QUEUE_GUARD_STATUS{
    [](const IoDevice &d) { return detail::known_device_supports_status_requests(d); }, "queued status request",
    "status-capable actuator"};

/// @brief Apply one QueueGuard. Returns true (and logs) when the operation must be rejected.
///
/// Matches the historical guard exactly: an unregistered/unknown device (dev == nullptr) is *not*
/// rejected here — it passes through so discovery and imported devices keep working.
bool queue_guard_rejects(IOHomeControlComponent *hub, const std::string &device_id, const QueueGuard &guard) {
  const IoDevice *dev = hub->get_device(device_id);
  if (dev != nullptr && !guard.accepts(*dev)) {
    detail::log_rejected_operation(device_id, *dev, guard.rejection_noun, guard.expected);
    return true;
  }
  return false;
}

}  // namespace

void IOHomeControlComponent::arm_execute_confirmation_poll_(const std::string &device_id, bool for_stop) {
  uint32_t const existing = this->poll_policy_.get_next_update(device_id);
  uint32_t const delay_ms = settle_delay_ms(this->poll_policy_.get_interval(device_id), 0, for_stop);
  this->begin_status_poll_tracking_(device_id, delay_ms);
  if (existing != 0 && existing < millis() + delay_ms)
    this->poll_policy_.set_next_update(device_id, existing);
}

// Execute an authenticated request on the standard command channel and, on success, feed the
// device's reply back through the normal inbound status parser so all state normalization stays
// in one place.
bool IOHomeControlComponent::execute_request_and_update_(const std::string &device_id, const IoFrame &request,
                                                         bool warn_on_no_response, uint32_t retry_after_fail_ms) {
  IoFrame response;
  const ExchangeOutcome outcome = this->send_and_receive_(request, response, FREQ_CH2);
  // An unconfirmed acceptance means the device authenticated the request but never closed the
  // exchange. Whether that counts as success depends entirely on what the request was *for*:
  //   - a command (CMD_EXECUTE) is done — the device has it and is acting on it, and its own
  //     asynchronous status update carries the result a few seconds later;
  //   - a status poll or a name read exists to obtain a payload. Getting none means the question
  //     went unanswered, so it stays a failure and keeps the aggressive auth-shaped poll backoff
  //     that exists for precisely this shape of miss.
  const bool unconfirmed_counts_as_success = request.cmd == CMD_EXECUTE;
  if (outcome == ExchangeOutcome::FAILED ||
      (outcome == ExchangeOutcome::SUCCESS_UNCONFIRMED && !unconfirmed_counts_as_success)) {
    const auto &dbg = this->exchange_engine_.get_debug();
    if (IoDevice *dev = this->registry_.get(device_id); dev != nullptr) {
      detail::record_exchange_timeout(*dev, dbg.tries);
      this->notify_device_update_(device_id);
    }
    if (retry_after_fail_ms != 0)
      this->schedule_background_poll_backoff_(device_id, dbg.saw_challenge);
    this->log_exchange_debug_(device_id.c_str());
    if (warn_on_no_response) {
      ESP_LOGW(detail::TAG, "Command 0x%02X failed for device %s: no valid response (stage=%s tries=%u)", request.cmd,
               device_id.c_str(), dbg.stage, dbg.tries);
    }
    return false;
  }

  if (outcome == ExchangeOutcome::SUCCESS_UNCONFIRMED) {
    // The device authenticated the request, so it has the command; it just does not close the
    // exchange with a reply (see ExchangeOutcome). There is no frame to parse, and inventing a
    // position from a request we only know was *accepted* would be worse than leaving the last
    // known state alone — the device's own asynchronous status update supplies the real one, and
    // that path authenticates now. Clear the failure streaks: this was not a failure.
    if (retry_after_fail_ms != 0)
      this->poll_policy_.clear_failure_streaks(device_id);
    if (IoDevice *dev = this->registry_.get(device_id); dev != nullptr) {
      detail::update_link_health(*dev, this->radio_);
      this->notify_device_update_(device_id);
    }
    return true;
  }

  if (response.cmd == CMD_ERROR_RESP)
    return this->handle_error_response_(device_id, request, response, retry_after_fail_ms);

  if (retry_after_fail_ms != 0)
    this->poll_policy_.clear_failure_streaks(device_id);

  // The immediate reply to our own CMD_EXECUTE (position/tilt/stop/favorite/vent) is not
  // trustworthy for target/current position on at least some devices — see
  // update_device_status_()'s trust_position doc comment. Every other request we send
  // (status poll, get name, ...) keeps trusting its reply as before.
  this->update_device_status_(response, request.cmd != CMD_EXECUTE);
  return true;
}

bool IOHomeControlComponent::handle_error_response_(const std::string &device_id, const IoFrame &request,
                                                    const IoFrame &response, uint32_t retry_after_fail_ms) {
  IoDevice *dev = this->registry_.get(device_id);
  // An explicit refusal is still a reply from the device: this path returns before
  // execute_request_and_update_()'s update_device_status_() call, so it must stamp link health
  // itself to keep update_link_health()'s "every frame from a registered device" contract.
  if (dev != nullptr)
    detail::update_link_health(*dev, this->radio_);
  if (response.data_len == 0) {
    detail::log_frame_issue(this, "rx", "unsupported_payload", response, frame_length(response));
  } else if (dev != nullptr) {
    detail::record_command_result(*dev, device_id, response.data[0], request.cmd, true);
  } else {
    detail::log_command_result(device_id, response.data[0], request.cmd, true);
  }
  if (dev != nullptr)
    this->notify_device_update_(device_id);
  if (retry_after_fail_ms != 0)
    this->schedule_background_poll_backoff_(device_id, this->exchange_engine_.get_debug().saw_challenge);
  return false;
}

bool IOHomeControlComponent::run_execute_operation_(const std::string &device_id, const ExecuteRequestSpec &spec,
                                                    const std::function<bool(const IoDevice &)> &accepts,
                                                    const char *rejection_profile,
                                                    const std::function<bool(IoFrame &, const IoDevice &)> &build) {
  // Every false return means this command will not happen: an unregistered or not-yet-initialized
  // device, a profile guard rejection, a builder failure, or an exchange that ended with no valid
  // response or an explicit CMD_ERROR_RESP. In all of them the prediction the entity applied at
  // control() time must be withdrawn, or the Home Assistant cover animates a movement that is not
  // occurring — indefinitely, since only a frame from the device can settle it.
  const bool accepted = this->try_execute_operation_(device_id, spec, accepts, rejection_profile, build);
  if (!accepted)
    this->registry_.rollback_optimistic(device_id);
  return accepted;
}

bool IOHomeControlComponent::try_execute_operation_(const std::string &device_id, const ExecuteRequestSpec &spec,
                                                    const std::function<bool(const IoDevice &)> &accepts,
                                                    const char *rejection_profile,
                                                    const std::function<bool(IoFrame &, const IoDevice &)> &build) {
  auto *dev = this->get_device(device_id);
  if (dev == nullptr || !this->initialized_)
    return false;

  // Once a device family is known, use the profile helpers to reject YAML/entity mismatches
  // before they hit the radio path. Unknown types still pass through so discovery and imported
  // devices keep working as before.
  if (!accepts(*dev)) {
    detail::log_rejected_operation(device_id, *dev, spec.action, rejection_profile);
    return false;
  }

  this->begin_status_poll_tracking_(device_id, this->poll_policy_.get_interval(device_id));

  ESP_LOGI(detail::TAG, "Sending %s to device %s (profile=%s)", spec.action, device_id.c_str(),
           operation_profile_name(*dev));

  IoFrame request;
  if (!build(request, *dev)) {
    this->poll_policy_.clear(device_id);
    return false;
  }
  if (!this->execute_request_and_update_(device_id, request, true, 0)) {
    this->schedule_background_poll_backoff_(device_id, this->exchange_engine_.get_debug().saw_challenge);
    return false;
  }
  this->arm_execute_confirmation_poll_(device_id, spec.settle_as_stop);
  return true;
}

bool IOHomeControlComponent::set_device_position(const std::string &device_id, uint8_t position) {
  const auto *dev = this->get_device(device_id);
  if (dev == nullptr)
    return false;
  // action and rejection profile depend on this device's class and the requested value, so
  // resolve them here where dev is in scope; run_execute_operation_() re-checks dev/initialized_.
  return this->run_execute_operation_(
      device_id, {position_command_action(*dev, position), position == POS_STOP},
      [position](const IoDevice &d) { return detail::known_device_accepts_execute_position(d, position); },
      position_rejection_profile(*dev, position),
      [this, position](IoFrame &request, const IoDevice &d) {
        return create_execute_position(request, this->node_id_, d.node_id, d.low_power, position, d.silent);
      });
}

bool IOHomeControlComponent::execute_device_command_(const std::string &device_id, CoverCommand cmd) {
  return this->run_execute_operation_(
      device_id, {cover_command_name(cmd), cmd == CoverCommand::STOP},
      [](const IoDevice &d) { return detail::known_device_matches_entity_class(d, DeviceCapabilityClass::COVER); },
      "cover entity",
      [this, cmd](IoFrame &request, const IoDevice &d) {
        // FORCE_OPEN needs the device's own wire-scale "fully open" position (0, or 100 for an
        // IoDevice::inverted device such as a horizontal awning) — create_execute_command() has no
        // device access to resolve that, so it is built separately here where d is in scope.
        return cmd == CoverCommand::FORCE_OPEN
                   ? create_force_open(request, this->node_id_, d.node_id, d.low_power,
                                       d.inverted ? FORCE_OPEN_WIRE_POSITION_INVERTED : FORCE_OPEN_WIRE_POSITION)
                   : create_execute_command(request, this->node_id_, d.node_id, d.low_power, cmd, d.silent);
      });
}

bool IOHomeControlComponent::set_device_tilt(const std::string &device_id, uint8_t tilt_percent) {
  char action[EXECUTE_ACTION_BUF_SIZE];
  snprintf(action, sizeof(action), "tilt=%u%%", tilt_percent);
  return this->run_execute_operation_(
      device_id, {action, false}, [](const IoDevice &d) { return detail::known_device_accepts_execute_tilt(d); },
      "tilt-capable cover",
      [this, tilt_percent](IoFrame &request, const IoDevice &d) {
        return create_execute_tilt(request, this->node_id_, d.node_id, d.low_power, tilt_percent);
      });
}

bool IOHomeControlComponent::set_device_position_and_tilt(const std::string &device_id, uint8_t position,
                                                          uint8_t tilt_percent) {
  char action[EXECUTE_ACTION_BUF_SIZE];
  snprintf(action, sizeof(action), "position=%u%% tilt=%u%%", position, tilt_percent);
  return this->run_execute_operation_(
      device_id, {action, false}, [](const IoDevice &d) { return detail::known_device_accepts_execute_tilt(d); },
      "tilt-capable cover",
      [this, position, tilt_percent](IoFrame &request, const IoDevice &d) {
        return create_execute_position_and_tilt(request, this->node_id_, d.node_id, d.low_power, position,
                                                tilt_percent);
      });
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
                              ? create_get_status_tilt(request, this->node_id_, dev->node_id, dev->low_power)
                              : create_get_status(request, this->node_id_, dev->node_id, dev->low_power);
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
  if (!create_get_name(request, this->node_id_, dev->node_id, dev->low_power))
    return false;
  return this->execute_request_and_update_(device_id, request, false, 0);
}

bool IOHomeControlComponent::set_light_position(const std::string &device_id, uint8_t position) {
  auto *dev = this->get_device(device_id);
  if (dev == nullptr || !this->initialized_)
    return false;

  if (!detail::known_device_matches_entity_class(*dev, DeviceCapabilityClass::LIGHT)) {
    detail::log_rejected_operation(device_id, *dev, "light command", "light entity");
    return false;
  }

  // Light entities reuse the controller's existing execute path — the same position encoding
  // covers use, confirmed on real dimmable hardware (see the somfy_izymo_dimmer_* captures).
  return this->set_device_position(device_id, position);
}

bool IOHomeControlComponent::set_light_state(const std::string &device_id, bool on) {
  return this->set_light_position(device_id, on ? BINARY_ENTITY_ON_POSITION : BINARY_ENTITY_OFF_POSITION);
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
  if (queue_guard_rejects(this, device_id, QUEUE_GUARD_COVER)) {
    // control() applies the optimistic prediction before calling this method, so a queue-time
    // guard rejection is a command that will not happen and must withdraw it, exactly as
    // run_execute_operation_() does for a dispatch-time failure. No-op when nothing was predicted.
    this->registry_.rollback_optimistic(device_id);
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

bool IOHomeControlComponent::queue_device_command(const std::string &device_id, CoverCommand cmd) {
  const IoDevice *dev = this->get_device(device_id);
  // Every false return below is a command that will not happen; control() (STOP →
  // apply_optimistic_stop) already predicted, so withdraw it — see queue_set_device_position().
  if (!this->initialized_ || dev == nullptr) {
    this->registry_.rollback_optimistic(device_id);
    return false;
  }
  // Same COVER guard as QUEUE_GUARD_COVER, kept inline here: this method returns bool and its
  // rejection noun is the specific command name rather than a fixed "queued cover command".
  if (!detail::known_device_matches_entity_class(*dev, DeviceCapabilityClass::COVER)) {
    detail::log_rejected_operation(device_id, *dev, cover_command_name(cmd), "cover entity");
    this->registry_.rollback_optimistic(device_id);
    return false;
  }
  this->op_queue_.enqueue_device_command(device_id, cmd);
  return true;
}

void IOHomeControlComponent::queue_set_device_tilt(const std::string &device_id, uint8_t tilt_percent) {
  if (queue_guard_rejects(this, device_id, QUEUE_GUARD_TILT)) {
    // Withdraw the entity's optimistic prediction — see queue_set_device_position().
    this->registry_.rollback_optimistic(device_id);
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
  if (queue_guard_rejects(this, device_id, QUEUE_GUARD_POSITION_AND_TILT)) {
    // Withdraw the entity's optimistic prediction — see queue_set_device_position().
    this->registry_.rollback_optimistic(device_id);
    return;
  }
  this->op_queue_.enqueue_set_position_and_tilt(device_id, position, tilt_percent);
}

void IOHomeControlComponent::queue_request_device_status(const std::string &device_id) {
  if (queue_guard_rejects(this, device_id, QUEUE_GUARD_STATUS))
    return;
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

void IOHomeControlComponent::queue_set_light_position(const std::string &device_id, uint8_t position) {
  if (queue_guard_rejects(this, device_id, QUEUE_GUARD_LIGHT))
    return;
  this->op_queue_.enqueue_set_light_position(device_id, position);
}

void IOHomeControlComponent::queue_set_light_state(const std::string &device_id, bool on) {
  this->queue_set_light_position(device_id, on ? BINARY_ENTITY_ON_POSITION : BINARY_ENTITY_OFF_POSITION);
}

void IOHomeControlComponent::queue_set_lock_state(const std::string &device_id, bool locked) {
  if (queue_guard_rejects(this, device_id, QUEUE_GUARD_LOCK))
    return;
  this->op_queue_.enqueue_set_lock_state(device_id, locked);
}

void IOHomeControlComponent::queue_set_switch_state(const std::string &device_id, bool on) {
  if (queue_guard_rejects(this, device_id, QUEUE_GUARD_SWITCH))
    return;
  this->op_queue_.enqueue_set_switch_state(device_id, on);
}

// === 1W transmit ===

void IOHomeControlComponent::execute_oneway_command_(const std::string &controller_id, CoverCommand cmd) {
  this->execute_oneway_([&] { this->oneway_transmitter_.send_command(controller_id, cmd); });
}

void IOHomeControlComponent::execute_oneway_position_(const std::string &controller_id, uint8_t position) {
  this->execute_oneway_([&] { this->oneway_transmitter_.send_position(controller_id, position); });
}

void IOHomeControlComponent::execute_oneway_enroll_(const std::string &controller_id) {
  this->execute_oneway_([&] { this->oneway_transmitter_.send_enrollment(controller_id); });
}

void IOHomeControlComponent::execute_oneway_unenroll_(const std::string &controller_id) {
  this->execute_oneway_([&] { this->oneway_transmitter_.send_unenrollment(controller_id); });
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
      // operation.position already carries the target IO position (0-100) regardless of whether
      // it was enqueued via queue_set_light_state() (binary extremes) or
      // queue_set_light_position() (dimmable) — see enqueue_set_light_state()'s thin-wrapper doc.
      this->set_light_position(operation.device_id, operation.position);
      break;
    case PendingOperationType::SET_LOCK_STATE:
      this->set_lock_state(operation.device_id, operation.position == BINARY_ENTITY_OFF_POSITION);
      break;
    case PendingOperationType::SET_SWITCH_STATE:
      this->set_switch_state(operation.device_id, operation.position == BINARY_ENTITY_ON_POSITION);
      break;
    case PendingOperationType::ONEWAY_COMMAND:
      // device_id carries the controller-identity handle for 1W ops — see PendingOperation.
      this->execute_oneway_command_(operation.device_id, operation.command);
      break;
    case PendingOperationType::ONEWAY_POSITION:
      this->execute_oneway_position_(operation.device_id, operation.position);
      break;
    case PendingOperationType::ONEWAY_ENROLL:
      this->execute_oneway_enroll_(operation.device_id);
      break;
    case PendingOperationType::ONEWAY_UNENROLL:
      this->execute_oneway_unenroll_(operation.device_id);
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
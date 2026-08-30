/// @file platform_lock.cpp
/// @brief Experimental lock entity for IO-Homecontrol lock devices.
/// @ingroup hioc_platforms

#include "platform_lock.h"

#include "hub_internal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace home_io_control {

static const char *const TAG = "home_io_control.lock";

void IOHomeLock::setup() {
  // Locks participate in the same shared registry and callback fan-out as the other entity types.
  this->register_device_binding_(this, /*inverted=*/false, [this](const std::string &id, const IoDevice &dev) {
    this->on_device_update_(id, dev);
  });

  this->traits.set_assumed_state(false);
  this->traits.set_requires_code(false);
  this->traits.set_supports_open(false);
  this->traits.set_supported_states(
      {lock::LOCK_STATE_LOCKED, lock::LOCK_STATE_UNLOCKED, lock::LOCK_STATE_LOCKING, lock::LOCK_STATE_UNLOCKING});
}

void IOHomeLock::control(const lock::LockCall &call) {
  const auto state = call.get_state();
  if (!state.has_value())
    return;

  switch (state.value()) {
    case lock::LOCK_STATE_LOCKED:
    case lock::LOCK_STATE_LOCKING:
      this->parent_->queue_set_lock_state(this->device_id_, true);
      break;
    case lock::LOCK_STATE_UNLOCKED:
    case lock::LOCK_STATE_UNLOCKING:
      this->parent_->queue_set_lock_state(this->device_id_, false);
      break;
    case lock::LOCK_STATE_NONE:
    case lock::LOCK_STATE_JAMMED:
    default:
      break;
  }
}

void IOHomeLock::on_device_update_(const std::string &id, const IoDevice &dev) {
  if (id != this->device_id_ || dev.position == UNKNOWN_POSITION)
    return;

  if (!effective_is_stopped(dev)) {
    const float eff_target = effective_target(dev);
    if (eff_target != UNKNOWN_POSITION) {
      this->publish_state(eff_target < detail::BINARY_ENTITY_ON_POSITION_THRESHOLD ? lock::LOCK_STATE_UNLOCKING
                                                                                   : lock::LOCK_STATE_LOCKING);
    }
    return;
  }

  this->publish_state(dev.position < detail::BINARY_ENTITY_ON_POSITION_THRESHOLD ? lock::LOCK_STATE_UNLOCKED
                                                                                 : lock::LOCK_STATE_LOCKED);
}

void IOHomeLock::dump_config() {
  LOG_LOCK("", "IO-Homecontrol Lock", this);
  ESP_LOGCONFIG(TAG, "  Device ID: %s", this->device_id_.c_str());
  this->log_poll_interval_config_(TAG);
  ESP_LOGCONFIG(TAG, "  Status: experimental and untested");
}

}  // namespace home_io_control
}  // namespace esphome
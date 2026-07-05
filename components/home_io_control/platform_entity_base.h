#pragma once

/// @file platform_entity_base.h
/// @brief Shared device-binding mixin for IO-Homecontrol entity platforms.
/// @ingroup hioc_platforms
///
/// IOHomeCover, IOHomeLight, IOHomeSwitch and IOHomeLock all bind an ESPHome entity to a hub
/// device: the same five YAML setters, the same registration ritual in setup(), and the same
/// poll-interval dump_config line. DeviceBoundEntity centralizes exactly that shared state and
/// wiring.
///
/// It is intentionally NOT an ESPHome base class — it is a plain mixin the entities inherit
/// alongside their real ESPHome base (cover::Cover, light::LightOutput, switch_::Switch,
/// lock::Lock). Entity-specific state mapping (cover position/tilt/movement inference, binary
/// on/off decoding, the lock state machine) deliberately stays in the entities; only the
/// device-binding plumbing lives here.

#include "hub_internal.h"

#include "esphome/core/application.h"

#include <functional>
#include <string>
#include <utility>

namespace esphome {
namespace home_io_control {

/// @brief Mixin holding the hub-device binding shared by all IO-Homecontrol entity platforms.
/// @ingroup hioc_platforms
class DeviceBoundEntity {
 public:
  /// @brief Set the parent controller component.
  /// @param parent Pointer to the IOHomeControlComponent instance.
  void set_parent(IOHomeControlComponent *parent) { this->parent_ = parent; }
  /// @brief Set the unique IO-homecontrol device ID (from YAML).
  /// @param id Hexadecimal node ID string (e.g., "123ABC").
  void set_device_id(const std::string &id) { this->device_id_ = id; }
  /// @brief Set the declared device type (from YAML).
  /// @param type Device type enum.
  void set_device_type(DeviceType type) { this->device_type_ = type; }
  /// @brief Set the declared device subtype (from YAML).
  /// @param subtype Subtype value.
  void set_subtype(uint8_t subtype) { this->subtype_ = subtype; }
  /// @brief Configure bounded follow-up polling while a state change is expected.
  /// @param poll_interval_ms Poll interval in milliseconds; zero keeps the default single settle poll only.
  void set_status_poll_interval(uint32_t poll_interval_ms) { this->status_poll_interval_ms_ = poll_interval_ms; }

 protected:
  /// @brief Perform the shared setup() registration ritual.
  ///
  /// Registers the device with the controller, sets its status-poll interval, subscribes the
  /// entity's update callback, and schedules the delayed initial status request — in the exact
  /// order every entity used before.
  /// @param self The entity itself; used for set_timeout(), since DeviceBoundEntity is not a Component.
  /// @param inverted Initial inversion flag passed to add_device (covers compute it; others pass false).
  /// @param on_update The entity's device-update callback.
  void register_device_binding_(Component *self, bool inverted,
                                std::function<void(const std::string &, const IoDevice &)> on_update) {
    this->parent_->add_device(this->device_id_, this->device_type_, this->subtype_, inverted);
    this->parent_->set_device_status_poll_interval(this->device_id_, this->status_poll_interval_ms_);
    this->parent_->register_device_callback(std::move(on_update));
    // Schedule the delayed initial poll through the public scheduler: Component::set_timeout is
    // protected, and DeviceBoundEntity is a mixin, not a Component subclass, so it cannot call it
    // through `self`. App.scheduler.set_timeout is the public entry point set_timeout wraps.
    App.scheduler.set_timeout(self, "init_status", INITIAL_STATUS_REQUEST_DELAY_MS,
                              [this]() { this->parent_->queue_request_device_status(this->device_id_); });
  }

  /// @brief Shared inbound-update guard for binary endpoints.
  ///
  /// Matches this device and accepts only a settled (stopped) status with a known position.
  /// Covers and locks intentionally use their own richer guards instead of this filter.
  [[nodiscard]] bool passes_binary_update_filter_(const std::string &id, const IoDevice &dev) const {
    return id == this->device_id_ && dev.position != UNKNOWN_POSITION && dev.is_stopped;
  }

  /// @brief Emit the shared two-branch poll-interval line for dump_config().
  /// @param tag Log tag of the calling entity.
  void log_poll_interval_config_(const char *tag) const {
    if (this->status_poll_interval_ms_ == 0) {
      ESP_LOGCONFIG(tag, "  Status Poll Interval: default single settle poll");
    } else {
      ESP_LOGCONFIG(tag, "  Status Poll Interval: %u ms", this->status_poll_interval_ms_);
    }
  }

  IOHomeControlComponent *parent_{nullptr};
  std::string device_id_;
  DeviceType device_type_{DeviceType::UNKNOWN};
  uint8_t subtype_{0};
  uint32_t status_poll_interval_ms_{0};
};

}  // namespace home_io_control
}  // namespace esphome

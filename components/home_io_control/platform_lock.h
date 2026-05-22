#pragma once

/// @file platform_lock.h
/// @brief Experimental lock entity for IO-Homecontrol lock-capable devices.
/// @ingroup hioc_platforms
///
/// Presents lock and unlock actions through ESPHome's native lock entity model while reusing
/// the hub's shared execute/status paths.
/// @todo Validate end-to-end behavior with physical IO-Homecontrol lock devices, including any
///       vendor-specific status encodings beyond the shared 0/100 binary mapping used here.
///
/// @warning This platform has not been tested with physical IO-Homecontrol lock devices.
///          Behavior is inferred from existing protocol evidence and may need hardware tuning.

#include "esphome/components/lock/lock.h"
#include "esphome/core/component.h"
#include "hub_core.h"

namespace esphome {
namespace home_io_control {

/// @brief Lock entity for IO-Homecontrol lock devices.
/// @ingroup hioc_platforms
class IOHomeLock : public lock::Lock, public Component {
 public:
  /// @brief Initialize the lock entity and register it with the shared hub.
  void setup() override;
  /// @brief Dump configuration to the log.
  void dump_config() override;
  /// @brief Get setup priority (DATA).
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }

  /// @brief Set parent controller.
  /// @param parent Pointer to IOHomeControlComponent.
  void set_parent(IOHomeControlComponent *parent) { this->parent_ = parent; }
  /// @brief Set device ID from YAML.
  /// @param id Hex string node ID.
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
  /// @brief Apply a Home Assistant lock control request.
  /// @param call Desired state change.
  void control(const lock::LockCall &call) override;
  /// @brief Handle inbound device status updates.
  /// @param id Device ID.
  /// @param dev Updated device state.
  void on_device_update_(const std::string &id, const IoDevice &dev);

  IOHomeControlComponent *parent_{nullptr};
  std::string device_id_;
  DeviceType device_type_{DeviceType::UNKNOWN};
  uint8_t subtype_{0};
  uint32_t status_poll_interval_ms_{0};
};

}  // namespace home_io_control
}  // namespace esphome
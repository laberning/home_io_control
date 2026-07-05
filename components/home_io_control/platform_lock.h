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
#include "platform_entity_base.h"

namespace esphome {
namespace home_io_control {

/// @brief Lock entity for IO-Homecontrol lock devices.
/// @ingroup hioc_platforms
///
/// The device-binding setters, setup() registration ritual and poll-interval dump line come
/// from DeviceBoundEntity; the lock state machine (locking/unlocking/locked/unlocked) lives here
/// and uses its own inbound-update guard because it acts on in-flight movement too.
class IOHomeLock : public lock::Lock, public Component, public DeviceBoundEntity {
 public:
  /// @brief Initialize the lock entity and register it with the shared hub.
  void setup() override;
  /// @brief Dump configuration to the log.
  void dump_config() override;
  /// @brief Get setup priority (DATA).
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  /// @brief Apply a Home Assistant lock control request.
  /// @param call Desired state change.
  void control(const lock::LockCall &call) override;
  /// @brief Handle inbound device status updates.
  /// @param id Device ID.
  /// @param dev Updated device state.
  void on_device_update_(const std::string &id, const IoDevice &dev);
};

}  // namespace home_io_control
}  // namespace esphome
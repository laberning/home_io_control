/// @file hub_management.cpp
/// @brief Thin hub wrapper delegating management actions to ManagementActions.
/// @ingroup hioc_hub
///
/// All management logic lives in management_actions.cpp. This file provides only the
/// IOHomeControlComponent::rename_device() / identify_device() / force_open_device() /
/// scan_paired_devices() implementations that the virtual dispatch table requires.

#include "hub_internal.h"

namespace esphome {
namespace home_io_control {

/// Rename a registered device and verify by reading the name back.
///
/// Thin wrapper that delegates to management_actions_.rename_device() so that
/// the virtual dispatch defined in hub_core.h is fulfilled.
IOHomeControlComponent::ManagementActionResult IOHomeControlComponent::rename_device(const std::string &device_id,
                                                                                     const std::string &new_name) {
  return this->management_actions_.rename_device(device_id, new_name);
}

/// Trigger a registered device's physical identify (jog/flash).
///
/// Thin wrapper that delegates to management_actions_.identify_device() so that
/// the virtual dispatch defined in hub_core.h is fulfilled.
IOHomeControlComponent::ManagementActionResult IOHomeControlComponent::identify_device(const std::string &device_id) {
  return this->management_actions_.identify_device(device_id);
}

/// Move a cover device to fully open at elevated priority, intended to bypass wind/rain
/// soft locks.
///
/// Thin wrapper that delegates to management_actions_.force_open_device() so that
/// the virtual dispatch defined in hub_core.h is fulfilled.
IOHomeControlComponent::ManagementActionResult IOHomeControlComponent::force_open_device(const std::string &device_id) {
  return this->management_actions_.force_open_device(device_id);
}

/// Broadcast a roll-call and report every device that answers.
///
/// Thin wrapper that delegates to management_actions_.scan_paired_devices() so that
/// the virtual dispatch defined in hub_core.h is fulfilled.
IOHomeControlComponent::ManagementActionResult IOHomeControlComponent::scan_paired_devices() {
  return this->management_actions_.scan_paired_devices();
}

}  // namespace home_io_control
}  // namespace esphome

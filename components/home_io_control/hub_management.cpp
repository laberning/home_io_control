/// @file hub_management.cpp
/// @brief Thin hub wrapper delegating management actions to ManagementActions.
/// @ingroup hioc_hub
///
/// All management logic lives in management_actions.cpp. This file provides
/// only the IOHomeControlComponent::rename_device() implementation that the
/// virtual dispatch table requires.

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

}  // namespace home_io_control
}  // namespace esphome

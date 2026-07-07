#pragma once

/// @file management_actions.h
/// @brief Hub-level management operations exposed as Home Assistant actions.
/// @ingroup hioc_hub
///
/// ManagementActions owns the rename-device flow and the native API registration
/// that wires it into Home Assistant. The API service descriptor holds a
/// ManagementActions* and calls only public methods on it, so no friend
/// declarations into the hub are needed.
///
/// IOHomeControlComponent exposes this collaborator through thin protected
/// wrappers (api_rename_device_, register_management_actions_).

#include "exchange_engine.h"
#include "device_registry.h"

#include <cstdint>
#include <string>

namespace esphome {
namespace home_io_control {

// Forward declaration — full definition included only in management_actions.cpp.
class IOHomeControlComponent;

/// Result of a hub-level management action such as rename.
///
/// Returned by ManagementActions::rename_device() and re-exported from the hub
/// as IOHomeControlComponent::ManagementActionResult via a public type alias.
struct ManagementActionResult {
  bool success{false};          ///< Whether the requested management action succeeded.
  bool verified{false};         ///< Whether a follow-up readback confirmed the applied state.
  bool has_result_code{false};  ///< True when result_code contains a decoded CMD_ERROR_RESP byte.
  uint8_t result_code{0};       ///< Optional CMD_ERROR_RESP result byte.
  std::string action;           ///< Action name, e.g. "rename_device".
  std::string device_id;        ///< Target IO-homecontrol device ID.
  std::string message;          ///< Human-readable outcome summary.
  std::string requested_name;   ///< Requested normalized UTF-8 name for rename actions.
  std::string applied_name;     ///< Verified cached UTF-8 name after a readback, when available.
};

/// Encapsulates hub-level management operations exposed as Home Assistant actions.
///
/// ManagementActions is constructed once by IOHomeControlComponent. It stores pointers
/// and references into hub members; it is non-copyable for the same reason as ExchangeEngine.
/// @ingroup hioc_hub
class ManagementActions {
 public:
  /// Construct with all required collaborators.
  ///
  /// @param node_id      Controller 3-byte node ID, owned by the hub.
  /// @param engine       Shared exchange engine for radio transactions.
  /// @param registry     Device registry for device lookups.
  /// @param initialized  Pointer to the hub's initialized flag.
  /// @param hub          Hub pointer for ESPHome API calls and public hub methods.
  ManagementActions(const uint8_t *node_id, ExchangeEngine &engine, DeviceRegistry &registry, const bool *initialized,
                    IOHomeControlComponent *hub);

  /// Non-copyable — stores references and pointers into hub member addresses.
  ManagementActions(const ManagementActions &) = delete;
  ManagementActions &operator=(const ManagementActions &) = delete;

  /// Register the rename_device action with ESPHome's native API server.
  void register_actions();

  /// Native API callback: rename a device and publish the result as a HA event.
  void api_rename_device(const std::string &device_id, const std::string &new_name);

  /// Rename a registered device and verify the result by reading the name back.
  /// @param device_id Target device ID (hex string, case-insensitive).
  /// @param new_name  Requested UTF-8 device name.
  /// @return Structured result describing success, verification, and any error.
  ManagementActionResult rename_device(const std::string &device_id, const std::string &new_name);

  /// Publish a management result as a structured log line and Home Assistant event.
  void publish_result(const ManagementActionResult &result);

 private:
  const uint8_t *node_id_;
  ExchangeEngine &engine_;
  DeviceRegistry &registry_;
  const bool *initialized_;
  IOHomeControlComponent *hub_;
};

}  // namespace home_io_control
}  // namespace esphome

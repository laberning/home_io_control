#pragma once

/// @file management_actions.h
/// @brief Hub-level management operations exposed as Home Assistant actions.
/// @ingroup hioc_hub
///
/// ManagementActions owns the family of advanced, non-entity hub actions (device rename,
/// identify, force-open, ...) and the native API registration that wires them into Home
/// Assistant. Every action is registered through one data-driven API service descriptor
/// (detail::ManagementServiceDescriptor); adding a new action does not require a new
/// descriptor class. The descriptor holds a ManagementActions* and calls only public
/// methods on it, so no friend declarations into the hub are needed.
///
/// IOHomeControlComponent exposes this collaborator through thin protected wrappers
/// (api_rename_device_, api_identify_device_, api_force_open_device_, register_management_actions_).

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

  /// Register all management actions (rename, identify, force-open, ...) with ESPHome's
  /// native API server.
  void register_actions();

  /// Native API callback: rename a device and publish the result as a HA event.
  void api_rename_device(const std::string &device_id, const std::string &new_name);

  /// Rename a registered device and verify the result by reading the name back.
  /// @param device_id Target device ID (hex string, case-insensitive).
  /// @param new_name  Requested UTF-8 device name.
  /// @return Structured result describing success, verification, and any error.
  ManagementActionResult rename_device(const std::string &device_id, const std::string &new_name);

  /// Native API callback: trigger a device's physical identify and publish the result as a HA event.
  void api_identify_device(const std::string &device_id);

  /// @brief Trigger a registered device's physical identify (brief jog/flash).
  ///
  /// Deliberately performs no device-type gating beyond "is it registered on this hub" — identify
  /// exists precisely to let a user work out what an unknown registry entry physically is, so
  /// restricting it to specific device types would defeat the purpose. A CMD_ERROR_RESP reply is
  /// treated as a successful trigger (not a failure): per the protocol, devices may answer an
  /// identify request that way and still perform the jog.
  /// @param device_id Target device ID (hex string, case-insensitive).
  /// @return Structured result describing success and any validation failure. `verified` is
  ///         always false — there is no readback that confirms a physical jog happened.
  ManagementActionResult identify_device(const std::string &device_id);

  /// Native API callback: force-open a device and publish the result as a HA event.
  void api_force_open_device(const std::string &device_id);

  /// @brief Move a registered cover device to fully open at elevated priority, intended to
  /// bypass wind/rain soft locks.
  ///
  /// Safety-sensitive: this deliberately requires an explicit `device_id` (a hub-level action,
  /// not a permanent per-device button) because it can override legitimate environmental
  /// protection. Queues the command onto the hub's normal cover-command dispatch path
  /// (IOHomeControlComponent::queue_device_command()), which already owns capability gating,
  /// poll tracking, and backoff — this method does not talk to the radio directly. The result
  /// therefore only confirms that the command was queued; the actual movement outcome arrives
  /// later via the device's normal cover-state/polling pipeline, so `verified` is always false.
  /// @note The elevated-priority mechanism (see create_force_open() in proto_commands.cpp) is
  ///       confirmed on real hardware to correctly move a device to fully open, but bypassing
  ///       an *active* environmental lock is still experimental — only that the device accepts
  ///       the frame and opens when nothing is locking it has been observed so far.
  /// @param device_id Target device ID (hex string, case-insensitive).
  /// @return Structured result describing whether the command was queued.
  ManagementActionResult force_open_device(const std::string &device_id);

  /// Publish a management result as a structured log line and Home Assistant event.
  void publish_result(const ManagementActionResult &result);

 private:
  /// @brief Shared preamble for every management action: validate the hub and device ID,
  /// then resolve the target device.
  ///
  /// Initializes `result` with the action name and normalized device ID, then checks (in
  /// order) that the hub is initialized, that `device_id` is a well-formed 6-hex-digit ID,
  /// and that it names a device registered on this hub. On any failure, sets `result.message`
  /// to a stable, caller-facing string and returns nullptr; callers should return `result`
  /// immediately in that case.
  /// @param action Action name to record on `result` (e.g. MANAGEMENT_ACTION_RENAME_DEVICE).
  /// @param device_id Requested device ID (hex string, case-insensitive, as received from HA).
  /// @param result Populated with the action name and normalized device ID, plus an error
  ///               message on failure.
  /// @return Pointer to the resolved device, or nullptr if any validation step failed.
  IoDevice *resolve_device_(const char *action, const std::string &device_id, ManagementActionResult &result);

  /// @brief Send an authenticated request and report a stock timeout failure on no response.
  ///
  /// Shared by every action that talks to the device directly (rename, identify): both send
  /// their built request via ExchangeEngine::send_and_receive() and, on failure, log the same
  /// exchange debug snapshot and report the same "no valid response to <action_verb> request"
  /// message shape. Actions that don't talk to the radio directly (force-open, which queues
  /// through the hub's cover-command dispatch path instead) have no use for this helper.
  /// @param request Frame to send.
  /// @param response Populated with the device's reply on success.
  /// @param action_verb Bare verb used only in the failure message, e.g. "rename" or "identify".
  /// @param result Result to update with `message` on failure; `result.device_id` must already be
  ///               set (via resolve_device_()) since it is used for the debug log.
  /// @return true if a response was received; false on timeout (result.message set, debug
  ///         snapshot logged).
  bool send_authenticated_request_(const IoFrame &request, IoFrame &response, const char *action_verb,
                                   ManagementActionResult &result);

  const uint8_t *node_id_;
  ExchangeEngine &engine_;
  DeviceRegistry &registry_;
  const bool *initialized_;
  IOHomeControlComponent *hub_;
};

}  // namespace home_io_control
}  // namespace esphome

#pragma once

/// @file device_registry.h
/// @brief Per-hub device table, update-callback fan-out, and linked-remote map.
/// @ingroup hioc_hub
///
/// DeviceRegistry owns the three containers that the hub previously kept as raw
/// private members.  Extracting them into this class lets the polling loop, status
/// engine, and pairing flow share a single, testable source of truth without
/// knowing about each other.

#include "proto_device_model.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace esphome {
namespace home_io_control {

/// Callback type invoked when a device's state changes.
using DeviceUpdateCallback = std::function<void(const std::string &device_id, const IoDevice &device)>;

/// Owns the per-hub device table, update callbacks, and linked-remote associations.
///
/// All add/get/subscribe/notify operations that IOHomeControlComponent previously
/// performed on its own private maps now go through this class.  The class has no
/// ESPHome dependencies beyond logging and is directly host-testable.
/// @ingroup hioc_hub
class DeviceRegistry {
 public:
  /// Register a device by ID with default metadata (UNKNOWN type, subtype 0, not inverted).
  /// No-op when @p device_id is already registered.  Warns and returns when the hex string is invalid.
  /// @param device_id Hexadecimal node ID string (e.g. "ABC123").
  void add(const std::string &device_id);

  /// Register a device with full YAML-derived metadata.
  /// No-op when @p device_id is already registered.  Warns and returns when the hex string is invalid.
  /// @param device_id Hexadecimal node ID string.
  /// @param type      Device type from YAML declaration.
  /// @param subtype   Device subtype byte.
  /// @param inverted  Position-inversion flag.
  void add(const std::string &device_id, DeviceType type, uint8_t subtype, bool inverted);

  /// Insert or overwrite a device entry without deduplication or hex-validation checks.
  /// Used by the pairing flow which already validated the device during discovery.
  /// @param device_id Hexadecimal node ID string.
  /// @param device    Fully-built device to store.
  void put(const std::string &device_id, IoDevice device);

  /// Retrieve a registered device by ID.
  /// @return Pointer to the stored IoDevice, or nullptr when not found.
  [[nodiscard]] IoDevice *get(const std::string &device_id);

  /// Register a callback that fires whenever a device's state changes.
  /// @param cb Callable with signature void(const std::string &device_id, const IoDevice &device).
  void subscribe(DeviceUpdateCallback cb);

  /// Invoke all registered callbacks for @p device_id.
  /// No-op when @p device_id is not in the registry.
  /// @param device_id Device whose state just changed.
  void notify(const std::string &device_id);

  /// Record that a remote node controls a registered device.
  /// When activity from the remote is overheard, a status poll is scheduled for the linked device.
  /// @param remote_id Node ID of the remote control.
  /// @param device_id Node ID of the device it controls.
  void add_linked_remote(const std::string &remote_id, const std::string &device_id);

  /// Retrieve the list of device IDs linked to a remote.
  /// @return Pointer to the device-ID list, or nullptr when the remote is unknown.
  [[nodiscard]] const std::vector<std::string> *linked_devices(const std::string &remote_id) const;

  /// @return Number of registered devices.
  [[nodiscard]] size_t size() const { return devices_.size(); }

  /// @return Number of distinct linked-remote entries.
  [[nodiscard]] size_t linked_remote_count() const { return linked_remotes_.size(); }

  /// Invoke @p fn(remote_id, device_id_list) for every linked-remote entry.
  /// @param fn Callable receiving the remote ID and its associated device ID list.
  void for_each_linked_remote(
      const std::function<void(const std::string &, const std::vector<std::string> &)> &fn) const;

  /// Mutable begin iterator over (device_id, IoDevice) pairs (supports range-for in the poll loop).
  std::map<std::string, IoDevice>::iterator begin() { return devices_.begin(); }
  /// Mutable end iterator over (device_id, IoDevice) pairs.
  std::map<std::string, IoDevice>::iterator end() { return devices_.end(); }

 private:
  std::map<std::string, IoDevice> devices_;
  std::vector<DeviceUpdateCallback> callbacks_;
  std::map<std::string, std::vector<std::string>> linked_remotes_;
};

}  // namespace home_io_control
}  // namespace esphome

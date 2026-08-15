#pragma once

/// @file device_registry.h
/// @brief Per-hub device table, update-callback fan-out, and linked-remote map.
/// @ingroup hioc_hub
///
/// DeviceRegistry owns the per-hub device table, the update-callback list, and the
/// linked-remote map. Keeping them in one class lets the polling loop, status
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

/// YAML-declared device metadata for registration; defaults match an undeclared device.
/// A new per-device attribute is a new field with a default here, not a new `add()` overload.
struct DeviceConfig {
  DeviceType type{DeviceType::UNKNOWN};  ///< Device type from YAML declaration.
  uint8_t subtype{0};                    ///< Device subtype byte.
  bool inverted{false};                  ///< Position-inversion flag.
  bool optimistic_state{true};           ///< Whether `apply_optimistic_target`/`clear_optimistic_target` may fire.
  bool silent{false};                    ///< Send position moves in "silent operation" (slower) mode.
};

/// Owns the per-hub device table, update callbacks, and linked-remote associations.
///
/// All of the hub's add/get/subscribe/notify operations go through this class —
/// IOHomeControlComponent holds no device state of its own.  The class has no
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
  /// @param cfg       Device type/subtype/inversion/optimistic-state metadata.
  void add(const std::string &device_id, const DeviceConfig &cfg);

  /// Insert or overwrite a device entry without deduplication or hex-validation checks.
  /// Used by the pairing flow which already validated the device during discovery.
  /// @param device_id Hexadecimal node ID string.
  /// @param device    Fully-built device to store.
  void put(const std::string &device_id, IoDevice device);

  /// Retrieve a registered device by ID.
  /// @return Pointer to the stored IoDevice, or nullptr when not found.
  [[nodiscard]] IoDevice *get(const std::string &device_id);

  /// Set a device's `dimmable` flag (see IoDevice::dimmable). No-op when the device is unknown.
  /// Called by platform_light.cpp's setup() right after registration, since `dimmable` is a
  /// light-platform YAML choice, not something add()'s shared cover/light/switch/lock signature
  /// should carry for every entity type.
  /// @param device_id Device to update.
  /// @param dimmable  New value for IoDevice::dimmable.
  void set_dimmable(const std::string &device_id, bool dimmable);

  /// Select a device's travel profile. Like set_dimmable() this is a declared preference with no
  /// protocol readback — nothing on the wire reports which profile a device is in.
  /// @param device_id Hexadecimal node ID string.
  /// @param silent    True to send position moves in "silent operation" (slower) mode.
  void set_silent(const std::string &device_id, bool silent);

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

  /// Record that a remote's typed-broadcast presses (e.g. "all awnings") should also apply to
  /// @p device_id, matching how 1W remotes address a device class rather than a single node.
  /// Independent of add_linked_remote()'s id-keyed map — a device may be linked both ways;
  /// callers dedup (see IOHomeControlComponent's 1W dispatch) so it is only touched once per press.
  /// @param type      Device class the broadcast targets.
  /// @param device_id Node ID of the device to add to that class.
  void add_linked_remote_class(DeviceType type, const std::string &device_id);

  /// Retrieve the list of device IDs linked to a device class.
  /// @return Pointer to the device-ID list, or nullptr when no device is linked to that class.
  [[nodiscard]] const std::vector<std::string> *linked_devices_for_class(DeviceType type) const;

  /// Set an optimistic target position ahead of a confirming poll/response, and notify.
  ///
  /// No-op (and returns false) when the device is unknown or has `optimistic_state == false`.
  /// Never touches `position` — only the caller's later poll/response settles that. Used by
  /// both the 1W linked-remote path and HA-issued 2W cover commands so the entity shows
  /// movement direction immediately instead of only after the confirming update arrives.
  /// @param device_id         Device to update.
  /// @param target_io_position Target position in IO units (0=open, 100=closed).
  /// @return true if the optimistic state was applied.
  bool apply_optimistic_target(const std::string &device_id, float target_io_position);

  /// Clear a device's optimistic target (e.g. on STOP), and notify.
  ///
  /// No-op (and returns false) when the device is unknown or has `optimistic_state == false`.
  /// @param device_id Device to update.
  /// @return true if the optimistic target was cleared.
  bool clear_optimistic_target(const std::string &device_id);

  /// Set an optimistic slat angle ahead of a confirming poll, and notify.
  ///
  /// No-op (and returns false) when the device is unknown, has `optimistic_state == false`, or is
  /// not a tilt-capable type. Nothing else can fill the gap: unlike a position command, a tilt
  /// command's own reply carries no slat angle this hub can use. The EXECUTE ack lays its payload
  /// out differently from a status reply and is not decoded for tilt at all (see the offset
  /// constants in hub_status.cpp and
  /// tests/corpus/captures/issues/issue_60_tilt_execute_ack_tilt_block*.yaml), so without this the
  /// entity would keep showing the pre-command angle until the next status poll seconds later.
  ///
  /// Deliberately does not touch `is_stopped`, unlike apply_optimistic_target(): the HA movement
  /// animation is derived from main-position delta, and a tilt-only command does not move the
  /// main position — marking the device as moving would animate an open/close that is not
  /// happening. The EXECUTE ack settles `is_stopped` from the wire a fraction of a second later.
  /// @param device_id   Device to update.
  /// @param tilt_percent Slat angle in the same percent scale as `IoDevice::tilt` (0-100).
  /// @return true if the optimistic tilt was applied.
  bool apply_optimistic_tilt(const std::string &device_id, float tilt_percent);

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
  std::map<DeviceType, std::vector<std::string>> linked_remote_classes_;
};

}  // namespace home_io_control
}  // namespace esphome

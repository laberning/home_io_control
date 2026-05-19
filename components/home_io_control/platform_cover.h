#pragma once

/// @file platform_cover.h
/// @brief ESPHome cover entity for IO‑Homecontrol devices.
///
/// Maps IO‑Homecontrol devices (shutters, awnings, blinds) to Home Assistant cover entities
/// with position control and real‑time feedback.
///
/// Position mapping between Home Assistant and IO‑Homecontrol:
///   HA:  1.0 = fully open,  0.0 = fully closed
///   IO:  0   = fully open,  100  = fully closed
/// The conversion is: ha_position = 1.0 - (io_position / 100.0)
///
/// @warning Some device types (e.g., horizontal awnings) have inverted position mapping
///          by default (see default_inverted_for_type() in proto_frame.h). The invert
///          flag flips the HA↔IO conversion accordingly. Ensure the device's actual
///          behavior matches the mapping, or set invert_position accordingly.

#include "esphome/core/component.h"
#include "esphome/components/cover/cover.h"
#include "hub_core.h"

namespace esphome {
namespace home_io_control {

/// @brief Cover entity representing an IO‑Homecontrol shutter/awning/blind.
class IOHomeCover : public cover::Cover, public Component {
 public:
  IOHomeCover() { this->position = UNKNOWN_POSITION; }
  /// @brief Initialize the cover entity (register device, subscribe to updates, schedule initial status poll).
  void setup() override;
  /// @brief Dump configuration to log.
  void dump_config() override;
  /// @brief Get setup priority (DATA so we have device registry available).
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }

  /// @brief Return the traits object describing this cover's capabilities.
  /// @return CoverTraits configured for position, stop, and optional tilt.
  cover::CoverTraits get_traits() override;

  /// @brief Set the parent controller component.
  /// @param parent Pointer to the IOHomeControlComponent instance.
  void set_parent(IOHomeControlComponent *parent) { this->parent_ = parent; }
  /// @brief Set the unique device ID (from YAML).
  /// @param id Hexadecimal node ID string (e.g., "123ABC").
  void set_device_id(const std::string &id) { this->device_id_ = id; }
  /// @brief Set the declared device type (from YAML).
  /// @param type Device type enum.
  void set_device_type(DeviceType type) { this->device_type_ = type; }
  /// @brief Set the declared device subtype (from YAML).
  /// @param subtype Subtype value.
  void set_subtype(uint8_t subtype) { this->subtype_ = subtype; }
  /// @brief Enable or disable position inversion.
  /// Some devices (e.g., horizontal awnings) report reversed open/close semantics.
  /// @param invert True to invert the mapping (HA 1.0 → IO 0, HA 0.0 → IO 100).
  void set_invert_position(bool invert) {
    this->invert_ = invert;
    this->invert_explicit_ = true;
  }
  /// @brief Configure bounded follow-up polling while a state change is expected.
  /// @param poll_interval_ms Poll interval in milliseconds; zero keeps the default single settle poll only.
  void set_status_poll_interval(uint32_t poll_interval_ms) { this->status_poll_interval_ms_ = poll_interval_ms; }
  /// @brief Query whether this device supports tilt (slat angle) control.
  /// @return true if the underlying device type supports tilt.
  [[nodiscard]] bool supports_tilt() const;

 protected:
  /// @brief Handle cover commands from Home Assistant (open/close/stop/set_position).
  /// @param call CoverCall containing the requested operation.
  void control(const cover::CoverCall &call) override;
  /// @brief Callback invoked when the underlying device state changes.
  /// @param id Device ID that updated.
  /// @param dev Updated IoDevice state.
  void on_device_update_(const std::string &id, const IoDevice &dev);
  /// @brief Resolve the current inversion mode.
  /// @return Explicit YAML override when set, otherwise the runtime device profile.
  [[nodiscard]] bool effective_invert_() const;

  IOHomeControlComponent *parent_{nullptr};
  std::string device_id_;
  DeviceType device_type_{DeviceType::UNKNOWN};
  uint8_t subtype_{0};
  uint32_t status_poll_interval_ms_{0};
  bool invert_{false};
  bool invert_explicit_{false};
};

}  // namespace home_io_control
}  // namespace esphome

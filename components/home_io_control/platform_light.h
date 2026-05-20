#pragma once

/// @file platform_light.h
/// @brief Experimental binary light entity for IO‑Homecontrol devices.
/// @ingroup hioc_platforms
///
/// Provides a minimal on/off light representation. Position < 50 is treated as on.
/// This platform is experimental and not yet validated on real hardware.
/// @todo Validate end-to-end behavior with physical IO-Homecontrol light devices,
///       including passive state updates, startup state restoration, and any vendor-specific
///       position encodings that do not map cleanly to binary on/off semantics.
///
/// @warning This platform has not been tested with physical IO‑Homecontrol light devices.
///          Behavior is inferred from the protocol specification and may be incomplete.
///          Use with caution and verify with actual hardware before production deployment.

#include "esphome/core/component.h"
#include "esphome/components/light/light_output.h"
#include "hub_core.h"

namespace esphome {
namespace home_io_control {

/// @brief Binary light entity for IO‑Homecontrol on/off devices.
/// @ingroup hioc_platforms
class IOHomeLight : public light::LightOutput, public Component {
 public:
  /// @brief Initialize the light entity.
  void setup() override;
  /// @brief Dump configuration to log.
  void dump_config() override;
  /// @brief Get setup priority (DATA).
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }

  /// @brief Return traits: binary on/off only (no dimming).
  /// @return LightTraits with ColorMode::ON_OFF.
  light::LightTraits get_traits() override;
  /// @brief Store the HA LightState object for state updates.
  /// @param state Pointer to LightState.
  void setup_state(light::LightState *state) override { this->state_ = state; }
  void write_state(light::LightState *state) override;

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
  /// @brief Handle inbound device status updates.
  /// @param id Device ID.
  /// @param dev Updated device state.
  void on_device_update_(const std::string &id, const IoDevice &dev);

  IOHomeControlComponent *parent_{nullptr};
  light::LightState *state_{nullptr};
  std::string device_id_;
  DeviceType device_type_{DeviceType::UNKNOWN};
  uint8_t subtype_{0};
  uint32_t status_poll_interval_ms_{0};
  /// Guard so that an inbound radio status does not echo back as a new outbound command.
  bool suppress_write_{false};
};

}  // namespace home_io_control
}  // namespace esphome
#pragma once

/// @file platform_light.h
/// @brief Experimental binary light entity for IO‑Homecontrol devices.
///
/// Provides a minimal on/off light representation. Position < 50 is treated as on.
/// This platform is experimental and not yet validated on real hardware.
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

 protected:
  /// @brief Handle inbound device status updates.
  /// @param id Device ID.
  /// @param dev Updated device state.
  void on_device_update_(const std::string &id, const IoDevice &dev);

  IOHomeControlComponent *parent_{nullptr};
  light::LightState *state_{nullptr};
  std::string device_id_;
  /// Guard so that an inbound radio status does not echo back as a new outbound command.
  bool suppress_write_{false};
};

}  // namespace home_io_control
}  // namespace esphome
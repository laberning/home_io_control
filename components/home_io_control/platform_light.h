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
#include "platform_entity_base.h"

namespace esphome {
namespace home_io_control {

/// @brief Binary light entity for IO‑Homecontrol on/off devices.
/// @ingroup hioc_platforms
///
/// The device-binding setters, setup() registration ritual and poll-interval dump line come
/// from DeviceBoundEntity; only the on/off command and status decoding live here.
class IOHomeLight : public light::LightOutput, public Component, public DeviceBoundEntity {
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

 protected:
  /// @brief Handle inbound device status updates.
  /// @param id Device ID.
  /// @param dev Updated device state.
  void on_device_update_(const std::string &id, const IoDevice &dev);

  light::LightState *state_{nullptr};
  /// Guard so that an inbound radio status does not echo back as a new outbound command.
  ///
  /// This flag is a light-only asymmetry: LightState::make_call().perform() re-enters
  /// write_state(), so the inbound-update path must suppress the echo. Switches publish state
  /// without re-entering write_state(), so they need no equivalent guard.
  bool suppress_write_{false};
};

}  // namespace home_io_control
}  // namespace esphome
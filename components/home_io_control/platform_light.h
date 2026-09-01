#pragma once

/// @file platform_light.h
/// @brief Binary or dimmable light entity for IO‑Homecontrol devices.
/// @ingroup hioc_platforms
///
/// Defaults to a minimal on/off representation (position < 50 is treated as on), validated on
/// real hardware (a Somfy Izymo dimmer; see the somfy_izymo_dimmer_* captures). Setting
/// `dimmable: true` in YAML switches to ColorMode::BRIGHTNESS, mapping HA's 0.0-1.0 brightness
/// onto the same 0-100 IO position field platform_cover.cpp uses for position — confirmed on the
/// same hardware to produce real intermediate brightness levels, not just the two binary
/// extremes.
///
/// The protocol gives no machine-readable signal for whether a given LIGHT-type device actually
/// supports intermediate positions (DISCOVER_RESP's subtype byte is manufacturer-opaque) — so
/// `dimmable` is an explicit opt-in, not auto-detected. Devices that are genuinely binary-only
/// should leave it unset/false.

#include "esphome/core/component.h"
#include "esphome/components/light/light_output.h"
#include "platform_entity_base.h"

namespace esphome {
namespace home_io_control {

/// @brief Binary or dimmable light entity for IO‑Homecontrol devices.
/// @ingroup hioc_platforms
///
/// The device-binding setters, setup() registration ritual and poll-interval dump line come
/// from DeviceBoundEntity; only the on/off/brightness command and status decoding live here.
class IOHomeLight : public light::LightOutput, public Component, public DeviceBoundEntity {
 public:
  /// @brief Initialize the light entity.
  void setup() override;
  /// @brief Dump configuration to log.
  void dump_config() override;
  /// @brief Get setup priority (DATA).
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }

  /// @brief Enable brightness control (from YAML). Default false: binary on/off only.
  /// @param dimmable True to expose ColorMode::BRIGHTNESS instead of ColorMode::ON_OFF.
  void set_dimmable(bool dimmable) { this->dimmable_ = dimmable; }

  /// @brief Return traits: ColorMode::BRIGHTNESS if dimmable, else ColorMode::ON_OFF.
  light::LightTraits get_traits() override;
  /// @brief Store the HA LightState object for state updates.
  ///
  /// LightState::setup() calls this first, then — moments later in that same call, before we get
  /// a chance to do anything else — applies its own restore_mode (default ALWAYS_OFF) via a real
  /// write_state() call, exactly as if HA had requested it. That's correct for a local PWM/LED
  /// output (which needs some defined state immediately), but wrong for a radio-controlled device
  /// with no local output to initialize: it would silently turn the light off (or on) on every
  /// reflash regardless of the device's actual state. Arm suppress_write_ here so that one
  /// boot-time write is swallowed the same way an inbound-update echo already is; the delayed
  /// initial status poll (register_device_binding_(), same as covers) is the real source of truth.
  /// @param state Pointer to LightState.
  void setup_state(light::LightState *state) override {
    this->state_ = state;
    this->suppress_write_ = true;
  }
  void write_state(light::LightState *state) override;

 protected:
  /// @brief Handle inbound device status updates.
  /// @param id Device ID.
  /// @param dev Updated device state.
  void on_device_update_(const std::string &id, const IoDevice &dev);

  light::LightState *state_{nullptr};
  bool dimmable_{false};
  /// Guard so that an inbound radio status (or LightState's own boot-time restore_mode push —
  /// see setup_state()) does not echo back as a new outbound command.
  ///
  /// This flag is a light-only asymmetry: LightState::make_call().perform() re-enters
  /// write_state(), so both the inbound-update path and setup_state() must suppress that.
  /// Switches publish state without re-entering write_state(), so they need no equivalent guard.
  bool suppress_write_{false};
};

}  // namespace home_io_control
}  // namespace esphome
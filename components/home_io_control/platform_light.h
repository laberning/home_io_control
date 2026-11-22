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

#include <optional>
#include <utility>

namespace esphome {
namespace home_io_control {

/// @brief What a light output published on its LightState: the values it expects to come back as a write.
/// @ingroup hioc_platforms
struct LightReading {
  bool on{false};                   ///< Whether the device reports the light on.
  std::optional<float> brightness;  ///< The device's brightness, 0.0-1.0; nullopt for an on/off light.
};

/// @brief Whether a write_state() with these values is the echo of a published reading.
///
/// A reading and a user value are told apart by what they say: a user value that differs from the reading is
/// never an echo, and one that equals it changes nothing on the device. Brightness is compared only while the
/// light is on, as an off light shows no level.
/// @ingroup hioc_platforms
/// @param published The reading that was published.
/// @param on Whether the LightState being written is on.
/// @param brightness The LightState's brightness, 0.0-1.0.
/// @return true when the write says what the reading said.
[[nodiscard]] inline bool is_light_echo(const LightReading &published, bool on, float brightness) {
  if (published.on != on)
    return false;
  return !on || !published.brightness.has_value() || *published.brightness == brightness;
}

/// @brief The one rule for what a light output does with the writes ESPHome makes on its LightState.
/// @ingroup hioc_platforms
///
/// LightState::make_call().perform() makes the output's write_state() run on the light's next loop, and on
/// these lights a write is a radio command. A published reading must therefore never be sent back, a user
/// value must always be sent, and a transition must send only its end. Every light output of this component
/// (IOHomeLight, IOHomeHeaterNightLight, IOHomeOneWayLight) owns one gate: it publishes through it and starts
/// write_state() with should_send().
///
/// The echo is recognised by the values it carries, not by a flag: a flag swallows the next write whatever
/// it says, and a user value that arrives between the publish and the light's loop would be lost.
/// \todo A reading published while a user value still waits to be written replaces that value, and the write
/// that follows reads as the echo. A value from Home Assistant's API waits like this when the hub's loop runs after
/// the API's in the same pass (ESPHome does not keep its loop order fixed); a one-way button of the light's identity
/// pressed together with a slider value loses the value that way, as its report publishes the level. Without
/// LightState::set_external_state() the component cannot see a waiting write; ESPHome 2026.11 offers it.
class LightWriteGate {
 public:
  /// @brief Show a device's light state on its LightState, expecting the write that follows.
  ///
  /// Publishes `on` (and `brightness`, when given) only when the LightState does not already show it, and
  /// remembers it as the expected echo. The change is instant, whatever the light's default_transition_length.
  /// Publishes nothing while a transition runs: a call would end the transition and drop its end value; the next
  /// reading after the transition's end is shown.
  /// @param state The LightState to update.
  /// @param on Whether the device reports the light on.
  /// @param brightness The device's brightness, 0.0-1.0; nullopt for an on/off light.
  void publish(light::LightState *state, bool on, std::optional<float> brightness) {
    if (state->is_transformer_active())
      return;
    if (state->current_values.is_on() == on && state->remote_values.is_on() == on &&
        (!brightness.has_value() || (state->current_values.get_brightness() == *brightness &&
                                     state->remote_values.get_brightness() == *brightness)))
      return;
    this->expected_echo_ = LightReading{on, brightness};
    auto call = state->make_call();
    call.set_state(on);
    if (brightness.has_value())
      call.set_brightness(*brightness);
    // Instant: a call without a length takes the light's default_transition_length, and a transition would fade the
    // reading in and send its end to the device as a command.
    call.set_transition_length(0);
    call.set_save(false);
    call.perform();
  }

  /// @brief Swallow the next write whatever it says.
  ///
  /// LightState::setup() applies its restore_mode through a real write_state() right after setup_state(); on a
  /// radio-controlled light that would switch the device at every reboot, and the poll is the source of truth.
  void swallow_boot_write() { this->boot_write_pending_ = true; }

  /// @brief Decide whether the write_state() being made goes to the device; call it first in write_state().
  ///
  /// False while a transition runs (only its end, written once it is over, is sent), once for the boot write,
  /// and for the write that echoes the published reading. The expected echo is cleared by every call, so it
  /// swallows at most one write.
  /// @param state The LightState being written.
  /// @return true when the values go to the device.
  [[nodiscard]] bool should_send(const light::LightState *state) {
    const std::optional<LightReading> echo = std::exchange(this->expected_echo_, std::nullopt);
    if (state->is_transformer_active())
      return false;
    if (std::exchange(this->boot_write_pending_, false))
      return false;
    // The LightState's raw brightness, as the outputs read it for the command (never gamma-corrected).
    return !echo.has_value() ||
           !is_light_echo(*echo, state->current_values.is_on(), state->current_values.get_brightness());
  }

 private:
  std::optional<LightReading> expected_echo_;  ///< Set by publish(); the write it causes is not sent.
  bool boot_write_pending_{false};             ///< Set by swallow_boot_write().
};

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
  /// reflash regardless of the device's actual state. The gate swallows that one boot-time write; the
  /// delayed initial status poll (register_device_binding_(), same as covers) is the real source of truth.
  /// @param state Pointer to LightState.
  void setup_state(light::LightState *state) override {
    this->state_ = state;
    this->gate_.swallow_boot_write();
  }
  /// @brief Send the requested on/off or brightness to the device (see LightWriteGate::should_send()).
  /// @param state The LightState holding the requested values.
  void write_state(light::LightState *state) override;

 protected:
  /// @brief Handle inbound status updates of this light's device.
  /// @param dev Updated device state.
  void on_device_update_(const IoDevice &dev);

  light::LightState *state_{nullptr};
  bool dimmable_{false};
  /// Publishes the device's level and decides which writes go to the device (echo, boot write, transition).
  LightWriteGate gate_;
};

}  // namespace home_io_control
}  // namespace esphome
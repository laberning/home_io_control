#pragma once

/// @file platform_climate_controls.h
/// @brief Generated per-heater auxiliary entities: the window binary sensor, the open-window
/// detection switch and the night light.
/// @ingroup hioc_platforms
///
/// All are device-bound companions (DeviceBoundCompanion) that climate.py generates alongside a
/// `climate:` entity: the window pair when the device kind has WINDOW_COMPANIONS, the night light when
/// the YAML asks for it on a kind with NIGHT_LIGHT. They show readings the hub's poll already stores in
/// IoDevice::heating and add no radio traffic of their own, except their writes and the read-backs that
/// confirm them.
///
/// @todo Experimental: the meaning of registers 0x010E (window sensor) and 0x0106 (open-window
///       detection) is unconfirmed on hardware. An AGILIA IO radiator answers both reads (window 0,
///       detection 1, tests/corpus/captures/exchange/atlantic_agilia_exchange_status_read.yaml), but no
///       report has opened a window or switched detection off yet
///       ([issue #165](https://github.com/laberning/home_io_control/issues/165)).

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/light/light_output.h"
#include "esphome/components/switch/switch.h"
#include "esphome/core/component.h"
#include "hub_core.h"
#include "platform_entity_base.h"
#include "platform_light.h"
#include "proto_device_model.h"

namespace esphome {
namespace home_io_control {

/// @brief Binary sensor (`device_class: window`) that is ON while the heater's window sensor reports open.
/// @ingroup hioc_platforms
///
/// Reads IoDevice::heating.window_open. No state until the first poll has read register 0x010E, so
/// "closed" always means the heater said so.
class IOHomeWindowBinarySensor : public binary_sensor::BinarySensor, public Component, public DeviceBoundCompanion {
 public:
  /// @brief Subscribe to the device's updates and publish what is already known.
  void setup() override;

  /// @brief Dump sensor configuration to the log.
  void dump_config() override;

  /// @brief Get setup priority so the parent hub is available first.
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }
};

/// @brief Switch that turns the heater's open-window detection on and off.
/// @ingroup hioc_platforms
///
/// A write sends register 0x0106 and reads it back; the switch shows what the heater answered, so it
/// has no state until the first read (the boot poll) and never shows a command the heater did not
/// confirm. Disabled by default and in the config entity category.
class IOHomeOpenWindowDetectionSwitch : public switch_::Switch, public Component, public DeviceBoundCompanion {
 public:
  /// @brief Subscribe to the device's updates and publish what is already known.
  void setup() override;

  /// @brief Dump switch configuration to the log.
  void dump_config() override;

  /// @brief Get setup priority so the parent hub is available first.
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  /// @brief Write the detection setting and read it back.
  /// @param state True to switch detection on.
  void write_state(bool state) override;
};

/// @brief Dimmable light for a heater's night light (`night_light:` on a heater with io_model
/// `heater_with_light`).
/// @ingroup hioc_platforms
///
/// On/off is register 0x0144 and the brightness register 0x0142, a percentage that maps 1:1 onto Home
/// Assistant's brightness (100 % = full). The light shows what the heater reports: no state until the
/// first poll has read 0x0144, every acknowledged write is read back, and a write the heater does not
/// acknowledge leaves the light showing the heater's last reading.
///
/// Switching on writes the brightness only when it differs from what the heater holds, then the on/off
/// register; so on/off at an unchanged brightness never depends on the brightness write.
///
/// \todo Experimental: confirm on a real heater that writing 0x0144 switches the light (the heater's
///       own controller writes it, issue #222) and that writing 0x0142 dims it.
class IOHomeHeaterNightLight : public light::LightOutput, public Component, public DeviceBoundCompanion {
 public:
  /// @brief Subscribe to the device's updates and publish what is already known.
  void setup() override;

  /// @brief Dump light configuration to the log.
  void dump_config() override;

  /// @brief Get setup priority so the parent hub is available first.
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }

  /// @brief Brightness is the light's only colour mode.
  /// @return Traits with ColorMode::BRIGHTNESS.
  light::LightTraits get_traits() override;

  /// @brief Store the LightState and swallow the write its restore_mode makes at boot.
  ///
  /// LightState::setup() applies its restore_mode through a real write_state() right after this call;
  /// on a radio-controlled light that would switch the heater's light at every reboot. The poll is the
  /// source of truth, so that one write is suppressed.
  /// @param state The LightState this output belongs to.
  void setup_state(light::LightState *state) override {
    this->state_ = state;
    this->suppress_write_ = true;
  }

  /// @brief Send the requested state to the heater and read it back.
  /// @param state The LightState holding the requested values.
  void write_state(light::LightState *state) override;

 protected:
  /// @brief Publish the heater's light state, without echoing it back as a write.
  /// @param dev The device record.
  void on_device_update_(const IoDevice &dev);

  /// @brief Write one light register.
  /// @param fn NIGHT_LIGHT or NIGHT_LIGHT_BRIGHTNESS.
  /// @param value Value to write.
  /// @return true when the heater acknowledged it.
  bool write_register_(HeatingFunction fn, float value);

  light::LightState *state_{nullptr};
  /// Swallows the write_state() that LightState::make_call().perform() causes for a state this entity
  /// publishes itself (and the boot-time restore, see setup_state()).
  bool suppress_write_{false};
  /// True while write_state() runs its writes and read-backs: the device updates they cause are not
  /// published one by one (a half-updated reading would flash a stale brightness), but once at the end.
  bool writing_{false};
};

}  // namespace home_io_control
}  // namespace esphome

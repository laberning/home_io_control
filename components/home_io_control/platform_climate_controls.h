#pragma once

/// @file platform_climate_controls.h
/// @brief Generated per-heater auxiliary entities: the window binary sensor, the open-window
/// detection switch, the night light, the heating-level text sensor and the central-heating-mode sensor.
/// @ingroup hioc_platforms
///
/// All are device-bound companions (DeviceBoundCompanion) that climate.py generates alongside a
/// `climate:` entity: the window pair when the device kind has WINDOW_COMPANIONS, the night light when
/// the YAML asks for it on a kind with NIGHT_LIGHT; the heating-level text sensor is opt-in on an
/// electrical heater, and the central-heating-mode binary sensor is opt-in on a kind with
/// CENTRAL_HEATING_MODE (a towel dryer). They show readings the hub's poll already stores in
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
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/component.h"
#include "platform_entity_base.h"
#include "platform_light.h"
#include "proto_device_model.h"

namespace esphome {
namespace home_io_control {

/// @brief Text sensor (`heating_level:` on a climate entry) with the level the heater applies right now.
/// @ingroup hioc_platforms
///
/// Shows `off`, `frost protection`, `eco`, `comfort -2`, `comfort -1`, `comfort`, `boost` or `secured`
/// (heating_level_name()), read from IoDevice::heating.level: the main parameter of the heater's status
/// read. A plain heater and a pilot-wire interface are polled with that read; a register heater gets it
/// only where its programme is known to report a level (HeatingProgrammePeriod::reports_level: a towel dryer,
/// not a radiator). No state until the heater has reported a level,
/// so a heater that reports none (a pilot-wire interface, a radiator) leaves the sensor without one.
///
/// \todo Experimental on a towel dryer: its status read is captured on one unit in comfort, eco and boost,
///       not yet during a drying run or a temporary override.
class IOHomeHeatingLevelSensor : public text_sensor::TextSensor, public Component, public DeviceBoundCompanion {
 public:
  /// @brief Subscribe to the device's updates and publish what is already known.
  void setup() override;

  /// @brief Dump sensor configuration to the log.
  void dump_config() override;

  /// @brief Get setup priority so the parent hub is available first.
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }
};

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

/// @brief Binary sensor (`central_heating_mode:` on a climate entry) that is ON while a mixed towel dryer
/// runs on the house's central heating.
/// @ingroup hioc_platforms
///
/// Reads IoDevice::heating.central_heating_mode (register 0x0153, read at every poll once this sensor is
/// declared). While it is on the dryer's electric element stays off, so the card shows idle and Boost and
/// Drying do nothing. No state until the heater has answered with a value, and none for a heater that
/// answers without one (an electric unit).
///
/// \todo Experimental: read on one Atlantic Adelis mixed unit over four panel toggles (issue #243); confirm
///       on a second unit.
class IOHomeCentralHeatingModeBinarySensor : public binary_sensor::BinarySensor,
                                             public Component,
                                             public DeviceBoundCompanion {
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
/// On/off is register 0x0144 and the brightness register 0x0142, which runs 0-10000. Home Assistant's
/// brightness follows the scale of the heater's vendor app (HEATING_NIGHT_LIGHT_SCALE: 33 % = 1000,
/// 66 % = 2000, 100 % = 10000, linear in between; night_light_register_from_percent() and
/// night_light_fraction_from_register()). A Sauter Hekla's panel sets 100, 1000, 2000 and 10000, its four
/// levels (issue #222), which show as 3 %, 33 %, 66 % and 100 %. A light switched on at the percentage the
/// heater already shows writes no brightness, so the heater keeps its own level. The light shows what the heater
/// reports: no state until the first poll has read 0x0144, every acknowledged write is read back, and a write the
/// heater does not acknowledge leaves the light showing the heater's last reading.
///
/// Switching on writes the brightness only when it differs from what the heater holds, then the on/off
/// register; so on/off at an unchanged brightness never depends on the brightness write.
///
/// Switching with 0x0144 and dimming with 0x0142 are confirmed on a Sauter Hekla (issue #222).
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

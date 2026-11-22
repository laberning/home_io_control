#pragma once

/// @file platform_climate_controls.h
/// @brief Generated per-heater auxiliary entities: the heater setting switches (open-window detection, occupancy
/// detection) and the night light.
/// @ingroup hioc_platforms
///
/// Both are device-bound companions (DeviceBoundCompanion) that climate.py generates alongside a `climate:` entity: the
/// open-window detection switch when the device kind has WINDOW_COMPANIONS (made from its reading row, with the window
/// sensor), the occupancy-detection switch when the YAML asks for it on a kind with OCCUPANCY, and the night light when
/// the YAML asks for it on a kind with NIGHT_LIGHT. Every other entity that shows a heater reading (the window sensor,
/// the heating level, central-heating mode, occupancy, energy total, run timers) is a generic entity of
/// platform_device_readings.h. They show readings the hub's poll already stores in IoDevice::reading_values and add no
/// radio traffic of their own, except their writes and the read-backs that confirm them.
///
/// @todo Experimental: the meaning of register 0x0106 (open-window detection) is unconfirmed on hardware. An AGILIA IO
///       radiator answers the read (detection 1,
///       tests/corpus/captures/exchange/atlantic_agilia_exchange_status_read.yaml), but no report has switched
///       detection off yet ([issue #165](https://github.com/laberning/home_io_control/issues/165)).

#include "esphome/components/light/light_output.h"
#include "esphome/components/switch/switch.h"
#include "esphome/core/component.h"
#include "platform_device_readings.h"
#include "platform_entity_base.h"
#include "platform_light.h"
#include "proto_device_model.h"

namespace esphome {
namespace home_io_control {

/// @brief Switch for one on/off setting register of a heater.
/// @ingroup hioc_platforms
///
/// Used for the open-window detection (register 0x0106, created automatically on a heater with window
/// companions) and the opt-in `occupancy_detection:` key (register 0x0101). It shows a reading
/// (set_reading(), DeviceReading::OPEN_WINDOW_DETECTION or OCCUPANCY_DETECTION) and writes the register that
/// reading is (reading_register()). A write sends the register and reads it back; the switch shows what the heater
/// answered, so it has no state until the first read (the boot poll) and never shows a command the heater did not
/// confirm. A change made from the vendor app or on the heater shows at the next poll, or soon after the hub hears the
/// vendor's controller write it. The generated switches restore nothing and the open-window one is disabled by default,
/// in the config entity category.
class IOHomeHeaterSettingSwitch : public switch_::Switch, public Component, public DeviceReadingCompanion {
 public:
  /// @brief Subscribe to the device's updates and publish what is already known.
  void setup() override;

  /// @brief Dump switch configuration to the log.
  void dump_config() override;

  /// @brief Get setup priority so the parent hub is available first.
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  /// @brief Write the setting and read it back.
  /// @param state True to switch the setting on.
  void write_state(bool state) override;
};

/// @brief Dimmable light for a heater's night light (`night_light:` on a heater with io_model
/// `heater_with_light`).
/// @ingroup hioc_platforms
///
/// On/off is register 0x0144 and the brightness register 0x0142, which runs 0-10000. The light writes and shows
/// a percentage: the brightness register's row maps it to the register value on the scale of the heater's vendor
/// app (HEATING_NIGHT_LIGHT_SCALE: 33 % = 1000, 66 % = 2000, 100 % = 10000, linear in between), so a reading holds
/// percent. A Sauter Hekla's panel sets 100, 1000, 2000 and 10000, its four
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
  /// source of truth, so the gate swallows that one write.
  /// @param state The LightState this output belongs to.
  void setup_state(light::LightState *state) override {
    this->state_ = state;
    this->gate_.swallow_boot_write();
  }

  /// @brief Send the requested state to the heater and read it back (see LightWriteGate::should_send()).
  /// @param state The LightState holding the requested values.
  void write_state(light::LightState *state) override;

 protected:
  /// @brief Publish the heater's light state, without echoing it back as a write.
  /// @param dev The device record.
  void on_device_update_(const IoDevice &dev);

  /// @brief Write one light register.
  /// @param fn NIGHT_LIGHT or NIGHT_LIGHT_BRIGHTNESS.
  /// @param value Value to write: 0 or 1 for the on/off register, a percentage for the brightness.
  /// @param unit Appended to the value in the log line of a write the heater did not acknowledge ("%" for a
  ///        percentage).
  /// @return true when the heater acknowledged it.
  bool write_register_(DeviceRegister fn, float value, const char *unit = "");

  light::LightState *state_{nullptr};
  /// Publishes the heater's light and decides which writes go to the heater (echo, boot restore, transition).
  LightWriteGate gate_;
  /// True while write_state() runs its writes and read-backs: the device updates they cause are not
  /// published one by one (a half-updated reading would flash a stale brightness), but once at the end.
  bool writing_{false};
};

}  // namespace home_io_control
}  // namespace esphome

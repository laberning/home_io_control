#pragma once

/// @file platform_climate_controls.h
/// @brief Generated per-heater auxiliary entities: the window binary sensor, the heater setting switches
/// (open-window detection, occupancy detection), the night light, the heating-level text sensor and the
/// register binary sensors (central-heating mode, occupancy) and the register sensor (energy total, run timers).
/// @ingroup hioc_platforms
///
/// All are device-bound companions (DeviceBoundCompanion) that climate.py generates alongside a
/// `climate:` entity: the window pair when the device kind has WINDOW_COMPANIONS, the night light when
/// the YAML asks for it on a kind with NIGHT_LIGHT; the heating-level text sensor is opt-in on an
/// electrical heater, the central-heating-mode binary sensor is opt-in on a kind with CENTRAL_HEATING_MODE (a
/// towel dryer), and the occupancy-detection switch and occupancy binary sensor are opt-in on a kind with
/// OCCUPANCY, the energy sensor is opt-in on a kind with ENERGY_METER, and the run-timer sensors are opt-in on a kind
/// with RUN_TIMERS (a towel dryer). They show readings the hub's poll already stores in IoDevice::heating and add no
/// radio traffic of their own, except their writes and the read-backs that confirm them.
///
/// @todo Experimental: the meaning of registers 0x010E (window sensor) and 0x0106 (open-window
///       detection) is unconfirmed on hardware. An AGILIA IO radiator answers both reads (window 0,
///       detection 1, tests/corpus/captures/exchange/atlantic_agilia_exchange_status_read.yaml), but no
///       report has opened a window or switched detection off yet
///       ([issue #165](https://github.com/laberning/home_io_control/issues/165)).

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/light/light_output.h"
#include "esphome/components/sensor/sensor.h"
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

/// @brief A device-bound companion that shows one heater register (the base of the register entities below).
/// @ingroup hioc_platforms
///
/// The generated code names the register with set_function(); the entity reads its value from
/// IoDevice::heating, which the hub's poll fills, and shows nothing until the heater has answered with one.
class HeaterRegisterCompanion : public DeviceBoundCompanion {
 public:
  /// @brief Name the register this entity shows (generated code).
  /// @param fn A register the readings hold (HeatingReadings::value_of() or number_of()).
  void set_function(HeatingFunction fn) { this->function_ = fn; }

 protected:
  /// @brief The value the heater last reported for this entity's register.
  /// @param dev The device record.
  /// @return The register's integer value, or -1 while it has not been read (or was dropped).
  [[nodiscard]] int16_t reading_(const IoDevice &dev) const { return dev.heating.value_of(this->function_); }

  /// @brief The number the heater last reported for this entity's register, for a register kept as a number with
  /// a unit (HeatingReadings::number_of()): wide enough for a counter that reaches past 32767.
  /// @param dev The device record.
  /// @return The register's value, or NaN while it has not been read (or was dropped).
  [[nodiscard]] float number_reading_(const IoDevice &dev) const { return dev.heating.number_of(this->function_); }

  HeatingFunction function_{HeatingFunction::COUNT};  ///< The register; COUNT until the generated code names it.
};

/// @brief Binary sensor that shows one flag register of a heater, with no state until the heater has reported it.
/// @ingroup hioc_platforms
///
/// Used for the opt-in `central_heating_mode:` (register 0x0153) and `occupancy:` (register 0x0110) keys of a
/// climate entry. The register is read at every poll once the entity is declared (the climate entity asks the
/// hub, see HeatingOptInReads). ON is 1 and OFF is 0, so "off" always means the heater said so: nothing read, a
/// reading dropped, and a heater that answers without a value (its register is absent) all leave the sensor
/// without a state.
///
/// While a mixed towel dryer runs on central heating its electric element stays off, so the card shows idle and
/// Boost and Drying do nothing; `occupancy:` is on while the heater sees someone in the room.
///
/// \todo Experimental: the central-heating mode is read on one Atlantic Adelis mixed unit over four panel
///       toggles (issue #243); confirm on a second unit. The occupancy is read on a Sauter Hekla with the
///       detection on (issue #222); what it shows with the detection off is not confirmed, and no other heater
///       has answered it yet.
class IOHomeHeaterRegisterBinarySensor : public binary_sensor::BinarySensor,
                                         public Component,
                                         public HeaterRegisterCompanion {
 public:
  /// @brief Subscribe to the device's updates and publish what is already known.
  void setup() override;

  /// @brief Dump sensor configuration to the log.
  void dump_config() override;

  /// @brief Get setup priority so the parent hub is available first.
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }
};

/// @brief Sensor that shows one numeric register of a heater, with no state until the heater has reported it.
/// @ingroup hioc_platforms
///
/// Used for the opt-in `energy:` key of a climate entry (the energy total, register 0x0116, in kWh) and for the
/// run timers of a towel dryer: `boost_duration:` and `drying_duration:` show one register in minutes, and
/// `remaining_time:` shows the minutes left of whichever run is active (set_shows_timed_run_remaining()). The unit,
/// device class, state class and accuracy come from the key's schema (climate.py), so another numeric register
/// is a table row. The register is read at every poll once the entity is declared (the climate entity asks the
/// hub, see HeatingOptInReads); the minutes left of a run are read only while that run is on, and show 0 otherwise.
/// Nothing read, a reading dropped, and a heater that answers without a value (its register is absent) all leave the
/// sensor without a state, never at 0.
///
/// \todo Experimental except on a Sauter Tahual: the total equals that radiator's display (PR #238); compare it
///       with the display of an EMOTION 3 H, an Adelis and a Hekla.
/// \todo Experimental: the run timers are read on Atlantic Adelis units only (issue #202); `drying_duration:` is the
///       duration of the last drying run started, which a change on the panel alone does not update.
class IOHomeHeaterRegisterSensor : public sensor::Sensor, public Component, public HeaterRegisterCompanion {
 public:
  /// @brief Show the minutes left of the run the dryer is in instead of one register (generated code).
  /// @details The value follows the temporary state, 0 while no boost or drying runs (timed_run_remaining_minutes()),
  ///          so the entity has no single register and set_function() is not called for it.
  void set_shows_timed_run_remaining() { this->shows_timed_run_remaining_ = true; }

  /// @brief Subscribe to the device's updates and publish what is already known.
  void setup() override;

  /// @brief Dump sensor configuration to the log.
  void dump_config() override;

  /// @brief Get setup priority so the parent hub is available first.
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  bool shows_timed_run_remaining_{false};  ///< The minutes left of the active run, not one register.
};

/// @brief Switch for one on/off setting register of a heater.
/// @ingroup hioc_platforms
///
/// Used for the open-window detection (register 0x0106, created automatically on a heater with window
/// companions) and the opt-in `occupancy_detection:` key (register 0x0101). A write sends the register and reads
/// it back; the switch shows what the heater answered, so it has no state until the first read (the boot poll)
/// and never shows a command the heater did not confirm. A change made from the vendor app or on the heater
/// shows at the next poll. The generated switches restore nothing and the open-window one is disabled by default,
/// in the config entity category.
class IOHomeHeaterSettingSwitch : public switch_::Switch, public Component, public HeaterRegisterCompanion {
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

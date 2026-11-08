#pragma once

/// @file platform_water_heater.h
/// @brief Water heater entity for a domestic hot water heater, and its opt-in companion sensors.
/// @ingroup hioc_platforms
///
/// Maps a domestic hot water heater (device_feature::WATER_HEATER, proto_dhw.h) onto an ESPHome
/// `water_heater`: water temperature, target temperature and operating mode. The heater's three modes map
/// to ESPHome's as follows (ADR 0068):
///
/// | Heater (register 0x0006) | ESPHome mode  |
/// |--------------------------|---------------|
/// | 0 automatic              | HEAT_PUMP     |
/// | 1 manual, ECO on         | ECO           |
/// | 2 manual, ECO off        | PERFORMANCE   |
///
/// OFF is not offered: no heater has been seen reporting an off state.
///
/// State is observed, not predicted, as on the climate entity: the hub polls the heater (default every 5
/// minutes) and this entity publishes from IoDevice::dhw, which only a decoded reply changes. Until a reading
/// has arrived an accepted write shows the commanded value; after that the read-back decides.
///
/// The companions (energy, heat pump running, electric backup running, boost) show readings the same poll
/// already stores and add no radio traffic.
///
/// The readings, the target EXECUTE and the mode write are confirmed on one Atlantic Calypso Connecte
/// ([issue #198](https://github.com/laberning/home_io_control/issues/198)): its display follows both writes.
/// That heater accepts only its own fixed targets (one per shower count), so a value between two steps is
/// not taken.
///
/// \todo The mode mapping and the target steps rest on that one heater; confirm them on a second one.

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/water_heater/water_heater.h"
#include "esphome/core/component.h"
#include "platform_companion_sensors.h"
#include "platform_entity_base.h"
#include "proto_dhw.h"

namespace esphome {
namespace home_io_control {

/// @brief ESPHome water heater mode for a heater mode.
/// @param mode Mode as register 0x0006 holds it.
/// @return The mapped ESPHome mode (see the file comment).
water_heater::WaterHeaterMode water_heater_mode_for(DhwMode mode);

/// @brief Heater mode for an ESPHome water heater mode.
/// @param mode ESPHome mode.
/// @param[out] out The heater mode; untouched when false is returned.
/// @return false for a mode the heater does not offer (OFF, ELECTRIC, HIGH_DEMAND, GAS).
bool dhw_mode_for(water_heater::WaterHeaterMode mode, DhwMode &out);

/// @brief Water heater entity for a domestic hot water heater.
/// @ingroup hioc_platforms
///
/// The device-binding setters and setup() registration ritual come from DeviceBoundEntity.
class IOHomeWaterHeater : public water_heater::WaterHeater, public Component, public DeviceBoundEntity {
 public:
  /// @brief Register the entity with the shared hub and schedule the first poll of the heater.
  void setup() override;
  /// @brief Dump configuration to the log.
  void dump_config() override;
  /// @brief Get setup priority (DATA).
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }

  /// @brief A call bound to this entity (the pattern of ESPHome's template water heater).
  /// @return A new call.
  water_heater::WaterHeaterCallInternal make_call() override { return water_heater::WaterHeaterCallInternal(this); }

  /// @brief Apply a Home Assistant request: a mode is written to register 0x0006, a target is sent in an
  /// EXECUTE. Each is independent; a failed send leaves its field as it was.
  /// @param call The requested changes, already checked against traits() by ESPHome.
  void control(const water_heater::WaterHeaterCall &call) override;

 protected:
  /// @brief Current and target temperature, the three modes, and the target range
  /// DHW_TARGET_MIN_C..DHW_TARGET_MAX_C at a 0.5 C step.
  water_heater::WaterHeaterTraits traits() override;

  /// @brief Inbound device-update hook: fold IoDevice::dhw into the entity and publish when a shown field
  /// changed. Ignores updates for other devices.
  void on_device_update_(const std::string &id, const IoDevice &dev);

  /// @brief Copy the readings into the entity's fields.
  /// @param readings What the hub has read from the heater.
  /// @return true when a published field changed.
  bool sync_from_readings_(const DhwReadings &readings);

  bool observed_{false};  ///< True once a reading has been folded in.
};

/// @brief Sensor of the heater's energy since installation, in kWh (`total_increasing`).
/// @ingroup hioc_platforms
///
/// Reads IoDevice::dhw.energy_wh (register 0x0004, Wh) and publishes it in kWh, so it fits the Home
/// Assistant energy dashboard. No state until the first poll has read it.
class IOHomeDhwEnergySensor : public IOHomeCompanionSensor {
 public:
  /// @brief Subscribe to the device's updates and publish what is already known.
  void setup() override;
  /// @brief Dump sensor configuration to the log.
  void dump_config() override;
};

/// @brief Binary sensor of one heating state of the heater: heat pump running, electric backup running, or
/// boost on.
/// @ingroup hioc_platforms
///
/// No state until the first poll has read the register it shows, so "off" always means the heater said so.
class IOHomeDhwBinarySensor : public binary_sensor::BinarySensor, public Component, public DeviceBoundCompanion {
 public:
  /// @brief Which reading the sensor shows.
  enum class Reading : uint8_t {
    HEAT_PUMP,        ///< Register 0x0021 bytes 3/4: the heat pump runs.
    ELECTRIC_BACKUP,  ///< Register 0x0021 byte 2: the electric element heats.
    BOOST,            ///< Register 0x0000: boost is on.
  };

  /// @brief Choose the reading (codegen sets it once).
  /// @param reading Reading to show.
  void set_reading(Reading reading) { this->reading_ = reading; }

  /// @brief Subscribe to the device's updates and publish what is already known.
  void setup() override;
  /// @brief Dump sensor configuration to the log.
  void dump_config() override;
  /// @brief Get setup priority so the parent hub is available first.
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  Reading reading_{Reading::HEAT_PUMP};
};

}  // namespace home_io_control
}  // namespace esphome

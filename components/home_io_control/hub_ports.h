#pragma once

/// @file hub_ports.h
/// @brief The three interfaces through which the entity platforms reach the hub (ADR 0084).
/// @ingroup hioc_hub
///
/// An entity holds a pointer to the port of its kind, never to the hub class: HubDevicePort for the
/// device-bound entities and their companions, HubHeatingPort for the climate and water heater entities
/// and their controls, HubControlPort for the entities that act on the hub as a whole. The hub
/// (IOHomeControlComponent) implements all three, so the generated `set_parent(hub)` call converts to
/// whichever port an entity takes; tests implement a port with a small double instead of building a hub.
///
/// The ports are contracts: they say what an entity may ask of the hub and what it can rely on, not how
/// the hub does it. A hub method an entity needs is a port method; anything else on the hub is out of the
/// entities' reach. The headers included here are the types the method signatures name; this header
/// never includes a hub header.

#include "device_registry.h"
#include "esphome/core/hal.h"
#include "oneway_controller.h"
#include "oneway_transmitter.h"
#include "proto_dhw.h"
#include "proto_heating.h"
#include "proto_heating_profiles.h"

#include <cstdint>
#include <functional>
#include <string>

namespace esphome {
namespace home_io_control {

/// @brief What a device-bound entity or companion needs from the hub: register its device, read the
/// device record, follow its updates, queue radio work for it and set its poll intervals.
/// @ingroup hioc_hub
///
/// All radio work an entity asks for is queued: a `queue_*` call returns at once and the hub performs it
/// from its main loop, so an entity never blocks on the radio. The device is addressed by its
/// hexadecimal node ID string. Calls for an ID the hub does not know are ignored, except where a method
/// says otherwise.
class HubDevicePort {
 public:
  // === Registry ===

  /// @brief Declare a device to the hub, with the metadata from the entity's YAML.
  /// @param device_id Hexadecimal node ID string.
  /// @param cfg Declared type, subtype, model, capability class and per-device overrides.
  virtual void add_device(const std::string &device_id, const DeviceConfig &cfg) = 0;

  /// @brief Declare a device by its ID only, with an undeclared kind that discovery may still resolve.
  /// @param device_id Hexadecimal node ID string.
  virtual void add_device(const std::string &device_id) = 0;

  /// @brief Read a device's record.
  /// @param device_id Hexadecimal node ID string.
  /// @return The record, or nullptr when the device is not declared. The pointer is valid until the
  ///         next declaration of a device.
  [[nodiscard]] virtual IoDevice *get_device(const std::string &device_id) = 0;

  /// @brief Subscribe to every device update. The callback receives the ID of the updated device and
  /// its record, so a subscriber filters for its own device.
  /// @param cb Called after each update, from the main loop.
  virtual void register_device_callback(DeviceUpdateCallback cb) = 0;

  /// @brief Hub-wide radio health: true once the hub has seen its radio stop responding after startup.
  /// Sticky until a reboot.
  [[nodiscard]] virtual bool radio_stopped_responding() const = 0;

  /// @brief Render a device's "last commanded by" text, naming this hub when it was the commander.
  /// @param dev Device record to read.
  [[nodiscard]] virtual std::string describe_last_commander(const IoDevice &dev) const = 0;

  // === Per-device settings ===

  /// @brief Select how fast a device's position moves run.
  /// @param device_id Hexadecimal node ID string.
  /// @param speed Speed profile.
  virtual void set_device_travel_speed(const std::string &device_id, TravelSpeed speed) = 0;

  /// @brief Set the interval of the follow-up polls while a state change is expected.
  /// @param device_id Hexadecimal node ID string.
  /// @param poll_interval_ms Interval in milliseconds; zero keeps the single settle poll only.
  virtual void set_device_status_poll_interval(const std::string &device_id, uint32_t poll_interval_ms) = 0;

  /// @brief Opt a device into the periodic rain sensor poll.
  /// @param device_id Hexadecimal node ID string.
  /// @param interval_ms Interval in milliseconds; zero removes the device from the schedule.
  virtual void set_device_rain_poll_interval(const std::string &device_id, uint32_t interval_ms) = 0;

  /// @brief Opt a device into the periodic sun level poll.
  /// @param device_id Hexadecimal node ID string.
  /// @param interval_ms Interval in milliseconds; zero removes the device from the schedule.
  virtual void set_device_sun_poll_interval(const std::string &device_id, uint32_t interval_ms) = 0;

  /// @brief Opt a device into the periodic battery level poll.
  /// @param device_id Hexadecimal node ID string.
  /// @param interval_ms Interval in milliseconds; zero removes the device from the schedule.
  virtual void set_device_battery_level_poll_interval(const std::string &device_id, uint32_t interval_ms) = 0;

  /// @brief Opt a device into the status refresh at rest: a status read whenever the device has reported
  /// nothing for a whole interval.
  /// @param device_id Hexadecimal node ID string.
  /// @param interval_ms Interval in milliseconds; zero removes the device from the schedule.
  virtual void set_device_status_refresh_interval(const std::string &device_id, uint32_t interval_ms) = 0;

  // === Optimistic state ===

  /// @brief Show a target position before the device confirms it, and notify subscribers. Does nothing
  /// for an unknown device or one that is not optimistic.
  /// @param device_id Hexadecimal node ID string.
  /// @param target_io_position Target in IO units (0 = open, 100 = closed).
  /// @return True when the prediction was applied.
  virtual bool apply_optimistic_target(const std::string &device_id, float target_io_position) = 0;

  /// @brief Show a stop before the device confirms it, and notify subscribers. If the stop then fails, the
  /// prediction it replaced comes back.
  /// @param device_id Hexadecimal node ID string.
  /// @return True when the prediction was applied.
  virtual bool apply_optimistic_stop(const std::string &device_id) = 0;

  /// @brief Show a slat angle before the device confirms it, and notify subscribers. Does nothing for an
  /// unknown device, a non-optimistic one or one that cannot tilt.
  /// @param device_id Hexadecimal node ID string.
  /// @param tilt_percent Slat angle in percent (0-100).
  /// @return True when the prediction was applied.
  virtual bool apply_optimistic_tilt(const std::string &device_id, float tilt_percent) = 0;

  // === Queued radio work ===

  /// @brief Queue a status read.
  /// @param device_id Hexadecimal node ID string.
  virtual void queue_request_device_status(const std::string &device_id) = 0;

  /// @brief Queue a read of the device's name.
  /// @param device_id Hexadecimal node ID string.
  virtual void queue_request_device_name(const std::string &device_id) = 0;

  /// @brief Queue a named command (stop, favorite, vent, ...).
  /// @param device_id Hexadecimal node ID string.
  /// @param cmd Command to send.
  /// @return True when the command was queued: the hub is running, the device is declared and the
  ///         command fits its capability class.
  virtual bool queue_device_command(const std::string &device_id, CoverCommand cmd) = 0;

  /// @brief Queue a position move. A tilt move queued for the same device is merged into one exchange.
  /// @param device_id Hexadecimal node ID string.
  /// @param position Target position (0-100).
  virtual void queue_set_device_position(const std::string &device_id, uint8_t position) = 0;

  /// @brief Queue a tilt move. A position move queued for the same device is merged into one exchange.
  /// @param device_id Hexadecimal node ID string.
  /// @param tilt_percent Target tilt (0-100).
  virtual void queue_set_device_tilt(const std::string &device_id, uint8_t tilt_percent) = 0;

  /// @brief Queue a combined position and tilt move.
  /// @param device_id Hexadecimal node ID string.
  /// @param position Target position (0-100).
  /// @param tilt_percent Target tilt (0-100).
  virtual void queue_set_device_position_and_tilt(const std::string &device_id, uint8_t position,
                                                  uint8_t tilt_percent) = 0;

  /// @brief Queue a light level.
  /// @param device_id Hexadecimal node ID string.
  /// @param position Level in IO units (0-100).
  virtual void queue_set_light_position(const std::string &device_id, uint8_t position) = 0;

  /// @brief Queue a light on or off.
  /// @param device_id Hexadecimal node ID string.
  /// @param on Desired state.
  virtual void queue_set_light_state(const std::string &device_id, bool on) = 0;

  /// @brief Queue a fan speed.
  /// @param device_id Hexadecimal node ID string.
  /// @param position Speed in IO units (0-100).
  virtual void queue_set_fan_position(const std::string &device_id, uint8_t position) = 0;

  /// @brief Queue a switch on or off.
  /// @param device_id Hexadecimal node ID string.
  /// @param on Desired state.
  virtual void queue_set_switch_state(const std::string &device_id, bool on) = 0;

  /// @brief Queue a lock or unlock.
  /// @param device_id Hexadecimal node ID string.
  /// @param locked Desired state.
  virtual void queue_set_lock_state(const std::string &device_id, bool locked) = 0;

 protected:
  HubDevicePort() = default;
  HubDevicePort(const HubDevicePort &) = default;
  HubDevicePort &operator=(const HubDevicePort &) = default;
  ~HubDevicePort() = default;  ///< Protected and non-virtual: nothing deletes a hub through its port.
};

/// @brief What the climate and water heater entities and their controls need from the hub: read and
/// write a heater's registers and drive a water heater's settings.
/// @ingroup hioc_hub
///
/// The register and water heater calls run radio exchanges and block the main loop until the exchange
/// ends; entities call them from their control handlers, and each returns whether the device accepted the
/// request. set_device_temperature_range() only records a setting and sends nothing.
class HubHeatingPort {
 public:
  /// @brief Run one heating function on a heater's register protocol: a write for a settable register, a
  /// read for a read-only one.
  /// @param device_id Hexadecimal node ID string.
  /// @param fn Function to run.
  /// @param value Value to write; ignored for a read.
  /// @return True when the device accepted it.
  virtual bool send_heating_command(const std::string &device_id, HeatingFunction fn, float value) = 0;

  /// @brief Read one register of a heater and fold the decoded value into the device record.
  /// @param device_id Hexadecimal node ID string.
  /// @param fn Register to read.
  /// @param[out] value_out Receives the decoded value on success; may be null.
  /// @param max_tries Upper bound on read attempts.
  /// @return True when the register was read.
  virtual bool read_heating_register(const std::string &device_id, HeatingFunction fn, float *value_out,
                                     uint8_t max_tries) = 0;

  /// @brief Send a planned climate write and read the register back.
  /// @param device_id Hexadecimal node ID string.
  /// @param plan The writes to send, in order.
  /// @param on_accepted Called once the device accepted the write, before the read-back.
  /// @return True when the write was accepted.
  virtual bool execute_heating_plan(const std::string &device_id, const HeatingWritePlan &plan,
                                    const std::function<void()> &on_accepted) = 0;

  /// @brief Set the temperature range a heater's temperatures are encoded in.
  /// @param device_id Hexadecimal node ID string.
  /// @param min_c Lower end in degrees Celsius.
  /// @param max_c Upper end in degrees Celsius.
  virtual void set_device_temperature_range(const std::string &device_id, float min_c, float max_c) = 0;

  /// @brief Set a water heater's target temperature.
  /// @param device_id Hexadecimal node ID string.
  /// @param celsius Target in degrees Celsius.
  /// @return True when the device accepted it.
  virtual bool set_water_heater_target(const std::string &device_id, float celsius) = 0;

  /// @brief Set a water heater's operating mode.
  /// @param device_id Hexadecimal node ID string.
  /// @param mode Mode to select.
  /// @return True when the device accepted it.
  virtual bool set_water_heater_mode(const std::string &device_id, DhwMode mode) = 0;

  /// @brief Start or end a water heater's boost.
  /// @param device_id Hexadecimal node ID string.
  /// @param on Desired state.
  /// @return True when the device accepted it.
  virtual bool set_water_heater_boost(const std::string &device_id, bool on) = 0;

  /// @brief Start or end a water heater's away mode.
  /// @param device_id Hexadecimal node ID string.
  /// @param on Desired state.
  /// @return True when the device accepted it.
  virtual bool set_water_heater_away(const std::string &device_id, bool on) = 0;

 protected:
  HubHeatingPort() = default;
  HubHeatingPort(const HubHeatingPort &) = default;
  HubHeatingPort &operator=(const HubHeatingPort &) = default;
  ~HubHeatingPort() = default;  ///< Protected and non-virtual: nothing deletes a hub through its port.
};

/// @brief What the hub-level entities need from the hub: pairing, scanning, the arming switches, the
/// one-way sender, the device event and pairing result text, and the LR1121 update.
/// @ingroup hioc_hub
///
/// A callback setter holds one callback; a second call replaces the first, as only one entity of each kind
/// exists per hub. The one-way report callbacks are a list, one entry per controller identity.
class HubControlPort {
 public:
  // === Pairing and scanning ===

  /// @brief Queue a discovery and pairing attempt. A press handled shortly after the end of an attempt is
  /// dropped, because the device has usually left its pairing mode by then.
  virtual void queue_discover_and_pair() = 0;

  /// @brief Run the roll-call of paired devices and publish its report. Blocks the main loop for the
  /// duration of the sweep and does nothing but report a failure while the radio is busy.
  virtual void trigger_scan_paired_devices() = 0;

  /// @brief Register the callback that runs after every pairing attempt.
  /// @param cb Called with no arguments.
  virtual void set_pairing_result_callback(std::function<void()> cb) = 0;

  /// @brief Describe the most recent pairing attempt as one structured line for the result text sensor.
  [[nodiscard]] virtual std::string pairing_result_text() const = 0;

  // === Arming switches ===

  /// @brief Arm or disarm the key extraction responder, which lets the user's existing hub pair to this
  /// one and take over its keys. The hub disarms it again by itself after a timeout.
  /// @param armed Desired state.
  virtual void set_key_extraction_armed(bool armed) = 0;

  /// @brief Register the callback that reports every change of the key extraction armed state, including
  /// those the hub makes itself.
  /// @param cb Called with the new state.
  virtual void set_key_extraction_armed_callback(std::function<void(bool)> cb) = 0;

  /// @brief Arm or disarm the one-way key adoption listener, which reports one overheard enrollment
  /// broadcast and then disarms itself. Receive only.
  /// @param armed Desired state.
  virtual void set_oneway_key_adoption_armed(bool armed) = 0;

  /// @brief Register the callback that reports every change of the one-way key adoption armed state,
  /// including those the hub makes itself.
  /// @param cb Called with the new state.
  virtual void set_oneway_key_adoption_armed_callback(std::function<void(bool)> cb) = 0;

  // === One-way ===

  /// @brief Queue a one-way button action, sent as the given controller identity.
  /// @param controller_id Handle of the identity.
  /// @param action Button action to send.
  virtual void send_oneway_action(const std::string &controller_id, OneWayButtonAction action) = 0;

  /// @brief Queue a one-way enrollment of the given controller identity.
  /// @param controller_id Handle of the identity.
  virtual void send_oneway_enroll(const std::string &controller_id) = 0;

  /// @brief Subscribe to the report that follows every one-way command attempt, successful or not.
  /// @param callback Called for every attempt of every identity; a subscriber filters for its own.
  virtual void add_oneway_command_report_callback(OneWayCommandReportFn callback) = 0;

  // === Device events ===

  /// @brief The most recent device event as text, or an empty string before the first one.
  [[nodiscard]] virtual const std::string &last_device_event() const = 0;

  /// @brief Register the callback that runs after every recorded device event.
  /// @param cb Called with no arguments.
  virtual void set_device_event_callback(std::function<void()> cb) = 0;

#ifdef IOHOME_LR1121_FIRMWARE_UPDATE
  // === LR1121 update ===

  /// @brief Run the LR1121 transceiver firmware update. Ends in a radio restart or a reboot.
  virtual void trigger_lr1121_firmware_update() = 0;

#ifdef IOHOME_LR1121_BOOTLOADER_UPDATE
  /// @brief Allow or forbid the irreversible bootloader rewrite for the next update. A permission only.
  /// @param allowed Desired state.
  virtual void set_bootloader_rewrite_allowed(bool allowed) = 0;
#endif
#endif

 protected:
  HubControlPort() = default;
  HubControlPort(const HubControlPort &) = default;
  HubControlPort &operator=(const HubControlPort &) = default;
  ~HubControlPort() = default;  ///< Protected and non-virtual: nothing deletes a hub through its port.
};

}  // namespace home_io_control
}  // namespace esphome

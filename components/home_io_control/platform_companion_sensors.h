#pragma once

/// @file platform_companion_sensors.h
/// @brief The auto-generated per-device diagnostic companion sensors.
/// @ingroup hioc_platforms
///
/// Every device-bound platform (cover, light, switch, lock) gets the same read-only companions
/// generated alongside it by platform_common.py: smoothed RSSI, seconds since last contact,
/// cumulative exchange-failure and unconfirmed-exchange counts, the stored device name, the
/// currently outstanding CMD_ERROR_RESP reason, and the last-command record's
/// commander/originator. They share the DeviceBoundCompanion binding (observe-only: no
/// add_device(), no polling) and an all-but-identical setup()/dump_config() skeleton, so they
/// live together here rather than in a file pair each.
///
/// What each one still owns is its dump_config() label and whatever its setup() has to do beyond
/// binding; everything they share sits in the three bases below.

#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/component.h"
#include "platform_entity_base.h"

namespace esphome {
namespace home_io_control {

/// @brief Shared base for the numeric per-device diagnostic companions.
///
/// Carries the three-base inheritance and the setup priority every one of them needs. DATA keeps
/// them behind the hub, so `parent_` is usable by the time their setup() runs.
/// @ingroup hioc_platforms
class IOHomeCompanionSensor : public sensor::Sensor, public Component, public DeviceBoundCompanion {
 public:
  /// @brief Get setup priority so the parent hub is available first.
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }
};

/// @brief Shared base for the textual per-device diagnostic companions.
///
/// The text-sensor counterpart of IOHomeCompanionSensor; same reasoning.
/// @ingroup hioc_platforms
class IOHomeCompanionTextSensor : public text_sensor::TextSensor, public Component, public DeviceBoundCompanion {
 public:
  /// @brief Get setup priority so the parent hub is available first.
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }
};

/// @brief Shared base for the companions that publish one cumulative `uint16_t` counter held on
/// IoDevice.
///
/// These differ only in which field they read and what they call themselves, so the binding lives
/// here once and each subclass supplies the field through counter_value(). Zero is a meaningful
/// reading for all of them — "none yet", not "no data" — so unlike RSSI and Last Contact they
/// publish unconditionally on setup.
/// @ingroup hioc_platforms
class IOHomeDeviceCounterSensor : public IOHomeCompanionSensor {
 public:
  /// @brief Register the device-update subscription and publish the initial cached count.
  void setup() final;

 protected:
  /// @brief Return the counter this sensor publishes.
  /// @param dev Device record to read the count from.
  [[nodiscard]] virtual uint16_t counter_value(const IoDevice &dev) const = 0;
};

/// @brief Diagnostic sensor that publishes a device's smoothed (EMA) RSSI in dBm.
///
/// Publishes nothing until the first RX from this device seeds the EMA (see
/// detail::update_link_health() in hub_internal.h) — Home Assistant shows the entity as
/// unavailable until then, rather than a misleading 0 dBm.
/// @ingroup hioc_platforms
class IOHomeRssiSensor : public IOHomeCompanionSensor {
 public:
  /// @brief Register the device-update subscription and publish the initial cached state.
  void setup() override;

  /// @brief Dump sensor configuration to the log.
  void dump_config() override;
};

/// @brief Diagnostic sensor that publishes seconds elapsed since the last frame received from a
/// device (see detail::update_link_health() in hub_internal.h).
///
/// This is an age, not a timestamp: it resets to ~0 on every frame from the device — including
/// replies to the hub's own status polls and commands, not just traffic the device sends
/// unprompted — and counts up from there. A periodic heartbeat (see HEARTBEAT_INTERVAL_MS in the
/// .cpp) re-publishes it even when the device stays quiet, so the value keeps advancing in Home
/// Assistant instead of freezing at whatever it was at the last frame. Publishes nothing until
/// the first frame is seen.
/// @ingroup hioc_platforms
class IOHomeLastContactSensor : public IOHomeCompanionSensor {
 public:
  /// @brief Register the device-update subscription, start the heartbeat, and publish the
  /// initial cached state.
  void setup() override;

  /// @brief Dump sensor configuration to the log.
  void dump_config() override;

 protected:
  /// @brief Compute and publish seconds since `dev.last_seen_ms`; no-op before the first frame.
  /// @param dev Device to read `last_seen_ms` from.
  void publish_age_(const IoDevice &dev);
};

/// @brief Diagnostic sensor that publishes a device's cumulative count of outbound exchanges
/// that timed out (no valid response) — see detail::record_exchange_timeout() in
/// hub_internal.h.
///
/// Unlike the RSSI and Last Contact sensors, zero is a meaningful value here (no failures yet),
/// so this publishes on setup unconditionally.
/// @ingroup hioc_platforms
class IOHomeExchangeFailuresSensor : public IOHomeDeviceCounterSensor {
 public:
  /// @brief Dump sensor configuration to the log.
  void dump_config() override;

 protected:
  /// @copydoc IOHomeDeviceCounterSensor::counter_value
  [[nodiscard]] uint16_t counter_value(const IoDevice &dev) const override { return dev.exchange_timeout_count; }
};

/// @brief Diagnostic sensor that publishes a device's cumulative count of exchanges it
/// authenticated and then never closed — see detail::record_exchange_unconfirmed() in
/// hub_internal.h.
///
/// Read it against Exchange Failures: that counter rising alone means the device is not hearing
/// the hub, while this one rising means it hears the hub and the reply is lost on the way back.
/// For a movement command this outcome is reported as success, so this sensor is the only place
/// it shows up.
///
/// Zero is meaningful here (none yet), so this publishes on setup unconditionally.
/// @ingroup hioc_platforms
class IOHomeUnconfirmedExchangesSensor : public IOHomeDeviceCounterSensor {
 public:
  /// @brief Dump sensor configuration to the log.
  void dump_config() override;

 protected:
  /// @copydoc IOHomeDeviceCounterSensor::counter_value
  [[nodiscard]] uint16_t counter_value(const IoDevice &dev) const override { return dev.exchange_unconfirmed_count; }
};

/// @brief Diagnostic text sensor that publishes the cached device name.
///
/// Beyond the shared companion behavior it also queues one boot-time GET_NAME request so the
/// cache gets populated without waiting for unrelated traffic.
/// @ingroup hioc_platforms
class IOHomeDeviceNameTextSensor : public IOHomeCompanionTextSensor {
 public:
  /// @brief Register the device-update subscription and schedule an initial name fetch.
  void setup() override;

  /// @brief Dump text-sensor configuration to the log.
  void dump_config() override;
};

/// @brief Diagnostic text sensor that publishes the symbolic name of a device's most recent
/// CMD_ERROR_RESP result code (e.g. "LIMITATION_BY_RAIN"), letting a "nothing happened" command
/// self-explain instead of only showing up in the log. Shared by every device-bound platform
/// (cover, light, switch, lock) via platform_common.py's companion-sensor codegen.
///
/// Not a per-operation outcome — it does not get set on every command, only on an explicit
/// CMD_ERROR_RESP. Publishes an empty string until the first one is seen, and again after any
/// subsequent successful status/command reply clears it (see detail::clear_command_result()), so
/// a non-empty value always means "this is still going on" rather than "this is what happened
/// last."
/// @ingroup hioc_platforms
class IOHomeActiveIssueTextSensor : public IOHomeCompanionTextSensor {
 public:
  /// @brief Register the device-update subscription and publish the initial cached state.
  void setup() override;

  /// @brief Dump text-sensor configuration to the log.
  void dump_config() override;
};

/// @brief Diagnostic text sensor naming the controller that last commanded this device.
///
/// Read from bytes the device already includes in every status reply — no extra radio traffic and
/// no probe. Last-writer-wins and inherently stale: it only changes when something actually
/// commands the device, and a foreign controller's node ID has no name unless the user recognises
/// it. Publishes an empty string until the first status reply carrying the record arrives.
/// @ingroup hioc_platforms
class IOHomeLastCommandedByTextSensor : public IOHomeCompanionTextSensor {
 public:
  void setup() override;
  void dump_config() override;
};

/// @brief Diagnostic text sensor naming what kind of source issued that last command.
///
/// The device's own Command Originator byte, rendered "name(0xXX)". Field-validated as a clean
/// remote-vs-motor-button split on roller shutters only; other device classes may report values
/// with no ORIGINATOR_* name, which surface as "unknown(0xXX)" rather than being dropped.
/// @ingroup hioc_platforms
class IOHomeLastCommandSourceTextSensor : public IOHomeCompanionTextSensor {
 public:
  void setup() override;
  void dump_config() override;
};

}  // namespace home_io_control
}  // namespace esphome

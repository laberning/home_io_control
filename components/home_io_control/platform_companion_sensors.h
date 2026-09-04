#pragma once

/// @file platform_companion_sensors.h
/// @brief The five auto-generated per-device diagnostic companion sensors.
/// @ingroup hioc_platforms
///
/// Every device-bound platform (cover, light, switch, lock) gets the same five read-only
/// companions generated alongside it by platform_common.py: smoothed RSSI, seconds since last
/// contact, cumulative exchange-failure count, the stored device name, and the currently
/// outstanding CMD_ERROR_RESP reason. They share the DeviceBoundCompanion binding (observe-only:
/// no add_device(), no polling) and an all-but-identical setup()/dump_config() skeleton, so they
/// live together here rather than in five near-duplicate file pairs.

#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/component.h"
#include "platform_entity_base.h"

namespace esphome {
namespace home_io_control {

/// @brief Diagnostic sensor that publishes a device's smoothed (EMA) RSSI in dBm.
///
/// Publishes nothing until the first RX from this device seeds the EMA (see
/// detail::update_link_health() in hub_internal.h) — Home Assistant shows the entity as
/// unavailable until then, rather than a misleading 0 dBm.
/// @ingroup hioc_platforms
class IOHomeRssiSensor : public sensor::Sensor, public Component, public DeviceBoundCompanion {
 public:
  /// @brief Register the device-update subscription and publish the initial cached state.
  void setup() override;

  /// @brief Dump sensor configuration to the log.
  void dump_config() override;

  /// @brief Get setup priority so the parent hub is available first.
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }
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
class IOHomeLastContactSensor : public sensor::Sensor, public Component, public DeviceBoundCompanion {
 public:
  /// @brief Register the device-update subscription, start the heartbeat, and publish the
  /// initial cached state.
  void setup() override;

  /// @brief Dump sensor configuration to the log.
  void dump_config() override;

  /// @brief Get setup priority so the parent hub is available first.
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }

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
class IOHomeExchangeFailuresSensor : public sensor::Sensor, public Component, public DeviceBoundCompanion {
 public:
  /// @brief Register the device-update subscription and publish the initial cached state.
  void setup() override;

  /// @brief Dump sensor configuration to the log.
  void dump_config() override;

  /// @brief Get setup priority so the parent hub is available first.
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }
};

/// @brief Diagnostic text sensor that publishes the cached device name.
///
/// Beyond the shared companion behavior it also queues one boot-time GET_NAME request so the
/// cache gets populated without waiting for unrelated traffic.
/// @ingroup hioc_platforms
class IOHomeDeviceNameTextSensor : public text_sensor::TextSensor, public Component, public DeviceBoundCompanion {
 public:
  /// @brief Register the device-update subscription and schedule an initial name fetch.
  void setup() override;

  /// @brief Dump text-sensor configuration to the log.
  void dump_config() override;

  /// @brief Get setup priority so the parent hub is available first.
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }
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
class IOHomeActiveIssueTextSensor : public text_sensor::TextSensor, public Component, public DeviceBoundCompanion {
 public:
  /// @brief Register the device-update subscription and publish the initial cached state.
  void setup() override;

  /// @brief Dump text-sensor configuration to the log.
  void dump_config() override;

  /// @brief Get setup priority so the parent hub is available first.
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }
};

}  // namespace home_io_control
}  // namespace esphome

#pragma once

/// @file platform_exchange_failures_sensor.h
/// @brief Diagnostic sensor exposing a device's cumulative exchange-timeout count.
/// @ingroup hioc_platforms

#include "esphome/components/sensor/sensor.h"
#include "esphome/core/component.h"
#include "platform_entity_base.h"

namespace esphome {
namespace home_io_control {

/// @brief Diagnostic sensor that publishes a device's cumulative count of outbound exchanges
/// that timed out (no valid response) — see detail::record_exchange_timeout() in
/// hub_internal.h.
///
/// Unlike the RSSI and Last Seen sensors, zero is a meaningful value here (no failures yet), so
/// this publishes on setup unconditionally.
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

}  // namespace home_io_control
}  // namespace esphome

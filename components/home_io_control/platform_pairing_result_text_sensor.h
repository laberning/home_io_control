#pragma once

/// @file platform_pairing_result_text_sensor.h
/// @brief Diagnostic text sensor exposing the machine-readable "Last Pairing Result" string.
/// @ingroup hioc_platforms

#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/component.h"
#include "hub_core.h"
#include "platform_entity_base.h"

namespace esphome {
namespace home_io_control {

/// @brief Diagnostic text sensor that publishes PairingTelemetry::result_sensor_string()
/// after every pairing attempt.
///
/// The published string is the frozen `v1;...` format documented on
/// PairingTelemetry::result_sensor_string() — the Phase 2 automated-rig read-back contract.
/// Nothing is published before the first pairing attempt of this boot.
/// @ingroup hioc_platforms
class IOHomePairingResultTextSensor : public text_sensor::TextSensor, public Component, public HubBoundEntity {
 public:
  /// @brief Register the pairing-result callback.
  void setup() override;

  /// @brief Dump text-sensor configuration to the log.
  void dump_config() override;

  /// @brief Get setup priority so the parent hub is available first.
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  /// @brief Publish the latest pairing telemetry result string.
  void on_pairing_result_();
};

}  // namespace home_io_control
}  // namespace esphome

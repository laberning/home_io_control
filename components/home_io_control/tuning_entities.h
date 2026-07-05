#pragma once

/// @file tuning_entities.h
/// @brief Home Assistant `number` and `select` entities that control runtime tuning parameters.
/// @ingroup hioc_tuning
///
/// These are thin entity classes that forward user changes from Home Assistant into the
/// hub's `TuningConfig`. Each entity is identified by the YAML key of the parameter it
/// controls, so the C++ side stays generic and new parameters do not require new classes.

#include "esphome/core/component.h"
#include "esphome/components/number/number.h"
#include "esphome/components/select/select.h"

#include <string>
#include <utility>
#include <vector>

namespace esphome {
namespace home_io_control {

// Forward declaration of the hub component.
class IOHomeControlComponent;

/// @brief Runtime-tunable `number` entity.
///
/// Forwards HA slider changes to the hub via the YAML key name. The hub updates the
/// matching `TuningConfig` field and applies the value to the radio driver when needed.
class IOHomeTuningNumber : public number::Number, public Component {
 public:
  /// @brief Construct a tuning number entity.
  /// @param parent Hub component that owns the tuning configuration.
  /// @param name YAML key of the parameter this slider controls (used for logs and dispatch).
  IOHomeTuningNumber(IOHomeControlComponent *parent, std::string name)
      : parent_(parent), param_name_(std::move(name)) {}

  /// @brief Publish the current tuning value so the HA slider reflects the active configuration on boot.
  void setup() override;
  /// @brief Empty dump_config override (kept for compatibility).
  void dump_config() override {}

 protected:
  /// @brief Called by ESPHome when the user changes the slider value in HA.
  /// @param value New value from the UI.
  void control(float value) override;

  IOHomeControlComponent *parent_{nullptr};
  std::string param_name_;
};

/// @brief Runtime-tunable `select` entity.
///
/// Forwards HA dropdown selections to the hub via the YAML key name. The option strings
/// match the YAML enumeration values so the user can copy the active value back to YAML.
class IOHomeTuningSelect : public select::Select, public Component {
 public:
  /// @brief Construct a tuning select entity.
  /// @param parent Hub component that owns the tuning configuration.
  /// @param name YAML key of the parameter this dropdown controls.
  IOHomeTuningSelect(IOHomeControlComponent *parent, std::string name)
      : parent_(parent), param_name_(std::move(name)) {}

  /// @brief Publish the current tuning option so the HA dropdown reflects the active configuration on boot.
  void setup() override;
  /// @brief Empty dump_config override (kept for compatibility).
  void dump_config() override {}

 protected:
  /// @brief Called by ESPHome when the user selects a new option in HA.
  /// @param value Selected option string.
  void control(const std::string &value) override;

  IOHomeControlComponent *parent_{nullptr};
  std::string param_name_;
};

}  // namespace home_io_control
}  // namespace esphome

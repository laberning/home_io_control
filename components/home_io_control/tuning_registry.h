/// @file tuning_registry.h
/// @brief Table-driven registry of runtime tuning parameters.
/// @ingroup hioc_tuning
///
/// The hub exposes each tuning parameter through Home Assistant `number`/`select`
/// entities. Rather than hand-syncing four string-dispatch functions in the hub, this
/// registry enumerates every parameter once, as a table row pairing the parameter's
/// wire name with getter/setter function pointers over TuningConfig. The hub's
/// update/get methods become thin table lookups.
///
/// @par Adding a tuning parameter
/// Add exactly two things and the cross-language sync check (`make tuning-sync`) will
/// keep them honest:
///   1. one row in the appropriate table below (`NUMBER_PARAMS` or `SELECT_PARAMS`), and
///   2. one matching entry in `tuning.py` (`_NUMBER_PARAMS` or `_SELECT_OPTIONS`).
/// The parameter `name` string must be identical on both sides.

#pragma once

#include "tuning_config.h"

#include <string>

namespace esphome {
namespace home_io_control {

/// One numeric tuning parameter: its wire name plus accessors over TuningConfig.
///
/// `get` returns the stored value as a float (for HA slider seeding); `set` writes the
/// value back, narrowing to the field's underlying integer type. `applies_to_radio`
/// marks parameters whose change must be pushed to the active radio driver immediately.
struct TuningNumberParam {
  const char *name;                    ///< YAML/HA parameter name (must match tuning.py).
  float (*get)(const TuningConfig &);  ///< Read the current value as a float.
  void (*set)(TuningConfig &, float);  ///< Write a new value (narrowed to the field type).
  bool applies_to_radio;               ///< True if changes must be re-applied to the radio.
};

/// One select tuning parameter: its wire name plus string accessors over TuningConfig.
///
/// `get` returns the current option string (matching the HA dropdown labels). `set`
/// parses an option string and returns false when the value is unparseable, so the hub
/// can skip the radio re-apply exactly as the original per-parameter logic did.
struct TuningSelectParam {
  const char *name;                                  ///< YAML/HA parameter name (must match tuning.py).
  std::string (*get)(const TuningConfig &);          ///< Read the current option string.
  bool (*set)(TuningConfig &, const std::string &);  ///< Parse/apply an option; false if unparseable.
  bool applies_to_radio;                             ///< True if changes must be re-applied to the radio.
};

/// Look up a numeric tuning parameter by name; returns nullptr if unknown.
const TuningNumberParam *find_tuning_number(const std::string &name);

/// Look up a select tuning parameter by name; returns nullptr if unknown.
const TuningSelectParam *find_tuning_select(const std::string &name);

/// @name Table iteration
/// Range accessors over the full parameter tables, for enumeration and tests.
/// @{
const TuningNumberParam *tuning_number_params_begin();
const TuningNumberParam *tuning_number_params_end();
const TuningSelectParam *tuning_select_params_begin();
const TuningSelectParam *tuning_select_params_end();
/// @}

}  // namespace home_io_control
}  // namespace esphome

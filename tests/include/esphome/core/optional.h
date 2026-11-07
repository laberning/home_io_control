#pragma once

// Host-test stub of esphome/core/optional.h: ESPHome names std::optional in its own namespace, and the
// real headers spell their signatures `optional<T>`. Mirrored so the stubs can spell them the same way.

#include <optional>

namespace esphome {

using std::make_optional;
using std::optional;

}  // namespace esphome

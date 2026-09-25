#pragma once

/// @file entity_helpers.h
/// @brief Conversions and renderers shared by the hub and its Home Assistant entities.
/// @ingroup hioc_hub
///
/// Carries no dependency on the hub itself, so an entity can include this header instead of the
/// hub's private helpers (make include-graph enforces that split).

#include "log_helpers.h"
#include "proto_constants.h"
#include "proto_device_model.h"
#include "proto_frame.h"
#include "proto_sizes.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

namespace esphome {
namespace home_io_control {
namespace detail {

// ============================================================================
// Shared constants
// ============================================================================

inline constexpr float BINARY_ENTITY_ON_POSITION_THRESHOLD =
    50.0F;  ///< Shared 0-100 cutoff: values below this mean binary "on".

// ============================================================================
// Percent conversion helpers
// ============================================================================

/// @brief Convert a 0.0-1.0 HA fraction (position, tilt, or brightness) to a 0-100 IO percent.
///
/// Rounds rather than truncates: HA quantizes call values to 0-255 before they ever reach us, so
/// its "50%" is 128/255=0.50196, not exactly 0.5 — a truncating cast compounds that quantization
/// into a consistent ~1% bias, caught on real hardware in both platform_cover.cpp (position and
/// tilt) and platform_light.cpp (brightness). Callers apply their own invert/complement logic
/// (e.g. `1.0F - fraction`) before calling this; it only owns the rounding.
/// @param fraction Value in [0.0, 1.0].
/// @return Rounded 0-100 percent.
inline uint8_t round_percent(float fraction) { return static_cast<uint8_t>(std::lround(fraction * 100.0F)); }

// ============================================================================
// Last-command record rendering
// ============================================================================

/// @brief Render a Command Originator byte as "name(0xXX)", e.g. "user_remote(0x01)".
///
/// The one rendering every originator display shares (the "Last Command Source" sensor and the 0x71
/// status-update log line), so a byte with no ORIGINATOR_* case reads "unknown(0x0A)" everywhere
/// rather than being silently dropped or mislabelled.
/// @param originator Command Originator byte (ORIGINATOR_*).
/// @return "name(0xXX)".
inline std::string format_originator(uint8_t originator) {
  return format_name_and_hex(originator_name(originator), originator);
}

/// @brief Render the "Last Commanded By" sensor string.
///
/// Always leads with the raw node ID — that is the diagnostic value, and the only thing a user can
/// match against a remote they own. The qualifier is additive, never a substitute: a device naming
/// its own ID is NOT reliably "the button on the motor" (the one non-shutter this project has data
/// on, a mains gate, names its own ID with an undefined originator), so the cause belongs to the
/// separate originator sensor, not to this one's wording.
/// @param dev Device record to read.
/// @param hub_node_id This hub's own 3-byte node ID.
/// @return e.g. "3B74DC", "C0FFEE (this hub)", "2FE2D2 (this device)"; empty before the first record.
inline std::string describe_last_commander(const IoDevice &dev, const uint8_t *hub_node_id) {
  if (!dev.has_last_command)
    return {};
  std::string out = node_id_to_string(dev.last_commander);
  if (memcmp(dev.last_commander, hub_node_id, NODE_ID_SIZE) == 0) {
    out += " (this hub)";
  } else if (memcmp(dev.last_commander, dev.node_id, NODE_ID_SIZE) == 0) {
    out += " (this device)";
  }
  return out;
}

/// @brief Render the "Last Command Source" sensor string.
///
/// Rendered by format_originator(), so an undecoded byte keeps its raw hex. The decode is
/// field-validated for roller shutters (a clean 0x00/0x01 split, remote vs. motor
/// button); gates, lights and multi-channel units are not validated and are expected to surface
/// undecoded values here — which is the point of keeping the raw hex in the string.
/// @param dev Device record to read.
/// @return e.g. "user_remote(0x01)"; empty before the first record.
inline std::string describe_last_command_source(const IoDevice &dev) {
  if (!dev.has_last_command)
    return {};
  return format_originator(dev.last_command_originator);
}

}  // namespace detail
}  // namespace home_io_control
}  // namespace esphome

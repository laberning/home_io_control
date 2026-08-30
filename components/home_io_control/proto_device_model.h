#pragma once

/// @file proto_device_model.h
/// @brief IO-Homecontrol device-type model, capabilities and runtime device state.
/// @ingroup hioc_protocol
///
/// DeviceType/DeviceCapabilityClass, the capability predicates, packed-metadata
/// decoders, cover commands, position/tilt report decoding and the IoDevice
/// runtime struct.

#include "proto_sizes.h"

#include <cstdint>
#include <string>

namespace esphome {
namespace home_io_control {

// ============================================================================
// Device Types — from the IO-Homecontrol specification
// ============================================================================

/// @brief Device type identifiers reported by IO‑Homecontrol products.
/// The numeric values follow the official specification. Do not reassign or reorder these.
enum class DeviceType : uint8_t {
  UNKNOWN = 0x00,                        ///< Unknown/unspecified device.
  VENETIAN_BLIND = 0x01,                 ///< Venetian blind.
  ROLLER_SHUTTER = 0x02,                 ///< Roller shutter.
  AWNING = 0x03,                         ///< Awning.
  WINDOW_OPENER = 0x04,                  ///< Window opening actuator.
  GARAGE_OPENER = 0x05,                  ///< Garage door opener.
  LIGHT = 0x06,                          ///< Binary light.
  GATE_OPENER = 0x07,                    ///< Gate opener.
  ROLLING_DOOR_OPENER = 0x08,            ///< Rolling door opener.
  LOCK = 0x09,                           ///< Lock.
  BLIND = 0x0A,                          ///< Generic blind.
  SCREEN = 0x0B,                         ///< Insect/privacy screen.
  BEACON = 0x0C,                         ///< Beacon (unpaired/announcement). Not YAML-selectable: a
                                         ///< discovery pseudo-type, not a configurable actuator.
                                         ///< Allowlisted in device_type_sync_test.cpp.
  DUAL_SHUTTER = 0x0D,                   ///< Dual-section shutter.
  HEATING_TEMPERATURE_INTERFACE = 0x0E,  ///< Heating temperature interface.
  ON_OFF_SWITCH = 0x0F,                  ///< Generic on/off switch.
  HORIZONTAL_AWNING = 0x10,              ///< Horizontal awning (open/close inverted).
  EXTERNAL_VENETIAN_BLIND = 0x11,        ///< External venetian blind.
  LOUVRE_BLIND = 0x12,                   ///< Louvre blind.
  CURTAIN_TRACK = 0x13,                  ///< Curtain track.
  // VENTILATION_POINT (capability class SWITCH), EXTERIOR_HEATING and HEAT_PUMP (both CLIMATE)
  // decode and carry capability classes but are deliberately withheld from YAML: no platform
  // consumes CLIMATE yet, and the climate/ventilation platform is unbuilt. They are allowlisted
  // in device_type_sync_test.cpp's reverse check; move them into __init__.py's
  // DEVICE_TYPE_OPTIONS (and yaml_device_type_name()) when that platform lands.
  VENTILATION_POINT = 0x14,  ///< Ventilation point.
  EXTERIOR_HEATING = 0x15,   ///< Exterior heating.
  HEAT_PUMP = 0x16,          ///< Heat pump.
  INTRUSION_ALARM = 0x17,    ///< Intrusion alarm.
  SWINGING_SHUTTER = 0x18,   ///< Swinging shutter.
};

/// @brief High‑level capability class derived from DeviceType.
enum class DeviceCapabilityClass : uint8_t {
  UNKNOWN = 0x00,  ///< Unknown capability.
  COVER = 0x01,    ///< Position‑controlled cover (shutter/blind/awning).
  LIGHT = 0x02,    ///< Binary on/off light.
  SWITCH = 0x03,   ///< Binary on/off switch.
  SENSOR = 0x04,   ///< Sensor device.
  BEACON = 0x05,   ///< Beacon.
  CLIMATE = 0x06,  ///< Climate device (heating/cooling).
  LOCK = 0x07,     ///< Lock.
};

/// @brief Convert a DeviceType to a lowercase string identifier.
/// @param type Device type enum.
/// @return Null‑terminated string name (e.g., "roller_shutter").
const char *device_type_name(DeviceType type);

/// @brief Return the YAML-friendly device-type name for types exposed in the Python schema.
/// @param type Device type enum.
/// @return Null‑terminated string (e.g., "external_venetian_blind"), or nullptr if the type
///         has no symbolic YAML alias (user must use a raw numeric value).
const char *yaml_device_type_name(DeviceType type);

/// @brief Map a raw IO‑Homecontrol type to the closest ESPHome/Home Assistant entity family.
/// @param type Raw device type.
/// @return Capability class (COVER, LIGHT, SWITCH, etc.).
DeviceCapabilityClass device_capability_class(DeviceType type);

/// @brief Get a human‑readable name for a capability class.
/// @param type Device type (unused, kept for signature compatibility).
/// @return String like "cover", "light", "switch", "unknown".
const char *device_capability_class_name(DeviceType type);

/// @brief Does this device type support precise position control (0–100)?
/// @param type Device type.
/// @return true for cover‑family devices.
bool device_supports_position_control(DeviceType type);

/// @brief Does this device type support binary on/off control?
/// @param type Device type.
/// @return true for lights and switches.
bool device_supports_binary_control(DeviceType type);

/// @brief Does this device type support status request commands (0x03)?
/// @param type Device type.
/// @return true for covers, binary devices, and lock devices.
bool device_supports_status_requests(DeviceType type);

/// @brief Does this device type support binary lock/unlock control via execute commands?
/// @param type Device type.
/// @return true for lock devices.
bool device_supports_lock_control(DeviceType type);

/// @brief Does this device type support tilt (slat angle) control?
/// @param type Device type.
/// @return true for venetian blinds, blinds, external venetian blinds, louvre blinds.
bool device_supports_tilt(DeviceType type);

/// @brief Does this device type support the ventilation position command?
///
/// The ventilation command moves window-type actuators to a predefined
/// partially-open position suitable for air exchange without fully opening.
/// @param type Device type.
/// @return true for WINDOW_OPENER and VENTILATION_POINT.
bool device_supports_vent(DeviceType type);

/// @brief Decode a protocol-packed device type from two metadata bytes.
/// @param type_msb First metadata byte.
/// @param type_subtype Second metadata byte containing the remaining type bits and subtype.
/// @return Decoded device type.
DeviceType decode_packed_device_type(uint8_t type_msb, uint8_t type_subtype);

/// @brief Decode a protocol-packed device subtype from the second metadata byte.
/// @param type_subtype Second metadata byte containing subtype in bits [5:0].
/// @return Manufacturer-specific subtype.
uint8_t decode_packed_device_subtype(uint8_t type_subtype);

/// @brief Encode a DeviceType/subtype pair into the two-byte packed metadata format used by
/// discovery responses — the inverse of decode_packed_device_type()/decode_packed_device_subtype().
/// @param type Device type to encode.
/// @param subtype Manufacturer-specific subtype; only bits [5:0] are used.
/// @param type_msb Output: first metadata byte.
/// @param type_subtype Output: second metadata byte (top 2 type bits + 6-bit subtype).
void encode_packed_device_type(DeviceType type, uint8_t subtype, uint8_t &type_msb, uint8_t &type_subtype);

/// @brief Human‑readable operation profile name for a device type.
/// Used for logging and diagnostics.
/// @param type Device type.
/// @return String such as "cover_position", "cover_position_tilt", "binary_on_off", "lock", etc.
const char *device_operation_profile_name(DeviceType type);

/// @brief Human-readable device type string for diagnostics, including the raw numeric value.
/// @param type Device type.
/// @return String such as "horizontal_awning (0x10)", or the raw hex form ("0x1A") when the
///         type has no symbolic name.
std::string format_device_type_diagnostic(DeviceType type);

/// @brief Build the YAML value for a device's `io_device_type` key.
/// @param type Device type.
/// @return A quoted symbolic name (e.g. `"horizontal_awning"`) when one exists, otherwise the
///         raw hex form (e.g. `0x1A`) for a type with no YAML alias.
std::string format_device_type_for_yaml(DeviceType type);

/// @brief Build the ready-to-paste YAML block describing a device, for both a fully-decoded
/// device and one whose type/subtype wasn't reported.
///
/// With `metadata_complete == false`, the type/subtype are unknown, so the returned snippet
/// uses a `<cover|light|switch|lock>` platform placeholder and a commented-out
/// `io_device_type` explanation; `type`, `subtype`, and `inverted` are ignored in this case.
///
/// With `metadata_complete == true`, the snippet names the concrete ESPHome platform for
/// `type` and fills in `io_subtype` and (for an inverted cover) `invert_position: true`. If
/// `type` has no known ESPHome platform, an empty string is returned instead — the caller
/// decides what to say when there's no snippet to show.
///
/// `low_power: true` is emitted in **both** shapes when `low_power` is set (the device
/// self-reported POWER_SAVE_LOW_POWER in its discovery Multi Information Byte); it is omitted
/// entirely otherwise, since its absence is a valid, correct config.
///
/// The emitted keys must track the device-bound platform schemas (`platform_schema_extension()`
/// in platform_common.py, plus each of cover.py/light.py/switch.py/lock.py's own extra keys) by
/// hand. `make yaml-emitter-sync` (scripts/check-yaml-emitters.py) catches drift between the two
/// statically.
/// @param type Decoded device type.
/// @param subtype Decoded device subtype; only used when `metadata_complete` is true.
/// @param device_id Hex device ID string (e.g. "38B4A1").
/// @param metadata_complete Whether the discovery response included type/subtype metadata.
/// @param inverted Whether the device's open/close positions are swapped; only used when
///        `metadata_complete` is true and `type` is a cover.
/// @param low_power Whether the device self-reported a low-power / duty-cycled class; emits
///        `low_power: true` when set, in both snippet shapes.
/// @return Multi-line YAML snippet, or an empty string when `metadata_complete` is true but
///         `type` maps to no ESPHome platform.
std::string build_device_yaml_snippet(DeviceType type, uint8_t subtype, const std::string &device_id,
                                      bool metadata_complete, bool inverted, bool low_power);

// ============================================================================
// Cover Commands
// ============================================================================

/// @brief Named device commands for cover-type actuators.
///
/// These represent the discrete non-positional actions a controller can send to
/// a cover device. Each maps to a specific wire encoding in the CMD_EXECUTE payload.
/// Using this enum avoids conflating numeric positions (0–100) with command codes.
enum class CoverCommand : uint8_t {
  STOP = 0,        ///< Stop movement immediately.
  FAVORITE = 1,    ///< Move to stored favorite/"My" position.
  VENT = 2,        ///< Move to ventilation position (window-type devices).
  FORCE_OPEN = 3,  ///< Move to fully open at elevated priority; intended to bypass soft locks
                   ///< and environmental limits (confirmed on real hardware to move correctly;
                   ///< bypassing an active lock is still unconfirmed — see create_force_open()
                   ///< in proto_commands.cpp).
};

/// @brief Get a human-readable name for a CoverCommand.
/// @param cmd The cover command to name.
/// @return Null-terminated string such as "STOP", "FAVORITE", or "VENT".
const char *cover_command_name(CoverCommand cmd);

// ============================================================================
// Position Report Decoding
// ============================================================================

/// In status responses, position is encoded as a 16-bit value where
/// 0x0000 = fully open (0%) and 0xC800 = fully closed (100%).
static constexpr uint16_t STATUS_POS_MAX = 0xC800;
/// Target-reached tolerance expressed in raw IO-homecontrol position units.
/// 100 raw units out of 51200 full-scale is about 0.195%, so this only absorbs
/// tiny target/current mismatches from device rounding or early stopped flags.
static constexpr uint16_t STATUS_POS_TOLERANCE_RAW = 100;

/// Packed device metadata uses two bytes where the high 8 bits carry the upper type bits and the
/// low byte carries both the remaining type bits and the 6-bit manufacturer subtype.
static constexpr uint8_t DEVICE_METADATA_SIZE = 2;
static constexpr uint8_t DEVICE_TYPE_LOW_BITS_SHIFT = 2;
static constexpr uint8_t DEVICE_TYPE_HIGH_BITS_SHIFT = 6;
static constexpr uint8_t DEVICE_SUBTYPE_MASK = 0x3F;

// ============================================================================
// Device State
// ============================================================================

/// @brief Sentinel value meaning "position is not known yet".
/// Matches POS_UNKNOWN (0xD4 = 212 decimal) for easy debugging.
static constexpr float UNKNOWN_POSITION = 212.0F;
static constexpr uint8_t DEVICE_NAME_BUFFER_SIZE = 32;  ///< Device name storage including null terminator

/// @brief Sentinel value meaning "no RSSI sample recorded yet" for `last_rssi_dbm`/`rssi_ema_scaled`.
/// A real RSSI reading from these radios is always well above `INT16_MIN` — and so is any real
/// `rssi_ema_scaled` fixed-point value.
static constexpr int16_t RSSI_UNKNOWN_DBM = INT16_MIN;

/// @brief EMA weight denominator and fixed-point scale for `IoDevice::rssi_ema_scaled`.
/// One constant serves both roles by construction: the update `S += x − round(S/N)` blends each
/// new sample `x` in at weight 1/N while keeping `S = N × EMA`.
static constexpr int16_t RSSI_EMA_SCALE = 8;

/// @brief What the hub predicted ahead of confirmation, kept apart from what the device reported.
///
/// The observed fields on IoDevice (`position`, `target`, `tilt`, `is_stopped`) mean "this is what
/// the device last told us" and are never written by a guess. Anything the hub predicts — to give
/// the Home Assistant UI immediate feedback across the queue-dispatch and exchange gap — lands
/// here instead, so it can be withdrawn when the command that produced it fails
/// (DeviceRegistry::rollback_optimistic()) without risking a real observation.
///
/// Consumers read through effective_target() / effective_tilt() / effective_is_stopped() rather
/// than either set of fields directly.
struct OptimisticState {
  /// Predicted movement. NONE defers to the observed `is_stopped`; a bool could not express
  /// "predict stopped" (apply_optimistic_stop) distinctly from "no prediction".
  enum class Motion : uint8_t {
    NONE = 0,  ///< No movement prediction; the observed `is_stopped` decides.
    MOVING,    ///< Predicted to be travelling (a position command was issued).
    STOPPED,   ///< Predicted to be at rest (a STOP was issued).
  };

  float target{UNKNOWN_POSITION};  ///< Predicted main-position target, or UNKNOWN_POSITION.
  float tilt{UNKNOWN_POSITION};    ///< Predicted slat angle, or UNKNOWN_POSITION.
  Motion motion{Motion::NONE};     ///< Predicted movement state.

  /// @return true when nothing is predicted, so a rollback would be a no-op.
  [[nodiscard]] bool empty() const {
    return target == UNKNOWN_POSITION && tilt == UNKNOWN_POSITION && motion == Motion::NONE;
  }
  /// Withdraw every prediction.
  void clear() { *this = {}; }
  /// Withdraw the position prediction; call when a decoded position observation supersedes it.
  /// Clears `motion` with `target` because apply_optimistic_target() sets the two together.
  void clear_position() {
    target = UNKNOWN_POSITION;
    motion = Motion::NONE;
  }
  /// Withdraw the tilt prediction; call when a decoded tilt observation supersedes it.
  void clear_tilt() { tilt = UNKNOWN_POSITION; }
};

/// @brief Runtime state of a paired IO‑Homecontrol device.
struct IoDevice {
  uint8_t node_id[NODE_ID_SIZE]{};       ///< Device's 3‑byte radio address.
  DeviceType type{DeviceType::UNKNOWN};  ///< Device type (shutter, awning, etc.).
  uint8_t subtype{0};                    ///< Device subtype (manufacturer‑specific).
  char name[DEVICE_NAME_BUFFER_SIZE]{};  ///< Cached UTF-8 device name decoded from Latin-1 wire payloads.
  float position{UNKNOWN_POSITION};      ///< Current position: 0=open, 100=closed, or UNKNOWN_POSITION.
  float tilt{UNKNOWN_POSITION};          ///< Current tilt: 0=closed, 100=open, or UNKNOWN_POSITION.
  float target{UNKNOWN_POSITION};        ///< Target position the device is moving toward.
  bool is_stopped{true};                 ///< True if device is not moving.
  bool inverted{false};                  ///< True if open/close positions are swapped (e.g., horizontal awning).
  bool silent{false};                    ///< True to send position moves with the reference hub's "silent
                                         ///< operation" extended block, which makes the motor travel more
                                         ///< slowly. Like `inverted`/`dimmable` this is a YAML-declared
                                         ///< preference, not a protocol-reported fact — nothing on the wire
                                         ///< tells us whether a device is in that mode.
  bool optimistic_state{true};           ///< True if `target` may be set ahead of a confirming poll/response.
  bool low_power{false};                 ///< YAML-declared: this target is a low-power / duty-cycled receiver, so
                                         ///< directed frames set CTRL1_LOW_POWER and use the long wake-up preamble.
                                         ///< A protocol-reported class exists (discovery Multi Information Byte) but
                                         ///< is surfaced through the pairing/scan snippet, never applied at runtime.
  bool dimmable{false};                  ///< True for a LIGHT-class device configured `dimmable: true` in YAML.
                                         ///< Not a protocol-level fact (the wire gives no dimmable-capability
                                         ///< signal) — set from platform_light.cpp's YAML config via
                                         ///< DeviceRegistry::set_dimmable(), purely for accurate profile-name
                                         ///< logging.
  uint8_t last_result_code{0};           ///< Last CMD_ERROR_RESP result byte (0 = none recorded). See
                                ///< command_result_name()/is_limitation_result() in proto_constants.h. Cleared by
                                ///< the next successful status/command reply for this device. Note: 0 is also the
                                ///< real RESULT_UNKNOWN_STATUS_REPLY wire value, so that specific explicit reply is
                                ///< indistinguishable here from "nothing recorded yet" — a known, accepted tradeoff.
  uint32_t last_result_at_ms{0};              ///< millis() timestamp of last_result_code, 0 when none recorded.
  uint32_t last_status{0};                    ///< millis() timestamp of last received status.
  int16_t last_rssi_dbm{RSSI_UNKNOWN_DBM};    ///< Most recent raw RSSI sample (dBm), or RSSI_UNKNOWN_DBM.
  int16_t rssi_ema_scaled{RSSI_UNKNOWN_DBM};  ///< Smoothed RSSI as fixed point in 1/RSSI_EMA_SCALE dBm — read
                                              ///< through device_rssi_ema_dbm(), updated by
                                              ///< detail::update_link_health() in hub_internal.h. Fixed point
                                              ///< (rather than whole dBm) keeps sub-dBm EMA contributions from
                                              ///< vanishing to integer truncation, so the average converges on a
                                              ///< stable signal instead of stalling up to RSSI_EMA_SCALE−1 dBm
                                              ///< away. RSSI_UNKNOWN_DBM before the first sample.
  uint32_t last_seen_ms{0};  ///< millis() of the last frame received from this device (any command), 0 = never.
  uint16_t exchange_timeout_count{0};  ///< Cumulative count of outbound exchanges to this device with no valid
                                       ///< response (see detail::record_exchange_timeout() in hub_internal.h).
  uint16_t exchange_attempt_count{0};  ///< Cumulative attempts (`ExchangeEngine::DebugInfo::tries`, 1-based per
                                       ///< exchange) across those timed-out exchanges only — attempts within an
                                       ///< ultimately successful exchange are not counted (deliberate scope limit).
  OptimisticState optimistic{};        ///< Hub-side predictions; see OptimisticState. Never observation.
};

/// @brief Convert an `rssi_ema_scaled` fixed-point value to whole dBm (round half away from zero).
/// @param scaled Fixed-point EMA value in 1/RSSI_EMA_SCALE dBm units (not the sentinel).
/// @return Rounded dBm value.
inline int16_t rssi_scaled_to_dbm(int16_t scaled) {
  constexpr int16_t half = RSSI_EMA_SCALE / 2;
  return static_cast<int16_t>((scaled + (scaled >= 0 ? half : -half)) / RSSI_EMA_SCALE);
}

/// @brief A device's smoothed RSSI in whole dBm.
/// @param dev Device record to read.
/// @return Rounded EMA in dBm, or RSSI_UNKNOWN_DBM when no sample has been recorded yet.
inline int16_t device_rssi_ema_dbm(const IoDevice &dev) {
  return dev.rssi_ema_scaled == RSSI_UNKNOWN_DBM ? RSSI_UNKNOWN_DBM : rssi_scaled_to_dbm(dev.rssi_ema_scaled);
}

/// @brief The main-position target a consumer should act on: the prediction when one stands,
/// otherwise the device's own last reported target.
/// @param dev Device record to read.
/// @return The predicted target when set, otherwise `dev.target`.
inline float effective_target(const IoDevice &dev) {
  return dev.optimistic.target != UNKNOWN_POSITION ? dev.optimistic.target : dev.target;
}

/// @brief The slat angle a consumer should act on, prediction first.
/// @param dev Device record to read.
/// @return The predicted tilt when set, otherwise `dev.tilt`.
inline float effective_tilt(const IoDevice &dev) {
  return dev.optimistic.tilt != UNKNOWN_POSITION ? dev.optimistic.tilt : dev.tilt;
}

/// @brief Whether a consumer should treat the device as at rest, prediction first.
/// @param dev Device record to read.
/// @return The predicted movement state when one stands, otherwise `dev.is_stopped`.
inline bool effective_is_stopped(const IoDevice &dev) {
  switch (dev.optimistic.motion) {
    case OptimisticState::Motion::MOVING:
      return false;
    case OptimisticState::Motion::STOPPED:
      return true;
    case OptimisticState::Motion::NONE:
    default:
      return dev.is_stopped;
  }
}

/// @brief Determine whether a device type has inverted position mapping by default.
/// @param type Device type.
/// @return true for horizontal awnings; false otherwise.
bool default_inverted_for_type(DeviceType type);

/// @brief Decode target/current position values from a status frame.
/// @param target_raw 16‑bit raw target value.
/// @param current_raw 16‑bit raw current value.
/// @param is_stopped True if device reports stopped.
/// @param target Output target position (0–100 or UNKNOWN_POSITION).
/// @param position Output current position (0–100 or UNKNOWN_POSITION).
void decode_position_report(uint16_t target_raw, uint16_t current_raw, bool is_stopped, float &target, float &position);
/// @brief Has the device reached its target within tolerance?
/// @param target Target position (0–100 or UNKNOWN_POSITION).
/// @param position Current position (0–100 or UNKNOWN_POSITION).
/// @return true if positions match within STATUS_POS_TOLERANCE_RAW.
bool has_reached_target_position(float target, float position);
/// @brief Decode tilt angle from raw 16‑bit value.
/// @param tilt_raw Raw tilt value from status frame.
/// @return Tilt percentage (0 = closed, 100 = open) or UNKNOWN_POSITION.
float decode_tilt_report(uint16_t tilt_raw);

}  // namespace home_io_control
}  // namespace esphome

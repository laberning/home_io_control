#pragma once

/// @file proto_device_model.h
/// @brief IO-Homecontrol capability predicates, packed-metadata decoders and runtime device state.
/// @ingroup hioc_protocol
///
/// The capability predicates, packed-metadata decoders, position/tilt report decoding and the IoDevice
/// runtime struct. The kind enums (DeviceType, DeviceModel, DeviceCapabilityClass, CoverCommand) are in
/// proto_device_kinds.h; the per-(type, subtype) names and flags live in the table in
/// proto_device_types.h.

#include "proto_device_features.h"
#include "proto_device_kinds.h"
#include "proto_heating.h"
#include "proto_sizes.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>

namespace esphome {
namespace home_io_control {

/// @brief Does this device take position EXECUTEs (a percent, on / off, or stop in the main
/// parameter) and answer status requests (0x03)?
/// @details Read from the capability class of the (type, subtype) row; one rule serves both
///          questions because every kind that takes position EXECUTEs reports its state with a
///          status request.
/// @param type Device type.
/// @param subtype Device subtype.
/// @return true for covers, lights, switches, locks and heat-level heaters.
bool device_takes_execute_position(DeviceType type, uint8_t subtype);

/// @brief Does this device never move, so a status reply must not start the movement settle loop?
/// @details Heaters commanded with an EXECUTE (heating levels or temperatures; climate profile) and exterior heating (a
/// heat level in the
///          position field) have no motion; their status "stopped" flag is not consulted.
/// @param type Device type.
/// @param subtype Device subtype.
/// @return true when the device has no motion.
bool device_has_no_motion(DeviceType type, uint8_t subtype);

/// @brief Can a climate entity drive this device?
///
/// True exactly when a heating profile exists for (type, subtype) (proto_heating_profiles.h): the
/// electrical heater with an adjustable setpoint (52/1, private registers) and the plain electrical
/// heater (52/0, heating levels). This only says a climate entity may bind; whether the private
/// register protocol may be sent to the device is device_uses_heating_registers().
/// @param type Device type.
/// @param subtype Device subtype.
/// @return true for devices that have a heating profile.
bool device_supports_climate_control(DeviceType type, uint8_t subtype);

/// @brief Decode the 10-bit node type from two packed metadata bytes.
/// @param type_msb First metadata byte.
/// @param type_subtype Second metadata byte containing the remaining type bits and subtype.
/// @return Decoded node type; a value the enum has no enumerator for is returned as is.
DeviceType decode_packed_device_type(uint8_t type_msb, uint8_t type_subtype);

/// @brief Decode a protocol-packed device subtype from the second metadata byte.
/// @param type_subtype Second metadata byte containing subtype in bits [5:0].
/// @return Manufacturer-specific subtype.
uint8_t decode_packed_device_subtype(uint8_t type_subtype);

/// @brief Encode a DeviceType/subtype pair into the two-byte packed metadata format used by
/// discovery responses — the inverse of decode_packed_device_type()/decode_packed_device_subtype().
/// @param type Device type to encode.
/// @param subtype Manufacturer-specific subtype; only bits [5:0] are used.
/// @param type_msb Output: first metadata byte. Only the low ten bits of `type` fit the wire field;
///        higher bits are dropped.
/// @param type_subtype Output: second metadata byte (top 2 type bits + 6-bit subtype).
void encode_packed_device_type(DeviceType type, uint8_t subtype, uint8_t &type_msb, uint8_t &type_subtype);

/// @brief Human‑readable operation profile name for a device kind.
/// Used for logging and diagnostics.
/// @param type Device type.
/// @param subtype Device subtype; a cover is "cover_position_tilt" only when its (type, subtype) has a slat
///        angle, and "cover_discrete" when it has no intermediate positions.
/// @return String such as "cover_position", "cover_position_tilt", "cover_discrete", "binary_on_off", "lock", etc.
const char *device_operation_profile_name(DeviceType type, uint8_t subtype);

/// @brief Human-readable device type string for diagnostics, including the raw numeric value.
/// @param type Device type.
/// @return String such as "horizontal_awning (0x10)", or the raw hex form ("0x1A") when the
///         type has no symbolic name. The value is printed in full ("0x3FF").
std::string format_device_type_diagnostic(DeviceType type);

/// @brief A device type as raw hexadecimal.
/// @param type Device type.
/// @return The value in at least two digits, e.g. "0x1A" or "0x3FF".
std::string format_device_type_hex(DeviceType type);

/// @brief The speed a cover's position moves ask the motor for (ADR 0065).
///
/// The 8-byte execute frame carries a speed profile byte; the 6-byte frame a wall pad sends carries
/// none and leaves the speed to the motor. Like `inverted`, a YAML-declared preference: nothing on
/// the wire reports which one a device is moved with. The enumerator for "the motor's own speed" is
/// not a bare `DEFAULT`, which the Arduino core defines as a macro on some targets.
enum class TravelSpeed : uint8_t {
  MOTOR_DEFAULT = 0,  ///< The 6-byte frame with no profile: the motor's own speed, as its wall pad moves it.
  SILENT = 1,         ///< Profile 0x05, the manufacturer app's silent operation: slower than the motor's own.
  FAST = 2,           ///< Profile 0x06: the 8-byte frame with the fast speed block, sent only when chosen.
};

/// @brief Get the name a travel speed is configured and published under.
/// @param speed The speed to name.
/// @return "default", "silent" or "fast".
const char *travel_speed_name(TravelSpeed speed);

/// @brief Look up a travel speed by the name travel_speed_name() gives it.
/// @param name "default", "silent" or "fast".
/// @param speed Set to the matching speed on success, untouched otherwise.
/// @return true when the name is one of the three.
bool parse_travel_speed(const std::string &name, TravelSpeed &speed);

/// @brief What a device says it is doing, from bits [2:0] of byte 0 of a status reply (CMD_PRIVATE_RESP,
/// CMD_STATUS_UPDATE).
///
/// The numbering is the public KLF 200 API's node state (v3.18, Table 105 "State"). The byte packs the
/// state under a result code, `(code << 3) | state`, the same layout as a CMD_ERROR_RESP result byte
/// (error_resp_state()); every ordinary status reply in the corpus carries 4 while a cover travels and 5
/// once it is at rest.
/// WAITING_FOR_POWER is confirmed in ordinary status replies and in command acknowledgements of a VELUX
/// SML shutter behind a KUX 110 (issue #230); WAITING_FOR_EXECUTING rests on the API.
enum class ExecutionState : uint8_t {
  NON_EXECUTING = 0,          ///< Unable to execute under the given conditions (example: temperature too high).
  EXECUTION_ERROR = 1,        ///< An error occurred during the previous execution; cleared when the next starts.
  WAITING_FOR_EXECUTING = 2,  ///< Accepted, waiting for its turn to run.
  WAITING_FOR_POWER = 3,      ///< Accepted, waiting for power (queued behind another device on a shared supply).
  EXECUTING = 4,              ///< Running.
  DONE = 5,                   ///< Not executing, no error, ready.
  UNDEFINED = 0xFF,           ///< Bits [2:0] hold 6 or 7, which no source defines.
};

/// @brief Read the execution state out of byte 0 of a status reply.
/// @param status_byte Byte 0 of the reply's payload.
/// @return The state, or ExecutionState::UNDEFINED when bits [2:0] hold 6 or 7.
ExecutionState execution_state_of(uint8_t status_byte);

/// @brief Read the result code out of byte 0 of a reply to an EXECUTE.
///
/// The same byte as execution_state_of() reads, bits [7:3]: 0 for a command taken without complaint,
/// otherwise a code named by result_code_name() (5 says a parameter of the request is unknown).
/// @param status_byte Byte 0 of the reply's payload.
/// @return Code 0..31.
uint8_t result_code_of(uint8_t status_byte);

/// @brief Get a human-readable name for an ExecutionState.
/// @param state The state to name.
/// @return Null-terminated lowercase string such as "executing", "waiting_for_power" or "undefined".
const char *execution_state_name(ExecutionState state);

/// @brief A device's coarse battery level, as it reports it in byte 1 of its status payload.
///
/// Three levels, not a percentage. Only FULL_CHARGE has been seen on real hardware so far (every
/// solar Somfy capture in the corpus, in daylight); LOW_CHARGE and NORMAL_CHARGE are the two values
/// below it and have not been observed. Mains devices report NOT_REPORTED.
/// The names avoid a bare `LOW`, which the Arduino core defines as a macro.
enum class BatteryState : uint8_t {
  NOT_REPORTED = 0,   ///< The device does not report a battery (mains powered), or no status was read yet.
  LOW_CHARGE = 1,     ///< Battery low.
  NORMAL_CHARGE = 2,  ///< Battery neither low nor full.
  FULL_CHARGE = 3,    ///< Battery full.
};

/// @brief Get the name a battery level is published under.
/// @param state The level to name.
/// @return "low", "normal" or "full", or "not_reported" for NOT_REPORTED.
const char *battery_state_name(BatteryState state);

/// @brief Decode the coarse battery level from byte 1 of a status payload (CMD_PRIVATE_RESP to a
/// status poll, or CMD_STATUS_UPDATE).
/// @param status_byte data[1] of the payload.
/// @return The level held in bits 5-6 (STATUS_BATTERY_MASK); the other bits are ignored.
BatteryState decode_battery_state(uint8_t status_byte);

// ============================================================================
// Position Report Decoding
// ============================================================================

/// Target-reached tolerance expressed in raw IO-homecontrol position units.
/// 100 raw units out of 51200 full-scale is about 0.195%, so this only absorbs
/// tiny target/current mismatches from device rounding or early stopped flags.
static constexpr uint16_t STATUS_POS_TOLERANCE_RAW = 100;

/// Packed device metadata uses two bytes where the high 8 bits carry the upper type bits and the
/// low byte carries both the remaining type bits and the 6-bit manufacturer subtype.
static constexpr uint8_t DEVICE_METADATA_SIZE = 2;
static constexpr uint8_t DEVICE_TYPE_LOW_BITS_SHIFT = 2;
static constexpr uint8_t DEVICE_TYPE_HIGH_BITS_SHIFT = 6;
static constexpr uint16_t DEVICE_TYPE_WIRE_MASK = 0x3FF;  ///< The node-type wire field is ten bits wide.
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
  /// True while a pending STOP replaced a `Motion::MOVING` prediction that a failed STOP should
  /// bring back (DeviceRegistry::rollback_optimistic()). A failed STOP means the device keeps
  /// travelling, and a device that reports nothing mid-travel has no observation to fall back on
  /// that still carries the direction. Only set alongside `Motion::STOPPED`.
  bool stop_replaced_movement{false};
  float stop_replaced_target{UNKNOWN_POSITION};  ///< The replaced `target`; meaningful only with the flag.

  /// @return true when nothing is predicted, so a rollback would be a no-op.
  [[nodiscard]] bool empty() const {
    return target == UNKNOWN_POSITION && tilt == UNKNOWN_POSITION && motion == Motion::NONE && !stop_replaced_movement;
  }
  /// Withdraw every prediction.
  void clear() { *this = {}; }
  /// Withdraw the position prediction; call when a decoded position observation supersedes it.
  /// Clears `motion` with `target` because apply_optimistic_target() sets the two together, and
  /// the replaced movement with them: an observation outranks every guess about travel.
  void clear_position() {
    target = UNKNOWN_POSITION;
    motion = Motion::NONE;
    forget_replaced_movement();
  }
  /// Drop the movement a STOP replaced, once nothing can roll the STOP back any more.
  void forget_replaced_movement() {
    stop_replaced_movement = false;
    stop_replaced_target = UNKNOWN_POSITION;
  }
  /// Withdraw the tilt prediction; call when a decoded tilt observation supersedes it.
  void clear_tilt() { tilt = UNKNOWN_POSITION; }
};

/// @brief The last "stopped" report a device sent away from its own target, kept to confirm the next.
///
/// A cover can come to rest a little off its target and say so (a VELUX SSL 119-171 raw short of
/// an end stop, a Somfy pergola 378-479 raw off an intermediate target; corpus
/// velux_ssl_statuspoll_stopped_off_target and somfy_pergola_louver_statuspoll_replies_sx1262).
/// A second report naming the same target and the same current position confirms the device is
/// at rest there (decisions::classify_stopped_report()). Observed values only, never a prediction.
struct StopCandidate {
  float target{UNKNOWN_POSITION};    ///< Target the device reported with the off-target "stopped".
  float position{UNKNOWN_POSITION};  ///< Current position it reported with it.

  /// @brief True when a report is remembered.
  [[nodiscard]] bool armed() const { return target != UNKNOWN_POSITION && position != UNKNOWN_POSITION; }
  /// @brief Forget the remembered report.
  void clear() { *this = {}; }
};

/// @brief What a register heater with a programme has shown about its temperature in force in this boot.
/// @details The hub reads that register while the programme runs (HeatingProgrammePeriod::in_force_fn). A heater
///          that has answered it with a value has the programme's registers, so from then on a target set while
///          the programme runs is a temporary override, even while a missed read leaves the value unknown.
///          A target set while the programme runs and the state is UNKNOWN triggers that read first
///          (HeaterController::read_programme_state()); only poll_temperature_in_force_() changes it.
enum class TemperatureInForceSupport : uint8_t {
  UNKNOWN,   ///< Not answered yet in this boot (no read, or no reply): a target while the programme runs first
             ///< reads the register and writes nothing if the heater stays silent.
  REPORTED,  ///< Answered with a value at least once: a target while the programme runs is an override. Kept
             ///< until the next boot, also when a later answer carries no value.
  ABSENT,    ///< The first answer came without a value: the heater has no such register. The hub stops reading
             ///< it until the next boot, and a target writes the comfort setpoint.
};

/// @brief The values of a device's readings, and which of them the device has, was asked for or does not have.
/// @details One store per device for every source of a reading (a private register, a status reply, a data object,
///          the answer to an event subscription or a limitation read),
///          indexed by a slot, the number of the reading in the reading table (DeviceReading,
///          proto_device_readings.h; a static_assert there keeps every reading within SLOTS). Runtime only, until the
///          next boot: nothing is persisted. A slot holds a value or not, independently of two flags:
///           - asked: an entity of the YAML wants the reading, so the hub's poll reads it;
///           - absent: the device does not have the reading (it answered the read without a value, or the hub cannot
///             ask for it), so the hub reads it no more.
///          A value that is stored is no longer absent; a reading found absent holds no value. Every reading read
///          through reading_value() is held here, whichever source filled it.
struct DeviceReadingValues {
  static constexpr size_t SLOTS = 48;  ///< Number of readings the store can hold.

  /// @brief Keep a value; the slot is no longer absent. A NaN is a value too (a sensor the device reports as faulty).
  void store(size_t slot, float value) {
    if (slot >= SLOTS)
      return;
    this->values_[slot] = value;
    this->held_ |= mask_of(slot);
    this->absent_ &= ~mask_of(slot);
  }
  /// @brief Forget the value of a reading: it reads as never read again. Absent and asked stay as they are.
  void clear(size_t slot) {
    if (slot < SLOTS)
      this->held_ &= ~mask_of(slot);
  }
  /// @brief Mark the device as not having this reading; a value held before is dropped.
  void mark_absent(size_t slot) {
    if (slot >= SLOTS)
      return;
    this->absent_ |= mask_of(slot);
    this->held_ &= ~mask_of(slot);
  }
  /// @brief Declare that an entity wants this reading read. A reading already found absent stays absent.
  void ask(size_t slot) {
    if (slot < SLOTS)
      this->asked_ |= mask_of(slot);
  }
  /// @return The value, or nullopt when never read, cleared, absent or out of range.
  [[nodiscard]] std::optional<float> value(size_t slot) const {
    return slot < SLOTS && (this->held_ & mask_of(slot)) != 0 ? std::optional<float>{this->values_[slot]}
                                                              : std::nullopt;
  }
  /// @return true once the device was found not to have this reading.
  [[nodiscard]] bool absent(size_t slot) const { return slot < SLOTS && (this->absent_ & mask_of(slot)) != 0; }
  /// @return true while an entity wants this reading read.
  [[nodiscard]] bool asked(size_t slot) const { return slot < SLOTS && (this->asked_ & mask_of(slot)) != 0; }

 private:
  using Mask = uint64_t;
  static_assert(SLOTS <= std::numeric_limits<Mask>::digits, "every slot needs a bit in the masks; widen Mask");
  static constexpr Mask mask_of(size_t slot) { return Mask{1} << slot; }
  float values_[SLOTS]{};
  Mask held_{0};    ///< Bit per slot: a value is stored.
  Mask absent_{0};  ///< Bit per slot: the device does not have the reading.
  Mask asked_{0};   ///< Bit per slot: an entity wants the reading read.
};

/// @brief Runtime state of a paired IO‑Homecontrol device.
struct IoDevice {
  uint8_t node_id[NODE_ID_SIZE]{};       ///< Device's 3‑byte radio address.
  DeviceType type{DeviceType::UNKNOWN};  ///< Device type (shutter, awning, etc.).
  uint8_t subtype{0};                    ///< Device subtype (manufacturer‑specific).
  DeviceModel model{DeviceModel::NONE};  ///< YAML-declared product model below the subtype (`io_model:`); never
                                         ///< on the wire. Resolver input, like `dimmable`.
  char name[DEVICE_NAME_BUFFER_SIZE]{};  ///< Cached UTF-8 device name decoded from Latin-1 wire payloads.
  float position{UNKNOWN_POSITION};      ///< Current position: 0=open, 100=closed, or UNKNOWN_POSITION.
  float tilt{UNKNOWN_POSITION};          ///< Current tilt: 0=closed, 100=open, or UNKNOWN_POSITION.
  float target{UNKNOWN_POSITION};        ///< Target position the device is moving toward.
  bool is_stopped{true};                 ///< True if device is not moving.
  ExecutionState execution_state{ExecutionState::UNDEFINED};  ///< Execution state of the last status report that
                                                              ///< carried one; UNDEFINED until then, and for a
                                                              ///< device that has no motion.
  StopCandidate stop_candidate{};      ///< Last off-target "stopped" report, to confirm the next one.
  bool reports_remaining_time{false};  ///< The device has sent a nonzero remaining time in a status reply, so
                                       ///< its zero means "about to stop" rather than "not provided".
  DeviceFeatures features;             ///< What this device can do, resolved by resolve_device_features(). Every
                                       ///< capability check on a registered device reads this set.
  DeviceCapabilityClass entity_class{
      DeviceCapabilityClass::UNKNOWN};  ///< Class of the entity that declared the
                                        ///< device; UNKNOWN for a device no entity declared (the type's class then
                                        ///< applies). Resolver input.
  std::optional<bool> invert_override = std::nullopt;  ///< YAML `invert_position:` when declared; wins over the kind's
                                                       ///< default. Resolver input.
  bool inverted{false};  ///< True if open/close positions are swapped (e.g., horizontal awning).
                         ///< Resolved: `invert_override`, else device_feature::INVERTED.
  TravelSpeed travel_speed{TravelSpeed::MOTOR_DEFAULT};  ///< Speed profile of position moves. Like `inverted`, a
                                                         ///< YAML-declared preference: nothing on the wire reports it.
  bool optimistic_state{true};     ///< True if `target` may be set ahead of a confirming poll/response.
  bool low_power{false};           ///< YAML-declared: this target is a low-power / duty-cycled receiver, so
                                   ///< directed frames set CTRL1_LOW_POWER and use the long wake-up preamble.
                                   ///< A protocol-reported class exists (discovery Multi Information Byte) but
                                   ///< is surfaced through the pairing/scan snippet, never applied at runtime.
  bool dimmable{false};            ///< YAML-declared `dimmable: true` of a LIGHT entity. Resolver input: it
                                   ///< alone decides a light's device_feature::LEVELS (unless the kind is main
                                   ///< parameter only; light subtype 58 is on/off only, VELUX KLF 200 API v3.18,
                                   ///< App. 2).
  uint8_t last_result_code{0};     ///< Last CMD_ERROR_RESP result byte (0 = none recorded). See
                                   ///< command_result_name()/is_limitation_result() in proto_constants.h. Cleared by
                                   ///< the next successful status/command reply for this device. Note: 0 is also the
                                   ///< real RESULT_UNKNOWN_STATUS_REPLY wire value, so that specific explicit reply is
                                   ///< indistinguishable here from "nothing recorded yet" — a known, accepted tradeoff.
  uint32_t last_result_at_ms{0};   ///< millis() timestamp of last_result_code, 0 when none recorded.
  bool speed_block_warned{false};  ///< The hub has logged, in this boot, that this device answered the speed
                                   ///< block with "parameter unknown". Run-time only, never persisted.
  uint8_t last_commander[NODE_ID_SIZE]{};     ///< Node ID of the controller that last commanded this device, as
                                              ///< reported verbatim by the device in its own status payload. All
                                              ///< zeroes until a status reply carrying the record has been decoded
                                              ///< (see decode_last_command_record() in proto_codecs.h).
  uint8_t last_command_originator{0};         ///< That command's Command Originator byte (ORIGINATOR_* in
                                              ///< proto_constants.h). Only meaningful when `has_last_command`.
  bool has_last_command{false};               ///< True once a status reply carried a well-formed last-command record.
  bool limited_by_rain{false};                ///< Derived from observed status replies, never from a prediction
                                              ///< (decisions::is_rain_limited(); ADR 0030 keeps predictions apart):
                                              ///< the last move was ordered by the rain sensor, or a position
                                              ///< command was clamped while rain protection was recently active.
                                              ///< Separate from `last_result_code`, which only an explicit
                                              ///< CMD_ERROR_RESP sets.
  uint32_t last_rain_evidence_ms{0};          ///< millis() of the last status reply that named the rain sensor as the
                                              ///< last commander; 0 = never. Memory for the clamp rule, aged out
                                              ///< by RAIN_EVIDENCE_HOLD_MS.
  uint32_t last_status{0};                    ///< millis() timestamp of last received status.
  int16_t last_rssi_dbm{RSSI_UNKNOWN_DBM};    ///< Most recent raw RSSI sample (dBm), or RSSI_UNKNOWN_DBM.
  int16_t rssi_ema_scaled{RSSI_UNKNOWN_DBM};  ///< Smoothed RSSI as fixed point in 1/RSSI_EMA_SCALE dBm — read
                                              ///< through device_rssi_ema_dbm(), updated by
                                              ///< detail::update_link_health() in entity_helpers.h. Fixed point
                                              ///< (rather than whole dBm) keeps sub-dBm EMA contributions from
                                              ///< vanishing to integer truncation, so the average converges on a
                                              ///< stable signal instead of stalling up to RSSI_EMA_SCALE−1 dBm
                                              ///< away. RSSI_UNKNOWN_DBM before the first sample.
  uint32_t last_seen_ms{0};  ///< millis() of the last frame received from this device (any command), 0 = never.
  uint32_t last_moving_evidence_ms{
      0};  ///< millis() of the last sign this device is travelling (see
           ///< note_moving_evidence(), clear_moving_evidence()), 0 = never. Hub-side belief, not observation.
  uint32_t moving_until_ms{0};         ///< millis() at which the remaining time of the last trusted moving report has
                                       ///< run out (plus PREDICTED_END_MARGIN_MS), 0 = unknown. Written by
                                       ///< note_moving_until(); dropped with the moving evidence.
  uint16_t exchange_timeout_count{0};  ///< Cumulative count of outbound exchanges to this device with no valid
                                       ///< response (see detail::record_exchange_timeout() in hub_internal.h).
  uint16_t exchange_attempt_count{0};  ///< Cumulative attempts (`ExchangeEngine::DebugInfo::tries`, 1-based per
                                       ///< exchange) across those timed-out exchanges only — attempts within an
                                       ///< ultimately successful exchange are not counted (deliberate scope limit).
  uint16_t exchange_unconfirmed_count{0};  ///< Cumulative count of exchanges this device authenticated and then never
                                           ///< closed: it answered with a 0x3C challenge, we answered that, and no
                                           ///< final reply arrived. Counted separately from exchange_timeout_count
                                           ///< because the two have different causes and different fixes — silence
                                           ///< from the start means the device never heard us, while silence after a
                                           ///< challenge means the reply was lost on the way back (or our challenge
                                           ///< answer never landed). For a CMD_EXECUTE this outcome is treated as
                                           ///< success and raises no failure count, so without this counter it is
                                           ///< invisible in the diagnostics.
  bool confirms_execute{false};  ///< True once this device has answered a CMD_EXECUTE with a reply that closed the
                                 ///< exchange (a status, or an error). Learned at runtime and never stored: it
                                 ///< separates a device whose missing reply is an anomaly from one that never sends
                                 ///< one (see decisions::retry_after_unconfirmed_accept_is_safe()).
  OptimisticState optimistic{};  ///< Hub-side predictions; see OptimisticState. Never observation.
  float temperature_range_min_c{
      HEATING_TEMPERATURE_RANGE_MIN_C};  ///< Lower end of the temperature encoding range of a heater that
                                         ///< carries a temperature in its main parameter (YAML `min_temperature:`).
  float temperature_range_max_c{
      HEATING_TEMPERATURE_RANGE_MAX_C};  ///< Upper end of that range (YAML `max_temperature:`).
  DeviceReadingValues reading_values{};  ///< What the hub has read from the device, by reading (reading_value()): a
                                         ///< heater's or a water heater's registers and status reply, a data object,
                                         ///< a cover's rain, sun level and battery. Kept across DeviceRegistry::put().
  bool battery_entities{false};  ///< YAML-declared (`battery: true`): the battery entities exist, so the status log
                                 ///< line names the level even when the device reports none.
  bool has_system_key{false};    ///< YAML-declared (`system_key:`): this device keeps a key of its own. Every
                                 ///< exchange with it uses `system_key` below instead of the hub's key.
  uint8_t system_key[AES_KEY_SIZE]{};  ///< The device's own key; meaningful only when `has_system_key`. Key material:
                                       ///< never logged outside a bannered report block.
  bool registered_by_pairing{false};   ///< Added by a pairing in this boot, not declared in YAML. The record is
                                       ///< lost at reboot, so reports still print its YAML block, and a later
                                       ///< pairing may replace it (DeviceRegistry::get_declared()).
  uint8_t model_check_attempts{0};     ///< Runs of the hub's one-off model check (a heater kind with model rows:
                                       ///< model name read, declared-model hint) so far; the check stops after
                                       ///< it completes or after a bounded number of runs. Runtime only.
  /// What a register heater with a programme has answered to the read of its temperature in force. Runtime
  /// only, until the next boot.
  TemperatureInForceSupport temperature_in_force{TemperatureInForceSupport::UNKNOWN};
  bool model_checked{false};  ///< The model check completed: both of its reads were answered.
};

/// @brief Resolve a device's capabilities and inversion from its kind and its YAML overrides.
/// @details The one place a device's features are decided. The base is the (type, subtype, model) row
///          (device_features(), with the declared IoDevice::model); a kind with no row, or an undeclared one, takes
///          undeclared_kind_features() of the declaring entity's class (the type's own class when no
///          entity declared the device), made main-parameter-only at subtype 58 like any other kind.
///          A light's LEVELS then come from the YAML `dimmable:` flag alone, and never on a kind that
///          takes the main parameter only. `inverted` is the YAML override, else the kind's default.
///          Called wherever a record gets or changes its kind: DeviceRegistry::add() and put(), and
///          when a status reply teaches an undeclared device its type.
/// @param dev Device to resolve; reads type, subtype, model, entity_class, dimmable and invert_override and
///        writes features and inverted.
void resolve_device_features(IoDevice &dev);

/// @brief Record that a device is (believed to be) travelling right now.
///
/// With clear_moving_evidence(), one of the two writers of IoDevice::last_moving_evidence_ms.
/// Called wherever the hub learns a receiver is moving *because of something that reached it*: a
/// decoded status that says "not stopped", a movement command the device accepted, or an overheard
/// linked-remote movement command. Deliberately NOT called for the hub's own optimistic prediction:
/// that is applied before the command is even sent, so treating it as proof of movement would make a
/// resting receiver look awake to the very command that starts it. A low-power receiver that is
/// moving ignores the long wake-up preamble, so this stamp steers the exchange engine to the short
/// one first (decisions::wake_belief()).
/// @param dev    Device to stamp.
/// @param now_ms Current millis(). Must be non-zero in practice; 0 would read as "never".
inline void note_moving_evidence(IoDevice &dev, uint32_t now_ms) {
  dev.last_moving_evidence_ms = now_ms;
  dev.moving_until_ms = 0;  // A fresh sign of travel says nothing about when it ends until a count says so.
}

/// @brief Record when the travel the device is reporting should be over, from its own remaining time.
///
/// Call after note_moving_evidence() for the same report: that call forgets an earlier end. While
/// the end has not been reached the receiver is still travelling by its own account, so a silent
/// exchange is not taken as proof that it has come to rest (decisions::moving_until_holds()).
/// @param dev      Device to stamp.
/// @param until_ms millis() at which the reported remaining time, plus a margin, has run out. Non-zero.
inline void note_moving_until(IoDevice &dev, uint32_t until_ms) { dev.moving_until_ms = until_ms; }

/// @brief Forget that a device was travelling: it reported itself stopped (including in the ack to a
/// STOP), a linked remote was heard stopping it, or it stayed silent to an exchange sent while it
/// was believed moving.
///
/// The counterpart of note_moving_evidence(). Without it a receiver that has demonstrably stopped
/// would keep reading as "moving" for the rest of LOW_POWER_MAX_TRAVEL_MS, although it goes back to
/// duty-cycling within seconds and then hears only the wake-up preamble.
/// @param dev Device to clear.
inline void clear_moving_evidence(IoDevice &dev) {
  dev.last_moving_evidence_ms = 0;
  dev.moving_until_ms = 0;
}

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

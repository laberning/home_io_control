#pragma once

/// @file proto_heating_profiles.h
/// @brief Heating profiles: what a climate card offers for each kind of heater, and how.
/// @ingroup hioc_protocol
///
/// A profile is one row per (node type, subtype, model) that a `climate:` entity can drive. It names the
/// control path (the private register protocol of proto_heating.h, or the standard EXECUTE with a
/// heating level as the main parameter), the HVAC modes and presets the card shows, how each maps to
/// a wire value in both directions, which registers a poll reads and which companion entities exist.
/// The platform asks the profile and never branches on a device type.
///
/// The enums here are protocol-side: they carry no ESPHome type. The platform maps HeatingHvacMode
/// and HeatingPresetId onto ESPHome's climate modes and presets.
///
/// \todo Heating-level control of the plain electrical heater (node type 52, subtype 0) is confirmed for
///       Away, Eco, Comfort, Comfort -1, Comfort -2 and Off on one Thermor pilot-wire interface (issue #228),
///       which ignores Boost and reports no level in its status. The Boost level and the level read-back are
///       unconfirmed on hardware; confirm them with a heater that takes a boost or reports its level.
/// \todo Every 52/1 seen is an Atlantic heater. A non-Atlantic 52/1 would need the heating-level
///       path instead; the profile keys on (type, subtype, model), and the model is the YAML's.

#include "proto_constants.h"
#include "proto_device_kinds.h"
#include "proto_device_model.h"
#include "proto_heating.h"
#include "proto_sizes.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace esphome {
namespace home_io_control {

/// @brief How a heater is commanded and read.
enum class HeatingControlPath : uint8_t {
  PRIVATE_REGISTERS,    ///< CMD_WRITE_PRIVATE register reads and writes (proto_heating.h).
  HEATING_LEVEL,        ///< Standard EXECUTE with a heating level as the main parameter; read with 0x03.
  TEMPERATURE_EXECUTE,  ///< Standard EXECUTE with a temperature or an alias as the main parameter; read with 0x03.
};

/// @brief HVAC modes a profile can offer.
enum class HeatingHvacMode : uint8_t {
  OFF,   ///< Heater off.
  HEAT,  ///< Heating on.
  AUTO,  ///< The device decides.
};

/// @brief Presets a profile can offer. CUSTOM entries carry their own name.
enum class HeatingPresetId : uint8_t {
  NONE,     ///< No preset: selecting it ends the one that runs (a towel dryer's boost or drying).
  AWAY,     ///< Nobody is in the room (the frost-protection level on a level heater).
  COMFORT,  ///< Comfort level.
  ECO,      ///< Economy level.
  BOOST,    ///< Boost level.
  CUSTOM,   ///< Named by HeatingPresetEntry::custom_name.
};

/// @brief Main-parameter prefix byte of a heating level: the level occupies the low byte.
static constexpr uint8_t HEATING_LEVEL_MP_PREFIX = 0xFC;

/// @brief Heating levels (low byte of the main parameter, prefix HEATING_LEVEL_MP_PREFIX).
namespace heating_level {
static constexpr uint8_t OFF = 0x00;               ///< Off.
static constexpr uint8_t FROST_PROTECTION = 0x01;  ///< Frost protection.
static constexpr uint8_t ECO = 0x02;               ///< Economy.
static constexpr uint8_t COMFORT_MINUS_2 = 0x03;   ///< Comfort, two degrees lower.
static constexpr uint8_t COMFORT_MINUS_1 = 0x04;   ///< Comfort, one degree lower.
static constexpr uint8_t COMFORT = 0x05;           ///< Comfort.
static constexpr uint8_t BOOST = 0x07;             ///< Boost.
static constexpr uint8_t SECURED = 0x3F;           ///< Secured; reported by the device, never commanded.
}  // namespace heating_level

/// @brief Main-parameter prefix byte of a setpoint alias: the alias occupies the low byte.
static constexpr uint8_t HEATING_ALIAS_MP_PREFIX = 0xD8;

/// @brief Setpoint aliases (low byte of the main parameter, prefix HEATING_ALIAS_MP_PREFIX).
namespace heating_alias {
static constexpr uint8_t COMFORT = 0x0F;            ///< The device's comfort setpoint.
static constexpr uint8_t THERMOSTAT_ECO = 0x0E;     ///< Heating temperature interface: the economy setpoint.
static constexpr uint8_t SECURED = 0x0A;            ///< Heating temperature interface: the secured setpoint.
static constexpr uint8_t HEAT_PUMP_SETBACK = 0x0E;  ///< Heat pump: the setback setpoint.
static constexpr uint8_t HEAT_PUMP_ECO = 0x12;      ///< Heat pump: the economy setpoint.
static constexpr uint8_t HEAT_PUMP_HALTED = 0x13;   ///< Heat pump: halted.
}  // namespace heating_alias

/// @brief Main parameter of a heat pump's off command: MAIN_PARAMETER_KEEP, which the command pairs with the
/// off value of a functional parameter.
static constexpr uint16_t HEATING_OFF_MP = MAIN_PARAMETER_KEEP;

/// @brief Sentinel for HeatingModeEntry::main_parameter: send the entity's target temperature, encoded
/// over the device's range, as the main parameter.
static constexpr uint16_t HEATING_MP_FROM_TARGET = 0xFFFF;

/// @brief Values of functional parameter 9 on the heating temperature interface: the active mode.
/// @details The heating / cooling bits (`4`, `8`) belong to a separate operating-mode command (main
///          parameter `0xD400` with FP9 `4`, `8` or `0xC`); they are never combined into these values and
///          this component sends no operating-mode command.
namespace heating_mode_flags {
static constexpr uint8_t OFF = 0x00;     ///< Off.
static constexpr uint8_t AUTO = 0x01;    ///< The device follows its own schedule.
static constexpr uint8_t MANUAL = 0x02;  ///< The device follows the commanded setpoint.
}  // namespace heating_mode_flags

/// @brief Values of functional parameter 16 on the heat pump: the on/off state.
/// @details FP16 is the last functional parameter (the last bit of the second bitmap). FP15 reports the
///          device's capabilities (`1` heating, `2` cooling, `4` away, `8` pool, `0x10` hot water) and
///          FP16's other bits report its state; both are read-only here: no command selects cooling,
///          and this component does not read FP15.
namespace heating_fp16 {
static constexpr uint16_t ON = 0x4000;   ///< On: sent with a setpoint alias to select it.
static constexpr uint16_t OFF = 0x8000;  ///< Off: sent with HEATING_OFF_MP.
}  // namespace heating_fp16

/// @brief Which functional parameter, if any, accompanies a TEMPERATURE_EXECUTE command.
enum class HeatingFpKind : uint8_t {
  NONE,  ///< A main-parameter-only command.
  FP9,   ///< Functional parameter 9, one byte.
  FP16,  ///< Functional parameter 16, two bytes.
};

/// @brief A functional parameter and its value.
struct HeatingFp {
  HeatingFpKind kind{HeatingFpKind::NONE};  ///< Which parameter; NONE sends none.
  uint16_t value{0};                        ///< Its value (one byte for FP9, two for FP16).
};

/// @brief One HVAC mode the card offers and the wire value that selects it.
struct HeatingModeEntry {
  HeatingHvacMode hvac;        ///< Mode shown on the card.
  uint8_t wire;                ///< PRIVATE_REGISTERS: mode register value. HEATING_LEVEL: level low byte.
                               ///< TEMPERATURE_EXECUTE: unused.
  uint16_t main_parameter{0};  ///< TEMPERATURE_EXECUTE: the main parameter the mode sends, or
                               ///< HEATING_MP_FROM_TARGET.
  HeatingFp fp{};              ///< TEMPERATURE_EXECUTE: the functional parameter the mode sends.
};

/// @brief One preset the card offers.
/// @details Every preset sends a command when selected: there is no way to list one the device only reports.
///          What a heater applies right now is not a preset but the level in HeatingReadings::level.
struct HeatingPresetEntry {
  const char *custom_name;   ///< Name of a CUSTOM preset; nullptr otherwise.
  HeatingPresetId id;        ///< Standard preset, or CUSTOM.
  HeatingFunction write_fn;  ///< PRIVATE_REGISTERS: register the preset writes. Unused for levels.
  uint8_t wire;              ///< Value written: register value, or the low byte of a level or alias.
  HeatingFp fp{};            ///< TEMPERATURE_EXECUTE: the functional parameter sent with the alias.
};

/// @brief What a device-reported wire value shows on the card.
struct HeatingObservedState {
  uint8_t wire;          ///< Mode register value, or level low byte, as the device reports it.
  HeatingHvacMode hvac;  ///< HVAC mode shown.
  int8_t preset;         ///< Index into the profile's presets shown with it, or -1 for none.
};

/// @brief Where a register heater's "heating" / "idle" comes from.
enum class HeatingActionSource : uint8_t {
  REGULATOR,     ///< The regulator register (HeatingFunction::REGULATOR_STATE): heating at state 2.
  WORKING_RATE,  ///< The working rate (HeatingFunction::WORKING_RATE): heating above 0 %. A towel dryer's
                 ///< regulator reads 0 during boost and drying, while the element runs at 100 %.
};

/// @brief Companion entities and traits a profile has.
struct HeatingProfileFeatures {
  bool target_temperature;   ///< The card has a setpoint.
  bool current_temperature;  ///< The card shows a measured temperature.
  bool action;               ///< The card shows heating / idle.
  bool mode_observed;        ///< A device reading decides the card's mode; false when the mode is the
                             ///< last command (nothing reads it back).
};

/// @brief How a heater's own weekly programme shows that it runs, and the temperature it heats to.
/// @details The programme runs while the heater's mode shows as `auto`: the mode register reads a value the profile's
///          observed table maps to HVAC auto (PROG on a towel dryer, PROG and AUTO on a radiator) and, where
///          the profile keeps timed runs in a register of its own, that register reads `idle_preset_state`
///          (no boost or drying on top). Then `in_force_fn` holds the temperature the heater heats to; outside
///          the programme it is not a temperature (it reads 1 in boost and 2 in drying on a towel dryer).
///          A target set while the programme runs is a temporary override: it goes to `derogation_fn`, and
///          the comfort setpoint in `comfort_fn` and the programme stay as they are.
struct HeatingProgrammePeriod {
  HeatingFunction in_force_fn;    ///< Register of the temperature in force.
  HeatingFunction comfort_fn;     ///< Register of the comfort setpoint.
  HeatingFunction derogation_fn;  ///< Register of the temporary override of the programme: a target set while
                                  ///< the programme runs is written here and holds until the programme's next
                                  ///< change; 0 ends it.
  /// The heater answers the status read with its heating level, which every poll then makes after the
  /// registers (HeatingReadings::level, the `heating_level:` sensor). False for a heater kind no unit of
  /// which has answered that read yet (the radiator family): its poll sends no status read, and its
  /// `heating_level:` sensor stays without a state.
  bool reports_level{false};
  /// The heater takes the override only in an eco period; in a comfort period it acknowledges the write and
  /// ignores it, as its own panel changes the comfort temperature there (ADR 0094). A target while the
  /// programme runs is then planned from the period the heater reports (HeatingDeviceFacts::period): the
  /// override in an eco period, the comfort setpoint in a comfort period. Needs `reports_level`, which is
  /// where the period comes from (checked at build time).
  bool override_in_eco_only{false};
  /// Value of HeatingProfile::preset_fn with no timed run on top; read only by a profile that has
  /// `preset_observed`.
  uint8_t idle_preset_state{0};
};

/// @brief One heater kind.
/// @details A row is written with designated initializers, in declaration order. `type` through `features`
///          and `modes` have no default, so a row that leaves one out does not compile; every other field has
///          one and is left out where unused. Each list is a span over a `constexpr` array, which carries its
///          own length.
struct HeatingProfile {
  // --- Required: every row states these ---
  DeviceType type;                  ///< Node type.
  uint8_t subtype;                  ///< Subtype (0 = the type's own row).
  DeviceModel model;                ///< Model (NONE = the (type, subtype) row's own profile).
  HeatingControlPath path;          ///< How the heater is commanded and read.
  HeatingProfileFeatures features;  ///< What the card and its companions offer.
  // The byte-sized optional fields sit here, in the padding before the first span, so the row packs.
  uint8_t mp_prefix{0};             ///< High byte of a level or alias main parameter (0 for registers).
  bool fixed_range{false};          ///< TEMPERATURE_EXECUTE: the temperature range is fixed, so the YAML
                                    ///< `min_temperature:` / `max_temperature:` do not apply.
  bool experimental_writes{false};  ///< PRIVATE_REGISTERS: the mode and preset writes are unconfirmed on a device.
  std::span<const HeatingModeEntry> modes;  ///< HVAC modes offered, with the value each writes.

  // --- Optional: a default stands for "unused". ---
  HeatingFp setpoint_fp{};   ///< TEMPERATURE_EXECUTE: the functional parameter sent with a setpoint.
  float visual_min_c{0.0F};  ///< Lowest target temperature the card offers (0 without a setpoint).
  float visual_max_c{0.0F};  ///< Highest target temperature the card offers (0 without a setpoint).
  /// PRIVATE_REGISTERS: how the heater's own programme shows that it runs, the temperature it heats to and
  /// the register a target set meanwhile is written to; nullptr for a heater without one.
  const HeatingProgrammePeriod *programme_period{nullptr};
  // NOLINTBEGIN(readability-redundant-member-init): a span's own default is empty, but g++'s
  // -Wmissing-field-initializers still wants the member initializer before a row may leave it out.
  std::span<const HeatingPresetEntry> presets{};     ///< Presets offered.
  std::span<const HeatingObservedState> observed{};  ///< Device-reported value -> card state.
  /// PRIVATE_REGISTERS: registers every poll reads, in order.
  std::span<const HeatingFunction> poll_registers{};
  /// PRIVATE_REGISTERS: registers read once until they have been read.
  std::span<const HeatingFunction> boot_registers{};
  /// Device-reported value of `preset_fn` -> preset; empty when the mode register names the preset too
  /// (`observed`). With it, `observed` sets only the mode and this table alone sets the preset, since
  /// mode and preset live in separate registers.
  std::span<const HeatingObservedState> preset_observed{};
  /// PRIVATE_REGISTERS: registers every poll reads after `poll_registers` whose miss is not a poll failure:
  /// a setting the heater's own controller changes (open-window detection), or a part the heater may not
  /// have (a night light), so a model declared on a heater without the part costs a read, not the poll.
  /// A missed read keeps the earlier value; the next poll asks again.
  std::span<const HeatingFunction> extra_poll_registers{};
  // NOLINTEND(readability-redundant-member-init)
  /// PRIVATE_REGISTERS: the register the HVAC modes write and the card's mode is read from.
  HeatingFunction mode_fn{HeatingFunction::SET_MODE};
  /// PRIVATE_REGISTERS with `preset_observed`: the register the card's preset is read from. The presets'
  /// own `write_fn` say what each writes.
  HeatingFunction preset_fn{HeatingFunction::SET_MODE};
  HeatingActionSource action_source{HeatingActionSource::REGULATOR};  ///< Where `features.action` reads from.
};

/// @brief Look up the heating profile for a (type, subtype, model).
/// @param type Node type.
/// @param subtype Subtype; one with no profile of its own falls back to the type's profile (subtype 0),
///        the same rule as find_device_type_info().
/// @param model Declared model. A model with a profile of its own gets it; any other falls back to the
///        (type, subtype) profile. No default: a caller that forgot the model would silently drive a
///        towel dryer as a radiator.
/// @return The profile, or nullptr when no climate entity drives this device.
const HeatingProfile *find_heating_profile(DeviceType type, uint8_t subtype, DeviceModel model);

/// @brief Every heating profile, in table order, for code that checks the whole table.
/// @return The whole table. A lookup for one device goes through find_heating_profile().
std::span<const HeatingProfile> heating_profiles();

/// @brief Look up the heating profile of a device record: its type, subtype and declared model.
/// @param dev The device.
/// @return The profile, or nullptr when no climate entity drives this device.
const HeatingProfile *find_heating_profile(const IoDevice &dev);

/// @brief Does a climate entity drive this device over the private register protocol?
/// @details The gate for every CMD_WRITE_PRIVATE frame the hub sends: a heater that takes the standard
///          EXECUTE must never be sent a register write. The control path is a fact of the
///          (type, subtype): a model's profile keeps the path of the profile above it (checked by
///          heating_profiles_test.cpp), so this and the other path predicates take no model.
/// @param type Node type.
/// @param subtype Subtype.
/// @return true when the profile's control path is HeatingControlPath::PRIVATE_REGISTERS.
bool device_uses_heating_registers(DeviceType type, uint8_t subtype);

/// @brief May the diagnostic register-read probes (`register_00`, `register_01`) be sent to this device?
/// @details A read of a register is the request climate support already sends to register-protocol
///          heaters, so those are allowed, and so is a water heater (device_feature::WATER_HEATER), whose
///          readings are family-0x00 registers. Every other kind -- covers, lights, switches, locks, the
///          standard-EXECUTE heaters -- is refused. This gates the probe and the register reads only; a
///          heater's register writes stay behind device_uses_heating_registers(), a water heater's
///          behind its WATER_HEATER bit.
/// @param type Node type.
/// @param subtype Subtype.
/// @return true for a register-protocol heater or a water heater.
bool device_accepts_register_probe(DeviceType type, uint8_t subtype);

/// @brief Does a climate entity drive this device with heating levels?
/// @param type Node type.
/// @param subtype Subtype.
/// @return true when the profile's control path is HeatingControlPath::HEATING_LEVEL.
bool device_uses_heating_levels(DeviceType type, uint8_t subtype);

/// @brief Does a climate entity drive this device with a temperature or an alias in the main parameter?
/// @param type Node type.
/// @param subtype Subtype.
/// @return true when the profile's control path is HeatingControlPath::TEMPERATURE_EXECUTE.
bool device_uses_heating_temperature(DeviceType type, uint8_t subtype);

/// @brief Main parameter of a heating level, as it travels in an EXECUTE and in a status reply.
/// @param level Level low byte (heating_level::*).
/// @return HEATING_LEVEL_MP_PREFIX in the high byte, `level` in the low byte.
constexpr uint16_t heating_level_main_parameter(uint8_t level) {
  return static_cast<uint16_t>((HEATING_LEVEL_MP_PREFIX << BITS_PER_BYTE) | level);
}

/// @brief Main parameter of a prefixed value (a level or an alias).
/// @param prefix High byte (HEATING_LEVEL_MP_PREFIX or HEATING_ALIAS_MP_PREFIX).
/// @param low Low byte.
/// @return The 16-bit main parameter.
constexpr uint16_t heating_prefixed_main_parameter(uint8_t prefix, uint8_t low) {
  return static_cast<uint16_t>((prefix << BITS_PER_BYTE) | low);
}

/// @brief Name of a heating level, as the `heating_level:` sensor shows it.
/// @param level Level low byte (heating_level::*).
/// @return `off`, `frost protection`, `eco`, `comfort -2`, `comfort -1`, `comfort`, `boost` or `secured`;
///         nullptr for a value that is not a level.
const char *heating_level_name(uint8_t level);

/// @brief Read a heating level out of a main parameter.
/// @param main_parameter The 16-bit value from a status reply.
/// @param[out] level Level low byte.
/// @return true when the value is a heating level (high byte HEATING_LEVEL_MP_PREFIX).
bool decode_heating_level(uint16_t main_parameter, uint8_t &level);

/// @brief Find what a device-reported wire value shows on the card.
/// @param profile The device's profile.
/// @param wire Mode register value or level low byte.
/// @return The matching entry, or nullptr when the value is not one the profile maps.
const HeatingObservedState *find_observed_state(const HeatingProfile &profile, uint8_t wire);

/// @brief Find what a device-reported value of the preset register (`preset_fn`) shows on the card.
/// @param profile The device's profile.
/// @param wire Value of the profile's preset register.
/// @return The matching entry, or nullptr when the profile has no preset table or does not map the value.
const HeatingObservedState *find_observed_preset(const HeatingProfile &profile, uint8_t wire);

/// @brief Does the heater's own programme run right now, with no timed run on top of it?
/// @details True when the profile has a programme period, its mode register reads a value the profile's
///          observed table shows as HVAC auto and, on a profile with a preset register of its own, that
///          register reads the idle value. An unread register is not the programme. A mode value the observed
///          table does not know is not the programme either.
/// @param profile The device's profile.
/// @param readings What the hub has read from the heater.
/// @return false for a profile without a programme period.
bool programme_period_active(const HeatingProfile &profile, const HeatingReadings &readings);

/// @brief The timed-run register a target must read besides the mode, if any.
/// @details A profile that keeps boost and drying in a register of its own (a non-empty preset table) runs the
///          programme only with none on top, so once the mode shows the programme that register decides
///          whether it runs. In any other mode the programme does not run, whatever that register holds.
/// @param profile The device's profile.
/// @param readings What the hub has read from the heater (the mode as it was just read).
/// @return The register, or std::nullopt when the mode does not show the programme or the profile has no
///         timed-run register.
std::optional<HeatingFunction> timed_run_register(const HeatingProfile &profile, const HeatingReadings &readings);

/// @brief The temperature a heater heats to while its own programme runs.
/// @param profile The device's profile.
/// @param readings What the hub has read from the heater.
/// @return The temperature in force, or NaN when the programme does not run (or the profile has none) or
///         the register has not been read.
float programme_temperature_in_force(const HeatingProfile &profile, const HeatingReadings &readings);

/// @brief Drop the temperature in force from the readings when the programme does not run.
/// @details Keeps a value from an earlier run of the programme from being shown when the heater returns to it.
/// @param profile The device's profile.
/// @param[in,out] readings What the hub has read from the heater.
void clear_inactive_programme_reading(const HeatingProfile &profile, HeatingReadings &readings);

// === Heater writes: what a request turns into ===

/// @brief What a climate request asks for.
enum class HeatingWriteKind : uint8_t {
  MODE,    ///< Select an HVAC mode.
  PRESET,  ///< Select a preset, standard or custom.
  TARGET,  ///< Set the target temperature.
};

/// @brief One climate request, in profile terms: no ESPHome type.
struct HeatingWriteRequest {
  HeatingWriteKind kind{HeatingWriteKind::MODE};  ///< What the request asks for.
  HeatingHvacMode hvac{HeatingHvacMode::OFF};     ///< MODE: the mode.
  HeatingPresetId preset{HeatingPresetId::NONE};  ///< PRESET: the standard preset, or CUSTOM.
  const char *custom_name{nullptr};               ///< PRESET with CUSTOM: the preset's name.
  float target_c{NAN};                            ///< TARGET: the requested temperature.
  float shown_target_c{NAN};                      ///< MODE: the target the card shows now (NaN when none), for a
                                                  ///< mode that sends the target (HEATING_MP_FROM_TARGET).
};

/// @brief The period of its programme a heater says it is in, from the heating level of its status read.
enum class HeatingPeriod : uint8_t {
  UNKNOWN,  ///< Not read, no reply, or a level that is neither comfort nor eco.
  COMFORT,  ///< The level is comfort.
  ECO,      ///< The level is eco.
};

/// @brief What the hub knows about the device a request goes to.
struct HeatingDeviceFacts {
  bool has_record{false};                              ///< The hub has a record of the device.
  float range_min_c{HEATING_TEMPERATURE_RANGE_MIN_C};  ///< Lower end of the YAML temperature range.
  float range_max_c{HEATING_TEMPERATURE_RANGE_MAX_C};  ///< Upper end of the YAML temperature range.
  float reported_setpoint_c{NAN};                      ///< The setpoint the device last reported (NaN until read).
  bool programme_runs{false};                          ///< The programme runs (programme_period_active()) on a
                                                       ///< heater that has reported its temperature in force in
                                                       ///< this boot (TemperatureInForceSupport::REPORTED): a
                                                       ///< target is then a temporary override.
  bool in_force_unresolved{false};                     ///< The programme runs on a heater that has not yet answered
                                                       ///< the read of its temperature in force in this boot
                                                       ///< (TemperatureInForceSupport::UNKNOWN): whether a target
                                                       ///< is an override or a comfort write is not known yet.
  /// The heater answered the read of the registers that say whether the programme runs (the mode, and the
  /// timed run where it matters), made for this request. Only a target on a profile with a programme period
  /// looks at it: without it the heater did not say what it runs.
  bool programme_state_answered{false};
  /// The period the heater reported in the status read made for this request. Only a target on a profile
  /// whose override applies in an eco period only (HeatingProgrammePeriod::override_in_eco_only) looks at it,
  /// while the programme runs; UNKNOWN for every other heater.
  HeatingPeriod period{HeatingPeriod::UNKNOWN};
};

/// @brief Why a request becomes no write.
enum class HeatingRefusal : uint8_t {
  NONE,                        ///< The request becomes a write.
  NO_MATCH,                    ///< The profile offers no such mode or preset.
  NO_SETPOINT,                 ///< The profile has no adjustable setpoint.
  NO_TARGET,                   ///< The mode sends the target temperature and none is known or encodable.
  UNENCODABLE,                 ///< The target temperature cannot be encoded over the device's range.
  PROGRAMME_STATE_UNANSWERED,  ///< A target on a heater with a programme whose mode (or timed run) read got no
                               ///< reply or no value (HeatingDeviceFacts::programme_state_answered): nothing is
                               ///< written, because neither register is known to be right.
  IN_FORCE_UNKNOWN,            ///< A target while the programme runs, before the heater has answered the read of its
                               ///< temperature in force in this boot (HeatingDeviceFacts::in_force_unresolved): the
                               ///< read made for the request got no reply, so nothing is written.
  PERIOD_UNKNOWN,              ///< A target while the programme runs on a heater that takes the override in an eco
                               ///< period only (HeatingProgrammePeriod::override_in_eco_only), and the status read
                               ///< made for the request gave no comfort or eco level (HeatingDeviceFacts::period):
                               ///< nothing is written, because the override or the comfort write would be a guess.
};

/// @brief How a planned write reaches the device.
enum class HeatingPlanKind : uint8_t {
  NONE,         ///< No write: see HeatingWritePlan::refusal.
  REGISTER,     ///< A private register write.
  LEVEL,        ///< A heating-level EXECUTE.
  TEMPERATURE,  ///< An EXECUTE with a temperature or an alias as the main parameter.
};

/// @brief The write a request turns into, with everything needed to send it and to read it back.
struct HeatingWritePlan {
  HeatingPlanKind kind{HeatingPlanKind::NONE};             ///< How to send it.
  HeatingRefusal refusal{HeatingRefusal::NONE};            ///< Why there is no write (kind NONE).
  HeatingFunction register_fn{HeatingFunction::SET_MODE};  ///< REGISTER: the register.
  float register_value{0.0F};      ///< REGISTER: the value, as encode_heating_payload() takes it.
  uint8_t level{0};                ///< LEVEL: the level low byte.
  uint16_t main_parameter{0};      ///< TEMPERATURE: the main parameter.
  HeatingFp fp{};                  ///< TEMPERATURE: the functional parameter that accompanies it.
  bool temporary_override{false};  ///< REGISTER: the write is a temporary override of the programme; a read-back
                                   ///< that differs from it is a refusal (heating_override_taken()).
  bool read_back{false};           ///< Read `read_back_fn` after an accepted write.
  HeatingFunction read_back_fn{HeatingFunction::SET_MODE};  ///< The register to read back.
  int8_t preset_index{-1};  ///< PRESET: index into the profile's presets of the matched entry; -1 otherwise.

  /// @return Whether the request became a write.
  [[nodiscard]] bool is_write() const { return this->kind != HeatingPlanKind::NONE; }
};

/// @brief Does a request ask the heater what it runs before it is planned?
/// @details A target on a profile with a programme period does: where it goes (the override register or the
///          comfort setpoint) depends on whether the programme runs now, and only the heater knows. Nothing
///          else reads first: a mode or preset is written as asked, a profile without a programme period has
///          one rule for every target.
/// @param profile The device's profile.
/// @param request What the card asks for.
/// @return true for a TARGET on a profile with a programme period.
bool heating_request_reads_programme_state(const HeatingProfile &profile, const HeatingWriteRequest &request);

/// @brief Collect what the hub knows about a device, for plan_heating_write().
/// @param dev The device record, or nullptr when the hub has none.
/// @param programme_state_answered Whether the read of the programme-state registers made for this request was
///        answered (HeatingDeviceFacts::programme_state_answered); false when no read was made. When it was, the
///        device's stored heating level is the one that read just refreshed and gives the period.
/// @return The facts; `has_record` false for nullptr.
HeatingDeviceFacts heating_device_facts(const IoDevice *dev, bool programme_state_answered);

/// @brief Decide what one climate request writes to a heater.
/// @details The single place that turns a mode, preset or target into a register write, a heating level or a
///          temperature EXECUTE: which of the three the profile's control path calls for, with which value
///          (including the main parameter of a level, an alias or an encoded temperature, and the target a
///          mode sends), and whether the register is read back afterwards. The temperature is encoded over
///          the fixed range of a profile that has one, else over the device's YAML range.
///
///          A target on a profile with a programme period is planned from what the heater just said it runs
///          (heating_request_reads_programme_state()): the facts come from a read made for this request, and
///          without an answer (HeatingDeviceFacts::programme_state_answered) the target is refused with
///          HeatingRefusal::PROGRAMME_STATE_UNANSWERED. The mode the card shows is not an input.
///
///          A target set while the programme runs (HeatingDeviceFacts::programme_runs: the heater's mode shows
///          `auto`, no timed run is on top, and it has reported a temperature in force in this boot) is a temporary
///          override: it is planned as a write of HeatingProgrammePeriod::derogation_fn with a read-back of that
///          register (HeatingWritePlan::temporary_override), and the comfort setpoint is not touched, in every period
///          (ADR 0092). On a heater that takes the override in an eco period only
///          (HeatingProgrammePeriod::override_in_eco_only, ADR 0094) that holds in an eco period; in a comfort period
///          the target is the comfort setpoint, as on the heater's panel, and a period that is not known
///          (HeatingDeviceFacts::period) is refused with HeatingRefusal::PERIOD_UNKNOWN.
///          In any other mode, and on a heater that answered the read of its temperature in force without a
///          value (TemperatureInForceSupport::ABSENT), it writes the comfort setpoint. While the programme runs and
///          the heater has not answered the read of its temperature in force yet
///          (HeatingDeviceFacts::in_force_unresolved) the target is refused with HeatingRefusal::IN_FORCE_UNKNOWN:
///          neither write is right before the heater has answered, so a silent heater gets no write.
///          \todo Confirm the override on a radiator that takes it. A towel dryer takes it in an eco period and
///                refuses it in a comfort period (issue #202), which is why it writes the comfort setpoint there; a
///                Thermor EMOTION 3 H radiator refuses it (issue #228) and has no rule of its own yet.
/// @param profile The device's profile.
/// @param request What the card asks for.
/// @param facts What the hub knows about the device.
/// @return The plan; `kind` NONE with a `refusal` when the request is no write.
HeatingWritePlan plan_heating_write(const HeatingProfile &profile, const HeatingWriteRequest &request,
                                    const HeatingDeviceFacts &facts);

/// @brief Did the heater take a temporary override? Compares the read-back of the override register with
///        the request, in the register's resolution of a tenth of a degree.
/// @param requested_c The temperature written.
/// @param read_back_c The temperature the register holds afterwards (NaN when the read-back failed).
/// @return true when the register holds what was asked.
bool heating_override_taken(float requested_c, float read_back_c);

// === The climate card: what it shows ===

/// @brief The heating / idle state a card shows.
enum class HeatingAction : uint8_t {
  OFF,      ///< The heater is off.
  HEATING,  ///< The element is heating.
  IDLE,     ///< On, and the element is not heating.
};

/// @brief What a climate card shows, in profile terms: no ESPHome type.
/// @details Folded from the heater's readings (reduce_heating_card()) and from the last command
///          (command_heating_card()). The two bookkeeping flags decide how the two meet: until a reading has
///          arrived the card shows what was commanded, afterwards only what the heater reports.
struct HeatingCardState {
  HeatingHvacMode mode{HeatingHvacMode::OFF};  ///< The HVAC mode shown.
  int8_t preset_index{-1};                     ///< Index into the profile's presets shown, or -1 for none.
  float target_c{NAN};                         ///< The target temperature shown; NaN when none is known.
  float current_c{NAN};                        ///< The measured temperature; NaN until read or on a sensor fault.
  float max_target_c{HEATING_TEMP_MAX_C};      ///< Upper end of the target range the card offers.
  HeatingAction action{HeatingAction::OFF};    ///< Heating, idle or off.
  bool observed{false};                        ///< A reading has been folded in: readings, not commands, decide.
  bool mode_commanded{false};                  ///< A mode was commanded (matters where nothing reads the mode).

  /// @brief Do the two cards show the same thing?
  /// @details Compares what a card displays (mode, preset, temperatures, range, action), not the two
  ///          bookkeeping flags, so a flag flipping alone is no change to publish. Two NaN temperatures are
  ///          the same state, not a change.
  /// @param other The card to compare with.
  [[nodiscard]] bool shows_same_as(const HeatingCardState &other) const;
};

/// @brief What a command leaves on the card.
struct HeatingCardCommand {
  HeatingCardState card;  ///< The card after the command.
  bool shown{false};      ///< The command is shown: no reading has been folded in yet, so the card takes it.
                          ///< False when the heater's own report decides what the card shows.
};

/// @brief The card of a heater nothing has been read from yet.
/// @details Its target range reaches up to the profile's own upper end where the profile has a setpoint.
/// @param profile The device's profile.
HeatingCardState initial_heating_card(const HeatingProfile &profile);

/// @brief Fold a heater's readings into the card.
/// @details The mode or level, the preset kept in a register of its own, the temperatures (while the
///          programme runs, the temperature in force replaces the comfort setpoint), the action and the heater's own
///          maximum setpoint, never past the profile's upper end. A value the profile does not map leaves the card as
///          it was. Where nothing reads the mode, a device that reports a setpoint or an alias is evidently on, so the
///          card shows heat until a mode is commanded. Sets `observed` once any reading was present.
/// @param profile The device's profile.
/// @param readings What the hub has read from the heater.
/// @param prior The card before the readings.
/// @return The card after them.
HeatingCardState reduce_heating_card(const HeatingProfile &profile, const HeatingReadings &readings,
                                     const HeatingCardState &prior);

/// @brief What the card shows after a command the heater accepted.
/// @details The one place the commanded and the observed state meet. Once a reading has been folded in, the
///          card follows the read-back and the command changes nothing (a mode on a profile that never reads
///          its mode back is the exception: the command stands). Before that the card takes the command:
///          a mode, which on the register and level paths supersedes the preset; a preset, shown as the
///          read-back would show it (through the profile's observed tables) or, where nothing is read back,
///          as the preset itself with heat for an alias preset; a target, which on a temperature heater ends
///          an alias preset. A mode command always sets `mode_commanded`.
/// @param profile The device's profile.
/// @param request The request that was sent.
/// @param plan The plan it became; its `preset_index` names the preset a preset request matched.
/// @param prior The card before the command.
/// @return The card after it and whether the command is shown.
HeatingCardCommand command_heating_card(const HeatingProfile &profile, const HeatingWriteRequest &request,
                                        const HeatingWritePlan &plan, const HeatingCardState &prior);

/// @brief How far a profile's heater has been confirmed on hardware.
enum class HeatingHardwareStatus : uint8_t {
  UNVALIDATED,          ///< Not run on hardware: the temperature EXECUTE paths, or no profile.
  LEVELS_CONFIRMED,     ///< Heating levels: every level but Boost confirmed; no level read-back.
  WRITES_EXPERIMENTAL,  ///< Readings and setpoint confirmed; the mode and preset writes are not.
  CONFIRMED,            ///< Setpoint, mode, presets and read-back confirmed.
};

/// @brief How far a heater's profile has been confirmed on hardware.
/// @param profile The device's profile, or nullptr while the type is unknown.
/// @return UNVALIDATED for nullptr and for the temperature EXECUTE profiles, LEVELS_CONFIRMED for the heating-level
///         profile, otherwise CONFIRMED or WRITES_EXPERIMENTAL for a register profile.
HeatingHardwareStatus heating_hardware_status(const HeatingProfile *profile);

}  // namespace home_io_control
}  // namespace esphome

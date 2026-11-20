#pragma once

/// @file proto_heating.h
/// @brief Pure codec for the private register protocol of 2W heaters and water heaters (CMD_WRITE_PRIVATE 0x20).
/// @ingroup hioc_protocol
///
/// A heater with an adjustable setpoint (node type 52, subtype 1) exposes its state as 16-bit
/// registers `0x01xx`, read with `0C 60 01 <reg>` and written with `0C 61 01 <reg> <value>`; the
/// device answers with CMD_WRITE_PRIVATE_ACK (0x21) echoing the header, followed by the value. Values
/// are little-endian. A domestic hot water heater answers the same requests in register family `0x00`
/// (`0C 60 00 <reg>`) and holds its multi-byte values most significant byte first; its register row names
/// the family, and the codec the byte order. An Atlantic AGILIA IO radiator answers every read and write this way
/// (tests/corpus/captures/exchange/atlantic_agilia_exchange_{status_read,setpoint_and_off_read_back,mode_read_back}.yaml,
/// pinned by corpus_heating_test.cpp).
///
/// One descriptor table (`DEVICE_REGISTERS` in the .cpp) lists every register this component
/// uses: its family, its access (read-only, read/write, write-only) and how its value is encoded. The same row
/// drives the get payload, the set payload and the decoder of the reply, so a register is described
/// exactly once. This module is pure: it does not touch a device record or the radio —
/// proto_commands.h's create_write_private() frames a payload.
///
/// Confirmed on that radiator: the setpoint, mode (all four writable values), room temperature,
/// regulator and maximum-setpoint registers. The presence register has been written and acknowledged,
/// with no visible effect observed; the window and open-window-detection registers have only been read
/// at rest.
///
/// An Atlantic Adelis towel dryer answers the same protocol and adds its own registers: its mode
/// (0x0136), its temporary state (boost or drying, 0x013E), its working rate (0x0126), the temperature
/// it holds right now (0x0137) and its eco setback (0x0104). They have been read on two units in every
/// state (tests/corpus/captures/probe/atlantic_adelis_probe_*.yaml); writing 0x0136 and 0x013E is
/// confirmed against the display, the others are read-only.
///
/// A Sauter Hekla heater adds a night light: on/off in register 0x0144 (`01` / `00`, read on a heater
/// with the light on and off, and written as `0C 61 01 44 <value>` by the heater's own Cozytouch
/// controller) and its brightness in register 0x0142, which runs 0-10000: the panel's four levels write
/// 100, 1000, 2000 and 10000 (issue #222, corpus
/// `sauter_hekla_exchange_night_light_brightness_levels_sx1262`).
///
/// An Atlantic Calypso Connecte water heater holds its water temperature, operating mode, heating state, boost and
/// away durations, energy counter and brand letter in family-0x00 registers (issue #198, corpus
/// tests/corpus/captures/exchange/atlantic_calypso_exchange_register_read.yaml, pinned by corpus_dhw_test.cpp); its
/// target, boost and away state travel in the status reply (proto_dhw.h).

#include "proto_frame.h"
#include "proto_private_registers.h"
#include "proto_value_codecs.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace esphome {
namespace home_io_control {

/// @brief Registers and functions of a 2W heater, one row of the descriptor table each.
///
/// The `heating_control` action offers some of them as functions under a name built from the row's name; the same row
/// is also the register's read path.
enum class DeviceRegister : uint8_t {
  PAIRED_DEVICES,         ///< Register 0x012C, read: the paired-device list. Logged, never decoded.
  COMFORT_SETPOINT,       ///< Register 0x0103, read/write: comfort setpoint, tenths of a degree.
  OPERATING_MODE,         ///< Register 0x0100, read/write: operating mode (HeatingMode).
  PRESENCE,               ///< Register 0x0110, read/write: someone is in the room (1) or not (0). A controller
                          ///< writes it to tell the heater; a Sauter Hekla reports there whether its own
                          ///< detection sees someone (issue #222), 0 with nobody in the room and 1 while
                          ///< someone walks around it, with its occupancy detection on.
                          ///< \todo Confirm on another heater that the register reports detected presence.
  OPEN_WINDOW_DETECTION,  ///< Register 0x0106, read/write: open-window detection on (1) or off (0).
  SETPOINT_BLOCK,         ///< Register 0x0130, read: the comfort/eco/auto setpoint block. Logged, never decoded.
  ROOM_TEMPERATURE,       ///< Register 0x0115, read-only: measured room temperature, signed hundredths of a degree.
  REGULATOR_STATE,        ///< Register 0x0117, read-only: 0 none, 1 standby, 2 heating.
  MAX_SETPOINT,           ///< Register 0x0102, read-only: highest setpoint the heater accepts, tenths of a degree.
  WINDOW_OPEN,            ///< Register 0x010E, read-only: the heater's window sensor, 0 closed / 1 open.
  TOWEL_MODE,             ///< Register 0x0136, read/write: a towel dryer's mode (TowelDryerMode).
  TEMPORARY_STATE,        ///< Register 0x013E, read/write: a towel dryer's temporary state (HeatingTemporaryState).
  WORKING_RATE,           ///< Register 0x0126, read-only: share of the time the element heats, 0-100 %.
  TOWEL_TEMPERATURE_IN_FORCE,  ///< Register 0x0137, read-only: the temperature a towel dryer holds right now, tenths of
                               ///< a degree. Only a temperature while no boost or drying runs (it reads 1 and 2 there).
                               ///< \todo Confirm on a second towel dryer (the reads so far are from one unit).
  ECO_SETBACK,                 ///< Register 0x0104, read-only: how far below the comfort setpoint a towel dryer's eco
                               ///< period heats, tenths of a kelvin.
  NIGHT_LIGHT,                 ///< Register 0x0144, read/write: a heater's night light on (1) or off (0).
  NIGHT_LIGHT_BRIGHTNESS,  ///< Register 0x0142, read/write: the night light's brightness, 16-bit, 0-10000 on the wire.
                           ///< Written and read as a percentage on HEATING_NIGHT_LIGHT_SCALE. A write dims the
                           ///< light on a Sauter Hekla (issue #222).
                           ///< \todo Confirm that values between the panel's four levels are taken as written.
  DEROGATED_SETPOINT,      ///< Register 0x011D, read/write: a heater's temporary override of its programme,
                           ///< tenths of a degree; 0 while none is active. Written with 0 it cancels the override.
                           ///< A towel dryer's own panel sets it when the temperature is raised in an eco period
                           ///< ("dérogation") and the override ends at the next comfort period. A sweep taken
                           ///< during a panel override read 16.5 here (0 otherwise).
                           ///< A write from the hub behaves the same on a towel dryer: taken and held in an eco
                           ///< period, cancelled by 0, acknowledged and ignored (reads back 0) in a comfort period
                           ///< (issue #202). A Thermor EMOTION 3 H radiator ignores it as well (issue #228).
                           ///< \todo Confirm on a radiator that takes it that a write holds the temperature until
                           ///<       the programme's next change and that a write of 0 cancels it.
  TEMPERATURE_IN_FORCE,    ///< Register 0x012A, read-only: the temperature a heater with an adjustable setpoint
                           ///< (AGILIA, Hekla, EMOTION 3 H) holds right now, tenths of a degree. The same value
                           ///< as TOWEL_TEMPERATURE_IN_FORCE on the towel dryer, whose sweep read it in both.
                           ///< \todo Read on a radiator: no AGILIA, Hekla or EMOTION 3 H has answered it yet. A
                           ///<       heater that answers it empty keeps the comfort-setpoint write.
  CENTRAL_HEATING_MODE,    ///< Register 0x0153, read-only: 1 while a mixed towel dryer runs on the house's
                           ///< central heating (its electric element stays off), 0 while it uses the element.
                           ///< An electric unit is expected to answer without a value.
                           ///< \todo Experimental: read on one Atlantic Adelis mixed unit over four panel
                           ///<       toggles (issue #243), never written. Confirm on a second unit, and on
                           ///<       an electric one what the read returns.
  OCCUPANCY_DETECTION,     ///< Register 0x0101, read/write: the heater's occupancy detection on (1) or off (0).
                           ///< On a Sauter Hekla it reads 1 with the detection switched on and 0 with it off,
                           ///< and the vendor app writes it (issue #222); the detected presence shows in
                           ///< PRESENCE.
                           ///< \todo Experimental except on the Hekla: confirm on another heater that it
                           ///<       answers this register.
  ENERGY_TOTAL,            ///< Register 0x0116, read-only: the energy the heater has used, whole kilowatt-hours,
                           ///< the total its own display shows. The only register of the family whose
                           ///< 16-bit value is sent most significant byte first (every other is little-endian).
                           ///< Two Sauter Tahual radiators answered `0D 5E` (3422 kWh) and `01 08` (264 kWh),
                           ///< equal to their displays (PR #238, corpus `sauter_tahual_probe_energy_register`);
                           ///< two Thermor EMOTION 3 H answered `0C BE` and `0F A5` (issue #228) and two Atlantic
                           ///< Adelis units `02 3F` and `03 B2`, whose displays were not compared.
                           ///< \todo Experimental except on the Sauter Tahual: compare with the display on
                           ///<       an EMOTION 3 H, an Adelis and a Hekla.
  BOOST_REMAINING,         ///< Register 0x013A, read-only: the minutes left of a running boost, rounded up; one
                           ///< byte. Read on an Atlantic Adelis during a one-hour boost: `3C` (60) a moment
                           ///< after the start and `37` (55) five minutes later, matching the display's
                           ///< countdown (issue #202, corpus `atlantic_adelis_probe_boost_remaining`). A second
                           ///< unit read `1C` (28) during a boost, and `00` was read during a drying run; no
                           ///< read while nothing runs exists, so the hub reads it during a boost only.
                           ///< \todo Experimental: read on Atlantic Adelis units only; confirm on another dryer.
  BOOST_DURATION,          ///< Register 0x013B, read-only here: the boost duration set on the panel, minutes. Two
                           ///< bytes, the duration then the panel's maximum: `3C 3C` (60, 60) with a one-hour
                           ///< boost set, `0F 3C` (15) and `1E 3C` (30) after the panel was set to those, and
                           ///< `23 3C` (35, 60) on a second unit; the value changed 1-3 minutes after the panel
                           ///< did (issue #202). Only the first byte is read.
                           ///< \todo Experimental: nobody has written it, so how the dryer takes a new
                           ///<       duration is not known.
  DRYING_REMAINING,        ///< Register 0x013C, read-only: the minutes left of a running drying run; one byte,
                           ///< `1C` (28) with the display between 30 and 27 minutes left (issue #202, corpus
                           ///< `atlantic_adelis_probe_register_01_sweep_drying`), `00` while none runs.
                           ///< \todo Experimental: read on Atlantic Adelis units only; confirm on another dryer.
  DRYING_DURATION,         ///< Register 0x013D, read-only here: the duration of the last drying run that was
                           ///< started, minutes. Two bytes, the duration then the panel's maximum: `1E 78`
                           ///< (30, 120) after a run of 30 minutes. A change on the panel alone does not show
                           ///< until the next run starts (checked twice, issue #202). Only the first byte is read.
                           ///< \todo Experimental: nobody has written it, so how the dryer takes a new
                           ///<       duration is not known.
  DHW_WATER_TEMPERATURE,   ///< Family 0x00, register 0x17, read-only: a water heater's water temperature, signed
                           ///< big-endian tenths of a degree in the first two value bytes. Four more bytes
                           ///< follow that never changed; they are not read.
  DHW_MODE,                ///< Family 0x00, register 0x06, read/write: a water heater's operating mode
                           ///< (DhwMode). Followed the display in every run.
  DHW_HEATING,             ///< Family 0x00, register 0x21, read-only: what is heating, eight bytes. Byte 0 is on
                           ///< while the heater heats at all, byte 2 while the electric element runs (during
                           ///< a boost only), bytes 3 and 4 while the heat pump runs (always set together).
                           ///< No typed value of its own: each of those is a reading that is a slice of it.
  DHW_BOOST_DURATION,      ///< Family 0x00, register 0x00, read/write: a water heater's boost duration, one byte.
                           ///< It read 1 during a boost started on the display and 0 before and after it. A
                           ///< duration is not the state: the state is FP16 (proto_dhw.h).
  DHW_AWAY_DURATION,       ///< Family 0x00, register 0x05, read/write: a water heater's away (holiday) duration,
                           ///< one byte: the days the holiday still runs. It read 0 at rest and with a holiday
                           ///< scheduled for later, 4 and 2 four and two days before the end, and 255 for a
                           ///< holiday set on the display with no end date.
  DHW_ENERGY,              ///< Family 0x00, register 0x04, read-only: the energy a water heater has consumed since
                           ///< it was installed, 32-bit big-endian, Wh. Matches the display's "since start"
                           ///< total to the kWh, and rose while the heat pump ran.
  DHW_BRAND,               ///< Family 0x00, register 0x07, read-only: one ASCII letter naming the brand (`41` =
                           ///< 'A' on the Calypso).
  COUNT,                   ///< Not a function: the number of registers, for a table or loop over all of them.
};

/// @brief Operating modes of the mode register (DeviceRegister::OPERATING_MODE).
enum class HeatingMode : uint8_t {
  AUTO = 0x00,               ///< Automatic mode; the device manages the setpoint itself.
  MANUAL = 0x01,             ///< Manual mode; follows the last written setpoint.
  PROG = 0x02,               ///< Program mode; the device runs its own stored weekly schedule.
  EXTERNAL_SETPOINT = 0x03,  ///< Setpoint comes from an external controller. Readable, never written (see
                             ///< heating_mode_is_writable()). Not named EXTERNAL: the Arduino core defines
                             ///< that as a macro.
  OFF = 0x04,                ///< Off / standby.
};

/// @brief The word for a mode in the `heating_control` action ("auto", "manual", "prog", "external", "off").
/// @param mode Mode.
/// @return Null-terminated lowercase word; "unknown" for an out-of-range value.
const char *heating_mode_name(HeatingMode mode);

/// @brief The mode a word names, the inverse of heating_mode_name().
/// @param name Lowercase word.
/// @return The mode, or nullopt for a word no mode has. The result is not necessarily writable
///         (heating_mode_is_writable()).
std::optional<HeatingMode> heating_mode_from_name(std::string_view name);

/// @brief The words of the modes that may be written, for a message that lists them.
/// @return "auto, manual, prog, off": every mode but the external setpoint, comma separated.
std::string heating_writable_mode_names();

/// @brief Whether this component may write a mode to the mode register.
/// @details Every mode except EXTERNAL_SETPOINT. That mode hands the setpoint to an external controller, so
///          writing it is untested and could leave a heater waiting for a controller that is not there.
///          Reading it is fine: the climate card shows it as heat. encode_heating_write() refuses a mode
///          this rule rejects; the actions and climate profiles offer only modes it accepts.
/// @param mode Mode to check.
/// @return false for EXTERNAL_SETPOINT, true for every other mode.
constexpr bool heating_mode_is_writable(HeatingMode mode) { return mode != HeatingMode::EXTERNAL_SETPOINT; }

/// @brief Modes of a towel dryer's mode register (DeviceRegister::TOWEL_MODE).
/// @details OFF, PROG and MANUAL are the values two Atlantic Adelis units report with their display
///          showing off, PROG and MANUEL (issue #202, corpus atlantic_adelis_probe_mode_reads).
enum class TowelDryerMode : uint8_t {
  OFF = 0x00,     ///< Off (standby).
  AUTO = 0x01,    ///< Defined by the register's range, never seen on a device; never written here.
  PROG = 0x02,    ///< Program mode: the device runs its own weekly program.
  MANUAL = 0x03,  ///< Manual mode: the device holds the comfort setpoint.
};

/// @brief Temporary states of a towel dryer (DeviceRegister::TEMPORARY_STATE): a timed run on top of
/// the mode, which the mode register does not show.
enum class HeatingTemporaryState : uint8_t {
  NONE = 0x00,    ///< No temporary state: the mode applies.
  BOOST = 0x01,   ///< Boost: full power for the boost duration.
  DRYING = 0x02,  ///< Drying: full power for the drying duration.
};

/// @brief Regulator states of the regulator register (DeviceRegister::REGULATOR_STATE).
enum class HeatingRegulatorState : uint8_t {
  NONE = 0x00,     ///< No regulation active.
  STANDBY = 0x01,  ///< Regulating, the element is idle.
  HEATING = 0x02,  ///< The element is heating.
};

/// @brief Lowest setpoint a write may carry: the low end of the write range of the setpoint registers.
///
/// The conventional frost-protection setpoint, safely representable as wire value 70.
constexpr float HEATING_TEMP_MIN_C = 7.0F;

/// @brief Highest setpoint a write may carry: the high end of the write range of the setpoint registers.
///
/// The setpoint register is a 16-bit little-endian value in tenths of a degree; 28.0 °C (`18 01`) is
/// the ceiling Atlantic radiator manuals document and the value an AGILIA IO radiator reports in its
/// maximum setpoint register (tests/corpus/captures/exchange/atlantic_agilia_exchange_status_read.yaml).
/// No setpoint above 25.5 °C (a non-zero high byte) has been written on hardware yet.
constexpr float HEATING_TEMP_MAX_C = 28.0F;

/// @brief Payload byte at index 2: the high byte of the 16-bit register number of a heater, `0x01`.
constexpr uint8_t HEATING_REGISTER_HIGH_BYTE = 0x01;

/// @brief The high byte of the 16-bit register number of a domestic hot water heater: its registers are family 0x00.
constexpr uint8_t DHW_REGISTER_HIGH_BYTE = 0x00;

/// @brief Default encoding range of a temperature main parameter, in degrees Celsius. The main
/// parameter spans TEMPERATURE_MP_SPAN steps over [min, max]; this range is the one a device
/// is assumed to use unless the YAML says otherwise (`min_temperature:` / `max_temperature:`).
static constexpr float HEATING_TEMPERATURE_RANGE_MIN_C = -40.0F;
static constexpr float HEATING_TEMPERATURE_RANGE_MAX_C = 80.0F;  ///< See HEATING_TEMPERATURE_RANGE_MIN_C.

/// @brief Highest value of the night light brightness register (DeviceRegister::NIGHT_LIGHT_BRIGHTNESS),
/// full brightness. The register runs 0-10000; a Sauter Hekla's panel sets 100, 1000, 2000 and 10000
/// for its four levels (issue #222).
constexpr uint16_t HEATING_NIGHT_LIGHT_BRIGHTNESS_MAX = 10000;

/// @brief Highest brightness percentage of a Home Assistant light.
constexpr uint8_t HEATING_NIGHT_LIGHT_PERCENT_MAX = 100;

/// @brief The night light brightness scale, the one the heater's own vendor app uses: its slider shows
/// 1000 as 33 %, 2000 as 66 % and 10000 as 100 % (issue #222). Between two knots the scale is linear.
/// It spreads the range where the light visibly changes over two thirds of the slider: above about 3000
/// a Sauter Hekla's light hardly gets brighter (issue #222). The brightness register's row carries it
/// (ValueCodec::SCALED_U16), so a write takes a percentage and a reading shows one.
constexpr ValueScaleKnot HEATING_NIGHT_LIGHT_SCALE[] = {
    {0, 0},
    {33, 1000},
    {66, 2000},
    {HEATING_NIGHT_LIGHT_PERCENT_MAX, HEATING_NIGHT_LIGHT_BRIGHTNESS_MAX},
};
static_assert(value_scale_rises(HEATING_NIGHT_LIGHT_SCALE), "the scale divides by the distance between its knots");

/// @brief Register 0x0119: the heater's model name, ASCII, not terminated ("ADELIS 2 DIGIT" on an
/// Atlantic Adelis, corpus atlantic_adelis_probe_register_01_sweep). Read raw for a log line; no
/// DeviceRegister decodes it.
constexpr uint8_t HEATING_REGISTER_MODEL_NAME = 0x19;

/// @brief Who may do what with a register.
enum class HeatingAccess : uint8_t {
  READ_ONLY,   ///< Reads only; a write is refused by the codec.
  READ_WRITE,  ///< Reads and writes.
};

/// @brief A register's refusal of values its range holds but this component never writes.
/// @return true for a value the register refuses; false for one the codec alone must judge.
using HeatingValueRefusal = bool (*)(float value);

/// @brief One register of the heater, the single description both directions are built from.
///
/// A request is {PRIVATE_REGISTER_PREFIX, direction, register_high, register_low, [value...]}; `direction` is
/// PRIVATE_REGISTER_DIRECTION_GET or PRIVATE_REGISTER_DIRECTION_SET. The row is also the register's write rule: its
/// codec, `max_value`, `write_range` and `refuses` say which values a write may carry, and every caller asks
/// heating_write_accepts() instead of keeping a bound of its own.
struct DeviceRegisterRow {
  /// The register's name: its DeviceRegister enumerator in lower case. It is the register's label in logs and the
  /// stem of the function name the `heating_control` action takes ("set_" or "read_" + name); a sync test checks it.
  const char *name;
  DeviceRegister fn;     ///< Function this row describes.
  uint8_t register_low;  ///< Payload byte 3: low byte of the register number.
  HeatingAccess access;  ///< Whether the register may be read, written or both.
  ValueCodec codec;      ///< How the value maps onto the trailing bytes.
  /// BOUNDED_BYTE, BOUNDED_U16, SCALED_U16, SCALED_BYTE: highest value a reply may carry and a write may send.
  uint16_t max_value{0};
  /// Payload byte 2: the register's family, the high byte of its number. The family does not say how a multi-byte
  /// value is laid out: the codec does.
  uint8_t register_high{HEATING_REGISTER_HIGH_BYTE};
  /// The action's read of this function shows the decoded value in its message, not only that the device
  /// answered (the window sensor).
  bool action_shows_value{false};
  // NOLINTBEGIN(readability-redundant-member-init): g++'s -Wmissing-field-initializers wants the member
  // initializer before a row may leave the field out.
  std::optional<ValueRange> write_range{};  ///< Tenths registers: the values a write may carry.
  HeatingValueRefusal refuses{nullptr};     ///< A value the register refuses although the codec holds it.
  /// The `heating_control` action offers this function (see heating_action_function_name()). The label names the value
  /// in the action's messages ("temperature", "mode"); "" for a function that takes no value (a read). nullptr: not a
  /// function of the action.
  const char *action_value_label{nullptr};
  std::span<const ValueScaleKnot>
      scale{};  ///< SCALED_U16, SCALED_BYTE: the scale from the natural value to the register value.
  // NOLINTEND(readability-redundant-member-init)
};

/// @brief Replaces the register table for as long as it lives, so a test can give a device registers production does
/// not have (a heater model no hardware has confirmed yet).
/// @details The table is process-wide: one object at a time, on the test's thread. A row names its register by a
///          DeviceRegister value that no production row uses (a value above DeviceRegister::COUNT). Production code
///          never creates one.
class ScopedRegisterTable {
 public:
  /// @param rows The rows that replace DEVICE_REGISTERS; they must outlive this object.
  explicit ScopedRegisterTable(std::span<const DeviceRegisterRow> rows);
  ~ScopedRegisterTable();
  ScopedRegisterTable(const ScopedRegisterTable &) = delete;
  ScopedRegisterTable &operator=(const ScopedRegisterTable &) = delete;

 private:
  std::span<const DeviceRegisterRow> previous_;  ///< The table to restore.
};

/// @brief The function a `heating_control` function name selects.
struct HeatingActionFunction {
  DeviceRegister fn{};                 ///< The function to run.
  const char *replaced_name{nullptr};  ///< The retired name that was used; nullptr for a current name.
};

/// @brief The name the `heating_control` action offers a function under.
/// @details "set_" + the register's name for a writable register, "read_" + the name for a read-only one.
/// @param fn Function.
/// @return The function name, or "" for a function the action does not offer.
std::string heating_action_function_name(DeviceRegister fn);

/// @brief The function the `heating_control` action runs under a name.
/// @details Only the functions the action offers: the readable and writable settings the table marks for it, not
///          every register of the table. A retired name (ADR 0109) is accepted and says which name it replaced.
/// @param name Lowercase function name as the action takes it.
/// @return The function, or nullopt for a name the action does not offer.
std::optional<HeatingActionFunction> heating_action_function(std::string_view name);

/// @brief How the `heating_control` action names the value of a function in its messages.
/// @param fn Function.
/// @return "temperature", "mode", "presence" or "open-window detection"; "" for an action function that takes no
///         value (a read); nullptr for a function the action does not offer or an out-of-range value.
const char *heating_action_value_label(DeviceRegister fn);

/// @brief The register's name: its DeviceRegister enumerator in lower case, the label of the register in logs
///        ("comfort_setpoint", "towel_temperature_in_force", ...).
/// @param fn Function.
/// @return Null-terminated string; "unknown" for an out-of-range value.
const char *device_register_name(DeviceRegister fn);

/// @brief Whether the register behind a function can be written.
/// @param fn Function.
/// @return true for a read/write or write-only register; false for a read-only one or an
///         out-of-range value.
bool device_register_is_writable(DeviceRegister fn);

/// @brief Whether the register behind a function can be read.
/// @param fn Function.
/// @return true for a register of the table; false for an out-of-range value.
bool device_register_is_readable(DeviceRegister fn);

/// @brief The value form of a register: its codec, bound, write range and scale, which encode_value() and
/// decode_value() take.
/// @param fn Function.
/// @return The form, or nullopt for a function with no row.
std::optional<ValueForm> device_register_form(DeviceRegister fn);

/// @brief Whether the `heating_control` action reports the decoded value of a register it reads.
/// @param fn Function.
/// @return true for a register whose read the action answers with its value (the window sensor).
bool device_register_action_shows_value(DeviceRegister fn);

/// @brief How the value of a register maps onto the bytes of its reply.
/// @param fn Function.
/// @return The register's codec; ValueCodec::NONE for a register with no typed value or an out-of-range value.
ValueCodec device_register_codec(DeviceRegister fn);

/// @brief The two bytes of the register number behind a function: its family (the high byte) and its index.
/// @param fn Function.
/// @param[out] register_high The register's family, HEATING_REGISTER_HIGH_BYTE or DHW_REGISTER_HIGH_BYTE; untouched
///        when false is returned.
/// @param[out] register_low The register's low byte; untouched when false is returned.
/// @return false for an out-of-range value.
bool device_register_bytes(DeviceRegister fn, uint8_t &register_high, uint8_t &register_low);

/// @brief The family of a function's register: the high byte of its number.
/// @param fn Function.
/// @return HEATING_REGISTER_HIGH_BYTE or DHW_REGISTER_HIGH_BYTE; HEATING_REGISTER_HIGH_BYTE for an out-of-range value.
uint8_t device_register_family(DeviceRegister fn);

/// @brief Encode a write ("set") of one register into a CMD_WRITE_PRIVATE (0x20) payload.
///
/// `value` is interpreted by the register's encoding:
///   - COMFORT_SETPOINT: degrees Celsius within [HEATING_TEMP_MIN_C, HEATING_TEMP_MAX_C], encoded as
///     a 16-bit little-endian value in tenths of a degree, round-half-away-from-zero(10 * value).
///   - DEROGATED_SETPOINT: as COMFORT_SETPOINT, or 0 (`00 00`) to cancel the override.
///   - OPERATING_MODE: a HeatingMode value widened to float that heating_mode_is_writable() accepts.
///   - PRESENCE, OPEN_WINDOW_DETECTION, OCCUPANCY_DETECTION: 0 or 1.
///   - TOWEL_MODE: a TowelDryerMode value widened to float (0-3).
///   - TEMPORARY_STATE: a HeatingTemporaryState value widened to float (0-2).
///   - NIGHT_LIGHT: 0 or 1.
///   - DHW_MODE: a DhwMode value widened to float (0-2).
///   - DHW_BOOST_DURATION, DHW_AWAY_DURATION: a whole number 0-255.
///   - NIGHT_LIGHT_BRIGHTNESS: a percentage 0-100, encoded on HEATING_NIGHT_LIGHT_SCALE as a 16-bit
///   little-endian register value (0-HEATING_NIGHT_LIGHT_BRIGHTNESS_MAX), rounded to the nearest whole value.
/// A read-only register (including PAIRED_DEVICES, SETPOINT_BLOCK and the measurement registers) is never
/// encoded as a write, and out-of-range, non-integral (for enum/binary encodings) or non-finite
/// input is rejected: a byte derived from a truncating or wrapping cast is never emitted.
/// @param fn Register to write.
/// @param value Register-specific value (see above).
/// @param out Output buffer of PRIVATE_REGISTER_PAYLOAD_MAX_SIZE bytes.
/// @return Number of payload bytes written (5 or 6), or 0 on invalid input or a read-only register.
size_t encode_heating_write(DeviceRegister fn, float value, uint8_t out[PRIVATE_REGISTER_PAYLOAD_MAX_SIZE]);

/// @brief Does the register's write rule accept this value?
/// @details The one judgement every caller asks before it sends or offers a value: the register is writable, its
///          refusal predicate (the mode register refuses the external setpoint) lets the value through, and the codec
///          takes it inside the row's range, so it is exactly the values encode_heating_write() encodes.
/// @param fn Register to write.
/// @param value Register-specific value (see encode_heating_write()).
/// @return true when a write of `value` would be encoded.
bool heating_write_accepts(DeviceRegister fn, float value);

/// @brief The values a write of a register may carry, where its row names a range.
/// @param fn Register.
/// @return The range of a writable tenths register (the setpoint and the override, HEATING_TEMP_MIN_C to
///         HEATING_TEMP_MAX_C); nullopt for a register with no range of its own, a read-only register or an
///         out-of-range value.
std::optional<ValueRange> heating_write_range(DeviceRegister fn);

/// @brief Encode a read ("get") of one register into a CMD_WRITE_PRIVATE (0x20) payload.
/// @param fn Register to read.
/// @param out Output buffer of PRIVATE_REGISTER_PAYLOAD_MAX_SIZE bytes.
/// @return 4 (the payload is `0C 60 <family> <reg>`: the register's family byte is 01 for a heater and 00 for a water
///         heater), or 0 for a register that has no row or is write-only.
size_t encode_heating_read(DeviceRegister fn, uint8_t out[PRIVATE_REGISTER_PAYLOAD_MAX_SIZE]);

/// @brief Encode the function the `heating_control` action runs: a read for a read-only register,
/// a write for every other.
///
/// PAIRED_DEVICES and SETPOINT_BLOCK are reads (their `value` is ignored); the measurement registers are
/// reads too; the others are writes as in encode_heating_write().
/// @param fn Function to encode.
/// @param value Function-specific value (ignored for a read).
/// @param out Output buffer of PRIVATE_REGISTER_PAYLOAD_MAX_SIZE bytes.
/// @return Number of payload bytes written (4, 5 or 6), or 0 on invalid input.
size_t encode_heating_payload(DeviceRegister fn, float value, uint8_t out[PRIVATE_REGISTER_PAYLOAD_MAX_SIZE]);

/// @brief The value bytes of a reply that echoes a function's register.
/// @details Accepts a CMD_WRITE_PRIVATE_ACK (0x21) whose data starts with `0C 60 <family> <reg>` (a read) or
///          `0C 61 <family> <reg>` (a write) for this function's register. The value bytes follow the echo and may be
///          none: a register the device does not hold is answered without any.
/// @param reply The device's reply frame.
/// @param fn Register the request addressed.
/// @return The value bytes (inside `reply`), or nullopt for another command, another register or another family.
std::optional<std::span<const uint8_t>> register_reply_value(const IoFrame &reply, DeviceRegister fn);

/// @brief Decode the value bytes of a register by the register's own codec.
/// @param fn Register.
/// @param value_bytes The value bytes of a reply, starting at the first one (register_reply_value()).
/// @return The value in natural units (see decode_heating_reply()), or nullopt for a register with no typed value
///         (codec NONE), a truncated or out-of-range value or an out-of-range function.
std::optional<float> decode_register_value(DeviceRegister fn, std::span<const uint8_t> value_bytes);

/// @brief Decode the value a heater answered a register read or write with.
///
/// Accepts a CMD_WRITE_PRIVATE_ACK (0x21) whose data starts with `0C 60 <family> <reg>` or
/// `0C 61 <family> <reg>` for this function's register, followed by at least the register's value bytes.
/// Anything else (another command, another register, a truncated value, a function without a typed
/// decode such as PAIRED_DEVICES) is "no value".
///
/// The value is returned in natural units: degrees Celsius for the temperature registers, whole
/// kilowatt-hours for the energy total, minutes for the run timers, a water heater's water temperature in degrees
/// and its energy in kilowatt-hours with three decimals, percent for the night light's brightness (on
/// HEATING_NIGHT_LIGHT_SCALE), the raw byte for the others. A byte above its
/// register's range (a mode, a state, a percentage) is "no value". A raw `-1` on the room-temperature register is a
/// sensor fault: it decodes as NaN with `true` returned, because the heater did answer.
/// @param reply The device's reply frame.
/// @param fn Register the request addressed.
/// @param[out] value Decoded value; untouched when `false` is returned.
/// @return true when the reply carries a value for this register.
bool decode_heating_reply(const IoFrame &reply, DeviceRegister fn, float &value);

/// @brief Does a heater's reply carry the value bytes of a register, whatever they say?
/// @details True for a reply that echoes the register and holds at least the register's codec length of value bytes,
///          also when decode_heating_reply() refuses them (a mode byte above the register's range). The difference
///          tells a heater that has the register and answers it oddly from one that has none and answers it empty.
/// @param reply The device's reply frame.
/// @param fn Register the request addressed.
/// @return true when the reply holds the register's value bytes; false for an empty answer, another register or a
///         function with no typed value (PAIRED_DEVICES).
bool heating_reply_carries_value(const IoFrame &reply, DeviceRegister fn);

}  // namespace home_io_control
}  // namespace esphome

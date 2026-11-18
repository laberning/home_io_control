#pragma once

/// @file proto_heating.h
/// @brief Pure codec for the private register protocol of 2W heaters (CMD_WRITE_PRIVATE 0x20).
/// @ingroup hioc_protocol
///
/// A heater with an adjustable setpoint (node type 52, subtype 1) exposes its state as 16-bit
/// registers `0x01xx`, read with `0C 60 01 <reg>` and written with `0C 61 01 <reg> <value>`; the
/// device answers with CMD_WRITE_PRIVATE_ACK (0x21) echoing the header, followed by the value. Values
/// are little-endian. An Atlantic AGILIA IO radiator answers every read and write this way
/// (tests/corpus/captures/exchange/atlantic_agilia_exchange_{status_read,setpoint_and_off_read_back,mode_read_back}.yaml,
/// pinned by corpus_heating_test.cpp).
///
/// One descriptor table (`HEATING_FUNCTIONS` in the .cpp) lists every register this component
/// uses: its access (read-only, read/write, write-only) and how its value is encoded. The same row
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

#include "proto_frame.h"
#include "proto_private_registers.h"
#include "proto_value_codecs.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>

namespace esphome {
namespace home_io_control {

/// @brief Registers and functions of a 2W heater, one row of the descriptor table each.
///
/// The `SET_*` names are the functions of the `heating_control` action; the same row is also the
/// register's read path (reading SET_TEMPERATURE reads the comfort setpoint).
enum class HeatingFunction : uint8_t {
  POWER_ON,                   ///< Register 0x012C, read: the paired-device list. Logged, never decoded.
  SET_TEMPERATURE,            ///< Register 0x0103, read/write: comfort setpoint, tenths of a degree.
  SET_MODE,                   ///< Register 0x0100, read/write: operating mode (HeatingMode).
  SET_PRESENCE,               ///< Register 0x0110, read/write: someone is in the room (1) or not (0). A controller
                              ///< writes it to tell the heater; a Sauter Hekla reports there whether its own
                              ///< detection sees someone (issue #222), 0 with nobody in the room and 1 while
                              ///< someone walks around it, with its occupancy detection on.
                              ///< \todo Confirm on another heater that the register reports detected presence.
  SET_OPEN_WINDOW_DETECTION,  ///< Register 0x0106, read/write: open-window detection on (1) or off (0).
  MIDNIGHT_SYNC,              ///< Register 0x0130, read: the comfort/eco/auto setpoint block. Logged, never decoded.
  ROOM_TEMPERATURE,           ///< Register 0x0115, read-only: measured room temperature, signed hundredths of a degree.
  REGULATOR_STATE,            ///< Register 0x0117, read-only: 0 none, 1 standby, 2 heating.
  MAX_SETPOINT,               ///< Register 0x0102, read-only: highest setpoint the heater accepts, tenths of a degree.
  WINDOW_OPEN,                ///< Register 0x010E, read-only: the heater's window sensor, 0 closed / 1 open.
  TOWEL_MODE,                 ///< Register 0x0136, read/write: a towel dryer's mode (TowelDryerMode).
  TEMPORARY_STATE,            ///< Register 0x013E, read/write: a towel dryer's temporary state (HeatingTemporaryState).
  WORKING_RATE,               ///< Register 0x0126, read-only: share of the time the element heats, 0-100 %.
  TEMPERATURE_IN_FORCE,    ///< Register 0x0137, read-only: the temperature a towel dryer holds right now, tenths of a
                           ///< degree. Only a temperature while no boost or drying runs (it reads 1 and 2 there).
                           ///< \todo Confirm on a second towel dryer (the reads so far are from one unit).
  ECO_SETBACK,             ///< Register 0x0104, read-only: how far below the comfort setpoint a towel dryer's eco
                           ///< period heats, tenths of a kelvin.
  NIGHT_LIGHT,             ///< Register 0x0144, read/write: a heater's night light on (1) or off (0).
  NIGHT_LIGHT_BRIGHTNESS,  ///< Register 0x0142, read/write: the night light's brightness, 0-10000, 16-bit.
                           ///< A write dims the light on a Sauter Hekla (issue #222).
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
  HEATER_TEMPERATURE_IN_FORCE,  ///< Register 0x012A, read-only: the temperature a heater with an adjustable setpoint
                                ///< (AGILIA, Hekla, EMOTION 3 H) holds right now, tenths of a degree. The same value
                                ///< as TEMPERATURE_IN_FORCE on the towel dryer, whose sweep read it in both.
                                ///< \todo Read on a radiator: no AGILIA, Hekla or EMOTION 3 H has answered it yet. A
                                ///<       heater that answers it empty keeps the comfort-setpoint write.
  CENTRAL_HEATING_MODE,         ///< Register 0x0153, read-only: 1 while a mixed towel dryer runs on the house's
                                ///< central heating (its electric element stays off), 0 while it uses the element.
                                ///< An electric unit is expected to answer without a value.
                                ///< \todo Experimental: read on one Atlantic Adelis mixed unit over four panel
                                ///<       toggles (issue #243), never written. Confirm on a second unit, and on
                                ///<       an electric one what the read returns.
  OCCUPANCY_DETECTION,          ///< Register 0x0101, read/write: the heater's occupancy detection on (1) or off (0).
                                ///< On a Sauter Hekla it reads 1 with the detection switched on and 0 with it off,
                                ///< and the vendor app writes it (issue #222); the detected presence shows in
                                ///< SET_PRESENCE.
                                ///< \todo Experimental except on the Hekla: confirm on another heater that it
                                ///<       answers this register.
  ENERGY_TOTAL,                 ///< Register 0x0116, read-only: the energy the heater has used, whole kilowatt-hours,
                                ///< the total its own display shows. The only register of the family whose
                                ///< 16-bit value is sent most significant byte first (every other is little-endian).
                                ///< Two Sauter Tahual radiators answered `0D 5E` (3422 kWh) and `01 08` (264 kWh),
                                ///< equal to their displays (PR #238, corpus `sauter_tahual_probe_energy_register`);
                                ///< two Thermor EMOTION 3 H answered `0C BE` and `0F A5` (issue #228) and two Atlantic
                                ///< Adelis units `02 3F` and `03 B2`, whose displays were not compared.
                                ///< \todo Experimental except on the Sauter Tahual: compare with the display on
                                ///<       an EMOTION 3 H, an Adelis and a Hekla.
  BOOST_REMAINING,              ///< Register 0x013A, read-only: the minutes left of a running boost, rounded up; one
                                ///< byte. Read on an Atlantic Adelis during a one-hour boost: `3C` (60) a moment
                                ///< after the start and `37` (55) five minutes later, matching the display's
                                ///< countdown (issue #202, corpus `atlantic_adelis_probe_boost_remaining`). A second
                                ///< unit read `1C` (28) during a boost, and `00` was read during a drying run; no
                                ///< read while nothing runs exists, so the hub reads it during a boost only.
                                ///< \todo Experimental: read on Atlantic Adelis units only; confirm on another dryer.
  BOOST_DURATION,               ///< Register 0x013B, read-only here: the boost duration set on the panel, minutes. Two
                                ///< bytes, the duration then the panel's maximum: `3C 3C` (60, 60) with a one-hour
                                ///< boost set, `0F 3C` (15) and `1E 3C` (30) after the panel was set to those, and
                                ///< `23 3C` (35, 60) on a second unit; the value changed 1-3 minutes after the panel
                                ///< did (issue #202). Only the first byte is read.
                                ///< \todo Experimental: nobody has written it, so how the dryer takes a new
                                ///<       duration is not known.
  DRYING_REMAINING,             ///< Register 0x013C, read-only: the minutes left of a running drying run; one byte,
                                ///< `1C` (28) with the display between 30 and 27 minutes left (issue #202, corpus
                                ///< `atlantic_adelis_probe_register_01_sweep_drying`), `00` while none runs.
                                ///< \todo Experimental: read on Atlantic Adelis units only; confirm on another dryer.
  DRYING_DURATION,              ///< Register 0x013D, read-only here: the duration of the last drying run that was
                                ///< started, minutes. Two bytes, the duration then the panel's maximum: `1E 78`
                                ///< (30, 120) after a run of 30 minutes. A change on the panel alone does not show
                                ///< until the next run starts (checked twice, issue #202). Only the first byte is read.
                                ///< \todo Experimental: nobody has written it, so how the dryer takes a new
                                ///<       duration is not known.
  COUNT,                        ///< Not a function: the number of functions, for storage sized by function.
};

/// @brief Operating modes of the mode register (HeatingFunction::SET_MODE).
enum class HeatingMode : uint8_t {
  AUTO = 0x00,               ///< Automatic mode; the device manages the setpoint itself.
  MANUAL = 0x01,             ///< Manual mode; follows the last written setpoint.
  PROG = 0x02,               ///< Program mode; the device runs its own stored weekly schedule.
  EXTERNAL_SETPOINT = 0x03,  ///< Setpoint comes from an external controller. Readable, never written (see
                             ///< heating_mode_is_writable()). Not named EXTERNAL: the Arduino core defines
                             ///< that as a macro.
  OFF = 0x04,                ///< Off / standby.
};

/// @brief Whether this component may write a mode to the mode register.
/// @details Every mode except EXTERNAL_SETPOINT. That mode hands the setpoint to an external controller, so
///          writing it is untested and could leave a heater waiting for a controller that is not there.
///          Reading it is fine: the climate card shows it as heat. encode_heating_write() refuses a mode
///          this rule rejects; the actions and climate profiles offer only modes it accepts.
/// @param mode Mode to check.
/// @return false for EXTERNAL_SETPOINT, true for every other mode.
constexpr bool heating_mode_is_writable(HeatingMode mode) { return mode != HeatingMode::EXTERNAL_SETPOINT; }

/// @brief Modes of a towel dryer's mode register (HeatingFunction::TOWEL_MODE).
/// @details OFF, PROG and MANUAL are the values two Atlantic Adelis units report with their display
///          showing off, PROG and MANUEL (issue #202, corpus atlantic_adelis_probe_mode_reads).
enum class TowelDryerMode : uint8_t {
  OFF = 0x00,     ///< Off (standby).
  AUTO = 0x01,    ///< Defined by the register's range, never seen on a device; never written here.
  PROG = 0x02,    ///< Program mode: the device runs its own weekly program.
  MANUAL = 0x03,  ///< Manual mode: the device holds the comfort setpoint.
};

/// @brief Temporary states of a towel dryer (HeatingFunction::TEMPORARY_STATE): a timed run on top of
/// the mode, which the mode register does not show.
enum class HeatingTemporaryState : uint8_t {
  NONE = 0x00,    ///< No temporary state: the mode applies.
  BOOST = 0x01,   ///< Boost: full power for the boost duration.
  DRYING = 0x02,  ///< Drying: full power for the drying duration.
};

/// @brief Regulator states of the regulator register (HeatingFunction::REGULATOR_STATE).
enum class HeatingRegulatorState : uint8_t {
  NONE = 0x00,     ///< No regulation active.
  STANDBY = 0x01,  ///< Regulating, the element is idle.
  HEATING = 0x02,  ///< The element is heating.
};

/// @brief Lowest setpoint this codec will encode.
///
/// The conventional frost-protection setpoint, safely representable as wire value 70.
constexpr float HEATING_TEMP_MIN_C = 7.0F;

/// @brief Highest setpoint this codec will encode.
///
/// The setpoint register is a 16-bit little-endian value in tenths of a degree; 28.0 °C (`18 01`) is
/// the ceiling Atlantic radiator manuals document and the value an AGILIA IO radiator reports in its
/// maximum setpoint register (tests/corpus/captures/exchange/atlantic_agilia_exchange_status_read.yaml).
/// No setpoint above 25.5 °C (a non-zero high byte) has been written on hardware yet.
constexpr float HEATING_TEMP_MAX_C = 28.0F;

/// @brief Payload byte at index 2: the high byte of the 16-bit register number, constant `0x01`.
constexpr uint8_t HEATING_REGISTER_HIGH_BYTE = 0x01;

/// @brief Default encoding range of a temperature main parameter, in degrees Celsius. The main
/// parameter spans TEMPERATURE_MP_SPAN steps over [min, max]; this range is the one a device
/// is assumed to use unless the YAML says otherwise (`min_temperature:` / `max_temperature:`).
static constexpr float HEATING_TEMPERATURE_RANGE_MIN_C = -40.0F;
static constexpr float HEATING_TEMPERATURE_RANGE_MAX_C = 80.0F;  ///< See HEATING_TEMPERATURE_RANGE_MIN_C.

/// @brief Highest value of the night light brightness register (HeatingFunction::NIGHT_LIGHT_BRIGHTNESS),
/// full brightness. The register runs 0-10000; a Sauter Hekla's panel sets 100, 1000, 2000 and 10000
/// for its four levels (issue #222).
constexpr uint16_t HEATING_NIGHT_LIGHT_BRIGHTNESS_MAX = 10000;

/// @brief Highest brightness percentage of a Home Assistant light.
constexpr uint8_t HEATING_NIGHT_LIGHT_PERCENT_MAX = 100;

/// @brief One knot of the night light brightness scale: a percentage and the register value it stands for.
struct NightLightScaleKnot {
  uint8_t percent;          ///< Brightness a Home Assistant light shows, 0-100 %.
  uint16_t register_value;  ///< Register value, 0-HEATING_NIGHT_LIGHT_BRIGHTNESS_MAX.
};

/// @brief The night light brightness scale, the one the heater's own vendor app uses: its slider shows
/// 1000 as 33 %, 2000 as 66 % and 10000 as 100 % (issue #222). Between two knots the scale is linear.
/// It spreads the range where the light visibly changes over two thirds of the slider: above about 3000
/// a Sauter Hekla's light hardly gets brighter (issue #222). Ascending in both fields.
constexpr NightLightScaleKnot HEATING_NIGHT_LIGHT_SCALE[] = {
    {0, 0},
    {33, 1000},
    {66, 2000},
    {HEATING_NIGHT_LIGHT_PERCENT_MAX, HEATING_NIGHT_LIGHT_BRIGHTNESS_MAX},
};

/// @brief Night light brightness register value for a percentage, on HEATING_NIGHT_LIGHT_SCALE.
/// @param percent Brightness, 0-100 %; a larger value is clamped to 100 %.
/// @return The register value, 0-HEATING_NIGHT_LIGHT_BRIGHTNESS_MAX, rounded to the nearest whole value.
constexpr uint16_t night_light_register_from_percent(uint8_t percent) {
  const unsigned p = std::min<unsigned>(percent, HEATING_NIGHT_LIGHT_PERCENT_MAX);
  for (size_t i = 1; i < std::size(HEATING_NIGHT_LIGHT_SCALE); ++i) {
    const NightLightScaleKnot &lo = HEATING_NIGHT_LIGHT_SCALE[i - 1];
    const NightLightScaleKnot &hi = HEATING_NIGHT_LIGHT_SCALE[i];
    if (p <= hi.percent) {
      const unsigned span = hi.percent - lo.percent;
      // Adding half the span before the division rounds to the nearest register value.
      const unsigned scaled = ((p - lo.percent) * (hi.register_value - lo.register_value)) + (span / 2);
      return static_cast<uint16_t>(lo.register_value + (scaled / span));
    }
  }
  return HEATING_NIGHT_LIGHT_BRIGHTNESS_MAX;
}

/// @brief Night light brightness as a fraction of full brightness (0.0-1.0) for a register value, the
/// form a Home Assistant light shows: the inverse of night_light_register_from_percent(), unrounded, so
/// the panel's lowest level 100 shows as 3.3 %.
/// @param raw Register value; one above HEATING_NIGHT_LIGHT_BRIGHTNESS_MAX is clamped to it.
constexpr float night_light_fraction_from_register(uint16_t raw) {
  const unsigned r = std::min<unsigned>(raw, HEATING_NIGHT_LIGHT_BRIGHTNESS_MAX);
  for (size_t i = 1; i < std::size(HEATING_NIGHT_LIGHT_SCALE); ++i) {
    const NightLightScaleKnot &lo = HEATING_NIGHT_LIGHT_SCALE[i - 1];
    const NightLightScaleKnot &hi = HEATING_NIGHT_LIGHT_SCALE[i];
    if (r <= hi.register_value) {
      const float slope =
          static_cast<float>(hi.percent - lo.percent) / static_cast<float>(hi.register_value - lo.register_value);
      const float percent = static_cast<float>(lo.percent) + (static_cast<float>(r - lo.register_value) * slope);
      return percent / static_cast<float>(HEATING_NIGHT_LIGHT_PERCENT_MAX);
    }
  }
  return 1.0F;
}

/// @brief Register 0x0119: the heater's model name, ASCII, not terminated ("ADELIS 2 DIGIT" on an
/// Atlantic Adelis, corpus atlantic_adelis_probe_register_01_sweep). Read raw for a log line; no
/// HeatingFunction decodes it.
constexpr uint8_t HEATING_REGISTER_MODEL_NAME = 0x19;

/// @brief Stable lowercase name for a heating function ("power_on", "set_temperature", ...).
/// @param fn Function.
/// @return Null-terminated string; "unknown" for an out-of-range value.
const char *heating_function_name(HeatingFunction fn);

/// @brief Whether the register behind a function can be written.
/// @param fn Function.
/// @return true for a read/write or write-only register; false for a read-only one or an
///         out-of-range value.
bool heating_function_is_writable(HeatingFunction fn);

/// @brief Whether the register behind a function can be read.
/// @param fn Function.
/// @return true for a read/write or read-only register; false for a write-only one or an
///         out-of-range value.
bool heating_function_is_readable(HeatingFunction fn);

/// @brief How the value of a register maps onto the bytes of its reply.
/// @param fn Function.
/// @return The register's codec; ValueCodec::NONE for a register with no typed value or an out-of-range value.
ValueCodec heating_function_codec(HeatingFunction fn);

/// @brief Low byte of the 0x01xx register behind a function.
/// @param fn Function.
/// @param[out] register_low The register's low byte; untouched when false is returned.
/// @return false for an out-of-range value.
bool heating_function_register(HeatingFunction fn, uint8_t &register_low);

/// @brief Encode a write ("set") of one register into a CMD_WRITE_PRIVATE (0x20) payload.
///
/// `value` is interpreted by the register's encoding:
///   - SET_TEMPERATURE: degrees Celsius within [HEATING_TEMP_MIN_C, HEATING_TEMP_MAX_C], encoded as
///     a 16-bit little-endian value in tenths of a degree, round-half-away-from-zero(10 * value).
///   - DEROGATED_SETPOINT: as SET_TEMPERATURE, or 0 (`00 00`) to cancel the override.
///   - SET_MODE: a HeatingMode value widened to float that heating_mode_is_writable() accepts.
///   - SET_PRESENCE, SET_OPEN_WINDOW_DETECTION, OCCUPANCY_DETECTION: 0 or 1.
///   - TOWEL_MODE: a TowelDryerMode value widened to float (0-3).
///   - TEMPORARY_STATE: a HeatingTemporaryState value widened to float (0-2).
///   - NIGHT_LIGHT: 0 or 1.
///   - NIGHT_LIGHT_BRIGHTNESS: an integer 0-HEATING_NIGHT_LIGHT_BRIGHTNESS_MAX (10000), encoded as a 16-bit
///   little-endian value.
/// A read-only register (including POWER_ON, MIDNIGHT_SYNC and the measurement registers) is never
/// encoded as a write, and out-of-range, non-integral (for enum/binary encodings) or non-finite
/// input is rejected: a byte derived from a truncating or wrapping cast is never emitted.
/// @param fn Register to write.
/// @param value Register-specific value (see above).
/// @param out Output buffer of PRIVATE_REGISTER_PAYLOAD_MAX_SIZE bytes.
/// @return Number of payload bytes written (5 or 6), or 0 on invalid input or a read-only register.
size_t encode_heating_write(HeatingFunction fn, float value, uint8_t out[PRIVATE_REGISTER_PAYLOAD_MAX_SIZE]);

/// @brief Encode a read ("get") of one register into a CMD_WRITE_PRIVATE (0x20) payload.
/// @param fn Register to read.
/// @param out Output buffer of PRIVATE_REGISTER_PAYLOAD_MAX_SIZE bytes.
/// @return 4 (the payload is always `0C 60 01 <reg>`), or 0 for a write-only register or an
///         out-of-range value.
size_t encode_heating_read(HeatingFunction fn, uint8_t out[PRIVATE_REGISTER_PAYLOAD_MAX_SIZE]);

/// @brief Encode the function the `heating_control` action runs: a read for a read-only register,
/// a write for every other.
///
/// POWER_ON and MIDNIGHT_SYNC are reads (their `value` is ignored); the measurement registers are
/// reads too; the others are writes as in encode_heating_write().
/// @param fn Function to encode.
/// @param value Function-specific value (ignored for a read).
/// @param out Output buffer of PRIVATE_REGISTER_PAYLOAD_MAX_SIZE bytes.
/// @return Number of payload bytes written (4, 5 or 6), or 0 on invalid input.
size_t encode_heating_payload(HeatingFunction fn, float value, uint8_t out[PRIVATE_REGISTER_PAYLOAD_MAX_SIZE]);

/// @brief Decode the value a heater answered a register read or write with.
///
/// Accepts a CMD_WRITE_PRIVATE_ACK (0x21) whose data starts with `0C 60 01 <reg>` or
/// `0C 61 01 <reg>` for this function's register, followed by at least the register's value bytes.
/// Anything else (another command, another register, a truncated value, a function without a typed
/// decode such as POWER_ON) is "no value".
///
/// The value is returned in natural units: degrees Celsius for the temperature registers, whole
/// kilowatt-hours for the energy total, minutes for the run timers, the raw byte for the others. A byte above its
/// register's range (a mode, a state, a percentage) is "no value". A raw `-1` on the room-temperature register is a
/// sensor fault: it decodes as NaN with `true` returned, because the heater did answer.
/// @param reply The device's reply frame.
/// @param fn Register the request addressed.
/// @param[out] value Decoded value; untouched when `false` is returned.
/// @return true when the reply carries a value for this register.
bool decode_heating_reply(const IoFrame &reply, HeatingFunction fn, float &value);

/// @brief Does a heater's reply carry the value bytes of a register, whatever they say?
/// @details True for a reply that echoes the register and holds at least the register's codec length of value bytes,
///          also when decode_heating_reply() refuses them (a mode byte above the register's range). The difference
///          tells a heater that has the register and answers it oddly from one that has none and answers it empty.
/// @param reply The device's reply frame.
/// @param fn Register the request addressed.
/// @return true when the reply holds the register's value bytes; false for an empty answer, another register or a
///         function with no typed value (POWER_ON).
bool heating_reply_carries_value(const IoFrame &reply, HeatingFunction fn);

}  // namespace home_io_control
}  // namespace esphome

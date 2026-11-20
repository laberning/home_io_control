/// @file proto_device_readings.cpp
/// @brief The named readings a device can show, the capability each needs and where each value comes from.
/// @ingroup hioc_protocol

#include "proto_device_readings.h"

#include "proto_codecs.h"
#include "proto_dhw.h"
#include "proto_heating_profiles.h"
#include "proto_sizes.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>
#include <variant>

namespace esphome {
namespace home_io_control {

namespace {

/// One callable from a lambda per source kind, for std::visit: a kind added to ReadingSource without a lambda here
/// fails to compile instead of falling through to "no value".
template<class... Fs> struct Overloaded : Fs... {
  using Fs::operator()...;
};

/// The registers of the two runs of a towel dryer, which the minutes left of "the active run" needs polled.
constexpr DeviceRegister RUN_REMAINING_REGISTERS[] = {DeviceRegister::BOOST_REMAINING,
                                                      DeviceRegister::DRYING_REMAINING};

/// Low byte of a 16-bit main parameter.
constexpr uint16_t STATUS_VALUE_LOW_BYTE_MASK = 0xFF;

/// A two-bit field of a functional parameter (a water heater's FP16 holds 0 keep, 1 on and 2 off in each).
constexpr uint16_t STATUS_FIELD_MASK = 0x3;
/// The field value that reads "on" in a water heater's operating-mode word (ADR 0081).
constexpr uint8_t STATUS_FIELD_ON = 1;

std::optional<float> remaining_time_of_active_run(const IoDevice &dev);
std::optional<float> battery_low_of(const IoDevice &dev);
const char *battery_state_value_name(uint8_t value);

/// The reading table: one row per value the hub keeps of a device, and the one place that says when a device's poll
/// reads it (`poll`, ReadingPoll) and for which devices (`feature`, else the heating profile: heating_profile_reads()).
/// The rows are in read order: within a policy the poll reads them top to bottom, so the order below is the order of
/// the registers on the wire. The first rows are the ones an entity of the YAML asks for (WHEN_ASKED), each with the
/// capability it needs, in the order of the registers' enumeration (DeviceRegister); then the registers every poll
/// reads, the ones that may miss, and the one read once; the last rows are not polled for their own sake. A towel
/// dryer's minutes left of a run are read only while that run is on (HeatingReadCondition), so an idle dryer costs no
/// read for them; the minutes left of whichever run is active are derived from the two registers, which are rows of
/// their own. The room temperature leads the registers every poll reads: it is the value a user looks at, so a poll
/// that dies half-way still delivered it. The water heater's rows (capability WATER_HEATER) follow the heater's, in
/// the order its poll reads them; the three readings that are slices of its heating register name the register once in
/// the poll.
constexpr DeviceReadingRow DEVICE_READINGS[] = {
    {.name = "occupancy",
     .source = RegisterSource{.fn = DeviceRegister::PRESENCE},
     .reading = DeviceReading::OCCUPANCY,
     .poll = ReadingPoll::WHEN_ASKED,
     .feature = DeviceFeature::OCCUPANCY},
    {.name = "central_heating_mode",
     .source = RegisterSource{.fn = DeviceRegister::CENTRAL_HEATING_MODE},
     .reading = DeviceReading::CENTRAL_HEATING_MODE,
     .poll = ReadingPoll::WHEN_ASKED,
     .feature = DeviceFeature::CENTRAL_HEATING_MODE},
    {.name = "occupancy_detection",
     .source = RegisterSource{.fn = DeviceRegister::OCCUPANCY_DETECTION},
     .reading = DeviceReading::OCCUPANCY_DETECTION,
     .poll = ReadingPoll::WHEN_ASKED,
     .feature = DeviceFeature::OCCUPANCY},
    {.name = "energy_total",
     .source = RegisterSource{.fn = DeviceRegister::ENERGY_TOTAL},
     .reading = DeviceReading::ENERGY_TOTAL,
     .poll = ReadingPoll::WHEN_ASKED,
     .feature = DeviceFeature::ENERGY_METER},
    {.name = "boost_remaining",
     .source = RegisterSource{.fn = DeviceRegister::BOOST_REMAINING,
                              .only_while = HeatingReadCondition{DeviceRegister::TEMPORARY_STATE,
                                                                 static_cast<uint8_t>(HeatingTemporaryState::BOOST)}},
     .reading = DeviceReading::BOOST_REMAINING,
     .poll = ReadingPoll::WHEN_ASKED,
     .feature = DeviceFeature::RUN_TIMERS},
    {.name = "boost_duration",
     .source = RegisterSource{.fn = DeviceRegister::BOOST_DURATION},
     .reading = DeviceReading::BOOST_DURATION,
     .poll = ReadingPoll::WHEN_ASKED,
     .feature = DeviceFeature::RUN_TIMERS},
    {.name = "drying_remaining",
     .source = RegisterSource{.fn = DeviceRegister::DRYING_REMAINING,
                              .only_while = HeatingReadCondition{DeviceRegister::TEMPORARY_STATE,
                                                                 static_cast<uint8_t>(HeatingTemporaryState::DRYING)}},
     .reading = DeviceReading::DRYING_REMAINING,
     .poll = ReadingPoll::WHEN_ASKED,
     .feature = DeviceFeature::RUN_TIMERS},
    {.name = "drying_duration",
     .source = RegisterSource{.fn = DeviceRegister::DRYING_DURATION},
     .reading = DeviceReading::DRYING_DURATION,
     .poll = ReadingPoll::WHEN_ASKED,
     .feature = DeviceFeature::RUN_TIMERS},
    {.name = "remaining_time",
     .source = DerivedSource{.value = remaining_time_of_active_run, .polls = RUN_REMAINING_REGISTERS},
     .reading = DeviceReading::REMAINING_TIME,
     .poll = ReadingPoll::WHEN_ASKED,
     .feature = DeviceFeature::RUN_TIMERS},
    {.name = "room_temperature",
     .source = RegisterSource{.fn = DeviceRegister::ROOM_TEMPERATURE},
     .reading = DeviceReading::ROOM_TEMPERATURE,
     .poll = ReadingPoll::EVERY_POLL,
     .card = CardUse::CURRENT_TEMPERATURE},
    {.name = "setpoint",
     .source = RegisterSource{.fn = DeviceRegister::COMFORT_SETPOINT},
     .reading = DeviceReading::SETPOINT,
     .poll = ReadingPoll::EVERY_POLL,
     .card = CardUse::TARGET_TEMPERATURE},
    {.name = "mode",
     .source = RegisterSource{.fn = DeviceRegister::OPERATING_MODE},
     .reading = DeviceReading::MODE,
     .poll = ReadingPoll::EVERY_POLL},
    {.name = "regulator_state",
     .source = RegisterSource{.fn = DeviceRegister::REGULATOR_STATE},
     .reading = DeviceReading::REGULATOR_STATE,
     .poll = ReadingPoll::EVERY_POLL},
    {.name = "window_open",
     .source = RegisterSource{.fn = DeviceRegister::WINDOW_OPEN},
     .reading = DeviceReading::WINDOW_OPEN,
     .poll = ReadingPoll::EVERY_POLL,
     .feature = DeviceFeature::WINDOW_COMPANIONS},
    {.name = "towel_mode",
     .source = RegisterSource{.fn = DeviceRegister::TOWEL_MODE},
     .reading = DeviceReading::TOWEL_MODE,
     .poll = ReadingPoll::EVERY_POLL},
    {.name = "temporary_state",
     .source = RegisterSource{.fn = DeviceRegister::TEMPORARY_STATE},
     .reading = DeviceReading::TEMPORARY_STATE,
     .poll = ReadingPoll::EVERY_POLL},
    {.name = "working_rate",
     .source = RegisterSource{.fn = DeviceRegister::WORKING_RATE},
     .reading = DeviceReading::WORKING_RATE,
     .poll = ReadingPoll::EVERY_POLL},
    // The heater's own controller changes the detection setting (a change from the vendor app), and a night light is
    // a part a model may lack: a miss costs a read, not the poll.
    {.name = "open_window_detection",
     .source = RegisterSource{.fn = DeviceRegister::OPEN_WINDOW_DETECTION},
     .reading = DeviceReading::OPEN_WINDOW_DETECTION,
     .poll = ReadingPoll::EVERY_POLL_MAY_MISS,
     .feature = DeviceFeature::WINDOW_COMPANIONS},
    {.name = "night_light",
     .source = RegisterSource{.fn = DeviceRegister::NIGHT_LIGHT},
     .reading = DeviceReading::NIGHT_LIGHT,
     .poll = ReadingPoll::EVERY_POLL_MAY_MISS,
     .feature = DeviceFeature::NIGHT_LIGHT},
    {.name = "night_light_brightness",
     .source = RegisterSource{.fn = DeviceRegister::NIGHT_LIGHT_BRIGHTNESS},
     .reading = DeviceReading::NIGHT_LIGHT_BRIGHTNESS,
     .poll = ReadingPoll::EVERY_POLL_MAY_MISS,
     .feature = DeviceFeature::NIGHT_LIGHT},
    {.name = "max_setpoint",
     .source = RegisterSource{.fn = DeviceRegister::MAX_SETPOINT},
     .reading = DeviceReading::MAX_SETPOINT,
     .poll = ReadingPoll::ONCE_AFTER_BOOT,
     .card = CardUse::TARGET_TEMPERATURE},
    // The temperatures in force are read by the programme rule (only while the programme runs), and the derogated
    // setpoint by a write's read-back.
    {.name = "towel_temperature_in_force",
     .source = RegisterSource{.fn = DeviceRegister::TOWEL_TEMPERATURE_IN_FORCE},
     .reading = DeviceReading::TOWEL_TEMPERATURE_IN_FORCE,
     .poll = ReadingPoll::NOT_POLLED},
    {.name = "temperature_in_force",
     .source = RegisterSource{.fn = DeviceRegister::TEMPERATURE_IN_FORCE},
     .reading = DeviceReading::TEMPERATURE_IN_FORCE,
     .poll = ReadingPoll::NOT_POLLED},
    {.name = "derogated_setpoint",
     .source = RegisterSource{.fn = DeviceRegister::DEROGATED_SETPOINT},
     .reading = DeviceReading::DEROGATED_SETPOINT,
     .poll = ReadingPoll::NOT_POLLED},
    // The status read answers these two.
    {.name = "heating_level",
     .source = StatusMainParameterSource{.form = StatusValueForm::LEVEL},
     .reading = DeviceReading::HEATING_LEVEL,
     .poll = ReadingPoll::NOT_POLLED,
     .feature = DeviceFeature::REPORTS_HEATING_LEVEL,
     .value_name = heating_level_name},
    {.name = "status_setpoint",
     .source = StatusMainParameterSource{.form = StatusValueForm::TEMPERATURE},
     .reading = DeviceReading::STATUS_SETPOINT,
     .poll = ReadingPoll::NOT_POLLED},
    // A water heater (issue #198): its registers in the order the poll reads them, then the readings its status read
    // answers. The three heating readings are bytes of one register: byte 0 is on while the heater heats at all, byte 2
    // while the element runs (during a boost only) and bytes 3 and 4 while the heat pump runs (always set together).
    {.name = "hot_water_temperature",
     .source = RegisterSource{.fn = DeviceRegister::DHW_WATER_TEMPERATURE},
     .reading = DeviceReading::HOT_WATER_TEMPERATURE,
     .poll = ReadingPoll::EVERY_POLL,
     .feature = DeviceFeature::HOT_WATER_REGISTERS_CALYPSO},
    {.name = "hot_water_mode",
     .source = RegisterSource{.fn = DeviceRegister::DHW_MODE},
     .reading = DeviceReading::HOT_WATER_MODE,
     .poll = ReadingPoll::EVERY_POLL,
     .feature = DeviceFeature::HOT_WATER_REGISTERS_CALYPSO,
     .value_name = dhw_mode_name,
     .write = ValueWrite{RegisterWrite{DeviceRegister::DHW_MODE}}},
    // The target is the main parameter of the status reply, over the range of the heater's model: the row a device's
    // capability selects holds it, and the write encodes over the same range. This row is the Atlantic Calypso's.
    {.name = "hot_water_target",
     .source = StatusMainParameterSource{.form = StatusValueForm::TEMPERATURE,
                                         .range = ValueRange{DHW_TARGET_MIN_C, DHW_TARGET_MAX_C}},
     .reading = DeviceReading::HOT_WATER_TARGET,
     .poll = ReadingPoll::NOT_POLLED,
     .feature = DeviceFeature::HOT_WATER_TARGET_20_62,
     .write = ValueWrite{MainParameterWrite{}}},
    {.name = "hot_water_heating",
     .source = RegisterSource{.fn = DeviceRegister::DHW_HEATING,
                              .part = RegisterPart{.first = 0, .length = 1, .codec = ValueCodec::ANY_NONZERO}},
     .reading = DeviceReading::HOT_WATER_HEATING,
     .poll = ReadingPoll::EVERY_POLL,
     .feature = DeviceFeature::HOT_WATER_REGISTERS_CALYPSO},
    {.name = "hot_water_electric_backup",
     .source = RegisterSource{.fn = DeviceRegister::DHW_HEATING,
                              .part = RegisterPart{.first = 2, .length = 1, .codec = ValueCodec::ANY_NONZERO}},
     .reading = DeviceReading::HOT_WATER_ELECTRIC_BACKUP,
     .poll = ReadingPoll::EVERY_POLL,
     .feature = DeviceFeature::HOT_WATER_REGISTERS_CALYPSO},
    {.name = "hot_water_heat_pump",
     .source = RegisterSource{.fn = DeviceRegister::DHW_HEATING,
                              .part = RegisterPart{.first = 3, .length = 2, .codec = ValueCodec::ANY_NONZERO}},
     .reading = DeviceReading::HOT_WATER_HEAT_PUMP,
     .poll = ReadingPoll::EVERY_POLL,
     .feature = DeviceFeature::HOT_WATER_REGISTERS_CALYPSO},
    {.name = "hot_water_boost_duration",
     .source = RegisterSource{.fn = DeviceRegister::DHW_BOOST_DURATION},
     .reading = DeviceReading::HOT_WATER_BOOST_DURATION,
     .poll = ReadingPoll::EVERY_POLL,
     .feature = DeviceFeature::HOT_WATER_REGISTERS_CALYPSO},
    {.name = "hot_water_away_duration",
     .source = RegisterSource{.fn = DeviceRegister::DHW_AWAY_DURATION},
     .reading = DeviceReading::HOT_WATER_AWAY_DURATION,
     .poll = ReadingPoll::EVERY_POLL,
     .feature = DeviceFeature::HOT_WATER_REGISTERS_CALYPSO},
    {.name = "hot_water_energy",
     .source = RegisterSource{.fn = DeviceRegister::DHW_ENERGY},
     .reading = DeviceReading::HOT_WATER_ENERGY,
     .poll = ReadingPoll::EVERY_POLL,
     .feature = DeviceFeature::HOT_WATER_REGISTERS_CALYPSO},
    {.name = "hot_water_brand",
     .source = RegisterSource{.fn = DeviceRegister::DHW_BRAND},
     .reading = DeviceReading::HOT_WATER_BRAND,
     .poll = ReadingPoll::ONCE_AFTER_BOOT,
     .feature = DeviceFeature::HOT_WATER_REGISTERS_CALYPSO},
    {.name = "hot_water_capabilities",
     .source = StatusFunctionalParameterSource{.fp = DHW_FP_CAPABILITIES, .form = StatusParameterForm::WORD},
     .reading = DeviceReading::HOT_WATER_CAPABILITIES,
     .poll = ReadingPoll::NOT_POLLED,
     .feature = DeviceFeature::HOT_WATER_BOOST_FP16},
    // Boost and away are two fields of the operating-mode word, FP16, on the Atlantic Calypso: one EXECUTE sets the
    // whole word (the off word ends both), FP15 says which the heater supports, and the duration register is written
    // first (ADR 0081, ADR 0110). The words and durations are the heater's row data.
    {.name = "hot_water_boost",
     .source = StatusFunctionalParameterSource{.fp = DHW_FP_OPERATING_MODE,
                                               .form = StatusParameterForm::FIELD_EQUALS,
                                               .shift = DHW_FP16_BOOST_SHIFT,
                                               .equals = STATUS_FIELD_ON},
     .reading = DeviceReading::HOT_WATER_BOOST,
     .poll = ReadingPoll::NOT_POLLED,
     .feature = DeviceFeature::HOT_WATER_BOOST_FP16,
     .write = ValueWrite{.target = FunctionalParameterWrite{.fp = DHW_FP_OPERATING_MODE,
                                                            .on_word = DHW_OPERATING_MODE_BOOST_ON,
                                                            .off_word = DHW_OPERATING_MODE_OFF},
                         .guard = WriteGuard{.reading = DeviceReading::HOT_WATER_CAPABILITIES,
                                             .test = WriteGuardTest::HAS_BIT,
                                             .value = DHW_CAPABILITY_BOOST},
                         .before = FixedRegisterWrite{.fn = DeviceRegister::DHW_BOOST_DURATION,
                                                      .value = DHW_BOOST_START_DURATION}}},
    {.name = "hot_water_away",
     .source = StatusFunctionalParameterSource{.fp = DHW_FP_OPERATING_MODE,
                                               .form = StatusParameterForm::FIELD_EQUALS,
                                               .shift = DHW_FP16_AWAY_SHIFT,
                                               .equals = STATUS_FIELD_ON},
     .reading = DeviceReading::HOT_WATER_AWAY,
     .poll = ReadingPoll::NOT_POLLED,
     .feature = DeviceFeature::HOT_WATER_AWAY_FP16,
     .write = ValueWrite{.target = FunctionalParameterWrite{.fp = DHW_FP_OPERATING_MODE,
                                                            .on_word = DHW_OPERATING_MODE_AWAY_ON,
                                                            .off_word = DHW_OPERATING_MODE_OFF},
                         .guard = WriteGuard{.reading = DeviceReading::HOT_WATER_CAPABILITIES,
                                             .test = WriteGuardTest::HAS_BIT,
                                             .value = DHW_CAPABILITY_AWAY},
                         .before = FixedRegisterWrite{.fn = DeviceRegister::DHW_AWAY_DURATION,
                                                      .value = DHW_AWAY_UNTIL_TURNED_OFF}}},
    // A cover's readings. A YAML key of the cover declares each (the poll interval of the three read on a schedule,
    // `battery:` for the two of the status reply), so none needs a capability of the kind. The three read on a schedule
    // drop their value after repeated misses.
    {.name = "rain",
     .source = LimitationSource{.type = LimitationType::MINIMUM},
     .reading = DeviceReading::RAIN,
     .poll = ReadingPoll::NOT_POLLED,
     .stale = ReadingStale::DROP},
    {.name = "sun_level",
     .source = EventSource{.event = EVENT_LOCAL_SENSOR,
                           .data_length = LOCAL_SENSOR_EVENT_DATA_LEN,
                           .type_byte = LOCAL_SENSOR_TYPE_SUN,
                           .value_offset = LOCAL_SENSOR_VALUE_OFFSET,
                           .form = {.codec = ValueCodec::U16_BE},
                           .factor = SUN_VALUE_LUX_PER_STEP},
     .reading = DeviceReading::SUN_LEVEL,
     .poll = ReadingPoll::NOT_POLLED,
     .stale = ReadingStale::DROP},
    {.name = "battery_level",
     .source = EventSource{.event = EVENT_BATTERY_LEVEL_CHANGE,
                           .data_length = BATTERY_LEVEL_EVENT_DATA_LEN,
                           .type_byte = std::nullopt,
                           .value_offset = 0,
                           .form = {.codec = ValueCodec::BOUNDED_BYTE, .max_value = BATTERY_LEVEL_MAX_PERCENT}},
     .reading = DeviceReading::BATTERY_LEVEL,
     .poll = ReadingPoll::NOT_POLLED,
     .stale = ReadingStale::DROP},
    {.name = "battery_state",
     .source = StatusBatterySource{},
     .reading = DeviceReading::BATTERY_STATE,
     .poll = ReadingPoll::NOT_POLLED,
     .value_name = battery_state_value_name},
    {.name = "battery_low",
     .source = DerivedSource{.value = battery_low_of, .polls = {}, .codec = ValueCodec::BINARY},
     .reading = DeviceReading::BATTERY_LOW,
     .poll = ReadingPoll::NOT_POLLED},
    // An Atlantic heat pump's energy counters (issue #244): one entry of its data object each, a marker byte and a
    // big-endian count of kWh (`80 24 AC` is 9388), `81 FF FF` when the entry has no value. A counter only grows, so
    // it keeps its value through misses.
    {.name = "heat_pump_heating_energy",
     .source = DataObjectSource{.recipe = TRANSFER_RECIPE_ATLANTIC_HEAT_PUMP,
                                .offset = 0x0023,
                                .first = 0,
                                .length = 3,
                                .codec = ValueCodec::MARKED_U16_BE,
                                .max_value = 0},
     .reading = DeviceReading::HEAT_PUMP_HEATING_ENERGY,
     .poll = ReadingPoll::WHEN_ASKED,
     .feature = DeviceFeature::HEAT_PUMP_ENERGY,
     .stale = ReadingStale::KEEP},
    {.name = "heat_pump_hot_water_energy",
     .source = DataObjectSource{.recipe = TRANSFER_RECIPE_ATLANTIC_HEAT_PUMP,
                                .offset = 0x0025,
                                .first = 0,
                                .length = 3,
                                .codec = ValueCodec::MARKED_U16_BE,
                                .max_value = 0},
     .reading = DeviceReading::HEAT_PUMP_HOT_WATER_ENERGY,
     .poll = ReadingPoll::WHEN_ASKED,
     .feature = DeviceFeature::HEAT_PUMP_ENERGY,
     .stale = ReadingStale::KEEP},
    // The heat pump's outdoor temperature (issue #244): the marker byte, then signed tenths of a degree (`80 00 77` is
    // 11.9). A measurement, so it is dropped after repeated misses rather than shown stale.
    {.name = "heat_pump_outdoor_temperature",
     .source = DataObjectSource{.recipe = TRANSFER_RECIPE_ATLANTIC_HEAT_PUMP,
                                .offset = 0x019B,
                                .first = 0,
                                .length = 3,
                                .codec = ValueCodec::MARKED_TENTHS_S16_BE,
                                .max_value = 0},
     .reading = DeviceReading::HEAT_PUMP_OUTDOOR_TEMPERATURE,
     .poll = ReadingPoll::WHEN_ASKED,
     .feature = DeviceFeature::HEAT_PUMP_OUTDOOR_TEMPERATURE,
     .stale = ReadingStale::DROP},
};

/// The rows every lookup walks: DEVICE_READINGS, or the rows of a ScopedReadingTable.
// The seam of ScopedReadingTable: a test gives a device rows production does not have, and production never changes it.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): the seam named above
std::span<const DeviceReadingRow> active_table = DEVICE_READINGS;

/// Run `visit` on every row whose source is the register `fn`, whole or a part of it, in table order.
template<class Visit> void for_each_register_reading(DeviceRegister fn, Visit visit) {
  for (const DeviceReadingRow &row : active_table) {
    const auto *source = std::get_if<RegisterSource>(&row.source);
    if (source != nullptr && source->fn == fn)
      visit(row, *source);
  }
}

/// The value a register's reply gives a reading: its slice by the part's codec, or the whole value by the register's.
std::optional<float> decode_register_source(const RegisterSource &source, std::span<const uint8_t> value_bytes) {
  if (!source.part)
    return decode_register_value(source.fn, value_bytes);
  const RegisterPart &part = *source.part;
  // A slice the reply does not hold fully is no value: a cut-off reply says nothing of its last bytes.
  if (static_cast<size_t>(part.first) + part.length > value_bytes.size())
    return std::nullopt;
  return decode_value(part.codec, value_bytes.subspan(part.first, part.length));
}

/// @brief The minutes left of the timed run a towel dryer is in.
/// @details Follows the temporary state (DeviceRegister::TEMPORARY_STATE): the register of the run it shows, as
///          the rows with a condition on that state name it, or 0 with no run active. Boost and drying never run
///          together, since one register holds the state.
std::optional<float> remaining_time_of_active_run(const IoDevice &dev) {
  const int state = register_wire(dev, DeviceRegister::TEMPORARY_STATE);
  if (state < 0)
    return std::nullopt;
  for (const DeviceReadingRow &row : active_table) {
    const auto *source = std::get_if<RegisterSource>(&row.source);
    if (source != nullptr && source->only_while && source->only_while->fn == DeviceRegister::TEMPORARY_STATE &&
        std::cmp_equal(source->only_while->value, state))
      return register_value(dev, source->fn);
  }
  return 0.0F;
}

/// @brief The name of each stored value of the battery state; NOT_REPORTED is never stored (the reading is absent).
const char *battery_state_value_name(uint8_t value) {
  if (value == static_cast<uint8_t>(BatteryState::NOT_REPORTED) ||
      value > static_cast<uint8_t>(BatteryState::FULL_CHARGE))
    return nullptr;
  return battery_state_name(static_cast<BatteryState>(value));
}

/// @brief 1 while the device reports a low battery, 0 while it reports normal or full, none while it reports nothing.
std::optional<float> battery_low_of(const IoDevice &dev) {
  switch (battery_state_of(dev)) {
    case BatteryState::LOW_CHARGE:
      return 1.0F;
    case BatteryState::NORMAL_CHARGE:
    case BatteryState::FULL_CHARGE:
      return 0.0F;
    case BatteryState::NOT_REPORTED:
      break;
  }
  return std::nullopt;
}

}  // namespace

namespace {

/// The codec a reading's value is read with: a register's or its part's, a data object's, a flag's for a field of a
/// status parameter. NONE for a value that is worked out or a main parameter, which are numbers.
ValueCodec reading_codec(const DeviceReadingRow &row) {
  return std::visit(Overloaded{
                        [](const RegisterSource &source) {
                          return source.part ? source.part->codec : device_register_codec(source.fn);
                        },
                        [](const DataObjectSource &source) { return source.codec; },
                        [](const DerivedSource &source) { return source.codec; },
                        [](const StatusMainParameterSource &) { return ValueCodec::NONE; },
                        // A field of a functional parameter is a flag, and the whole word a number.
                        [](const StatusFunctionalParameterSource &source) {
                          return source.form == StatusParameterForm::FIELD_EQUALS ? ValueCodec::BINARY
                                                                                  : ValueCodec::NONE;
                        },
                        [](const EventSource &source) { return source.form.codec; },
                        // A limitation reply is the rain flag.
                        [](const LimitationSource &) { return ValueCodec::BINARY; },
                        [](const StatusBatterySource &) { return ValueCodec::NONE; },
                    },
                    row.source);
}

}  // namespace

ReadingValueKind reading_value_kind(const DeviceReadingRow &row) {
  if (row.value_name != nullptr)
    return ReadingValueKind::NAMED;
  const ValueCodec codec = reading_codec(row);
  return codec == ValueCodec::BINARY || codec == ValueCodec::ANY_NONZERO ? ReadingValueKind::FLAG
                                                                         : ReadingValueKind::NUMBER;
}

bool reading_logs_unchanged(const DeviceReadingRow &row) { return reading_value_kind(row) != ReadingValueKind::FLAG; }

const DeviceReadingRow *find_device_reading(DeviceReading reading, std::span<const DeviceReadingRow> table) {
  const auto row = std::ranges::find(table, reading, &DeviceReadingRow::reading);
  return row != table.end() ? &*row : nullptr;
}

const DeviceReadingRow *find_device_reading(DeviceReading reading) {
  return find_device_reading(reading, active_table);
}

std::span<const DeviceReadingRow> device_readings() { return active_table; }

size_t device_reading_row_matches(const IoDevice &dev, DeviceReading reading, std::span<const DeviceReadingRow> table) {
  const auto rows = static_cast<size_t>(std::ranges::count(table, reading, &DeviceReadingRow::reading));
  // A reading with one row has it, whatever the device: the row's capability only says whether a device may have it.
  if (rows <= 1)
    return rows;
  return static_cast<size_t>(std::ranges::count_if(table, [&dev, reading](const DeviceReadingRow &row) {
    return row.reading == reading && device_can_have_reading(dev, row);
  }));
}

const DeviceReadingRow *device_reading_row(const IoDevice &dev, DeviceReading reading,
                                           std::span<const DeviceReadingRow> table) {
  if (device_reading_row_matches(dev, reading, table) != 1)
    return nullptr;
  const auto row = std::ranges::find_if(table, [&dev, reading](const DeviceReadingRow &candidate) {
    return candidate.reading == reading && device_can_have_reading(dev, candidate);
  });
  // A reading with one row whose capability the device lacks has no match above, so the single row is taken here.
  return row != table.end() ? &*row : find_device_reading(reading, table);
}

const DeviceReadingRow *device_reading_row(const IoDevice &dev, DeviceReading reading) {
  return device_reading_row(dev, reading, active_table);
}

std::optional<float> reading_value(const IoDevice &dev, DeviceReading reading,
                                   std::span<const DeviceReadingRow> table) {
  const DeviceReadingRow *row = find_device_reading(reading, table);
  if (row == nullptr)
    return std::nullopt;
  // Only a derived reading is worked out; every other kind is the value its source stored.
  const auto stored = [&dev, reading] { return dev.reading_values.value(reading_slot(reading)); };
  return std::visit(Overloaded{
                        [&stored](const RegisterSource &) { return stored(); },
                        [&stored](const StatusMainParameterSource &) { return stored(); },
                        [&stored](const DataObjectSource &) { return stored(); },
                        [&stored](const StatusFunctionalParameterSource &) { return stored(); },
                        [&stored](const EventSource &) { return stored(); },
                        [&stored](const LimitationSource &) { return stored(); },
                        [&stored](const StatusBatterySource &) { return stored(); },
                        [&dev](const DerivedSource &source) { return source.value(dev); },
                    },
                    row->source);
}

std::optional<float> reading_value(const IoDevice &dev, DeviceReading reading) {
  return reading_value(dev, reading, active_table);
}

std::optional<float> decode_data_object_reading(const DataObjectSource &source, std::span<const uint8_t> data) {
  // A slice the data cuts off is no value: a shortened one could decode as a different number.
  if (source.first + source.length > data.size())
    return std::nullopt;
  return decode_value(source.codec, data.subspan(source.first, source.length), source.max_value);
}

std::optional<float> decode_event_reading(const EventSource &source, const IoFrame &frame) {
  StatusUpdateEvent event;
  if (!decode_status_update_event(frame, event) || event.event_id != source.event ||
      event.data_len != source.data_length)
    return std::nullopt;
  const std::span<const uint8_t> data(frame.data + event.data_offset, event.data_len);
  if (source.type_byte && (data.empty() || data.front() != *source.type_byte))
    return std::nullopt;
  if (source.value_offset >= data.size())
    return std::nullopt;
  const std::optional<float> decoded = decode_value(source.form, data.subspan(source.value_offset));
  // The sun level's product (at most 65535 * 100) stays below 2^24, so the float is exact.
  return decoded ? std::optional<float>(*decoded * static_cast<float>(source.factor)) : std::nullopt;
}

BatteryState battery_state_of(const IoDevice &dev) {
  const int value = reading_wire(dev, DeviceReading::BATTERY_STATE);
  return value < 0 ? BatteryState::NOT_REPORTED : static_cast<BatteryState>(value);
}

std::optional<float> decode_status_reading(DeviceReading reading, std::span<const uint8_t> main_parameter,
                                           uint8_t level_prefix, float range_min_c, float range_max_c) {
  const DeviceReadingRow *row = find_device_reading(reading);
  return row != nullptr ? decode_status_reading(*row, main_parameter, level_prefix, range_min_c, range_max_c)
                        : std::nullopt;
}

std::optional<float> decode_status_reading(const DeviceReadingRow &row, std::span<const uint8_t> main_parameter,
                                           uint8_t level_prefix, float range_min_c, float range_max_c) {
  const auto *source = std::get_if<StatusMainParameterSource>(&row.source);
  const std::optional<float> decoded = decode_value(ValueCodec::U16_BE, main_parameter);
  if (source == nullptr || !decoded)
    return std::nullopt;
  // A row with a range of its own is encoded over it, not over the device's.
  if (source->range) {
    range_min_c = source->range->min;
    range_max_c = source->range->max;
  }
  const auto word = static_cast<uint16_t>(*decoded);
  switch (source->form) {
    case StatusValueForm::LEVEL:
      if ((word >> BITS_PER_BYTE) != level_prefix)
        return std::nullopt;
      return static_cast<float>(word & STATUS_VALUE_LOW_BYTE_MASK);
    case StatusValueForm::TEMPERATURE: {
      float celsius = 0.0F;
      if (!decode_temperature_main_parameter(word, range_min_c, range_max_c, celsius))
        return std::nullopt;
      // Rounded to a tenth for display, but never past the end of the range the device encodes over: an end that is
      // no whole tenth would otherwise round outside the range and a write of that setpoint would be refused.
      return std::clamp(std::round(celsius * VALUE_TENTHS_PER_UNIT) / VALUE_TENTHS_PER_UNIT, range_min_c, range_max_c);
    }
  }
  return std::nullopt;
}

float decode_status_parameter(const StatusFunctionalParameterSource &source, uint16_t word) {
  if (source.form == StatusParameterForm::FIELD_EQUALS)
    return ((word >> source.shift) & STATUS_FIELD_MASK) == source.equals ? 1.0F : 0.0F;
  return static_cast<float>(word);
}

size_t store_status_parameter_readings(IoDevice &dev, const IoFrame &reply) {
  size_t stored = 0;
  for (const DeviceReadingRow &row : active_table) {
    const auto *source = std::get_if<StatusFunctionalParameterSource>(&row.source);
    uint16_t word = 0;
    if (source == nullptr || !decode_status_functional_parameter(reply, source->fp, word))
      continue;
    store_reading(dev, row.reading, decode_status_parameter(*source, word));
    stored++;
  }
  return stored;
}

std::span<const DeviceRegister> reading_polled_registers(const DeviceReadingRow &row) {
  return std::visit(Overloaded{
                        [](const RegisterSource &source) { return std::span<const DeviceRegister>{&source.fn, 1}; },
                        [](const DerivedSource &source) { return source.polls; },
                        [](const DataObjectSource &) { return std::span<const DeviceRegister>{}; },
                        [](const StatusMainParameterSource &) { return std::span<const DeviceRegister>{}; },
                        [](const StatusFunctionalParameterSource &) { return std::span<const DeviceRegister>{}; },
                        [](const EventSource &) { return std::span<const DeviceRegister>{}; },
                        [](const LimitationSource &) { return std::span<const DeviceRegister>{}; },
                        [](const StatusBatterySource &) { return std::span<const DeviceRegister>{}; },
                    },
                    row.source);
}

std::optional<DeviceRegister> reading_register(DeviceReading reading) {
  const DeviceReadingRow *row = find_device_reading(reading);
  if (row == nullptr)
    return std::nullopt;
  if (const auto *source = std::get_if<RegisterSource>(&row->source))
    return source->fn;
  return std::nullopt;
}

const DeviceReadingRow *find_register_reading(DeviceRegister fn) {
  for (const DeviceReadingRow &row : active_table) {
    const auto *source = std::get_if<RegisterSource>(&row.source);
    if (source != nullptr && source->fn == fn)
      return &row;
  }
  return nullptr;
}

std::optional<float> register_value(const IoDevice &dev, DeviceRegister fn) {
  const DeviceReadingRow *row = find_register_reading(fn);
  return row != nullptr ? dev.reading_values.value(reading_slot(row->reading)) : std::nullopt;
}

bool reading_is_scheduled(const DeviceReadingRow &row) {
  return std::holds_alternative<DataObjectSource>(row.source) || std::holds_alternative<EventSource>(row.source) ||
         std::holds_alternative<LimitationSource>(row.source);
}

bool reading_is_from_status_reply(const DeviceReadingRow &row) {
  return std::holds_alternative<StatusMainParameterSource>(row.source) ||
         std::holds_alternative<StatusFunctionalParameterSource>(row.source) ||
         std::holds_alternative<StatusBatterySource>(row.source);
}

bool device_can_have_reading(const IoDevice &dev, const DeviceReadingRow &row) {
  return !row.feature.has_value() || has_feature(dev.features, *row.feature);
}

bool device_has_reading_capability(const IoDevice &dev, const DeviceReadingRow &row) {
  return row.feature.has_value() && has_feature(dev.features, *row.feature);
}

int reading_wire(const IoDevice &dev, DeviceReading reading) {
  const std::optional<float> value = reading_value(dev, reading);
  // A NaN (a faulty sensor) has no whole number, and converting it would be undefined.
  return value && !std::isnan(*value) ? static_cast<int>(*value) : -1;
}

int register_wire(const IoDevice &dev, DeviceRegister fn) {
  const DeviceReadingRow *row = find_register_reading(fn);
  return row != nullptr ? reading_wire(dev, row->reading) : -1;
}

int register_wire(const IoDevice &dev, std::optional<DeviceRegister> fn) { return fn ? register_wire(dev, *fn) : -1; }

void store_reading(IoDevice &dev, DeviceReading reading, float value) {
  dev.reading_values.store(reading_slot(reading), value);
}

void clear_reading(IoDevice &dev, DeviceReading reading) { dev.reading_values.clear(reading_slot(reading)); }

void mark_reading_absent(IoDevice &dev, DeviceReading reading) {
  dev.reading_values.mark_absent(reading_slot(reading));
}

bool reading_absent(const IoDevice &dev, DeviceReading reading) {
  return dev.reading_values.absent(reading_slot(reading));
}

bool reading_reported(const IoDevice &dev, DeviceReading reading) {
  return dev.reading_values.reported(reading_slot(reading));
}

void ask_reading(IoDevice &dev, DeviceReading reading) { dev.reading_values.ask(reading_slot(reading)); }

bool reading_asked(const IoDevice &dev, DeviceReading reading) {
  return dev.reading_values.asked(reading_slot(reading));
}

StatusStore store_status_main_parameter(IoDevice &dev, DeviceReading reading, std::span<const uint8_t> main_parameter,
                                        uint8_t level_prefix) {
  const DeviceReadingRow *row = device_reading_row(dev, reading);
  if (row == nullptr)
    return {};
  const HeatingCardRange range = device_temperature_range(dev);
  const std::optional<float> value =
      decode_status_reading(*row, main_parameter, level_prefix, range.min_c, range.max_c);
  if (!value)
    return {};
  const std::optional<float> before = reading_value(dev, reading);
  store_reading(dev, reading, *value);
  return {value, !before || *before != *value};
}

namespace {

/// Two values are the same step when they differ by less than this: no step of a target is finer than a thousandth of a
/// degree.
constexpr float STEP_MATCH_TOLERANCE = 0.001F;

/// @brief The steps of a stepped target: the knots of a scaled register, or an even grid over a main-parameter range.
struct TargetSteps {
  // NOLINTBEGIN(readability-redundant-member-init): g++'s -Wmissing-field-initializers wants the member initializer
  // before a designated initializer may leave the field out.
  std::span<const ValueScaleKnot> knots{};  ///< The steps of a register target, in natural units.
  // NOLINTEND(readability-redundant-member-init)
  float first{0.0F};    ///< Grid: the lowest step.
  float last{0.0F};     ///< Grid: the highest step.
  float spacing{0.0F};  ///< Grid: the distance between two steps; 0 for a knot list.

  /// @return The lowest step.
  [[nodiscard]] float lowest() const {
    return this->knots.empty() ? this->first : static_cast<float>(this->knots.front().natural);
  }
  /// @return The highest step.
  [[nodiscard]] float highest() const {
    return this->knots.empty() ? this->last : static_cast<float>(this->knots.back().natural);
  }
  /// @return The highest step that does not exceed `value` (`value` lies from the lowest to the highest step).
  [[nodiscard]] float at_or_below(float value) const {
    if (this->knots.empty())
      return this->first + (std::floor(((value - this->first) / this->spacing) + STEP_MATCH_TOLERANCE) * this->spacing);
    auto below = static_cast<float>(this->knots.front().natural);
    for (const ValueScaleKnot &knot : this->knots) {
      if (static_cast<float>(knot.natural) <= value + STEP_MATCH_TOLERANCE)
        below = static_cast<float>(knot.natural);
    }
    return below;
  }
  /// @return The lowest step that is not below `value` (`value` lies from the lowest to the highest step).
  [[nodiscard]] float at_or_above(float value) const {
    const float below = this->at_or_below(value);
    if (std::fabs(below - value) < STEP_MATCH_TOLERANCE)
      return below;
    if (this->knots.empty())
      return below + this->spacing;
    for (const ValueScaleKnot &knot : this->knots) {
      if (static_cast<float>(knot.natural) > below)
        return static_cast<float>(knot.natural);
    }
    return below;
  }
};

/// The steps a write side's target takes, or nullopt when it takes any value.
std::optional<TargetSteps> target_steps(const DeviceReadingRow &row, const ValueWrite &write) {
  if (const auto *target = std::get_if<RegisterWrite>(&write.target)) {
    const std::optional<ValueForm> form = device_register_form(target->fn);
    if (!form || form->codec != ValueCodec::SCALED_BYTE || form->scale.size() < 2)
      return std::nullopt;
    return TargetSteps{.knots = form->scale};
  }
  const auto *source = std::get_if<StatusMainParameterSource>(&row.source);
  if (std::holds_alternative<MainParameterWrite>(write.target) && source != nullptr && source->range &&
      write.rounding.step && *write.rounding.step > 0.0F)
    return TargetSteps{.first = source->range->min, .last = source->range->max, .spacing = *write.rounding.step};
  return std::nullopt;
}

/// What a value becomes under the rounding policy, or the refusal that stops it.
/// @param steps The steps of the target.
/// @param rounding The policy.
/// @param value The value asked for.
/// @param[out] rounded The value to write.
/// @return NONE when `rounded` is to be written, else the reason it is not.
ValueWriteRefusal round_to_step(const TargetSteps &steps, RoundingPolicy policy, float value, float &rounded) {
  if (!std::isfinite(value))
    return ValueWriteRefusal::UNENCODABLE;
  if (value < steps.lowest() - STEP_MATCH_TOLERANCE || value > steps.highest() + STEP_MATCH_TOLERANCE)
    return ValueWriteRefusal::OUT_OF_RANGE;
  const float below = steps.at_or_below(value);
  const float above = steps.at_or_above(value);
  if (std::fabs(below - value) < STEP_MATCH_TOLERANCE) {
    rounded = below;
    return ValueWriteRefusal::NONE;
  }
  switch (policy) {
    case RoundingPolicy::EXACT:
      return ValueWriteRefusal::OFF_STEP;
    case RoundingPolicy::DOWN:
      rounded = below;
      break;
    case RoundingPolicy::UP:
      rounded = above;
      break;
    case RoundingPolicy::NEAREST:
      // Halfway between two steps the higher one, as std::round() takes a half.
      rounded = (value - below) < (above - value) ? below : above;
      break;
  }
  return ValueWriteRefusal::NONE;
}

/// Whether the device's reading lets the guard's write through: a reading that has not been read does.
bool guard_allows(const IoDevice &dev, const WriteGuard &guard) {
  const int wire = reading_wire(dev, guard.reading);
  if (wire < 0)
    return true;
  switch (guard.test) {
    case WriteGuardTest::EQUALS:
      return std::cmp_equal(wire, guard.value);
    case WriteGuardTest::HAS_BIT:
      return (static_cast<uint16_t>(wire) & guard.value) == guard.value;
  }
  return false;
}

}  // namespace

ValueWritePlan plan_value_write(const IoDevice &dev, const DeviceReadingRow &row, float value) {
  if (!row.write)
    return {};
  const ValueWrite &write = *row.write;
  ValueWritePlan plan;
  if (write.guard && !guard_allows(dev, *write.guard)) {
    plan.refusal = ValueWriteRefusal::GUARD;
    return plan;
  }
  // A value between the steps of a stepped target is rounded or refused before anything judges it.
  float written = value;
  if (const std::optional<TargetSteps> steps = target_steps(row, write)) {
    const ValueWriteRefusal refusal = round_to_step(*steps, write.rounding.policy, value, written);
    if (refusal != ValueWriteRefusal::NONE) {
      plan.refusal = refusal;
      return plan;
    }
  }
  if (const auto *target = std::get_if<RegisterWrite>(&write.target)) {
    // The register's own row judges and encodes the value (ADR 0105), so a value the register refuses never leaves.
    if (!heating_write_accepts(target->fn, written)) {
      plan.refusal = ValueWriteRefusal::REGISTER_RULE;
      return plan;
    }
    plan.refusal = ValueWriteRefusal::NONE;
    plan.register_fn = target->fn;
    plan.register_value = written;
    plan.before = write.before;
    return plan;
  }
  if (const auto *target = std::get_if<FunctionalParameterWrite>(&write.target)) {
    // A flag: 1 sets the on word, 0 the off word.
    if (written != 0.0F && written != 1.0F) {
      plan.refusal = ValueWriteRefusal::UNENCODABLE;
      return plan;
    }
    plan.refusal = ValueWriteRefusal::NONE;
    plan.functional_parameter = target->fp;
    plan.functional_word = written == 1.0F ? target->on_word : target->off_word;
    // The word that switches the flag off ends what the before-write set up, so it goes with the on word only.
    if (written == 1.0F)
      plan.before = write.before;
    return plan;
  }
  // A main parameter is encoded over the range its row's read side decodes over.
  const auto *source = std::get_if<StatusMainParameterSource>(&row.source);
  if (source == nullptr || !source->range) {
    plan.refusal = ValueWriteRefusal::UNENCODABLE;
    return plan;
  }
  switch (encode_temperature_target({source->range->min, source->range->max}, written, plan.main_parameter)) {
    case HeatingRefusal::NONE:
      plan.refusal = ValueWriteRefusal::NONE;
      plan.before = write.before;
      break;
    case HeatingRefusal::OUT_OF_RANGE:
      plan.refusal = ValueWriteRefusal::OUT_OF_RANGE;
      break;
    default:
      plan.refusal = ValueWriteRefusal::UNENCODABLE;
      break;
  }
  return plan;
}

std::optional<ValueRange> value_write_range(const DeviceReadingRow &row) {
  if (!row.write)
    return std::nullopt;
  if (const auto *target = std::get_if<RegisterWrite>(&row.write->target)) {
    const std::optional<ValueForm> form = device_register_form(target->fn);
    if (form && form->codec == ValueCodec::SCALED_BYTE && form->scale.size() >= 2) {
      return ValueRange{static_cast<float>(form->scale.front().natural),
                        static_cast<float>(form->scale.back().natural)};
    }
    return heating_write_range(target->fn);
  }
  const auto *source = std::get_if<StatusMainParameterSource>(&row.source);
  if (std::holds_alternative<MainParameterWrite>(row.write->target) && source != nullptr)
    return source->range;
  return std::nullopt;
}

std::optional<float> value_write_step(const DeviceReadingRow &row) {
  if (!row.write)
    return std::nullopt;
  const std::optional<TargetSteps> steps = target_steps(row, *row.write);
  if (!steps)
    return std::nullopt;
  if (steps->knots.empty())
    return steps->spacing;
  // Knots that are not all the same distance apart have no single step to offer.
  const float spacing = static_cast<float>(steps->knots[1].natural) - static_cast<float>(steps->knots[0].natural);
  for (size_t i = 2; i < steps->knots.size(); ++i) {
    const float gap = static_cast<float>(steps->knots[i].natural) - static_cast<float>(steps->knots[i - 1].natural);
    if (std::fabs(gap - spacing) >= STEP_MATCH_TOLERANCE)
      return std::nullopt;
  }
  return spacing;
}

std::vector<float> value_write_steps(const DeviceReadingRow &row) {
  std::vector<float> values;
  if (!row.write)
    return values;
  const std::optional<TargetSteps> steps = target_steps(row, *row.write);
  if (!steps)
    return values;
  if (!steps->knots.empty()) {
    for (const ValueScaleKnot &knot : steps->knots)
      values.push_back(static_cast<float>(knot.natural));
    return values;
  }
  for (size_t i = 0; steps->first + (static_cast<float>(i) * steps->spacing) <= steps->last + STEP_MATCH_TOLERANCE; ++i)
    values.push_back(steps->first + (static_cast<float>(i) * steps->spacing));
  return values;
}

ScopedReadingTable::ScopedReadingTable(std::span<const DeviceReadingRow> rows) : previous_(active_table) {
  active_table = rows;
}

ScopedReadingTable::~ScopedReadingTable() { active_table = previous_; }

bool store_register_reply(IoDevice &dev, DeviceRegister fn, std::span<const uint8_t> value_bytes) {
  bool stored = false;
  for_each_register_reading(fn, [&](const DeviceReadingRow &row, const RegisterSource &source) {
    if (const std::optional<float> value = decode_register_source(source, value_bytes)) {
      store_reading(dev, row.reading, *value);
      stored = true;
    }
  });
  return stored;
}

bool clear_register_reading(IoDevice &dev, DeviceRegister fn) {
  bool found = false;
  for_each_register_reading(fn, [&](const DeviceReadingRow &row, const RegisterSource &) {
    clear_reading(dev, row.reading);
    found = true;
  });
  return found;
}

void ask_register_reading(IoDevice &dev, DeviceRegister fn) {
  for_each_register_reading(
      fn, [&](const DeviceReadingRow &row, const RegisterSource &) { ask_reading(dev, row.reading); });
}

bool register_reading_wanted(const IoDevice &dev, DeviceRegister fn) {
  const DeviceReadingRow *row = find_register_reading(fn);
  return row != nullptr && reading_asked(dev, row->reading) && !reading_absent(dev, row->reading);
}

void mark_register_reading_absent(IoDevice &dev, DeviceRegister fn) {
  for_each_register_reading(
      fn, [&](const DeviceReadingRow &row, const RegisterSource &) { mark_reading_absent(dev, row.reading); });
}

bool heating_register_due(DeviceRegister fn, const IoDevice &dev) {
  const DeviceReadingRow *row = find_register_reading(fn);
  const auto *source = row != nullptr ? std::get_if<RegisterSource>(&row->source) : nullptr;
  if (source == nullptr || !source->only_while)
    return true;
  return std::cmp_equal(register_wire(dev, source->only_while->fn), source->only_while->value);
}

bool clear_readings_read_only_while(DeviceRegister written, IoDevice &dev) {
  bool cleared = false;
  for (const DeviceReadingRow &row : active_table) {
    const auto *source = std::get_if<RegisterSource>(&row.source);
    if (source != nullptr && source->only_while && source->only_while->fn == written &&
        reading_value(dev, row.reading)) {
      clear_reading(dev, row.reading);
      cleared = true;
    }
  }
  return cleared;
}

}  // namespace home_io_control
}  // namespace esphome

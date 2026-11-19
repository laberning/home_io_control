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

/// A two-bit field of a functional parameter and its value for "on" (0 keep, 1 on, 2 off).
constexpr uint16_t STATUS_FIELD_MASK = 0x3;
constexpr uint16_t STATUS_FIELD_ON = 1;

std::optional<float> remaining_time_of_active_run(const IoDevice &dev);

/// The reading table: one row per value the hub keeps of a device, and the one place that says when a device's poll
/// reads it (`poll`, ReadingPoll) and for which devices (`feature`, else the heating profile: heating_profile_reads()).
/// The rows are in read order: within a policy the poll reads them top to bottom, so the order below is the order of
/// the registers on the wire. The first rows are the ones an entity of the YAML asks for (WHEN_ASKED), each with the
/// capability it needs, in the order of the registers' enumeration (DeviceRegister); then the registers every poll
/// reads, the ones that may miss, and the one read once; the last rows are not polled for their own sake. A towel
/// dryer's minutes left of a run are read only while that run is on (HeatingReadCondition), so an idle dryer costs no
/// read for them; the minutes left of whichever run is active are derived from the two registers, which are rows of
/// their own. The room temperature leads the registers every poll reads: it is the value a user looks at, so a poll
/// that dies half-way still delivered it. A water heater's rows (capability WATER_HEATER) come last, in the order its
/// poll reads them; the three readings that are slices of its heating register name the register once in the poll.
constexpr DeviceReadingRow DEVICE_READINGS[] = {
    {.name = "occupancy",
     .source = RegisterSource{.fn = DeviceRegister::SET_PRESENCE},
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
     .poll = ReadingPoll::EVERY_POLL},
    {.name = "setpoint",
     .source = RegisterSource{.fn = DeviceRegister::SET_TEMPERATURE},
     .reading = DeviceReading::SETPOINT,
     .poll = ReadingPoll::EVERY_POLL},
    {.name = "mode",
     .source = RegisterSource{.fn = DeviceRegister::SET_MODE},
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
     .source = RegisterSource{.fn = DeviceRegister::SET_OPEN_WINDOW_DETECTION},
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
     .poll = ReadingPoll::ONCE_AFTER_BOOT},
    // The temperatures in force are read by the programme rule (only while the programme runs), and the derogated
    // setpoint by a write's read-back.
    {.name = "temperature_in_force",
     .source = RegisterSource{.fn = DeviceRegister::TEMPERATURE_IN_FORCE},
     .reading = DeviceReading::TEMPERATURE_IN_FORCE,
     .poll = ReadingPoll::NOT_POLLED},
    {.name = "heater_temperature_in_force",
     .source = RegisterSource{.fn = DeviceRegister::HEATER_TEMPERATURE_IN_FORCE},
     .reading = DeviceReading::HEATER_TEMPERATURE_IN_FORCE,
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
     .feature = DeviceFeature::WATER_HEATER},
    {.name = "hot_water_mode",
     .source = RegisterSource{.fn = DeviceRegister::DHW_MODE},
     .reading = DeviceReading::HOT_WATER_MODE,
     .poll = ReadingPoll::EVERY_POLL,
     .feature = DeviceFeature::WATER_HEATER,
     .value_name = dhw_mode_name},
    {.name = "hot_water_heating",
     .source = RegisterSource{.fn = DeviceRegister::DHW_HEATING,
                              .part = RegisterPart{.first = 0, .length = 1, .codec = ValueCodec::ANY_NONZERO}},
     .reading = DeviceReading::HOT_WATER_HEATING,
     .poll = ReadingPoll::EVERY_POLL,
     .feature = DeviceFeature::WATER_HEATER},
    {.name = "hot_water_electric_backup",
     .source = RegisterSource{.fn = DeviceRegister::DHW_HEATING,
                              .part = RegisterPart{.first = 2, .length = 1, .codec = ValueCodec::ANY_NONZERO}},
     .reading = DeviceReading::HOT_WATER_ELECTRIC_BACKUP,
     .poll = ReadingPoll::EVERY_POLL,
     .feature = DeviceFeature::WATER_HEATER},
    {.name = "hot_water_heat_pump",
     .source = RegisterSource{.fn = DeviceRegister::DHW_HEATING,
                              .part = RegisterPart{.first = 3, .length = 2, .codec = ValueCodec::ANY_NONZERO}},
     .reading = DeviceReading::HOT_WATER_HEAT_PUMP,
     .poll = ReadingPoll::EVERY_POLL,
     .feature = DeviceFeature::WATER_HEATER},
    {.name = "hot_water_boost_duration",
     .source = RegisterSource{.fn = DeviceRegister::DHW_BOOST_DURATION},
     .reading = DeviceReading::HOT_WATER_BOOST_DURATION,
     .poll = ReadingPoll::EVERY_POLL,
     .feature = DeviceFeature::WATER_HEATER},
    {.name = "hot_water_away_duration",
     .source = RegisterSource{.fn = DeviceRegister::DHW_AWAY_DURATION},
     .reading = DeviceReading::HOT_WATER_AWAY_DURATION,
     .poll = ReadingPoll::EVERY_POLL,
     .feature = DeviceFeature::WATER_HEATER},
    {.name = "hot_water_energy",
     .source = RegisterSource{.fn = DeviceRegister::DHW_ENERGY},
     .reading = DeviceReading::HOT_WATER_ENERGY,
     .poll = ReadingPoll::EVERY_POLL,
     .feature = DeviceFeature::WATER_HEATER},
    {.name = "hot_water_brand",
     .source = RegisterSource{.fn = DeviceRegister::DHW_BRAND},
     .reading = DeviceReading::HOT_WATER_BRAND,
     .poll = ReadingPoll::ONCE_AFTER_BOOT,
     .feature = DeviceFeature::WATER_HEATER},
    {.name = "hot_water_capabilities",
     .source = StatusFunctionalParameterSource{.fp = DHW_FP_CAPABILITIES, .form = StatusParameterForm::WORD},
     .reading = DeviceReading::HOT_WATER_CAPABILITIES,
     .poll = ReadingPoll::NOT_POLLED,
     .feature = DeviceFeature::WATER_HEATER},
    {.name = "hot_water_boost",
     .source =
         StatusFunctionalParameterSource{
             .fp = DHW_FP_OPERATING_MODE, .form = StatusParameterForm::FIELD_ON, .shift = DHW_FP16_BOOST_SHIFT},
     .reading = DeviceReading::HOT_WATER_BOOST,
     .poll = ReadingPoll::NOT_POLLED,
     .feature = DeviceFeature::WATER_HEATER},
    {.name = "hot_water_away",
     .source =
         StatusFunctionalParameterSource{
             .fp = DHW_FP_OPERATING_MODE, .form = StatusParameterForm::FIELD_ON, .shift = DHW_FP16_AWAY_SHIFT},
     .reading = DeviceReading::HOT_WATER_AWAY,
     .poll = ReadingPoll::NOT_POLLED,
     .feature = DeviceFeature::WATER_HEATER},
};

/// Run `visit` on every row whose source is the register `fn`, whole or a part of it, in table order.
template<class Visit> void for_each_register_reading(DeviceRegister fn, Visit visit) {
  for (const DeviceReadingRow &row : DEVICE_READINGS) {
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
  for (const DeviceReadingRow &row : DEVICE_READINGS) {
    const auto *source = std::get_if<RegisterSource>(&row.source);
    if (source != nullptr && source->only_while && source->only_while->fn == DeviceRegister::TEMPORARY_STATE &&
        std::cmp_equal(source->only_while->value, state))
      return register_value(dev, source->fn);
  }
  return 0.0F;
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
                        [](const DerivedSource &) { return ValueCodec::NONE; },
                        [](const StatusMainParameterSource &) { return ValueCodec::NONE; },
                        // A field of a functional parameter is a flag, and the whole word a number.
                        [](const StatusFunctionalParameterSource &source) {
                          return source.form == StatusParameterForm::FIELD_ON ? ValueCodec::BINARY : ValueCodec::NONE;
                        },
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

const DeviceReadingRow *find_device_reading(DeviceReading reading, std::span<const DeviceReadingRow> table) {
  const auto row = std::ranges::find(table, reading, &DeviceReadingRow::reading);
  return row != table.end() ? &*row : nullptr;
}

const DeviceReadingRow *find_device_reading(DeviceReading reading) {
  return find_device_reading(reading, DEVICE_READINGS);
}

std::span<const DeviceReadingRow> device_readings() { return DEVICE_READINGS; }

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
                        [&dev](const DerivedSource &source) { return source.value(dev); },
                    },
                    row->source);
}

std::optional<float> reading_value(const IoDevice &dev, DeviceReading reading) {
  return reading_value(dev, reading, DEVICE_READINGS);
}

std::optional<float> decode_data_object_reading(const DataObjectSource &source, std::span<const uint8_t> data) {
  if (source.first >= data.size())
    return std::nullopt;
  const std::span<const uint8_t> slice =
      data.subspan(source.first, std::min<size_t>(source.length, data.size() - source.first));
  return decode_value(source.codec, slice, source.max_value);
}

std::optional<float> decode_status_reading(DeviceReading reading, std::span<const uint8_t> main_parameter,
                                           uint8_t level_prefix, float range_min_c, float range_max_c) {
  const DeviceReadingRow *row = find_device_reading(reading);
  const auto *source = row != nullptr ? std::get_if<StatusMainParameterSource>(&row->source) : nullptr;
  const std::optional<float> decoded = decode_value(ValueCodec::U16_BE, main_parameter);
  if (source == nullptr || !decoded)
    return std::nullopt;
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
      return std::round(celsius * VALUE_TENTHS_PER_UNIT) / VALUE_TENTHS_PER_UNIT;
    }
  }
  return std::nullopt;
}

size_t store_status_parameter_readings(IoDevice &dev, const IoFrame &reply) {
  size_t stored = 0;
  for (const DeviceReadingRow &row : DEVICE_READINGS) {
    const auto *source = std::get_if<StatusFunctionalParameterSource>(&row.source);
    uint16_t word = 0;
    if (source == nullptr || !decode_status_functional_parameter(reply, source->fp, word))
      continue;
    auto value = static_cast<float>(word);
    if (source->form == StatusParameterForm::FIELD_ON)
      value = ((word >> source->shift) & STATUS_FIELD_MASK) == STATUS_FIELD_ON ? 1.0F : 0.0F;
    dev.reading_values.store(reading_slot(row.reading), value);
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
  for (const DeviceReadingRow &row : DEVICE_READINGS) {
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

bool store_register_reading(IoDevice &dev, DeviceRegister fn, float value) {
  const DeviceReadingRow *row = find_register_reading(fn);
  if (row == nullptr)
    return false;
  dev.reading_values.store(reading_slot(row->reading), value);
  return true;
}

bool store_register_reply(IoDevice &dev, DeviceRegister fn, std::span<const uint8_t> value_bytes) {
  bool stored = false;
  for_each_register_reading(fn, [&](const DeviceReadingRow &row, const RegisterSource &source) {
    if (const std::optional<float> value = decode_register_source(source, value_bytes)) {
      dev.reading_values.store(reading_slot(row.reading), *value);
      stored = true;
    }
  });
  return stored;
}

bool clear_register_reading(IoDevice &dev, DeviceRegister fn) {
  bool found = false;
  for_each_register_reading(fn, [&](const DeviceReadingRow &row, const RegisterSource &) {
    dev.reading_values.clear(reading_slot(row.reading));
    found = true;
  });
  return found;
}

void ask_register_reading(IoDevice &dev, DeviceRegister fn) {
  for_each_register_reading(fn, [&](const DeviceReadingRow &row, const RegisterSource &) {
    dev.reading_values.ask(reading_slot(row.reading));
  });
}

bool register_reading_wanted(const IoDevice &dev, DeviceRegister fn) {
  const DeviceReadingRow *row = find_register_reading(fn);
  return row != nullptr && dev.reading_values.asked(reading_slot(row->reading)) &&
         !dev.reading_values.absent(reading_slot(row->reading));
}

void mark_register_reading_absent(IoDevice &dev, DeviceRegister fn) {
  for_each_register_reading(fn, [&](const DeviceReadingRow &row, const RegisterSource &) {
    dev.reading_values.mark_absent(reading_slot(row.reading));
  });
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
  for (const DeviceReadingRow &row : DEVICE_READINGS) {
    const auto *source = std::get_if<RegisterSource>(&row.source);
    if (source != nullptr && source->only_while && source->only_while->fn == written &&
        dev.reading_values.value(reading_slot(row.reading))) {
      dev.reading_values.clear(reading_slot(row.reading));
      cleared = true;
    }
  }
  return cleared;
}

}  // namespace home_io_control
}  // namespace esphome

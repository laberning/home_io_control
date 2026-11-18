/// @file proto_device_readings.cpp
/// @brief The named readings a device can show, the capability each needs and where each value comes from.
/// @ingroup hioc_protocol

#include "proto_device_readings.h"

#include "proto_codecs.h"
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
constexpr HeatingFunction RUN_REMAINING_REGISTERS[] = {HeatingFunction::BOOST_REMAINING,
                                                       HeatingFunction::DRYING_REMAINING};

/// Low byte of a 16-bit main parameter.
constexpr uint16_t STATUS_VALUE_LOW_BYTE_MASK = 0xFF;

std::optional<float> remaining_time_of_active_run(const IoDevice &dev);

/// The reading table. The first rows are the ones an entity of the YAML asks for, with the capability each needs; the
/// rest are the registers and status values the climate card keeps by the heater's profile. A towel dryer's minutes
/// left of a run are read only while that run is on (HeatingReadCondition), so an idle dryer costs no read for them.
/// `poll` is the cadence of a device that reads the reading (ReadingPoll); the heating profiles' own register lists
/// still say which heaters read which.
constexpr DeviceReadingRow DEVICE_READINGS[] = {
    {.name = "central_heating_mode",
     .source = HeatingRegisterSource{.fn = HeatingFunction::CENTRAL_HEATING_MODE},
     .reading = DeviceReading::CENTRAL_HEATING_MODE,
     .poll = ReadingPoll::WHEN_ASKED,
     .feature = DeviceFeature::CENTRAL_HEATING_MODE},
    {.name = "occupancy_detection",
     .source = HeatingRegisterSource{.fn = HeatingFunction::OCCUPANCY_DETECTION},
     .reading = DeviceReading::OCCUPANCY_DETECTION,
     .poll = ReadingPoll::WHEN_ASKED,
     .feature = DeviceFeature::OCCUPANCY},
    {.name = "occupancy",
     .source = HeatingRegisterSource{.fn = HeatingFunction::SET_PRESENCE},
     .reading = DeviceReading::OCCUPANCY,
     .poll = ReadingPoll::WHEN_ASKED,
     .feature = DeviceFeature::OCCUPANCY},
    {.name = "open_window_detection",
     .source = HeatingRegisterSource{.fn = HeatingFunction::SET_OPEN_WINDOW_DETECTION},
     .reading = DeviceReading::OPEN_WINDOW_DETECTION,
     .poll = ReadingPoll::EVERY_POLL,
     .feature = DeviceFeature::WINDOW_COMPANIONS},
    {.name = "energy_total",
     .source = HeatingRegisterSource{.fn = HeatingFunction::ENERGY_TOTAL},
     .reading = DeviceReading::ENERGY_TOTAL,
     .poll = ReadingPoll::WHEN_ASKED,
     .feature = DeviceFeature::ENERGY_METER},
    {.name = "boost_duration",
     .source = HeatingRegisterSource{.fn = HeatingFunction::BOOST_DURATION},
     .reading = DeviceReading::BOOST_DURATION,
     .poll = ReadingPoll::WHEN_ASKED,
     .feature = DeviceFeature::RUN_TIMERS},
    {.name = "drying_duration",
     .source = HeatingRegisterSource{.fn = HeatingFunction::DRYING_DURATION},
     .reading = DeviceReading::DRYING_DURATION,
     .poll = ReadingPoll::WHEN_ASKED,
     .feature = DeviceFeature::RUN_TIMERS},
    {.name = "boost_remaining",
     .source =
         HeatingRegisterSource{.fn = HeatingFunction::BOOST_REMAINING,
                               .only_while = HeatingReadCondition{HeatingFunction::TEMPORARY_STATE,
                                                                  static_cast<uint8_t>(HeatingTemporaryState::BOOST)}},
     .reading = DeviceReading::BOOST_REMAINING,
     .poll = ReadingPoll::WHEN_ASKED,
     .feature = DeviceFeature::RUN_TIMERS},
    {.name = "drying_remaining",
     .source =
         HeatingRegisterSource{.fn = HeatingFunction::DRYING_REMAINING,
                               .only_while = HeatingReadCondition{HeatingFunction::TEMPORARY_STATE,
                                                                  static_cast<uint8_t>(HeatingTemporaryState::DRYING)}},
     .reading = DeviceReading::DRYING_REMAINING,
     .poll = ReadingPoll::WHEN_ASKED,
     .feature = DeviceFeature::RUN_TIMERS},
    {.name = "remaining_time",
     .source = DerivedSource{.value = remaining_time_of_active_run, .polls = RUN_REMAINING_REGISTERS},
     .reading = DeviceReading::REMAINING_TIME,
     .poll = ReadingPoll::WHEN_ASKED,
     .feature = DeviceFeature::RUN_TIMERS},
    {.name = "room_temperature",
     .source = HeatingRegisterSource{.fn = HeatingFunction::ROOM_TEMPERATURE},
     .reading = DeviceReading::ROOM_TEMPERATURE,
     .poll = ReadingPoll::EVERY_POLL},
    {.name = "setpoint",
     .source = HeatingRegisterSource{.fn = HeatingFunction::SET_TEMPERATURE},
     .reading = DeviceReading::SETPOINT,
     .poll = ReadingPoll::EVERY_POLL},
    {.name = "max_setpoint",
     .source = HeatingRegisterSource{.fn = HeatingFunction::MAX_SETPOINT},
     .reading = DeviceReading::MAX_SETPOINT,
     .poll = ReadingPoll::ONCE_AFTER_BOOT},
    {.name = "mode",
     .source = HeatingRegisterSource{.fn = HeatingFunction::SET_MODE},
     .reading = DeviceReading::MODE,
     .poll = ReadingPoll::EVERY_POLL},
    {.name = "regulator_state",
     .source = HeatingRegisterSource{.fn = HeatingFunction::REGULATOR_STATE},
     .reading = DeviceReading::REGULATOR_STATE,
     .poll = ReadingPoll::EVERY_POLL},
    {.name = "window_open",
     .source = HeatingRegisterSource{.fn = HeatingFunction::WINDOW_OPEN},
     .reading = DeviceReading::WINDOW_OPEN,
     .poll = ReadingPoll::EVERY_POLL},
    {.name = "towel_mode",
     .source = HeatingRegisterSource{.fn = HeatingFunction::TOWEL_MODE},
     .reading = DeviceReading::TOWEL_MODE,
     .poll = ReadingPoll::EVERY_POLL},
    {.name = "temporary_state",
     .source = HeatingRegisterSource{.fn = HeatingFunction::TEMPORARY_STATE},
     .reading = DeviceReading::TEMPORARY_STATE,
     .poll = ReadingPoll::EVERY_POLL},
    {.name = "working_rate",
     .source = HeatingRegisterSource{.fn = HeatingFunction::WORKING_RATE},
     .reading = DeviceReading::WORKING_RATE,
     .poll = ReadingPoll::EVERY_POLL},
    {.name = "temperature_in_force",
     .source = HeatingRegisterSource{.fn = HeatingFunction::TEMPERATURE_IN_FORCE},
     .reading = DeviceReading::TEMPERATURE_IN_FORCE,
     .poll = ReadingPoll::NEVER},
    {.name = "heater_temperature_in_force",
     .source = HeatingRegisterSource{.fn = HeatingFunction::HEATER_TEMPERATURE_IN_FORCE},
     .reading = DeviceReading::HEATER_TEMPERATURE_IN_FORCE,
     .poll = ReadingPoll::NEVER},
    {.name = "derogated_setpoint",
     .source = HeatingRegisterSource{.fn = HeatingFunction::DEROGATED_SETPOINT},
     .reading = DeviceReading::DEROGATED_SETPOINT,
     .poll = ReadingPoll::NEVER},
    {.name = "night_light",
     .source = HeatingRegisterSource{.fn = HeatingFunction::NIGHT_LIGHT},
     .reading = DeviceReading::NIGHT_LIGHT,
     .poll = ReadingPoll::EVERY_POLL},
    {.name = "night_light_brightness",
     .source = HeatingRegisterSource{.fn = HeatingFunction::NIGHT_LIGHT_BRIGHTNESS},
     .reading = DeviceReading::NIGHT_LIGHT_BRIGHTNESS,
     .poll = ReadingPoll::EVERY_POLL},
    {.name = "heating_level",
     .source = StatusMainParameterSource{.form = StatusValueForm::LEVEL},
     .reading = DeviceReading::HEATING_LEVEL,
     .poll = ReadingPoll::NEVER},
    {.name = "status_setpoint",
     .source = StatusMainParameterSource{.form = StatusValueForm::TEMPERATURE},
     .reading = DeviceReading::STATUS_SETPOINT,
     .poll = ReadingPoll::NEVER},
};

/// @brief The minutes left of the timed run a towel dryer is in.
/// @details Follows the temporary state (HeatingFunction::TEMPORARY_STATE): the register of the run it shows, as
///          the rows with a condition on that state name it, or 0 with no run active. Boost and drying never run
///          together, since one register holds the state.
std::optional<float> remaining_time_of_active_run(const IoDevice &dev) {
  const int state = register_wire(dev, HeatingFunction::TEMPORARY_STATE);
  if (state < 0)
    return std::nullopt;
  for (const DeviceReadingRow &row : DEVICE_READINGS) {
    const auto *source = std::get_if<HeatingRegisterSource>(&row.source);
    if (source != nullptr && source->only_while && source->only_while->fn == HeatingFunction::TEMPORARY_STATE &&
        std::cmp_equal(source->only_while->value, state))
      return register_value(dev, source->fn);
  }
  return 0.0F;
}

}  // namespace

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
                        [&stored](const HeatingRegisterSource &) { return stored(); },
                        [&stored](const StatusMainParameterSource &) { return stored(); },
                        [&stored](const DataObjectSource &) { return stored(); },
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
      return celsius;
    }
  }
  return std::nullopt;
}

std::span<const HeatingFunction> reading_polled_registers(const DeviceReadingRow &row) {
  return std::visit(
      Overloaded{
          [](const HeatingRegisterSource &source) { return std::span<const HeatingFunction>{&source.fn, 1}; },
          [](const DerivedSource &source) { return source.polls; },
          [](const DataObjectSource &) { return std::span<const HeatingFunction>{}; },
          [](const StatusMainParameterSource &) { return std::span<const HeatingFunction>{}; },
      },
      row.source);
}

std::optional<HeatingFunction> reading_register(DeviceReading reading) {
  const DeviceReadingRow *row = find_device_reading(reading);
  if (row == nullptr)
    return std::nullopt;
  if (const auto *source = std::get_if<HeatingRegisterSource>(&row->source))
    return source->fn;
  return std::nullopt;
}

const DeviceReadingRow *find_register_reading(HeatingFunction fn) {
  for (const DeviceReadingRow &row : DEVICE_READINGS) {
    const auto *source = std::get_if<HeatingRegisterSource>(&row.source);
    if (source != nullptr && source->fn == fn)
      return &row;
  }
  return nullptr;
}

std::optional<float> register_value(const IoDevice &dev, HeatingFunction fn) {
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

int register_wire(const IoDevice &dev, HeatingFunction fn) {
  const DeviceReadingRow *row = find_register_reading(fn);
  return row != nullptr ? reading_wire(dev, row->reading) : -1;
}

bool store_register_reading(IoDevice &dev, HeatingFunction fn, float value) {
  const DeviceReadingRow *row = find_register_reading(fn);
  if (row == nullptr)
    return false;
  dev.reading_values.store(reading_slot(row->reading), value);
  return true;
}

bool clear_register_reading(IoDevice &dev, HeatingFunction fn) {
  const DeviceReadingRow *row = find_register_reading(fn);
  if (row == nullptr)
    return false;
  dev.reading_values.clear(reading_slot(row->reading));
  return true;
}

void ask_register_reading(IoDevice &dev, HeatingFunction fn) {
  if (const DeviceReadingRow *row = find_register_reading(fn))
    dev.reading_values.ask(reading_slot(row->reading));
}

bool register_reading_wanted(const IoDevice &dev, HeatingFunction fn) {
  const DeviceReadingRow *row = find_register_reading(fn);
  return row != nullptr && dev.reading_values.asked(reading_slot(row->reading)) &&
         !dev.reading_values.absent(reading_slot(row->reading));
}

void mark_register_reading_absent(IoDevice &dev, HeatingFunction fn) {
  if (const DeviceReadingRow *row = find_register_reading(fn))
    dev.reading_values.mark_absent(reading_slot(row->reading));
}

bool heating_register_due(HeatingFunction fn, const IoDevice &dev) {
  const DeviceReadingRow *row = find_register_reading(fn);
  const auto *source = row != nullptr ? std::get_if<HeatingRegisterSource>(&row->source) : nullptr;
  if (source == nullptr || !source->only_while)
    return true;
  return std::cmp_equal(register_wire(dev, source->only_while->fn), source->only_while->value);
}

bool clear_readings_read_only_while(HeatingFunction written, IoDevice &dev) {
  bool cleared = false;
  for (const DeviceReadingRow &row : DEVICE_READINGS) {
    const auto *source = std::get_if<HeatingRegisterSource>(&row.source);
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

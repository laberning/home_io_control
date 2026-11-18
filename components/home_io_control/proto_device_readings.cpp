/// @file proto_device_readings.cpp
/// @brief The named readings a device can show, the capability each needs and where each value comes from.
/// @ingroup hioc_protocol

#include "proto_device_readings.h"

#include <algorithm>
#include <iterator>
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

std::optional<float> remaining_time_of_active_run(const IoDevice &dev);

/// The reading table. A towel dryer's minutes left of a run are read only while that run is on
/// (HeatingReadCondition), so an idle dryer costs no read for them.
constexpr DeviceReadingRow DEVICE_READINGS[] = {
    {.name = "central_heating_mode",
     .source = HeatingRegisterSource{.fn = HeatingFunction::CENTRAL_HEATING_MODE},
     .reading = DeviceReading::CENTRAL_HEATING_MODE,
     .feature = DeviceFeature::CENTRAL_HEATING_MODE},
    {.name = "occupancy_detection",
     .source = HeatingRegisterSource{.fn = HeatingFunction::OCCUPANCY_DETECTION},
     .reading = DeviceReading::OCCUPANCY_DETECTION,
     .feature = DeviceFeature::OCCUPANCY},
    {.name = "occupancy",
     .source = HeatingRegisterSource{.fn = HeatingFunction::SET_PRESENCE},
     .reading = DeviceReading::OCCUPANCY,
     .feature = DeviceFeature::OCCUPANCY},
    {.name = "open_window_detection",
     .source = HeatingRegisterSource{.fn = HeatingFunction::SET_OPEN_WINDOW_DETECTION},
     .reading = DeviceReading::OPEN_WINDOW_DETECTION,
     .feature = DeviceFeature::WINDOW_COMPANIONS},
    {.name = "energy_total",
     .source = HeatingRegisterSource{.fn = HeatingFunction::ENERGY_TOTAL},
     .reading = DeviceReading::ENERGY_TOTAL,
     .feature = DeviceFeature::ENERGY_METER},
    {.name = "boost_duration",
     .source = HeatingRegisterSource{.fn = HeatingFunction::BOOST_DURATION},
     .reading = DeviceReading::BOOST_DURATION,
     .feature = DeviceFeature::RUN_TIMERS},
    {.name = "drying_duration",
     .source = HeatingRegisterSource{.fn = HeatingFunction::DRYING_DURATION},
     .reading = DeviceReading::DRYING_DURATION,
     .feature = DeviceFeature::RUN_TIMERS},
    {.name = "boost_remaining",
     .source =
         HeatingRegisterSource{.fn = HeatingFunction::BOOST_REMAINING,
                               .only_while = HeatingReadCondition{HeatingFunction::TEMPORARY_STATE,
                                                                  static_cast<uint8_t>(HeatingTemporaryState::BOOST)}},
     .reading = DeviceReading::BOOST_REMAINING,
     .feature = DeviceFeature::RUN_TIMERS},
    {.name = "drying_remaining",
     .source =
         HeatingRegisterSource{.fn = HeatingFunction::DRYING_REMAINING,
                               .only_while = HeatingReadCondition{HeatingFunction::TEMPORARY_STATE,
                                                                  static_cast<uint8_t>(HeatingTemporaryState::DRYING)}},
     .reading = DeviceReading::DRYING_REMAINING,
     .feature = DeviceFeature::RUN_TIMERS},
    {.name = "remaining_time",
     .source = DerivedSource{.value = remaining_time_of_active_run, .polls = RUN_REMAINING_REGISTERS},
     .reading = DeviceReading::REMAINING_TIME,
     .feature = DeviceFeature::RUN_TIMERS},
};

/// @brief The minutes left of the timed run a towel dryer is in.
/// @details Follows the temporary state (HeatingFunction::TEMPORARY_STATE): the register of the run it shows, as
///          the rows with a condition on that state name it, or 0 with no run active. Boost and drying never run
///          together, since one register holds the state.
std::optional<float> remaining_time_of_active_run(const IoDevice &dev) {
  const int16_t state = dev.heating.value_of(HeatingFunction::TEMPORARY_STATE);
  if (state < 0)
    return std::nullopt;
  for (const DeviceReadingRow &row : DEVICE_READINGS) {
    const auto *source = std::get_if<HeatingRegisterSource>(&row.source);
    if (source != nullptr && source->only_while && source->only_while->fn == HeatingFunction::TEMPORARY_STATE &&
        static_cast<int16_t>(source->only_while->value) == state)
      return dev.heating.reading_of(source->fn);
  }
  return 0.0F;
}

}  // namespace

const DeviceReadingRow *find_device_reading(DeviceReading reading) {
  const auto *row = std::ranges::find(DEVICE_READINGS, reading, &DeviceReadingRow::reading);
  return row != std::end(DEVICE_READINGS) ? row : nullptr;
}

std::span<const DeviceReadingRow> device_readings() { return DEVICE_READINGS; }

std::optional<float> reading_value(const IoDevice &dev, DeviceReading reading) {
  const DeviceReadingRow *row = find_device_reading(reading);
  if (row == nullptr)
    return std::nullopt;
  return std::visit(Overloaded{
                        [&dev](const HeatingRegisterSource &source) { return dev.heating.reading_of(source.fn); },
                        [&dev](const DerivedSource &source) { return source.value(dev); },
                    },
                    row->source);
}

std::span<const HeatingFunction> reading_polled_registers(const DeviceReadingRow &row) {
  return std::visit(
      Overloaded{
          [](const HeatingRegisterSource &source) { return std::span<const HeatingFunction>{&source.fn, 1}; },
          [](const DerivedSource &source) { return source.polls; },
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

bool heating_register_due(HeatingFunction fn, const HeatingReadings &readings) {
  const DeviceReadingRow *row = find_register_reading(fn);
  const auto *source = row != nullptr ? std::get_if<HeatingRegisterSource>(&row->source) : nullptr;
  if (source == nullptr || !source->only_while)
    return true;
  return readings.value_of(source->only_while->fn) == static_cast<int16_t>(source->only_while->value);
}

bool clear_readings_read_only_while(HeatingFunction written, HeatingReadings &readings) {
  bool cleared = false;
  for (const DeviceReadingRow &row : DEVICE_READINGS) {
    const auto *source = std::get_if<HeatingRegisterSource>(&row.source);
    if (source != nullptr && source->only_while && source->only_while->fn == written && readings.has(source->fn)) {
      readings.clear(source->fn);
      cleared = true;
    }
  }
  return cleared;
}

}  // namespace home_io_control
}  // namespace esphome

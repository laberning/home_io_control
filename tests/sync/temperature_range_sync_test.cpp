/// @file temperature_range_sync_test.cpp
/// @brief Guards the default heating temperature range against Python/C++ drift.
///
/// climate.py's `DEFAULT_TEMPERATURE_RANGE_C` is what the climate entity reports as its range when
/// no `min_temperature:`/`max_temperature:` is configured; it has to equal the range the heating
/// protocol encodes over (`HEATING_TEMPERATURE_RANGE_MIN_C` / `_MAX_C`, proto_heating.h), or the
/// entity would show a range the device does not use. The test parses climate.py at runtime
/// (Python stays the single source of truth for the YAML side, same mechanism as
/// device_type_sync_test.cpp).

#include "proto_heating.h"
#include "support/python_dict_parser.h"

using namespace esphome::home_io_control;

TEST(TemperatureRangeSync, PythonDefaultRangeMatchesTheProtocolRange) {
  const auto range =
      test::parse_python_float_pair("components/home_io_control/climate.py", "DEFAULT_TEMPERATURE_RANGE_C");
  ASSERT_EQ(range.size(), 2u) << "DEFAULT_TEMPERATURE_RANGE_C not parsed from climate.py";
  EXPECT_FLOAT_EQ(static_cast<float>(range[0]), HEATING_TEMPERATURE_RANGE_MIN_C)
      << "climate.py DEFAULT_TEMPERATURE_RANGE_C low drifted from HEATING_TEMPERATURE_RANGE_MIN_C";
  EXPECT_FLOAT_EQ(static_cast<float>(range[1]), HEATING_TEMPERATURE_RANGE_MAX_C)
      << "climate.py DEFAULT_TEMPERATURE_RANGE_C high drifted from HEATING_TEMPERATURE_RANGE_MAX_C";
}

/// @file travel_speed_sync_test.cpp
/// @brief Guards the `travel_speed:` option names against Python/C++ drift.
///
/// cover.py's `TRAVEL_SPEEDS` lists the YAML values, which are also the options of the travel
/// speed select, in enum order; the names are the ones `travel_speed_name()` publishes, so the
/// select's state reads back as a valid YAML value. The test parses cover.py at runtime (Python
/// stays the single source of truth for the YAML side) and pins, for every entry, that the name
/// is the C++ name of the enumerator it maps to, that `parse_travel_speed()` accepts it, and that
/// the two sides list the same speeds in the same order.

#include "proto_device_model.h"
#include "support/python_dict_parser.h"

#include <cstddef>
#include <iterator>
#include <string>

using namespace esphome::home_io_control;

namespace {

/// Every enumerator of TravelSpeed in declaration order, with the identifier cover.py spells it by.
/// Listed by hand, so a new enumerator needs a row here: the count check below catches a Python
/// entry without a row, and -Wswitch in travel_speed_name() catches an enumerator without a name,
/// but nothing catches an enumerator added to neither this list nor cover.py.
struct CppSpeed {
  const char *identifier;
  TravelSpeed speed;
};
constexpr CppSpeed CPP_SPEEDS[] = {
    {"TravelSpeed.MOTOR_DEFAULT", TravelSpeed::MOTOR_DEFAULT},
    {"TravelSpeed.SILENT", TravelSpeed::SILENT},
    {"TravelSpeed.FAST", TravelSpeed::FAST},
};

}  // namespace

TEST(TravelSpeedSync, EveryPythonNameIsTheCppNameOfItsEnumerator) {
  const auto entries = test::parse_python_ordered_dict("components/home_io_control/cover.py", "TRAVEL_SPEEDS");
  ASSERT_EQ(entries.size(), std::size(CPP_SPEEDS))
      << "cover.py TRAVEL_SPEEDS and the C++ TravelSpeed enumerators differ in number";
  for (size_t i = 0; i < entries.size(); i++) {
    const auto &[name, identifier] = entries[i];
    EXPECT_EQ(identifier, CPP_SPEEDS[i].identifier) << "TRAVEL_SPEEDS[\"" << name << "\"] is out of enum order";
    EXPECT_EQ(name, travel_speed_name(CPP_SPEEDS[i].speed))
        << "cover.py names " << identifier << " \"" << name << "\"; travel_speed_name() disagrees";
    TravelSpeed parsed{};
    ASSERT_TRUE(parse_travel_speed(name, parsed)) << "parse_travel_speed() rejects \"" << name << "\"";
    EXPECT_EQ(parsed, CPP_SPEEDS[i].speed) << name;
  }
}

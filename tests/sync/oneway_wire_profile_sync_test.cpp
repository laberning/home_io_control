/// @file oneway_wire_profile_sync_test.cpp
/// @brief Verifies that the manufacturers with a real 1W wire profile are the same in C++
///        (ONEWAY_WIRE_PROFILES, oneway_controller.h) and in Python
///        (ONEWAY_WIRE_PROFILE_MANUFACTURER_NAMES, hub_validators.py).
///
/// The YAML codegen warns for an explicit manufacturer that has no profile, while the firmware falls
/// back to a guessed Somfy-shaped one. If the two lists drift, a manufacturer would be warned about
/// although it has a profile, or silently guessed at although the warning is the only hint. Both
/// source files are parsed at runtime, like manufacturer_sync_test.cpp does, so neither copy is
/// treated as the authoritative one.

#include "oneway_controller.h"
#include "support/python_dict_parser.h"
#include "test_helpers.h"

#include <fstream>
#include <set>
#include <string>

using namespace esphome::home_io_control;

namespace {

constexpr const char *VALIDATORS = "components/home_io_control/hub_validators.py";

/// The quoted names of the module-level `ONEWAY_WIRE_PROFILE_MANUFACTURER_NAMES = ("a", "b")` tuple.
std::set<std::string> parse_python_profile_manufacturer_names() {
  std::ifstream file = test::detail::open_source_or_fail(VALIDATORS);
  std::set<std::string> names;
  std::string line;
  const std::string declaration = "ONEWAY_WIRE_PROFILE_MANUFACTURER_NAMES";
  while (std::getline(file, line)) {
    if (line.compare(0, declaration.size(), declaration) != 0)
      continue;
    const auto hash = line.find('#');
    if (hash != std::string::npos)
      line.erase(hash);
    for (size_t open = line.find('"'); open != std::string::npos; open = line.find('"', open + 1)) {
      const size_t close = line.find('"', open + 1);
      if (close == std::string::npos)
        break;
      names.insert(line.substr(open + 1, close - open - 1));
      open = close;
    }
  }
  return names;
}

}  // namespace

TEST(OneWayWireProfileSync, PythonManufacturersWithAProfileAreTheCppTableRows) {
  const auto options = test::parse_python_uint8_dict(VALIDATORS, "MANUFACTURER_OPTIONS");
  const auto names = parse_python_profile_manufacturer_names();
  ASSERT_FALSE(names.empty()) << "ONEWAY_WIRE_PROFILE_MANUFACTURER_NAMES not parsed from hub_validators.py";

  std::set<uint8_t> python;
  for (const std::string &name : names) {
    ASSERT_TRUE(options.count(name)) << name << " is not a MANUFACTURER_OPTIONS name";
    python.insert(options.at(name));
  }
  std::set<uint8_t> cpp;
  for (const OneWayWireProfileRow &row : ONEWAY_WIRE_PROFILES)
    cpp.insert(row.manufacturer);

  EXPECT_EQ(python, cpp) << "ONEWAY_WIRE_PROFILE_MANUFACTURER_NAMES (hub_validators.py) and ONEWAY_WIRE_PROFILES "
                            "(oneway_controller.h) must list the same manufacturers";
}

TEST(OneWayWireProfileSync, EveryTableRowResolvesToItselfAndIsNoGuess) {
  std::set<uint8_t> seen;
  for (const OneWayWireProfileRow &row : ONEWAY_WIRE_PROFILES) {
    EXPECT_TRUE(seen.insert(row.manufacturer).second) << "manufacturer listed twice";
    EXPECT_FALSE(row.profile.profile_is_a_guess);
    EXPECT_EQ(resolve_oneway_wire_profile(row.manufacturer).execute_acei, row.profile.execute_acei);
    EXPECT_FALSE(resolve_oneway_wire_profile(row.manufacturer).profile_is_a_guess);
  }
}

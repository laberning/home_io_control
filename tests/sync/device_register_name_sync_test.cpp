/// @file device_register_name_sync_test.cpp
/// @brief Verifies that every register's name (the label in logs and the stem of its `heating_control` function
///        name) is its DeviceRegister enumerator in lower case, and that no name repeats.
///
/// The enumerators are parsed from proto_heating.h at runtime, like the other sync tests (see
/// python_dict_parser.h), so a renamed enumerator or a row with a stale name fails here.

#include "proto_heating.h"
#include "support/python_dict_parser.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <string>
#include <vector>

using namespace esphome::home_io_control;

namespace {

constexpr const char *HEADER = "components/home_io_control/proto_heating.h";
constexpr const char *ENUM_START = "enum class DeviceRegister";
constexpr const char *COUNT_ENUMERATOR = "COUNT";

/// The enumerator names of DeviceRegister in declaration order, COUNT excluded. An enumerator is an upper-case word
/// at the enum body's indentation; wrapped doc comments start with `///<` and never match.
std::vector<std::string> parse_register_enumerators() {
  std::ifstream in = test::detail::open_source_or_fail(HEADER);
  std::vector<std::string> names;
  std::string line;
  bool in_enum = false;
  while (std::getline(in, line)) {
    if (!in_enum) {
      in_enum = line.find(ENUM_START) != std::string::npos;
      continue;
    }
    if (line.rfind("};", 0) == 0)
      break;
    if (line.rfind("  ", 0) != 0 || line.size() < 3 || line[2] == ' ' || line[2] == '/')
      continue;
    size_t end = 2;
    while (end < line.size() && (std::isupper(static_cast<unsigned char>(line[end])) != 0 ||
                                 std::isdigit(static_cast<unsigned char>(line[end])) != 0 || line[end] == '_'))
      end++;
    if (end > 2)
      names.push_back(line.substr(2, end - 2));
  }
  names.erase(std::remove(names.begin(), names.end(), COUNT_ENUMERATOR), names.end());
  return names;
}

std::string lower_case(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
  return text;
}

}  // namespace

TEST(DeviceRegisterNameSync, EveryRegisterIsNamedAfterItsEnumeratorInLowerCase) {
  const std::vector<std::string> enumerators = parse_register_enumerators();
  ASSERT_EQ(enumerators.size(), static_cast<size_t>(DeviceRegister::COUNT))
      << "the parser missed an enumerator of DeviceRegister, or a register was added without being parsed";
  for (size_t i = 0; i < enumerators.size(); i++) {
    EXPECT_EQ(std::string(device_register_name(static_cast<DeviceRegister>(i))), lower_case(enumerators[i]))
        << "the row of " << enumerators[i] << " must be named after its enumerator";
  }
}

TEST(DeviceRegisterNameSync, NoTwoRegistersShareAName) {
  std::set<std::string> seen;
  for (size_t i = 0; i < static_cast<size_t>(DeviceRegister::COUNT); i++) {
    const std::string name = device_register_name(static_cast<DeviceRegister>(i));
    EXPECT_TRUE(seen.insert(name).second) << "name used twice: " << name;
  }
}

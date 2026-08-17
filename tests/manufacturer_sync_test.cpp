/// @file manufacturer_sync_test.cpp
/// @brief Verifies that Python's MANUFACTURER_OPTIONS dict (__init__.py) and C++'s
///        MANUFACTURER_* constants (proto_constants.h) agree on every name and value,
///        preventing drift between the two hand-maintained tables.
///
/// Same shape of check as device_type_sync_test.cpp, for the 1W `manufacturer:` YAML exposure
/// (validate_manufacturer(), __init__.py) — including sharing that file's Python-dict parser
/// (python_dict_parser.h). Both source files are parsed at runtime rather than hand-transcribed
/// here, so neither language's copy is treated as more authoritative than the other — a rename or
/// a value change on either side without a matching change on the other fails here, rather than
/// being caught by a user's misdirected paste months later. This is the older of two
/// cross-language sync mechanisms this project uses; see device_type_sync_test.cpp's file header
/// for the fuller note on the newer script-based form (`scripts/check-yaml-emitters.py` etc.).

#include "support/python_dict_parser.h"
#include "test_helpers.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <string>

namespace {

/// Parse every `static constexpr uint8_t MANUFACTURER_XXX = N;` from proto_constants.h.
/// MANUFACTURER_ID_MAX is a bound, not a manufacturer, and is deliberately excluded.
std::map<std::string, uint8_t> parse_cpp_manufacturer_constants() {
  const char *path = "components/home_io_control/proto_constants.h";
  std::ifstream file(path);
  EXPECT_TRUE(file.is_open()) << "Cannot open " << path << " — run tests from the project root.";
  if (!file.is_open())
    return {};

  static const std::string prefix = "static constexpr uint8_t MANUFACTURER_";
  std::map<std::string, uint8_t> entries;
  std::string line;
  while (std::getline(file, line)) {
    auto start = line.find(prefix);
    if (start == std::string::npos)
      continue;
    start += prefix.size();
    auto name_end = line.find_first_of(" =", start);
    if (name_end == std::string::npos)
      continue;
    std::string name = line.substr(start, name_end - start);
    if (name == "ID_MAX")
      continue;
    auto eq = line.find('=', name_end);
    auto semi = line.find(';', eq);
    if (eq == std::string::npos || semi == std::string::npos)
      continue;
    std::string val_str = line.substr(eq + 1, semi - eq - 1);
    auto vstart = val_str.find_first_not_of(" \t");
    auto vend = val_str.find_last_not_of(" \t");
    val_str = val_str.substr(vstart, vend - vstart + 1);
    entries[name] = static_cast<uint8_t>(std::stoul(val_str, nullptr, 0));
  }
  return entries;
}

std::string to_upper(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
  return s;
}

std::string to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}

}  // namespace

TEST(ManufacturerSync, PythonOptionsMatchCppConstants) {
  auto py_entries = test::parse_python_uint8_dict("components/home_io_control/__init__.py", "MANUFACTURER_OPTIONS");
  auto cpp_entries = parse_cpp_manufacturer_constants();
  ASSERT_GT(py_entries.size(), 10u) << "Parsed too few entries from __init__.py — parser may be broken.";
  ASSERT_GT(cpp_entries.size(), 10u) << "Parsed too few entries from proto_constants.h — parser may be broken.";

  for (const auto &[name, value] : py_entries) {
    const std::string cpp_name = to_upper(name);
    auto it = cpp_entries.find(cpp_name);
    ASSERT_NE(it, cpp_entries.end()) << "Python MANUFACTURER_OPTIONS[\"" << name << "\"] has no matching "
                                     << "MANUFACTURER_" << cpp_name << " constant in proto_constants.h.";
    EXPECT_EQ(it->second, value) << "MANUFACTURER_OPTIONS[\"" << name << "\"] = 0x" << std::hex
                                 << static_cast<int>(value) << " but proto_constants.h's MANUFACTURER_" << cpp_name
                                 << " = 0x" << std::hex << static_cast<int>(it->second) << ". Values must match.";
  }
}

TEST(ManufacturerSync, CppConstantsAreExposedInPython) {
  // The reverse direction: a manufacturer constant added to the C++ table but never given a
  // Python name is not a bug (validate_manufacturer()'s raw-hex escape hatch still reaches it),
  // but it is a missed usability win this test surfaces rather than lets go unnoticed.
  auto py_entries = test::parse_python_uint8_dict("components/home_io_control/__init__.py", "MANUFACTURER_OPTIONS");
  auto cpp_entries = parse_cpp_manufacturer_constants();
  ASSERT_GT(cpp_entries.size(), 10u) << "Parsed too few entries from proto_constants.h — parser may be broken.";

  for (const auto &[cpp_name, value] : cpp_entries) {
    const std::string py_name = to_lower(cpp_name);
    auto it = py_entries.find(py_name);
    ASSERT_NE(it, py_entries.end()) << "proto_constants.h's MANUFACTURER_" << cpp_name << " (0x" << std::hex
                                    << static_cast<int>(value) << ") has no matching \"" << py_name
                                    << "\" entry in Python MANUFACTURER_OPTIONS (__init__.py).";
    EXPECT_EQ(it->second, value) << "MANUFACTURER_OPTIONS[\"" << py_name << "\"] = 0x" << std::hex
                                 << static_cast<int>(it->second) << " but proto_constants.h's MANUFACTURER_" << cpp_name
                                 << " = 0x" << std::hex << static_cast<int>(value) << ". Values must match.";
  }
}

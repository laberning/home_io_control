#pragma once

/// @file python_dict_parser.h
/// @brief Shared parser for a top-level `NAME = { "key": 0xNN, ... }` dict in a Python source
/// file, used by this project's C++-side Python/C++ cross-language sync tests
/// (device_type_sync_test.cpp, manufacturer_sync_test.cpp).
///
/// Hand-rolled `std::string` scanning, not `ast` -- these tests run in the same binary as the C++
/// constants they check, which is why this style exists alongside the newer script-based sync
/// checks (scripts/check-tuning-sync.py, scripts/check-yaml-emitters.py, both `make lint`); see
/// either sync test's file header for the fuller note on why both forms are kept.

#include <gtest/gtest.h>

#include <fstream>
#include <map>
#include <string>

namespace test {

/// Parse a top-level `NAME = { "key": 0xNN, ... }` dict out of a Python source file.
///
/// Matches lines shaped like `"assa_abloy": 0x05,` between the `NAME = {` line and the matching
/// `}`. Skips blank lines and comment lines (first non-whitespace character `#`) inside the dict
/// body -- an ordinary explanatory comment (`# "future_vendor": 0x0D,  # not yet assigned`) must
/// never register as a phantom entry, which is exactly the bug this parser had before the two
/// near-identical copies of it were merged into this one.
/// @param path Path to the Python source file, relative to the project root.
/// @param dict_name The dict's variable name, e.g. "MANUFACTURER_OPTIONS".
/// @return name -> value map; empty (with a test failure already recorded via EXPECT_TRUE) if
///         `path` could not be opened.
inline std::map<std::string, uint8_t> parse_python_uint8_dict(const char *path, const char *dict_name) {
  std::ifstream file(path);
  EXPECT_TRUE(file.is_open()) << "Cannot open " << path << " — run tests from the project root.";
  if (!file.is_open())
    return {};

  std::map<std::string, uint8_t> entries;
  std::string line;
  bool in_dict = false;

  while (std::getline(file, line)) {
    if (!in_dict) {
      if (line.find(dict_name) != std::string::npos && line.find('{') != std::string::npos)
        in_dict = true;
      continue;
    }
    if (line.find('}') != std::string::npos)
      break;

    auto first = line.find_first_not_of(" \t");
    if (first == std::string::npos || line[first] == '#')
      continue;  // blank or comment-only line -- not a real entry

    // Parse lines like:  "assa_abloy": 0x05,
    auto q1 = line.find('"');
    if (q1 == std::string::npos)
      continue;
    auto q2 = line.find('"', q1 + 1);
    if (q2 == std::string::npos)
      continue;
    auto colon = line.find(':', q2);
    if (colon == std::string::npos)
      continue;
    std::string name = line.substr(q1 + 1, q2 - q1 - 1);
    std::string val_str = line.substr(colon + 1);
    // Trim whitespace and trailing comma
    auto start = val_str.find_first_not_of(" \t");
    if (start == std::string::npos)
      continue;
    auto end = val_str.find_last_not_of(" \t,");
    val_str = val_str.substr(start, end - start + 1);
    entries[name] = static_cast<uint8_t>(std::stoul(val_str, nullptr, 0));
  }
  return entries;
}

}  // namespace test

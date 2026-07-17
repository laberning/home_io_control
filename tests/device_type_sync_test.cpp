/// @file device_type_sync_test.cpp
/// @brief Verifies that yaml_device_type_name() and device_type_name() cover every entry
///        in the Python DEVICE_TYPE_OPTIONS dict, preventing C++/Python drift.
///
/// The test parses __init__.py at runtime so Python remains the single source of truth.

#include "proto_device_model.h"
#include "test_helpers.h"

#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using namespace esphome::home_io_control;

namespace {

struct DeviceTypeEntry {
  std::string name;
  uint8_t value;
};

/// Parse DEVICE_TYPE_OPTIONS from the Python __init__.py source file.
/// Matches lines like:  "roller_shutter": 0x02,
std::vector<DeviceTypeEntry> parse_python_device_type_options() {
  const char *path = "components/home_io_control/__init__.py";
  std::ifstream file(path);
  EXPECT_TRUE(file.is_open()) << "Cannot open " << path << " — run tests from the project root.";
  if (!file.is_open())
    return {};

  std::vector<DeviceTypeEntry> entries;
  std::string line;
  bool in_dict = false;

  while (std::getline(file, line)) {
    if (line.find("DEVICE_TYPE_OPTIONS") != std::string::npos && line.find('{') != std::string::npos) {
      in_dict = true;
      continue;
    }
    if (in_dict) {
      if (line.find('}') != std::string::npos)
        break;
      // Parse lines like:  "roller_shutter": 0x02,
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
      uint8_t val = static_cast<uint8_t>(std::stoul(val_str, nullptr, 0));
      entries.push_back({name, val});
    }
  }
  return entries;
}

}  // namespace

TEST(DeviceTypeSync, YamlDeviceTypeNameCoversAllPythonOptions) {
  auto entries = parse_python_device_type_options();
  ASSERT_GT(entries.size(), 15u) << "Parsed too few entries from __init__.py — parser may be broken.";

  for (const auto &entry : entries) {
    auto type = static_cast<DeviceType>(entry.value);
    const char *result = yaml_device_type_name(type);

    ASSERT_NE(result, nullptr) << "yaml_device_type_name() returns nullptr for \"" << entry.name << "\" (0x" << std::hex
                               << static_cast<int>(entry.value) << "). Add it to the switch in proto_frame.cpp.";

    EXPECT_EQ(std::string(result), entry.name)
        << "yaml_device_type_name(0x" << std::hex << static_cast<int>(entry.value) << ") returned \"" << result
        << "\" but Python DEVICE_TYPE_OPTIONS uses \"" << entry.name << "\". Names must match exactly.";
  }
}

TEST(DeviceTypeSync, DeviceTypeNameCoversAllEnumValues) {
  // Every value 0x01–0x18 in the DeviceType enum should have a meaningful name.
  for (uint8_t v = 0x01; v <= 0x18; v++) {
    auto type = static_cast<DeviceType>(v);
    const char *name = device_type_name(type);
    EXPECT_NE(name, nullptr) << "device_type_name() returned nullptr for 0x" << std::hex << static_cast<int>(v);
    if (name != nullptr) {
      EXPECT_STRNE(name, "unknown") << "device_type_name(0x" << std::hex << static_cast<int>(v)
                                    << ") returned \"unknown\" — add it to the switch in proto_frame.cpp.";
    }
  }
}

TEST(DeviceTypeSync, DeviceCapabilityClassCoversAllEnumValues) {
  // Every value 0x01–0x18 should map to a known capability class.
  for (uint8_t v = 0x01; v <= 0x18; v++) {
    auto type = static_cast<DeviceType>(v);
    auto cap = device_capability_class(type);
    EXPECT_NE(cap, DeviceCapabilityClass::UNKNOWN) << "device_capability_class(0x" << std::hex << static_cast<int>(v)
                                                   << ") returned UNKNOWN — add it to the switch in proto_frame.cpp.";
  }
}

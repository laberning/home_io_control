/// @file device_type_sync_test.cpp
/// @brief Verifies that yaml_device_type_name() and device_type_name() cover every entry
///        in the Python DEVICE_TYPE_OPTIONS dict, preventing C++/Python drift.
///
/// The test parses __init__.py at runtime so Python remains the single source of truth. This is
/// the older of two cross-language sync mechanisms this project uses (see manufacturer_sync_test.cpp
/// and python_dict_parser.h for the shared parser); newer table-to-table syncs are written as host
/// Python scripts under `scripts/` instead (e.g. scripts/check-yaml-emitters.py, `make lint`) — kept
/// here rather than ported because it runs in the same binary as the constants it checks.

#include "proto_device_model.h"
#include "support/python_dict_parser.h"
#include "test_helpers.h"

#include <cstdlib>
#include <string>

using namespace esphome::home_io_control;

TEST(DeviceTypeSync, YamlDeviceTypeNameCoversAllPythonOptions) {
  auto entries = test::parse_python_uint8_dict("components/home_io_control/__init__.py", "DEVICE_TYPE_OPTIONS");
  ASSERT_GT(entries.size(), 15u) << "Parsed too few entries from __init__.py — parser may be broken.";

  for (const auto &[name, value] : entries) {
    auto type = static_cast<DeviceType>(value);
    const char *result = yaml_device_type_name(type);

    ASSERT_NE(result, nullptr) << "yaml_device_type_name() returns nullptr for \"" << name << "\" (0x" << std::hex
                               << static_cast<int>(value) << "). Add it to the switch in proto_device_model.cpp.";

    EXPECT_EQ(std::string(result), name) << "yaml_device_type_name(0x" << std::hex << static_cast<int>(value)
                                         << ") returned \"" << result << "\" but Python DEVICE_TYPE_OPTIONS uses \""
                                         << name << "\". Names must match exactly.";
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
                                    << ") returned \"unknown\" — add it to the switch in proto_device_model.cpp.";
    }
  }
}

TEST(DeviceTypeSync, DeviceCapabilityClassCoversAllEnumValues) {
  // Every value 0x01–0x18 should map to a known capability class.
  for (uint8_t v = 0x01; v <= 0x18; v++) {
    auto type = static_cast<DeviceType>(v);
    auto cap = device_capability_class(type);
    EXPECT_NE(cap, DeviceCapabilityClass::UNKNOWN)
        << "device_capability_class(0x" << std::hex << static_cast<int>(v)
        << ") returned UNKNOWN — add it to the switch in proto_device_model.cpp.";
  }
}

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
#include <set>
#include <string>

using namespace esphome::home_io_control;

namespace {

/// DeviceType enumerators deliberately NOT exposed in __init__.py's DEVICE_TYPE_OPTIONS, each
/// with the reason. Keeping this list here (rather than only as a comment in the header) is what
/// makes the reverse sync check below able to tell "withheld on purpose" from "forgotten".
struct AllowlistedType {
  DeviceType type;
  const char *reason;
};
constexpr AllowlistedType kNotYamlSelectable[] = {
    {DeviceType::BEACON, "discovery pseudo-type (unpaired/announcement), not a configurable actuator"},
    {DeviceType::VENTILATION_POINT, "climate/ventilation platform unbuilt; not selectable yet"},
    {DeviceType::EXTERIOR_HEATING, "capability class CLIMATE, which no platform consumes yet"},
    {DeviceType::HEAT_PUMP, "capability class CLIMATE, which no platform consumes yet"},
};

/// True iff `v` is a real, named DeviceType enumerator. device_type_name() has no default: label,
/// so -Wswitch forces a case for every enumerator, and it returns "unknown" only for UNKNOWN and
/// for values with no enumerator at all. That makes this an exact membership test with no
/// hand-maintained list to keep in sync: a newly added enumerator gains a name (forced by
/// -Wswitch) and is picked up here automatically — including one added above SWINGING_SHUTTER,
/// which a hard-coded 0x18 upper bound would silently skip.
bool is_named_device_type(uint8_t v) { return std::string(device_type_name(static_cast<DeviceType>(v))) != "unknown"; }

}  // namespace

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
  // Every value 0x01–0x18 in the DeviceType enum should have a meaningful name. Unlike the two
  // reverse tests below, this one cannot use is_named_device_type() to find enumerators — a "name
  // is 'unknown'" bug is exactly what it exists to catch — so it keeps an explicit range. The
  // enum is contiguous through SWINGING_SHUTTER (0x18); extend this bound if it grows. -Wswitch on
  // device_type_name() (no default: label) is the primary guard that every enumerator is handled.
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

TEST(DeviceTypeSync, EveryEnumValueIsYamlSelectableOrAllowlisted) {
  // Reverse direction of YamlDeviceTypeNameCoversAllPythonOptions: every DeviceType enumerator
  // must EITHER appear in Python DEVICE_TYPE_OPTIONS or be in kNotYamlSelectable above with a
  // reason. This closes the direction that let VENTILATION_POINT/EXTERIOR_HEATING/HEAT_PUMP
  // drift out of YAML reach with nothing noticing.
  auto entries = test::parse_python_uint8_dict("components/home_io_control/__init__.py", "DEVICE_TYPE_OPTIONS");
  ASSERT_GT(entries.size(), 15u) << "Parsed too few entries from __init__.py — parser may be broken.";

  std::set<uint8_t> python_values;
  for (const auto &[name, value] : entries)
    python_values.insert(value);

  std::set<uint8_t> allowlisted;
  for (const auto &entry : kNotYamlSelectable)
    allowlisted.insert(static_cast<uint8_t>(entry.type));

  // Scan the whole uint8_t range and let is_named_device_type() pick out the real enumerators, so
  // an enumerator added above SWINGING_SHUTTER is checked automatically rather than falling
  // outside a hard-coded 0x18 bound.
  for (uint16_t v = 0x01; v <= 0xFF; v++) {
    if (!is_named_device_type(static_cast<uint8_t>(v)))
      continue;
    const bool in_python = python_values.count(static_cast<uint8_t>(v)) != 0;
    const bool in_allowlist = allowlisted.count(static_cast<uint8_t>(v)) != 0;
    EXPECT_TRUE(in_python || in_allowlist)
        << "DeviceType 0x" << std::hex << static_cast<int>(v) << " (" << device_type_name(static_cast<DeviceType>(v))
        << ") is neither in Python DEVICE_TYPE_OPTIONS nor in kNotYamlSelectable. Add it to "
        << "__init__.py's DEVICE_TYPE_OPTIONS and yaml_device_type_name(), or allowlist it with a reason.";
    EXPECT_FALSE(in_python && in_allowlist)
        << "DeviceType 0x" << std::hex << static_cast<int>(v)
        << " is both YAML-selectable and allowlisted as not-selectable — resolve the contradiction.";
  }
}

TEST(DeviceTypeSync, DeviceCapabilityClassCoversAllEnumValues) {
  // Every named DeviceType enumerator should map to a known capability class. Scanning the full
  // range and skipping non-enumerators (rather than looping a hard-coded 0x01..0x18) means an
  // enumerator added above SWINGING_SHUTTER is checked here without touching the bound.
  for (uint16_t v = 0x01; v <= 0xFF; v++) {
    if (!is_named_device_type(static_cast<uint8_t>(v)))
      continue;
    auto type = static_cast<DeviceType>(v);
    auto cap = device_capability_class(type);
    EXPECT_NE(cap, DeviceCapabilityClass::UNKNOWN)
        << "device_capability_class(0x" << std::hex << static_cast<int>(v)
        << ") returned UNKNOWN — add it to the switch in proto_device_model.cpp.";
  }
}

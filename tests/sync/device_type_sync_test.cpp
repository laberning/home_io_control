/// @file device_type_sync_test.cpp
/// @brief Verifies that the node-type table (proto_device_types.cpp) and the Python DEVICE_TYPE_OPTIONS
///        dict agree, preventing C++/Python drift.
///
/// The test parses hub_validators.py at runtime so Python remains the single source of truth. This is
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
#include <vector>

using namespace esphome::home_io_control;

namespace {

/// DeviceType enumerators deliberately NOT exposed in hub_validators.py's DEVICE_TYPE_OPTIONS, each
/// with the reason. Keeping this list here (rather than only as a comment in the header) is what
/// makes the reverse sync check below able to tell "withheld on purpose" from "forgotten".
struct AllowlistedType {
  DeviceType type;
  const char *reason;
};
constexpr AllowlistedType kNotYamlSelectable[] = {
    {DeviceType::SECURE_CONFIGURATION_DEVICE, "a commissioning tool, not an actuator any platform drives"},
    {DeviceType::BEACON, "discovery pseudo-type (unpaired/announcement), not a configurable actuator"},
    {DeviceType::VENTILATION_POINT, "ventilation platform unbuilt; not selectable yet"},
    {DeviceType::ZONE_CONTROL, "no platform drives it yet"},
    {DeviceType::SIREN, "no platform drives it yet"},
    {DeviceType::DOMESTIC_HOT_WATER, "no platform drives it yet"},
    {DeviceType::HEAT_RECOVERY_VENTILATION, "no platform drives it yet"},
    {DeviceType::CENTRAL_HOUSE_CONTROL, "a controller class, not a device a platform drives"},
    {DeviceType::TEST_AND_EVALUATION, "test equipment, not a device a platform drives"},
    {DeviceType::REMOTE_CONTROLLER, "a controller class, not a device a platform drives"},
};

/// Named DeviceType enumerators that deliberately map to DeviceCapabilityClass::UNKNOWN, each with
/// the reason: no ESPHome entity family fits them.
constexpr AllowlistedType kNoEntityClass[] = {
    {DeviceType::SECURE_CONFIGURATION_DEVICE, "a commissioning tool, not an actuator"},
    {DeviceType::ZONE_CONTROL, "no entity family yet"},
    {DeviceType::SIREN, "no entity family yet"},
    {DeviceType::DOMESTIC_HOT_WATER, "no entity family yet"},
    {DeviceType::HEAT_RECOVERY_VENTILATION, "no entity family yet"},
    {DeviceType::CENTRAL_HOUSE_CONTROL, "a controller class"},
    {DeviceType::TEST_AND_EVALUATION, "test equipment"},
    {DeviceType::REMOTE_CONTROLLER, "a controller class"},
};

/// Every DeviceType enumerator, listed by hand: a node type missing from the table is exactly what
/// the name test below has to catch, so it cannot use the table to find the enumerators.
constexpr DeviceType kAllTypes[] = {
    DeviceType::INTERIOR_VENETIAN_BLIND,
    DeviceType::ROLLER_SHUTTER,
    DeviceType::VERTICAL_EXTERIOR_AWNING,
    DeviceType::WINDOW_OPENER,
    DeviceType::GARAGE_OPENER,
    DeviceType::LIGHT,
    DeviceType::GATE_OPENER,
    DeviceType::ROLLING_DOOR_OPENER,
    DeviceType::LOCK,
    DeviceType::VERTICAL_INTERIOR_BLIND,
    DeviceType::SECURE_CONFIGURATION_DEVICE,
    DeviceType::BEACON,
    DeviceType::DUAL_ROLLER_SHUTTER,
    DeviceType::HEATING_TEMPERATURE_INTERFACE,
    DeviceType::ON_OFF_SWITCH,
    DeviceType::HORIZONTAL_AWNING,
    DeviceType::EXTERIOR_VENETIAN_BLIND,
    DeviceType::LOUVRE_BLIND,
    DeviceType::CURTAIN_TRACK,
    DeviceType::VENTILATION_POINT,
    DeviceType::EXTERIOR_HEATING,
    DeviceType::HEAT_PUMP,
    DeviceType::INTRUSION_ALARM,
    DeviceType::SWINGING_SHUTTER,
    DeviceType::SLIDING_WINDOW,
    DeviceType::ZONE_CONTROL,
    DeviceType::BIOCLIMATIC_PERGOLA,
    DeviceType::SIREN,
    DeviceType::DOMESTIC_HOT_WATER,
    DeviceType::ELECTRICAL_HEATER,
    DeviceType::HEAT_RECOVERY_VENTILATION,
    DeviceType::CENTRAL_HOUSE_CONTROL,
    DeviceType::TEST_AND_EVALUATION,
    DeviceType::REMOTE_CONTROLLER,
};

template<size_t N> bool listed(const AllowlistedType (&list)[N], DeviceType type) {
  for (const auto &entry : list) {
    if (entry.type == type)
      return true;
  }
  return false;
}

/// Highest node-type value the wire field can carry (ten bits).
constexpr uint16_t kMaxNodeType = 1023;

/// True iff `v` is a real, named DeviceType enumerator, as the table reports it.
bool is_named_device_type(uint16_t v) { return is_known_device_type(v); }

}  // namespace

TEST(DeviceTypeSync, YamlDeviceTypeNameCoversAllPythonOptions) {
  auto entries = test::parse_python_uint8_dict("components/home_io_control/hub_validators.py", "DEVICE_TYPE_OPTIONS");
  ASSERT_GT(entries.size(), 15u) << "Parsed too few entries from hub_validators.py — parser may be broken.";

  for (const auto &[name, value] : entries) {
    auto type = static_cast<DeviceType>(value);
    const char *result = yaml_device_type_name(type);

    ASSERT_NE(result, nullptr) << "yaml_device_type_name() returns nullptr for \"" << name << "\" (0x" << std::hex
                               << static_cast<int>(value) << "). Add it to the table in proto_device_types.cpp.";

    EXPECT_EQ(std::string(result), name) << "yaml_device_type_name(0x" << std::hex << static_cast<int>(value)
                                         << ") returned \"" << result << "\" but Python DEVICE_TYPE_OPTIONS uses \""
                                         << name << "\". Names must match exactly.";
  }
}

TEST(DeviceTypeSync, DeviceTypeNameCoversAllEnumValues) {
  // Every DeviceType enumerator has a table row with a real name. kAllTypes is the hand-kept
  // list of enumerators; the scan below proves it is complete by checking that the table knows no
  // node type outside it.
  for (const DeviceType type : kAllTypes) {
    const char *name = device_type_name(type);
    ASSERT_NE(name, nullptr);
    EXPECT_STRNE(name, "unknown") << "device_type_name(" << static_cast<unsigned>(type)
                                  << ") returned \"unknown\" — add a row to the table in proto_device_types.cpp.";
    EXPECT_TRUE(is_named_device_type(static_cast<uint16_t>(type)));
  }
  size_t table_types = 0;
  for (uint16_t v = 1; v <= kMaxNodeType; v++) {
    if (is_named_device_type(v))
      table_types++;
  }
  EXPECT_EQ(table_types, sizeof(kAllTypes) / sizeof(kAllTypes[0]))
      << "the table names a node type that kAllTypes (and so the enum) lacks, or the reverse";
}

TEST(DeviceTypeSync, EveryEnumValueIsYamlSelectableOrAllowlisted) {
  // Reverse direction of YamlDeviceTypeNameCoversAllPythonOptions: every DeviceType enumerator
  // must EITHER appear in Python DEVICE_TYPE_OPTIONS or be in kNotYamlSelectable above with a
  // reason, and the two must agree with the table's YAML_SELECTABLE flag.
  auto entries = test::parse_python_uint8_dict("components/home_io_control/hub_validators.py", "DEVICE_TYPE_OPTIONS");
  ASSERT_GT(entries.size(), 15u) << "Parsed too few entries from hub_validators.py — parser may be broken.";

  std::set<uint16_t> python_values;
  for (const auto &[name, value] : entries)
    python_values.insert(value);

  for (const DeviceType type : kAllTypes) {
    const auto v = static_cast<uint16_t>(type);
    const bool in_python = python_values.count(v) != 0;
    const bool in_allowlist = listed(kNotYamlSelectable, type);
    EXPECT_TRUE(in_python || in_allowlist)
        << "DeviceType " << v << " (" << device_type_name(type)
        << ") is neither in Python DEVICE_TYPE_OPTIONS nor in kNotYamlSelectable. Add it to "
        << "hub_validators.py's DEVICE_TYPE_OPTIONS and the table's YAML_SELECTABLE flag, or allowlist it with a "
           "reason.";
    EXPECT_FALSE(in_python && in_allowlist)
        << "DeviceType " << v
        << " is both YAML-selectable and allowlisted as not-selectable — resolve the contradiction.";
    EXPECT_EQ(yaml_device_type_name(type) != nullptr, in_python)
        << "DeviceType " << v << " (" << device_type_name(type) << "): the table's YAML_SELECTABLE flag disagrees with "
        << "DEVICE_TYPE_OPTIONS.";
  }
}

TEST(DeviceTypeSync, DeviceCapabilityClassCoversAllEnumValues) {
  // Every named node type maps to an entity class, unless it is listed in kNoEntityClass with a reason.
  for (const DeviceType type : kAllTypes) {
    const auto cap = device_capability_class(type);
    if (listed(kNoEntityClass, type)) {
      EXPECT_EQ(cap, DeviceCapabilityClass::UNKNOWN)
          << device_type_name(type) << " is allowlisted as having no entity class but the table gives it one.";
    } else {
      EXPECT_NE(cap, DeviceCapabilityClass::UNKNOWN)
          << "device_capability_class(" << static_cast<unsigned>(type) << ") returned UNKNOWN — set a class in the "
          << "table in proto_device_types.cpp, or allowlist it in kNoEntityClass with a reason.";
    }
  }
}

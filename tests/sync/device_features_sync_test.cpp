/// @file device_features_sync_test.cpp
/// @brief Verifies that the Python mirror of the capability word (hub_validators.py `FEATURE_*`,
///        `KIND_EXPERIMENTAL`, `DEVICE_FEATURES`, `NAMED_POSITION_FEATURES`) agrees with the C++
///        node-type table, bit for bit and row for row, preventing C++/Python drift.
///
/// The C++ table (proto_device_types.cpp) is authoritative; the test parses hub_validators.py at
/// runtime, like the other sync tests (see device_type_sync_test.cpp and python_dict_parser.h).

#include "proto_device_features.h"
#include "proto_device_types.h"
#include "support/python_dict_parser.h"

#include "device_table_walk.h"
#include "test_helpers.h"

#include <map>
#include <set>
#include <string>
#include <utility>

using namespace esphome::home_io_control;

namespace {

constexpr const char *VALIDATORS = "components/home_io_control/hub_validators.py";

/// Bit 15 of the mirrored word: Python's copy of device_type_flags::EXPERIMENTAL.
constexpr DeviceFeatures KIND_EXPERIMENTAL_BIT = 1U << 15U;

/// Every capability bit, by the name its Python constant carries after the `FEATURE_` prefix. Listed
/// by hand: a bit added to proto_device_features.h and missing here (and in Python) fails the
/// bit-by-bit check below.
const std::map<std::string, DeviceFeatures> CPP_FEATURE_BITS = {
    {"MAIN_PARAMETER_ONLY", device_feature::MAIN_PARAMETER_ONLY},
    {"LEVELS", device_feature::LEVELS},
    {"TILT", device_feature::TILT},
    {"FAVORITE", device_feature::FAVORITE},
    {"VENT", device_feature::VENT},
    {"PARTIAL", device_feature::PARTIAL},
    {"PEDESTRIAN", device_feature::PEDESTRIAN},
    {"EXTENDED_POSITION_FRAME", device_feature::EXTENDED_POSITION_FRAME},
    {"WINDOW_COMPANIONS", device_feature::WINDOW_COMPANIONS},
    {"INVERTED", device_feature::INVERTED},
};

/// The word Python keeps for a row: the capability bits plus bit 15 for the row's EXPERIMENTAL flag.
/// A type YAML cannot name (a remote controller) has no Python entry at all, so its rows mirror
/// nothing: nothing declares such a type in a config.
DeviceFeatures mirrored_word(DeviceType type, uint8_t subtype) {
  const DeviceTypeInfo *type_row = find_exact_device_type_info(type, 0);
  const bool yaml_named = type_row != nullptr && (type_row->flags & device_type_flags::YAML_SELECTABLE) != 0;
  const bool experimental = yaml_named && device_is_experimental(type, subtype);
  return static_cast<DeviceFeatures>(device_features(type, subtype) | (experimental ? KIND_EXPERIMENTAL_BIT : 0U));
}

/// hub_validators.device_features() re-implemented over the parsed tables, so the test checks the
/// data, not a second copy of the lookup in another language.
DeviceFeatures python_features(const std::map<std::string, uint8_t> &type_values,
                               const std::map<std::string, uint16_t> &table, DeviceType type, uint8_t subtype) {
  std::string name;
  for (const auto &[candidate, value] : type_values) {
    if (value == static_cast<uint8_t>(type) && static_cast<unsigned>(type) <= 0xFF)
      name = candidate;
  }
  if (!name.empty()) {
    const auto exact = table.find(subtype == 0 ? name : name + "/" + std::to_string(subtype));
    if (exact != table.end())
      return exact->second;
  }
  const auto base_it = name.empty() ? table.end() : table.find(name);
  const DeviceFeatures base = base_it == table.end() ? DeviceFeatures{0} : base_it->second;
  return subtype == SUBTYPE_MAIN_PARAMETER_ONLY ? main_parameter_only_variant(base) : base;
}

}  // namespace

TEST(DeviceFeaturesSync, EveryBitHasTheSameValueInBothLanguages) {
  const auto python = test::parse_python_uint16_assignments(VALIDATORS, "FEATURE_");
  ASSERT_FALSE(python.empty()) << "FEATURE_* not parsed from hub_validators.py";
  for (const auto &[name, bit] : CPP_FEATURE_BITS) {
    ASSERT_TRUE(python.count(name)) << "FEATURE_" << name << " is missing from hub_validators.py";
    EXPECT_EQ(python.at(name), bit) << name;
  }
  for (const auto &[name, value] : python)
    EXPECT_TRUE(CPP_FEATURE_BITS.count(name)) << "FEATURE_" << name << " has no C++ bit";
}

TEST(DeviceFeaturesSync, TheWholeWordOfEveryRowAgrees) {
  const auto type_values = test::parse_python_uint8_dict(VALIDATORS, "DEVICE_TYPE_OPTIONS");
  const auto table = test::parse_python_uint16_dict(VALIDATORS, "DEVICE_FEATURES");
  ASSERT_FALSE(table.empty()) << "DEVICE_FEATURES not parsed from hub_validators.py";

  // Every (type, subtype) the table knows, plus subtypes 0 and 58 of each type, which the lookup
  // answers without a row of their own.
  std::set<std::pair<DeviceType, uint8_t>> kinds;
  test::for_each_table_row([&kinds](const DeviceTypeInfo &row) {
    kinds.emplace(row.type, row.subtype);
    kinds.emplace(row.type, 0);
    kinds.emplace(row.type, SUBTYPE_MAIN_PARAMETER_ONLY);
  });
  for (const auto &[type, subtype] : kinds) {
    EXPECT_EQ(python_features(type_values, table, type, subtype), mirrored_word(type, subtype))
        << format_device_ids(type, subtype);
  }
}

TEST(DeviceFeaturesSync, EveryPythonEntryNamesARealRowOfAYamlType) {
  const auto type_values = test::parse_python_uint8_dict(VALIDATORS, "DEVICE_TYPE_OPTIONS");
  const auto table = test::parse_python_uint16_dict(VALIDATORS, "DEVICE_FEATURES");
  for (const auto &[key, word] : table) {
    const auto slash = key.find('/');
    const std::string name = key.substr(0, slash);
    const uint8_t subtype = slash == std::string::npos ? 0 : static_cast<uint8_t>(std::stoul(key.substr(slash + 1)));
    ASSERT_TRUE(type_values.count(name)) << key << ": " << name << " is not a DEVICE_TYPE_OPTIONS name";
    const DeviceTypeInfo *row = find_exact_device_type_info(static_cast<DeviceType>(type_values.at(name)), subtype);
    ASSERT_NE(row, nullptr) << key << " names no C++ row";
    EXPECT_EQ(mirrored_word(row->type, row->subtype), word) << key;
  }
}

TEST(DeviceFeaturesSync, EveryRowThatHasCapabilitiesOrIsExperimentalHasAnEntryWhenItsTypeIsYamlNamed) {
  const auto type_values = test::parse_python_uint8_dict(VALIDATORS, "DEVICE_TYPE_OPTIONS");
  const auto table = test::parse_python_uint16_dict(VALIDATORS, "DEVICE_FEATURES");
  test::for_each_table_row([&](const DeviceTypeInfo &row) {
    const bool experimental = (row.flags & device_type_flags::EXPERIMENTAL) != 0;
    if (row.features == 0 && !experimental)
      return;
    const DeviceTypeInfo *type_row = find_exact_device_type_info(row.type, 0);
    ASSERT_NE(type_row, nullptr) << format_device_ids(row.type, row.subtype);
    // A type YAML cannot name (a remote controller) has no entry: nothing declares it in a config.
    if ((type_row->flags & device_type_flags::YAML_SELECTABLE) == 0)
      return;
    std::string name;
    for (const auto &[candidate, value] : type_values) {
      if (value == static_cast<uint8_t>(row.type))
        name = candidate;
    }
    ASSERT_FALSE(name.empty()) << format_device_ids(row.type, row.subtype);
    const std::string key = row.subtype == 0 ? name : name + "/" + std::to_string(row.subtype);
    EXPECT_TRUE(table.count(key)) << key << " is missing from DEVICE_FEATURES";
  });
}

TEST(DeviceFeaturesSync, TheExperimentalMarkerIsBit15AndNoFeatureBitUsesIt) {
  const auto kind = test::parse_python_uint16_assignments(VALIDATORS, "KIND_");
  ASSERT_TRUE(kind.count("EXPERIMENTAL")) << "KIND_EXPERIMENTAL is missing from hub_validators.py";
  EXPECT_EQ(kind.at("EXPERIMENTAL"), KIND_EXPERIMENTAL_BIT);
  for (const auto &[name, bit] : CPP_FEATURE_BITS)
    EXPECT_EQ(bit & KIND_EXPERIMENTAL_BIT, 0u) << name << " collides with the experimental marker";
}

TEST(DeviceFeaturesSync, NamedPositionFeaturesAreTheCppNamedPositions) {
  const auto python = test::parse_python_uint16_dict(VALIDATORS, "NAMED_POSITION_FEATURES");
  ASSERT_FALSE(python.empty()) << "NAMED_POSITION_FEATURES not parsed from hub_validators.py";
  EXPECT_EQ(python.size(), std::size(NAMED_POSITIONS)) << "a named position is missing on one side";
  for (const NamedPosition &named : NAMED_POSITIONS) {
    ASSERT_TRUE(python.count(named.name)) << named.name << " is missing from NAMED_POSITION_FEATURES";
    EXPECT_EQ(python.at(named.name), named.feature) << named.name;
  }
}

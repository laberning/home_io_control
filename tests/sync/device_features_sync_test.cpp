/// @file device_features_sync_test.cpp
/// @brief Verifies that the Python mirror of the device capabilities (hub_validators.py `FEATURES`,
///        `DEVICE_FEATURES`, `EXPERIMENTAL_KINDS`, `NAMED_POSITION_FEATURES`, `DEVICE_MODEL_OPTIONS`)
///        agrees with the C++ node-type table by capability name and row for row (model rows
///        included), and that climate.py's `OPT_IN_READ_FEATURES` gates each opt-in register read
///        by the capability HEATING_OPT_IN_READS names, preventing C++/Python drift.
///
/// The C++ table (proto_device_types.cpp) is authoritative; the test parses hub_validators.py at
/// runtime, like the other sync tests (see device_type_sync_test.cpp and python_dict_parser.h).

#include "proto_device_features.h"
#include "proto_device_types.h"
#include "proto_heating.h"
#include "proto_heating_profiles.h"
#include "support/python_dict_parser.h"

#include "device_table_walk.h"
#include "test_helpers.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace esphome::home_io_control;

namespace {

constexpr const char *VALIDATORS = "components/home_io_control/hub_validators.py";
constexpr const char *CLIMATE = "components/home_io_control/climate.py";

using NameSet = std::set<std::string>;

/// Every capability by the name the Python mirror writes. Listed by hand: an enumerator added to
/// proto_device_features.h and missing here (and in Python) fails the first test below.
const std::map<std::string, DeviceFeature> CPP_FEATURES = {
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
    {"WATER_HEATER", device_feature::WATER_HEATER},
    {"NIGHT_LIGHT", device_feature::NIGHT_LIGHT},
    {"TEMPERATURE_RANGE", device_feature::TEMPERATURE_RANGE},
    {"CENTRAL_HEATING_MODE", device_feature::CENTRAL_HEATING_MODE},
    {"OCCUPANCY", device_feature::OCCUPANCY},
    {"ENERGY_METER", device_feature::ENERGY_METER},
    {"RUN_TIMERS", device_feature::RUN_TIMERS},
};

/// The names of the capabilities in a set.
NameSet names_of(DeviceFeatures features) {
  NameSet names;
  for (const auto &[name, feature] : CPP_FEATURES) {
    if (has_feature(features, feature))
      names.insert(name);
  }
  return names;
}

/// The set of the capabilities a Python entry names; a name C++ lacks fails the test.
DeviceFeatures features_of(const NameSet &names) {
  DeviceFeatures features;
  for (const std::string &name : names) {
    const auto it = CPP_FEATURES.find(name);
    if (it == CPP_FEATURES.end()) {
      ADD_FAILURE() << name << " is named in hub_validators.py but is no C++ capability";
      continue;
    }
    features |= it->second;
  }
  return features;
}

/// The Python name of a node type (its DEVICE_TYPE_OPTIONS key), or "" for one YAML has no name for.
std::string python_type_name(const std::map<std::string, uint8_t> &type_values, DeviceType type) {
  if (static_cast<unsigned>(type) > 0xFF)
    return "";
  for (const auto &[name, value] : type_values) {
    if (value == static_cast<uint8_t>(type))
      return name;
  }
  return "";
}

/// The Python key of an exact row: "<type name>" for subtype 0, else "<type name>/<subtype>".
std::string row_key(const std::string &name, uint8_t subtype) {
  return subtype == 0 ? name : name + "/" + std::to_string(subtype);
}

/// The capabilities C++ answers for a kind: the names the mirror has to list.
NameSet cpp_names(DeviceType type, uint8_t subtype, DeviceModel model = DeviceModel::NONE) {
  return names_of(device_features(type, subtype, model));
}

/// The Python key of a model row: "<type name>/<subtype>/<model name>".
std::string model_key(const std::map<std::string, uint8_t> &type_values, const DeviceTypeInfo &model_row) {
  const std::string name = python_type_name(type_values, model_row.type);
  return name.empty() ? "" : name + "/" + std::to_string(model_row.subtype) + "/" + model_row.name;
}

/// hub_validators.device_features() re-implemented over the parsed table, so the test checks the
/// data, not a second copy of the lookup in another language.
NameSet python_features(const std::map<std::string, uint8_t> &type_values, const std::map<std::string, NameSet> &table,
                        DeviceType type, uint8_t subtype) {
  const std::string name = python_type_name(type_values, type);
  if (!name.empty()) {
    const auto exact = table.find(row_key(name, subtype));
    if (exact != table.end())
      return exact->second;
  }
  const auto base_it = name.empty() ? table.end() : table.find(name);
  const NameSet base = base_it == table.end() ? NameSet{} : base_it->second;
  return subtype == SUBTYPE_MAIN_PARAMETER_ONLY ? names_of(main_parameter_only_variant(features_of(base))) : base;
}

}  // namespace

TEST(DeviceFeaturesSync, EveryCapabilityIsNamedInBothLanguagesInTheSameOrder) {
  // C++ side: the hand map covers the whole enumeration, one name per enumerator.
  ASSERT_EQ(CPP_FEATURES.size(), static_cast<size_t>(DeviceFeature::COUNT))
      << "a DeviceFeature enumerator is missing from CPP_FEATURES (or listed twice)";
  std::vector<std::string> cpp_in_enum_order(CPP_FEATURES.size());
  for (const auto &[name, feature] : CPP_FEATURES) {
    ASSERT_LT(static_cast<size_t>(feature), cpp_in_enum_order.size()) << name;
    EXPECT_TRUE(cpp_in_enum_order[static_cast<size_t>(feature)].empty()) << name << " shares an enumerator";
    cpp_in_enum_order[static_cast<size_t>(feature)] = name;
  }
  const auto python = test::parse_python_string_collection(VALIDATORS, "FEATURES");
  ASSERT_FALSE(python.empty()) << "FEATURES not parsed from hub_validators.py";
  EXPECT_EQ(python, cpp_in_enum_order) << "FEATURES must list the C++ capabilities in enumeration order";
}

TEST(DeviceFeaturesSync, TheMainParameterOnlySubtypeHasTheSameValueInBothLanguages) {
  const auto python = test::parse_python_uint8_assignments(VALIDATORS, "SUBTYPE_");
  ASSERT_TRUE(python.count("MAIN_PARAMETER_ONLY")) << "SUBTYPE_MAIN_PARAMETER_ONLY not parsed from hub_validators.py";
  EXPECT_EQ(python.at("MAIN_PARAMETER_ONLY"), SUBTYPE_MAIN_PARAMETER_ONLY);
}

TEST(DeviceFeaturesSync, TheCapabilitiesOfEveryRowAgree) {
  const auto type_values = test::parse_python_uint8_dict(VALIDATORS, "DEVICE_TYPE_OPTIONS");
  const auto table = test::parse_python_name_set_dict(VALIDATORS, "DEVICE_FEATURES");
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
    // A type YAML cannot name (a remote controller) has no Python entry at all: nothing declares such
    // a type in a config, so its rows mirror nothing.
    if (python_type_name(type_values, type).empty())
      continue;
    EXPECT_EQ(python_features(type_values, table, type, subtype), cpp_names(type, subtype))
        << format_device_ids(type, subtype);
  }
}

TEST(DeviceFeaturesSync, EveryPythonEntryNamesARealRowOfAYamlType) {
  const auto type_values = test::parse_python_uint8_dict(VALIDATORS, "DEVICE_TYPE_OPTIONS");
  const auto table = test::parse_python_name_set_dict(VALIDATORS, "DEVICE_FEATURES");
  const auto models = test::parse_python_uint8_dict(VALIDATORS, "DEVICE_MODEL_OPTIONS");
  for (const auto &[key, names] : table) {
    const auto model_slash = key.find('/', key.find('/') + 1);
    if (key.find('/') != std::string::npos && model_slash != std::string::npos) {
      // "<name>/<subtype>/<model>": a model row.
      const std::string name = key.substr(0, key.find('/'));
      const auto subtype = static_cast<uint8_t>(std::stoul(key.substr(key.find('/') + 1)));
      const std::string model_name = key.substr(model_slash + 1);
      ASSERT_TRUE(type_values.count(name)) << key;
      ASSERT_TRUE(models.count(model_name)) << key << ": " << model_name << " is not a DEVICE_MODEL_OPTIONS name";
      const auto type = static_cast<DeviceType>(type_values.at(name));
      const auto model = static_cast<DeviceModel>(models.at(model_name));
      const DeviceTypeInfo *row = find_device_model_info(type, subtype, model);
      ASSERT_NE(row, nullptr) << key << " names no C++ model row";
      EXPECT_STREQ(row->name, model_name.c_str()) << key;
      EXPECT_EQ(cpp_names(type, subtype, model), names) << key;
      continue;
    }
    const auto slash = key.find('/');
    const std::string name = key.substr(0, slash);
    const uint8_t subtype = slash == std::string::npos ? 0 : static_cast<uint8_t>(std::stoul(key.substr(slash + 1)));
    ASSERT_TRUE(type_values.count(name)) << key << ": " << name << " is not a DEVICE_TYPE_OPTIONS name";
    const DeviceTypeInfo *row = find_exact_device_type_info(static_cast<DeviceType>(type_values.at(name)), subtype);
    ASSERT_NE(row, nullptr) << key << " names no C++ row";
    EXPECT_EQ(cpp_names(row->type, row->subtype), names) << key;
  }
}

TEST(DeviceFeaturesSync, EveryRowWithCapabilitiesHasAnEntryWhenItsTypeIsYamlNamed) {
  const auto type_values = test::parse_python_uint8_dict(VALIDATORS, "DEVICE_TYPE_OPTIONS");
  const auto table = test::parse_python_name_set_dict(VALIDATORS, "DEVICE_FEATURES");
  test::for_each_table_row([&](const DeviceTypeInfo &row) {
    if (row.features.empty())
      return;
    // A type YAML cannot name (a remote controller) has no entry: nothing declares it in a config.
    const std::string name = python_type_name(type_values, row.type);
    if (name.empty())
      return;
    const std::string key = row_key(name, row.subtype);
    EXPECT_TRUE(table.count(key)) << key << " is missing from DEVICE_FEATURES";
  });
}

TEST(DeviceFeaturesSync, TheExperimentalKindsAreTheCppRowsWithTheExperimentalFlag) {
  const auto type_values = test::parse_python_uint8_dict(VALIDATORS, "DEVICE_TYPE_OPTIONS");
  const auto python_list = test::parse_python_string_collection(VALIDATORS, "EXPERIMENTAL_KINDS");
  const NameSet python(python_list.begin(), python_list.end());
  EXPECT_EQ(python.size(), python_list.size()) << "EXPERIMENTAL_KINDS lists a kind twice";

  // A type YAML cannot name has no kind a config could declare, so it is not mirrored.
  NameSet cpp;
  test::for_each_table_row([&](const DeviceTypeInfo &row) {
    const std::string name = python_type_name(type_values, row.type);
    if (!name.empty() && device_is_experimental(row.type, row.subtype))
      cpp.insert(row_key(name, row.subtype));
  });
  EXPECT_EQ(python, cpp);
}

TEST(DeviceFeaturesSync, NamedPositionFeaturesAreTheCppNamedPositions) {
  const auto python = test::parse_python_ordered_dict(VALIDATORS, "NAMED_POSITION_FEATURES");
  ASSERT_FALSE(python.empty()) << "NAMED_POSITION_FEATURES not parsed from hub_validators.py";
  EXPECT_EQ(python.size(), std::size(NAMED_POSITIONS)) << "a named position is missing on one side";
  std::map<std::string, std::string> by_command(python.begin(), python.end());
  for (const NamedPosition &named : NAMED_POSITIONS) {
    ASSERT_TRUE(by_command.count(named.name)) << named.name << " is missing from NAMED_POSITION_FEATURES";
    const NameSet names = names_of(named.feature);
    ASSERT_EQ(names.size(), 1u) << named.name << " must need exactly one capability";
    EXPECT_EQ(by_command.at(named.name), *names.begin()) << named.name;
  }
}

TEST(DeviceFeaturesSync, EveryModelRowIsMirroredWithItsNameAndCapabilities) {
  const auto type_values = test::parse_python_uint8_dict(VALIDATORS, "DEVICE_TYPE_OPTIONS");
  const auto table = test::parse_python_name_set_dict(VALIDATORS, "DEVICE_FEATURES");
  const auto models = test::parse_python_uint8_dict(VALIDATORS, "DEVICE_MODEL_OPTIONS");
  ASSERT_FALSE(models.empty()) << "DEVICE_MODEL_OPTIONS not parsed from hub_validators.py";
  std::set<std::string> cpp_model_names;
  test::for_each_model_row([&](const DeviceTypeInfo &row) {
    cpp_model_names.insert(row.name);
    ASSERT_TRUE(models.count(row.name)) << row.name << " is missing from DEVICE_MODEL_OPTIONS";
    EXPECT_EQ(models.at(row.name), static_cast<uint8_t>(row.model)) << row.name;
    const std::string key = model_key(type_values, row);
    ASSERT_FALSE(key.empty()) << row.name << ": a model row needs a YAML-named type";
    ASSERT_TRUE(table.count(key)) << key << " is missing from DEVICE_FEATURES";
    EXPECT_EQ(table.at(key), cpp_names(row.type, row.subtype, row.model)) << key;
  });
  for (const auto &[name, value] : models)
    EXPECT_TRUE(cpp_model_names.count(name)) << "DEVICE_MODEL_OPTIONS names " << name << ", which no C++ model row has";
}

TEST(DeviceFeaturesSync, EveryOptInRegisterNeedsTheSameCapabilityInBothLanguages) {
  // climate.py accepts an opt-in key by this capability; the hub reads the register only with the
  // capability of HEATING_OPT_IN_READS. Were they to differ, a key would be accepted whose read never runs.
  const auto python = test::parse_python_ordered_dict(CLIMATE, "OPT_IN_READ_FEATURES");
  ASSERT_FALSE(python.empty()) << "OPT_IN_READ_FEATURES not parsed from climate.py";
  const std::map<std::string, std::string> by_function(python.begin(), python.end());
  size_t cpp_rows = 0;
  for (unsigned i = 0; i < static_cast<unsigned>(HeatingFunction::COUNT); i++) {
    const auto fn = static_cast<HeatingFunction>(i);
    const HeatingOptInRead *row = find_heating_opt_in_read(fn);
    if (row == nullptr)
      continue;
    cpp_rows++;
    // The Python key is the enumerator's name, which is the function's name in capitals.
    std::string name = heating_function_name(fn);
    std::ranges::transform(name, name.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    ASSERT_TRUE(by_function.count(name)) << name << " is missing from OPT_IN_READ_FEATURES";
    const NameSet names = names_of(row->feature);
    ASSERT_EQ(names.size(), 1u) << name << " must need exactly one capability";
    EXPECT_EQ(by_function.at(name), *names.begin()) << name;
  }
  EXPECT_EQ(python.size(), cpp_rows) << "OPT_IN_READ_FEATURES names a register HEATING_OPT_IN_READS does not read";
}

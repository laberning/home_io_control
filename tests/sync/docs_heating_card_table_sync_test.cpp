/// @file docs_heating_card_table_sync_test.cpp
/// @brief Verifies that the card table in docs/heating.md ("What each heater kind offers") agrees with
///        the heating profiles (proto_heating_profiles.cpp): one row per profile, keyed by the YAML's
///        `io_device_type`, `io_subtype` and `io_model`, with the profile's modes, presets, and whether
///        the card has a target, a room temperature and heating / idle.
///
/// The profiles are authoritative; the docs follow them. A new profile fails this test until its row
/// exists, and a row for a profile that is gone fails it too. The "Heater kind" column is prose and is
/// not compared.

#include "proto_device_types.h"
#include "proto_heating_profiles.h"
#include "support/markdown_table.h"

#include <gtest/gtest.h>

#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

using namespace esphome::home_io_control;

namespace {

constexpr const char *DOCS_PAGE = "docs/heating.md";
constexpr const char *TABLE_HEADER = "| Heater kind | `io_device_type` | `io_subtype` | `io_model` | Modes | Presets | "
                                     "Target | Room temperature | Heating / idle |";
constexpr size_t COLUMN_COUNT = 9;
constexpr const char *NO_MODEL = "";

/// What one row of the table, or one profile, says a card offers.
struct Card {
  std::set<std::string> modes;
  std::set<std::string> presets;
  bool target = false;
  bool room_temperature = false;
  bool action = false;

  bool operator==(const Card &) const = default;
};

/// A card is keyed the way a YAML entry names its heater: type name, subtype, model name ("" for none).
using CardKey = std::tuple<std::string, unsigned, std::string>;

/// The words a cell sets in backticks, in order: "`off`, `heat`" -> {"off", "heat"}.
std::vector<std::string> code_words(const std::string &cell) {
  std::vector<std::string> words;
  size_t open = cell.find('`');
  while (open != std::string::npos) {
    const size_t close = cell.find('`', open + 1);
    if (close == std::string::npos)
      break;
    words.push_back(cell.substr(open + 1, close - open - 1));
    open = cell.find('`', close + 1);
  }
  return words;
}

/// Read a "yes" / "no" cell; anything else is a failure naming the row.
bool yes_or_no(const std::string &cell, const std::string &row, const char *column) {
  if (cell != "yes" && cell != "no")
    ADD_FAILURE() << DOCS_PAGE << ", row '" << row << "': column " << column << " must say yes or no, not '" << cell
                  << "'";
  return cell == "yes";
}

/// Read the card table of the docs page.
std::map<CardKey, Card> parse_docs_table() {
  std::map<CardKey, Card> cards;
  for (const std::vector<std::string> &cells : test::read_markdown_table(DOCS_PAGE, TABLE_HEADER)) {
    if (cells.size() != COLUMN_COUNT) {
      ADD_FAILURE() << DOCS_PAGE << ": a row of the card table has " << cells.size() << " cells, not " << COLUMN_COUNT;
      continue;
    }
    const std::vector<std::string> type = code_words(cells[1]);
    const std::vector<std::string> subtype = code_words(cells[2]);
    const std::vector<std::string> model = code_words(cells[3]);
    if (type.size() != 1 || subtype.size() != 1 || model.size() > 1) {
      ADD_FAILURE() << DOCS_PAGE << ", row '" << cells[0] << "': the type and subtype cells hold one word in"
                    << " backticks each, the model cell one or none";
      continue;
    }
    Card card;
    for (const std::string &mode : code_words(cells[4]))
      card.modes.insert(mode);
    for (const std::string &preset : code_words(cells[5]))
      card.presets.insert(preset);
    card.target = yes_or_no(cells[6], cells[0], "Target");
    card.room_temperature = yes_or_no(cells[7], cells[0], "Room temperature");
    card.action = yes_or_no(cells[8], cells[0], "Heating / idle");
    const CardKey key{type[0], static_cast<unsigned>(std::stoul(subtype[0])), model.empty() ? NO_MODEL : model[0]};
    EXPECT_TRUE(cards.emplace(key, card).second) << DOCS_PAGE << " lists '" << cells[0] << "' twice";
  }
  return cards;
}

/// The name Home Assistant shows for a mode.
const char *mode_name(HeatingHvacMode mode) {
  switch (mode) {
    case HeatingHvacMode::OFF:
      return "off";
    case HeatingHvacMode::HEAT:
      return "heat";
    case HeatingHvacMode::AUTO:
      return "auto";
  }
  return "?";
}

/// The name Home Assistant shows for a preset: the standard preset's own, or the custom name.
std::string preset_name(const HeatingPresetEntry &preset) {
  switch (preset.id) {
    case HeatingPresetId::NONE:
      return "none";
    case HeatingPresetId::AWAY:
      return "away";
    case HeatingPresetId::COMFORT:
      return "comfort";
    case HeatingPresetId::ECO:
      return "eco";
    case HeatingPresetId::BOOST:
      return "boost";
    case HeatingPresetId::CUSTOM:
      return preset.custom_name;
  }
  return "?";
}

/// The card table as the profiles define it.
std::map<CardKey, Card> cards_of_the_profiles() {
  std::map<CardKey, Card> cards;
  for (const HeatingProfile &profile : heating_profiles()) {
    const DeviceTypeInfo *type = find_exact_device_type_info(profile.type, 0);
    if (type == nullptr) {
      ADD_FAILURE() << "a heating profile names node type " << static_cast<unsigned>(profile.type)
                    << ", which the node-type table does not have";
      continue;
    }
    std::string model = NO_MODEL;
    if (profile.model != DeviceModel::NONE) {
      const DeviceTypeInfo *model_row = find_device_model_info(profile.type, profile.subtype, profile.model);
      if (model_row == nullptr) {
        ADD_FAILURE() << "a heating profile of " << type->name << " names a model the node-type table does not have";
        continue;
      }
      model = model_row->name;
    }
    Card card;
    for (const HeatingModeEntry &mode : profile.modes)
      card.modes.insert(mode_name(mode.hvac));
    for (const HeatingPresetEntry &preset : profile.presets)
      card.presets.insert(preset_name(preset));
    card.target = profile.features.target_temperature;
    card.room_temperature = profile.features.current_temperature;
    card.action = profile.features.action;
    cards[CardKey{type->name, profile.subtype, model}] = card;
  }
  return cards;
}

/// A key as a reader would write it, for failure messages.
std::string describe(const CardKey &key) {
  std::string text = std::get<0>(key) + ", subtype " + std::to_string(std::get<1>(key));
  if (!std::get<2>(key).empty())
    text += ", io_model " + std::get<2>(key);
  return text;
}

/// A set as a comma-separated list, for failure messages.
std::string join(const std::set<std::string> &words) {
  std::string text;
  for (const std::string &word : words)
    text += (text.empty() ? "" : ", ") + word;
  return text;
}

}  // namespace

TEST(DocsHeatingCardTableSync, EveryHeatingProfileHasARowAndEveryRowAProfile) {
  const std::map<CardKey, Card> docs = parse_docs_table();
  const std::map<CardKey, Card> profiles = cards_of_the_profiles();
  ASSERT_FALSE(profiles.empty());
  for (const auto &[key, card] : profiles)
    EXPECT_TRUE(docs.count(key)) << DOCS_PAGE << " has no card-table row for " << describe(key);
  for (const auto &[key, card] : docs)
    EXPECT_TRUE(profiles.count(key)) << DOCS_PAGE << " has a card-table row for " << describe(key)
                                     << ", which has no heating profile";
}

TEST(DocsHeatingCardTableSync, EachRowOffersWhatItsProfileOffers) {
  const std::map<CardKey, Card> docs = parse_docs_table();
  ASSERT_FALSE(docs.empty());
  for (const auto &[key, profile] : cards_of_the_profiles()) {
    const auto row = docs.find(key);
    if (row == docs.end())
      continue;  // reported by the test above
    const Card &listed = row->second;
    EXPECT_EQ(join(listed.modes), join(profile.modes)) << "modes of " << describe(key);
    EXPECT_EQ(join(listed.presets), join(profile.presets)) << "presets of " << describe(key);
    EXPECT_EQ(listed.target, profile.target) << "target of " << describe(key);
    EXPECT_EQ(listed.room_temperature, profile.room_temperature) << "room temperature of " << describe(key);
    EXPECT_EQ(listed.action, profile.action) << "heating / idle of " << describe(key);
  }
}

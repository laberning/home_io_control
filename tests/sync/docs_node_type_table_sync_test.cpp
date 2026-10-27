/// @file docs_node_type_table_sync_test.cpp
/// @brief Verifies that the node-type table in docs/supported-devices.md ("Node types and
///        subtypes") agrees with the C++ table (proto_device_types.cpp): every type value, its
///        name, its named subtypes, and "(experimental)" on exactly the subtypes whose row carries
///        device_type_flags::EXPERIMENTAL.
///
/// The C++ table is authoritative; the docs follow it. The test parses the Markdown table at
/// runtime, like the other sync tests parse hub_validators.py (see device_type_sync_test.cpp and
/// python_dict_parser.h). The "Support" column is prose and is not compared.

#include "proto_device_types.h"
#include "support/python_dict_parser.h"

#include "device_table_walk.h"
#include "test_helpers.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace esphome::home_io_control;

namespace {

constexpr const char *DOCS_PAGE = "docs/supported-devices.md";
constexpr const char *TABLE_HEADER = "| Type | Name | Named subtypes | Support |";
constexpr const char *EXPERIMENTAL_SUFFIX = " (experimental)";

/// A named subtype as the docs list it.
struct DocsSubtype {
  std::string name;
  bool experimental = false;
};

/// One row of the docs table.
struct DocsRow {
  std::string name;
  std::map<unsigned, DocsSubtype> subtypes;
};

/// Split a Markdown table line into its trimmed cells.
std::vector<std::string> split_cells(const std::string &line) {
  std::vector<std::string> cells;
  size_t start = line.find('|');
  while (start != std::string::npos) {
    const size_t end = line.find('|', start + 1);
    if (end == std::string::npos)
      break;
    cells.push_back(test::detail::trim_ws(line.substr(start + 1, end - start - 1)));
    start = end;
  }
  return cells;
}

/// Parse the "Named subtypes" cell: `N name[ (experimental)]` entries separated by ", ".
std::map<unsigned, DocsSubtype> parse_subtypes(const std::string &cell) {
  std::map<unsigned, DocsSubtype> subtypes;
  size_t start = 0;
  while (start < cell.size()) {
    size_t end = cell.find(", ", start);
    if (end == std::string::npos)
      end = cell.size();
    const std::string entry = cell.substr(start, end - start);
    start = end + 2;
    const size_t space = entry.find(' ');
    if (space == std::string::npos) {
      ADD_FAILURE() << "subtype entry '" << entry << "' has no name";
      continue;
    }
    DocsSubtype subtype;
    subtype.name = entry.substr(space + 1);
    const std::string suffix = EXPERIMENTAL_SUFFIX;
    if (subtype.name.size() > suffix.size() &&
        subtype.name.compare(subtype.name.size() - suffix.size(), suffix.size(), suffix) == 0) {
      subtype.experimental = true;
      subtype.name.erase(subtype.name.size() - suffix.size());
    }
    subtypes[static_cast<unsigned>(std::stoul(entry.substr(0, space)))] = subtype;
  }
  return subtypes;
}

/// Read the node-type table of the docs page, keyed by the type value.
std::map<unsigned, DocsRow> parse_docs_table() {
  std::map<unsigned, DocsRow> rows;
  std::ifstream file = test::detail::open_source_or_fail(DOCS_PAGE);
  if (!file.is_open())
    return rows;
  std::string line;
  bool in_table = false;
  while (std::getline(file, line)) {
    if (!in_table) {
      in_table = line == TABLE_HEADER;
      continue;
    }
    if (line.rfind('|', 0) != 0)
      break;  // the first line that is not a table row ends the table
    const std::vector<std::string> cells = split_cells(line);
    if (cells.size() < 4 || cells[0].rfind("---", 0) == 0)
      continue;
    std::string value = cells[0];
    value.erase(std::remove(value.begin(), value.end(), '`'), value.end());
    DocsRow row;
    row.name = cells[1];
    row.subtypes = parse_subtypes(cells[2]);
    rows[static_cast<unsigned>(std::stoul(value, nullptr, 16))] = row;
  }
  EXPECT_TRUE(in_table) << "the node-type table header was not found in " << DOCS_PAGE;
  return rows;
}

/// The table's type name as the docs print it: the identifier with spaces for underscores.
std::string display_name(const char *identifier) {
  std::string name = identifier;
  for (char &c : name) {
    if (c == '_')
      c = ' ';
  }
  return name;
}

}  // namespace

TEST(DocsNodeTypeTableSync, EveryTypeOfTheCppTableIsListedWithItsName) {
  const std::map<unsigned, DocsRow> docs = parse_docs_table();
  ASSERT_FALSE(docs.empty());
  std::map<unsigned, std::string> expected;
  test::for_each_table_row([&expected](const DeviceTypeInfo &row) {
    // Type 0 is the "unknown" placeholder, not a node type a device reports.
    if (row.subtype == 0 && row.type != DeviceType::UNKNOWN)
      expected[static_cast<unsigned>(row.type)] = display_name(row.name);
  });
  for (const auto &[value, name] : expected) {
    const auto found = docs.find(value);
    ASSERT_NE(found, docs.end()) << "type 0x" << std::hex << value << " (" << name << ") is not in " << DOCS_PAGE;
    EXPECT_EQ(found->second.name, name) << "type 0x" << std::hex << value;
  }
  for (const auto &[value, row] : docs)
    EXPECT_TRUE(expected.count(value)) << DOCS_PAGE << " lists type 0x" << std::hex << value << " (" << row.name
                                       << "), which the C++ table does not have";
}

TEST(DocsNodeTypeTableSync, NamedSubtypesMatchAndAreMarkedExperimentalExactlyWhenTheRowIs) {
  const std::map<unsigned, DocsRow> docs = parse_docs_table();
  ASSERT_FALSE(docs.empty());
  std::map<unsigned, std::map<unsigned, DocsSubtype>> expected;
  test::for_each_table_row([&expected](const DeviceTypeInfo &row) {
    if (row.subtype == 0)
      return;
    DocsSubtype subtype;
    subtype.name = row.name;
    subtype.experimental = (row.flags & device_type_flags::EXPERIMENTAL) != 0;
    expected[static_cast<unsigned>(row.type)][row.subtype] = subtype;
  });
  for (const auto &[value, row] : docs) {
    const auto want = expected.find(value);
    const std::map<unsigned, DocsSubtype> none;
    const std::map<unsigned, DocsSubtype> &want_subtypes = want == expected.end() ? none : want->second;
    EXPECT_EQ(row.subtypes.size(), want_subtypes.size()) << "type 0x" << std::hex << value << " (" << row.name << ")";
    for (const auto &[subtype, named] : want_subtypes) {
      const auto listed = row.subtypes.find(subtype);
      ASSERT_NE(listed, row.subtypes.end()) << "type 0x" << std::hex << value << ": subtype " << std::dec << subtype
                                            << " (" << named.name << ") is not named in " << DOCS_PAGE;
      EXPECT_EQ(listed->second.name, named.name)
          << "type 0x" << std::hex << value << " subtype " << std::dec << subtype;
      EXPECT_EQ(listed->second.experimental, named.experimental)
          << "type 0x" << std::hex << value << " subtype " << std::dec << subtype << ": \"(experimental)\" must be on"
          << " exactly the rows flagged EXPERIMENTAL";
    }
  }
}

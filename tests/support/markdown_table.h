#pragma once

/// @file markdown_table.h
/// @brief Test helper: read one Markdown table of a docs page as rows of trimmed cells.
/// @details Shared by the sync tests that check a table in `docs/` against a C++ table, so the cell
///          splitting and the "where does the table end" rule exist once.
/// @ingroup hioc_protocol

#include "support/python_dict_parser.h"

#include <gtest/gtest.h>

#include <fstream>
#include <string>
#include <vector>

namespace test {

/// @brief Split a Markdown table line into its trimmed cells.
/// @param line One line of a table, starting and ending with `|`.
/// @return The text between each pair of bars, without surrounding spaces.
inline std::vector<std::string> split_markdown_cells(const std::string &line) {
  std::vector<std::string> cells;
  size_t start = line.find('|');
  while (start != std::string::npos) {
    const size_t end = line.find('|', start + 1);
    if (end == std::string::npos)
      break;
    cells.push_back(detail::trim_ws(line.substr(start + 1, end - start - 1)));
    start = end;
  }
  return cells;
}

/// @brief Read the body rows of the table that starts with `header` in a docs page.
/// @details The table ends at the first line that does not start with a bar. The `|---|` separator row is
///          left out. A missing page or header records a test failure and returns no rows.
/// @param page Path of the page, relative to the project root.
/// @param header The table's header line, exactly as the page writes it.
/// @return One vector of cells per body row, in page order.
inline std::vector<std::vector<std::string>> read_markdown_table(const char *page, const char *header) {
  std::vector<std::vector<std::string>> rows;
  std::ifstream file = detail::open_source_or_fail(page);
  if (!file.is_open())
    return rows;
  std::string line;
  bool in_table = false;
  while (std::getline(file, line)) {
    if (!in_table) {
      in_table = line == header;
      continue;
    }
    if (line.rfind('|', 0) != 0)
      break;
    std::vector<std::string> cells = split_markdown_cells(line);
    if (cells.empty() || cells[0].rfind("---", 0) == 0)
      continue;
    rows.push_back(std::move(cells));
  }
  EXPECT_TRUE(in_table) << "the table header '" << header << "' was not found in " << page;
  return rows;
}

}  // namespace test

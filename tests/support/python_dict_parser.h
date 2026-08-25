#pragma once

/// @file python_dict_parser.h
/// @brief Shared parsers for this project's C++-side Python/C++ cross-language sync tests
/// (device_type_sync_test.cpp, manufacturer_sync_test.cpp, opcode_name_sync_test.cpp).
///
/// Hand-rolled `std::string` scanning, not `ast` -- these tests run in the same binary as the C++
/// constants they check, which is why this style exists alongside the newer script-based sync
/// checks (scripts/check-tuning-sync.py, scripts/check-yaml-emitters.py, both `make lint`); see
/// any sync test's file header for the fuller note on why both forms are kept.
///
/// Four public functions:
///  - parse_python_uint8_dict(path, dict_name): a top-level `NAME = { "key": 0xNN, ... }` dict
///    (string-keyed, int-valued) -- used for MANUFACTURER_OPTIONS and DEVICE_TYPE_OPTIONS
///    (device_type_sync_test.cpp, manufacturer_sync_test.cpp).
///  - parse_python_uint8_keyed_string_dict(path, dict_name): the mirror shape,
///    `NAME = { 0xNN: "value", ... }` (int-keyed, string-valued) -- used for CMD_NAMES
///    (opcode_name_sync_test.cpp).
///  - parse_cpp_uint8_constants(path, prefix): `static constexpr uint8_t <prefix>NAME = value;`
///    C++ declarations -- used for MANUFACTURER_* and CMD_* in proto_constants.h
///    (manufacturer_sync_test.cpp, opcode_name_sync_test.cpp).
///  - parse_python_uint8_assignments(path, prefix): bare module-level `<prefix>NAME = 0xNN`
///    Python assignments -- used for protolib.py's rekey-pipeline CMD_* mirror
///    (opcode_name_sync_test.cpp).
///
/// The two dict-shaped parsers share one scan loop (parse_python_dict_entries(), below) rather
/// than being two near-identical copies -- that duplication bug pattern is exactly what this
/// header's history already records once (see parse_python_uint8_dict()'s note below).

#include <gtest/gtest.h>

#include <cctype>
#include <cstdint>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace test {
namespace detail {

/// Trim leading/trailing whitespace (spaces and tabs only) from `s`. Shared by every trimming
/// site in this file rather than being copy-pasted at each one.
inline std::string trim_ws(std::string s) {
  auto start = s.find_first_not_of(" \t");
  if (start == std::string::npos)
    return "";
  auto end = s.find_last_not_of(" \t");
  return s.substr(start, end - start + 1);
}

/// Open `path` for reading, recording a clear EXPECT_TRUE failure naming `path` if it could not
/// be opened. Shared by every "open this source file or bail" site in this file rather than being
/// copy-pasted at each one; callers must still check `.is_open()` themselves (an `std::ifstream`
/// can't signal failure through its return value alone) and return `{}` on failure.
inline std::ifstream open_source_or_fail(const char *path) {
  std::ifstream file(path);
  EXPECT_TRUE(file.is_open()) << "Cannot open " << path << " — run tests from the project root.";
  return file;
}

/// Parse `text` as a C/Python-style unsigned integer literal (decimal or `0x`-prefixed hex), the
/// same as `std::stoul(text, nullptr, 0)`. `stoul`'s prefix parsing only tolerates *trailing*
/// garbage after a valid leading numeral -- leading garbage before the numeral (e.g. a comment
/// buffered ahead of a value) makes it throw `std::invalid_argument`, which would otherwise
/// escape a sync test's body as an opaque, undiagnosable crash. This wraps that call so a
/// malformed value instead becomes a normal, actionable test failure naming the source `path` and
/// the constant/entry (`context`) being parsed.
/// @return The parsed value, or 0 (with an ADD_FAILURE already recorded) if `text` could not be
///         parsed as an integer literal.
inline uint8_t parse_uint8_or_fail(const std::string &text, const char *path, const std::string &context) {
  try {
    return static_cast<uint8_t>(std::stoul(text, nullptr, 0));
  } catch (const std::exception &e) {
    ADD_FAILURE() << "Cannot parse '" << text << "' as an integer for " << context << " in " << path << ": "
                  << e.what();
    return 0;
  }
}

/// Trim a raw dict value/key token: a trailing `#`-prefixed comment (consistent with how
/// parse_python_uint8_assignments() handles the same problem for its own value tokens), then
/// leading/trailing whitespace, then a single trailing comma (and any whitespace before it), then
/// a pair of surrounding double quotes. Order matters: the comment can trail *after* the comma
/// (`"PRIVATE2",  # stored-position readback`), and the comma sits outside any quotes in both dict
/// shapes this project parses (`"key": 0xNN,` and `0xNN: "value",`).
inline std::string trim_dict_token(std::string s) {
  auto hash = s.find('#');
  if (hash != std::string::npos)
    s = s.substr(0, hash);

  s = trim_ws(s);
  if (s.empty())
    return "";

  if (s.back() == ',')
    s = trim_ws(s.substr(0, s.size() - 1));

  if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
    s = s.substr(1, s.size() - 2);

  return s;
}

/// Shared core: parse every entry of a top-level `NAME = { <key>: <value>, ... }` dict out of a
/// Python source file, regardless of which side (key or value) is quoted.
///
/// Matches lines between the `NAME = {` line and the matching `}`. Skips blank lines and comment
/// lines (first non-whitespace character `#`) inside the dict body -- an ordinary explanatory
/// comment (`# "future_vendor": 0x0D,  # not yet assigned`) must never register as a phantom
/// entry, which is exactly the bug this parser had before the two near-identical copies of it
/// were merged into one (the reason this shared core exists at all). The terminator is *not*
/// brace-tracking despite what that history might suggest -- it breaks on the first line
/// containing `}`, which is correct for the flat, single-level dicts this project has (no nested
/// braces inside a `CMD_NAMES`/`MANUFACTURER_OPTIONS`/`DEVICE_TYPE_OPTIONS` entry).
/// @param path Path to the Python source file, relative to the project root.
/// @param dict_name The dict's variable name, e.g. "MANUFACTURER_OPTIONS".
/// @return Raw (key text, value text) pairs, split on the key's terminating `:` (found *after*
///         the closing quote when the key is a quoted string, so a colon inside the key text
///         itself -- e.g. a hypothetical `"ren:son": 0x08,` -- doesn't split in the wrong place)
///         and trimmed of whitespace, a trailing comma and surrounding double quotes; empty (with
///         a test failure already recorded via EXPECT_TRUE) if `path` could not be opened.
inline std::vector<std::pair<std::string, std::string>> parse_python_dict_entries(const char *path,
                                                                                  const char *dict_name) {
  std::ifstream file = open_source_or_fail(path);
  if (!file.is_open())
    return {};

  std::vector<std::pair<std::string, std::string>> entries;
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

    // If the key is a quoted string, its terminating ':' must be found after the closing quote
    // -- a colon inside the key text itself must not be mistaken for the key/value separator.
    // A bare int-literal key (the `{0xNN: "value"}` shape) has no quotes to skip.
    std::string::size_type colon;
    if (line[first] == '"') {
      auto q2 = line.find('"', first + 1);
      colon = (q2 == std::string::npos) ? std::string::npos : line.find(':', q2);
    } else {
      colon = line.find(':', first);
    }
    if (colon == std::string::npos)
      continue;

    std::string key = trim_dict_token(line.substr(0, colon));
    std::string value = trim_dict_token(line.substr(colon + 1));
    if (key.empty() || value.empty())
      continue;

    entries.emplace_back(std::move(key), std::move(value));
  }
  return entries;
}

}  // namespace detail

/// Parse a top-level `NAME = { "key": 0xNN, ... }` dict out of a Python source file
/// (string-keyed, int-valued) -- e.g. MANUFACTURER_OPTIONS, DEVICE_TYPE_OPTIONS.
/// @param path Path to the Python source file, relative to the project root.
/// @param dict_name The dict's variable name, e.g. "MANUFACTURER_OPTIONS".
/// @return name -> value map; empty (with a test failure already recorded via EXPECT_TRUE) if
///         `path` could not be opened.
inline std::map<std::string, uint8_t> parse_python_uint8_dict(const char *path, const char *dict_name) {
  std::map<std::string, uint8_t> result;
  for (const auto &[key, value] : detail::parse_python_dict_entries(path, dict_name))
    result[key] = detail::parse_uint8_or_fail(value, path, std::string(dict_name) + "[\"" + key + "\"]");
  return result;
}

/// Parse a top-level `NAME = { 0xNN: "value", ... }` dict out of a Python source file
/// (int-keyed, string-valued) -- e.g. CMD_NAMES.
/// @param path Path to the Python source file, relative to the project root.
/// @param dict_name The dict's variable name, e.g. "CMD_NAMES".
/// @return value -> name map; empty (with a test failure already recorded via EXPECT_TRUE) if
///         `path` could not be opened.
inline std::map<uint8_t, std::string> parse_python_uint8_keyed_string_dict(const char *path, const char *dict_name) {
  std::map<uint8_t, std::string> result;
  for (const auto &[key, value] : detail::parse_python_dict_entries(path, dict_name))
    result[detail::parse_uint8_or_fail(key, path, std::string(dict_name) + " key '" + key + "'")] = value;
  return result;
}

/// Parse every `static constexpr uint8_t <prefix>NAME = value;` declaration out of a C++ header.
///
/// Handles the two layouts this project's `uint8_t` constant blocks actually use: the value on
/// the declaration line (`= 0xNN;` or `= N;`), and the value wrapped onto the immediately
/// following line (clang-format's habit once the doc comment pushes the line past its width
/// limit). A declaration is buffered from its `<prefix>NAME` line forward until a `;` is seen,
/// then the value is taken verbatim as the buffered text between `=` and `;`, trimmed of
/// whitespace -- correct for both decimal (`MANUFACTURER_*`) and hex (`CMD_*`) literals, since no
/// comment text ever appears between `=` and `;` in this project's headers (verified for both
/// prefixes). `std::stoul(..., 0)` autodetects the base. `stoul`'s prefix parsing only tolerates
/// *trailing* garbage after a valid leading numeral -- if a future declaration ever breaks the
/// no-comment invariant by putting comment text *before* the value, `stoul` throws
/// `std::invalid_argument` rather than skipping past it. `parse_uint8_or_fail()` (below) catches
/// that and turns it into a clear, actionable test failure naming this constant, instead of an
/// unhandled exception escaping the test body.
/// @param path Path to the C++ header, relative to the project root.
/// @param prefix The constant name prefix, e.g. "MANUFACTURER_" or "CMD_".
/// @return name (prefix stripped) -> value map; empty (with a test failure already recorded via
///         EXPECT_TRUE) if `path` could not be opened.
inline std::map<std::string, uint8_t> parse_cpp_uint8_constants(const char *path, const char *prefix) {
  std::ifstream file = detail::open_source_or_fail(path);
  if (!file.is_open())
    return {};

  const std::string decl_prefix = std::string("static constexpr uint8_t ") + prefix;
  std::map<std::string, uint8_t> entries;
  std::string line;

  while (std::getline(file, line)) {
    auto start = line.find(decl_prefix);
    if (start == std::string::npos)
      continue;
    start += decl_prefix.size();
    auto name_end = line.find_first_of(" =", start);
    if (name_end == std::string::npos)
      continue;
    std::string name = line.substr(start, name_end - start);

    // Buffer forward until ';' is seen -- the value may be wrapped onto the next line.
    std::string buffer = line;
    while (buffer.find(';') == std::string::npos && std::getline(file, line))
      buffer += " " + line;

    auto eq = buffer.find('=', start);
    if (eq == std::string::npos)
      continue;
    auto semi = buffer.find(';', eq);
    if (semi == std::string::npos)
      continue;

    // The text between '=' and ';' is the value, verbatim -- no comment text ever appears there
    // in this project's headers (verified for both MANUFACTURER_* and CMD_*), so this handles
    // plain decimal (MANUFACTURER_*) and hex (CMD_*) literals alike without needing to know which
    // shape a given prefix uses. std::stoul(..., 0) autodetects the base. If a future declaration
    // ever does put a comment *before* the value here, parse_uint8_or_fail() reports it as a clear
    // failure naming this constant rather than throwing an unhandled exception (stoul's prefix
    // parsing only tolerates trailing garbage *after* a valid leading numeral, not leading
    // garbage before one).
    std::string val_str = detail::trim_ws(buffer.substr(eq + 1, semi - eq - 1));
    if (val_str.empty())
      continue;

    entries[name] = detail::parse_uint8_or_fail(val_str, path, std::string(prefix) + name);
  }
  return entries;
}

/// Parse every bare module-level `<prefix>NAME = 0xNN` assignment out of a Python source file.
///
/// Deliberately strict: the assignment must start at column 0 (so a same-named local inside a
/// function body cannot match) and the value must be a bare hex/int literal (no expressions, no
/// references to other names) -- this is meant for small, hand-curated constant mirrors such as
/// protolib.py's `--rekey` pipeline constants, not general Python assignment parsing.
/// @param path Path to the Python source file, relative to the project root.
/// @param prefix The constant name prefix, e.g. "CMD_".
/// @return name (prefix stripped) -> value map; empty (with a test failure already recorded via
///         EXPECT_TRUE) if `path` could not be opened.
inline std::map<std::string, uint8_t> parse_python_uint8_assignments(const char *path, const char *prefix) {
  std::ifstream file = detail::open_source_or_fail(path);
  if (!file.is_open())
    return {};

  const std::string decl_prefix(prefix);
  std::map<std::string, uint8_t> entries;
  std::string line;

  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#')
      continue;
    if (line.compare(0, decl_prefix.size(), decl_prefix) != 0)
      continue;  // not a module-level assignment of this prefix (must start at column 0)

    auto eq = line.find('=');
    if (eq == std::string::npos)
      continue;
    std::string name = line.substr(decl_prefix.size(), eq - decl_prefix.size());
    auto name_end = name.find_first_of(" \t");
    if (name_end != std::string::npos)
      name = name.substr(0, name_end);
    if (name.empty())
      continue;

    std::string val_str = line.substr(eq + 1);
    auto hash = val_str.find('#');
    if (hash != std::string::npos)
      val_str = val_str.substr(0, hash);
    val_str = detail::trim_ws(val_str);
    if (val_str.empty())
      continue;

    // Must be a bare int/hex literal, not an expression or a reference to another constant.
    bool is_literal = true;
    for (char c : val_str) {
      if (!std::isxdigit(static_cast<unsigned char>(c)) && c != 'x' && c != 'X') {
        is_literal = false;
        break;
      }
    }
    if (!is_literal)
      continue;

    entries[name] = detail::parse_uint8_or_fail(val_str, path, decl_prefix + name);
  }
  return entries;
}

}  // namespace test

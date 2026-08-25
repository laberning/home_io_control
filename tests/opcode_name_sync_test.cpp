/// @file opcode_name_sync_test.cpp
/// @brief Verifies that the hand-maintained opcode-name tables in proto_constants.{h,cpp} and
///        scripts/corpus/protolib.py agree on every command byte, preventing drift between them.
///
/// This is the third instance of the older/C++-test cross-language sync mechanism this project
/// uses -- see manufacturer_sync_test.cpp and device_type_sync_test.cpp for the other two, and
/// either file's header for the fuller note on the newer script-based form
/// (`scripts/check-yaml-emitters.py` etc., `make lint`). All three share the parsers in
/// python_dict_parser.h.
///
/// These tables *name* opcodes for decoding/logging only. Naming an opcode here never implies it
/// may be transmitted -- most concretely for `CMD_UNKNOWN4A_REQ` (0x4A), documented in
/// proto_constants.h as "Delete File", which must never be sent and has no builder anywhere in
/// this codebase. Its presence (and that of the other never-sent opcodes this file checks, e.g.
/// 0xF0-0xF3, 0x54/0x55) in both the C++ switch and Python's `CMD_NAMES` is correct and required:
/// both tables are decode-side "render this byte by name instead of UNKNOWN_CMD" lookups, not
/// evidence of -- or license for -- a transmit path.

#include "proto_constants.h"
#include "support/python_dict_parser.h"
#include "test_helpers.h"

#include <cstring>
#include <map>
#include <string>

using namespace esphome::home_io_control;

namespace {
constexpr const char *kProtoConstantsPath = "components/home_io_control/proto_constants.h";
constexpr const char *kProtolibPath = "scripts/corpus/protolib.py";
constexpr const char *kCppFloorMsg = "Parsed too few CMD_* constants from proto_constants.h — parser may be broken.";
constexpr const char *kPyFloorMsg = "Parsed too few entries from protolib.py's CMD_NAMES — parser may be broken.";
}  // namespace

TEST(OpcodeNameSync, CppConstantsAreNamedByCommandName) {
  // Closes the hole noted in the plan: radio_sx1262_rx_test.cpp's NamedCommandsAreEitherKnown...
  // iterates command_name()'s output, so it can never catch a CMD_* constant that was added to
  // proto_constants.h but never given a case in command_name()'s switch -- that opcode is simply
  // absent from its loop's working set. This test starts from the constants instead.
  auto cpp_entries = test::parse_cpp_uint8_constants(kProtoConstantsPath, "CMD_");
  ASSERT_GT(cpp_entries.size(), 40u) << kCppFloorMsg;

  for (const auto &[name, value] : cpp_entries) {
    EXPECT_STREQ(command_name(value), name.c_str())
        << "command_name(0x" << std::hex << static_cast<int>(value) << ") does not match CMD_" << name
        << " — add or fix its case in proto_constants.cpp's command_name() switch.";
  }
}

TEST(OpcodeNameSync, CppConstantsAreExposedInPython) {
  // Catches *silence*: cmd_name() degrades to UNKNOWN_0x{:02X} for a value CMD_NAMES has no entry
  // for, so a missing name here is lossy but never wrong. This is the direction that closes
  // today's real gap (7 CMD_* constants missing from CMD_NAMES).
  auto cpp_entries = test::parse_cpp_uint8_constants(kProtoConstantsPath, "CMD_");
  auto py_entries = test::parse_python_uint8_keyed_string_dict(kProtolibPath, "CMD_NAMES");
  ASSERT_GT(cpp_entries.size(), 40u) << kCppFloorMsg;
  ASSERT_GT(py_entries.size(), 40u) << kPyFloorMsg;

  for (const auto &[name, value] : cpp_entries) {
    auto it = py_entries.find(value);
    ASSERT_NE(it, py_entries.end()) << "proto_constants.h's CMD_" << name << " (0x" << std::hex
                                    << static_cast<int>(value) << ") has no matching entry in protolib.py's "
                                    << "CMD_NAMES.";
    EXPECT_EQ(it->second, name) << "protolib.py's CMD_NAMES[0x" << std::hex << static_cast<int>(value) << "] = \""
                                << it->second << "\" but proto_constants.h's CMD_" << name << " expects \"" << name
                                << "\". Names must match exactly.";
  }
}

TEST(OpcodeNameSync, PythonCmdNamesMatchCppConstants) {
  // Catches *misinformation*: a rename or renumber in proto_constants.h leaving a stale CMD_NAMES
  // entry would make cmd_name() label a frame with a name that is confidently incorrect, in a
  // scaffold a human then reads while hand-writing an `expect:` value. Unlike the manufacturer
  // sync test's analogous reverse-direction check, there is no escape hatch here that makes this
  // soft -- both directions are hard failures for CMD_NAMES.
  auto cpp_entries = test::parse_cpp_uint8_constants(kProtoConstantsPath, "CMD_");
  auto py_entries = test::parse_python_uint8_keyed_string_dict(kProtolibPath, "CMD_NAMES");
  ASSERT_GT(cpp_entries.size(), 40u) << kCppFloorMsg;
  ASSERT_GT(py_entries.size(), 40u) << kPyFloorMsg;

  for (const auto &[value, name] : py_entries) {
    auto it = cpp_entries.find(name);
    ASSERT_NE(it, cpp_entries.end()) << "protolib.py's CMD_NAMES[0x" << std::hex << static_cast<int>(value) << "] = \""
                                     << name << "\" has no matching CMD_" << name << " constant in proto_constants.h.";
    EXPECT_EQ(it->second, value) << "protolib.py's CMD_NAMES[0x" << std::hex << static_cast<int>(value) << "] = \""
                                 << name << "\" but proto_constants.h's CMD_" << name << " = 0x" << std::hex
                                 << static_cast<int>(it->second) << ". Values must match.";
  }
}

TEST(OpcodeNameSync, PythonRekeyConstantsMatchCpp) {
  // protolib.py's bare module-level CMD_* constants (§1.3 of the plan this test implements) drive
  // which frames `ingest.py --rekey` rewrites — more load-bearing than CMD_NAMES, which only
  // decorates scaffold notes. Deliberately one-directional: the reverse (every C++ CMD_* must have
  // a bare Python constant) must NOT be asserted — that set is an intentional 7-of-50 subset
  // scoped to --rekey's needs, and asserting the reverse would force 43 unused module-level
  // constants into protolib.py.
  auto py_entries = test::parse_python_uint8_assignments(kProtolibPath, "CMD_");
  auto cpp_entries = test::parse_cpp_uint8_constants(kProtoConstantsPath, "CMD_");
  // Exact-count guard rather than a floor: this set is small, fixed and hand-curated, so a bare
  // ASSERT_GT would be meaninglessly loose here.
  ASSERT_EQ(py_entries.size(), 7u) << "protolib.py's rekey-pipeline CMD_* constants changed count — "
                                   << "update this test's expectations (and check the parser) deliberately.";

  for (const auto &[name, value] : py_entries) {
    auto it = cpp_entries.find(name);
    ASSERT_NE(it, cpp_entries.end()) << "protolib.py's rekey-pipeline constant CMD_" << name
                                     << " has no matching constant in proto_constants.h.";
    EXPECT_EQ(it->second, value) << "protolib.py's CMD_" << name << " = 0x" << std::hex << static_cast<int>(value)
                                 << " but proto_constants.h's CMD_" << name << " = 0x" << std::hex
                                 << static_cast<int>(it->second) << ". Values must match.";
  }
}

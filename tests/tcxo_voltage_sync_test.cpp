/// @file tcxo_voltage_sync_test.cpp
/// @brief Guards the SX1262/LR1121 TCXO voltage enum against Python/C++ drift.
///
/// The SX1262 `SetDIO3AsTCXOCtrl` and LR1121 `SetTcxoMode` commands take the identical 0-based
/// voltage code (0x00 = 1.6 V .. 0x07 = 3.3 V, Semtech SX1261/2 datasheet Table 13-35). Both
/// drivers pass the generated `TCXO_VOLTAGE_OPTIONS` integer straight through with no mapping,
/// so the Python table has to *be* the chip encoding. This test parses `__init__.py` at runtime
/// (Python stays the single source of truth, same mechanism as device_type_sync_test.cpp) and
/// pins:
///   - the voltage rungs are 0-based and contiguous, 1_6V..3_3V -> 0x00..0x07 in order;
///   - 1_8V == DEFAULT_TCXO_VOLTAGE_SETTING_1P8V == 0x02 (the schema default);
///   - NONE == TCXO_VOLTAGE_NONE (0xFF), the bare-crystal sentinel;
///   - no non-sentinel code exceeds the chips' 3-bit field.

#include "hub_core.h"
#include "radio_interface.h"
#include "support/python_dict_parser.h"
#include "test_helpers.h"

#include <map>
#include <string>

using namespace esphome::home_io_control;

namespace {

std::map<std::string, uint8_t> parse_options() {
  auto entries = test::parse_python_uint8_dict("components/home_io_control/__init__.py", "TCXO_VOLTAGE_OPTIONS");
  std::map<std::string, uint8_t> by_name;
  for (const auto &[name, value] : entries)
    by_name[name] = value;
  return by_name;
}

// Voltage rungs in ascending order; the parsed table must map these to 0x00..0x07.
constexpr const char *kVoltageRungs[] = {"1_6V", "1_7V", "1_8V", "2_2V", "2_4V", "2_7V", "3_0V", "3_3V"};

}  // namespace

TEST(TcxoVoltageSync, RungsAreZeroBasedAndContiguous) {
  auto opts = parse_options();
  ASSERT_EQ(opts.size(), 9u) << "expected 8 voltage rungs + NONE — parser broken or table changed shape";

  for (uint8_t expected = 0; expected < 8; expected++) {
    const std::string name = kVoltageRungs[expected];
    ASSERT_TRUE(opts.count(name)) << "TCXO_VOLTAGE_OPTIONS is missing \"" << name << "\"";
    EXPECT_EQ(opts[name], expected) << "\"" << name << "\" must be 0x" << std::hex << static_cast<int>(expected)
                                    << " — the chips' own SetDIO3AsTCXOCtrl / SetTcxoMode code, 0-based. A 1-based "
                                       "table re-introduces the SX1262 off-by-one.";
  }
}

TEST(TcxoVoltageSync, DefaultConstantMatchesTable) {
  auto opts = parse_options();
  ASSERT_TRUE(opts.count("1_8V"));
  EXPECT_EQ(opts["1_8V"], 0x02) << "1.8 V is chip code 0x02 (datasheet Table 13-35)";
  EXPECT_EQ(DEFAULT_TCXO_VOLTAGE_SETTING_1P8V, opts["1_8V"])
      << "hub_core.h DEFAULT_TCXO_VOLTAGE_SETTING_1P8V drifted from TCXO_VOLTAGE_OPTIONS[\"1_8V\"]";
}

TEST(TcxoVoltageSync, NoneSentinelMatchesTable) {
  auto opts = parse_options();
  ASSERT_TRUE(opts.count("NONE")) << "the bare-crystal sentinel is missing from TCXO_VOLTAGE_OPTIONS";
  EXPECT_EQ(opts["NONE"], TCXO_VOLTAGE_NONE)
      << "radio_interface.h TCXO_VOLTAGE_NONE drifted from TCXO_VOLTAGE_OPTIONS[\"NONE\"]";
}

TEST(TcxoVoltageSync, EveryVoltageCodeFitsThe3BitChipField) {
  for (const auto &[name, value] : parse_options()) {
    if (name == "NONE")
      continue;  // sentinel — never sent to the chip
    EXPECT_LE(value, 0x07u) << "\"" << name << "\" = 0x" << std::hex << static_cast<int>(value)
                            << " is outside the SX1262/LR1121 tcxoVoltage field (0x00..0x07)";
  }
}

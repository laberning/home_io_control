/// @file fem_tx_power_sync_test.cpp
/// @brief Guards the FEM `tx_power` ceilings in `__init__.py` against the driver's own estimate.
///
/// `FEM_TX_POWER_MAX_QUIET` (Python) is the highest `tx_power` per FEM part whose estimated
/// antenna-port power stays at or under the 868 MHz SRD limit of +14 dBm; the schema warns above
/// it, and docs/hardware.md quotes the same numbers. The estimate itself lives in C++
/// (`sx1262_fem_tx_estimate()`), with no shared header. This test parses `__init__.py` at runtime
/// (Python stays the source of truth, same mechanism as tcxo_voltage_sync_test.cpp) and pins each
/// ceiling to the estimate: the ceiling itself stays within the limit, the next setting exceeds it.

#include "radio_interface.h"
#include "radio_sx1262.h"
#include "support/python_dict_parser.h"
#include "test_helpers.h"

#include <iterator>
#include <string>

using namespace esphome::home_io_control;

namespace {

/// SRD ERP limit the ceilings are defined against, in dBm.
constexpr int SRD_LIMIT_DBM = 14;

struct FemUnderTest {
  const char *yaml_name;
  FemProfile profile;
};
constexpr FemUnderTest kFemProfiles[] = {
    {"gc1109", FemProfile::GC1109},
    {"kct8103l", FemProfile::KCT8103L},
    {"xy16p35", FemProfile::XY16P35},
};

}  // namespace

TEST(FemTxPowerSync, PythonCeilingsSitExactlyAtTheSrdLimitOfTheDriverEstimate) {
  auto ceilings = test::parse_python_uint8_dict("components/home_io_control/__init__.py", "FEM_TX_POWER_MAX_QUIET");
  ASSERT_EQ(ceilings.size(), std::size(kFemProfiles)) << "a FEM part was added or removed on one side only";

  for (const auto &fem : kFemProfiles) {
    ASSERT_TRUE(ceilings.count(fem.yaml_name)) << "FEM_TX_POWER_MAX_QUIET is missing \"" << fem.yaml_name << "\"";
    const uint8_t ceiling = ceilings[fem.yaml_name];
    EXPECT_LE(sx1262_fem_tx_estimate(fem.profile, ceiling).antenna_dbm, SRD_LIMIT_DBM)
        << fem.yaml_name << ": the documented ceiling must not exceed the SRD limit";
    EXPECT_GT(sx1262_fem_tx_estimate(fem.profile, static_cast<uint8_t>(ceiling + 1)).antenna_dbm, SRD_LIMIT_DBM)
        << fem.yaml_name << ": one step above the ceiling must exceed it, or the ceiling is needlessly low";
  }
}

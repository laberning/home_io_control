/// @file pa_pin_sync_test.cpp
/// @brief Guards the SX1276 PA_BOOST selector against Python/C++ drift.
///
/// The `pa_pin` YAML option arrives in the driver as a register-shaped byte: `0x80` selects the
/// PA_BOOST output, anything else the RFO pin. Three places name that value — `PA_PIN_OPTIONS` in
/// `__init__.py` (the YAML side, source of truth here), `SX1276_PA_SELECT_PA_BOOST` in the driver
/// header, and the hub's `DEFAULT_PA_PIN_PA_BOOST` schema default. This test parses `__init__.py`
/// at runtime (same mechanism as tcxo_voltage_sync_test.cpp) and pins all three together, so a
/// change on one side cannot silently flip which pin an SX1276 board transmits on.

#include "hub_core.h"
#include "radio_sx1276.h"
#include "support/python_dict_parser.h"
#include "test_helpers.h"

using namespace esphome::home_io_control;

TEST(PaPinSync, BoostSelectorMatchesPythonAndTheHubDefault) {
  auto options = test::parse_python_uint8_dict("components/home_io_control/__init__.py", "PA_PIN_OPTIONS");
  ASSERT_TRUE(options.count("BOOST")) << "PA_PIN_OPTIONS lost its BOOST entry";
  EXPECT_EQ(options["BOOST"], SX1276_PA_SELECT_PA_BOOST)
      << "radio_sx1276.h SX1276_PA_SELECT_PA_BOOST drifted from PA_PIN_OPTIONS[\"BOOST\"]";
  EXPECT_EQ(DEFAULT_PA_PIN_PA_BOOST, SX1276_PA_SELECT_PA_BOOST)
      << "hub_core.h DEFAULT_PA_PIN_PA_BOOST drifted from the driver's selector";
}

TEST(PaPinSync, RfoIsEveryOtherValue) {
  auto options = test::parse_python_uint8_dict("components/home_io_control/__init__.py", "PA_PIN_OPTIONS");
  ASSERT_TRUE(options.count("RFO")) << "PA_PIN_OPTIONS lost its RFO entry";
  EXPECT_NE(options["RFO"], SX1276_PA_SELECT_PA_BOOST) << "RFO must not select PA_BOOST";
}

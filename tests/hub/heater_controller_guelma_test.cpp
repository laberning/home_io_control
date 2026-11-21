/// @file heater_controller_guelma_test.cpp
/// @brief HeaterController on the production rows of a Sauter Guelma connecte (io_model: electric_water_heater): an
/// electric storage water heater whose state is all registers, whose target is a shower count written in manual mode
/// only, and whose boost and absence are flag registers (issue #247).
///
/// The replies are the heater's own, from the corpus captures `sauter_guelma_exchange_register_read` and
/// `_register_write`; the frames sent are checked byte for byte.

#include "heater_controller.h"
#include "proto_device_readings.h"
#include "proto_dhw.h"
#include "proto_heating.h"

#include "heater_controller_fixture.h"
#include "heater_controller_helpers.h"
#include "test_helpers.h"

#include <string>
#include <vector>

using namespace esphome::home_io_control;
using namespace heater_controller_test;

namespace {

/// The payload `0C 61 00 <register> <value>` of a register write, or `0C 60 00 <register>` of a read.
std::vector<uint8_t> write_payload(uint8_t register_low, uint8_t value) {
  return {0x0C, 0x61, 0x00, register_low, value};
}
std::vector<uint8_t> read_payload(uint8_t register_low) { return {0x0C, 0x60, 0x00, register_low}; }

/// A Sauter Guelma declared as `io_model: electric_water_heater`.
IoDevice *make_guelma(HeaterControllerFixture &fx) {
  DeviceConfig config;
  config.type = DeviceType::DOMESTIC_HOT_WATER;
  config.model = DeviceModel::ELECTRIC_WATER_HEATER;
  config.entity_class = DeviceCapabilityClass::WATER_HEATER;
  IoDevice *dev = fx.add_device(config);
  EXPECT_NE(dev, nullptr);
  return dev;
}

/// A Guelma whose mode has been read as `mode`.
IoDevice *guelma_in(HeaterControllerFixture &fx, DhwMode mode) {
  IoDevice *dev = make_guelma(fx);
  test::store_reading(*dev, DeviceReading::HOT_WATER_MODE, static_cast<int>(mode));
  return dev;
}

}  // namespace

// === The model ===

TEST(GuelmaHeater, TheModelCarriesOneMemberOfEachWaterHeaterGroupAndNoneOfTheCalypsos) {
  HeaterControllerFixture fx;
  const IoDevice *dev = make_guelma(fx);
  for (const DeviceFeature member : {DeviceFeature::WATER_HEATER, DeviceFeature::HOT_WATER_REGISTERS_GUELMA,
                                     DeviceFeature::HOT_WATER_TARGET_SHOWER_STEPS, DeviceFeature::HOT_WATER_BOOST_FLAG,
                                     DeviceFeature::HOT_WATER_AWAY_FLAG})
    EXPECT_TRUE(has_feature(dev->features, member)) << static_cast<int>(member);
  for (const DeviceFeature calypso :
       {DeviceFeature::HOT_WATER_REGISTERS_CALYPSO, DeviceFeature::HOT_WATER_OPERATING_MODE_WORD,
        DeviceFeature::HOT_WATER_TARGET_20_62, DeviceFeature::HOT_WATER_BOOST_FP16, DeviceFeature::HOT_WATER_AWAY_FP16})
    EXPECT_FALSE(has_feature(dev->features, calypso)) << static_cast<int>(calypso);
  EXPECT_FALSE(device_has_status_reading(*dev)) << "its status reply carries nothing";
}

// === The poll ===

TEST(GuelmaHeater, ThePollReadsItsRegistersInOrderAndSendsNoStatusRead) {
  HeaterControllerFixture fx;
  const IoDevice *dev = make_guelma(fx);
  // The heater's answers from the corpus capture, in the order the poll reads them.
  fx.queue_register_ack({0x0C, 0x60, 0x00, 0x17, 0x01, 0x8E});              // water 39.8 C
  fx.queue_register_ack({0x0C, 0x60, 0x00, 0x06, 0x02});                    // manual
  fx.queue_register_ack({0x0C, 0x60, 0x00, 0x49, 0x02, 0xBC});              // target 70.0 C
  fx.queue_register_ack({0x0C, 0x60, 0x00, 0x52, 0x00});                    // boost off
  fx.queue_register_ack({0x0C, 0x60, 0x00, 0x53, 0x00});                    // absence off
  fx.queue_register_ack({0x0C, 0x60, 0x00, 0x46, 0x02});                    // two showers available
  fx.queue_register_ack({0x0C, 0x60, 0x00, 0x4A, 0x02});                    // two showers aimed for
  fx.queue_register_ack({0x0C, 0x60, 0x00, 0x32, 0x05, 0xDC});              // 1500 W
  fx.queue_register_ack({0x0C, 0x60, 0x00, 0x04, 0x00, 0xEA, 0x94, 0x74});  // 15 373 428 Wh

  EXPECT_TRUE(fx.controller.poll("ABC123"));

  EXPECT_TRUE(fx.status_reads.empty()) << "no row of the heater reads from a status reply";
  EXPECT_TRUE(fx.executes.empty());
  const auto &sent = fx.radio.get_sent_data();
  const std::vector<uint8_t> order = {0x17, 0x06, 0x49, 0x52, 0x53, 0x46, 0x4A, 0x32, 0x04};
  ASSERT_EQ(sent.size(), order.size()) << "none of the Calypso's registers (0x00, 0x05, 0x07, 0x21)";
  for (size_t i = 0; i < order.size(); i++)
    EXPECT_EQ(sent_payload(sent[i], 4), read_payload(order[i])) << i;

  EXPECT_FLOAT_EQ(*reading_value(*dev, DeviceReading::HOT_WATER_TEMPERATURE), 39.8F);
  EXPECT_EQ(reading_wire(*dev, DeviceReading::HOT_WATER_MODE), static_cast<int>(DhwMode::MANUAL_ECO_OFF));
  EXPECT_FLOAT_EQ(*reading_value(*dev, DeviceReading::HOT_WATER_TARGET), 70.0F);
  EXPECT_EQ(reading_wire(*dev, DeviceReading::HOT_WATER_BOOST), 0);
  EXPECT_EQ(reading_wire(*dev, DeviceReading::HOT_WATER_AWAY), 0);
  EXPECT_EQ(reading_wire(*dev, DeviceReading::HOT_WATER_SHOWERS_AVAILABLE), 2);
  EXPECT_EQ(reading_wire(*dev, DeviceReading::HOT_WATER_SHOWERS_TARGET), 2);
  EXPECT_FLOAT_EQ(*reading_value(*dev, DeviceReading::HOT_WATER_HEATING_POWER), 1500.0F);
  EXPECT_NEAR(*reading_value(*dev, DeviceReading::HOT_WATER_ENERGY), 15373.428F, 0.01F) << "kWh";
}

// === The target ===

TEST(GuelmaHeater, SixtyDegreesInManualModeWritesThreeShowersAndReadsTheTargetBack) {
  HeaterControllerFixture fx;
  const IoDevice *dev = guelma_in(fx, DhwMode::MANUAL_ECO_OFF);
  fx.queue_register_ack({0x0C, 0x61, 0x00, 0x48, 0x03});        // the ack the heater sent (corpus)
  fx.queue_register_ack({0x0C, 0x60, 0x00, 0x49, 0x02, 0x58});  // the read-back: 60.0 C

  EXPECT_TRUE(fx.controller.set_water_heater_target("ABC123", 60.0F));

  const auto &sent = fx.radio.get_sent_data();
  ASSERT_EQ(sent.size(), 2u);
  EXPECT_EQ(sent_payload(sent[0], 5), write_payload(0x48, 3)) << "60 C is three showers";
  EXPECT_EQ(sent_payload(sent[1], 4), read_payload(0x49)) << "the temperature it heats to";
  EXPECT_TRUE(fx.executes.empty()) << "no main-parameter EXECUTE";
  EXPECT_TRUE(fx.status_reads.empty());
  EXPECT_FLOAT_EQ(*reading_value(*dev, DeviceReading::HOT_WATER_TARGET), 60.0F);
}

TEST(GuelmaHeater, FiftyAndSeventyDegreesAreTwoAndFourShowers) {
  for (const auto &[celsius, showers] : {std::pair{50.0F, 2}, std::pair{70.0F, 4}}) {
    HeaterControllerFixture fx;
    guelma_in(fx, DhwMode::MANUAL_ECO_OFF);
    fx.queue_register_ack({0x0C, 0x61, 0x00, 0x48, static_cast<uint8_t>(showers)});
    EXPECT_TRUE(fx.controller.set_water_heater_target("ABC123", celsius));
    ASSERT_FALSE(fx.radio.get_sent_data().empty());
    EXPECT_EQ(sent_payload(fx.radio.get_sent_data()[0], 5), write_payload(0x48, static_cast<uint8_t>(showers)))
        << celsius;
  }
}

TEST(GuelmaHeater, ATargetBetweenTheStepsOrOutsideThemSendsNothing) {
  HeaterControllerFixture fx;
  guelma_in(fx, DhwMode::MANUAL_ECO_OFF);
  for (const float celsius : {55.0F, 65.0F, 42.0F, 7.0F, 75.0F})
    EXPECT_FALSE(fx.controller.set_water_heater_target("ABC123", celsius)) << celsius;
  EXPECT_TRUE(fx.radio.get_sent_data().empty()) << "refused, never rounded";
}

TEST(GuelmaHeater, ATargetInAutomaticModeIsRefusedWithoutAFrame) {
  HeaterControllerFixture fx;
  guelma_in(fx, DhwMode::AUTO);
  EXPECT_FALSE(fx.controller.set_water_heater_target("ABC123", 60.0F));
  EXPECT_TRUE(fx.radio.get_sent_data().empty()) << "the heater picks its own target; the manual setting stays";
}

TEST(GuelmaHeater, TheCardOffersFiftyToSeventyInStepsOfTen) {
  HeaterControllerFixture fx;
  make_guelma(fx);
  const HeatingCardRange range = fx.controller.water_heater_target_range("ABC123");
  EXPECT_FLOAT_EQ(range.min_c, 50.0F);
  EXPECT_FLOAT_EQ(range.max_c, 70.0F);
  EXPECT_EQ(fx.controller.water_heater_target_step("ABC123"), 10.0F);
}

// === The mode ===

TEST(GuelmaHeater, TheModeRowNamesAutomaticAndElectricOnly) {
  HeaterControllerFixture fx;
  make_guelma(fx);
  EXPECT_STREQ(fx.controller.water_heater_mode_name("ABC123", DhwMode::AUTO), "auto");
  EXPECT_STREQ(fx.controller.water_heater_mode_name("ABC123", DhwMode::MANUAL_ECO_OFF), "manual electric");
  EXPECT_EQ(fx.controller.water_heater_mode_name("ABC123", DhwMode::MANUAL_ECO_ON), nullptr) << "no heat pump";
}

TEST(GuelmaHeater, ManualModeIsWrittenAndTheTargetReadBack) {
  HeaterControllerFixture fx;
  const IoDevice *dev = guelma_in(fx, DhwMode::AUTO);
  fx.queue_register_ack({0x0C, 0x61, 0x00, 0x06, 0x02});        // the ack the heater sent (corpus)
  fx.queue_register_ack({0x0C, 0x60, 0x00, 0x06, 0x02});        // the mode read back
  fx.queue_register_ack({0x0C, 0x60, 0x00, 0x49, 0x01, 0xF4});  // the target read back: 50.0 C

  EXPECT_TRUE(fx.controller.set_water_heater_mode("ABC123", DhwMode::MANUAL_ECO_OFF));

  const auto &sent = fx.radio.get_sent_data();
  ASSERT_EQ(sent.size(), 3u);
  EXPECT_EQ(sent_payload(sent[0], 5), write_payload(0x06, 2));
  EXPECT_EQ(sent_payload(sent[1], 4), read_payload(0x06));
  EXPECT_EQ(sent_payload(sent[2], 4), read_payload(0x49));
  EXPECT_EQ(reading_wire(*dev, DeviceReading::HOT_WATER_MODE), static_cast<int>(DhwMode::MANUAL_ECO_OFF));
  EXPECT_FLOAT_EQ(*reading_value(*dev, DeviceReading::HOT_WATER_TARGET), 50.0F);
}

TEST(GuelmaHeater, TheHeatPumpModeItLacksIsNeverWritten) {
  HeaterControllerFixture fx;
  make_guelma(fx);
  EXPECT_FALSE(fx.controller.set_water_heater_mode("ABC123", DhwMode::MANUAL_ECO_ON));
  EXPECT_TRUE(fx.radio.get_sent_data().empty());
}

// === Boost and absence ===

TEST(GuelmaHeater, BoostAndAbsenceWriteTheirFlagsAndAreShownFromTheReadBack) {
  const struct {
    uint8_t register_low;
    DeviceReading reading;
    bool boost;
  } flags[] = {{0x52, DeviceReading::HOT_WATER_BOOST, true}, {0x53, DeviceReading::HOT_WATER_AWAY, false}};
  for (const auto &flag : flags) {
    for (const bool on : {true, false}) {
      HeaterControllerFixture fx;
      const IoDevice *dev = make_guelma(fx);
      fx.queue_register_ack({0x0C, 0x61, 0x00, flag.register_low, static_cast<uint8_t>(on)});
      fx.queue_register_ack({0x0C, 0x60, 0x00, flag.register_low, static_cast<uint8_t>(on)});

      EXPECT_TRUE(flag.boost ? fx.controller.set_water_heater_boost("ABC123", on)
                             : fx.controller.set_water_heater_away("ABC123", on));

      const auto &sent = fx.radio.get_sent_data();
      ASSERT_EQ(sent.size(), 2u) << "the flag, then its read-back: no duration write, no EXECUTE";
      EXPECT_EQ(sent_payload(sent[0], 5), write_payload(flag.register_low, on ? 1 : 0));
      EXPECT_EQ(sent_payload(sent[1], 4), read_payload(flag.register_low));
      EXPECT_TRUE(fx.executes.empty());
      EXPECT_TRUE(fx.status_reads.empty());
      EXPECT_EQ(reading_wire(*dev, flag.reading), on ? 1 : 0);
    }
  }
}

TEST(GuelmaHeater, ABoostTheHeaterRefusesInAbsenceIsShownOffFromTheReadBack) {
  HeaterControllerFixture fx;
  const IoDevice *dev = make_guelma(fx);
  fx.queue_register_ack({0x0C, 0x61, 0x00, 0x52, 0x01});  // acknowledged
  fx.queue_register_ack({0x0C, 0x60, 0x00, 0x52, 0x00});  // but not kept: the heater is in absence

  fx.controller.set_water_heater_boost("ABC123", true);

  EXPECT_EQ(reading_wire(*dev, DeviceReading::HOT_WATER_BOOST), 0) << "the entity shows what the heater holds";
}

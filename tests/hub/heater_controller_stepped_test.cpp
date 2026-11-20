/// @file heater_controller_stepped_test.cpp
/// @brief HeaterController: a water heater whose target, boost and away are registers and whose target takes only
/// steps (the shape in stepped_water_heater_rows.h), and the Calypso's poll that still reads the status.
///
/// The shape is the Sauter Guelma's as its contributor describes it: boost and away are flag registers, the target
/// is a shower count written to a register in manual mode only and read from another register, and the heater
/// answers no useful status read. The rows are test-only; the controller is the production one.

#include "heater_controller.h"
#include "reading_store.h"
#include "stepped_water_heater_rows.h"
#include "proto_commands.h"
#include "proto_device_readings.h"
#include "proto_dhw.h"
#include "proto_frame.h"
#include "proto_heating.h"
#include "proto_private_registers.h"

#include "log_capture.h"
#include "heater_controller_fixture.h"
#include "heater_controller_helpers.h"

#include <string>
#include <vector>

using namespace esphome::home_io_control;
using namespace heater_controller_test;
using namespace test::stepped;

namespace {

/// The payload `0C 61 00 <register> <value>` of a register write, or `0C 60 00 <register>` of a read.
std::vector<uint8_t> write_payload(uint8_t register_low, uint8_t value) {
  return {0x0C, 0x61, 0x00, register_low, value};
}
std::vector<uint8_t> read_payload(uint8_t register_low) { return {0x0C, 0x60, 0x00, register_low}; }

/// A water heater in manual mode, so the target's guard lets a write through.
IoDevice *manual_heater(HeaterControllerFixture &fx) {
  IoDevice *dev = make_water_heater(fx);
  test::store_reading(*dev, DeviceReading::HOT_WATER_MODE, static_cast<int>(DhwMode::MANUAL_ECO_ON));
  return dev;
}

/// The reading values of the shape, for readability.
int wire(const IoDevice &dev, DeviceReading reading) { return reading_wire(dev, reading); }

}  // namespace

// === The poll ===

TEST(SteppedHeater, ThePollReadsItsRegistersAndSendsNoStatusRead) {
  const SteppedHeaterTables tables;
  HeaterControllerFixture fx;
  const IoDevice *dev = make_water_heater(fx);
  fx.queue_register_ack({0x0C, 0x60, 0x00, 0x06, 0x01});  // mode: manual
  fx.queue_register_ack({0x0C, 0x60, 0x00, 0x49, 0x3C});  // target: 60
  fx.queue_register_ack({0x0C, 0x60, 0x00, 0x52, 0x01});  // boost on
  fx.queue_register_ack({0x0C, 0x60, 0x00, 0x53, 0x00});  // away off

  EXPECT_TRUE(fx.controller.poll("ABC123"));

  EXPECT_TRUE(fx.status_reads.empty()) << "no row of the heater reads from a status reply";
  const auto &sent = fx.radio.get_sent_data();
  ASSERT_EQ(sent.size(), 4u);
  EXPECT_EQ(sent_payload(sent[0], 4), read_payload(0x06));
  EXPECT_EQ(sent_payload(sent[1], 4), read_payload(0x49));
  EXPECT_EQ(sent_payload(sent[2], 4), read_payload(0x52));
  EXPECT_EQ(sent_payload(sent[3], 4), read_payload(0x53));
  EXPECT_EQ(wire(*dev, DeviceReading::HOT_WATER_MODE), 1);
  EXPECT_EQ(reading_value(*dev, DeviceReading::HOT_WATER_TARGET), 60.0F);
  EXPECT_EQ(wire(*dev, DeviceReading::HOT_WATER_BOOST), 1);
  EXPECT_EQ(wire(*dev, DeviceReading::HOT_WATER_AWAY), 0);
}

TEST(SteppedHeater, TheCalypsosPollStillReadsTheStatusFirstWithTheFpiBytesItHasAlwaysAsked) {
  HeaterControllerFixture fx;
  const IoDevice *dev = make_water_heater(fx);

  fx.controller.poll("ABC123");  // the registers get no answer here; the status read is what is checked

  ASSERT_GE(fx.status_reads.size(), 1u);
  IoFrame expected;
  ASSERT_TRUE(create_status_mp_fp_read(expected, fx.node_id, dev->node_id, dev->low_power, 0x00, 0x43));
  const IoFrame &request = fx.status_reads[0].request;
  ASSERT_EQ(request.data_len, expected.data_len);
  EXPECT_EQ(std::vector<uint8_t>(request.data, request.data + request.data_len),
            std::vector<uint8_t>(expected.data, expected.data + expected.data_len));
  EXPECT_EQ(DHW_STATUS_FPI1, 0x00);
  EXPECT_EQ(DHW_STATUS_FPI2, 0x43) << "FP10, FP15 and FP16";
}

// === The target ===

TEST(SteppedHeater, AStepIsWrittenAsItsShowerCountAndReadBackFromTheTargetRegister) {
  const SteppedHeaterTables tables;
  HeaterControllerFixture fx;
  const IoDevice *dev = manual_heater(fx);
  fx.queue_register_ack({0x0C, 0x61, 0x00, 0x48, 0x04});  // the write's ack
  fx.queue_register_ack({0x0C, 0x60, 0x00, 0x49, 0x46});  // the read-back: 70

  EXPECT_TRUE(fx.controller.set_water_heater_target("ABC123", 70.0F));

  const auto &sent = fx.radio.get_sent_data();
  ASSERT_EQ(sent.size(), 2u);
  EXPECT_EQ(sent_payload(sent[0], 5), write_payload(0x48, 4)) << "70 degrees is four showers";
  EXPECT_EQ(sent_payload(sent[1], 4), read_payload(0x49)) << "the row's read side";
  EXPECT_TRUE(fx.executes.empty());
  EXPECT_TRUE(fx.status_reads.empty());
  EXPECT_EQ(reading_value(*dev, DeviceReading::HOT_WATER_TARGET), 70.0F);
}

TEST(SteppedHeater, EachStepIsItsOwnCount) {
  const SteppedHeaterTables tables;
  for (const auto &[celsius, count] : {std::pair{50.0F, 2}, std::pair{60.0F, 3}}) {
    HeaterControllerFixture fx;
    manual_heater(fx);
    fx.queue_register_ack({0x0C, 0x61, 0x00, 0x48, static_cast<uint8_t>(count)});
    EXPECT_TRUE(fx.controller.set_water_heater_target("ABC123", celsius));
    EXPECT_EQ(sent_payload(fx.radio.get_sent_data().at(0), 5), write_payload(0x48, static_cast<uint8_t>(count)))
        << celsius;
  }
}

TEST(SteppedHeater, AValueBetweenTheStepsOrOutsideThemIsRefusedWithoutAFrame) {
  const SteppedHeaterTables tables;
  HeaterControllerFixture fx;
  manual_heater(fx);
  for (const float celsius : {55.0F, 65.0F, 49.0F, 71.0F, 20.0F, NAN})
    EXPECT_FALSE(fx.controller.set_water_heater_target("ABC123", celsius)) << celsius;
  EXPECT_TRUE(fx.radio.get_sent_data().empty());
  EXPECT_TRUE(fx.executes.empty());
}

TEST(SteppedHeater, TheTargetIsRefusedWithoutAFrameOutsideManualModeAndTriedBeforeTheModeIsRead) {
  const SteppedHeaterTables tables;
  {
    HeaterControllerFixture fx;
    IoDevice *dev = make_water_heater(fx);
    test::store_reading(*dev, DeviceReading::HOT_WATER_MODE, static_cast<int>(DhwMode::AUTO));
    EXPECT_FALSE(fx.controller.set_water_heater_target("ABC123", 60.0F));
    EXPECT_TRUE(fx.radio.get_sent_data().empty()) << "automatic mode: the heater chooses its own target";
  }
  {
    HeaterControllerFixture fx;
    make_water_heater(fx);
    EXPECT_FALSE(fx.controller.set_water_heater_target("ABC123", 60.0F)) << "no answer here";
    ASSERT_FALSE(fx.radio.get_sent_data().empty()) << "the mode has not been read: the write is tried";
    EXPECT_EQ(sent_payload(fx.radio.get_sent_data()[0], 5), write_payload(0x48, 3));
  }
}

TEST(SteppedHeater, TheCardOffersTheRangeAndTheStepOfTheTargetRow) {
  const SteppedHeaterTables tables;
  HeaterControllerFixture fx;
  make_water_heater(fx);
  const HeatingCardRange range = fx.controller.water_heater_target_range("ABC123");
  EXPECT_FLOAT_EQ(range.min_c, 50.0F);
  EXPECT_FLOAT_EQ(range.max_c, 70.0F);
  EXPECT_EQ(fx.controller.water_heater_target_step("ABC123"), 10.0F);
  EXPECT_FALSE(fx.controller.water_heater_target_step("FFFFFF").has_value()) << "an unknown device has none";
}

TEST(SteppedHeater, TheCalypsosCardStepIsNoneSoTheCardKeepsItsWholeDegree) {
  HeaterControllerFixture fx;
  make_water_heater(fx);
  EXPECT_FALSE(fx.controller.water_heater_target_step("ABC123").has_value());
}

// === Boost and away ===

TEST(SteppedHeater, BoostAndAwayAreFlagWritesToTheirRegistersShownFromTheReadBack) {
  const SteppedHeaterTables tables;
  const struct {
    uint8_t register_low;
    DeviceReading reading;
    bool boost;
  } features[] = {{0x52, DeviceReading::HOT_WATER_BOOST, true}, {0x53, DeviceReading::HOT_WATER_AWAY, false}};
  for (const auto &feature : features) {
    for (const bool on : {true, false}) {
      HeaterControllerFixture fx;
      const IoDevice *dev = make_water_heater(fx);
      fx.queue_register_ack({0x0C, 0x61, 0x00, feature.register_low, static_cast<uint8_t>(on)});
      fx.queue_register_ack({0x0C, 0x60, 0x00, feature.register_low, static_cast<uint8_t>(on)});

      EXPECT_TRUE(feature.boost ? fx.controller.set_water_heater_boost("ABC123", on)
                                : fx.controller.set_water_heater_away("ABC123", on));

      const auto &sent = fx.radio.get_sent_data();
      ASSERT_EQ(sent.size(), 2u) << "the flag, then its read-back; no duration write, no EXECUTE";
      EXPECT_EQ(sent_payload(sent[0], 5), write_payload(feature.register_low, on ? 1 : 0));
      EXPECT_EQ(sent_payload(sent[1], 4), read_payload(feature.register_low));
      EXPECT_TRUE(fx.executes.empty());
      EXPECT_TRUE(fx.status_reads.empty());
      EXPECT_EQ(wire(*dev, feature.reading), on ? 1 : 0) << "shown from the read-back";
    }
  }
}

TEST(SteppedHeater, BoostAndAwayAreIndependentFlags) {
  const SteppedHeaterTables tables;
  HeaterControllerFixture fx;
  const IoDevice *dev = make_water_heater(fx);
  fx.queue_register_ack({0x0C, 0x61, 0x00, 0x52, 0x01});
  fx.queue_register_ack({0x0C, 0x60, 0x00, 0x52, 0x01});
  fx.queue_register_ack({0x0C, 0x61, 0x00, 0x53, 0x01});
  fx.queue_register_ack({0x0C, 0x60, 0x00, 0x53, 0x01});

  EXPECT_TRUE(fx.controller.set_water_heater_boost("ABC123", true));
  EXPECT_TRUE(fx.controller.set_water_heater_away("ABC123", true));

  EXPECT_EQ(wire(*dev, DeviceReading::HOT_WATER_BOOST), 1) << "away does not end boost on this heater";
  EXPECT_EQ(wire(*dev, DeviceReading::HOT_WATER_AWAY), 1);
}

TEST(SteppedHeater, ARefusedStepIsLoggedWithTheStepsTheHeaterAccepts) {
  const SteppedHeaterTables tables;
  HeaterControllerFixture fx;
  manual_heater(fx);
  const test::LogCapture log;
  EXPECT_FALSE(fx.controller.set_water_heater_target("ABC123", 55.0F));
  size_t warnings = 0;
  for (const auto &line : log.lines()) {
    if (line.level == 'W' && line.format.find("accepts") != std::string::npos)
      warnings++;
  }
  EXPECT_EQ(warnings, 1u) << "one warning that names the steps";
  const DeviceReadingRow target = test::stepped::target_row();
  EXPECT_EQ(value_write_steps(target), (std::vector<float>{50.0F, 60.0F, 70.0F}));
}

// === The production table: a water heater of another register map ===

namespace {

/// A water heater that has none of the Calypso's register map, boost or away: the capabilities another model would
/// carry, on the production reading table.
IoDevice *other_model_heater(HeaterControllerFixture &fx) {
  IoDevice *dev = make_water_heater(fx);
  dev->features = DeviceFeature::WATER_HEATER;
  return dev;
}

}  // namespace

TEST(WaterHeaterRegisterMap, AHeaterWithoutTheCalypsosMembersSendsNoStatusReadAndPollsNoneOfItsRegisters) {
  HeaterControllerFixture fx;
  other_model_heater(fx);

  EXPECT_TRUE(fx.controller.poll("ABC123"));

  EXPECT_TRUE(fx.status_reads.empty()) << "no selected row reads from a status reply";
  EXPECT_TRUE(fx.radio.get_sent_data().empty()) << "none of the Calypso's registers is read";
}

TEST(WaterHeaterRegisterMap, AHeaterWithoutTheCalypsosMembersIsNeverWrittenItsModeBoostOrAway) {
  HeaterControllerFixture fx;
  other_model_heater(fx);
  EXPECT_FALSE(fx.controller.set_water_heater_mode("ABC123", DhwMode::AUTO));
  EXPECT_FALSE(fx.controller.set_water_heater_boost("ABC123", true));
  EXPECT_FALSE(fx.controller.set_water_heater_away("ABC123", false));
  EXPECT_TRUE(fx.radio.get_sent_data().empty());
  EXPECT_TRUE(fx.executes.empty());
}

TEST(WaterHeaterRegisterMap, TheCalypsosPollReadsTheStatusThenItsWaterTemperatureRegisterFirst) {
  HeaterControllerFixture fx;
  make_water_heater(fx);
  fx.controller.poll("ABC123");
  ASSERT_EQ(fx.status_reads.size(), 1u);
  ASSERT_FALSE(fx.radio.get_sent_data().empty());
  EXPECT_EQ(sent_payload(fx.radio.get_sent_data()[0], 4), read_payload(0x17));
}

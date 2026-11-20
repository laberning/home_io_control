/// @file heater_controller_dhw_test.cpp
/// @brief HeaterController: the water heater's writes and reads (HeaterControllerFixture).

#include "heater_controller.h"
#include "payload_frame.h"
#include "reading_store.h"
#include "water_heater_values.h"
#include "decisions_status.h"
#include "proto_codecs.h"
#include "proto_commands.h"
#include "proto_device_readings.h"
#include "proto_dhw.h"
#include "proto_frame.h"
#include "proto_heating.h"
#include "proto_heating_profiles.h"
#include "proto_private_registers.h"

#include "heater_controller_fixture.h"
#include "heater_controller_helpers.h"

#include <array>
#include <cmath>
#include <functional>
#include <vector>

using namespace esphome::home_io_control;
using namespace heater_controller_test;

// === Water heater ===

TEST(HeaterController, ATargetOutsideTheHeatersRangeIsNeverSent) {
  HeaterControllerFixture fx;
  make_water_heater(fx);
  EXPECT_FALSE(fx.controller.set_water_heater_target("ABC123", 19.5F));
  EXPECT_FALSE(fx.controller.set_water_heater_target("ABC123", 62.5F));
  EXPECT_FALSE(fx.controller.set_water_heater_target("ABC123", NAN));
  EXPECT_TRUE(fx.radio.get_sent_data().empty());
  EXPECT_TRUE(fx.executes.empty());
}

TEST(HeaterController, TheTargetIsAMainParameterOnlyExecuteThroughTheBareOperation) {
  HeaterControllerFixture fx;
  const IoDevice *dev = make_water_heater(fx);

  EXPECT_TRUE(fx.controller.set_water_heater_target("ABC123", 54.0F));

  ASSERT_EQ(fx.executes.size(), 1u);
  EXPECT_EQ(fx.executes[0].action, "hot water target");
  EXPECT_EQ(fx.executes[0].rejection_profile, "water heater");
  EXPECT_EQ(fx.executes[0].kind, HeaterExecuteKind::TRY) << "a water heater never moves: no overlay to roll back";
  ASSERT_TRUE(fx.executes[0].built);
  IoFrame expected;
  ASSERT_TRUE(create_execute_heating_main_parameter(expected, fx.node_id, dev->node_id, false, 0xA1E8));
  uint8_t expected_raw[64];
  uint8_t got_raw[64];
  const uint8_t expected_len = serialize(expected, expected_raw, sizeof(expected_raw));
  const uint8_t got_len = serialize(fx.executes[0].request, got_raw, sizeof(got_raw));
  EXPECT_EQ(std::vector<uint8_t>(got_raw, got_raw + got_len),
            std::vector<uint8_t>(expected_raw, expected_raw + expected_len));
  ASSERT_EQ(fx.timers.size(), 1u);
  EXPECT_EQ(fx.timers[0].id, decisions::heating_poll_timer_id(dev->node_id));
  EXPECT_EQ(fx.timers[0].delay_ms, HEATING_POST_WRITE_POLL_DELAY_MS) << "the heating state is read again soon";
}

TEST(HeaterController, TheTargetIsReadBackByTheStatusReadOfItsRowAndAMissedReadDoesNotFailTheWrite) {
  HeaterControllerFixture fx;
  make_water_heater(fx);

  EXPECT_TRUE(fx.controller.set_water_heater_target("ABC123", 54.0F));
  ASSERT_EQ(fx.status_reads.size(), 1u) << "the row's read side is the status reply";
  EXPECT_EQ(fx.status_reads[0].request.data[0], PRIVATE_STATUS_MP_FP);

  HeaterControllerFixture missed;
  make_water_heater(missed);
  missed.status_read_result = false;
  EXPECT_TRUE(missed.controller.set_water_heater_target("ABC123", 54.0F)) << "the write was accepted";
}

TEST(HeaterController, AValueWriteOfAReadingWithNoWriteSideIsRefusedWithoutAFrame) {
  HeaterControllerFixture fx;
  make_water_heater(fx);
  EXPECT_FALSE(fx.controller.write_value("ABC123", DeviceReading::HOT_WATER_ENERGY, 1.0F));
  EXPECT_TRUE(fx.radio.get_sent_data().empty());
  EXPECT_TRUE(fx.executes.empty());
}

TEST(HeaterController, AValueWriteToADeviceThatIsNoWaterHeaterIsRefusedWithoutAFrame) {
  HeaterControllerFixture fx;
  make_cover(fx);
  EXPECT_FALSE(fx.controller.write_value("ABC123", DeviceReading::HOT_WATER_TARGET, 54.0F));
  EXPECT_FALSE(fx.controller.set_water_heater_mode("ABC123", DhwMode::AUTO));
  EXPECT_TRUE(fx.radio.get_sent_data().empty());
  EXPECT_TRUE(fx.executes.empty());
}

TEST(HeaterController, ABadModeValueIsJudgedByTheRegistersRuleBeforeAnyFrame) {
  HeaterControllerFixture fx;
  make_water_heater(fx);
  EXPECT_FALSE(fx.controller.write_value("ABC123", DeviceReading::HOT_WATER_MODE, 3.0F));
  EXPECT_TRUE(fx.radio.get_sent_data().empty());
}

TEST(HeaterController, TheCardRangeOfAWaterHeaterIsTheRangeOfItsTargetRow) {
  HeaterControllerFixture fx;
  const IoDevice *dev = make_water_heater(fx);
  const HeatingCardRange range = fx.controller.water_heater_target_range("ABC123");
  const HeatingCardRange row_range = device_temperature_range(*dev);
  EXPECT_FLOAT_EQ(range.min_c, row_range.min_c);
  EXPECT_FLOAT_EQ(range.max_c, row_range.max_c);
  EXPECT_FLOAT_EQ(range.max_c, DHW_TARGET_MAX_C) << "the Calypso's row";
  EXPECT_TRUE(std::isnan(fx.controller.water_heater_target_range("FFFFFF").min_c)) << "an unknown device has none";
}

TEST(HeaterController, ARefusedTargetExecuteMovesNoPoll) {
  HeaterControllerFixture fx;
  make_water_heater(fx);
  fx.execute_result = false;

  EXPECT_FALSE(fx.controller.set_water_heater_target("ABC123", 54.0F));
  EXPECT_TRUE(fx.timers.empty());
}

TEST(HeaterController, AnUnacknowledgedModeWriteIsNotReadBack) {
  HeaterControllerFixture fx;
  const IoDevice *dev = make_water_heater(fx);
  EXPECT_FALSE(fx.controller.set_water_heater_mode("ABC123", DhwMode::AUTO));
  for (const auto &tx : fx.radio.get_sent_data())
    EXPECT_EQ(tx[10], PRIVATE_REGISTER_DIRECTION_SET) << "only the write is tried";
  EXPECT_LE(fx.radio.get_sent_data().size(), static_cast<size_t>(EXCHANGE_RETRY_COUNT));
  EXPECT_EQ(test::water_heater_values(*dev).mode, -1);
  EXPECT_TRUE(fx.status_reads.empty());
}

TEST(HeaterController, TheModeIsARegisterWriteReadBackAtOnceThenTheStatusIsReadForTheTarget) {
  HeaterControllerFixture fx;
  const IoDevice *dev = make_water_heater(fx);
  fx.queue_register_ack({0x0C, 0x61, 0x00, 0x06, 0x02});
  fx.queue_register_ack({0x0C, 0x60, 0x00, 0x06, 0x02});

  EXPECT_TRUE(fx.controller.set_water_heater_mode("ABC123", DhwMode::MANUAL_ECO_OFF));

  ASSERT_EQ(fx.radio.get_sent_data().size(), 2u);
  const auto &write = fx.radio.get_sent_data()[0];
  EXPECT_EQ(write[8], CMD_WRITE_PRIVATE);
  EXPECT_EQ(sent_payload(write, 5), (std::vector<uint8_t>{0x0C, 0x61, 0x00, 0x06, 0x02}));
  EXPECT_EQ(sent_payload(fx.radio.get_sent_data()[1], 4), (std::vector<uint8_t>{0x0C, 0x60, 0x00, 0x06}));
  EXPECT_EQ(test::water_heater_values(*dev).mode, static_cast<int16_t>(DhwMode::MANUAL_ECO_OFF));
  ASSERT_EQ(fx.status_reads.size(), 1u) << "the target follows the mode: the poll's status read";
  EXPECT_EQ(fx.status_reads[0].request.cmd, CMD_PRIVATE);
  EXPECT_EQ(fx.status_reads[0].request.data[0], PRIVATE_STATUS_MP_FP);
  EXPECT_EQ(fx.status_reads[0].retry_after_fail_ms, 0u) << "the status read arms no backoff of its own";
  EXPECT_EQ(fx.status_reads[0].max_tries, EXCHANGE_RETRY_COUNT);
  ASSERT_FALSE(fx.timers.empty());
  EXPECT_EQ(fx.timers.back().delay_ms, HEATING_POST_WRITE_POLL_DELAY_MS);
}

TEST(HeaterController, AMissedStatusReadAfterAModeWriteDoesNotFailTheWrite) {
  HeaterControllerFixture fx;
  make_water_heater(fx);
  fx.status_read_result = false;
  fx.queue_register_ack({0x0C, 0x61, 0x00, 0x06, 0x00});
  fx.queue_register_ack({0x0C, 0x60, 0x00, 0x06, 0x00});

  EXPECT_TRUE(fx.controller.set_water_heater_mode("ABC123", DhwMode::AUTO));
  ASSERT_FALSE(fx.timers.empty());
  EXPECT_EQ(fx.timers.back().delay_ms, HEATING_POST_WRITE_POLL_DELAY_MS) << "the poll still reads it soon";
}

TEST(HeaterController, BoostOnWritesTheDurationThenTheBoostWordThenReadsTheStatus) {
  HeaterControllerFixture fx;
  make_water_heater(fx);
  fx.queue_register_ack({0x0C, 0x61, 0x00, 0x00, 0x00});  // the heater acks the duration without keeping it
  fx.queue_register_ack({0x0C, 0x60, 0x00, 0x00, 0x01});  // its read-back

  EXPECT_TRUE(fx.controller.set_water_heater_boost("ABC123", true));

  ASSERT_EQ(fx.radio.get_sent_data().size(), 2u);
  EXPECT_EQ(sent_payload(fx.radio.get_sent_data()[0], 5), (std::vector<uint8_t>{0x0C, 0x61, 0x00, 0x00, 0x01}))
      << "the duration goes first, although the heater ignores it on its own";
  ASSERT_EQ(fx.executes.size(), 1u);
  EXPECT_EQ(fx.executes[0].action, "hot water boost");
  EXPECT_EQ(fx.executes[0].kind, HeaterExecuteKind::TRY);
  ASSERT_TRUE(fx.executes[0].built);
  const IoFrame &execute = fx.executes[0].request;
  EXPECT_EQ(std::vector<uint8_t>(execute.data + execute.data_len - 6, execute.data + execute.data_len),
            (std::vector<uint8_t>{0xD4, 0x00, 0x00, 0x01, 0x49, 0x00}));
  EXPECT_EQ(fx.status_reads.size(), 1u) << "the status is read at once";
}

TEST(HeaterController, SwitchingOffSendsTheOffWordOnlyAndReadsTheStatus) {
  for (const bool boost : {true, false}) {
    HeaterControllerFixture fx;
    make_water_heater(fx);

    EXPECT_TRUE(boost ? fx.controller.set_water_heater_boost("ABC123", false)
                      : fx.controller.set_water_heater_away("ABC123", false));

    EXPECT_TRUE(fx.radio.get_sent_data().empty()) << boost << ": no duration write, no read-back of one";
    ASSERT_EQ(fx.executes.size(), 1u);
    EXPECT_EQ(fx.executes[0].action, boost ? "hot water boost off" : "hot water away off");
    const IoFrame &execute = fx.executes[0].request;
    EXPECT_EQ(std::vector<uint8_t>(execute.data + execute.data_len - 6, execute.data + execute.data_len),
              (std::vector<uint8_t>{0xD4, 0x00, 0x00, 0x01, 0x4A, 0x00}));
    EXPECT_EQ(fx.status_reads.size(), 1u);
  }
}

TEST(HeaterController, AnUnacknowledgedModeWordIsNotReadBack) {
  HeaterControllerFixture fx;
  make_water_heater(fx);
  fx.execute_result = false;

  EXPECT_FALSE(fx.controller.set_water_heater_boost("ABC123", false));
  EXPECT_TRUE(fx.status_reads.empty());
}

TEST(HeaterController, AWriteWhoseCapabilityBitIsClearIsRefusedOnceFp15IsRead) {
  HeaterControllerFixture fx;
  IoDevice *dev = make_water_heater(fx);
  test::seed_water_heater(*dev, {.capabilities = DHW_CAPABILITY_AWAY});  // no boost
  EXPECT_FALSE(fx.controller.set_water_heater_boost("ABC123", true));
  EXPECT_FALSE(fx.controller.set_water_heater_boost("ABC123", false))
      << "off is refused too: the word is not supported";
  test::seed_water_heater(*dev, {.capabilities = DHW_CAPABILITY_BOOST});  // no away
  EXPECT_FALSE(fx.controller.set_water_heater_away("ABC123", true));
  EXPECT_TRUE(fx.radio.get_sent_data().empty()) << "nothing reaches the radio";
  EXPECT_TRUE(fx.executes.empty());
}

TEST(HeaterController, ARadiatorAndACoverAreNeverSentAFamilyZeroFrame) {
  for (const DeviceType type : {DeviceType::ELECTRICAL_HEATER, DeviceType::ROLLER_SHUTTER}) {
    HeaterControllerFixture fx;
    fx.add_device_of_kind(type, type == DeviceType::ELECTRICAL_HEATER ? 1 : 0);
    EXPECT_FALSE(fx.controller.read_dhw_reading("ABC123", DeviceRegister::DHW_MODE, 1));
    EXPECT_FALSE(fx.controller.read_heating_register("ABC123", DeviceRegister::DHW_MODE, nullptr, 1));
    EXPECT_FALSE(fx.controller.set_water_heater_mode("ABC123", DhwMode::AUTO));
    EXPECT_TRUE(fx.radio.get_sent_data().empty()) << static_cast<unsigned>(type);
  }
}

TEST(HeaterController, ARegisterIsWrittenOnlyToADeviceOfTheFamilyThatOwnsIt) {
  {
    HeaterControllerFixture fx;
    fx.add_device_of_kind(DeviceType::ELECTRICAL_HEATER, 1);
    EXPECT_FALSE(fx.controller.send_heating_command("ABC123", DeviceRegister::DHW_MODE, 1.0F))
        << "a water heater's register never goes to a radiator";
    EXPECT_TRUE(fx.radio.get_sent_data().empty());
  }
  {
    HeaterControllerFixture fx;
    make_water_heater(fx);
    EXPECT_FALSE(fx.controller.send_heating_command("ABC123", DeviceRegister::OPERATING_MODE, 1.0F))
        << "a radiator's register never goes to a water heater";
    EXPECT_FALSE(fx.controller.read_heating_register("ABC123", DeviceRegister::ROOM_TEMPERATURE, nullptr, 1));
    EXPECT_TRUE(fx.radio.get_sent_data().empty());
  }
}

TEST(HeaterController, AWaterHeaterModeTheRegisterDoesNotDefineIsNeverSent) {
  HeaterControllerFixture fx;
  make_water_heater(fx);
  EXPECT_FALSE(fx.controller.set_water_heater_mode("ABC123", static_cast<DhwMode>(3)));
  EXPECT_TRUE(fx.radio.get_sent_data().empty()) << "the row's rule refuses it before a frame is built";
}

TEST(HeaterController, AReadingIsFoldedIntoTheWaterHeaterRecord) {
  HeaterControllerFixture fx;
  const IoDevice *dev = make_water_heater(fx);
  fx.queue_register_ack({0x0C, 0x60, 0x00, test::dhw_register_byte(DeviceRegister::DHW_MODE), 0x01});  // manual, ECO on

  EXPECT_TRUE(fx.controller.read_dhw_reading("ABC123", DeviceRegister::DHW_MODE, 1));

  EXPECT_EQ(test::water_heater_values(*dev).mode, static_cast<int16_t>(DhwMode::MANUAL_ECO_ON));
}

TEST(HeaterController, ADhwStatusReadAsksForTheMainParameterAndFp15AndFp16) {
  HeaterControllerFixture fx;
  make_water_heater(fx);

  EXPECT_TRUE(fx.controller.read_dhw_status("ABC123", 2));
  ASSERT_EQ(fx.status_reads.size(), 1u);
  EXPECT_EQ(fx.status_reads[0].request.cmd, CMD_PRIVATE);
  EXPECT_EQ(fx.status_reads[0].request.data[1], DHW_STATUS_FPI1);
  EXPECT_EQ(fx.status_reads[0].request.data[2], DHW_STATUS_FPI2);
  EXPECT_EQ(fx.status_reads[0].max_tries, 2);

  fx.status_read_result = false;
  EXPECT_FALSE(fx.controller.read_dhw_status("ABC123", 2));
  EXPECT_FALSE(fx.controller.read_dhw_status("DEF456", 2)) << "an unknown device is never asked";
  EXPECT_EQ(fx.status_reads.size(), 2u);
}

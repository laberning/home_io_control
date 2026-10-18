/// @file corpus_battery_state_test.cpp
/// @brief The coarse battery decode applied to real status frames from the corpus.
///
/// Solar Somfy actuators (RS100 Solar, Oximo 40 Solar) report level 3 in byte 1 of every status
/// reply and status update captured, and mains ones (Sunea awning, Izymo dimmer) report 0. No
/// capture holds a lower level yet, so these tests pin the two values seen on real hardware.

#include "corpus_generated.h"
#include "proto_constants.h"
#include "proto_device_model.h"
#include "proto_frame.h"

#include <gtest/gtest.h>

using namespace esphome::home_io_control;

namespace {

/// Parse a status-bearing capture frame and decode the battery level from data[1].
BatteryState battery_of(const uint8_t *bytes, size_t len) {
  IoFrame frame;
  EXPECT_TRUE(parse(bytes, static_cast<uint8_t>(len), frame));
  EXPECT_TRUE(frame.cmd == CMD_PRIVATE_RESP || frame.cmd == CMD_STATUS_UPDATE);
  EXPECT_GE(frame.data_len, 2);
  return decode_battery_state(frame.data[1]);
}

template<size_t N> BatteryState battery_of(const uint8_t (&bytes)[N]) { return battery_of(bytes, N); }

}  // namespace

TEST(CorpusBatteryState, SolarRs100ReadsFullInStatusRepliesAndUpdates) {
  using namespace corpus;
  // Frames 2/5/7 are 0x71 status updates, 3/4/10/12/14 are 0x04 status-poll replies.
  EXPECT_EQ(battery_of(somfy_rs100_statuspoll_kig300_sx1276_frame2_bytes), BatteryState::FULL_CHARGE);
  EXPECT_EQ(battery_of(somfy_rs100_statuspoll_kig300_sx1276_frame3_bytes), BatteryState::FULL_CHARGE);
  EXPECT_EQ(battery_of(somfy_rs100_statuspoll_kig300_sx1276_frame4_bytes), BatteryState::FULL_CHARGE);
  EXPECT_EQ(battery_of(somfy_rs100_statuspoll_kig300_sx1276_frame5_bytes), BatteryState::FULL_CHARGE);
  EXPECT_EQ(battery_of(somfy_rs100_statuspoll_kig300_sx1276_frame7_bytes), BatteryState::FULL_CHARGE);
  EXPECT_EQ(battery_of(somfy_rs100_statuspoll_kig300_sx1276_frame10_bytes), BatteryState::FULL_CHARGE);
}

TEST(CorpusBatteryState, SolarOximoReadsFull) {
  using namespace corpus;
  EXPECT_EQ(battery_of(somfy_oximo40_statuspoll_sx1262_frame9_bytes), BatteryState::FULL_CHARGE);
  EXPECT_EQ(battery_of(somfy_oximo40_statuspoll_sx1262_frame14_bytes), BatteryState::FULL_CHARGE);
  EXPECT_EQ(battery_of(somfy_oximo40_statuspoll_sx1262_frame23_bytes), BatteryState::FULL_CHARGE);
}

TEST(CorpusBatteryState, MainsDevicesReportNoBattery) {
  using namespace corpus;
  EXPECT_EQ(battery_of(somfy_awning_statuspoll_success_sx1262_frame3_bytes), BatteryState::NOT_REPORTED);
  EXPECT_EQ(battery_of(somfy_izymo_dimmer_statuspoll_light_on_sx1262_frame1_bytes), BatteryState::NOT_REPORTED);
}

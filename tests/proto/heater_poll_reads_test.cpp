/// @file heater_poll_reads_test.cpp
/// @brief Tests for the registers a heater poll reads (pinned per kind) and for how a poll selects what it reads.

#include "proto_codecs.h"
#include "test_helpers.h"
#include "heating_profiles.h"
#include "payload_frame.h"
#include "reading_store.h"
#include "proto_commands.h"
#include "proto_device_types.h"
#include "proto_heating_profiles.h"

#include <gtest/gtest.h>

#include <array>
#include <algorithm>
#include <cmath>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <vector>

#include "pinned_poll.h"

using namespace esphome::home_io_control;
using test::PinnedPoll;
using test::pinned_poll;

// === The registers a heater poll reads (pinned) ===

TEST(HeaterPollReadsPin, TheRadiatorReadsTheDetectionSettingEveryPollAndTheMaximumOnce) {
  const PinnedPoll poll = pinned_poll(DeviceType::ELECTRICAL_HEATER, 1, DeviceModel::NONE);
  EXPECT_EQ(poll.every_poll,
            (std::vector<DeviceRegister>{DeviceRegister::ROOM_TEMPERATURE, DeviceRegister::COMFORT_SETPOINT,
                                         DeviceRegister::OPERATING_MODE, DeviceRegister::REGULATOR_STATE,
                                         DeviceRegister::WINDOW_OPEN}));
  EXPECT_EQ(poll.every_poll_may_miss, (std::vector<DeviceRegister>{DeviceRegister::OPEN_WINDOW_DETECTION}));
  EXPECT_EQ(poll.once_after_boot, (std::vector<DeviceRegister>{DeviceRegister::MAX_SETPOINT}));
}

TEST(HeaterPollReadsPin, TheHeaterWithALightReadsTheRadiatorsRegistersThenThoseOfTheLight) {
  const PinnedPoll poll = pinned_poll(DeviceType::ELECTRICAL_HEATER, 1, DeviceModel::HEATER_WITH_LIGHT);
  EXPECT_EQ(poll.every_poll,
            (std::vector<DeviceRegister>{DeviceRegister::ROOM_TEMPERATURE, DeviceRegister::COMFORT_SETPOINT,
                                         DeviceRegister::OPERATING_MODE, DeviceRegister::REGULATOR_STATE,
                                         DeviceRegister::WINDOW_OPEN}));
  EXPECT_EQ(poll.every_poll_may_miss,
            (std::vector<DeviceRegister>{DeviceRegister::OPEN_WINDOW_DETECTION, DeviceRegister::NIGHT_LIGHT,
                                         DeviceRegister::NIGHT_LIGHT_BRIGHTNESS}));
  EXPECT_EQ(poll.once_after_boot, (std::vector<DeviceRegister>{DeviceRegister::MAX_SETPOINT}));
}

TEST(HeaterPollReadsPin, TheTowelDryerReadsItsOwnRegistersAndTheMaximumOnce) {
  const PinnedPoll poll = pinned_poll(DeviceType::ELECTRICAL_HEATER, 1, DeviceModel::TOWEL_DRYER);
  EXPECT_EQ(poll.every_poll,
            (std::vector<DeviceRegister>{DeviceRegister::ROOM_TEMPERATURE, DeviceRegister::COMFORT_SETPOINT,
                                         DeviceRegister::TOWEL_MODE, DeviceRegister::TEMPORARY_STATE,
                                         DeviceRegister::WORKING_RATE}));
  EXPECT_TRUE(poll.every_poll_may_miss.empty());
  EXPECT_EQ(poll.once_after_boot, (std::vector<DeviceRegister>{DeviceRegister::MAX_SETPOINT}));
}

TEST(HeaterPollReadsPin, AHeaterThatTakesTheStatusReadReadsNoRegisterOfItsOwn) {
  for (const auto &[type, subtype, model] :
       {std::tuple{DeviceType::ELECTRICAL_HEATER, uint8_t{0}, DeviceModel::NONE},
        std::tuple{DeviceType::ELECTRICAL_HEATER, uint8_t{0}, DeviceModel::PILOT_WIRE_INTERFACE},
        std::tuple{DeviceType::HEATING_TEMPERATURE_INTERFACE, uint8_t{0}, DeviceModel::NONE},
        std::tuple{DeviceType::HEAT_PUMP, uint8_t{0}, DeviceModel::NONE}}) {
    const PinnedPoll poll = pinned_poll(type, subtype, model);
    EXPECT_TRUE(poll.every_poll.empty());
    EXPECT_TRUE(poll.every_poll_may_miss.empty());
    EXPECT_TRUE(poll.once_after_boot.empty());
  }
}

// === Selecting what a poll reads ===

namespace {

/// A radiator with a light: the kind with the most optional parts.
struct SelectingRig {
  const HeatingProfile &profile{
      *find_heating_profile(DeviceType::ELECTRICAL_HEATER, 1, DeviceModel::HEATER_WITH_LIGHT)};
  IoDevice dev;
  SelectingRig() { test::set_kind(dev, DeviceType::ELECTRICAL_HEATER, 1, DeviceModel::HEATER_WITH_LIGHT); }
  std::vector<DeviceRegister> reads(ReadingPoll policy) const { return test::polled_registers(dev, policy); }
};

}  // namespace

TEST(HeaterPollReads, AskedRegistersAreSelectedWhenAskedAndNotAfterAnEmptyAnswer) {
  SelectingRig rig;
  EXPECT_TRUE(rig.reads(ReadingPoll::WHEN_ASKED).empty()) << "nothing asked";
  ask_register_reading(rig.dev, DeviceRegister::OCCUPANCY_DETECTION);
  ask_register_reading(rig.dev, DeviceRegister::PRESENCE);
  EXPECT_EQ(rig.reads(ReadingPoll::WHEN_ASKED),
            (std::vector<DeviceRegister>{DeviceRegister::PRESENCE, DeviceRegister::OCCUPANCY_DETECTION}))
      << "in the order of the table, not of the asking";
  mark_register_reading_absent(rig.dev, DeviceRegister::PRESENCE);
  EXPECT_EQ(rig.reads(ReadingPoll::WHEN_ASKED), (std::vector<DeviceRegister>{DeviceRegister::OCCUPANCY_DETECTION}));
}

TEST(HeaterPollReads, AnAskedRegisterTheKindHasNoCapabilityForIsNotSelected) {
  SelectingRig rig;
  ask_register_reading(rig.dev, DeviceRegister::CENTRAL_HEATING_MODE);
  EXPECT_TRUE(rig.reads(ReadingPoll::WHEN_ASKED).empty()) << "a radiator has no central-heating mode";
}

TEST(HeaterPollReads, TheMaximumIsSelectedUntilItHoldsAValue) {
  SelectingRig rig;
  EXPECT_EQ(rig.reads(ReadingPoll::ONCE_AFTER_BOOT), (std::vector<DeviceRegister>{DeviceRegister::MAX_SETPOINT}));
  test::store_reading(rig.dev, DeviceRegister::MAX_SETPOINT, 28.0F);
  EXPECT_TRUE(rig.reads(ReadingPoll::ONCE_AFTER_BOOT).empty());
}

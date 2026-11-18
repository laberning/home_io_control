/// @file platform_device_sensor_test.cpp
/// @brief The `sensor:` entry that declares a device with no other main entity: it registers the device and asks
/// the hub to read each reading of its keys, at the entry's update_interval or else at the reading's own default.

#include "platform_device_sensor.h"

#include "hub_port_doubles.h"

#include <gtest/gtest.h>

using namespace esphome::home_io_control;

namespace {

constexpr const char *DEVICE_ID = "ABC123";
constexpr uint32_t HOUR_MS = 3600000;
constexpr uint32_t DAY_MS = 86400000;

/// A sensor entry bound to a double of the hub's device port.
class PlatformDeviceSensor : public ::testing::Test {
 protected:
  void SetUp() override {
    entry.set_parent(&hub);
    entry.set_device_id(DEVICE_ID);
    entry.set_device_type(DeviceType::HEAT_PUMP);
    entry.set_subtype(1);
  }

  test::DevicePortDouble hub;
  IOHomeDeviceSensor entry;
};

}  // namespace

TEST_F(PlatformDeviceSensor, RegistersTheDeviceItDeclares) {
  entry.setup();

  const IoDevice *dev = hub.get_device(DEVICE_ID);
  ASSERT_NE(dev, nullptr);
  EXPECT_EQ(dev->type, DeviceType::HEAT_PUMP);
  EXPECT_EQ(dev->subtype, 1);
}

TEST_F(PlatformDeviceSensor, EachReadingIsReadAtItsOwnDefaultWhenTheEntrySetsNoInterval) {
  entry.add_data_object_reading(DeviceReading::ENERGY_TOTAL, HOUR_MS);
  entry.add_data_object_reading(DeviceReading::OCCUPANCY, DAY_MS);

  entry.setup();

  EXPECT_EQ(hub.reading_poll_interval(DEVICE_ID, DeviceReading::ENERGY_TOTAL), HOUR_MS);
  EXPECT_EQ(hub.reading_poll_interval(DEVICE_ID, DeviceReading::OCCUPANCY), DAY_MS);
}

TEST_F(PlatformDeviceSensor, TheEntrysUpdateIntervalAppliesToEveryReading) {
  entry.add_data_object_reading(DeviceReading::ENERGY_TOTAL, HOUR_MS);
  entry.add_data_object_reading(DeviceReading::OCCUPANCY, DAY_MS);
  entry.set_update_interval(2 * HOUR_MS);

  entry.setup();

  EXPECT_EQ(hub.reading_poll_interval(DEVICE_ID, DeviceReading::ENERGY_TOTAL), 2 * HOUR_MS);
  EXPECT_EQ(hub.reading_poll_interval(DEVICE_ID, DeviceReading::OCCUPANCY), 2 * HOUR_MS);
}

TEST_F(PlatformDeviceSensor, AnEntryWithoutReadingKeysSchedulesNothing) {
  entry.set_update_interval(HOUR_MS);

  entry.setup();

  EXPECT_NE(hub.get_device(DEVICE_ID), nullptr);
  EXPECT_EQ(hub.reading_poll_interval(DEVICE_ID, DeviceReading::ENERGY_TOTAL), 0u);
}

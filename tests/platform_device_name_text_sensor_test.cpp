/// @file platform_device_name_text_sensor_test.cpp
/// @brief Tests for the generated device-name diagnostic text sensor.

#include "platform_device_name_text_sensor.h"

#include "hub_core.h"
#include "hub_internal.h"
#include "test_helpers.h"

#include <cstring>

using namespace esphome::home_io_control;

namespace {

class TestableDeviceNameHub : public IOHomeControlComponent {
 public:
  using IOHomeControlComponent::callbacks_;
  using IOHomeControlComponent::notify_device_update_;

  void queue_request_device_name(const std::string &device_id) override { this->last_requested_device_id_ = device_id; }

  const std::string &last_requested_device_id() const { return this->last_requested_device_id_; }

 private:
  std::string last_requested_device_id_;
};

}  // namespace

TEST(PlatformDeviceNameTextSensor, SetupPublishesCachedNameAndSchedulesFetch) {
  TestableDeviceNameHub hub;
  hub.add_device("ABC123");
  auto *device = hub.get_device("ABC123");
  ASSERT_NE(device, nullptr);
  std::strcpy(device->name, "Patio Awning");

  IOHomeDeviceNameTextSensor sensor;
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();

  EXPECT_EQ(sensor.state, "Patio Awning");
  EXPECT_EQ(sensor.last_timeout_name_, "init_name");
  EXPECT_EQ(sensor.last_timeout_ms_, esphome::home_io_control::detail::INITIAL_STATUS_REQUEST_DELAY_MS);
}

TEST(PlatformDeviceNameTextSensor, InitialFetchTimeoutQueuesNameRequest) {
  TestableDeviceNameHub hub;
  hub.add_device("ABC123");

  IOHomeDeviceNameTextSensor sensor;
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();

  ASSERT_TRUE(static_cast<bool>(sensor.last_timeout_callback_));
  sensor.last_timeout_callback_();
  EXPECT_EQ(hub.last_requested_device_id(), "ABC123");
}

TEST(PlatformDeviceNameTextSensor, DeviceUpdatePublishesLatestName) {
  TestableDeviceNameHub hub;
  hub.add_device("ABC123");

  IOHomeDeviceNameTextSensor sensor;
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();

  auto *device = hub.get_device("ABC123");
  ASSERT_NE(device, nullptr);
  std::strcpy(device->name, "Dexxo");
  hub.notify_device_update_("ABC123");

  EXPECT_EQ(sensor.state, "Dexxo");
}
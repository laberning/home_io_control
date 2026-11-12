/// @file platform_active_issue_text_sensor_test.cpp
/// @brief Tests for the generated active-issue diagnostic text sensor.

#include "platform_companion_sensors.h"

#include "hub_core.h"
#include "proto_constants.h"
#include "test_helpers.h"
#include "stubs/radio_test_common.h"

using namespace esphome::home_io_control;

namespace {

class TestableActiveIssueHub : public IOHomeControlComponent {
 public:
  using IOHomeControlComponent::notify_device_update_;
  using IOHomeControlComponent::radio_;
  using IOHomeControlComponent::initialized_;
};

}  // namespace

TEST(PlatformActiveIssueTextSensor, SetupWithNoRecordedResultPublishesEmptyString) {
  TestableActiveIssueHub hub;
  hub.add_device("ABC123");

  IOHomeActiveIssueTextSensor sensor;
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();

  EXPECT_EQ(sensor.state, "") << "a device with no recorded CMD_ERROR_RESP should publish an empty string";
}

TEST(PlatformActiveIssueTextSensor, SetupWithRecordedResultPublishesSymbolicName) {
  TestableActiveIssueHub hub;
  hub.add_device("ABC123");
  auto *device = hub.get_device("ABC123");
  ASSERT_NE(device, nullptr);
  device->last_result_code = RESULT_LIMITATION_BY_RAIN;

  IOHomeActiveIssueTextSensor sensor;
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();

  EXPECT_EQ(sensor.state, "LIMITATION_BY_RAIN")
      << "a previously recorded result should publish its symbolic name on setup";
}

TEST(PlatformActiveIssueTextSensor, DeviceUpdatePublishesNewResultAndClears) {
  TestableActiveIssueHub hub;
  hub.add_device("ABC123");
  auto *device = hub.get_device("ABC123");
  ASSERT_NE(device, nullptr);

  IOHomeActiveIssueTextSensor sensor;
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();
  ASSERT_EQ(sensor.state, "");

  device->last_result_code = RESULT_LIMITATION_BY_WIND;
  hub.notify_device_update_("ABC123");

  EXPECT_EQ(sensor.state, "LIMITATION_BY_WIND") << "a device update should publish the newly recorded result";

  device->last_result_code = 0;
  hub.notify_device_update_("ABC123");

  EXPECT_EQ(sensor.state, "") << "a device update after clearing should publish an empty string again";
}

TEST(PlatformActiveIssueTextSensor, SessionInterruptedIsNotShownAsAnIssue) {
  // A garage drive answered status reads with SESSION_INTERRUPTED right after an unconfirmed
  // command (issue #163); the code describes that exchange, not the device.
  TestableActiveIssueHub hub;
  hub.add_device("ABC123");
  auto *device = hub.get_device("ABC123");
  ASSERT_NE(device, nullptr);

  IOHomeActiveIssueTextSensor sensor;
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();

  device->last_result_code = RESULT_SESSION_INTERRUPTED;
  hub.notify_device_update_("ABC123");
  EXPECT_EQ(sensor.state, "");

  device->limited_by_rain = true;
  hub.notify_device_update_("ABC123");
  EXPECT_EQ(sensor.state, "LIMITATION_BY_RAIN") << "a derived limitation still shows behind a transient result";
}

TEST(PlatformActiveIssueTextSensor, DerivedRainLimitationPublishesLimitationByRain) {
  TestableActiveIssueHub hub;
  hub.add_device("ABC123");
  auto *device = hub.get_device("ABC123");
  ASSERT_NE(device, nullptr);

  IOHomeActiveIssueTextSensor sensor;
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();
  ASSERT_EQ(sensor.state, "");

  device->limited_by_rain = true;
  hub.notify_device_update_("ABC123");
  EXPECT_EQ(sensor.state, "LIMITATION_BY_RAIN");

  device->limited_by_rain = false;
  hub.notify_device_update_("ABC123");
  EXPECT_EQ(sensor.state, "") << "the derived source clears with the flag";
}

TEST(PlatformActiveIssueTextSensor, ExplicitResultWinsOverDerivedRainLimitation) {
  TestableActiveIssueHub hub;
  hub.add_device("ABC123");
  auto *device = hub.get_device("ABC123");
  ASSERT_NE(device, nullptr);
  device->limited_by_rain = true;
  device->last_result_code = RESULT_LIMITATION_BY_WIND;

  IOHomeActiveIssueTextSensor sensor;
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();

  EXPECT_EQ(sensor.state, "LIMITATION_BY_WIND");
}

TEST(PlatformActiveIssueTextSensor, DeviceUpdateForOtherDeviceIsIgnored) {
  TestableActiveIssueHub hub;
  hub.add_device("ABC123");
  hub.add_device("DEF456");
  auto *other = hub.get_device("DEF456");
  ASSERT_NE(other, nullptr);
  other->last_result_code = RESULT_LIMITATION_BY_RAIN;

  IOHomeActiveIssueTextSensor sensor;
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();
  ASSERT_EQ(sensor.state, "");

  hub.notify_device_update_("DEF456");

  EXPECT_EQ(sensor.state, "") << "updates for a different device id must not change this sensor's published state";
}

TEST(PlatformActiveIssueTextSensor, RadioThatStopsRespondingShowsOnEveryDeviceAndWinsOverAResult) {
  TestableActiveIssueHub hub;
  hub.add_device("ABC123");
  hub.add_device("DEF456");
  hub.get_device("DEF456")->last_result_code = RESULT_LIMITATION_BY_RAIN;
  auto *radio = new LatchingRadio();
  hub.radio_ = radio;
  hub.initialized_ = true;

  IOHomeActiveIssueTextSensor first;
  first.set_parent(&hub);
  first.set_device_id("ABC123");
  first.setup();
  IOHomeActiveIssueTextSensor second;
  second.set_parent(&hub);
  second.set_device_id("DEF456");
  second.setup();
  ASSERT_EQ(first.state, "");
  ASSERT_EQ(second.state, "LIMITATION_BY_RAIN");

  radio->latch("BUSY stuck high");
  hub.loop();

  EXPECT_EQ(first.state, ACTIVE_ISSUE_RADIO_STOPPED_RESPONDING) << "loop() must republish every device";
  EXPECT_EQ(second.state, ACTIVE_ISSUE_RADIO_STOPPED_RESPONDING) << "a dead radio outranks a device result";
  delete hub.radio_;
  hub.radio_ = nullptr;
}

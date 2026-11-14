/// @file platform_oneway_button_test.cpp
/// @brief Tests for the 1W command button, the enrollment button and the last-command text sensor, run
/// against a double of the hub's control port.

#include "platform_oneway_entities.h"

#include "hub_port_doubles.h"

#include <gtest/gtest.h>

using namespace esphome::home_io_control;

TEST(PlatformOneWayCommandButton, PressSendsItsActionAsItsControllerIdentity) {
  test::ControlPortDouble hub;
  IOHomeOneWayCommandButton button;
  button.set_parent(&hub);
  button.set_controller_id("awning_remote");
  button.set_action(OneWayButtonAction::VENT);

  button.press();

  ASSERT_EQ(hub.oneway_actions.size(), 1u);
  EXPECT_EQ(hub.oneway_actions[0].first, "awning_remote");
  EXPECT_EQ(hub.oneway_actions[0].second, OneWayButtonAction::VENT);
  EXPECT_TRUE(hub.oneway_enrollments.empty()) << "a command press never enrolls";
}

TEST(PlatformOneWayCommandButton, EveryPressIsSentAgain) {
  test::ControlPortDouble hub;
  IOHomeOneWayCommandButton button;
  button.set_parent(&hub);
  button.set_controller_id("awning_remote");
  button.set_action(OneWayButtonAction::STOP);

  button.press();
  button.press();

  EXPECT_EQ(hub.oneway_actions.size(), 2u) << "a button has no state to coalesce on";
}

TEST(PlatformOneWayEnrollButton, PressEnrollsItsControllerIdentity) {
  test::ControlPortDouble hub;
  IOHomeOneWayEnrollButton button;
  button.set_parent(&hub);
  button.set_controller_id("awning_remote");

  button.press();

  ASSERT_EQ(hub.oneway_enrollments.size(), 1u);
  EXPECT_EQ(hub.oneway_enrollments[0], "awning_remote");
  EXPECT_TRUE(hub.oneway_actions.empty()) << "an enrollment press sends no command";
}

TEST(PlatformOneWayLastCommandTextSensor, ShowsOnlyReportsOfItsOwnIdentity) {
  test::ControlPortDouble hub;
  IOHomeOneWayLastCommandTextSensor sensor;
  sensor.set_parent(&hub);
  sensor.set_controller_id("awning_remote");
  sensor.setup();

  OneWayCommandReport other{};
  other.controller_id = "window_remote";
  other.intent = "OPEN";
  other.sequence = 7;
  other.sequence_reserved = true;
  other.transmitted = true;
  hub.report_oneway_command(other);
  EXPECT_EQ(sensor.state, "") << "another identity's report is not this sensor's";

  OneWayCommandReport own = other;
  own.controller_id = "awning_remote";
  own.intent = "STOP";
  own.sequence = 1234;
  hub.report_oneway_command(own);
  EXPECT_EQ(sensor.state, format_oneway_command_report(own));
}

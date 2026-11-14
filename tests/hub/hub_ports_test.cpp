/// @file hub_ports_test.cpp
/// @brief The hub provides the three entity ports (hub_ports.h) and the entities take them from it.

#include "platform_cover.h"

#include "test_helpers.h"

#include <type_traits>

using namespace esphome::home_io_control;
using test::TestableHubComponent;

// The entities' set_parent() takes a port only when the hub converts to it (detail::port_of()); a hub that
// stopped implementing one publicly would hand its entities a null port, so the build checks all three.
static_assert(std::is_convertible_v<IOHomeControlComponent *, HubDevicePort *>);
static_assert(std::is_convertible_v<IOHomeControlComponent *, HubHeatingPort *>);
static_assert(std::is_convertible_v<IOHomeControlComponent *, HubControlPort *>);

TEST(HubPorts, DevicePortReachesTheHubsRegistry) {
  TestableHubComponent hub;
  HubDevicePort &port = hub;

  port.add_device("ABC123");

  EXPECT_EQ(port.get_device("ABC123"), hub.get_device("ABC123"));
  ASSERT_NE(port.get_device("ABC123"), nullptr);
}

TEST(HubPorts, ControlPortGivesThePairingResultAsTheSensorString) {
  TestableHubComponent hub;
  HubControlPort &port = hub;

  // The duration field reads the clock, which the host stub advances on every call, so compare the stable lead.
  EXPECT_EQ(port.pairing_result_text().rfind("v1; outcome=none; phase=idle;", 0), 0u);
}

TEST(HubPorts, EntityWiredToTheHubRegistersItsDeviceThroughTheDevicePort) {
  TestableHubComponent hub;
  IOHomeCover cover;
  cover.set_parent(&hub);
  cover.set_device_id("ABC123");

  cover.setup();

  EXPECT_NE(hub.get_device("ABC123"), nullptr) << "set_parent(hub) must hand the cover the hub's device port";
}

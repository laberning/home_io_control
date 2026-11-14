#include "platform_hub_controls.h"

#include "hub_port_doubles.h"

#include <gtest/gtest.h>

using namespace esphome::home_io_control;

// ============================================================================
// PlatformScanPairedDevicesButton test suite
// ============================================================================
// Hub-level button (no io_device_id, created dynamically from home_io_control.
// scan_paired_devices_button: true): a press asks the hub to scan through the control port. What the hub
// does with the request (the dual-pass roll-call, the busy guard) is covered in hub_ports_test.cpp.

TEST(PlatformScanPairedDevicesButton, PressAsksTheHubToScan) {
  test::ControlPortDouble hub;

  IOHomeScanPairedDevicesButton button;
  button.set_parent(&hub);
  button.press();

  EXPECT_EQ(hub.scan_presses, 1);
}

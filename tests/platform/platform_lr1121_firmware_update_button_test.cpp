#include "platform_lr1121_controls.h"

#include "hub_port_doubles.h"

#include <gtest/gtest.h>

using namespace esphome::home_io_control;

// ============================================================================
// PlatformLr1121FirmwareUpdateButton test suite
// ============================================================================
// Hub-level button (no io_device_id, created dynamically like the accept_foreign_pairing
// switch — see platform_accept_foreign_pairing_switch_test.cpp for the mirrored pattern): a press asks the
// hub to run the firmware update through the control port. The guard/verdict/flash logic is covered by
// hub_lr1121_firmware_update_test.cpp and what a press does on the real hub by hub_ports_test.cpp.

TEST(PlatformLr1121FirmwareUpdateButton, PressAsksTheHubToUpdate) {
  test::ControlPortDouble hub;

  IOHomeLr1121FirmwareUpdateButton button;
  button.set_parent(&hub);
  button.press();

  EXPECT_EQ(hub.lr1121_updates, 1);
}

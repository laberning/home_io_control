#include "platform_lr1121_firmware_update_button.h"
#include "hub_core.h"
#include "lr1121_firmware_decisions.h"
#include "radio_lr1121_firmware_updater.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"
#include "stubs/scripted_spi.h"

#include <gtest/gtest.h>

using namespace esphome::home_io_control;
using test::TestableHubComponent;

// ============================================================================
// PlatformLr1121FirmwareUpdateButton test suite
// ============================================================================
// Hub-level button (no io_device_id, created dynamically like the accept_foreign_pairing
// switch — see platform_accept_foreign_pairing_switch_test.cpp for the mirrored pattern): a
// press must dispatch to IOHomeControlComponent::trigger_lr1121_firmware_update(). The guard/
// verdict/flash logic itself is covered by hub_lr1121_firmware_update_test.cpp; this only checks
// that pressing the button actually reaches it.

TEST(PlatformLr1121FirmwareUpdateButton, PressDispatchesToHubTrigger) {
  ScriptedSpi spi;
  MockPin rst, busy;
  Lr1121FirmwareUpdater updater(&spi, &rst, &busy);

  TestableHubComponent hub;
  hub.lr1121_firmware_updater_ = &updater;
  hub.lr1121_flash_verdict_known_ = true;
  hub.lr1121_flash_verdict_ = FlashDecision::NEEDS_CONFIRMATION;

  IOHomeLr1121FirmwareUpdateButton button;
  button.set_parent(&hub);
  button.press();

  EXPECT_TRUE(hub.lr1121_flash_confirmation_armed_)
      << "pressing the button should have reached trigger_lr1121_firmware_update() and armed the "
         "confirmation window";
  EXPECT_TRUE(spi.transactions().empty()) << "a first (unconfirmed) press must not touch the chip";
}

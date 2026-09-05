#include "platform_hub_controls.h"
#include "hub_core.h"
#include "proto_commands.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"

#include <cstring>

#include <gtest/gtest.h>

using namespace esphome::home_io_control;
using test::TestableHubComponent;

// ============================================================================
// PlatformScanPairedDevicesButton test suite
// ============================================================================
// Hub-level button (no io_device_id, created dynamically from home_io_control.
// scan_paired_devices_button: true — see platform_lr1121_firmware_update_button_test.cpp for the
// mirrored pattern for a different hub-level button): a press must dispatch all the way to the
// roll-call, exactly as the native `scan_paired_devices` API action does, and must be inert while
// a radio exchange is already in flight.

namespace {

void setup_component(TestableHubComponent &component, MockRadio &radio) {
  component.node_id_[0] = 0xC0;
  component.node_id_[1] = 0xFF;
  component.node_id_[2] = 0xEE;
  static const uint8_t key[] = {0xD1, 0x74, 0x34, 0x93, 0xFA, 0x94, 0x38, 0x45,
                                0xAC, 0x43, 0x50, 0xEE, 0xFF, 0x34, 0x29, 0x34};
  std::memcpy(component.system_key_, key, AES_KEY_SIZE);
  component.initialized_ = true;
  component.radio_ = &radio;
  // Small window so the roll-call doesn't spend thousands of no-op host-test iterations waiting
  // out the default 2000 ms discovery window: in host tests millis() advances by exactly 1 per
  // call. Same rationale as hub_management_test.cpp's setup_component().
  component.tuning_.pairing_discovery_wait_ms = 20;
}

}  // namespace

TEST(PlatformScanPairedDevicesButton, PressRunsTheRollCallOnAllThreeChannels) {
  TestableHubComponent hub;
  MockRadio radio;
  setup_component(hub, radio);

  IOHomeScanPairedDevicesButton button;
  button.set_parent(&hub);
  button.press();

  // Proves the press reached api_scan_paired_devices() -> scan_paired_devices(), which retries
  // the roll-call on all three channels — mirrors
  // HubManagement.ScanPairedDevicesRetriesOnAllThreeChannels in hub_management_test.cpp.
  ASSERT_EQ(radio.get_tx_configs().size(), 3u) << "one roll-call attempt per channel";
  EXPECT_EQ(radio.get_tx_configs()[0].freq_hz, FREQ_CH2);
  EXPECT_EQ(radio.get_tx_configs()[1].freq_hz, FREQ_CH1);
  EXPECT_EQ(radio.get_tx_configs()[2].freq_hz, FREQ_CH3);
}

TEST(PlatformScanPairedDevicesButton, PressWhileBusyIsIgnoredButStillPublishesAFailedResult) {
  esphome::api::APIServer api_server;
  esphome::api::ScopedGlobalApiServer scoped_api_server(api_server);
  api_server.reset();

  TestableHubComponent hub;
  MockRadio radio;
  setup_component(hub, radio);
  hub.busy_ = true;

  IOHomeScanPairedDevicesButton button;
  button.set_parent(&hub);
  button.press();

  EXPECT_TRUE(radio.get_tx_configs().empty())
      << "a press while a radio exchange is already in flight must not re-enter it";

  // A busy press must not go silent: it's the one press outcome the native API action can never
  // produce (only an automation-triggered press can land here), so it still fires the same
  // log/event pair every other rejected management action does — see
  // trigger_scan_paired_devices()'s doc comment.
  ASSERT_EQ(api_server.events_.size(), 1u);
  const auto &event = api_server.events_.front();
  EXPECT_EQ(event.event_type, "esphome.home_io_control_action_result");
  EXPECT_EQ(event.data.at("action"), "scan_paired_devices");
  EXPECT_EQ(event.data.at("success"), "false");
}

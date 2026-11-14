/// @file hub_ports_test.cpp
/// @brief The hub provides the three entity ports (hub_ports.h) and the entities take them from it.

#include "platform_companion_sensors.h"
#include "platform_cover.h"
#include "platform_hub_controls.h"
#include "platform_lr1121_controls.h"

#include "lr1121_firmware_decisions.h"
#include "pairing_responder.h"
#include "proto_commands.h"
#include "radio_lr1121_firmware_updater.h"
#include "test_helpers.h"
#include "stubs/radio_test_common.h"
#include "stubs/scripted_spi.h"

#include <cstring>
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

// === Entities that act through what only the real hub does ===
// The platform tests drive these entities against port doubles. What the hub does behind the port (loop()
// republishing, the key-extraction responder, the roll-call, the firmware update guard) runs here on the
// real hub with the entity wired to it.

TEST(HubPorts, RadioThatStopsRespondingShowsOnEveryDeviceAndWinsOverAResult) {
  TestableHubComponent hub;
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

TEST(HubPorts, ThisHubQualifierUsesTheHubsConfiguredNodeId) {
  TestableHubComponent hub;
  hub.node_id_[0] = 0xC0;
  hub.node_id_[1] = 0xFF;
  hub.node_id_[2] = 0xEE;
  hub.add_device("ABC123");
  auto *device = hub.get_device("ABC123");
  ASSERT_NE(device, nullptr);
  device->last_commander[0] = 0xC0;
  device->last_commander[1] = 0xFF;
  device->last_commander[2] = 0xEE;
  device->last_command_originator = 0x01;
  device->has_last_command = true;

  IOHomeLastCommandedByTextSensor by_sensor;
  by_sensor.set_parent(&hub);
  by_sensor.set_device_id("ABC123");
  by_sensor.setup();

  EXPECT_EQ(by_sensor.state, "C0FFEE (this hub)")
      << "the qualifier must resolve against this hub's own configured node_id_, not a hardcoded value";
}

namespace {

/// Exposes protected write_state(bool) for direct invocation in tests.
class TestableAcceptForeignPairingSwitch : public IOHomeAcceptForeignPairingSwitch {
 public:
  using IOHomeAcceptForeignPairingSwitch::write_state;
};

/// Exposes protected write_state(bool) for direct invocation in tests.
class TestableRecoverOneWayKeySwitch : public IOHomeRecoverOneWayKeySwitch {
 public:
  using IOHomeRecoverOneWayKeySwitch::write_state;
};

void setup_scan_component(TestableHubComponent &component, MockRadio &radio) {
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

TEST(HubPorts, AcceptForeignPairingSwitchArmsAndDisarmsTheKeyExtractionResponder) {
  TestableHubComponent hub;
  TestableAcceptForeignPairingSwitch sw;
  sw.set_parent(&hub);

  sw.write_state(true);
  EXPECT_EQ(hub.key_extraction_.key_extraction_ctx_.state, pairing_responder::ResponderState::ARMED_IDLE)
      << "write_state(true) should arm the hub's key-extraction responder";
  EXPECT_TRUE(sw.get_state()) << "switch should publish on immediately";

  sw.write_state(false);
  EXPECT_EQ(hub.key_extraction_.key_extraction_ctx_.state, pairing_responder::ResponderState::DISARMED)
      << "write_state(false) should disarm the hub's key-extraction responder";
  EXPECT_FALSE(sw.get_state());
}

TEST(HubPorts, AcceptForeignPairingSwitchFollowsTheHubsAutoDisarm) {
  TestableHubComponent hub;
  TestableAcceptForeignPairingSwitch sw;
  sw.set_parent(&hub);
  sw.setup();  // registers the armed-state callback

  sw.write_state(true);
  ASSERT_TRUE(sw.get_state());
  ASSERT_TRUE(static_cast<bool>(hub.last_timeout_callback_)) << "arming should schedule the auto-off timeout";

  // Simulate the auto-off timeout firing (extraction window expired) — the hub disarms itself,
  // and the switch should follow via the armed-state callback, not a write_state() call.
  hub.last_timeout_callback_();

  EXPECT_EQ(hub.key_extraction_.key_extraction_ctx_.state, pairing_responder::ResponderState::DISARMED);
  EXPECT_FALSE(sw.get_state()) << "switch should publish off when the hub auto-disarms";
}

TEST(HubPorts, RecoverOneWayKeySwitchArmsTheOneWayAdoptionListenerOnly) {
  TestableHubComponent hub;
  TestableRecoverOneWayKeySwitch sw;
  sw.set_parent(&hub);

  sw.write_state(true);
  EXPECT_TRUE(hub.oneway_key_adoption_armed()) << "write_state(true) should arm the 1W adoption listener";
  EXPECT_EQ(hub.key_extraction_.key_extraction_ctx_.state, pairing_responder::ResponderState::DISARMED)
      << "it must NOT arm the 2W key-extraction responder";
  EXPECT_TRUE(sw.get_state()) << "switch should publish on immediately";

  sw.write_state(false);
  EXPECT_FALSE(hub.oneway_key_adoption_armed()) << "write_state(false) should disarm the 1W adoption listener";
  EXPECT_FALSE(sw.get_state());
}

TEST(HubPorts, RecoverOneWayKeySwitchFollowsTheHubsAutoDisarm) {
  TestableHubComponent hub;
  TestableRecoverOneWayKeySwitch sw;
  sw.set_parent(&hub);
  sw.setup();  // registers the armed-state callback through HubArmingSwitch::setup()

  sw.write_state(true);
  ASSERT_TRUE(sw.get_state());
  ASSERT_TRUE(static_cast<bool>(hub.last_timeout_callback_)) << "arming should schedule the auto-off timeout";

  hub.last_timeout_callback_();  // auto-off window expires; the hub disarms itself

  EXPECT_FALSE(hub.oneway_key_adoption_armed());
  EXPECT_FALSE(sw.get_state()) << "switch should publish off when the hub auto-disarms";
}

TEST(HubPorts, ScanPairedDevicesButtonRunsTheDualPassRollCall) {
  TestableHubComponent hub;
  MockRadio radio;
  setup_scan_component(hub, radio);

  IOHomeScanPairedDevicesButton button;
  button.set_parent(&hub);
  button.press();

  // Proves the press reached api_scan_paired_devices() -> scan_paired_devices(), which sweeps
  // both power-class passes on all three channels — mirrors
  // HubManagement.ScanPairedDevicesSweepsBothPowerClasses in hub_management_test.cpp.
  ASSERT_EQ(radio.get_tx_configs().size(), 6u) << "one roll-call attempt per channel, per pass";
  EXPECT_EQ(radio.get_tx_configs()[0].freq_hz, FREQ_CH2);
  EXPECT_EQ(radio.get_tx_configs()[1].freq_hz, FREQ_CH1);
  EXPECT_EQ(radio.get_tx_configs()[2].freq_hz, FREQ_CH3);
  EXPECT_EQ(radio.get_tx_configs()[3].freq_hz, FREQ_CH2);
  EXPECT_EQ(radio.get_tx_configs()[4].freq_hz, FREQ_CH1);
  EXPECT_EQ(radio.get_tx_configs()[5].freq_hz, FREQ_CH3);
}

TEST(HubPorts, ScanPairedDevicesButtonPressWhileBusyIsIgnoredButStillPublishesAFailedResult) {
  esphome::api::APIServer api_server;
  esphome::api::ScopedGlobalApiServer scoped_api_server(api_server);
  api_server.reset();

  TestableHubComponent hub;
  MockRadio radio;
  setup_scan_component(hub, radio);
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

TEST(HubPorts, Lr1121FirmwareUpdateButtonPressReachesTheHubsUpdateTrigger) {
  ScriptedSpi spi;
  MockPin rst, busy;
  Lr1121FirmwareUpdater updater(&spi, &rst, &busy);

  TestableHubComponent hub;
  hub.lr1121_firmware_update_.lr1121_firmware_updater_ = &updater;
  hub.lr1121_firmware_update_.lr1121_flash_verdict_known_ = true;
  hub.lr1121_firmware_update_.lr1121_flash_verdict_ = FlashDecision::NEEDS_CONFIRMATION;

  IOHomeLr1121FirmwareUpdateButton button;
  button.set_parent(&hub);
  button.press();

  EXPECT_TRUE(hub.lr1121_firmware_update_.lr1121_flash_confirmation_armed_)
      << "pressing the button should have reached trigger_lr1121_firmware_update() and armed the "
         "confirmation window";
  EXPECT_TRUE(spi.transactions().empty()) << "a first (unconfirmed) press must not touch the chip";
}

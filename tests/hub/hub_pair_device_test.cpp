/// @file hub_pair_device_test.cpp
/// @brief The `pair_device` management action: its result messages, and that a refusal leaves the
/// "Last Pairing Result" telemetry and its end stamp alone while an attempt publishes both.

#include "hub_core.h"
#include "proto_commands.h"
#include "proto_constants.h"
#include "proto_frame.h"

#include "management_test_fixture.h"
#include "pairing_test_support.h"
#include "test_helpers.h"
#include "stubs/radio_test_common.h"

#include <cstring>
#include <string>

using namespace esphome::home_io_control;
using namespace management_test;
using namespace pairing_test;

namespace {

constexpr uint8_t DEVICE_CHALLENGE[HMAC_SIZE] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};

/// Declare the paired device the way a cover entity's YAML does.
void declare_cover(TestableManagementComponent &component) {
  DeviceConfig cfg{};
  cfg.type = DeviceType::ROLLER_SHUTTER;
  cfg.entity_class = DeviceCapabilityClass::COVER;
  component.add_device(node_id_to_string(DEVICE_ID), cfg);
}

}  // namespace

TEST(HubPairDevice, RefusesAnUnknownDevice) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);

  const auto result = component.management_actions_.pair_device("123456");
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "device is not registered on this hub");
  EXPECT_TRUE(radio.get_sent_data().empty());
}

TEST(HubPairDevice, RefusesADeviceOnlyAnEarlierPairingRegistered) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  IoDevice paired{};
  memcpy(paired.node_id, DEVICE_ID, NODE_ID_SIZE);
  paired.registered_by_pairing = true;
  component.registry_.put(node_id_to_string(DEVICE_ID), paired);

  const auto result = component.management_actions_.pair_device(node_id_to_string(DEVICE_ID));
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message,
            "no entity of the YAML declares this device; add its entry first, since its kind and power class come "
            "from it");
  EXPECT_TRUE(radio.get_sent_data().empty());
}

TEST(HubPairDevice, ReportsTheConfirmOnSuccess) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  declare_cover(component);
  radio.queue_rx(to_packet(device_frame(component.node_id_, CMD_CHALLENGE_REQ, DEVICE_CHALLENGE, HMAC_SIZE)));
  radio.queue_rx(to_packet(device_frame(component.node_id_, CMD_KEY_CONFIRM, nullptr, 0)));

  const auto result = component.management_actions_.pair_device(node_id_to_string(DEVICE_ID));
  EXPECT_TRUE(result.success);
  EXPECT_FALSE(result.verified);
  EXPECT_EQ(result.message, "device confirmed the key");
  EXPECT_EQ(component.pairing_telemetry().discovery_attempts(), 0u) << "pair_device sends no discovery";
}

TEST(HubPairDevice, ReportsNoConfirmWhenTheDeviceStaysSilent) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  declare_cover(component);

  const auto result = component.management_actions_.pair_device(node_id_to_string(DEVICE_ID));
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "no key confirm: was the device's pairing window open? The log has the step it stopped at");
}

TEST(HubPairDevice, ARefusalPublishesNoPairingResultButAnAttemptDoes) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  int published = 0;
  component.set_pairing_result_callback([&published] { ++published; });

  // Not declared: refused before anything is sent.
  EXPECT_FALSE(component.pair_declared_device(node_id_to_string(DEVICE_ID)).attempted());
  EXPECT_EQ(published, 0);
  EXPECT_EQ(component.last_pairing_end_ms_, 0U);

  // Declared but the device never answers: an attempt ran, so its telemetry is published.
  declare_cover(component);
  const auto result = component.pair_declared_device(node_id_to_string(DEVICE_ID));
  EXPECT_TRUE(result.attempted());
  EXPECT_FALSE(result.confirmed);
  EXPECT_EQ(published, 1);
  EXPECT_NE(component.last_pairing_end_ms_, 0U);
}

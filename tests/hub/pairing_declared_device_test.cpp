/// @file pairing_declared_device_test.cpp
/// @brief pair_declared_device(): what it refuses, and that it runs directed with no discovery.

#include "hub_core.h"
#include "pairing_engine.h"
#include "proto_commands.h"
#include "proto_constants.h"
#include "proto_frame.h"

#include "pairing_test_support.h"
#include "test_helpers.h"
#include "stubs/radio_test_common.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

using namespace esphome::home_io_control;
using namespace pairing_test;

namespace {

constexpr uint8_t DEVICE_CHALLENGE[HMAC_SIZE] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};

class TestableComponent : public IOHomeControlComponent {
 public:
  using IOHomeControlComponent::exchange_engine_;
  using IOHomeControlComponent::initialized_;
  using IOHomeControlComponent::node_id_;
  using IOHomeControlComponent::pairing_telemetry_;
  using IOHomeControlComponent::radio_;
  using IOHomeControlComponent::registry_;
  using IOHomeControlComponent::system_key_;
  using IOHomeControlComponent::tuning_;

  RecentOneWayPairingSighting recent_oneway_pairing_sighting_{};
  test::TestablePairingEngine pairing_engine_{
      &radio_,          node_id_,  system_key_,        &tuning_,
      exchange_engine_, registry_, pairing_telemetry_, recent_oneway_pairing_sighting_};
};

void setup(TestableComponent &comp, MockRadio &radio) {
  comp.initialized_ = true;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);
}

/// Declare the device the way a cover entity's YAML does.
void declare_device(TestableComponent &comp) {
  DeviceConfig cfg{};
  cfg.type = DeviceType::ROLLER_SHUTTER;
  cfg.entity_class = DeviceCapabilityClass::COVER;
  comp.registry_.add(node_id_to_string(DEVICE_ID), cfg);
}

/// The device's side of the exchange: a challenge for our 0x31, then the confirm.
void queue_device_answers(MockRadio &radio, const uint8_t own[NODE_ID_SIZE]) {
  radio.queue_rx(to_packet(device_frame(own, CMD_CHALLENGE_REQ, DEVICE_CHALLENGE, HMAC_SIZE)));
  radio.queue_rx(to_packet(device_frame(own, CMD_KEY_CONFIRM, nullptr, 0)));
}

}  // namespace

TEST(PairingDeclaredDevice, AnUndeclaredDeviceIsRefusedBeforeAnythingIsSent) {
  TestableComponent comp;
  MockRadio radio;
  setup(comp, radio);

  EXPECT_FALSE(comp.pairing_engine_.pair_declared_device(node_id_to_string(DEVICE_ID)));
  EXPECT_TRUE(radio.get_sent_data().empty());
}

TEST(PairingDeclaredDevice, APairingRegisteredDeviceIsNotDeclaredAndIsRefused) {
  TestableComponent comp;
  MockRadio radio;
  setup(comp, radio);
  IoDevice paired{};
  memcpy(paired.node_id, DEVICE_ID, NODE_ID_SIZE);
  paired.registered_by_pairing = true;
  comp.registry_.put(node_id_to_string(DEVICE_ID), paired);

  EXPECT_FALSE(comp.pairing_engine_.pair_declared_device(node_id_to_string(DEVICE_ID)));
  EXPECT_TRUE(radio.get_sent_data().empty());
}

TEST(PairingDeclaredDevice, APullDirectionIsRefusedRatherThanSilentlyPushed) {
  TestableComponent comp;
  MockRadio radio;
  setup(comp, radio);
  declare_device(comp);
  comp.tuning_.pairing_key_transfer = KeyTransferMode::PULL;

  EXPECT_FALSE(comp.pairing_engine_.pair_declared_device(node_id_to_string(DEVICE_ID)));
  EXPECT_TRUE(radio.get_sent_data().empty());
}

TEST(PairingDeclaredDevice, TheKeyExchangeRunsDirectedWithNoDiscoveryBroadcast) {
  TestableComponent comp;
  MockRadio radio;
  setup(comp, radio);
  declare_device(comp);
  queue_device_answers(radio, comp.node_id_);

  EXPECT_TRUE(comp.pairing_engine_.pair_declared_device(node_id_to_string(DEVICE_ID)));

  const std::vector<uint8_t> commands = sent_commands(radio);
  EXPECT_EQ(std::count(commands.begin(), commands.end(), CMD_DISCOVER_REQ), 0);
  EXPECT_EQ(std::count(commands.begin(), commands.end(), CMD_DISCOVER_ALT_REQ), 0);
  EXPECT_EQ(std::count(commands.begin(), commands.end(), CMD_KEY_INIT), 1);
  EXPECT_EQ(std::count(commands.begin(), commands.end(), CMD_KEY_TRANSFER), 1);

  for (const IoFrame &frame : sent_frames_of(radio, CMD_KEY_INIT))
    EXPECT_EQ(memcmp(frame.dst, DEVICE_ID, NODE_ID_SIZE), 0);
  for (const IoFrame &frame : sent_frames_of(radio, CMD_KEY_TRANSFER))
    EXPECT_EQ(memcmp(frame.dst, DEVICE_ID, NODE_ID_SIZE), 0);

  const IoDevice *dev = comp.registry_.get(node_id_to_string(DEVICE_ID));
  ASSERT_NE(dev, nullptr);
  EXPECT_FALSE(dev->registered_by_pairing);
}

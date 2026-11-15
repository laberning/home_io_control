/// @file pairing_key_holders_test.cpp
/// @brief KeyHolderSearch on its own: the heater probe, the roll-call under a key, and which of the
/// devices that answer are registered. No PairingEngine is built.

#include "hub_pairing.h"
#include "pairing_key_holders.h"
#include "pairing_test_support.h"
#include "proto_commands.h"
#include "proto_device_types.h"
#include "proto_frame.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"

#include <cstring>
#include <vector>

using namespace esphome::home_io_control;
using namespace pairing_test;

namespace {

/// A KeyHolderSearch standing alone on the rig's exchange engine, registry and telemetry.
struct KeyHoldersRig : PairingPartsRig {
  using PairingPartsRig::PairingPartsRig;
  KeyHolderSearch holders{&radio_ptr, node_id, system_key, &tuning, engine, registry, telemetry};
};

/// The context after discovery of an Atlantic remote-controller node (the AGILIA pairing node), whose
/// heater is at the next address.
pairing::PairingContext context_of_atlantic_node() {
  pairing::PairingContext context;
  memcpy(context.device.node_id, DEVICE_ID, NODE_ID_SIZE);
  context.device_id = node_id_to_string(DEVICE_ID);
  context.device.type = DeviceType::REMOTE_CONTROLLER;
  context.discovery_manufacturer = MANUFACTURER_ATLANTIC_GROUP;
  return context;
}

/// Sends the unanswered heater probe makes before the roll-call's first 0x2A: three tries of its 0x20.
constexpr int PROBE_SENDS = 3;

/// Queue `reply` to arrive once the unanswered probe is over, i.e. during the roll-call.
void queue_roll_call_reply(MockRadio &radio, const IoFrame &reply) {
  radio.queue_rx_hold_until_sent(PROBE_SENDS + 1);
  radio.queue_rx(to_packet(reply));
}

}  // namespace

TEST(KeyHolderSearch, AHeaterThatAnswersTheProbeIsRegisteredWithTheKeyAndNoRollCallRuns) {
  MockRadio radio;
  KeyHoldersRig rig(radio);
  pairing::PairingContext context = context_of_atlantic_node();
  queue_heater_answers_probe(radio, rig.node_id);

  const KeyHolderSearch::Result found = rig.holders.search_and_register(context, OTHER_KEY, /*device_key=*/true, true);

  EXPECT_EQ(found.answered, 1);
  EXPECT_EQ(found.registered, 1);
  const IoDevice *heater = rig.registry.get("B6BDFC");
  ASSERT_NE(heater, nullptr);
  EXPECT_EQ(heater->type, DeviceType::ELECTRICAL_HEATER);
  ASSERT_TRUE(heater->has_system_key);
  EXPECT_EQ(0, memcmp(heater->system_key, OTHER_KEY, AES_KEY_SIZE));
  EXPECT_TRUE(heater->registered_by_pairing);
  EXPECT_TRUE(sent_frames_of(radio, CMD_DISCOVER_SPE_REQ).empty()) << "the probe found it: no roll-call";
  EXPECT_EQ(context.state, pairing::PairingState::SCAN_PULLED_KEY);
}

TEST(KeyHolderSearch, AHubKeyHeaterCarriesNoKeyOfItsOwn) {
  MockRadio radio;
  KeyHoldersRig rig(radio);
  pairing::PairingContext context = context_of_atlantic_node();
  queue_heater_answers_probe(radio, rig.node_id);

  const KeyHolderSearch::Result found =
      rig.holders.search_and_register(context, test::TEST_SYSTEM_KEY, /*device_key=*/false, true);

  EXPECT_EQ(found.registered, 1);
  const IoDevice *heater = rig.registry.get("B6BDFC");
  ASSERT_NE(heater, nullptr);
  EXPECT_FALSE(heater->has_system_key) << "it uses the hub key";
}

TEST(KeyHolderSearch, ASilentProbeFallsBackToTheRollCall) {
  MockRadio radio;
  KeyHoldersRig rig(radio);
  pairing::PairingContext context = context_of_atlantic_node();
  queue_roll_call_reply(radio, heater_roll_call_reply(rig.node_id));

  const KeyHolderSearch::Result found = rig.holders.search_and_register(context, OTHER_KEY, /*device_key=*/true, true);

  EXPECT_EQ(sent_frames_of(radio, CMD_WRITE_PRIVATE).size(), static_cast<size_t>(PROBE_SENDS));
  EXPECT_FALSE(sent_frames_of(radio, CMD_DISCOVER_SPE_REQ).empty());
  EXPECT_EQ(found.answered, 1);
  EXPECT_EQ(found.registered, 1);
  ASSERT_NE(rig.registry.get("B6BDFC"), nullptr);
}

TEST(KeyHolderSearch, ADeviceOfAnotherMakerIsFoundByTheRollCallOnly) {
  MockRadio radio;
  KeyHoldersRig rig(radio);
  pairing::PairingContext context = context_of_atlantic_node();
  context.discovery_manufacturer = 2;  // Somfy: no heater is known at the next address
  radio.queue_rx_hold_until_sent(1);
  radio.queue_rx(to_packet(heater_roll_call_reply(rig.node_id)));

  const KeyHolderSearch::Result found = rig.holders.search_and_register(context, OTHER_KEY, /*device_key=*/true, true);

  EXPECT_TRUE(sent_frames_of(radio, CMD_WRITE_PRIVATE).empty()) << "nothing is probed";
  EXPECT_EQ(found.registered, 1);
}

TEST(KeyHolderSearch, AResponderOfATypeWithoutAPlatformIsCountedButNotRegistered) {
  MockRadio radio;
  KeyHoldersRig rig(radio);
  pairing::PairingContext context = context_of_atlantic_node();
  // Type 22 subtype 1 (packed 05 81): the Atlantic heat pump, which no platform drives yet.
  queue_roll_call_reply(radio, heater_roll_call_reply(rig.node_id, 0x05, 0x81));

  const KeyHolderSearch::Result found = rig.holders.search_and_register(context, OTHER_KEY, /*device_key=*/true, true);

  EXPECT_EQ(found.answered, 1);
  EXPECT_EQ(found.registered, 0);
  EXPECT_EQ(rig.registry.get("B6BDFC"), nullptr);
}

TEST(KeyHolderSearch, ADeviceTheYamlDeclaresKeepsItsRecordAndIsNotProbed) {
  MockRadio radio;
  KeyHoldersRig rig(radio);
  pairing::PairingContext context = context_of_atlantic_node();
  DeviceConfig cfg;
  cfg.type = DeviceType::ELECTRICAL_HEATER;
  cfg.subtype = 1;
  rig.registry.add("B6BDFC", cfg);
  radio.queue_rx_hold_until_sent(1);
  radio.queue_rx(to_packet(heater_roll_call_reply(rig.node_id)));

  const KeyHolderSearch::Result found = rig.holders.search_and_register(context, OTHER_KEY, /*device_key=*/true, true);

  EXPECT_EQ(found.answered, 1);
  EXPECT_EQ(found.registered, 0);
  EXPECT_FALSE(rig.registry.get("B6BDFC")->has_system_key) << "the YAML record is left alone";
  EXPECT_TRUE(sent_frames_of(radio, CMD_WRITE_PRIVATE).empty());
}

TEST(KeyHolderSearch, ARecordAnEarlierPairingAddedIsReplaced) {
  MockRadio radio;
  KeyHoldersRig rig(radio);
  pairing::PairingContext context = context_of_atlantic_node();
  IoDevice earlier{};
  memcpy(earlier.node_id, HEATER_ID, NODE_ID_SIZE);
  earlier.type = DeviceType::ELECTRICAL_HEATER;
  earlier.subtype = 1;
  earlier.registered_by_pairing = true;
  rig.registry.put("B6BDFC", earlier);
  queue_heater_answers_probe(radio, rig.node_id);

  const KeyHolderSearch::Result found = rig.holders.search_and_register(context, OTHER_KEY, /*device_key=*/true, true);

  EXPECT_EQ(found.registered, 1);
  const IoDevice *heater = rig.registry.get("B6BDFC");
  ASSERT_NE(heater, nullptr);
  ASSERT_TRUE(heater->has_system_key);
  EXPECT_EQ(0, memcmp(heater->system_key, OTHER_KEY, AES_KEY_SIZE));
}

TEST(KeyHolderSearch, AnUnprovenKeyNoDeviceAnswersUnderRegistersNothing) {
  MockRadio radio;
  KeyHoldersRig rig(radio);
  pairing::PairingContext context = context_of_atlantic_node();

  const KeyHolderSearch::Result found =
      rig.holders.search_and_register(context, OTHER_KEY, /*device_key=*/true, /*key_proven=*/false);

  EXPECT_EQ(found.answered, 0);
  EXPECT_EQ(found.registered, 0);
  EXPECT_EQ(rig.registry.size(), 0u);
}

TEST(KeyHolderSearch, AnUnprovenKeyIsProvenByTheDeviceThatAnswersUnderIt) {
  MockRadio radio;
  KeyHoldersRig rig(radio);
  pairing::PairingContext context = context_of_atlantic_node();
  queue_heater_answers_probe(radio, rig.node_id);

  const KeyHolderSearch::Result found =
      rig.holders.search_and_register(context, OTHER_KEY, /*device_key=*/true, /*key_proven=*/false);

  EXPECT_EQ(found.answered, 1);
  EXPECT_EQ(found.registered, 1);
}

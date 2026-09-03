#include "hub_core.h"
#include "hub_internal.h"
#include "proto_codecs.h"
#include "proto_crypto.h"
#include "proto_frame.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"

#include <cstring>

using namespace esphome::home_io_control;
using test::TestableHubComponent;

// ============================================================================
// HubOneWayKeyAdoption test suite
// ============================================================================
// Hub wiring for opt-in, receive-only 1W controller-key adoption: arm/disarm, the
// disarm-on-adoption guarantee, the auto-off window, and — most importantly — that a disarmed
// hub behaves exactly as it did before this feature existed.

namespace {

constexpr uint8_t OUR_NODE_ID[NODE_ID_SIZE] = {0xC0, 0xFF, 0xEE};
constexpr uint8_t SENDER_NODE[NODE_ID_SIZE] = {0xAB, 0xCD, 0xEF};
constexpr uint8_t BROADCAST_ALL[NODE_ID_SIZE] = {0x00, 0x00, 0x3F};

/// The published add-controller vector's plaintext controller key
/// (tests/corpus/captures/enrollment/reference_1w_enrollment_add_controller_kat.yaml).
constexpr uint8_t PUBLISHED_KEY[AES_KEY_SIZE] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                                 0x09, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16};

void setup_component(TestableHubComponent &comp, MockRadio &radio) {
  std::memcpy(comp.node_id_, OUR_NODE_ID, NODE_ID_SIZE);
  static const uint8_t key[AES_KEY_SIZE] = {0xD1, 0x74, 0x34, 0x93, 0xFA, 0x94, 0x38, 0x45,
                                            0xAC, 0x43, 0x50, 0xEE, 0xFF, 0x34, 0x29, 0x34};
  std::memcpy(comp.system_key_, key, AES_KEY_SIZE);
  comp.initialized_ = true;
  comp.radio_ = &radio;
}

/// Build a well-formed 1W CMD_ONEWAY_ADD_CONTROLLER frame carrying `key` wrapped for `sender`,
/// with a genuine MAC over the command-specific span (cmd + enc_key).
IoFrame make_add_controller_frame(const uint8_t sender[NODE_ID_SIZE], const uint8_t key[AES_KEY_SIZE],
                                  uint16_t sequence) {
  IoFrame frame{};
  init_frame(frame, /*is_2w=*/false, /*start=*/true, /*end=*/true, /*low_power=*/false);
  set_src(frame, sender);
  set_dst(frame, BROADCAST_ALL);

  uint8_t payload[AES_KEY_SIZE + 4] = {0};
  crypto::crypt_1w_key(sender, key, payload);
  payload[AES_KEY_SIZE] = 0x02;                                     // manufacturer
  payload[AES_KEY_SIZE + 1] = 0x01;                                 // data
  payload[AES_KEY_SIZE + 2] = static_cast<uint8_t>(sequence >> 8);  // sequence, big-endian
  payload[AES_KEY_SIZE + 3] = static_cast<uint8_t>(sequence & 0xFF);
  set_cmd(frame, CMD_ONEWAY_ADD_CONTROLLER, payload, sizeof(payload));

  uint8_t span[1 + AES_KEY_SIZE];
  span[0] = CMD_ONEWAY_ADD_CONTROLLER;
  std::memcpy(&span[1], payload, AES_KEY_SIZE);
  crypto::create_1w_hmac(span, sizeof(span), sequence, key, frame.mac);
  frame.has_mac = true;
  return frame;
}

RadioRxPacket make_rx_packet(const IoFrame &frame) {
  RadioRxPacket pkt{};
  pkt.len = serialize(frame, pkt.data, sizeof(pkt.data));
  pkt.freq_hz = FREQ_CH2;
  return pkt;
}

}  // namespace

// ========================================================================================
// Arm / disarm lifecycle
// ========================================================================================

TEST(HubOneWayKeyAdoption, StartsDisarmed) {
  TestableHubComponent comp;
  MockRadio radio;
  setup_component(comp, radio);
  EXPECT_FALSE(comp.oneway_key_adoption_armed()) << "adoption must never be armed without an explicit request";
}

TEST(HubOneWayKeyAdoption, ArmAndManualDisarmNotifyTheSwitch) {
  TestableHubComponent comp;
  MockRadio radio;
  setup_component(comp, radio);

  std::vector<bool> notifications;
  comp.set_oneway_key_adoption_armed_callback([&](bool armed) { notifications.push_back(armed); });

  comp.set_oneway_key_adoption_armed(true);
  EXPECT_TRUE(comp.oneway_key_adoption_armed());
  comp.set_oneway_key_adoption_armed(false);
  EXPECT_FALSE(comp.oneway_key_adoption_armed());

  ASSERT_EQ(notifications.size(), 2u) << "the switch entity must be told about both transitions";
  EXPECT_TRUE(notifications[0]);
  EXPECT_FALSE(notifications[1]);
}

TEST(HubOneWayKeyAdoption, RedundantDisarmDoesNotRenotify) {
  TestableHubComponent comp;
  MockRadio radio;
  setup_component(comp, radio);

  int notifications = 0;
  comp.set_oneway_key_adoption_armed_callback([&](bool) { notifications++; });

  comp.set_oneway_key_adoption_armed(false);
  EXPECT_EQ(notifications, 0) << "disarming an already-disarmed listener must be a no-op";
}

// ========================================================================================
// Adoption behaviour
// ========================================================================================

TEST(HubOneWayKeyAdoption, ArmedAdoptionDisarmsImmediately) {
  TestableHubComponent comp;
  MockRadio radio;
  setup_component(comp, radio);
  comp.set_oneway_key_adoption_armed(true);
  ASSERT_TRUE(comp.oneway_key_adoption_armed());

  const IoFrame frame = make_add_controller_frame(SENDER_NODE, PUBLISHED_KEY, 0x1234);
  comp.process_received_packet_(make_rx_packet(frame));

  EXPECT_FALSE(comp.oneway_key_adoption_armed())
      << "one adoption per arm: recovering a key must close the window immediately";
}

TEST(HubOneWayKeyAdoption, DisarmedIgnoresAddControllerFrame) {
  TestableHubComponent comp;
  MockRadio radio;
  setup_component(comp, radio);
  ASSERT_FALSE(comp.oneway_key_adoption_armed());

  const IoFrame frame = make_add_controller_frame(SENDER_NODE, PUBLISHED_KEY, 0x1234);
  comp.process_received_packet_(make_rx_packet(frame));

  EXPECT_FALSE(comp.oneway_key_adoption_armed()) << "a disarmed hub must stay disarmed";
  EXPECT_TRUE(radio.get_sent_data().empty()) << "1W key adoption is receive-only — nothing may be transmitted";
}

TEST(HubOneWayKeyAdoption, MalformedFrameLeavesWindowArmed) {
  TestableHubComponent comp;
  MockRadio radio;
  setup_component(comp, radio);
  comp.set_oneway_key_adoption_armed(true);

  // Truncate the declared payload so the decoder rejects it on length. A corrupted capture must
  // not consume the user's arm — they would otherwise have to re-arm and repeat the physical
  // key-copy gesture for nothing.
  IoFrame frame = make_add_controller_frame(SENDER_NODE, PUBLISHED_KEY, 0x1234);
  uint8_t short_payload[AES_KEY_SIZE] = {0};
  std::memcpy(short_payload, frame.data, AES_KEY_SIZE);
  frame.has_mac = false;
  set_cmd(frame, CMD_ONEWAY_ADD_CONTROLLER, short_payload, sizeof(short_payload));

  comp.process_received_packet_(make_rx_packet(frame));
  EXPECT_TRUE(comp.oneway_key_adoption_armed()) << "a frame that fails to decode must not consume the arm";
}

TEST(HubOneWayKeyAdoption, ArmedIgnoresUnrelatedOneWayTraffic) {
  TestableHubComponent comp;
  MockRadio radio;
  setup_component(comp, radio);
  comp.set_oneway_key_adoption_armed(true);

  // An ordinary 1W EXECUTE from the same sender: the listener must not treat it as an adoption,
  // and must leave the window open for the frame the user is actually about to trigger.
  IoFrame frame{};
  init_frame(frame, /*is_2w=*/false, /*start=*/true, /*end=*/true, /*low_power=*/false);
  set_src(frame, SENDER_NODE);
  set_dst(frame, BROADCAST_ALL);
  const uint8_t payload[] = {0x01, 0x43, 0xD2, 0x00, 0x00, 0x00, 0x05, 0x99};
  set_cmd(frame, CMD_EXECUTE, payload, sizeof(payload));

  comp.process_received_packet_(make_rx_packet(frame));
  EXPECT_TRUE(comp.oneway_key_adoption_armed()) << "unrelated 1W traffic must not consume the arm";
}

// ========================================================================================
// The ready-to-paste adoption report
// ========================================================================================
// build_oneway_adoption_report() is pure, so these assert the exact text a user will be asked to
// copy — the host ESP_LOG stub discards its arguments, so testing the builder directly is the
// only way to pin the report's contents.

namespace {

OneWayAdoptedKey make_adopted(OneWayMacStatus mac_status) {
  OneWayAdoptedKey adopted{};
  std::memcpy(adopted.system_key, PUBLISHED_KEY, AES_KEY_SIZE);
  std::memcpy(adopted.sender_node, SENDER_NODE, NODE_ID_SIZE);
  adopted.manufacturer = 0x02;
  adopted.sequence = 0x1234;
  adopted.mac_status = mac_status;
  return adopted;
}

}  // namespace

TEST(HubOneWayKeyAdoption, ReportContainsPasteableBlockAndKeyExactlyOnce) {
  const std::string report =
      detail::build_oneway_adoption_report(make_adopted(OneWayMacStatus::VERIFIED), true, DeviceType::ROLLER_SHUTTER);

  EXPECT_NE(report.find("oneway_controllers:"), std::string::npos);
  EXPECT_NE(report.find("- id: adopted_abcdef"), std::string::npos) << "identity id derives from the sender node";
  EXPECT_NE(report.find("manufacturer: 0x02"), std::string::npos);
  EXPECT_NE(report.find("commands: [open, close, stop]"), std::string::npos);
  EXPECT_NE(report.find("system_key: \"01020304050607080910111213141516\""), std::string::npos)
      << "the key must be inline in the YAML block, mirroring the 2W key-extraction report's "
         "format, not a !secret placeholder pointing at a field name to fill in separately";

  // node_id is deliberately omitted -- a later step derives it, and asking a user to invent a
  // 3-byte address is an unanswerable question.
  EXPECT_EQ(report.find("\n    node_id:"), std::string::npos) << "node_id must not be presented as a field to fill in";
  EXPECT_NE(report.find("node_id omitted"), std::string::npos) << "and the report must say why";

  // The recovered key appears exactly once, in the one intentional emission.
  const std::string key_hex = "01020304050607080910111213141516";
  const size_t first = report.find(key_hex);
  ASSERT_NE(first, std::string::npos) << "the recovered key must be present for the user to copy";
  EXPECT_EQ(report.find(key_hex, first + 1), std::string::npos) << "and must appear exactly once";
}

TEST(HubOneWayKeyAdoption, ReportPrefillsObservedDeviceClass) {
  const std::string report =
      detail::build_oneway_adoption_report(make_adopted(OneWayMacStatus::VERIFIED), true, DeviceType::ROLLER_SHUTTER);

  EXPECT_NE(report.find("io_device_type: " + format_device_type_for_yaml(DeviceType::ROLLER_SHUTTER)),
            std::string::npos)
      << "an observed class must be prefilled rather than left for the user to guess";
  EXPECT_NE(report.find("observed from this sender"), std::string::npos) << "and marked as observed, not authoritative";
}

TEST(HubOneWayKeyAdoption, ReportFallsBackWhenNoClassObserved) {
  const std::string report =
      detail::build_oneway_adoption_report(make_adopted(OneWayMacStatus::VERIFIED), false, DeviceType::UNKNOWN);

  EXPECT_NE(report.find("# io_device_type: unknown"), std::string::npos)
      << "with nothing observed the key must be commented out rather than guessed";
  EXPECT_NE(report.find("DEBUG"), std::string::npos) << "and the user pointed at the log line that reveals it";
}

TEST(HubOneWayKeyAdoption, ReportSurfacesMacStatusDistinctly) {
  const std::string verified =
      detail::build_oneway_adoption_report(make_adopted(OneWayMacStatus::VERIFIED), false, DeviceType::UNKNOWN);
  const std::string failed =
      detail::build_oneway_adoption_report(make_adopted(OneWayMacStatus::FAILED), false, DeviceType::UNKNOWN);
  const std::string absent =
      detail::build_oneway_adoption_report(make_adopted(OneWayMacStatus::NOT_PRESENT), false, DeviceType::UNKNOWN);

  EXPECT_NE(verified.find("MAC VERIFIED"), std::string::npos);
  EXPECT_NE(failed.find("MAC FAILED"), std::string::npos);
  EXPECT_NE(absent.find("MAC not present"), std::string::npos);
  EXPECT_NE(verified, failed);
  EXPECT_NE(failed, absent);
}

// ========================================================================================
// io_device_type observation
// ========================================================================================

TEST(HubOneWayKeyAdoption, ObservedClassIsOnlyRecordedWhileArmed) {
  TestableHubComponent comp;
  MockRadio radio;
  setup_component(comp, radio);

  OneWayFrameInfo info{};
  std::memcpy(info.src, SENDER_NODE, NODE_ID_SIZE);
  info.target_type = DeviceType::ROLLER_SHUTTER;

  comp.oneway_key_adoption_.record_observed_class(info);
  EXPECT_FALSE(comp.oneway_key_adoption_.last_observed_class().valid)
      << "a disarmed hub must not accumulate observations";

  comp.set_oneway_key_adoption_armed(true);
  comp.oneway_key_adoption_.record_observed_class(info);
  EXPECT_TRUE(comp.oneway_key_adoption_.last_observed_class().valid);
  EXPECT_EQ(comp.oneway_key_adoption_.last_observed_class().type, DeviceType::ROLLER_SHUTTER);
}

TEST(HubOneWayKeyAdoption, UnknownClassDoesNotClobberAnEarlierObservation) {
  TestableHubComponent comp;
  MockRadio radio;
  setup_component(comp, radio);
  comp.set_oneway_key_adoption_armed(true);

  OneWayFrameInfo typed{};
  std::memcpy(typed.src, SENDER_NODE, NODE_ID_SIZE);
  typed.target_type = DeviceType::ROLLER_SHUTTER;
  comp.oneway_key_adoption_.record_observed_class(typed);

  // The add-controller frame itself broadcasts to "all", which decodes to UNKNOWN. It must not
  // erase the genuine class seen from the same sender moments earlier.
  OneWayFrameInfo untyped{};
  std::memcpy(untyped.src, SENDER_NODE, NODE_ID_SIZE);
  untyped.target_type = DeviceType::UNKNOWN;
  comp.oneway_key_adoption_.record_observed_class(untyped);

  EXPECT_TRUE(comp.oneway_key_adoption_.last_observed_class().valid);
  EXPECT_EQ(comp.oneway_key_adoption_.last_observed_class().type, DeviceType::ROLLER_SHUTTER);
}

TEST(HubOneWayKeyAdoption, ReArmingClearsAStaleObservation) {
  TestableHubComponent comp;
  MockRadio radio;
  setup_component(comp, radio);

  comp.set_oneway_key_adoption_armed(true);
  OneWayFrameInfo info{};
  std::memcpy(info.src, SENDER_NODE, NODE_ID_SIZE);
  info.target_type = DeviceType::ROLLER_SHUTTER;
  comp.oneway_key_adoption_.record_observed_class(info);
  ASSERT_TRUE(comp.oneway_key_adoption_.last_observed_class().valid);

  comp.set_oneway_key_adoption_armed(false);
  comp.set_oneway_key_adoption_armed(true);
  EXPECT_FALSE(comp.oneway_key_adoption_.last_observed_class().valid)
      << "an observation from a previous window must never prefill the next one";
}

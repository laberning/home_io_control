/// @file hub_management_test.cpp
/// @brief Tests for hub_management.cpp rename action support.

#include "hub_core.h"
#include "proto_commands.h"
#include "proto_frame.h"

#include "esphome/components/api/custom_api_device.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"

#include <cstring>

using namespace esphome::home_io_control;

namespace {

class TestableManagementComponent : public IOHomeControlComponent {
 public:
  using IOHomeControlComponent::api_force_open_device_;
  using IOHomeControlComponent::api_identify_device_;
  using IOHomeControlComponent::api_rename_device_;
  using IOHomeControlComponent::api_scan_paired_devices_;
  using IOHomeControlComponent::initialized_;
  using IOHomeControlComponent::node_id_;
  using IOHomeControlComponent::op_queue_;
  using IOHomeControlComponent::radio_;
  using IOHomeControlComponent::register_management_actions_;
  using IOHomeControlComponent::registry_;
  using IOHomeControlComponent::system_key_;
  using IOHomeControlComponent::tuning_;
};

static void setup_component(TestableManagementComponent &component, MockRadio &radio) {
  component.node_id_[0] = 0xC0;
  component.node_id_[1] = 0xFF;
  component.node_id_[2] = 0xEE;
  static const uint8_t key[] = {0xD1, 0x74, 0x34, 0x93, 0xFA, 0x94, 0x38, 0x45,
                                0xAC, 0x43, 0x50, 0xEE, 0xFF, 0x34, 0x29, 0x34};
  std::memcpy(component.system_key_, key, AES_KEY_SIZE);
  component.initialized_ = true;
  component.radio_ = &radio;
  // Small window so scan_paired_devices() tests don't spend thousands of no-op host-test
  // iterations waiting out the default 2000 ms discovery window: in host tests millis()
  // advances by exactly 1 per call.
  component.tuning_.pairing_discovery_wait_ms = 20;
  component.add_device("ABC123");
}

/// Build a CMD_DISCOVER_SPE_RESP reply frame with an arbitrary payload, for
/// scan_paired_devices() tests. `payload`/`payload_len` may be shorter than
/// DISCOVERY_RESP_FULL_SIZE to exercise truncated-metadata decoding.
static IoFrame build_spe_response(const uint8_t src[3], const uint8_t dst[3], const uint8_t *payload,
                                  uint8_t payload_len) {
  IoFrame frame{};
  init_frame(frame, true, true, true, false);
  set_dst(frame, dst);
  set_src(frame, src);
  set_cmd(frame, CMD_DISCOVER_SPE_RESP, payload, payload_len);
  return frame;
}

static RadioRxPacket frame_to_packet(const IoFrame &frame) {
  RadioRxPacket packet{};
  packet.len = serialize(frame, packet.data, sizeof(packet.data));
  packet.freq_hz = FREQ_CH2;
  return packet;
}

static IoFrame make_set_name_response(const uint8_t dst[3]) {
  IoFrame frame{};
  init_frame(frame, true, false, true, false);
  const uint8_t device_node_id[3] = {0xAB, 0xC1, 0x23};
  set_dst(frame, dst);
  set_src(frame, device_node_id);
  set_cmd(frame, CMD_SET_NAME_RESP, nullptr, 0);
  return frame;
}

static IoFrame make_get_name_response(const uint8_t dst[3], const char *name) {
  IoFrame frame{};
  init_frame(frame, true, false, true, false);
  const uint8_t device_node_id[3] = {0xAB, 0xC1, 0x23};
  set_dst(frame, dst);
  set_src(frame, device_node_id);

  uint8_t payload[DEVICE_NAME_WRITE_PAYLOAD_SIZE]{};
  std::string normalized_name;
  EXPECT_EQ(encode_device_name_payload(name, payload, normalized_name), DeviceNameValidationError::NONE);
  set_cmd(frame, CMD_GET_NAME_RESP, payload, DEVICE_NAME_WRITE_PAYLOAD_SIZE);
  return frame;
}

static IoFrame make_error_response(const uint8_t dst[3], uint8_t result_code) {
  IoFrame frame{};
  init_frame(frame, true, false, true, false);
  const uint8_t device_node_id[3] = {0xAB, 0xC1, 0x23};
  set_dst(frame, dst);
  set_src(frame, device_node_id);
  uint8_t payload[1] = {result_code};
  set_cmd(frame, CMD_ERROR_RESP, payload, sizeof(payload));
  return frame;
}

// identify_device does not check the response command type (there is no dedicated
// CMD_IDENTIFY response), so any endpoint-matched, non-error reply counts as an
// acknowledgment. Echoing CMD_IDENTIFY itself is a plausible real-world reply and keeps the
// test independent of any other action's response shape.
static IoFrame make_identify_ack_response(const uint8_t dst[3]) {
  IoFrame frame{};
  init_frame(frame, true, false, true, false);
  const uint8_t device_node_id[3] = {0xAB, 0xC1, 0x23};
  set_dst(frame, dst);
  set_src(frame, device_node_id);
  set_cmd(frame, CMD_IDENTIFY, nullptr, 0);
  return frame;
}

}  // namespace

TEST(HubManagement, RenameDeviceRejectsUnknownDevice) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);

  const auto result = component.rename_device("123456", "Patio Awning");
  EXPECT_FALSE(result.success);
  EXPECT_FALSE(result.verified);
  EXPECT_EQ(result.message, "device is not registered on this hub");
}

TEST(HubManagement, RenameDeviceRejectsWhenHubNotInitialized) {
  TestableManagementComponent component;

  const auto result = component.rename_device("ABC123", "Patio Awning");
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "hub is not initialized");
}

TEST(HubManagement, RenameDeviceRejectsInvalidDeviceId) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);

  const auto result = component.rename_device("12", "Patio Awning");
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "device ID must be exactly 6 hexadecimal characters");
}

TEST(HubManagement, RenameDeviceRejectsInvalidName) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);

  const auto result = component.rename_device("ABC123", "");
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, device_name_validation_error_description(DeviceNameValidationError::EMPTY));
}

TEST(HubManagement, RenameDeviceSuccessVerifiesReadbackAndCachesName) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  radio.queue_rx(frame_to_packet(make_set_name_response(component.node_id_)));
  radio.queue_rx(frame_to_packet(make_get_name_response(component.node_id_, "Patio Awning")));

  const auto result = component.rename_device("ABC123", "Patio Awning");
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.verified);
  EXPECT_EQ(result.message, "rename verified by device readback");
  EXPECT_EQ(result.applied_name, "Patio Awning");

  auto *device = component.get_device("ABC123");
  ASSERT_NE(device, nullptr);
  EXPECT_STREQ(device->name, "Patio Awning");
}

TEST(HubManagement, RenameDeviceAcknowledgedButReadbackMismatchReturnsUnverifiedSuccess) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  radio.queue_rx(frame_to_packet(make_set_name_response(component.node_id_)));
  radio.queue_rx(frame_to_packet(make_get_name_response(component.node_id_, "Other Name")));

  const auto result = component.rename_device("ABC123", "Patio Awning");
  EXPECT_TRUE(result.success);
  EXPECT_FALSE(result.verified);
  EXPECT_EQ(result.message, "rename acknowledged but readback did not match the requested name");
  EXPECT_EQ(result.applied_name, "Other Name");
}

TEST(HubManagement, RenameDeviceAcknowledgedButReadbackFailsReturnsUnverifiedSuccess) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  radio.queue_rx(frame_to_packet(make_set_name_response(component.node_id_)));

  const auto result = component.rename_device("ABC123", "Patio Awning");
  EXPECT_TRUE(result.success);
  EXPECT_FALSE(result.verified);
  EXPECT_EQ(result.message, "rename acknowledged but verification readback failed");
}

TEST(HubManagement, RenameDeviceExplicitErrorReturnsDecodedResultCode) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  radio.queue_rx(frame_to_packet(make_error_response(component.node_id_, RESULT_LIMITATION_BY_WIND)));

  const auto result = component.rename_device("ABC123", "Patio Awning");
  EXPECT_FALSE(result.success);
  EXPECT_TRUE(result.has_result_code);
  EXPECT_EQ(result.result_code, RESULT_LIMITATION_BY_WIND);
  EXPECT_EQ(result.message, std::string(command_result_name(RESULT_LIMITATION_BY_WIND)) + ": " +
                                command_result_description(RESULT_LIMITATION_BY_WIND));
}

TEST(HubManagement, IdentifyDeviceRejectsUnknownDevice) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);

  const auto result = component.identify_device("123456");
  EXPECT_FALSE(result.success);
  EXPECT_FALSE(result.verified);
  EXPECT_EQ(result.message, "device is not registered on this hub");
}

TEST(HubManagement, IdentifyDeviceRejectsWhenHubNotInitialized) {
  TestableManagementComponent component;

  const auto result = component.identify_device("ABC123");
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "hub is not initialized");
}

TEST(HubManagement, IdentifyDeviceRejectsInvalidDeviceId) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);

  const auto result = component.identify_device("12");
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "device ID must be exactly 6 hexadecimal characters");
}

TEST(HubManagement, IdentifyDeviceNoResponseFails) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  // No queued RX at all: send_and_receive times out with no valid response.

  const auto result = component.identify_device("ABC123");
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "no valid response to identify request");
}

TEST(HubManagement, IdentifyDeviceSuccessWithDirectResponse) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  radio.queue_rx(frame_to_packet(make_identify_ack_response(component.node_id_)));

  const auto result = component.identify_device("ABC123");
  EXPECT_TRUE(result.success);
  EXPECT_FALSE(result.verified);
  EXPECT_EQ(result.message, "identify acknowledged by device");
}

TEST(HubManagement, IdentifyDeviceErrorResponseIsTreatedAsSuccess) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  radio.queue_rx(frame_to_packet(make_error_response(component.node_id_, RESULT_LIMITATION_BY_WIND)));

  const auto result = component.identify_device("ABC123");
  EXPECT_TRUE(result.success) << "CMD_ERROR_RESP to an identify request is expected/non-fatal";
  EXPECT_FALSE(result.verified);
  EXPECT_TRUE(result.has_result_code);
  EXPECT_EQ(result.result_code, RESULT_LIMITATION_BY_WIND);
  EXPECT_EQ(result.message, "identify triggered (device reported " +
                                std::string(command_result_name(RESULT_LIMITATION_BY_WIND)) + ": " +
                                command_result_description(RESULT_LIMITATION_BY_WIND) + ")");
}

TEST(HubManagement, ApiIdentifyDevicePublishesManagementResultEvent) {
  esphome::api::APIServer api_server;
  esphome::api::ScopedGlobalApiServer scoped_api_server(api_server);
  api_server.reset();

  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  radio.queue_rx(frame_to_packet(make_identify_ack_response(component.node_id_)));

  component.api_identify_device_("ABC123");

  ASSERT_EQ(api_server.events_.size(), 1u);
  const auto &event = api_server.events_.front();
  EXPECT_EQ(event.event_type, "esphome.home_io_control_action_result");
  EXPECT_EQ(event.data.at("action"), "identify_device");
  EXPECT_EQ(event.data.at("device_id"), "ABC123");
  EXPECT_EQ(event.data.at("success"), "true");
  EXPECT_EQ(event.data.at("verified"), "false");
  EXPECT_EQ(event.data.at("message"), "identify acknowledged by device");
  EXPECT_EQ(event.data.count("requested_name"), 0u) << "identify has no name fields";
  EXPECT_EQ(event.data.count("applied_name"), 0u) << "identify has no name fields";
  EXPECT_EQ(event.data.count("result_code"), 0u) << "no result_code on a non-error acknowledgment";
}

TEST(HubManagement, ForceOpenDeviceRejectsUnknownDevice) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);

  const auto result = component.force_open_device("123456");
  EXPECT_FALSE(result.success);
  EXPECT_FALSE(result.verified);
  EXPECT_EQ(result.message, "device is not registered on this hub");
}

TEST(HubManagement, ForceOpenDeviceRejectsWhenHubNotInitialized) {
  TestableManagementComponent component;

  const auto result = component.force_open_device("ABC123");
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "hub is not initialized");
}

TEST(HubManagement, ForceOpenDeviceRejectsInvalidDeviceId) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);

  const auto result = component.force_open_device("12");
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "device ID must be exactly 6 hexadecimal characters");
}

TEST(HubManagement, ForceOpenDeviceEnqueuesForceOpenCommand) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);

  const auto result = component.force_open_device("ABC123");
  EXPECT_TRUE(result.success);
  EXPECT_FALSE(result.verified);
  EXPECT_EQ(result.message,
            "force open queued (elevated-priority open; wind/rain lock bypass unconfirmed; movement result arrives "
            "via cover state)");

  ASSERT_EQ(component.op_queue_.size(), 1u);
  EXPECT_EQ(component.op_queue_.front().type, PendingOperationType::DEVICE_COMMAND);
  EXPECT_EQ(component.op_queue_.front().command, CoverCommand::FORCE_OPEN);
  EXPECT_EQ(component.op_queue_.front().device_id, "ABC123");
}

TEST(HubManagement, ForceOpenDeviceRejectsNonCoverDevice) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  auto *dev = component.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  dev->type = DeviceType::LIGHT;

  const auto result = component.force_open_device("ABC123");
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "device does not accept cover commands");
  EXPECT_TRUE(component.op_queue_.empty());
}

TEST(HubManagement, ApiForceOpenDevicePublishesManagementResultEvent) {
  esphome::api::APIServer api_server;
  esphome::api::ScopedGlobalApiServer scoped_api_server(api_server);
  api_server.reset();

  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);

  component.api_force_open_device_("ABC123");

  ASSERT_EQ(api_server.events_.size(), 1u);
  const auto &event = api_server.events_.front();
  EXPECT_EQ(event.event_type, "esphome.home_io_control_action_result");
  EXPECT_EQ(event.data.at("action"), "force_open_device");
  EXPECT_EQ(event.data.at("device_id"), "ABC123");
  EXPECT_EQ(event.data.at("success"), "true");
  EXPECT_EQ(event.data.at("verified"), "false");
  EXPECT_EQ(event.data.at("message"),
            "force open queued (elevated-priority open; wind/rain lock bypass unconfirmed; movement result arrives "
            "via cover state)");
  EXPECT_EQ(event.data.count("requested_name"), 0u) << "force-open has no name fields";
  EXPECT_EQ(event.data.count("applied_name"), 0u) << "force-open has no name fields";
  EXPECT_EQ(event.data.count("result_code"), 0u) << "force-open's result is asynchronous, no result_code yet";
}

TEST(HubManagement, RegisterManagementActionsRegistersAllFourServices) {
  esphome::api::APIServer api_server;
  esphome::api::ScopedGlobalApiServer scoped_api_server(api_server);
  api_server.reset();

  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.register_management_actions_();

  ASSERT_EQ(esphome::api::global_api_server->user_services_.size(), 4u);

  const auto rename_response = esphome::api::global_api_server->user_services_[0]->encode_list_service_response();
  EXPECT_EQ(rename_response.name.str(), "rename_device");
  EXPECT_EQ(rename_response.supports_response, esphome::api::enums::SUPPORTS_RESPONSE_NONE);
  ASSERT_EQ(rename_response.args.size(), 2u);
  EXPECT_EQ(rename_response.args[0].name.str(), "device_id");
  EXPECT_EQ(rename_response.args[0].type, esphome::api::enums::SERVICE_ARG_TYPE_STRING);
  EXPECT_EQ(rename_response.args[1].name.str(), "new_name");
  EXPECT_EQ(rename_response.args[1].type, esphome::api::enums::SERVICE_ARG_TYPE_STRING);

  const auto identify_response = esphome::api::global_api_server->user_services_[1]->encode_list_service_response();
  EXPECT_EQ(identify_response.name.str(), "identify_device");
  EXPECT_EQ(identify_response.supports_response, esphome::api::enums::SUPPORTS_RESPONSE_NONE);
  ASSERT_EQ(identify_response.args.size(), 1u);
  EXPECT_EQ(identify_response.args[0].name.str(), "device_id");
  EXPECT_EQ(identify_response.args[0].type, esphome::api::enums::SERVICE_ARG_TYPE_STRING);

  const auto force_open_response = esphome::api::global_api_server->user_services_[2]->encode_list_service_response();
  EXPECT_EQ(force_open_response.name.str(), "force_open_device");
  EXPECT_EQ(force_open_response.supports_response, esphome::api::enums::SUPPORTS_RESPONSE_NONE);
  ASSERT_EQ(force_open_response.args.size(), 1u);
  EXPECT_EQ(force_open_response.args[0].name.str(), "device_id");
  EXPECT_EQ(force_open_response.args[0].type, esphome::api::enums::SERVICE_ARG_TYPE_STRING);

  const auto scan_response = esphome::api::global_api_server->user_services_[3]->encode_list_service_response();
  EXPECT_EQ(scan_response.name.str(), "scan_paired_devices");
  EXPECT_EQ(scan_response.supports_response, esphome::api::enums::SUPPORTS_RESPONSE_NONE);
  EXPECT_EQ(scan_response.args.size(), 0u) << "scan_paired_devices takes no arguments";
}

TEST(HubManagement, ApiRenameDevicePublishesManagementResultEvent) {
  esphome::api::APIServer api_server;
  esphome::api::ScopedGlobalApiServer scoped_api_server(api_server);
  api_server.reset();

  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  radio.queue_rx(frame_to_packet(make_set_name_response(component.node_id_)));
  radio.queue_rx(frame_to_packet(make_get_name_response(component.node_id_, "Patio Awning")));

  component.api_rename_device_("ABC123", "Patio Awning");

  ASSERT_EQ(api_server.events_.size(), 1u);
  const auto &event = api_server.events_.front();
  EXPECT_EQ(event.event_type, "esphome.home_io_control_action_result");
  EXPECT_EQ(event.data.at("action"), "rename_device");
  EXPECT_EQ(event.data.at("device_id"), "ABC123");
  EXPECT_EQ(event.data.at("success"), "true");
  EXPECT_EQ(event.data.at("verified"), "true");
  EXPECT_EQ(event.data.at("requested_name"), "Patio Awning");
  EXPECT_EQ(event.data.at("applied_name"), "Patio Awning");
}

TEST(HubManagement, ApiRenameDeviceSkipsEventWhenApiDisconnected) {
  esphome::api::APIServer api_server;
  esphome::api::ScopedGlobalApiServer scoped_api_server(api_server);
  api_server.reset();
  api_server.set_connected(false);

  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  radio.queue_rx(frame_to_packet(make_set_name_response(component.node_id_)));

  component.api_rename_device_("ABC123", "Patio Awning");
  EXPECT_TRUE(api_server.events_.empty());
}

TEST(HubManagement, RegisteredRenameActionExecutesComponentHandler) {
  esphome::api::APIServer api_server;
  esphome::api::ScopedGlobalApiServer scoped_api_server(api_server);
  api_server.reset();

  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.register_management_actions_();
  ASSERT_EQ(api_server.user_services_.size(), 4u);

  radio.queue_rx(frame_to_packet(make_set_name_response(component.node_id_)));
  radio.queue_rx(frame_to_packet(make_get_name_response(component.node_id_, "Patio Awning")));

  const auto response = api_server.user_services_.front()->encode_list_service_response();
  esphome::api::ExecuteServiceRequest request;
  request.key = response.key;
  request.args.init(2);
  request.args.emplace_back().string_ = esphome::StringRef("ABC123");
  request.args.emplace_back().string_ = esphome::StringRef("Patio Awning");

  EXPECT_TRUE(api_server.user_services_.front()->execute_service(request));

  auto *device = component.get_device("ABC123");
  ASSERT_NE(device, nullptr);
  EXPECT_STREQ(device->name, "Patio Awning");
}

// ============================================================================
// scan_paired_devices tests
// ============================================================================
// Reply payloads match the real fixtures in
// tests/corpus/captures/somfy_awning/discover_spe_paired_rollcall.yaml and
// tests/corpus/captures/somfy_dimmer/discover_spe_paired_rollcall.yaml:
// awning "04 00 30 E1 F2 02 CC FC 03" -> HORIZONTAL_AWNING/0, backbone 30E1F2, MANUFACTURER_SOMFY;
// dimmer "01 80 41 5C E4 02 CC 07 EB" -> LIGHT/0, backbone 415CE4, MANUFACTURER_SOMFY. Using real
// bytes here and in the corpus replay tests (tests/corpus_spe_rollcall_replay_test.cpp) means
// both tell the same story.

TEST(HubManagement, ScanPairedDevicesRejectsWhenHubNotInitialized) {
  TestableManagementComponent component;

  const auto result = component.scan_paired_devices();
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "hub is not initialized");
}

TEST(HubManagement, ScanPairedDevicesZeroRepliesReportsZeroAndSucceeds) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);

  const auto result = component.scan_paired_devices();
  EXPECT_TRUE(result.success) << "no replies is a valid result, not a failure";
  EXPECT_FALSE(result.verified);
  EXPECT_TRUE(result.device_id.empty()) << "there is no single target device";
  EXPECT_NE(result.message.find("0 devices detected"), std::string::npos);
  EXPECT_EQ(result.message.find("io_device_id"), std::string::npos) << "no YAML block when nothing replied";
}

TEST(HubManagement, ScanPairedDevicesReportsTruncationWhenMoreDevicesAnswerThanFit) {
  // An install larger than the array must say so rather than quietly listing a subset, which
  // would look like the missing devices had dropped off the network.
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);

  // One more distinct responder than the cap. Addresses are synthetic and deliberately unique;
  // the payload is a valid awning decode so each one produces a full report entry.
  const uint8_t payload[] = {0x04, 0x00, 0x30, 0xE1, 0xF2, 0x02, 0xCC, 0xFC, 0x03};
  constexpr int OVER_CAP = 25;
  for (int i = 0; i < OVER_CAP; i++) {
    const uint8_t src[3] = {0x10, static_cast<uint8_t>(i), 0x01};
    radio.queue_rx(frame_to_packet(build_spe_response(src, component.node_id_, payload, sizeof(payload))));
  }

  const auto result = component.scan_paired_devices();
  EXPECT_TRUE(result.success) << "a truncated scan is still a successful scan";
  EXPECT_NE(result.message.find("24 devices detected"), std::string::npos)
      << "the count must reflect what was actually kept";
  EXPECT_NE(result.message.find("truncated"), std::string::npos)
      << "the report itself must disclose the truncation, not just the log";
  EXPECT_EQ(radio.get_tx_configs().size(), 3u)
      << "a full array must not cut the scan short: skipping the remaining channels would reduce a "
         "large install to whichever devices happened to answer on CH2";
}

TEST(HubManagement, ScanPairedDevicesRetriesOnAllThreeChannels) {
  // A paired device only hears the roll-call if it happens to be awake on the channel the hub
  // transmits on, so the hub must give every channel a chance rather than transmitting once.
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);

  const auto result = component.scan_paired_devices();
  EXPECT_TRUE(result.success);

  ASSERT_EQ(radio.get_tx_configs().size(), 3u) << "one roll-call attempt per channel";
  EXPECT_EQ(radio.get_tx_configs()[0].freq_hz, FREQ_CH2) << "CH2 first: the protocol's designated TX channel";
  EXPECT_EQ(radio.get_tx_configs()[1].freq_hz, FREQ_CH1);
  EXPECT_EQ(radio.get_tx_configs()[2].freq_hz, FREQ_CH3);
  for (const auto &tx_config : radio.get_tx_configs())
    EXPECT_EQ(tx_config.preamble_len, LONG_PREAMBLE) << "each attempt must still wake a possibly-sleeping device";
}

TEST(HubManagement, ScanPairedDevicesKnownResponderReportedWithoutYamlSnippet) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);

  DeviceConfig cfg;
  cfg.type = DeviceType::HORIZONTAL_AWNING;
  component.add_device("30E1F2", cfg);

  const uint8_t payload[] = {0x04, 0x00, 0x30, 0xE1, 0xF2, 0x02, 0xCC, 0xFC, 0x03};
  const uint8_t src[3] = {0x30, 0xE1, 0xF2};
  radio.queue_rx(frame_to_packet(build_spe_response(src, component.node_id_, payload, sizeof(payload))));

  const auto result = component.scan_paired_devices();
  EXPECT_TRUE(result.success);
  EXPECT_NE(result.message.find("30E1F2"), std::string::npos);
  EXPECT_NE(result.message.find("horizontal_awning"), std::string::npos);
  EXPECT_NE(result.message.find("[known]"), std::string::npos);
  EXPECT_EQ(result.message.find("io_device_id"), std::string::npos) << "a known responder must not get a YAML snippet";
}

TEST(HubManagement, ScanPairedDevicesUnknownResponderGetsYamlSnippet) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);

  const uint8_t payload[] = {0x01, 0x80, 0x41, 0x5C, 0xE4, 0x02, 0xCC, 0x07, 0xEB};
  const uint8_t src[3] = {0x41, 0x5C, 0xE4};
  radio.queue_rx(frame_to_packet(build_spe_response(src, component.node_id_, payload, sizeof(payload))));

  const auto result = component.scan_paired_devices();
  EXPECT_TRUE(result.success);
  EXPECT_NE(result.message.find("[unknown]"), std::string::npos);
  EXPECT_NE(result.message.find("Paste this into your YAML to register it:"), std::string::npos);
  EXPECT_NE(result.message.find("platform: home_io_control"), std::string::npos);
  EXPECT_NE(result.message.find("io_device_id: \"415CE4\""), std::string::npos);
  EXPECT_NE(result.message.find("io_device_type: \"light\""), std::string::npos);
  EXPECT_NE(result.message.find("io_subtype: 0"), std::string::npos);
  EXPECT_EQ(component.get_device("415CE4"), nullptr) << "an unknown responder must not be auto-registered";
}

TEST(HubManagement, ScanPairedDevicesCarriesInversionAndRssiIntoTheReport) {
  // Each responder is decoded on arrival and copied field by field into a compact record, so
  // every field needs a test that would fail if the copy were dropped. Inversion and RSSI are the
  // two with no other coverage: inversion only shows up as a YAML line on an unknown cover, and
  // RSSI only in the summary line.
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  radio.set_last_capture_rssi(-73);

  // HORIZONTAL_AWNING (type 0x04) is the one type default_inverted_for_type() reports as
  // inverted, and it is unregistered here so the report includes its YAML block.
  const uint8_t payload[] = {0x04, 0x00, 0x30, 0xE1, 0xF2, 0x02, 0xCC, 0xFC, 0x03};
  const uint8_t src[3] = {0x30, 0xE1, 0xF2};
  radio.queue_rx(frame_to_packet(build_spe_response(src, component.node_id_, payload, sizeof(payload))));

  const auto result = component.scan_paired_devices();
  ASSERT_TRUE(result.success);
  EXPECT_NE(result.message.find("rssi=-73dBm"), std::string::npos)
      << "the reply's RSSI must reach the report, not a default";
  EXPECT_NE(result.message.find("invert_position: true"), std::string::npos)
      << "an inverted cover type must carry its inversion into the ready-to-paste YAML";
}

TEST(HubManagement, ScanPairedDevicesMixedKnownAndUnknownBothClassified) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);

  DeviceConfig cfg;
  cfg.type = DeviceType::HORIZONTAL_AWNING;
  component.add_device("30E1F2", cfg);

  const uint8_t known_payload[] = {0x04, 0x00, 0x30, 0xE1, 0xF2, 0x02, 0xCC, 0xFC, 0x03};
  const uint8_t known_src[3] = {0x30, 0xE1, 0xF2};
  radio.queue_rx(
      frame_to_packet(build_spe_response(known_src, component.node_id_, known_payload, sizeof(known_payload))));

  const uint8_t unknown_payload[] = {0x01, 0x80, 0x41, 0x5C, 0xE4, 0x02, 0xCC, 0x07, 0xEB};
  const uint8_t unknown_src[3] = {0x41, 0x5C, 0xE4};
  radio.queue_rx(
      frame_to_packet(build_spe_response(unknown_src, component.node_id_, unknown_payload, sizeof(unknown_payload))));

  const auto result = component.scan_paired_devices();
  EXPECT_TRUE(result.success);
  EXPECT_NE(result.message.find("2 devices detected (1 known, 1 unknown)"), std::string::npos);
  EXPECT_NE(result.message.find("30E1F2"), std::string::npos);
  EXPECT_NE(result.message.find("415CE4"), std::string::npos);
  EXPECT_NE(result.message.find("io_device_id: \"415CE4\""), std::string::npos);
  EXPECT_EQ(result.message.find("io_device_id: \"30E1F2\""), std::string::npos)
      << "the known device must not get a YAML snippet";

  // Known devices must be grouped under a "Known:" section before an "Unknown:" section, not
  // interleaved in arrival order — regardless of which one this run's radio happened to answer
  // first.
  const size_t known_header_pos = result.message.find("Known:\n");
  const size_t unknown_header_pos = result.message.find("Unknown:\n");
  ASSERT_NE(known_header_pos, std::string::npos);
  ASSERT_NE(unknown_header_pos, std::string::npos);
  EXPECT_LT(known_header_pos, unknown_header_pos) << "known section must come before the unknown section";
  EXPECT_LT(known_header_pos, result.message.find("30E1F2")) << "30E1F2 must be listed under Known:";
  EXPECT_LT(unknown_header_pos, result.message.find("415CE4")) << "415CE4 must be listed under Unknown:";
}

TEST(HubManagement, ScanPairedDevicesDedupsSameSourceThroughActionLayer) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);

  const uint8_t payload[] = {0x01, 0x80, 0x41, 0x5C, 0xE4, 0x02, 0xCC, 0x07, 0xEB};
  const uint8_t src[3] = {0x41, 0x5C, 0xE4};
  radio.queue_rx(frame_to_packet(build_spe_response(src, component.node_id_, payload, sizeof(payload))));
  radio.queue_rx(frame_to_packet(build_spe_response(src, component.node_id_, payload, sizeof(payload))));

  const auto result = component.scan_paired_devices();
  EXPECT_TRUE(result.success);
  EXPECT_NE(result.message.find("1 device detected"), std::string::npos) << "a duplicate reply must not double-count";

  // Count occurrences of the summary-line prefix specifically (not "415CE4" generally, which
  // also appears once more inside the unknown responder's io_device_id YAML line) to confirm
  // the device got exactly one report entry, not two.
  size_t occurrences = 0;
  size_t pos = 0;
  const std::string needle = "415CE4:";
  while ((pos = result.message.find(needle, pos)) != std::string::npos) {
    ++occurrences;
    pos += needle.size();
  }
  EXPECT_EQ(occurrences, 1u) << "the device should appear exactly once in the report";
}

TEST(HubManagement, ScanPairedDevicesTruncatedMetadataFallsBackToPlaceholderSnippet) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);

  // Shorter than DEVICE_METADATA_SIZE (2), so type/subtype stay UNKNOWN and
  // build_device_yaml_snippet() falls back to the placeholder platform form.
  const uint8_t payload[] = {0x04};
  const uint8_t src[3] = {0x99, 0x88, 0x77};
  radio.queue_rx(frame_to_packet(build_spe_response(src, component.node_id_, payload, sizeof(payload))));

  const auto result = component.scan_paired_devices();
  EXPECT_TRUE(result.success);
  EXPECT_NE(result.message.find("<cover|light|switch|lock>:"), std::string::npos)
      << "truncated metadata should fall back to the placeholder snippet form";
  EXPECT_NE(result.message.find("io_device_id: \"998877\""), std::string::npos);
}

TEST(HubManagement, ScanPairedDevicesNeverWritesRegistry) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);

  const size_t size_before = component.registry_.size();

  const uint8_t payload[] = {0x01, 0x80, 0x41, 0x5C, 0xE4, 0x02, 0xCC, 0x07, 0xEB};
  const uint8_t src[3] = {0x41, 0x5C, 0xE4};
  radio.queue_rx(frame_to_packet(build_spe_response(src, component.node_id_, payload, sizeof(payload))));

  const auto result = component.scan_paired_devices();
  EXPECT_TRUE(result.success);
  EXPECT_EQ(component.registry_.size(), size_before) << "an unknown responder must not be auto-registered";
  EXPECT_EQ(component.get_device("415CE4"), nullptr);
}

TEST(HubManagement, ScanPairedDevicesZeroArgDispatchExecutes) {
  esphome::api::APIServer api_server;
  esphome::api::ScopedGlobalApiServer scoped_api_server(api_server);
  api_server.reset();

  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.register_management_actions_();
  ASSERT_EQ(api_server.user_services_.size(), 4u);

  const auto response = api_server.user_services_[3]->encode_list_service_response();
  EXPECT_EQ(response.name.str(), "scan_paired_devices");
  ASSERT_EQ(response.args.size(), 0u);

  esphome::api::ExecuteServiceRequest request;
  request.key = response.key;
  request.args.init(0);

  EXPECT_TRUE(api_server.user_services_[3]->execute_service(request))
      << "a zero-arg request must dispatch since request.args.size() == arg_names_.size() == 0";
  EXPECT_EQ(api_server.events_.size(), 1u) << "dispatch should have run and published a result event";
}

TEST(HubManagement, ApiScanPairedDevicesPublishesManagementResultEvent) {
  esphome::api::APIServer api_server;
  esphome::api::ScopedGlobalApiServer scoped_api_server(api_server);
  api_server.reset();

  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);

  component.api_scan_paired_devices_();

  ASSERT_EQ(api_server.events_.size(), 1u);
  const auto &event = api_server.events_.front();
  EXPECT_EQ(event.event_type, "esphome.home_io_control_action_result");
  EXPECT_EQ(event.data.at("action"), "scan_paired_devices");
  EXPECT_EQ(event.data.at("device_id"), "");
  EXPECT_EQ(event.data.at("success"), "true");
  EXPECT_EQ(event.data.at("verified"), "false");
  EXPECT_FALSE(event.data.at("message").empty());
}

TEST(HubManagement, ApiScanPairedDevicesFailurePublishesEventWithEmptyDeviceId) {
  // scan_paired_devices() has no single target, so its ManagementActionResult::device_id is
  // always empty on both the success and failure paths — publish_result()'s log line must
  // handle that without a dangling "for device :" (host tests can't observe ESP_LOG output,
  // since IOHOME_HOST_LOG_STUB discards its arguments, so this asserts on the event/result
  // fields the log line is built from instead).
  esphome::api::APIServer api_server;
  esphome::api::ScopedGlobalApiServer scoped_api_server(api_server);
  api_server.reset();

  TestableManagementComponent component;  // deliberately not setup_component()'d: not initialized

  component.api_scan_paired_devices_();

  ASSERT_EQ(api_server.events_.size(), 1u);
  const auto &event = api_server.events_.front();
  EXPECT_EQ(event.event_type, "esphome.home_io_control_action_result");
  EXPECT_EQ(event.data.at("action"), "scan_paired_devices");
  EXPECT_EQ(event.data.at("device_id"), "") << "scan action never has a single target device";
  EXPECT_EQ(event.data.at("success"), "false");
  EXPECT_EQ(event.data.at("message"), "hub is not initialized");
}
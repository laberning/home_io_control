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
  using IOHomeControlComponent::initialized_;
  using IOHomeControlComponent::node_id_;
  using IOHomeControlComponent::op_queue_;
  using IOHomeControlComponent::radio_;
  using IOHomeControlComponent::register_management_actions_;
  using IOHomeControlComponent::system_key_;
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
  component.add_device("ABC123");
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

TEST(HubManagement, RegisterManagementActionsRegistersAllThreeServices) {
  esphome::api::APIServer api_server;
  esphome::api::ScopedGlobalApiServer scoped_api_server(api_server);
  api_server.reset();

  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.register_management_actions_();

  ASSERT_EQ(esphome::api::global_api_server->user_services_.size(), 3u);

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
  ASSERT_EQ(api_server.user_services_.size(), 3u);

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
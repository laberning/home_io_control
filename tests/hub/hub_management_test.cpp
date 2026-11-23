/// @file hub_management_test.cpp
/// @brief Tests for hub_management.cpp rename action support.

#include "hub_core.h"
#include "log_helpers.h"
#include "proto_commands.h"
#include "proto_crypto.h"
#include "proto_frame.h"

#include "esphome/components/api/custom_api_device.h"

#include "corpus_generated.h"
#include "corpus_test_helpers.h"
#include "log_capture.h"
#include "management_test_fixture.h"
#include "test_helpers.h"
#include "stubs/radio_test_common.h"

#include <array>
#include <cstring>
#include <algorithm>
#include <vector>

using namespace esphome::home_io_control;
using namespace management_test;

namespace {

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

  const auto result = component.management_actions_.rename_device("123456", "Patio Awning");
  EXPECT_FALSE(result.success);
  EXPECT_FALSE(result.verified);
  EXPECT_EQ(result.message, "device is not registered on this hub");
}

TEST(HubManagement, RenameDeviceRejectsWhenHubNotInitialized) {
  TestableManagementComponent component;

  const auto result = component.management_actions_.rename_device("ABC123", "Patio Awning");
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "hub is not initialized");
}

TEST(HubManagement, RenameDeviceRejectsInvalidDeviceId) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);

  const auto result = component.management_actions_.rename_device("12", "Patio Awning");
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "device ID must be exactly 6 hexadecimal characters");
}

TEST(HubManagement, RenameDeviceRejectsInvalidName) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);

  const auto result = component.management_actions_.rename_device("ABC123", "");
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, device_name_validation_error_description(DeviceNameValidationError::EMPTY));
}

TEST(HubManagement, RenameDeviceSuccessVerifiesReadbackAndCachesName) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  radio.queue_rx(frame_to_packet(make_set_name_response(component.node_id_)));
  radio.queue_rx(frame_to_packet(make_get_name_response(component.node_id_, "Patio Awning")));

  const auto result = component.management_actions_.rename_device("ABC123", "Patio Awning");
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

  const auto result = component.management_actions_.rename_device("ABC123", "Patio Awning");
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

  const auto result = component.management_actions_.rename_device("ABC123", "Patio Awning");
  EXPECT_TRUE(result.success);
  EXPECT_FALSE(result.verified);
  EXPECT_EQ(result.message, "rename acknowledged but verification readback failed");
}

TEST(HubManagement, RenameDeviceExplicitErrorReturnsDecodedResultCode) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  radio.queue_rx(frame_to_packet(make_error_response(component.node_id_, RESULT_LIMITATION_BY_WIND)));

  const auto result = component.management_actions_.rename_device("ABC123", "Patio Awning");
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

  const auto result = component.management_actions_.identify_device("123456", IDENTIFY_WINK_DEVICE_DEFAULT);
  EXPECT_FALSE(result.success);
  EXPECT_FALSE(result.verified);
  EXPECT_EQ(result.message, "device is not registered on this hub");
}

TEST(HubManagement, IdentifyDeviceRejectsWhenHubNotInitialized) {
  TestableManagementComponent component;

  const auto result = component.management_actions_.identify_device("ABC123", IDENTIFY_WINK_DEVICE_DEFAULT);
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "hub is not initialized");
}

TEST(HubManagement, IdentifyDeviceRejectsInvalidDeviceId) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);

  const auto result = component.management_actions_.identify_device("12", IDENTIFY_WINK_DEVICE_DEFAULT);
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "device ID must be exactly 6 hexadecimal characters");
}

TEST(HubManagement, IdentifyDeviceNoResponseFails) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  // No queued RX at all: send_and_receive times out with no valid response.

  const auto result = component.management_actions_.identify_device("ABC123", IDENTIFY_WINK_DEVICE_DEFAULT);
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "no valid response to identify request");
}

TEST(HubManagement, IdentifyDeviceSuccessWithDirectResponse) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  radio.queue_rx(frame_to_packet(make_identify_ack_response(component.node_id_)));

  const auto result = component.management_actions_.identify_device("ABC123", IDENTIFY_WINK_DEVICE_DEFAULT);
  EXPECT_TRUE(result.success);
  EXPECT_FALSE(result.verified);
  EXPECT_EQ(result.message, "identify acknowledged by device");
}

TEST(HubManagement, IdentifyDeviceErrorResponseIsTreatedAsSuccess) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  radio.queue_rx(frame_to_packet(make_error_response(component.node_id_, RESULT_LIMITATION_BY_WIND)));

  const auto result = component.management_actions_.identify_device("ABC123", IDENTIFY_WINK_DEVICE_DEFAULT);
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

  component.management_actions_.api_identify_device("ABC123");

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

// The identify wink time is byte 10 of the sent frame: 8 header bytes, cmd 0x1E, originator, wink time.
static constexpr size_t IDENTIFY_WINK_TIME_OFFSET = 10;

TEST(HubManagement, IdentifyDeviceSendsTheDeviceDefaultWinkTime) {
  esphome::api::APIServer api_server;
  esphome::api::ScopedGlobalApiServer scoped_api_server(api_server);
  api_server.reset();
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  radio.queue_rx(frame_to_packet(make_identify_ack_response(component.node_id_)));

  component.management_actions_.api_identify_device("ABC123");

  ASSERT_FALSE(radio.get_sent_data().empty());
  const auto &sent = radio.get_sent_data()[0];
  ASSERT_EQ(sent[8], CMD_IDENTIFY);
  EXPECT_EQ(sent[IDENTIFY_WINK_TIME_OFFSET], IDENTIFY_WINK_DEVICE_DEFAULT) << "the default must be bounded, never 255";
}

TEST(HubManagement, IdentifyDeviceForSendsTheRequestedSeconds) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  radio.queue_rx(frame_to_packet(make_identify_ack_response(component.node_id_)));

  const auto result = component.management_actions_.identify_device_for("ABC123", " 253 ");

  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.action, "identify_device_for");
  EXPECT_EQ(result.message, "identify acknowledged by device");
  ASSERT_FALSE(radio.get_sent_data().empty());
  EXPECT_EQ(radio.get_sent_data()[0][IDENTIFY_WINK_TIME_OFFSET], 253);
}

TEST(HubManagement, IdentifyDeviceForZeroStopsARunningIdentify) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  radio.queue_rx(frame_to_packet(make_identify_ack_response(component.node_id_)));

  const auto result = component.management_actions_.identify_device_for("ABC123", "0");

  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.message, "identify stopped");
  ASSERT_FALSE(radio.get_sent_data().empty());
  EXPECT_EQ(radio.get_sent_data()[0][IDENTIFY_WINK_TIME_OFFSET], IDENTIFY_WINK_STOP);
}

TEST(HubManagement, IdentifyDeviceForRefusesOutOfRangeSecondsWithoutSending) {
  for (const char *bad : {"254", "255", "256", "-1", "abc", "5.5", "", "  ", "1000", "99999999999999999999"}) {
    TestableManagementComponent component;
    MockRadio radio;
    setup_component(component, radio);

    const auto result = component.management_actions_.identify_device_for("ABC123", bad);

    EXPECT_FALSE(result.success) << "seconds '" << bad << "' must be refused";
    EXPECT_EQ(result.message, "seconds must be a whole number between 0 and 253 (0 stops a running identify)");
    EXPECT_TRUE(radio.get_sent_data().empty()) << "a refused value must not reach the radio";
  }
}

TEST(HubManagement, IdentifyDeviceForRejectsUnknownDevice) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);

  const auto result = component.management_actions_.identify_device_for("123456", "5");

  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.action, "identify_device_for");
  EXPECT_TRUE(radio.get_sent_data().empty());
}

TEST(HubManagement, ApiIdentifyDeviceForPublishesResultEventUnderItsOwnActionName) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  radio.queue_rx(frame_to_packet(make_identify_ack_response(component.node_id_)));
  esphome::api::APIServer api_server;
  esphome::api::ScopedGlobalApiServer scoped_api_server(api_server);
  api_server.reset();

  component.management_actions_.api_identify_device_for("ABC123", "3");

  ASSERT_EQ(api_server.events_.size(), 1u);
  EXPECT_EQ(api_server.events_[0].data.at("action"), "identify_device_for");
}

TEST(ManagementHelpers, ParseIdentifySecondsAcceptsZeroThroughMaxOnly) {
  uint8_t wink = 99;
  EXPECT_TRUE(parse_identify_seconds("0", wink));
  EXPECT_EQ(wink, 0);
  EXPECT_TRUE(parse_identify_seconds("1", wink));
  EXPECT_EQ(wink, 1);
  EXPECT_TRUE(parse_identify_seconds("253", wink));
  EXPECT_EQ(wink, 253);
  EXPECT_TRUE(parse_identify_seconds(" 7\t", wink));
  EXPECT_EQ(wink, 7);

  wink = 99;
  for (const char *bad : {"254", "255", "-1", "+5", "abc", "5.5", "", "1e2", "0x10", "99999999999999999999"}) {
    EXPECT_FALSE(parse_identify_seconds(bad, wink)) << "'" << bad << "' must be refused";
    EXPECT_EQ(wink, 99) << "a refused value leaves the output untouched";
  }
}

TEST(HubManagement, ForceOpenDeviceRejectsUnknownDevice) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);

  const auto result = component.management_actions_.force_open_device("123456");
  EXPECT_FALSE(result.success);
  EXPECT_FALSE(result.verified);
  EXPECT_EQ(result.message, "device is not registered on this hub");
}

TEST(HubManagement, ForceOpenDeviceRejectsWhenHubNotInitialized) {
  TestableManagementComponent component;

  const auto result = component.management_actions_.force_open_device("ABC123");
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "hub is not initialized");
}

TEST(HubManagement, ForceOpenDeviceRejectsInvalidDeviceId) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);

  const auto result = component.management_actions_.force_open_device("12");
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "device ID must be exactly 6 hexadecimal characters");
}

TEST(HubManagement, ForceOpenDeviceEnqueuesForceOpenCommand) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);

  const auto result = component.management_actions_.force_open_device("ABC123");
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
  test::set_kind(*dev, DeviceType::LIGHT);

  const auto result = component.management_actions_.force_open_device("ABC123");
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

  component.management_actions_.api_force_open_device("ABC123");

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

TEST(HubManagement, RegisterManagementActionsRegistersOnlyElevenServicesByDefault) {
  // diagnostic_probes_enabled_ defaults to false (no set_diagnostic_probes_enabled() call), so
  // probe_device/probe_sweep must not appear in the action list at all -- this is what keeps a
  // default build's Home Assistant action list clean, not just what refuses at call time.
  esphome::api::APIServer api_server;
  esphome::api::ScopedGlobalApiServer scoped_api_server(api_server);
  api_server.reset();

  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.register_management_actions_();

  ASSERT_EQ(esphome::api::global_api_server->user_services_.size(), 11u);
  for (const auto &service : esphome::api::global_api_server->user_services_) {
    const auto name = service->encode_list_service_response().name.str();
    EXPECT_NE(name, "probe_device");
    EXPECT_NE(name, "probe_sweep");
  }

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

  const auto identify_for_response = esphome::api::global_api_server->user_services_[2]->encode_list_service_response();
  EXPECT_EQ(identify_for_response.name.str(), "identify_device_for");
  ASSERT_EQ(identify_for_response.args.size(), 2u);
  EXPECT_EQ(identify_for_response.args[0].name.str(), "device_id");
  EXPECT_EQ(identify_for_response.args[1].name.str(), "seconds");
  EXPECT_EQ(identify_for_response.args[1].type, esphome::api::enums::SERVICE_ARG_TYPE_STRING);

  const auto force_open_response = esphome::api::global_api_server->user_services_[3]->encode_list_service_response();
  EXPECT_EQ(force_open_response.name.str(), "force_open_device");
  EXPECT_EQ(force_open_response.supports_response, esphome::api::enums::SUPPORTS_RESPONSE_NONE);
  ASSERT_EQ(force_open_response.args.size(), 1u);
  EXPECT_EQ(force_open_response.args[0].name.str(), "device_id");
  EXPECT_EQ(force_open_response.args[0].type, esphome::api::enums::SERVICE_ARG_TYPE_STRING);

  const auto scan_response = esphome::api::global_api_server->user_services_[4]->encode_list_service_response();
  EXPECT_EQ(scan_response.name.str(), "scan_paired_devices");
  EXPECT_EQ(scan_response.supports_response, esphome::api::enums::SUPPORTS_RESPONSE_NONE);
  EXPECT_EQ(scan_response.args.size(), 0u) << "scan_paired_devices takes no arguments";

  const auto oneway_set_position_response =
      esphome::api::global_api_server->user_services_[5]->encode_list_service_response();
  EXPECT_EQ(oneway_set_position_response.name.str(), "oneway_set_position");
  EXPECT_EQ(oneway_set_position_response.supports_response, esphome::api::enums::SUPPORTS_RESPONSE_NONE);
  ASSERT_EQ(oneway_set_position_response.args.size(), 2u);
  EXPECT_EQ(oneway_set_position_response.args[0].name.str(), "controller_id");
  EXPECT_EQ(oneway_set_position_response.args[0].type, esphome::api::enums::SERVICE_ARG_TYPE_STRING);
  EXPECT_EQ(oneway_set_position_response.args[1].name.str(), "position");
  EXPECT_EQ(oneway_set_position_response.args[1].type, esphome::api::enums::SERVICE_ARG_TYPE_STRING);

  const auto oneway_remove_controller_response =
      esphome::api::global_api_server->user_services_[6]->encode_list_service_response();
  EXPECT_EQ(oneway_remove_controller_response.name.str(), "oneway_remove_controller");
  EXPECT_EQ(oneway_remove_controller_response.supports_response, esphome::api::enums::SUPPORTS_RESPONSE_NONE);
  ASSERT_EQ(oneway_remove_controller_response.args.size(), 1u);
  EXPECT_EQ(oneway_remove_controller_response.args[0].name.str(), "controller_id");
  EXPECT_EQ(oneway_remove_controller_response.args[0].type, esphome::api::enums::SERVICE_ARG_TYPE_STRING);

  const auto oneway_set_sequence_response =
      esphome::api::global_api_server->user_services_[7]->encode_list_service_response();
  EXPECT_EQ(oneway_set_sequence_response.name.str(), "oneway_set_sequence");
  EXPECT_EQ(oneway_set_sequence_response.supports_response, esphome::api::enums::SUPPORTS_RESPONSE_NONE);
  ASSERT_EQ(oneway_set_sequence_response.args.size(), 3u);
  EXPECT_EQ(oneway_set_sequence_response.args[0].name.str(), "controller_id");
  EXPECT_EQ(oneway_set_sequence_response.args[1].name.str(), "sequence");
  EXPECT_EQ(oneway_set_sequence_response.args[2].name.str(), "allow_lower");
  for (const auto &arg : oneway_set_sequence_response.args)
    EXPECT_EQ(arg.type, esphome::api::enums::SERVICE_ARG_TYPE_STRING);

  const auto heating_control_response =
      esphome::api::global_api_server->user_services_[8]->encode_list_service_response();
  EXPECT_EQ(heating_control_response.name.str(), "heating_control");
  EXPECT_EQ(heating_control_response.supports_response, esphome::api::enums::SUPPORTS_RESPONSE_NONE);
  ASSERT_EQ(heating_control_response.args.size(), 3u);
  EXPECT_EQ(heating_control_response.args[0].name.str(), "device_id");
  EXPECT_EQ(heating_control_response.args[0].type, esphome::api::enums::SERVICE_ARG_TYPE_STRING);
  EXPECT_EQ(heating_control_response.args[1].name.str(), "function");
  EXPECT_EQ(heating_control_response.args[1].type, esphome::api::enums::SERVICE_ARG_TYPE_STRING);
  EXPECT_EQ(heating_control_response.args[2].name.str(), "value");
  EXPECT_EQ(heating_control_response.args[2].type, esphome::api::enums::SERVICE_ARG_TYPE_STRING);

  const auto pair_device_response = esphome::api::global_api_server->user_services_[9]->encode_list_service_response();
  EXPECT_EQ(pair_device_response.name.str(), "pair_device");
  EXPECT_EQ(pair_device_response.supports_response, esphome::api::enums::SUPPORTS_RESPONSE_NONE);
  ASSERT_EQ(pair_device_response.args.size(), 1u);
  EXPECT_EQ(pair_device_response.args[0].name.str(), "device_id");
  EXPECT_EQ(pair_device_response.args[0].type, esphome::api::enums::SERVICE_ARG_TYPE_STRING);

  const auto pull_key_response = esphome::api::global_api_server->user_services_[10]->encode_list_service_response();
  EXPECT_EQ(pull_key_response.name.str(), "pull_key");
  EXPECT_EQ(pull_key_response.supports_response, esphome::api::enums::SUPPORTS_RESPONSE_NONE);
  ASSERT_EQ(pull_key_response.args.size(), 1u);
  EXPECT_EQ(pull_key_response.args[0].name.str(), "controller_id");
  EXPECT_EQ(pull_key_response.args[0].type, esphome::api::enums::SERVICE_ARG_TYPE_STRING);
}

TEST(HubManagement, RegisterManagementActionsRegistersAllThirteenServicesWhenDiagnosticProbesEnabled) {
  esphome::api::APIServer api_server;
  esphome::api::ScopedGlobalApiServer scoped_api_server(api_server);
  api_server.reset();

  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.set_diagnostic_probes_enabled(true);
  component.register_management_actions_();

  ASSERT_EQ(esphome::api::global_api_server->user_services_.size(), 13u);

  const auto probe_device_response =
      esphome::api::global_api_server->user_services_[11]->encode_list_service_response();
  EXPECT_EQ(probe_device_response.name.str(), "probe_device");
  EXPECT_EQ(probe_device_response.supports_response, esphome::api::enums::SUPPORTS_RESPONSE_NONE);
  ASSERT_EQ(probe_device_response.args.size(), 3u);
  EXPECT_EQ(probe_device_response.args[0].name.str(), "device_id");
  EXPECT_EQ(probe_device_response.args[0].type, esphome::api::enums::SERVICE_ARG_TYPE_STRING);
  EXPECT_EQ(probe_device_response.args[1].name.str(), "probe");
  EXPECT_EQ(probe_device_response.args[1].type, esphome::api::enums::SERVICE_ARG_TYPE_STRING);
  EXPECT_EQ(probe_device_response.args[2].name.str(), "index");
  EXPECT_EQ(probe_device_response.args[2].type, esphome::api::enums::SERVICE_ARG_TYPE_STRING);

  const auto probe_sweep_response = esphome::api::global_api_server->user_services_[12]->encode_list_service_response();
  EXPECT_EQ(probe_sweep_response.name.str(), "probe_sweep");
  EXPECT_EQ(probe_sweep_response.supports_response, esphome::api::enums::SUPPORTS_RESPONSE_NONE);
  ASSERT_EQ(probe_sweep_response.args.size(), 4u);
  EXPECT_EQ(probe_sweep_response.args[0].name.str(), "device_id");
  EXPECT_EQ(probe_sweep_response.args[1].name.str(), "probe");
  EXPECT_EQ(probe_sweep_response.args[2].name.str(), "first_index");
  EXPECT_EQ(probe_sweep_response.args[3].name.str(), "last_index");
}

// ========================================================================================
// heating_control action (CMD_WRITE_PRIVATE 0x20)
// ========================================================================================

TEST(HubManagement, HeatingControlRejectsUnknownDevice) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  const auto result = component.management_actions_.heating_control("123456", "read_paired_devices", "");
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "device is not registered on this hub");
}

TEST(HubManagement, HeatingControlRejectsWhenHubNotInitialized) {
  TestableManagementComponent component;
  const auto result = component.management_actions_.heating_control("ABC123", "read_paired_devices", "");
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "hub is not initialized");
}

TEST(HubManagement, HeatingControlRejectsInvalidDeviceId) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  const auto result = component.management_actions_.heating_control("12", "read_paired_devices", "");
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "device ID must be exactly 6 hexadecimal characters");
}

// The override takes the same refusals on every model of a heater that has a programme.
TEST(HubManagement, HeatingControlSetDerogatedSetpointRefusesABadValueOnEveryModelWithAProgramme) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  make_climate_device(component);
  IoDevice *dev = component.get_device("ABC123");

  for (const DeviceModel model : {DeviceModel::NONE, DeviceModel::TOWEL_DRYER}) {
    test::set_kind(*dev, DeviceType::ELECTRICAL_HEATER, 1);
    dev->model = model;
    auto result = component.management_actions_.heating_control("ABC123", "set_derogated_setpoint", "5");
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.message, "temperature must be between 7.0 and 28.0");
    result = component.management_actions_.heating_control("ABC123", "set_derogated_setpoint", "warm");
    EXPECT_FALSE(result.success);
  }
  EXPECT_TRUE(radio.get_sent_data().empty());
}

TEST(HubManagement, HeatingControlSurfacesErrorResponseResultCode) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  make_climate_device(component);
  radio.queue_rx(frame_to_packet(make_error_response(component.node_id_, RESULT_LIMITATION_BY_WIND)));

  const auto result = component.management_actions_.heating_control("ABC123", "set_comfort_setpoint", "21.0");
  EXPECT_FALSE(result.success);
  EXPECT_TRUE(result.has_result_code);
  EXPECT_EQ(result.result_code, RESULT_LIMITATION_BY_WIND);
  EXPECT_EQ(result.message, std::string(command_result_name(RESULT_LIMITATION_BY_WIND)) + ": " +
                                command_result_description(RESULT_LIMITATION_BY_WIND));
}

TEST(HubManagement, ApiHeatingControlPublishesManagementResultEvent) {
  esphome::api::APIServer api_server;
  esphome::api::ScopedGlobalApiServer scoped_api_server(api_server);
  api_server.reset();

  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  make_climate_device(component);
  radio.queue_rx(
      frame_to_packet(test::make_device_reply(component.node_id_, CMD_WRITE_PRIVATE_ACK, {}, test::DEVICE_ABC123_ID)));

  component.management_actions_.api_heating_control("ABC123", "set_operating_mode", "auto");

  ASSERT_EQ(api_server.events_.size(), 1u);
  const auto &event = api_server.events_.front();
  EXPECT_EQ(event.event_type, "esphome.home_io_control_action_result");
  EXPECT_EQ(event.data.at("action"), "heating_control");
  EXPECT_EQ(event.data.at("device_id"), "ABC123");
  EXPECT_EQ(event.data.at("success"), "true");
  EXPECT_EQ(event.data.at("verified"), "false");
  EXPECT_EQ(event.data.at("message"), "heating operating_mode acknowledged by device");
}

TEST(HubManagement, ApiHeatingControlRetiredNameAddsTheNoticeToTheResultEvent) {
  esphome::api::APIServer api_server;
  esphome::api::ScopedGlobalApiServer scoped_api_server(api_server);
  api_server.reset();

  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  make_climate_device(component);
  radio.queue_rx(
      frame_to_packet(test::make_device_reply(component.node_id_, CMD_WRITE_PRIVATE_ACK, {}, test::DEVICE_ABC123_ID)));

  component.management_actions_.api_heating_control("ABC123", "set_mode", "auto");

  ASSERT_EQ(api_server.events_.size(), 1u);
  EXPECT_EQ(api_server.events_.front().data.at("success"), "true");
  EXPECT_EQ(api_server.events_.front().data.at("message"),
            "heating operating_mode acknowledged by device; heating_control function 'set_mode' is deprecated and "
            "stops working after January 2027; use 'set_operating_mode'");
}

// A retired name adds exactly one warning (the deprecation notice, ADR 0109) to what the same call under the new
// name logs: once on an accepted write, once on a refused value (whose failed result logs a warning of its own).
TEST(HubManagement, HeatingControlRetiredNameAddsExactlyOneWarningPerCall) {
  esphome::api::APIServer api_server;
  esphome::api::ScopedGlobalApiServer scoped_api_server(api_server);
  api_server.reset();

  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  make_climate_device(component);
  test::LogCapture log;
  const auto warnings_of = [&](const char *function, const char *value, bool acknowledged) {
    if (acknowledged)
      radio.queue_rx(frame_to_packet(
          test::make_device_reply(component.node_id_, CMD_WRITE_PRIVATE_ACK, {}, test::DEVICE_ABC123_ID)));
    log.clear();
    component.management_actions_.api_heating_control("ABC123", function, value);
    return log.count('W');
  };

  EXPECT_EQ(warnings_of("set_mode", "auto", true), warnings_of("set_operating_mode", "auto", true) + 1)
      << "an accepted write";
  EXPECT_EQ(warnings_of("set_temperature", "99", false), warnings_of("set_comfort_setpoint", "99", false) + 1)
      << "a refused value";

  ASSERT_EQ(api_server.events_.size(), 4u);
  EXPECT_EQ(api_server.events_[0].data.at("success"), "true");
  EXPECT_EQ(api_server.events_[1].data.at("success"), "true");
  EXPECT_EQ(api_server.events_[2].data.at("success"), "false");
  EXPECT_EQ(api_server.events_[3].data.at("success"), "false");
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

  component.management_actions_.api_rename_device("ABC123", "Patio Awning");

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

  component.management_actions_.api_rename_device("ABC123", "Patio Awning");
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
  ASSERT_EQ(api_server.user_services_.size(), 11u);

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
// tests/corpus/captures/discovery/somfy_awning_discovery_spe_paired_rollcall.yaml and
// tests/corpus/captures/discovery/somfy_izymo_dimmer_discovery_spe_paired_rollcall.yaml:
// awning "04 00 30 E1 F2 02 CC FC 03" -> HORIZONTAL_AWNING/0, backbone 30E1F2, MANUFACTURER_SOMFY;
// dimmer "01 80 41 5C E4 02 CC 07 EB" -> LIGHT/0, backbone 415CE4, MANUFACTURER_SOMFY. Using real
// bytes here and in the corpus replay tests (tests/corpus/corpus_spe_rollcall_replay_test.cpp) means
// both tell the same story.

TEST(HubManagement, ScanPairedDevicesRejectsWhenHubNotInitialized) {
  TestableManagementComponent component;

  const auto result = component.management_actions_.scan_paired_devices();
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "hub is not initialized");
}

TEST(HubManagement, ScanPairedDevicesZeroRepliesReportsZeroAndSucceeds) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);

  const auto result = component.management_actions_.scan_paired_devices();
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

  const auto result = component.management_actions_.scan_paired_devices();
  EXPECT_TRUE(result.success) << "a truncated scan is still a successful scan";
  EXPECT_NE(result.message.find("24 devices detected"), std::string::npos)
      << "the count must reflect what was actually kept";
  EXPECT_NE(result.message.find("truncated"), std::string::npos)
      << "the report itself must disclose the truncation, not just the log";
  EXPECT_EQ(radio.get_tx_configs().size(), 6u)
      << "a full array must not cut the scan short: skipping the remaining channels/passes would "
         "reduce a large install to whichever devices happened to answer first";
}

TEST(HubManagement, ScanPairedDevicesSweepsBothPowerClasses) {
  // A paired device only hears the roll-call if it happens to be awake on the channel the hub
  // transmits on, so the hub must give every channel a chance — twice, once per power class,
  // low-power pass first — rather than transmitting once.
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);

  const auto result = component.management_actions_.scan_paired_devices();
  EXPECT_TRUE(result.success);

  ASSERT_EQ(radio.get_sent_data().size(), 6u) << "two passes, three channels each";
  ASSERT_EQ(radio.get_tx_configs().size(), 6u);

  // Attempts 1-3: low-power pass, CTRL1 = LOW_POWER | ACK, MOCK_WAKE_PREAMBLE.
  const uint32_t expected_freq[] = {FREQ_CH2, FREQ_CH1, FREQ_CH3, FREQ_CH2, FREQ_CH1, FREQ_CH3};
  for (uint8_t i = 0; i < 3; i++) {
    IoFrame frame{};
    ASSERT_TRUE(parse(radio.get_sent_data()[i].data(), static_cast<uint8_t>(radio.get_sent_data()[i].size()), frame));
    EXPECT_EQ(frame.ctrl1, static_cast<uint8_t>(CTRL1_LOW_POWER | CTRL1_ACK))
        << "attempt " << (i + 1) << ": low-power pass must set LOW_POWER and ACK";
    EXPECT_EQ(radio.get_tx_configs()[i].freq_hz, expected_freq[i]) << "attempt " << (i + 1);
    EXPECT_EQ(radio.get_tx_configs()[i].preamble_len, MOCK_WAKE_PREAMBLE)
        << "attempt " << (i + 1) << ": LOW_POWER selects MOCK_WAKE_PREAMBLE via request_preamble_for()'s rule";
  }
  // Attempts 4-6: always-alive pass, CTRL1 = 0x00, `start_preamble_ms`.
  for (uint8_t i = 3; i < 6; i++) {
    IoFrame frame{};
    ASSERT_TRUE(parse(radio.get_sent_data()[i].data(), static_cast<uint8_t>(radio.get_sent_data()[i].size()), frame));
    EXPECT_EQ(frame.ctrl1, 0) << "attempt " << (i + 1) << ": always-alive pass sets neither LOW_POWER nor ACK";
    EXPECT_EQ(radio.get_tx_configs()[i].freq_hz, expected_freq[i]) << "attempt " << (i + 1);
    EXPECT_EQ(radio.get_tx_configs()[i].preamble_len, mock_preamble_bytes(component.tuning_.start_preamble_ms))
        << "attempt " << (i + 1);
  }
}

namespace {

/// A key a device keeps of its own (the test value the key-pull tests use).
constexpr uint8_t SCAN_DEVICE_KEY[AES_KEY_SIZE] = {0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE,
                                                   0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};

/// Whether the 0x2A in `raw` carries an HMAC that verifies under `key` (nonce in data[0..5], HMAC in data[6..11]).
bool roll_call_is_signed_with(const std::vector<uint8_t> &raw, const uint8_t *key) {
  IoFrame frame{};
  if (!parse(raw.data(), static_cast<uint8_t>(raw.size()), frame) || frame.data_len < 2 * HMAC_SIZE)
    return false;
  uint8_t expected[HMAC_SIZE];
  const uint8_t command = CMD_DISCOVER_SPE_REQ;
  return crypto::create_hmac(&command, 1, frame.data, key, expected) &&
         memcmp(expected, frame.data + HMAC_SIZE, HMAC_SIZE) == 0;
}

/// Register a device with a key of its own.
void add_keyed_device(TestableManagementComponent &component, const char *id, const uint8_t *key) {
  DeviceConfig cfg;
  cfg.type = DeviceType::ELECTRICAL_HEATER;
  cfg.subtype = 1;
  cfg.has_system_key = true;
  memcpy(cfg.system_key, key, AES_KEY_SIZE);
  component.add_device(id, cfg);
}

}  // namespace

TEST(HubManagement, ScanPairedDevicesSweepsOncePerDistinctKey) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  add_keyed_device(component, "AABB01", SCAN_DEVICE_KEY);
  add_keyed_device(component, "AABB02", SCAN_DEVICE_KEY);        // same key: no sweep of its own
  add_keyed_device(component, "AABB03", component.system_key_);  // equal to the hub key: no sweep either

  const auto result = component.management_actions_.scan_paired_devices();
  ASSERT_TRUE(result.success);
  ASSERT_EQ(radio.get_sent_data().size(), 12u) << "6 attempts for the hub key, 6 for the one other key";
  for (size_t i = 0; i < 12; i++) {
    const uint8_t *expected_key = i < 6 ? component.system_key_ : SCAN_DEVICE_KEY;
    const uint8_t *other_key = i < 6 ? SCAN_DEVICE_KEY : component.system_key_;
    EXPECT_TRUE(roll_call_is_signed_with(radio.get_sent_data()[i], expected_key)) << "attempt " << (i + 1);
    EXPECT_FALSE(roll_call_is_signed_with(radio.get_sent_data()[i], other_key)) << "attempt " << (i + 1);
  }
}

TEST(HubManagement, ScanPairedDevicesWithoutPerDeviceKeysSweepsTheHubKeyOnly) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);

  ASSERT_TRUE(component.management_actions_.scan_paired_devices().success);
  ASSERT_EQ(radio.get_sent_data().size(), 6u);
  EXPECT_TRUE(roll_call_is_signed_with(radio.get_sent_data()[0], component.system_key_));
}

TEST(HubManagement, ScanPairedDevicesNamesTheKeyOfAResponderFoundUnderAPerDeviceKey) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  add_keyed_device(component, "AABB01", SCAN_DEVICE_KEY);

  // Silent through the six hub-key attempts, then a registered keyed device answers the seventh.
  radio.queue_rx_hold_until_sent(7);
  const uint8_t payload[] = {0x0D, 0x01, 0xAA, 0xBB, 0x01, 0x02, 0xCC, 0xFC, 0x03};
  const uint8_t src[3] = {0xAA, 0xBB, 0x01};
  radio.queue_rx(frame_to_packet(build_spe_response(src, component.node_id_, payload, sizeof(payload))));

  const auto result = component.management_actions_.scan_paired_devices();
  ASSERT_TRUE(result.success);
  const std::string kcv = detail::format_key_check_value(SCAN_DEVICE_KEY);
  EXPECT_NE(result.message.find("(0x34/1) key=" + kcv + " rssi="), std::string::npos) << result.message;
  EXPECT_NE(result.message.find("[known]"), std::string::npos);
  EXPECT_EQ(result.message.find(detail::format_key_hex(SCAN_DEVICE_KEY)), std::string::npos)
      << "a known responder needs no snippet, so the key itself must not appear";
}

TEST(HubManagement, ScanPairedDevicesPutsAnUnknownKeyedResponderSnippetInsideAKeyBannerInTheLogOnly) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  add_keyed_device(component, "AABB01", SCAN_DEVICE_KEY);

  radio.queue_rx_hold_until_sent(7);
  const uint8_t payload[] = {0x0D, 0x01, 0x41, 0x5C, 0xE5, 0x0C, 0xCC, 0x07, 0xEB};
  const uint8_t src[3] = {0x41, 0x5C, 0xE5};
  radio.queue_rx(frame_to_packet(build_spe_response(src, component.node_id_, payload, sizeof(payload))));

  const auto result = component.management_actions_.scan_paired_devices();
  ASSERT_TRUE(result.success);

  // The device log gets the report with the key, inside a banner.
  const std::string &log = result.log_message;
  const std::string key_hex = detail::format_key_hex(SCAN_DEVICE_KEY);
  const size_t key_pos = log.find("system_key: \"" + key_hex + "\"");
  ASSERT_NE(key_pos, std::string::npos) << log;
  const size_t banner = log.find("DO NOT SHARE");
  const size_t end = log.find("END OF DEVICE KEY BLOCK");
  ASSERT_NE(banner, std::string::npos) << log;
  ASSERT_NE(end, std::string::npos) << log;
  EXPECT_LT(banner, key_pos);
  EXPECT_LT(key_pos, end);
  EXPECT_NE(log.find("  climate:\n"), std::string::npos);
  EXPECT_NE(log.find("key=" + detail::format_key_check_value(SCAN_DEVICE_KEY)), std::string::npos);

  // The message is what the action-result event carries into Home Assistant: no key, same device.
  EXPECT_EQ(result.message.find(key_hex), std::string::npos) << "the published message must not carry the key";
  EXPECT_EQ(result.message.find("system_key: \""), std::string::npos) << "no system_key line in the published snippet";
  EXPECT_EQ(result.message.find("DO NOT SHARE"), std::string::npos);
  EXPECT_NE(result.message.find("printed in the device log"), std::string::npos) << result.message;
  EXPECT_NE(result.message.find("  climate:\n"), std::string::npos);
  EXPECT_NE(result.message.find("key=" + detail::format_key_check_value(SCAN_DEVICE_KEY)), std::string::npos);
}

TEST(HubManagement, ScanPairedDevicesHintsWhenAKnownDeviceAnswersUnderAnotherKeyThanItsYaml) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  // The YAML gives this radiator the device key, but it answers the hub-key sweep: a key pasted on the
  // wrong radiator.
  add_keyed_device(component, "AABB01", SCAN_DEVICE_KEY);
  const uint8_t payload[] = {0x0D, 0x01, 0xAA, 0xBB, 0x01, 0x02, 0xCC, 0xFC, 0x03};
  const uint8_t src[3] = {0xAA, 0xBB, 0x01};
  radio.queue_rx(frame_to_packet(build_spe_response(src, component.node_id_, payload, sizeof(payload))));

  const auto result = component.management_actions_.scan_paired_devices();
  ASSERT_TRUE(result.success);
  EXPECT_NE(result.message.find("hint: answered under the key with check value " +
                                detail::format_key_check_value(component.system_key_) +
                                " but its YAML uses the key with check value " +
                                detail::format_key_check_value(SCAN_DEVICE_KEY) + "; fix its system_key:"),
            std::string::npos)
      << result.message;
}

TEST(HubManagement, ScanPairedDevicesGivesNoKeyHintWhenTheKeysAgree) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  add_keyed_device(component, "AABB01", SCAN_DEVICE_KEY);
  DeviceConfig plain;
  plain.type = DeviceType::ROLLER_SHUTTER;
  component.add_device("AABB02", plain);

  // AABB01 answers the per-device-key sweep (7th send on), AABB02 the hub-key sweep.
  const uint8_t payload1[] = {0x0D, 0x01, 0xAA, 0xBB, 0x01, 0x02, 0xCC, 0xFC, 0x03};
  const uint8_t src1[3] = {0xAA, 0xBB, 0x01};
  const uint8_t payload2[] = {0x00, 0x80, 0xAA, 0xBB, 0x02, 0x02, 0xCC, 0xFC, 0x03};
  const uint8_t src2[3] = {0xAA, 0xBB, 0x02};
  radio.queue_rx(frame_to_packet(build_spe_response(src2, component.node_id_, payload2, sizeof(payload2))));
  radio.queue_rx_hold_until_sent(7);
  radio.queue_rx(frame_to_packet(build_spe_response(src1, component.node_id_, payload1, sizeof(payload1))));

  const auto result = component.management_actions_.scan_paired_devices();
  ASSERT_TRUE(result.success);
  EXPECT_NE(result.message.find("AABB01"), std::string::npos);
  EXPECT_NE(result.message.find("AABB02"), std::string::npos);
  EXPECT_EQ(result.message.find("fix its system_key"), std::string::npos) << result.message;
}

TEST(HubManagement, ScanPairedDevicesHubKeyResponderSnippetCarriesNoKey) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  add_keyed_device(component, "AABB01", SCAN_DEVICE_KEY);

  const uint8_t payload[] = {0x0D, 0x01, 0x41, 0x5C, 0xE5, 0x0C, 0xCC, 0x07, 0xEB};
  const uint8_t src[3] = {0x41, 0x5C, 0xE5};
  radio.queue_rx(frame_to_packet(build_spe_response(src, component.node_id_, payload, sizeof(payload))));

  const auto result = component.management_actions_.scan_paired_devices();
  ASSERT_TRUE(result.success);
  EXPECT_NE(result.message.find("  climate:\n"), std::string::npos);
  EXPECT_EQ(result.message.find("system_key: \""), std::string::npos) << "no system_key line in the published snippet";
  EXPECT_EQ(result.message.find("key="), std::string::npos) << "hub-key lines stay unchanged";
  EXPECT_EQ(result.message.find("DO NOT SHARE"), std::string::npos);
}

TEST(HubManagement, ScanPairedDevicesAckTunableChangesNeitherPass) {
  // Both roll-call frame shapes are fixed, named constants (ROLL_CALL_PASSES in
  // management_actions.cpp): the low-power pass always sets ACK as part of its own header (it
  // matches a real VELUX hub's roll-call), and the always-alive pass never does.
  // pairing_discovery_ack_capable belongs to Discover & Pair and must not change either.
  auto sent_ctrl1_sequence = [](bool ack_capable_tuning) {
    TestableManagementComponent component;
    MockRadio radio;
    setup_component(component, radio);
    component.tuning_.pairing_discovery_ack_capable = ack_capable_tuning;

    const auto result = component.management_actions_.scan_paired_devices();
    EXPECT_TRUE(result.success);

    std::vector<uint8_t> ctrl1_bytes;
    for (const auto &sent : radio.get_sent_data()) {
      IoFrame frame{};
      EXPECT_TRUE(parse(sent.data(), static_cast<uint8_t>(sent.size()), frame));
      ctrl1_bytes.push_back(frame.ctrl1);
    }
    return ctrl1_bytes;
  };

  const std::vector<uint8_t> expected = {0x30, 0x30, 0x30, 0x00, 0x00, 0x00};
  EXPECT_EQ(sent_ctrl1_sequence(/*ack_capable_tuning=*/false), expected);
  EXPECT_EQ(sent_ctrl1_sequence(/*ack_capable_tuning=*/true), expected)
      << "the tunable must not add ACK to either pass";
}

// The always-alive pass must be byte-identical to a plain CMD_DISCOVER_SPE_REQ broadcast built
// with low_power=false, ack_capable=false — same frame shape, same preamble, same channels.
TEST(HubManagement, ScanPairedDevicesAlwaysAlivePassIsByteIdenticalToTheSingleShapeRollCall) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);

  const auto result = component.management_actions_.scan_paired_devices();
  ASSERT_TRUE(result.success);
  ASSERT_EQ(radio.get_sent_data().size(), 6u);
  ASSERT_EQ(radio.get_tx_configs().size(), 6u);

  const uint32_t expected_freq[] = {FREQ_CH2, FREQ_CH1, FREQ_CH3};
  for (uint8_t i = 0; i < 3; i++) {
    const uint8_t attempt = i + 3;  // attempts 4-6
    IoFrame reference{};
    // create_discovery_request() draws a fresh random nonce/HMAC every call, so only the frame's
    // shape (not the 12 payload bytes) can be compared against a freshly built reference frame.
    ASSERT_TRUE(create_discovery_request(reference, component.node_id_, CMD_DISCOVER_SPE_REQ, BROADCAST_DISCOVER,
                                         /*low_power=*/false, /*ack_capable=*/false, /*payload_enabled=*/false,
                                         /*payload=*/0, component.system_key_));

    IoFrame actual{};
    ASSERT_TRUE(parse(radio.get_sent_data()[attempt].data(),
                      static_cast<uint8_t>(radio.get_sent_data()[attempt].size()), actual));
    EXPECT_EQ(actual.ctrl0, reference.ctrl0) << "attempt " << (attempt + 1);
    EXPECT_EQ(actual.ctrl1, reference.ctrl1) << "attempt " << (attempt + 1);
    EXPECT_EQ(memcmp(actual.dst, reference.dst, NODE_ID_SIZE), 0) << "attempt " << (attempt + 1);
    EXPECT_EQ(memcmp(actual.src, reference.src, NODE_ID_SIZE), 0) << "attempt " << (attempt + 1);
    EXPECT_EQ(actual.cmd, reference.cmd) << "attempt " << (attempt + 1);
    EXPECT_EQ(actual.data_len, reference.data_len) << "attempt " << (attempt + 1);

    EXPECT_EQ(radio.get_tx_configs()[attempt].preamble_len, mock_preamble_bytes(component.tuning_.start_preamble_ms))
        << "attempt " << (attempt + 1);
    EXPECT_EQ(radio.get_tx_configs()[attempt].freq_hz, expected_freq[i]) << "attempt " << (attempt + 1);
  }
}

namespace {

/// Test-local MockRadio subclass shared by the tests below that need to tell which of the scan's
/// six attempts a given piece of radio activity belongs to. The base class's own queues/logs are
/// private, so this subclass never records waits itself: it records, in its own `tx_boundaries_`,
/// the call_log() index at which each send_packet() occurs (so a test can slice the log per
/// attempt) and gates queued replies by send count (so a test can place a reply under a specific
/// attempt) — then always delegates to MockRadio for the actual call-log/freq-history/rx
/// bookkeeping.
class ScanSweepTestRadio : public MockRadio {
 public:
  /// Deliver `packet` on the first wait_for_packet() call once at least `min_send_count`
  /// transmits have gone out — i.e. "no earlier than attempt `min_send_count`".
  void queue_rx_after(int min_send_count, const RadioRxPacket &packet) { pending_.push_back({min_send_count, packet}); }

  TxResult send_packet(const uint8_t *data, uint8_t len, const RadioTxConfig &tx_config) override {
    tx_boundaries_.push_back(call_log().size());
    return MockRadio::send_packet(data, len, tx_config);
  }

  bool wait_for_packet(RadioRxPacket &packet, uint32_t timeout_ms) override {
    if (!pending_.empty() && pending_.front().first <= get_send_count()) {
      queue_rx(pending_.front().second);
      pending_.pop_front();
    }
    return MockRadio::wait_for_packet(packet, timeout_ms);
  }

  /// call_log() index at which each attempt's listen window begins, one entry per send_packet().
  const std::vector<size_t> &tx_boundaries() const { return tx_boundaries_; }

  /// Entries still waiting for their `min_send_count` threshold. A test asserts this is 0 after
  /// the scan to prove a gated reply was actually released into the radio, not silently dropped
  /// because the scan ended before its threshold was reached.
  size_t pending_count() const { return pending_.size(); }

 private:
  std::deque<std::pair<int, RadioRxPacket>> pending_;
  std::vector<size_t> tx_boundaries_;
};

}  // namespace

TEST(HubManagement, ScanPairedDevicesEachPassUsesItsListenPolicy) {
  // The low-power pass listens with ROTATE_ALL_CHANNELS (starts listening on the TX channel
  // immediately — the first listen-side call_log() entry in its window is a wait, not a hop, and
  // it may later hop back onto its own TX channel); the always-alive pass listens with
  // ROTATE_SKIPPING_REQUEST (hops off the TX channel before its first listen, and never hops back
  // onto it for the rest of the window). Swapping the two policies in ROLL_CALL_PASSES must fail
  // this test.
  TestableManagementComponent component;
  ScanSweepTestRadio radio;
  setup_component(component, radio);

  const auto result = component.management_actions_.scan_paired_devices();
  EXPECT_TRUE(result.success);

  ASSERT_EQ(radio.tx_boundaries().size(), 6u) << "one send_packet() per attempt";
  const auto &call_log = radio.call_log();
  const auto &freq_history = radio.freq_history();
  const uint32_t tx_freq_per_attempt[] = {FREQ_CH2, FREQ_CH1, FREQ_CH3, FREQ_CH2, FREQ_CH1, FREQ_CH3};

  size_t freq_index = 0;  // freq_history() only records hops, in the same order call_log() does.
  for (uint8_t attempt = 0; attempt < 6; attempt++) {
    const size_t slice_start = radio.tx_boundaries()[attempt];
    const size_t slice_end = attempt + 1 < 6 ? radio.tx_boundaries()[attempt + 1] : call_log.size();
    ASSERT_LT(slice_start, slice_end) << "attempt " << (attempt + 1) << " must listen at least once";

    const bool is_low_power_pass = attempt < 3;
    EXPECT_EQ(call_log[slice_start], is_low_power_pass ? MockRadio::CallKind::kWait : MockRadio::CallKind::kHop)
        << "attempt " << (attempt + 1)
        << (is_low_power_pass ? ": ROTATE_ALL_CHANNELS must listen on the TX channel before any hop"
                              : ": ROTATE_SKIPPING_REQUEST must hop off the TX channel before its first listen");

    for (size_t i = slice_start; i < slice_end; i++) {
      if (call_log[i] != MockRadio::CallKind::kHop)
        continue;
      ASSERT_LT(freq_index, freq_history.size());
      if (!is_low_power_pass) {
        EXPECT_NE(freq_history[freq_index], tx_freq_per_attempt[attempt])
            << "attempt " << (attempt + 1) << ": always-alive pass must never hop back onto its own TX channel";
      }
      freq_index++;
    }
  }
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

  const auto result = component.management_actions_.scan_paired_devices();
  EXPECT_TRUE(result.success);
  EXPECT_NE(result.message.find("30E1F2"), std::string::npos);
  EXPECT_NE(result.message.find("horizontal awning"), std::string::npos);
  EXPECT_NE(result.message.find("[known]"), std::string::npos);
  EXPECT_EQ(result.message.find("io_device_id"), std::string::npos) << "a known responder must not get a YAML snippet";
}

TEST(HubManagement, ScanPairedDevicesKnownDeviceReportingLowPowerWithoutYamlFlagGetsHint) {
  // A hand-added known device that self-reports low-power but whose YAML never set
  // low_power: true.
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);

  DeviceConfig cfg;
  cfg.type = DeviceType::HORIZONTAL_AWNING;
  cfg.low_power = false;
  component.add_device("30E1F2", cfg);

  // Multi Information Byte data[6]=0xCD -> power_save = 1 = POWER_SAVE_LOW_POWER.
  const uint8_t payload[] = {0x04, 0x00, 0x30, 0xE1, 0xF2, 0x02, 0xCD, 0xFC, 0x03};
  const uint8_t src[3] = {0x30, 0xE1, 0xF2};
  radio.queue_rx(frame_to_packet(build_spe_response(src, component.node_id_, payload, sizeof(payload))));

  const auto result = component.management_actions_.scan_paired_devices();
  ASSERT_TRUE(result.success);
  EXPECT_NE(result.message.find("hint: reports power_save=low_power but its YAML has no low_power: true"),
            std::string::npos);
}

TEST(HubManagement, ScanPairedDevicesKnownDeviceReportingAlwaysAliveWithYamlLowPowerGetsHint) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);

  DeviceConfig cfg;
  cfg.type = DeviceType::HORIZONTAL_AWNING;
  cfg.low_power = true;
  component.add_device("30E1F2", cfg);

  // Multi Information Byte data[6]=0xCC -> power_save = 0 = POWER_SAVE_ALWAYS_ALIVE.
  const uint8_t payload[] = {0x04, 0x00, 0x30, 0xE1, 0xF2, 0x02, 0xCC, 0xFC, 0x03};
  const uint8_t src[3] = {0x30, 0xE1, 0xF2};
  radio.queue_rx(frame_to_packet(build_spe_response(src, component.node_id_, payload, sizeof(payload))));

  const auto result = component.management_actions_.scan_paired_devices();
  ASSERT_TRUE(result.success);
  EXPECT_NE(result.message.find("hint: reports power_save=always_alive but its YAML sets low_power: true"),
            std::string::npos);
}

TEST(HubManagement, ScanPairedDevicesKnownDeviceMatchingPowerClassGetsNoHint) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);

  DeviceConfig cfg;
  cfg.type = DeviceType::HORIZONTAL_AWNING;
  cfg.low_power = true;
  component.add_device("30E1F2", cfg);

  // Multi Information Byte data[6]=0xCD -> power_save = 1 = POWER_SAVE_LOW_POWER, matching YAML.
  const uint8_t payload[] = {0x04, 0x00, 0x30, 0xE1, 0xF2, 0x02, 0xCD, 0xFC, 0x03};
  const uint8_t src[3] = {0x30, 0xE1, 0xF2};
  radio.queue_rx(frame_to_packet(build_spe_response(src, component.node_id_, payload, sizeof(payload))));

  const auto result = component.management_actions_.scan_paired_devices();
  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.message.find("hint:"), std::string::npos) << "a matching power class must not get a hint";
}

TEST(HubManagement, ScanPairedDevicesKnownDeviceMatchingAlwaysAliveGetsNoHint) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);

  DeviceConfig cfg;
  cfg.type = DeviceType::HORIZONTAL_AWNING;
  cfg.low_power = false;
  component.add_device("30E1F2", cfg);

  // Multi Information Byte data[6]=0xCC -> power_save = 0 = POWER_SAVE_ALWAYS_ALIVE, matching YAML.
  const uint8_t payload[] = {0x04, 0x00, 0x30, 0xE1, 0xF2, 0x02, 0xCC, 0xFC, 0x03};
  const uint8_t src[3] = {0x30, 0xE1, 0xF2};
  radio.queue_rx(frame_to_packet(build_spe_response(src, component.node_id_, payload, sizeof(payload))));

  const auto result = component.management_actions_.scan_paired_devices();
  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.message.find("hint:"), std::string::npos) << "a matching power class must not get a hint";
}

TEST(HubManagement, ScanPairedDevicesKnownDeviceWithoutExtendedMetadataGetsNoHint) {
  // A short reply with no extended metadata has no self-reported power class to compare against
  // the registry, so it must never manufacture a hint.
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);

  DeviceConfig cfg;
  cfg.type = DeviceType::HORIZONTAL_AWNING;
  cfg.low_power = true;
  component.add_device("30E1F2", cfg);

  const uint8_t payload[] = {0x04, 0x00, 0x30, 0xE1, 0xF2};  // no extended metadata bytes
  const uint8_t src[3] = {0x30, 0xE1, 0xF2};
  radio.queue_rx(frame_to_packet(build_spe_response(src, component.node_id_, payload, sizeof(payload))));

  const auto result = component.management_actions_.scan_paired_devices();
  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.message.find("hint:"), std::string::npos)
      << "a responder without extended metadata has nothing to compare and must not get a hint";
}

TEST(HubManagement, ScanPairedDevicesUnknownPowerSaveValueGetsNoHint) {
  // power_save is a 2-bit field; only 0 (ALWAYS_ALIVE) and 1 (LOW_POWER) are known classes. A
  // reserved value (2 or 3) is not evidence of either class and must not produce a hint, even
  // against a YAML low_power: true device.
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);

  DeviceConfig cfg;
  cfg.type = DeviceType::HORIZONTAL_AWNING;
  cfg.low_power = true;
  component.add_device("30E1F2", cfg);

  // Multi Information Byte data[6]=0xCE -> power_save = 0xCE & 0x03 = 2 (reserved/unknown).
  const uint8_t payload[] = {0x04, 0x00, 0x30, 0xE1, 0xF2, 0x02, 0xCE, 0xFC, 0x03};
  const uint8_t src[3] = {0x30, 0xE1, 0xF2};
  radio.queue_rx(frame_to_packet(build_spe_response(src, component.node_id_, payload, sizeof(payload))));

  const auto result = component.management_actions_.scan_paired_devices();
  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.message.find("hint:"), std::string::npos)
      << "an unrecognized power_save value must not be treated as evidence of either class";
}

TEST(HubManagement, ScanPairedDevicesUnknownResponderGetsYamlSnippet) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);

  const uint8_t payload[] = {0x01, 0x80, 0x41, 0x5C, 0xE4, 0x02, 0xCC, 0x07, 0xEB};
  const uint8_t src[3] = {0x41, 0x5C, 0xE4};
  radio.queue_rx(frame_to_packet(build_spe_response(src, component.node_id_, payload, sizeof(payload))));

  const auto result = component.management_actions_.scan_paired_devices();
  EXPECT_TRUE(result.success);
  EXPECT_NE(result.message.find("[unknown]"), std::string::npos);
  EXPECT_NE(result.message.find("Paste this into your YAML to register it:"), std::string::npos);
  EXPECT_NE(result.message.find("platform: home_io_control"), std::string::npos);
  EXPECT_NE(result.message.find("io_device_id: \"415CE4\""), std::string::npos);
  EXPECT_NE(result.message.find("io_device_type: \"light\""), std::string::npos);
  EXPECT_NE(result.message.find("io_subtype: 0"), std::string::npos);
  EXPECT_EQ(result.message.find("low_power"), std::string::npos)
      << "Multi Information Byte data[6]=0xCC decodes to power_save=0 (ALWAYS_ALIVE), so no low_power line";
  EXPECT_EQ(result.message.find("hint:"), std::string::npos)
      << "an unknown responder has no registry entry to hint about";
  EXPECT_EQ(component.get_device("415CE4"), nullptr) << "an unknown responder must not be auto-registered";
}

TEST(HubManagement, ScanPairedDevicesHeaterWithSetpointGetsClimateSnippet) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);

  // Type 52 (0x34) subtype 1: packed bytes 0D 01.
  const uint8_t payload[] = {0x0D, 0x01, 0x41, 0x5C, 0xE5, 0x0C, 0xCC, 0x07, 0xEB};
  const uint8_t src[3] = {0x41, 0x5C, 0xE5};
  radio.queue_rx(frame_to_packet(build_spe_response(src, component.node_id_, payload, sizeof(payload))));

  const auto result = component.management_actions_.scan_paired_devices();
  ASSERT_TRUE(result.success);
  EXPECT_NE(result.message.find("electrical heater, with adjustable setpoint (0x34/1)"), std::string::npos)
      << result.message;
  EXPECT_NE(result.message.find("  climate:\n"), std::string::npos) << result.message;
  EXPECT_NE(result.message.find("io_device_type: \"electrical_heater\""), std::string::npos) << result.message;
  EXPECT_NE(result.message.find("io_subtype: 1"), std::string::npos) << result.message;
}

TEST(HubManagement, ScanPairedDevicesTypeWithoutPlatformIsReportedAsNotSupportedYet) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);

  // Type 22 subtype 1 (the Atlantic heat pump): recognised and named, but no platform drives it.
  const uint8_t payload[] = {0x05, 0x81, 0x41, 0x5C, 0xE5, 0x0C, 0xCC, 0x07, 0xEB};
  const uint8_t src[3] = {0x41, 0x5C, 0xE5};
  radio.queue_rx(frame_to_packet(build_spe_response(src, component.node_id_, payload, sizeof(payload))));

  const auto result = component.management_actions_.scan_paired_devices();
  ASSERT_TRUE(result.success);
  EXPECT_NE(result.message.find("heat pump"), std::string::npos) << result.message;
  EXPECT_NE(result.message.find("not supported yet"), std::string::npos) << result.message;
  EXPECT_EQ(result.message.find("io_device_id"), std::string::npos) << "no snippet for a type without a platform";
}

TEST(HubManagement, ScanPairedDevicesLowPowerResponderGetsLowPowerLineInSnippet) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);

  // Multi Information Byte is data[6]. 0xCD -> power_save = 0xCD & 0x03 = 1 = POWER_SAVE_LOW_POWER.
  const uint8_t payload[] = {0x01, 0x80, 0x41, 0x5C, 0xE4, 0x02, 0xCD, 0x07, 0xEB};
  const uint8_t src[3] = {0x41, 0x5C, 0xE4};
  radio.queue_rx(frame_to_packet(build_spe_response(src, component.node_id_, payload, sizeof(payload))));

  const auto result = component.management_actions_.scan_paired_devices();
  ASSERT_TRUE(result.success);
  EXPECT_NE(result.message.find("io_device_id: \"415CE4\""), std::string::npos);
  EXPECT_NE(result.message.find("low_power: true"), std::string::npos)
      << "a responder reporting POWER_SAVE_LOW_POWER must get low_power: true pre-filled in its snippet";
}

TEST(HubManagement, ScanPairedDevicesAlwaysAliveResponderOmitsLowPowerLine) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);

  // Multi Information Byte is data[6]. 0xCC -> power_save = 0xCC & 0x03 = 0 = POWER_SAVE_ALWAYS_ALIVE.
  const uint8_t payload[] = {0x01, 0x80, 0x41, 0x5C, 0xE4, 0x02, 0xCC, 0x07, 0xEB};
  const uint8_t src[3] = {0x41, 0x5C, 0xE4};
  radio.queue_rx(frame_to_packet(build_spe_response(src, component.node_id_, payload, sizeof(payload))));

  const auto result = component.management_actions_.scan_paired_devices();
  ASSERT_TRUE(result.success);
  EXPECT_NE(result.message.find("io_device_id: \"415CE4\""), std::string::npos);
  EXPECT_EQ(result.message.find("low_power"), std::string::npos)
      << "an always-alive responder must not get a low_power line";
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

  // HORIZONTAL_AWNING (type 0x04) is the one type inverted by default (device_feature::INVERTED),
  // and it is unregistered here so the report includes its YAML block.
  const uint8_t payload[] = {0x04, 0x00, 0x30, 0xE1, 0xF2, 0x02, 0xCC, 0xFC, 0x03};
  const uint8_t src[3] = {0x30, 0xE1, 0xF2};
  radio.queue_rx(frame_to_packet(build_spe_response(src, component.node_id_, payload, sizeof(payload))));

  const auto result = component.management_actions_.scan_paired_devices();
  ASSERT_TRUE(result.success);
  EXPECT_NE(result.message.find("rssi=-73dBm"), std::string::npos)
      << "the reply's RSSI must reach the report, not a default";
  EXPECT_NE(result.message.find("invert_position: true"), std::string::npos)
      << "an inverted cover type must carry its inversion into the ready-to-paste YAML";
}

TEST(HubManagement, ScanPairedDevicesReportsAnUnknownRssiAsNotAvailable) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  radio.set_last_capture_rssi_unknown();

  const uint8_t payload[] = {0x04, 0x00, 0x30, 0xE1, 0xF2, 0x02, 0xCC, 0xFC, 0x03};
  const uint8_t src[3] = {0x30, 0xE1, 0xF2};
  radio.queue_rx(frame_to_packet(build_spe_response(src, component.node_id_, payload, sizeof(payload))));

  const auto result = component.management_actions_.scan_paired_devices();
  ASSERT_TRUE(result.success);
  EXPECT_NE(result.message.find("rssi=n/a"), std::string::npos) << result.message;
  EXPECT_EQ(result.message.find("rssi=-"), std::string::npos);
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

  const auto result = component.management_actions_.scan_paired_devices();
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

TEST(HubManagement, ScanPairedDevicesSameSourceInBothPassesReportedOnce) {
  // A device that answers both the low-power and the always-alive pass (e.g. it happens to be
  // awake for both broadcasts) must still be reported once — dedup by src spans the whole scan,
  // not just one pass.
  TestableManagementComponent component;
  ScanSweepTestRadio radio;
  setup_component(component, radio);

  const uint8_t payload[] = {0x01, 0x80, 0x41, 0x5C, 0xE4, 0x02, 0xCC, 0x07, 0xEB};
  const uint8_t src[3] = {0x41, 0x5C, 0xE4};
  const auto packet = frame_to_packet(build_spe_response(src, component.node_id_, payload, sizeof(payload)));
  radio.queue_rx_after(1, packet);  // no earlier than attempt 1 (low-power pass)
  radio.queue_rx_after(4, packet);  // no earlier than attempt 4 (always-alive pass)

  const auto result = component.management_actions_.scan_paired_devices();
  EXPECT_TRUE(result.success);
  ASSERT_EQ(radio.pending_count(), 0u) << "both gated replies must have been released into the radio during the scan";
  EXPECT_NE(result.message.find("1 device detected"), std::string::npos)
      << "the same source answering in both passes must not double-count";

  size_t occurrences = 0;
  for (size_t pos = result.message.find("415CE4:"); pos != std::string::npos;
       pos = result.message.find("415CE4:", pos + 1)) {
    occurrences++;
  }
  EXPECT_EQ(occurrences, 1u) << "a device answering both passes must produce exactly one summary line";
}

TEST(HubManagement, ScanPairedDevicesLateLowPowerReplyInNormalPassStillGetsLowPowerSnippet) {
  // Low-power pass first: a sleeper that only answers once the always-alive windows are already
  // running is still collected and still correctly classified from its own self-reported Multi
  // Information Byte, regardless of which pass's window caught it.
  TestableManagementComponent component;
  ScanSweepTestRadio radio;
  setup_component(component, radio);

  // MIB 0xCD -> power_save = 0xCD & 0x03 = 1 = POWER_SAVE_LOW_POWER.
  const uint8_t payload[] = {0x01, 0x80, 0x41, 0x5C, 0xE4, 0x02, 0xCD, 0x07, 0xEB};
  const uint8_t src[3] = {0x41, 0x5C, 0xE4};
  radio.queue_rx_after(4, frame_to_packet(build_spe_response(src, component.node_id_, payload, sizeof(payload))));

  const auto result = component.management_actions_.scan_paired_devices();
  ASSERT_TRUE(result.success);
  ASSERT_EQ(radio.pending_count(), 0u) << "the gated reply must have been released into the radio during the scan";
  EXPECT_NE(result.message.find("io_device_id: \"415CE4\""), std::string::npos);
  EXPECT_NE(result.message.find("low_power: true"), std::string::npos)
      << "a reply caught only by the always-alive pass must still report its own self-described power class";
}

TEST(HubManagement, ScanPairedDevicesLowPowerPassHeaderMatchesTheKlr300RollCall) {
  // Corpus-grounded: our low-power pass's frame shape must match a real VELUX KLR300's own
  // roll-call, not just a comment's claim that it does.
  const auto *capture = corpus_test::capture_by_id("velux_klr300_discovery_rollcall_node_verification");
  ASSERT_NE(capture, nullptr) << "corpus capture renamed?";

  const corpus::CorpusFrame *rollcall_cf = nullptr;
  for (uint8_t i = 0; i < capture->frame_count; i++) {
    // Every frame in this capture is dir: rx (it is entirely the KLR300's own traffic, heard by
    // the tester's board), so filtering on cmd alone is enough.
    if (capture->frames[i].has_cmd && capture->frames[i].cmd == CMD_DISCOVER_SPE_REQ) {
      rollcall_cf = &capture->frames[i];
      break;
    }
  }
  ASSERT_NE(rollcall_cf, nullptr) << "no CMD_DISCOVER_SPE_REQ frame in the capture";
  const IoFrame expected = corpus_test::parse_capture_frame(*rollcall_cf);

  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);

  const auto result = component.management_actions_.scan_paired_devices();
  EXPECT_TRUE(result.success);

  ASSERT_FALSE(radio.get_sent_data().empty());
  IoFrame actual{};
  const auto &sent = radio.get_sent_data()[0];  // attempt 1: low-power pass, CH2
  ASSERT_TRUE(parse(sent.data(), static_cast<uint8_t>(sent.size()), actual));

  // Nonce/HMAC bytes differ per call (fresh challenge every transmit), so only the frame's shape
  // is compared.
  EXPECT_EQ(actual.ctrl0, expected.ctrl0) << "same length/flags as the captured KLR300 roll-call";
  EXPECT_EQ(actual.ctrl1, expected.ctrl1) << "CTRL1 must be LOW_POWER|ACK (0x30), the KLR300's own header";
  EXPECT_EQ(memcmp(actual.dst, expected.dst, NODE_ID_SIZE), 0);
  EXPECT_EQ(actual.cmd, expected.cmd);
  EXPECT_EQ(actual.data_len, expected.data_len) << "same nonce+HMAC payload length";
}

TEST(HubManagement, ScanPairedDevicesAlwaysAliveOnlySendsTheSingleShapeRollCall) {
  // scan_power_classes=always_alive runs only the always-alive pass: three attempts,
  // CTRL1=0x00, `start_preamble_ms`, and the report names the excluded class.
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.tuning_.scan_power_classes = ScanPowerClasses::ALWAYS_ALIVE;

  const auto result = component.management_actions_.scan_paired_devices();
  EXPECT_TRUE(result.success);

  ASSERT_EQ(radio.get_sent_data().size(), 3u) << "only the always-alive pass runs";
  ASSERT_EQ(radio.get_tx_configs().size(), 3u);
  const uint32_t expected_freq[] = {FREQ_CH2, FREQ_CH1, FREQ_CH3};
  for (uint8_t i = 0; i < 3; i++) {
    IoFrame frame{};
    ASSERT_TRUE(parse(radio.get_sent_data()[i].data(), static_cast<uint8_t>(radio.get_sent_data()[i].size()), frame));
    EXPECT_EQ(frame.ctrl1, 0) << "attempt " << (i + 1);
    EXPECT_EQ(radio.get_tx_configs()[i].freq_hz, expected_freq[i]) << "attempt " << (i + 1);
    EXPECT_EQ(radio.get_tx_configs()[i].preamble_len, mock_preamble_bytes(component.tuning_.start_preamble_ms))
        << "attempt " << (i + 1);
  }
  EXPECT_NE(result.message.find("NOTE: scan_power_classes=always_alive: low_power devices were not called."),
            std::string::npos);
}

TEST(HubManagement, ScanPairedDevicesLowPowerOnlySendsOnlyTheLowPowerPass) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.tuning_.scan_power_classes = ScanPowerClasses::LOW_POWER;

  const auto result = component.management_actions_.scan_paired_devices();
  EXPECT_TRUE(result.success);

  ASSERT_EQ(radio.get_sent_data().size(), 3u) << "only the low-power pass runs";
  ASSERT_EQ(radio.get_tx_configs().size(), 3u);
  const uint32_t expected_freq[] = {FREQ_CH2, FREQ_CH1, FREQ_CH3};
  for (uint8_t i = 0; i < 3; i++) {
    IoFrame frame{};
    ASSERT_TRUE(parse(radio.get_sent_data()[i].data(), static_cast<uint8_t>(radio.get_sent_data()[i].size()), frame));
    EXPECT_EQ(frame.ctrl1, static_cast<uint8_t>(CTRL1_LOW_POWER | CTRL1_ACK)) << "attempt " << (i + 1);
    EXPECT_EQ(radio.get_tx_configs()[i].freq_hz, expected_freq[i]) << "attempt " << (i + 1);
    EXPECT_EQ(radio.get_tx_configs()[i].preamble_len, MOCK_WAKE_PREAMBLE) << "attempt " << (i + 1);
  }
  EXPECT_NE(result.message.find("NOTE: scan_power_classes=low_power: always_alive devices were not called."),
            std::string::npos);
}

TEST(HubManagement, ScanPairedDevicesBothOmitsTheSelectionNote) {
  // Search for "scan_power_classes=", not "NOTE" — the truncation NOTE shares that word.
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);

  const auto result = component.management_actions_.scan_paired_devices();
  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.message.find("scan_power_classes="), std::string::npos)
      << "the default selection must not add a selection NOTE line";
}

TEST(HubManagement, ScanPairedDevicesDedupsSameSourceThroughActionLayer) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);

  const uint8_t payload[] = {0x01, 0x80, 0x41, 0x5C, 0xE4, 0x02, 0xCC, 0x07, 0xEB};
  const uint8_t src[3] = {0x41, 0x5C, 0xE4};
  radio.queue_rx(frame_to_packet(build_spe_response(src, component.node_id_, payload, sizeof(payload))));
  radio.queue_rx(frame_to_packet(build_spe_response(src, component.node_id_, payload, sizeof(payload))));

  const auto result = component.management_actions_.scan_paired_devices();
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

  const auto result = component.management_actions_.scan_paired_devices();
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

  const auto result = component.management_actions_.scan_paired_devices();
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
  ASSERT_EQ(api_server.user_services_.size(), 11u);

  const auto response = api_server.user_services_[4]->encode_list_service_response();
  EXPECT_EQ(response.name.str(), "scan_paired_devices");
  ASSERT_EQ(response.args.size(), 0u);

  esphome::api::ExecuteServiceRequest request;
  request.key = response.key;
  request.args.init(0);

  EXPECT_TRUE(api_server.user_services_[4]->execute_service(request))
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

  component.management_actions_.api_scan_paired_devices();

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
  // handle that without a dangling "for device :" (host tests can't read a logged line's text,
  // since IOHOME_HOST_LOG_STUB discards its arguments, so this asserts on the event/result
  // fields the log line is built from instead).
  esphome::api::APIServer api_server;
  esphome::api::ScopedGlobalApiServer scoped_api_server(api_server);
  api_server.reset();

  TestableManagementComponent component;  // deliberately not setup_component()'d: not initialized

  component.management_actions_.api_scan_paired_devices();

  ASSERT_EQ(api_server.events_.size(), 1u);
  const auto &event = api_server.events_.front();
  EXPECT_EQ(event.event_type, "esphome.home_io_control_action_result");
  EXPECT_EQ(event.data.at("action"), "scan_paired_devices");
  EXPECT_EQ(event.data.at("device_id"), "") << "scan action never has a single target device";
  EXPECT_EQ(event.data.at("success"), "false");
  EXPECT_EQ(event.data.at("message"), "hub is not initialized");
}

TEST(HubManagement, OneWaySetPositionActionIsRegisteredWithBothArguments) {
  esphome::api::APIServer api_server;
  esphome::api::ScopedGlobalApiServer scoped_api_server(api_server);
  api_server.reset();

  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.register_management_actions_();
  ASSERT_EQ(api_server.user_services_.size(), 11u);

  const auto response = api_server.user_services_[5]->encode_list_service_response();
  EXPECT_EQ(response.name.str(), "oneway_set_position");
  ASSERT_EQ(response.args.size(), 2u);
  EXPECT_EQ(response.args[0].name.str(), "controller_id");
  EXPECT_EQ(response.args[1].name.str(), "position");
}

TEST(HubManagement, OneWaySetPositionRejectsAnUnknownIdentity) {
  // 1W transmits nothing back, so an action that silently did nothing for a mistyped handle would
  // be indistinguishable from one that worked. The published result is the only signal there is.
  esphome::api::APIServer api_server;
  esphome::api::ScopedGlobalApiServer scoped_api_server(api_server);
  api_server.reset();

  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);

  component.management_actions_.api_oneway_set_position("nope", "50");

  ASSERT_EQ(api_server.events_.size(), 1u);
  const auto &event = api_server.events_.front();
  EXPECT_EQ(event.data.at("action"), "oneway_set_position");
  EXPECT_EQ(event.data.at("success"), "false");
}

TEST(HubManagement, OneWaySetPositionRejectsAMalformedPosition) {
  // A position that failed to parse and quietly became 0 would send a fully-open command to an
  // entire device class.
  esphome::api::APIServer api_server;
  esphome::api::ScopedGlobalApiServer scoped_api_server(api_server);
  api_server.reset();

  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);

  OneWayControllerIdentity identity{};
  identity.id = "awning_remote";
  identity.io_device_type = DeviceType::VERTICAL_EXTERIOR_AWNING;
  component.add_oneway_controller(identity);

  for (const char *bad : {"", "abc", "101", "-1", "12x"}) {
    api_server.reset();
    component.management_actions_.api_oneway_set_position("awning_remote", bad);
    ASSERT_EQ(api_server.events_.size(), 1u) << "input '" << bad << "' produced no result event";
    EXPECT_EQ(api_server.events_.front().data.at("success"), "false")
        << "position '" << bad << "' should be rejected, not coerced";
  }
}

TEST(HubManagement, OneWaySetPositionQueuesTheParsedPositionForAKnownIdentity) {
  // The malformed-input test above only pins the rejection half. Nothing else asserts that the
  // parsed value actually reaches the queue -- a wrong variable, a truncating cast, or an
  // off-by-one on the bound would pass every existing test and 1W emits nothing at runtime that
  // would reveal it.
  esphome::api::APIServer api_server;
  esphome::api::ScopedGlobalApiServer scoped_api_server(api_server);

  OneWayControllerIdentity identity{};
  identity.id = "awning_remote";
  identity.io_device_type = DeviceType::VERTICAL_EXTERIOR_AWNING;

  const struct {
    const char *input;
    uint8_t expected;
  } cases[] = {
      {"0", 0},
      {"75", 75},
      {"100", 100},
  };

  for (const auto &c : cases) {
    api_server.reset();

    TestableManagementComponent component;
    MockRadio radio;
    setup_component(component, radio);
    component.add_oneway_controller(identity);

    component.management_actions_.api_oneway_set_position("awning_remote", c.input);

    ASSERT_EQ(api_server.events_.size(), 1u) << "input '" << c.input << "' produced no result event";
    EXPECT_EQ(api_server.events_.front().data.at("success"), "true") << "input '" << c.input << "'";
    ASSERT_EQ(component.op_queue_.size(), 1u) << "input '" << c.input << "'";
    EXPECT_EQ(component.op_queue_.front().type, PendingOperationType::ONEWAY_POSITION) << "input '" << c.input << "'";
    EXPECT_EQ(component.op_queue_.front().device_id, "awning_remote")
        << "device_id carries the controller-identity handle for 1W ops";
    EXPECT_EQ(component.op_queue_.front().position, c.expected) << "input '" << c.input << "'";
  }
}

TEST(HubManagement, OneWayRemoveControllerRejectsAnUnknownIdentity) {
  // Same reasoning as OneWaySetPositionRejectsAnUnknownIdentity: 1W transmits nothing back, so a
  // silently-ignored mistyped handle would be indistinguishable from a working un-enroll.
  esphome::api::APIServer api_server;
  esphome::api::ScopedGlobalApiServer scoped_api_server(api_server);
  api_server.reset();

  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);

  component.management_actions_.api_oneway_remove_controller("nope");

  ASSERT_EQ(api_server.events_.size(), 1u);
  const auto &event = api_server.events_.front();
  EXPECT_EQ(event.data.at("action"), "oneway_remove_controller");
  EXPECT_EQ(event.data.at("success"), "false");
  EXPECT_TRUE(component.op_queue_.empty()) << "an unknown identity must not queue a transmit";
}

TEST(HubManagement, OneWayRemoveControllerQueuesForAKnownIdentity) {
  esphome::api::APIServer api_server;
  esphome::api::ScopedGlobalApiServer scoped_api_server(api_server);
  api_server.reset();

  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);

  OneWayControllerIdentity identity{};
  identity.id = "awning_remote";
  identity.io_device_type = DeviceType::VERTICAL_EXTERIOR_AWNING;
  component.add_oneway_controller(identity);

  component.management_actions_.api_oneway_remove_controller("awning_remote");

  ASSERT_EQ(api_server.events_.size(), 1u);
  EXPECT_EQ(api_server.events_.front().data.at("success"), "true");
  ASSERT_EQ(component.op_queue_.size(), 1u);
  EXPECT_EQ(component.op_queue_.front().type, PendingOperationType::ONEWAY_UNENROLL);
  EXPECT_EQ(component.op_queue_.front().device_id, "awning_remote")
      << "device_id carries the controller-identity handle for 1W ops";
}

// === oneway_set_sequence ===

namespace {

const uint8_t SEQUENCE_CONTROLLER_NODE[NODE_ID_SIZE] = {0x11, 0x22, 0x33};

/// A management component with one open 1W identity whose counter starts at `initial_sequence`.
void add_sequence_identity(TestableManagementComponent &component, uint16_t initial_sequence) {
  OneWayControllerIdentity identity{};
  identity.id = "awning_remote";
  memcpy(identity.node_id, SEQUENCE_CONTROLLER_NODE, NODE_ID_SIZE);
  memset(identity.system_key, 0x11, AES_KEY_SIZE);
  identity.io_device_type = DeviceType::VERTICAL_EXTERIOR_AWNING;
  identity.initial_sequence = initial_sequence;
  component.add_oneway_controller(identity);
  component.oneway_transmitter().setup();
}

uint16_t next_sequence(TestableManagementComponent &component) {
  uint16_t value = 0;
  EXPECT_TRUE(component.oneway_transmitter().peek_sequence("awning_remote", value));
  return value;
}

/// Run the action and return the one result event it published.
std::map<std::string, std::string> run_set_sequence(TestableManagementComponent &component,
                                                    esphome::api::APIServer &api_server, const std::string &id,
                                                    const std::string &sequence, const std::string &allow_lower) {
  api_server.reset();
  component.management_actions_.api_oneway_set_sequence(id, sequence, allow_lower);
  EXPECT_EQ(api_server.events_.size(), 1u);
  const auto &data = api_server.events_.front().data;
  EXPECT_EQ(data.at("action"), "oneway_set_sequence");
  return {data.begin(), data.end()};
}

}  // namespace

TEST(HubManagement, OneWaySetSequenceMovesTheCounterForwardAndNamesTheWrapMargin) {
  esphome::test_preferences::wipe();
  esphome::api::APIServer api_server;
  esphome::api::ScopedGlobalApiServer scoped_api_server(api_server);
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  add_sequence_identity(component, 100);

  const auto result = run_set_sequence(component, api_server, "awning_remote", "65000", "");

  EXPECT_EQ(result.at("success"), "true");
  EXPECT_EQ(result.at("device_id"), "awning_remote");
  EXPECT_NE(result.at("message").find("from 100 to 65000"), std::string::npos) << result.at("message");
  EXPECT_NE(result.at("message").find("536 commands left"), std::string::npos) << result.at("message");
  EXPECT_EQ(result.at("message").find("initial_sequence"), std::string::npos) << result.at("message");
  EXPECT_EQ(next_sequence(component), 65000);
  EXPECT_TRUE(radio.get_sent_data().empty()) << "the action sends nothing";
  EXPECT_TRUE(component.op_queue_.empty());
}

TEST(HubManagement, OneWaySetSequenceRefusesANumberNotAboveTheCurrentOneAndNamesIt) {
  esphome::test_preferences::wipe();
  esphome::api::APIServer api_server;
  esphome::api::ScopedGlobalApiServer scoped_api_server(api_server);
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  add_sequence_identity(component, 500);

  for (const char *value : {"499", "500"}) {
    const auto result = run_set_sequence(component, api_server, "awning_remote", value, "false");
    EXPECT_EQ(result.at("success"), "false") << value;
    EXPECT_NE(result.at("message").find("current 500"), std::string::npos) << result.at("message");
  }
  EXPECT_EQ(next_sequence(component), 500) << "a refusal changes nothing";
}

TEST(HubManagement, OneWaySetSequenceMovesBackwardsOnlyWithAllowLower) {
  esphome::test_preferences::wipe();
  esphome::api::APIServer api_server;
  esphome::api::ScopedGlobalApiServer scoped_api_server(api_server);
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  add_sequence_identity(component, 500);

  const auto result = run_set_sequence(component, api_server, "awning_remote", "10", "true");

  EXPECT_EQ(result.at("success"), "true");
  EXPECT_EQ(next_sequence(component), 10);
}

TEST(HubManagement, OneWaySetSequenceReadsAllowLowerAsHomeAssistantSendsIt) {
  esphome::test_preferences::wipe();
  esphome::api::APIServer api_server;
  esphome::api::ScopedGlobalApiServer scoped_api_server(api_server);
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  add_sequence_identity(component, 500);

  // Home Assistant passes an unquoted YAML boolean as "True" / "False".
  auto result = run_set_sequence(component, api_server, "awning_remote", "400", "False");
  EXPECT_EQ(result.at("success"), "false") << result.at("message");
  EXPECT_NE(result.at("message").find("current 500"), std::string::npos) << result.at("message");
  result = run_set_sequence(component, api_server, "awning_remote", " 400 ", " True ");
  EXPECT_EQ(result.at("success"), "true") << result.at("message");
  EXPECT_EQ(next_sequence(component), 400);
}

TEST(HubManagement, OneWaySetSequenceWarnsWhenInitialSequenceWouldReplaceTheValueAtBoot) {
  esphome::test_preferences::wipe();
  esphome::api::APIServer api_server;
  esphome::api::ScopedGlobalApiServer scoped_api_server(api_server);
  {
    TestableManagementComponent component;
    MockRadio radio;
    setup_component(component, radio);
    add_sequence_identity(component, 500);

    const auto result = run_set_sequence(component, api_server, "awning_remote", "10", "true");
    EXPECT_EQ(result.at("success"), "true");
    EXPECT_NE(result.at("message").find("initial_sequence 500 replaces it at the next boot"), std::string::npos)
        << result.at("message");
  }

  // The warning is true: the boot rule jumps forward to initial_sequence again.
  TestableManagementComponent rebooted;
  MockRadio radio;
  setup_component(rebooted, radio);
  add_sequence_identity(rebooted, 500);
  EXPECT_EQ(next_sequence(rebooted), 500);
  esphome::test_preferences::wipe();
}

TEST(HubManagement, OneWaySetSequenceFailsWithAReasonOnBadInput) {
  esphome::test_preferences::wipe();
  esphome::api::APIServer api_server;
  esphome::api::ScopedGlobalApiServer scoped_api_server(api_server);
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  add_sequence_identity(component, 500);

  struct Case {
    const char *id;
    const char *sequence;
    const char *allow_lower;
    const char *reason;
  };
  const Case cases[] = {
      {"nope", "600", "", "no oneway_controllers identity"},  {"awning_remote", "65536", "", "between 0 and 65535"},
      {"awning_remote", "-1", "", "between 0 and 65535"},     {"awning_remote", "12.5", "", "between 0 and 65535"},
      {"awning_remote", "abc", "", "between 0 and 65535"},    {"awning_remote", "", "", "between 0 and 65535"},
      {"awning_remote", "600", "yes", "allow_lower must be"},
  };
  for (const Case &c : cases) {
    const auto result = run_set_sequence(component, api_server, c.id, c.sequence, c.allow_lower);
    EXPECT_EQ(result.at("success"), "false") << c.sequence << " / " << c.allow_lower;
    EXPECT_NE(result.at("message").find(c.reason), std::string::npos) << result.at("message");
  }
  EXPECT_EQ(next_sequence(component), 500);
}

TEST(HubManagement, OneWaySetSequenceAcceptsTheLimitsOfTheRange) {
  esphome::test_preferences::wipe();
  esphome::api::APIServer api_server;
  esphome::api::ScopedGlobalApiServer scoped_api_server(api_server);
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  add_sequence_identity(component, 500);

  auto result = run_set_sequence(component, api_server, "awning_remote", "65535", "");
  EXPECT_EQ(result.at("success"), "true");
  EXPECT_NE(result.at("message").find("1 commands left"), std::string::npos) << result.at("message");
  result = run_set_sequence(component, api_server, "awning_remote", "0", "true");
  EXPECT_EQ(result.at("success"), "true");
  EXPECT_EQ(next_sequence(component), 0);
}

TEST(HubManagement, OneWaySetSequenceIsPersistedBeforeSuccessAndSurvivesAReboot) {
  esphome::test_preferences::wipe();
  esphome::api::APIServer api_server;
  esphome::api::ScopedGlobalApiServer scoped_api_server(api_server);
  {
    TestableManagementComponent component;
    MockRadio radio;
    setup_component(component, radio);
    add_sequence_identity(component, 100);

    // A write that cannot be persisted is reported as a failure, not success.
    esphome::test_preferences::faults().fail_sync = true;
    auto result = run_set_sequence(component, api_server, "awning_remote", "700", "");
    esphome::test_preferences::faults() = {};
    EXPECT_EQ(result.at("success"), "false");
    EXPECT_NE(result.at("message").find("persist"), std::string::npos) << result.at("message");
    EXPECT_EQ(next_sequence(component), 100) << "a failed write leaves the counter where it was";

    result = run_set_sequence(component, api_server, "awning_remote", "700", "");
    EXPECT_EQ(result.at("success"), "true");
  }

  // Reboot: a new component opens the counters again; YAML still says 100.
  TestableManagementComponent rebooted;
  MockRadio radio;
  setup_component(rebooted, radio);
  add_sequence_identity(rebooted, 100);
  EXPECT_EQ(next_sequence(rebooted), 700);
  esphome::test_preferences::wipe();
}

TEST(HubManagement, TheCommandAfterOneWaySetSequenceCarriesTheNewNumber) {
  esphome::test_preferences::wipe();
  esphome::api::APIServer api_server;
  esphome::api::ScopedGlobalApiServer scoped_api_server(api_server);
  TestableManagementComponent component;
  MockRadio radio;
  radio.set_rssi_default(-100);
  setup_component(component, radio);
  add_sequence_identity(component, 100);
  run_set_sequence(component, api_server, "awning_remote", "4242", "");

  ASSERT_TRUE(component.oneway_transmitter().send_command("awning_remote", CoverCommand::STOP));

  ASSERT_FALSE(radio.get_sent_data().empty());
  IoFrame frame{};
  const auto &bytes = radio.get_sent_data().front();
  ASSERT_TRUE(parse(bytes.data(), static_cast<uint8_t>(bytes.size()), frame));
  EXPECT_EQ(static_cast<uint16_t>((frame.data[6] << 8) | frame.data[7]), 4242);
  EXPECT_EQ(next_sequence(component), 4243);
  esphome::test_preferences::wipe();
}

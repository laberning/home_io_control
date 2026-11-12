/// @file diagnostic_probes_test.cpp
/// @brief Tests for the diagnostic probes (probe_device(), probe_sweep(), read_object and the register probes).

#include "diagnostic_probes.h"
#include "hub_core.h"
#include "log_helpers.h"
#include "proto_commands.h"
#include "proto_crypto.h"
#include "proto_frame.h"

#include "esphome/components/api/custom_api_device.h"

#include "corpus_generated.h"
#include "corpus_test_helpers.h"
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

/// A 6-byte CMD_PRIVATE_RESP function-ID reply, shaped exactly like
/// tests/corpus/captures/probe/multi_somfy_probe_private_function_id_replies.yaml frame 0
/// (data = 04 60 00 12 00 00). data_len == 6 clears PRIVATE_RESPONSE_MIN_DATA_LEN
/// (hub_status.cpp), so this is the frame shape that would misread as a position update if a
/// probe reply were ever routed through apply_private_response_status() instead of staying on
/// its own reporting path — see DiagnosticProbes::probe_device()'s doxygen and ADR 0024.
static IoFrame make_private_function_reply(const uint8_t dst[3]) {
  IoFrame frame{};
  init_frame(frame, true, false, true, false);
  const uint8_t device_node_id[3] = {0xAB, 0xC1, 0x23};
  set_dst(frame, dst);
  set_src(frame, device_node_id);
  const uint8_t payload[6] = {0x04, 0x60, 0x00, 0x12, 0x00, 0x00};
  set_cmd(frame, CMD_PRIVATE_RESP, payload, sizeof(payload));
  return frame;
}

/// Node ID of the device the probe tests talk to ("ABC123").
const uint8_t PROBE_DEVICE_NODE_ID[3] = {0xAB, 0xC1, 0x23};

}  // namespace

// --- probe_device() / probe_sweep() ---

TEST(HubManagement, ProbeDeviceRejectsUnknownDevice) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);

  const auto result = component.probe_device("123456", "private_fn", "6");
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "device is not registered on this hub");
  EXPECT_EQ(result.probe_name, "private_fn")
      << "the failure event must still carry the probe that was requested, not lose it to an "
         "early return";
  EXPECT_EQ(result.probe_index, "6");
}

TEST(HubManagement, ProbeDeviceRejectsWhenDiagnosticProbesDisabled) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  // Deliberately not enabled (no diagnostic_probes_enabled_() call).

  const auto result = component.probe_device("ABC123", "private_fn", "6");
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "diagnostic probes are not enabled; set diagnostic_probes: true in YAML");
  EXPECT_TRUE(result.terminal_refusal)
      << "disabled is a refusal that will recur for every remaining sweep index -- probe_sweep() "
         "must stop on it via this flag, not by matching message text";
}

TEST(HubManagement, ProbeDeviceRejectsWhenDeviceMoving) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.set_diagnostic_probes_enabled(true);
  auto *dev = component.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  dev->is_stopped = false;

  const auto result = component.probe_device("ABC123", "private_fn", "6");
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "device is moving; refusing to probe mid-transaction");
  EXPECT_TRUE(result.terminal_refusal) << "a device that is moving stays moving for the rest of a sweep";
}

TEST(HubManagement, ProbeDeviceRejectsInvalidIndex) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.set_diagnostic_probes_enabled(true);

  const auto result = component.probe_device("ABC123", "private_fn", "not-a-number");
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "index must be a decimal or 0x-prefixed byte value (0-255)");
  EXPECT_FALSE(result.terminal_refusal) << "a bad index for this index doesn't imply the next index is also bad";
}

TEST(HubManagement, ProbeDeviceRejectsLeadingZeroDecimalIndex) {
  // "010" is valid octal (8) to strtoul(), which would silently probe a different index than a
  // user copying a byte value out of a hex dump intended -- must be rejected, not parsed as octal.
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.set_diagnostic_probes_enabled(true);

  const auto result = component.probe_device("ABC123", "private_fn", "010");
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "index must be a decimal or 0x-prefixed byte value (0-255)");
}

TEST(HubManagement, ProbeDeviceRejectsUnknownProbeName) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.set_diagnostic_probes_enabled(true);

  const auto result = component.probe_device("ABC123", "unknown4a", "0");
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message,
            "unknown probe \"unknown4a\" (expected private_fn, private_fn_sub, status_ext, status_ext_fn6, "
            "status_ext_fn9, get_info1, get_info2, general_info3, private2, private2_short, status_mp_fp, "
            "status_fp_high, limitation, "
            "subscribe_event, unsubscribe_event, read_object, register_00, or register_01)");
  EXPECT_FALSE(result.terminal_refusal) << "an unrecognized probe name is validated up front by probe_sweep(), "
                                           "not by looping until this flag stops it";
}

TEST(HubManagement, ProbeDeviceNoResponseFails) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.set_diagnostic_probes_enabled(true);
  // No queued RX: send_and_receive times out.

  const auto result = component.probe_device("ABC123", "private_fn", "0x06");
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "no reply after " + std::to_string(EXCHANGE_RETRY_COUNT) +
                                " attempts (device asleep, unreachable, or silently ignoring this opcode)");
  EXPECT_FALSE(result.terminal_refusal)
      << "one silent index doesn't mean the rest of a sweep will be -- a sweep must keep going";
}

TEST(HubManagement, ProbeDeviceSuccessReportsRawReplyHexAndCmd) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.set_diagnostic_probes_enabled(true);
  radio.queue_rx(frame_to_packet(make_private_function_reply(component.node_id_)));

  const auto result = component.probe_device("ABC123", "private_fn", "0x06");
  EXPECT_TRUE(result.success);
  EXPECT_FALSE(result.verified) << "a probe reply's meaning is not known, so it is never verified";
  EXPECT_EQ(result.probe_name, "private_fn");
  EXPECT_EQ(result.probe_index, "0x06");
  EXPECT_TRUE(result.has_response_cmd);
  EXPECT_EQ(result.response_cmd, CMD_PRIVATE_RESP);
  EXPECT_FALSE(result.response_hex.empty());
}

TEST(HubManagement, ProbeDeviceErrorResponseReportsDecodedResultCode) {
  // The exact shape a real awning returned to a general_info3 probe: CMD_ERROR_RESP with
  // RESULT_COMMAND_UNKNOWN (0x08).
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.set_diagnostic_probes_enabled(true);
  radio.queue_rx(frame_to_packet(make_error_response(component.node_id_, RESULT_COMMAND_UNKNOWN)));

  const auto result = component.probe_device("ABC123", "private_fn", "0x06");
  EXPECT_TRUE(result.success) << "CMD_ERROR_RESP is a real, decoded reply, not a probe failure";
  EXPECT_TRUE(result.has_response_cmd);
  EXPECT_EQ(result.response_cmd, CMD_ERROR_RESP);
  EXPECT_FALSE(result.response_hex.empty()) << "the raw reply is still reported alongside the decode";
  EXPECT_TRUE(result.has_result_code);
  EXPECT_EQ(result.result_code, RESULT_COMMAND_UNKNOWN);
  EXPECT_NE(result.message.find(command_result_name(RESULT_COMMAND_UNKNOWN)), std::string::npos);
}

TEST(HubManagement, ProbeDeviceReportsDecodedLimitationReplyBesideRawHex) {
  // The decode follows the reply, not the probe name, so any probe drawing a 0x26 reports it.
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.set_diagnostic_probes_enabled(true);
  radio.queue_rx(
      frame_to_packet(test::make_limitation_reply(component.node_id_, PROBE_DEVICE_NODE_ID, 0xBA00, 0x02, 0x1D)));

  const auto result = component.probe_device("ABC123", "private_fn", "0x06");
  ASSERT_TRUE(result.success) << result.message;
  EXPECT_EQ(result.response_cmd, CMD_LIMITATION_STATUS_RESP);
  EXPECT_EQ(result.decoded,
            "limitation (assumed layout): param=MP value=93% (BA 00) originator=rain_sensor(0x02) time=0x1D (900 s)");
  EXPECT_NE(result.message.find("(LIMITATION_STATUS_RESP) hex="), std::string::npos) << result.message;
  EXPECT_NE(result.message.find("[" + result.decoded + "]"), std::string::npos) << result.message;
}

TEST(HubManagement, ProbeDeviceNonLimitationReplyHasNoDecodedText) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.set_diagnostic_probes_enabled(true);
  radio.queue_rx(frame_to_packet(make_private_function_reply(component.node_id_)));

  const auto result = component.probe_device("ABC123", "private_fn", "0x06");
  ASSERT_TRUE(result.success);
  EXPECT_TRUE(result.decoded.empty());
}

TEST(HubManagement, ProbeDeviceLimitationReplyNeverUpdatesDeviceRecord) {
  // A limitation reply is rendered, never applied (ADR 0024): no state, no result code.
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.set_diagnostic_probes_enabled(true);
  auto *dev = component.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  dev->position = 42.0F;
  dev->target = 42.0F;
  dev->is_stopped = true;
  dev->limited_by_rain = false;
  const uint8_t had_result_code = dev->last_result_code;
  radio.queue_rx(
      frame_to_packet(test::make_limitation_reply(component.node_id_, PROBE_DEVICE_NODE_ID, 0xBA00, 0x02, 0x1D)));

  ASSERT_TRUE(component.probe_device("ABC123", "private_fn", "0x06").success);

  EXPECT_FLOAT_EQ(dev->position, 42.0F);
  EXPECT_FLOAT_EQ(dev->target, 42.0F);
  EXPECT_TRUE(dev->is_stopped);
  EXPECT_FALSE(dev->limited_by_rain) << "a rain-sensor limitation reply must not flip the rain flag";
  EXPECT_EQ(dev->last_result_code, had_result_code);
}

TEST(HubManagement, ProbeSweepAppendsDecodedLimitationToTheIndexLine) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.set_diagnostic_probes_enabled(true);
  radio.queue_rx(frame_to_packet(test::make_limitation_reply(component.node_id_, PROBE_DEVICE_NODE_ID, 0xC800)));

  const auto result = component.probe_sweep("ABC123", "private_fn", "6", "6");
  ASSERT_TRUE(result.success);
  EXPECT_NE(result.message.find("index=0x06: cmd=0x26 hex="), std::string::npos) << result.message;
  EXPECT_NE(result.message.find("[limitation (assumed layout): param=MP value=100% (C8 00) "), std::string::npos)
      << result.message;
}

TEST(HubManagement, ProbeDeviceFunctionIdReplyNeverUpdatesDevicePosition) {
  // A probe's CMD_PRIVATE_RESP reply clears PRIVATE_RESPONSE_MIN_DATA_LEN (hub_status.cpp) and
  // would misread as a position update if it were ever routed through
  // apply_private_response_status(). DiagnosticProbes holds no hub pointer, only the three
  // DiagnosticProbesHub calls (ADR 0072), so this is a structural guarantee, not a runtime
  // check -- this test locks that guarantee in as a regression.
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.set_diagnostic_probes_enabled(true);
  auto *dev = component.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  dev->position = 42.0F;
  dev->target = 42.0F;
  dev->tilt = 17.0F;
  dev->is_stopped = true;
  radio.queue_rx(frame_to_packet(make_private_function_reply(component.node_id_)));

  const auto result = component.probe_device("ABC123", "private_fn", "0x06");
  ASSERT_TRUE(result.success);

  EXPECT_FLOAT_EQ(dev->position, 42.0F) << "probe reply must never update device position";
  EXPECT_FLOAT_EQ(dev->target, 42.0F) << "probe reply must never update device target";
  EXPECT_FLOAT_EQ(dev->tilt, 17.0F) << "probe reply must never update device tilt";
  EXPECT_TRUE(dev->is_stopped) << "probe reply must never update the stopped flag";
}

TEST(HubManagement, ProbeDeviceStatusMpFpSendsKlfStatusReadAndReportsRawReply) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.set_diagnostic_probes_enabled(true);
  radio.queue_rx(frame_to_packet(make_private_function_reply(component.node_id_)));

  // `index` is ignored for this probe, so any string (even an invalid one) must be accepted.
  const auto result = component.probe_device("ABC123", "status_mp_fp", "");
  ASSERT_TRUE(result.success) << result.message;
  EXPECT_EQ(result.probe_name, "status_mp_fp");
  EXPECT_TRUE(result.has_response_cmd);
  EXPECT_EQ(result.response_cmd, CMD_PRIVATE_RESP);
  EXPECT_FALSE(result.response_hex.empty());

  ASSERT_FALSE(radio.get_sent_data().empty());
  const auto &sent = radio.get_sent_data().front();
  IoFrame frame{};
  ASSERT_TRUE(parse(sent.data(), static_cast<uint8_t>(sent.size()), frame));
  EXPECT_EQ(frame.cmd, CMD_PRIVATE);
  const uint8_t expected[] = {0x01, 0xFE, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00};
  ASSERT_EQ(frame.data_len, sizeof(expected));
  EXPECT_EQ(0, std::memcmp(frame.data, expected, sizeof(expected)));
}

TEST(HubManagement, ProbeDeviceStatusFpHighSendsTheSecondBitmap) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.set_diagnostic_probes_enabled(true);
  radio.queue_rx(frame_to_packet(make_private_function_reply(component.node_id_)));

  const auto result = component.probe_device("ABC123", "status_fp_high", "0x43");
  ASSERT_TRUE(result.success) << result.message;
  EXPECT_EQ(result.probe_name, "status_fp_high");

  ASSERT_FALSE(radio.get_sent_data().empty());
  const auto &sent = radio.get_sent_data().front();
  IoFrame frame{};
  ASSERT_TRUE(parse(sent.data(), static_cast<uint8_t>(sent.size()), frame));
  EXPECT_EQ(frame.cmd, CMD_PRIVATE);
  const uint8_t expected[] = {0x01, 0x00, 0x43, 0x01, 0x01, 0x01};
  ASSERT_EQ(frame.data_len, sizeof(expected));
  EXPECT_EQ(0, std::memcmp(frame.data, expected, sizeof(expected)));
}

TEST(HubManagement, ProbeDeviceStatusFpHighRejectsInvalidBitmaps) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.set_diagnostic_probes_enabled(true);

  for (const char *bad : {"0", "0xFF"}) {
    const auto result = component.probe_device("ABC123", "status_fp_high", bad);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.message,
              "probe \"status_fp_high\" takes FP9-FP16 bitmap, bit 7 = FP9 ... bit 0 = FP16, at most 7 bits "
              "(e.g. 0x43 = FP10, FP15, FP16)");
  }
  const auto missing = component.probe_device("ABC123", "status_fp_high", "");
  EXPECT_FALSE(missing.success);
  EXPECT_EQ(missing.message, "index must be a decimal or 0x-prefixed byte value (0-255)");
  EXPECT_TRUE(radio.get_sent_data().empty());
}

TEST(HubManagement, ProbeSweepRefusesStatusFpHigh) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.set_diagnostic_probes_enabled(true);

  const auto result = component.probe_sweep("ABC123", "status_fp_high", "1", "3");
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "probe \"status_fp_high\" cannot be swept; use probe_device");
  EXPECT_TRUE(radio.get_sent_data().empty());
}

TEST(HubManagement, ProbeDeviceStatusMpFpReplyNeverUpdatesDeviceRecord) {
  // Both the reply shape heard from a real KLF 200 exchange (`04 2D 60 00 00 80 00 00 00`) and a
  // dry-state variant are CMD_PRIVATE_RESP frames long enough to look like a position report;
  // neither may reach the status decoder (ADR 0024).
  const std::vector<std::vector<uint8_t>> payloads = {
      {0x04, 0x2D, 0x60, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00},
      {0x04, 0x2D, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00},
  };
  for (const auto &payload : payloads) {
    TestableManagementComponent component;
    MockRadio radio;
    setup_component(component, radio);
    component.set_diagnostic_probes_enabled(true);
    auto *dev = component.get_device("ABC123");
    ASSERT_NE(dev, nullptr);
    dev->position = 42.0F;
    dev->target = 42.0F;
    dev->is_stopped = true;
    ASSERT_FALSE(dev->has_last_command);

    IoFrame reply{};
    init_frame(reply, true, false, true, false);
    const uint8_t device_node_id[3] = {0xAB, 0xC1, 0x23};
    set_dst(reply, component.node_id_);
    set_src(reply, device_node_id);
    set_cmd(reply, CMD_PRIVATE_RESP, payload.data(), static_cast<uint8_t>(payload.size()));
    radio.queue_rx(frame_to_packet(reply));

    const auto result = component.probe_device("ABC123", "status_mp_fp", "0");
    ASSERT_TRUE(result.success);
    EXPECT_FLOAT_EQ(dev->position, 42.0F);
    EXPECT_FLOAT_EQ(dev->target, 42.0F);
    EXPECT_FALSE(dev->has_last_command);
    EXPECT_FALSE(dev->limited_by_rain);
  }
}

namespace {
/// Serializes the `index`-th frame the radio transmitted and returns its bytes.
std::vector<uint8_t> sent_frame_bytes(const MockRadio &radio, size_t index) {
  IoFrame frame{};
  EXPECT_TRUE(
      parse(radio.get_sent_data()[index].data(), static_cast<uint8_t>(radio.get_sent_data()[index].size()), frame));
  uint8_t bytes[FRAME_MAX_SIZE] = {0};
  const uint8_t len = serialize(frame, bytes, sizeof(bytes));
  return std::vector<uint8_t>(bytes, bytes + len);
}
}  // namespace

TEST(HubManagement, ProbeDeviceLimitationIndexZeroSendsMinimumRead) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.set_diagnostic_probes_enabled(true);
  radio.queue_rx(
      frame_to_packet(test::make_limitation_reply(component.node_id_, PROBE_DEVICE_NODE_ID, 0xBA00, 0x02, 0x1D)));

  const auto result = component.probe_device("ABC123", "limitation", "0");
  ASSERT_TRUE(result.success) << result.message;
  ASSERT_FALSE(radio.get_sent_data().empty());
  // Byte for byte the request a KLF 200 sends for the minimum (own and device ids aside).
  EXPECT_EQ(sent_frame_bytes(radio, 0),
            (std::vector<uint8_t>{0x4B, 0x00, 0xAB, 0xC1, 0x23, 0xC0, 0xFF, 0xEE, 0x25, 0x80, 0x00, 0x00}));
  // The exact substring a reporter reads in the INFO log and the action-result event.
  EXPECT_NE(result.message.find("value=93% (BA 00) originator=rain_sensor(0x02) time=0x1D (900 s)"), std::string::npos)
      << result.message;
}

TEST(HubManagement, ProbeDeviceLimitationIndexOneSendsMaximumRead) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.set_diagnostic_probes_enabled(true);
  radio.queue_rx(frame_to_packet(test::make_limitation_reply(component.node_id_, PROBE_DEVICE_NODE_ID, 0xC800)));

  ASSERT_TRUE(component.probe_device("ABC123", "limitation", "1").success);
  ASSERT_FALSE(radio.get_sent_data().empty());
  EXPECT_EQ(sent_frame_bytes(radio, 0),
            (std::vector<uint8_t>{0x4B, 0x00, 0xAB, 0xC1, 0x23, 0xC0, 0xFF, 0xEE, 0x25, 0xC0, 0x00, 0x00}));
}

TEST(HubManagement, ProbeDeviceLimitationRejectsEveryOtherIndexWithoutTransmitting) {
  // The index is the KLF API's LimitationType, never the on-air selector byte: 0x80 and 0xC0
  // typed by a user must not reach the radio, and neither may any other value.
  for (const char *index : {"2", "0x80", "0xC0", "255"}) {
    SCOPED_TRACE(index);
    TestableManagementComponent component;
    MockRadio radio;
    setup_component(component, radio);
    component.set_diagnostic_probes_enabled(true);

    const auto result = component.probe_device("ABC123", "limitation", index);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.message, "probe \"limitation\" takes index 0 (minimum) or 1 (maximum)");
    EXPECT_TRUE(radio.get_sent_data().empty());
  }
}

TEST(HubManagement, ProbeSweepLimitationOverBothIndicesReportsDecodedLines) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.set_diagnostic_probes_enabled(true);
  radio.queue_rx(frame_to_packet(test::make_limitation_reply(component.node_id_, PROBE_DEVICE_NODE_ID, 0x0000)));
  radio.queue_rx(frame_to_packet(test::make_limitation_reply(component.node_id_, PROBE_DEVICE_NODE_ID, 0xC800)));

  const auto result = component.probe_sweep("ABC123", "limitation", "0", "1");
  ASSERT_TRUE(result.success) << result.message;
  EXPECT_NE(result.message.find("2 answered"), std::string::npos) << result.message;
  EXPECT_NE(result.message.find("index=0x00: cmd=0x26 hex="), std::string::npos) << result.message;
  EXPECT_NE(result.message.find("[limitation (assumed layout): param=MP value=0% (00 00)"), std::string::npos)
      << result.message;
  EXPECT_NE(result.message.find("index=0x01: cmd=0x26 hex="), std::string::npos) << result.message;
  EXPECT_NE(result.message.find("[limitation (assumed layout): param=MP value=100% (C8 00)"), std::string::npos)
      << result.message;
  ASSERT_GE(radio.get_sent_data().size(), 2u);
  EXPECT_EQ(sent_frame_bytes(radio, 0)[9], 0x80);
  EXPECT_EQ(sent_frame_bytes(radio, 1)[9], 0xC0);
}

TEST(HubManagement, ProbeSweepLimitationRejectsARangePastTheMaximumAsAWhole) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.set_diagnostic_probes_enabled(true);

  const auto result = component.probe_sweep("ABC123", "limitation", "0", "2");
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "probe \"limitation\" takes index 0 (minimum) or 1 (maximum)");
  EXPECT_TRUE(radio.get_sent_data().empty()) << "no index of a rejected range may be sent";
}

TEST(HubManagement, ProbeSweepRejectsStatusMpFpNoIndexProbeWithoutLooping) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.set_diagnostic_probes_enabled(true);

  const auto result = component.probe_sweep("ABC123", "status_mp_fp", "0", "15");
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "probe \"status_mp_fp\" takes no index; use probe_device");
  EXPECT_TRUE(radio.get_sent_data().empty());
}

TEST(HubManagement, ApiProbeDevicePublishesManagementResultEvent) {
  esphome::api::APIServer api_server;
  esphome::api::ScopedGlobalApiServer scoped_api_server(api_server);
  api_server.reset();

  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.set_diagnostic_probes_enabled(true);
  radio.queue_rx(frame_to_packet(make_private_function_reply(component.node_id_)));

  component.api_probe_device_("ABC123", "private_fn", "0x06");

  ASSERT_EQ(api_server.events_.size(), 1u);
  const auto &event = api_server.events_.front();
  EXPECT_EQ(event.event_type, "esphome.home_io_control_action_result");
  EXPECT_EQ(event.data.at("action"), "probe_device");
  EXPECT_EQ(event.data.at("device_id"), "ABC123");
  EXPECT_EQ(event.data.at("success"), "true");
  EXPECT_EQ(event.data.at("probe"), "private_fn");
  EXPECT_EQ(event.data.at("index"), "0x06");
  EXPECT_EQ(event.data.at("response_cmd"), "04");
  EXPECT_FALSE(event.data.at("response_hex").empty());
}

TEST(HubManagement, ProbeSweepRejectsInvertedRange) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.set_diagnostic_probes_enabled(true);

  const auto result = component.probe_sweep("ABC123", "private_fn", "9", "6");
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "last_index must be >= first_index");
}

TEST(HubManagement, ProbeSweepRejectsRangeTooWide) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.set_diagnostic_probes_enabled(true);

  const auto result = component.probe_sweep("ABC123", "private_fn", "0x00", "0x20");
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "sweep range too wide: 33 indices, max 16");
}

TEST(HubManagement, ProbeSweepRejectsInvalidIndexArguments) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.set_diagnostic_probes_enabled(true);

  const auto result = component.probe_sweep("ABC123", "private_fn", "nope", "6");
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "first_index/last_index must be decimal or 0x-prefixed byte values (0-255)");
}

TEST(HubManagement, ProbeSweepReportsOneLinePerIndex) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.set_diagnostic_probes_enabled(true);
  // Index 6 answers, index 7 gets no reply -- one queued RX only.
  radio.queue_rx(frame_to_packet(make_private_function_reply(component.node_id_)));

  const auto result = component.probe_sweep("ABC123", "private_fn", "6", "7");
  EXPECT_TRUE(result.success) << "the sweep action itself succeeds even if individual indices don't answer";
  EXPECT_NE(result.message.find("1 answered"), std::string::npos)
      << "expected 1 answered of 2 attempted, got: " << result.message;
  EXPECT_NE(result.message.find("index=0x06: cmd=0x04"), std::string::npos) << result.message;
  EXPECT_NE(result.message.find("index=0x07: no reply"), std::string::npos) << result.message;
}

TEST(HubManagement, ProbeSweepStopsEarlyWhenDeviceIsMoving) {
  // A terminal refusal (here, the device being mid-transaction for the whole sweep) must stop the
  // loop after the first index rather than repeating the same refusal for every remaining one in
  // the requested range.
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.set_diagnostic_probes_enabled(true);
  auto *dev = component.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  dev->is_stopped = false;

  const auto result = component.probe_sweep("ABC123", "private_fn", "6", "8");
  EXPECT_FALSE(result.success) << "a sweep cut short by a terminal refusal did not complete the requested range";
  EXPECT_NE(result.message.find("stopping sweep"), std::string::npos)
      << "device-moving mid-sweep should stop rather than repeat the same refusal: " << result.message;
  EXPECT_EQ(result.message.find("index=0x08"), std::string::npos)
      << "sweep should not have reached the third index: " << result.message;
}

TEST(HubManagement, ProbeSweepRejectsUnknownProbeNameWithoutLooping) {
  // An unrecognized probe name must be rejected once, up front, not retried for every index in
  // the range.
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.set_diagnostic_probes_enabled(true);

  const auto result = component.probe_sweep("ABC123", "unknown4a", "0", "5");
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message,
            "unknown probe \"unknown4a\" (expected private_fn, private_fn_sub, status_ext, status_ext_fn6, "
            "status_ext_fn9, get_info1, get_info2, general_info3, private2, private2_short, status_mp_fp, "
            "status_fp_high, limitation, "
            "subscribe_event, unsubscribe_event, read_object, register_00, or register_01)");
  EXPECT_EQ(result.message.find("index="), std::string::npos)
      << "an invalid probe name must not enter the per-index loop at all: " << result.message;
}

TEST(HubManagement, ProbeSweepRejectsNoIndexProbeWithoutLooping) {
  // general_info3 takes no index (ProbeDescriptor::needs_index == false); sweeping it would send
  // the identical zero-payload frame up to PROBE_SWEEP_MAX_INDICES times for no new information.
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.set_diagnostic_probes_enabled(true);

  const auto result = component.probe_sweep("ABC123", "general_info3", "0", "15");
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "probe \"general_info3\" takes no index; use probe_device");
  EXPECT_EQ(result.message.find("index="), std::string::npos)
      << "a no-index probe must not enter the per-index loop at all: " << result.message;
}

TEST(HubManagement, ProbeSweepRejectsGetInfo1NoIndexProbeWithoutLooping) {
  // get_info1 (0x54) carries no payload -- like general_info3, ProbeDescriptor::needs_index is
  // false, so probe_sweep() must reject it up front rather than resend the identical frame.
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.set_diagnostic_probes_enabled(true);

  const auto result = component.probe_sweep("ABC123", "get_info1", "0", "15");
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "probe \"get_info1\" takes no index; use probe_device");
  EXPECT_EQ(result.message.find("index="), std::string::npos)
      << "a no-index probe must not enter the per-index loop at all: " << result.message;
}

TEST(HubManagement, ProbeSweepRejectsUnregisteredDeviceWithoutLooping) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.set_diagnostic_probes_enabled(true);

  const auto result = component.probe_sweep("123456", "private_fn", "0", "5");
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "device is not registered on this hub");
  EXPECT_EQ(result.message.find("index="), std::string::npos)
      << "an unregistered device must not enter the per-index loop at all: " << result.message;
}

// --- subscribe_event: the one probe that changes device state (ADR 0060) ---------------------

namespace {

/// A frame from the probe device with command 0x70, the one a subscribed device is expected to send.
IoFrame make_device_event_frame(const uint8_t dst[3]) {
  IoFrame frame{};
  init_frame(frame, true, false, true, false);
  set_dst(frame, dst);
  set_src(frame, PROBE_DEVICE_NODE_ID);
  const uint8_t payload[4] = {0x20, 0x05, 0x06, 0x2A};
  set_cmd(frame, CMD_SET_CONFIG1_RESP, payload, sizeof(payload));
  return frame;
}

}  // namespace

TEST(HubManagement, ProbeDeviceSubscribeEventSendsTheAllowlistedEventForEachIndex) {
  struct Case {
    const char *index;
    uint8_t hi;
    uint8_t lo;
  };
  for (const Case &c : {Case{"0", 0x20, 0x01}, Case{"1", 0x20, 0x05}, Case{"0x01", 0x20, 0x05}}) {
    SCOPED_TRACE(c.index);
    TestableManagementComponent component;
    MockRadio radio;
    setup_component(component, radio);
    component.set_diagnostic_probes_enabled(true);
    radio.queue_rx(frame_to_packet(make_error_response(component.node_id_, RESULT_COMMAND_UNKNOWN)));

    const auto result = component.probe_device("ABC123", "subscribe_event", c.index);
    ASSERT_TRUE(result.success) << result.message;
    ASSERT_FALSE(radio.get_sent_data().empty());
    EXPECT_EQ(sent_frame_bytes(radio, 0),
              (std::vector<uint8_t>{0x4B, 0x00, 0xAB, 0xC1, 0x23, 0xC0, 0xFF, 0xEE, 0x73, 0x01, c.hi, c.lo}));
  }
}

TEST(HubManagement, ProbeDeviceSubscribeEventRejectsEveryOtherIndexWithoutTransmitting) {
  // 0x2001 / 0x2005 typed as an index, or any value past 1, must never reach the radio.
  for (const char *index : {"2", "0x2001", "0x2005", "255", "", "x"}) {
    SCOPED_TRACE(index);
    TestableManagementComponent component;
    MockRadio radio;
    setup_component(component, radio);
    component.set_diagnostic_probes_enabled(true);

    const auto result = component.probe_device("ABC123", "subscribe_event", index);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(radio.get_sent_data().empty());
  }
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.set_diagnostic_probes_enabled(true);
  EXPECT_EQ(component.probe_device("ABC123", "subscribe_event", "2").message,
            "probe \"subscribe_event\" takes index 0 (battery level event 0x2001) or 1 (local sensor event 0x2005)");
}

TEST(HubManagement, ProbeDeviceSubscribeEventIsRefusedWithoutDiagnosticProbes) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);

  const auto result = component.probe_device("ABC123", "subscribe_event", "1");
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "diagnostic probes are not enabled; set diagnostic_probes: true in YAML");
  EXPECT_TRUE(radio.get_sent_data().empty());
}

TEST(HubManagement, ProbeDeviceSubscribeEventIsRefusedWhileTheDeviceMoves) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.set_diagnostic_probes_enabled(true);
  auto *dev = component.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  dev->is_stopped = false;

  const auto result = component.probe_device("ABC123", "subscribe_event", "1");
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "device is moving; refusing to probe mid-transaction");
  EXPECT_TRUE(radio.get_sent_data().empty());
}

TEST(HubManagement, ProbeDeviceSubscribeEventNeverReachesAnUnpairedDevice) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.set_diagnostic_probes_enabled(true);

  const auto result = component.probe_device("123456", "subscribe_event", "1");
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "device is not registered on this hub");
  EXPECT_TRUE(radio.get_sent_data().empty());
}

TEST(HubManagement, ProbeDeviceUnsubscribeEventSendsActionZeroForTheAllowlistedEvent) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.set_diagnostic_probes_enabled(true);
  radio.queue_rx(frame_to_packet(make_error_response(component.node_id_, RESULT_COMMAND_UNKNOWN)));

  const auto result = component.probe_device("ABC123", "unsubscribe_event", "1");
  ASSERT_TRUE(result.success) << result.message;
  EXPECT_EQ(sent_frame_bytes(radio, 0),
            (std::vector<uint8_t>{0x4B, 0x00, 0xAB, 0xC1, 0x23, 0xC0, 0xFF, 0xEE, 0x73, 0x00, 0x20, 0x05}));
}

TEST(HubManagement, ProbeDeviceUnsubscribeEventRejectsOtherIndicesAndCannotBeSwept) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.set_diagnostic_probes_enabled(true);

  EXPECT_FALSE(component.probe_device("ABC123", "unsubscribe_event", "2").success);
  EXPECT_FALSE(component.probe_device("ABC123", "unsubscribe_event", "0x2005").success);
  EXPECT_EQ(component.probe_sweep("ABC123", "unsubscribe_event", "0", "1").message,
            "probe \"unsubscribe_event\" cannot be swept; use probe_device");
  EXPECT_TRUE(radio.get_sent_data().empty());
}

TEST(HubManagement, ProbeSweepRefusesSubscribeEvent) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.set_diagnostic_probes_enabled(true);

  const auto result = component.probe_sweep("ABC123", "subscribe_event", "0", "1");
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "probe \"subscribe_event\" cannot be swept; use probe_device");
  EXPECT_TRUE(radio.get_sent_data().empty());
}

TEST(HubManagement, ProbeDeviceSubscribeEventDecodesAnErrorCode) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.set_diagnostic_probes_enabled(true);
  radio.queue_rx(frame_to_packet(make_error_response(component.node_id_, RESULT_COMMAND_UNKNOWN)));

  const auto result = component.probe_device("ABC123", "subscribe_event", "1");
  ASSERT_TRUE(result.success) << result.message;
  EXPECT_EQ(result.response_cmd, CMD_ERROR_RESP);
  EXPECT_NE(result.message.find("[" + std::string(command_result_name(RESULT_COMMAND_UNKNOWN)) + ": "),
            std::string::npos)
      << result.message;
}

TEST(HubManagement, ProbeDeviceSubscribeEventReportsA0x70ReplyRawAndNeverAppliesIt) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.set_diagnostic_probes_enabled(true);
  auto *dev = component.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  dev->position = 42.0F;
  dev->target = 42.0F;
  dev->is_stopped = true;
  const auto battery_before = dev->battery;
  radio.queue_rx(frame_to_packet(make_device_event_frame(component.node_id_)));

  const auto result = component.probe_device("ABC123", "subscribe_event", "1");
  ASSERT_TRUE(result.success) << result.message;
  EXPECT_EQ(result.response_cmd, CMD_SET_CONFIG1_RESP);
  EXPECT_NE(result.message.find("SET_CONFIG1_RESP"), std::string::npos) << result.message;
  EXPECT_FLOAT_EQ(dev->position, 42.0F);
  EXPECT_FLOAT_EQ(dev->target, 42.0F);
  EXPECT_TRUE(dev->is_stopped);
  EXPECT_EQ(dev->battery, battery_before);
}

TEST(HubManagement, ProbeDeviceSubscribeEventRecordsAStatusUpdateAnswerAsTheFirstReport) {
  // The RS100 Solar io answers a subscription with a 0x71 carrying the event (the captured answer).
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.set_diagnostic_probes_enabled(true);
  auto *dev = component.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  dev->position = 42.0F;
  dev->target = 42.0F;
  dev->is_stopped = true;
  radio.queue_rx(frame_to_packet(corpus_test::sensor_event_report(PROBE_DEVICE_NODE_ID, component.node_id_)));

  const auto result = component.probe_device("ABC123", "subscribe_event", "1");
  ASSERT_TRUE(result.success) << result.message;
  EXPECT_EQ(result.response_cmd, CMD_STATUS_UPDATE);
  EXPECT_EQ(result.decoded, "event 0x2005, 5 data bytes");
  EXPECT_NE(result.message.find("[event 0x2005, 5 data bytes]"), std::string::npos) << result.message;
  EXPECT_EQ(component.device_event_count(), 1u);
  EXPECT_NE(component.last_device_event().find("ABC123 0x71 05 60 20 05 05 06 00 00 00 00"), std::string::npos)
      << component.last_device_event();
  EXPECT_FLOAT_EQ(dev->position, 42.0F);
  EXPECT_FLOAT_EQ(dev->target, 42.0F);
  EXPECT_TRUE(dev->is_stopped);
}

TEST(HubManagement, ProbeDeviceSubscribeEventAcceptedWithoutAReplyIsReportedAndResentOnce) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.set_diagnostic_probes_enabled(true);
  const uint8_t challenge[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
  IoFrame chal{};
  init_frame(chal, true, false, false, false);
  set_dst(chal, component.node_id_);
  set_src(chal, PROBE_DEVICE_NODE_ID);
  set_cmd(chal, CMD_CHALLENGE_REQ, challenge, sizeof(challenge));
  radio.queue_rx(frame_to_packet(chal));  // challenged, then silence

  const auto result = component.probe_device("ABC123", "subscribe_event", "1");
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "device accepted the probe request but sent no response");
  EXPECT_EQ(radio.get_send_count(), 3)
      << "the subscribe, our challenge answer, and the one re-send (left unanswered here); no third copy";
}

// --- read_object: the data-transport read (ADR 0062) ----------------------------------------------

namespace {

/// A frame from the probe device to the hub with the given command and payload.
IoFrame make_transfer_reply(const uint8_t dst[3], uint8_t cmd, const std::vector<uint8_t> &payload) {
  IoFrame frame{};
  init_frame(frame, true, false, true, false);
  set_dst(frame, dst);
  set_src(frame, PROBE_DEVICE_NODE_ID);
  EXPECT_TRUE(set_cmd(frame, cmd, payload.data(), static_cast<uint8_t>(payload.size())));
  return frame;
}

/// The 18-byte block `first, first+1, ...` of a transfer, echoing handle and chunk.
IoFrame make_block(const uint8_t dst[3], uint8_t handle, uint16_t chunk, uint8_t first, uint8_t count) {
  std::vector<uint8_t> payload = {handle, static_cast<uint8_t>(chunk >> 8), static_cast<uint8_t>(chunk & 0xFF)};
  for (uint8_t i = 0; i < count; i++)
    payload.push_back(static_cast<uint8_t>(first + i));
  return make_transfer_reply(dst, CMD_UNKNOWN4A_RESP, payload);
}

/// The `index`-th transmitted frame from its command byte on (command and payload).
std::vector<uint8_t> sent_from_cmd(const MockRadio &radio, size_t index) {
  const auto bytes = sent_frame_bytes(radio, index);
  return std::vector<uint8_t>(bytes.begin() + 8, bytes.end());
}

/// The command byte of the `index`-th transmitted frame.
uint8_t sent_cmd(const MockRadio &radio, size_t index) {
  const auto bytes = sent_frame_bytes(radio, index);
  return bytes.at(8);
}

}  // namespace

// --- register_00 / register_01: read-only register probes (ADR 0063) -------------------------------

namespace {

/// A 0x21 from the probe device echoing `0C 60 <hi> <lo>` followed by `value`.
IoFrame make_register_reply(const uint8_t dst[3], uint8_t reg_hi, uint8_t reg_lo, const std::vector<uint8_t> &value) {
  std::vector<uint8_t> payload = {0x0C, 0x60, reg_hi, reg_lo};
  payload.insert(payload.end(), value.begin(), value.end());
  return make_transfer_reply(dst, CMD_WRITE_PRIVATE_ACK, payload);
}

void make_hot_water_device(TestableManagementComponent &component) {
  auto *dev = component.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  test::set_kind(*dev, DeviceType::DOMESTIC_HOT_WATER);
}

}  // namespace

TEST(HubManagement, RegisterProbesSendTheReadRequestForTheirFamily) {
  struct Case {
    const char *probe;
    const char *index;
    std::vector<uint8_t> expected_payload;
  };
  const Case cases[] = {{"register_00", "0x2A", {0x0C, 0x60, 0x00, 0x2A}},
                        {"register_01", "0x15", {0x0C, 0x60, 0x01, 0x15}},
                        {"register_00", "0", {0x0C, 0x60, 0x00, 0x00}},
                        {"register_01", "255", {0x0C, 0x60, 0x01, 0xFF}}};
  for (const auto &c : cases) {
    SCOPED_TRACE(std::string(c.probe) + " " + c.index);
    TestableManagementComponent component;
    MockRadio radio;
    setup_component(component, radio);
    component.set_diagnostic_probes_enabled(true);
    make_hot_water_device(component);

    component.probe_device("ABC123", c.probe, c.index);

    ASSERT_FALSE(radio.get_sent_data().empty());
    const auto sent = sent_from_cmd(radio, 0);
    ASSERT_EQ(sent.size(), 5u);
    EXPECT_EQ(sent[0], CMD_WRITE_PRIVATE);
    EXPECT_EQ(std::vector<uint8_t>(sent.begin() + 1, sent.end()), c.expected_payload);
  }
}

TEST(HubManagement, RegisterProbesNeverSendTheWriteDirectionForAnyIndex) {
  // 0x61 is the register write. Every index of both probes, on a device that accepts them, must frame 0x60.
  for (const char *probe : {"register_00", "register_01"}) {
    for (unsigned index = 0; index <= 255; index++) {
      TestableManagementComponent component;
      MockRadio radio;
      setup_component(component, radio);
      component.set_diagnostic_probes_enabled(true);
      make_hot_water_device(component);

      component.probe_device("ABC123", probe, std::to_string(index));

      ASSERT_FALSE(radio.get_sent_data().empty()) << probe << " " << index;
      for (size_t i = 0; i < radio.get_sent_data().size(); i++) {
        const auto sent = sent_from_cmd(radio, i);
        ASSERT_GE(sent.size(), 3u);
        if (sent[0] == CMD_WRITE_PRIVATE) {
          ASSERT_EQ(sent[2], HEATING_DIRECTION_GET) << probe << " " << index << ": never the write direction";
        }
      }
    }
  }
}

TEST(HubManagement, RegisterProbeReportsTheValueBytesOfAnEchoedRegister) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.set_diagnostic_probes_enabled(true);
  make_hot_water_device(component);
  radio.queue_rx(frame_to_packet(make_register_reply(component.node_id_, 0x01, 0x15, {0xD2, 0x00})));

  const auto result = component.probe_device("ABC123", "register_01", "0x15");

  ASSERT_TRUE(result.success) << result.message;
  EXPECT_EQ(result.response_cmd, CMD_WRITE_PRIVATE_ACK);
  EXPECT_EQ(result.decoded, "value: D2 00");
  EXPECT_NE(result.message.find("[value: D2 00]"), std::string::npos) << result.message;
  EXPECT_FALSE(result.response_hex.empty()) << "the raw reply is still reported";
}

TEST(HubManagement, RegisterProbeDoesNotSplitAValueOffAnAckThatDoesNotEchoTheRegister) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.set_diagnostic_probes_enabled(true);
  make_hot_water_device(component);
  radio.queue_rx(frame_to_packet(make_register_reply(component.node_id_, 0x01, 0x16, {0xD2, 0x00})));

  const auto result = component.probe_device("ABC123", "register_01", "0x15");

  ASSERT_TRUE(result.success) << result.message;
  EXPECT_TRUE(result.decoded.empty());
  EXPECT_EQ(result.message.find("value:"), std::string::npos) << result.message;
}

TEST(HubManagement, RegisterProbeDecodesAnErrorAnswer) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.set_diagnostic_probes_enabled(true);
  make_hot_water_device(component);
  radio.queue_rx(frame_to_packet(make_error_response(component.node_id_, RESULT_COMMAND_UNKNOWN)));

  const auto result = component.probe_device("ABC123", "register_00", "1");

  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.response_cmd, CMD_ERROR_RESP);
  EXPECT_TRUE(result.has_result_code);
  EXPECT_EQ(result.result_code, RESULT_COMMAND_UNKNOWN);
  EXPECT_TRUE(result.decoded.empty());
}

TEST(HubManagement, RegisterProbeIsAcceptedOnARegisterHeater) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.set_diagnostic_probes_enabled(true);
  make_climate_device(component);
  radio.queue_rx(frame_to_packet(make_register_reply(component.node_id_, 0x01, 0x01, {0x01})));

  const auto result = component.probe_device("ABC123", "register_01", "1");

  EXPECT_TRUE(result.success) << result.message;
  EXPECT_EQ(result.decoded, "value: 01");
}

TEST(HubManagement, RegisterProbeIsRefusedForEveryOtherKindWithoutTransmitting) {
  for (const DeviceType kind : {DeviceType::ROLLER_SHUTTER, DeviceType::VERTICAL_EXTERIOR_AWNING, DeviceType::LIGHT,
                                DeviceType::ON_OFF_SWITCH, DeviceType::LOCK, DeviceType::UNKNOWN}) {
    SCOPED_TRACE(static_cast<unsigned>(kind));
    TestableManagementComponent component;
    MockRadio radio;
    setup_component(component, radio);
    component.set_diagnostic_probes_enabled(true);
    auto *dev = component.get_device("ABC123");
    ASSERT_NE(dev, nullptr);
    test::set_kind(*dev, kind);

    const auto result = component.probe_device("ABC123", "register_01", "1");

    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.terminal_refusal) << "the kind does not change partway through a sweep";
    EXPECT_NE(result.message.find("register-protocol heater or a domestic hot water device"), std::string::npos)
        << result.message;
    EXPECT_TRUE(radio.get_sent_data().empty());
  }
}

TEST(HubManagement, RegisterProbesSweepAndStopAtTheSixteenIndexCap) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.set_diagnostic_probes_enabled(true);
  make_hot_water_device(component);

  const auto too_wide = component.probe_sweep("ABC123", "register_00", "0", "16");
  EXPECT_FALSE(too_wide.success);
  EXPECT_NE(too_wide.message.find("sweep range too wide"), std::string::npos) << too_wide.message;
  EXPECT_TRUE(radio.get_sent_data().empty());

  const auto swept = component.probe_sweep("ABC123", "register_00", "0", "1");
  EXPECT_TRUE(swept.success) << swept.message;
  EXPECT_NE(swept.message.find("index=0x00"), std::string::npos);
  EXPECT_NE(swept.message.find("index=0x01"), std::string::npos);
}

TEST(HubManagement, RegisterProbeSweepOnACoverStopsAfterTheFirstRefusal) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.set_diagnostic_probes_enabled(true);
  auto *dev = component.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  test::set_kind(*dev, DeviceType::ROLLER_SHUTTER);

  const auto swept = component.probe_sweep("ABC123", "register_01", "0", "5");

  EXPECT_FALSE(swept.success);
  EXPECT_NE(swept.message.find("stopping sweep"), std::string::npos) << swept.message;
  EXPECT_TRUE(radio.get_sent_data().empty());
}

TEST(HubManagement, ReadObjectOpensPullsEveryBlockAndClosesTheTransfer) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.set_diagnostic_probes_enabled(true);
  // 40 bytes = 3 blocks (18 + 18 + 4), requested counting down from 3.
  radio.queue_rx(
      frame_to_packet(make_transfer_reply(component.node_id_, CMD_TRANSFER_OPEN_RESP, {0x10, 0x00, 0x00, 0x28})));
  radio.queue_rx(frame_to_packet(make_block(component.node_id_, 0x10, 3, 0, 18)));
  radio.queue_rx(frame_to_packet(make_block(component.node_id_, 0x10, 2, 18, 18)));
  radio.queue_rx(frame_to_packet(make_block(component.node_id_, 0x10, 1, 36, 4)));
  radio.queue_rx(frame_to_packet(make_block(component.node_id_, 0x10, 0, 0, 0)));

  const auto result = component.probe_device("ABC123", "read_object", "0");
  ASSERT_TRUE(result.success) << result.message;
  ASSERT_EQ(radio.get_sent_data().size(), 5u);
  // Open for object 0x8100: SAAC originator, class 0, 00, 81 00, four zero bytes.
  EXPECT_EQ(sent_frame_bytes(radio, 0), (std::vector<uint8_t>{0x51, 0x00, 0xAB, 0xC1, 0x23, 0xC0, 0xFF, 0xEE, 0x46,
                                                              0x08, 0x00, 0x00, 0x81, 0x00, 0x00, 0x00, 0x00, 0x00}));
  // Blocks counting down, then the close.
  EXPECT_EQ(sent_from_cmd(radio, 1), (std::vector<uint8_t>{0x4A, 0x10, 0x00, 0x03}));
  EXPECT_EQ(sent_from_cmd(radio, 2), (std::vector<uint8_t>{0x4A, 0x10, 0x00, 0x02}));
  EXPECT_EQ(sent_from_cmd(radio, 3), (std::vector<uint8_t>{0x4A, 0x10, 0x00, 0x01}));
  EXPECT_EQ(sent_from_cmd(radio, 4), (std::vector<uint8_t>{0x4A, 0x10, 0x00, 0x00}));
  // The data runs forward across blocks.
  std::string expected;
  for (int i = 0; i < 40; i++) {
    char buf[4];
    snprintf(buf, sizeof(buf), i == 0 ? "%02X" : " %02X", i);
    expected += buf;
  }
  EXPECT_EQ(result.response_hex, expected);
  EXPECT_NE(result.message.find("data=" + expected), std::string::npos) << result.message;
  EXPECT_NE(result.message.find("object 0x8100 class 0 handle 0x10 status 0 length 40"), std::string::npos)
      << result.message;
}

TEST(HubManagement, ReadObjectPausesBeforeEveryBlockRequestAndTheClose) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.set_diagnostic_probes_enabled(true);
  // 20 bytes = 2 blocks, then the close: three pauses.
  radio.queue_rx(
      frame_to_packet(make_transfer_reply(component.node_id_, CMD_TRANSFER_OPEN_RESP, {0x10, 0x00, 0x00, 0x14})));
  radio.queue_rx(frame_to_packet(make_block(component.node_id_, 0x10, 2, 0, 18)));
  radio.queue_rx(frame_to_packet(make_block(component.node_id_, 0x10, 1, 18, 2)));
  radio.queue_rx(frame_to_packet(make_block(component.node_id_, 0x10, 0, 0, 0)));
  esphome::test_hal::reset_delays();

  const auto result = component.probe_device("ABC123", "read_object", "0");
  ASSERT_TRUE(result.success) << result.message;
  ASSERT_EQ(radio.get_sent_data().size(), 4u);
  const auto &delays = esphome::test_hal::recorded_delays();
  EXPECT_EQ(std::count(delays.begin(), delays.end(), 650u), 3) << "one 650 ms pause per block request and the close";
}

TEST(HubManagement, ReadObjectIndexPicksTheObject) {
  struct Case {
    const char *index;
    uint8_t object_class;
    uint8_t hi;
    uint8_t lo;
  };
  for (const Case &c : {Case{"0", 0x00, 0x81, 0x00}, Case{"1", 0x00, 0x81, 0x03}, Case{"2", 0x00, 0x80, 0x03},
                        Case{"3", 0x00, 0x80, 0x01}, Case{"4", 0x00, 0x80, 0x02}, Case{"5", 0x00, 0x80, 0x00},
                        Case{"9", 0x00, 0xC0, 0x01}, Case{"10", 0x02, 0x61, 0x03}, Case{"13", 0x02, 0xA6, 0x01},
                        Case{"29", 0x02, 0xA6, 0x18}}) {
    SCOPED_TRACE(c.index);
    TestableManagementComponent component;
    MockRadio radio;
    setup_component(component, radio);
    component.set_diagnostic_probes_enabled(true);
    radio.queue_rx(frame_to_packet(make_error_response(component.node_id_, RESULT_COMMAND_UNKNOWN)));
    component.probe_device("ABC123", "read_object", c.index);
    ASSERT_FALSE(radio.get_sent_data().empty());
    const auto bytes = sent_frame_bytes(radio, 0);
    EXPECT_EQ(bytes[8], CMD_TRANSFER_OPEN);
    EXPECT_EQ(bytes[10], c.object_class);
    EXPECT_EQ(bytes[12], c.hi);
    EXPECT_EQ(bytes[13], c.lo);
  }
}

TEST(HubManagement, ReadObjectReportsAnErrorAnswerWithoutReadingOn) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.set_diagnostic_probes_enabled(true);
  radio.queue_rx(frame_to_packet(make_error_response(component.node_id_, RESULT_COMMAND_UNKNOWN)));

  const auto result = component.probe_device("ABC123", "read_object", "0");
  ASSERT_TRUE(result.success) << result.message;
  EXPECT_EQ(result.response_cmd, CMD_ERROR_RESP);
  EXPECT_EQ(radio.get_sent_data().size(), 1u) << "no block request after an error answer";
  EXPECT_TRUE(result.response_hex.find("FE") != std::string::npos);
}

TEST(HubManagement, ReadObjectStopsAtARefusedOpenAndAtAnEmptyObject) {
  {
    TestableManagementComponent component;
    MockRadio radio;
    setup_component(component, radio);
    component.set_diagnostic_probes_enabled(true);
    radio.queue_rx(
        frame_to_packet(make_transfer_reply(component.node_id_, CMD_TRANSFER_OPEN_RESP, {0x10, 0x02, 0x00, 0x28})));
    const auto result = component.probe_device("ABC123", "read_object", "0");
    EXPECT_NE(result.message.find("device refused the read"), std::string::npos) << result.message;
    EXPECT_EQ(radio.get_sent_data().size(), 1u);
  }
  {
    TestableManagementComponent component;
    MockRadio radio;
    setup_component(component, radio);
    component.set_diagnostic_probes_enabled(true);
    radio.queue_rx(
        frame_to_packet(make_transfer_reply(component.node_id_, CMD_TRANSFER_OPEN_RESP, {0x10, 0x00, 0x00, 0x00})));
    const auto result = component.probe_device("ABC123", "read_object", "0");
    EXPECT_NE(result.message.find("object is empty"), std::string::npos) << result.message;
    EXPECT_EQ(radio.get_sent_data().size(), 1u) << "length 0 ends the session; nothing to close";
  }
  {
    // Status 7 with length 0 (an RS100 Solar io's 0x6105): empty, not a refusal.
    TestableManagementComponent component;
    MockRadio radio;
    setup_component(component, radio);
    component.set_diagnostic_probes_enabled(true);
    radio.queue_rx(
        frame_to_packet(make_transfer_reply(component.node_id_, CMD_TRANSFER_OPEN_RESP, {0x10, 0x07, 0x00, 0x00})));
    const auto result = component.probe_device("ABC123", "read_object", "0");
    EXPECT_NE(result.message.find("object is empty (status 7)"), std::string::npos) << result.message;
    EXPECT_EQ(result.message.find("refused"), std::string::npos) << result.message;
    EXPECT_EQ(radio.get_sent_data().size(), 1u);
  }
}

TEST(HubManagement, ReadObjectClosesWithoutReadingAnObjectLongerThanTheCap) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.set_diagnostic_probes_enabled(true);
  // 0x01C2 = 450 bytes, above the 432-byte cap (24 blocks).
  radio.queue_rx(
      frame_to_packet(make_transfer_reply(component.node_id_, CMD_TRANSFER_OPEN_RESP, {0x10, 0x00, 0x01, 0xC2})));
  radio.queue_rx(frame_to_packet(make_block(component.node_id_, 0x10, 0, 0, 0)));

  const auto result = component.probe_device("ABC123", "read_object", "0");
  EXPECT_NE(result.message.find("closed unread"), std::string::npos) << result.message;
  ASSERT_EQ(radio.get_sent_data().size(), 2u);
  EXPECT_EQ(sent_cmd(radio, 0), CMD_TRANSFER_OPEN);
  EXPECT_EQ(sent_cmd(radio, 1), CMD_UNKNOWN4A_REQ);
}

TEST(HubManagement, ReadObjectReadsAnObjectOfTheLargestLengthARealDeviceAnnounced) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.set_diagnostic_probes_enabled(true);
  // 431 bytes (0x01AF) = 24 blocks, the last one partial (17 bytes): the length that set the cap.
  radio.queue_rx(
      frame_to_packet(make_transfer_reply(component.node_id_, CMD_TRANSFER_OPEN_RESP, {0x10, 0x00, 0x01, 0xAF})));
  for (uint16_t chunk = 24; chunk >= 1; chunk--) {
    const uint16_t first = static_cast<uint16_t>((24 - chunk) * 18);
    radio.queue_rx(frame_to_packet(
        make_block(component.node_id_, 0x10, chunk, static_cast<uint8_t>(first), chunk == 1 ? 17 : 18)));
  }
  radio.queue_rx(frame_to_packet(make_block(component.node_id_, 0x10, 0, 0, 0)));

  const auto result = component.probe_device("ABC123", "read_object", "0");
  ASSERT_TRUE(result.success) << result.message;
  EXPECT_EQ(radio.get_sent_data().size(), 26u) << "open, 24 block requests, close";
  EXPECT_EQ(result.message.find("incomplete"), std::string::npos) << result.message;
  EXPECT_EQ(result.response_hex.size(), 431u * 3 - 1) << "all 431 bytes, space separated";
}

TEST(HubManagement, ReadObjectClosesTheTransferAfterAFailedBlock) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  component.set_diagnostic_probes_enabled(true);
  radio.queue_rx(
      frame_to_packet(make_transfer_reply(component.node_id_, CMD_TRANSFER_OPEN_RESP, {0x10, 0x00, 0x00, 0x28})));
  radio.queue_rx(frame_to_packet(make_block(component.node_id_, 0x10, 3, 0, 18)));
  // The block for chunk 2 echoes chunk 7: a late answer to something else, not spliced in.
  radio.queue_rx(frame_to_packet(make_block(component.node_id_, 0x10, 7, 99, 18)));

  const auto result = component.probe_device("ABC123", "read_object", "0");
  EXPECT_NE(result.message.find("incomplete"), std::string::npos) << result.message;
  EXPECT_EQ(result.response_hex.find("63"), std::string::npos) << "the stray block's bytes must not appear";
  const size_t sent = radio.get_sent_data().size();
  ASSERT_GE(sent, 4u);
  const auto last = sent_frame_bytes(radio, sent - 1);
  EXPECT_EQ(last[8], CMD_UNKNOWN4A_REQ);
  EXPECT_EQ(last[11], 0x00) << "the last frame sent is the close (chunk 0)";
  EXPECT_EQ(last[10], 0x00);
}

TEST(HubManagement, ReadObjectIsGatedLikeEveryProbeAndIndexBounded) {
  {
    TestableManagementComponent component;
    MockRadio radio;
    setup_component(component, radio);
    const auto result = component.probe_device("ABC123", "read_object", "0");
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.message.find("diagnostic probes are not enabled"), std::string::npos);
    EXPECT_TRUE(radio.get_sent_data().empty());
  }
  {
    TestableManagementComponent component;
    MockRadio radio;
    setup_component(component, radio);
    component.set_diagnostic_probes_enabled(true);
    EXPECT_FALSE(component.probe_device("ABC123", "read_object", "30").success);
    EXPECT_FALSE(component.probe_device("ABC123", "read_object", "0x8100").success);
    EXPECT_EQ(component.probe_device("ABC123", "read_object", "30").message,
              "probe \"read_object\" takes index 0 (actuator capacity 0x8100), 1 (actuator state 0x8103), 2 (name "
              "record 0x8003), 3 (general info 1, 0x8001), 4 (general info 2, 0x8002), or 5 to 29 (further objects, "
              "see the diagnostic probes documentation)");
    EXPECT_EQ(component.probe_sweep("ABC123", "read_object", "0", "2").message,
              "probe \"read_object\" cannot be swept; use probe_device");
    EXPECT_TRUE(radio.get_sent_data().empty());
  }
}

// --- DiagnosticProbes on its own: the three injected hub calls are its only way to the hub ---

namespace {

/// A DiagnosticProbes with no hub behind it: a real engine and registry on a MockRadio, and the three
/// DiagnosticProbesHub calls as fakes that count what reached them.
struct StandaloneProbes {
  MockRadio radio;
  RadioDriver *radio_ptr{&radio};
  TuningConfig tuning{};
  uint8_t node_id[NODE_ID_SIZE]{0xC0, 0xFF, 0xEE};
  ExchangeEngine engine{&radio_ptr, node_id, test::TEST_SYSTEM_KEY, &tuning};
  DeviceRegistry registry;
  bool initialized{true};
  bool enabled{true};
  int radio_calls{0};
  std::vector<IoFrame> recorded_replies;
  DiagnosticProbes probes{node_id, engine, registry, &initialized,
                          DiagnosticProbesHub{[this]() { return enabled; },
                                              [this]() -> RadioDriver * {
                                                ++radio_calls;
                                                return &radio;
                                              },
                                              [this](const IoFrame &reply) {
                                                recorded_replies.push_back(reply);
                                                return false;
                                              }}};

  StandaloneProbes() { registry.add("ABC123"); }
};

}  // namespace

TEST(DiagnosticProbesSeam, DisabledHookRefusesBeforeAnyRadioTraffic) {
  StandaloneProbes rig;
  rig.enabled = false;

  const auto result = rig.probes.probe_device("ABC123", "private_fn", "6");
  EXPECT_FALSE(result.success);
  EXPECT_TRUE(result.terminal_refusal);
  EXPECT_EQ(result.message, "diagnostic probes are not enabled; set diagnostic_probes: true in YAML");
  EXPECT_TRUE(rig.radio.get_sent_data().empty()) << "the gate is the injected call, checked before any send";
  EXPECT_EQ(rig.radio_calls, 0);
  EXPECT_TRUE(rig.recorded_replies.empty());
}

TEST(DiagnosticProbesSeam, ReplyGoesOnlyToTheInjectedCallsAndLeavesTheRecordAlone) {
  StandaloneProbes rig;
  auto *dev = rig.registry.get("ABC123");
  ASSERT_NE(dev, nullptr);
  dev->position = 42.0F;
  dev->is_stopped = true;
  rig.radio.queue_rx(frame_to_packet(make_private_function_reply(rig.node_id)));

  const auto result = rig.probes.probe_device("ABC123", "private_fn", "0x06");
  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.response_cmd, CMD_PRIVATE_RESP);
  EXPECT_EQ(rig.radio_calls, 1) << "the capture log line asks the injected call for the radio";
  ASSERT_EQ(rig.recorded_replies.size(), 1U) << "the reply is offered to the event recorder, once";
  EXPECT_EQ(rig.recorded_replies[0].cmd, CMD_PRIVATE_RESP);
  EXPECT_FLOAT_EQ(dev->position, 42.0F) << "with no hub behind it, nothing applies the reply to the record";
  EXPECT_TRUE(result.decoded.empty()) << "the recorder said it was no event report, so no event header";
}

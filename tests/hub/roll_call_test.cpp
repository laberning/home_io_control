/// @file roll_call_test.cpp
/// @brief Tests for the roll-call responder formatting and the pulled-key report (roll_call.h).

#include "roll_call.h"

#include "log_helpers.h"
#include "test_helpers.h"

#include <string>

using namespace esphome::home_io_control;

namespace {

constexpr uint8_t PULLED_KEY[AES_KEY_SIZE] = {0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE,
                                              0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
constexpr uint8_t PAIRING_NODE[NODE_ID_SIZE] = {0xB6, 0xBD, 0xFB};

/// Add a roll-call reply from `src` of the packed type bytes to `state`.
void add_reply(ScanState &state, const uint8_t src[NODE_ID_SIZE], uint8_t type_msb, uint8_t type_low) {
  const uint8_t payload[] = {type_msb, type_low, src[0], src[1], src[2], 0x0C, 0xCC, 0x00, 0x00};
  IoFrame frame{};
  init_frame(frame, true, true, true, false);
  set_dst(frame, test::OWN_ID);
  set_src(frame, src);
  set_cmd(frame, CMD_DISCOVER_SPE_RESP, payload, sizeof(payload));
  const ScanResponder *entry = nullptr;
  add_scan_responder(state.responders, state.count, SCAN_MAX_REPLIES, frame, -60, 1, entry);
}

PulledKeyReportInfo report_info() {
  return {node_id_to_string(PAIRING_NODE), DeviceType::REMOTE_CONTROLLER, 0, PULLED_KEY, test::TEST_SYSTEM_KEY};
}

size_t count_of(const std::string &text, const std::string &needle) {
  size_t n = 0;
  for (size_t pos = text.find(needle); pos != std::string::npos; pos = text.find(needle, pos + needle.size()))
    n++;
  return n;
}

}  // namespace

TEST(PulledKeyReport, OneBannerFramesEveryResponderBlock) {
  ScanState state{};
  const uint8_t heater[NODE_ID_SIZE] = {0xB6, 0xBD, 0xFC};
  const uint8_t second[NODE_ID_SIZE] = {0xB6, 0xBD, 0xFD};
  add_reply(state, heater, 0x0D, 0x01);  // 52/1
  add_reply(state, second, 0x0D, 0x01);
  DeviceRegistry registry;

  const std::string report = build_pulled_key_report(report_info(), state, registry);

  EXPECT_EQ(count_of(report, "DO NOT SHARE"), 1u) << "one banner for the whole report, not one per device";
  EXPECT_EQ(report.rfind("DEVICE KEY RECEIVED -- DO NOT SHARE IT", 0), 0u);
  EXPECT_EQ(count_of(report, "system_key: \"" + detail::format_key_hex(PULLED_KEY) + "\""), 2u)
      << "each device's block carries the key";
  EXPECT_EQ(count_of(report, "  climate:\n"), 2u);
  EXPECT_NE(report.find("io_device_id: \"B6BDFC\""), std::string::npos);
  EXPECT_NE(report.find("io_device_id: \"B6BDFD\""), std::string::npos);
  EXPECT_NE(report.find("found 2 devices"), std::string::npos);
  EXPECT_NE(report.find("key check value " + detail::format_key_check_value(PULLED_KEY)), std::string::npos);
  EXPECT_NE(report.find("END OF DEVICE KEY REPORT"), std::string::npos);
}

TEST(PulledKeyReport, AResponderWithoutAPlatformGetsTheNotSupportedLineAndTheKey) {
  ScanState state{};
  const uint8_t src[NODE_ID_SIZE] = {0xB6, 0xBD, 0xFC};
  add_reply(state, src, 0x05, 0x80);  // 22/0: the heat pump
  DeviceRegistry registry;

  const std::string report = build_pulled_key_report(report_info(), state, registry);

  EXPECT_NE(report.find("not supported yet"), std::string::npos);
  EXPECT_NE(report.find(detail::format_key_hex(PULLED_KEY)), std::string::npos);
  EXPECT_EQ(report.find("io_device_id"), std::string::npos) << "no snippet for a type without a platform";
}

TEST(PulledKeyReport, WithNoResponderTheKeyIsGivenOnceForThePairingNode) {
  ScanState state{};
  DeviceRegistry registry;

  const std::string report = build_pulled_key_report(report_info(), state, registry);

  EXPECT_NE(report.find("found no device"), std::string::npos);
  EXPECT_EQ(count_of(report, detail::format_key_hex(PULLED_KEY)), 1u);
  EXPECT_NE(report.find("system_key: \"" + detail::format_key_hex(PULLED_KEY) + "\""), std::string::npos);
  EXPECT_EQ(report.find("home_io_control:"), std::string::npos) << "it is a per-device key, not the hub's";
}

TEST(PulledKeyReport, AKnownResponderIsReportedAsKnownWithoutASnippet) {
  ScanState state{};
  const uint8_t src[NODE_ID_SIZE] = {0xB6, 0xBD, 0xFC};
  add_reply(state, src, 0x0D, 0x01);
  DeviceRegistry registry;
  DeviceConfig cfg;
  cfg.type = DeviceType::ELECTRICAL_HEATER;
  cfg.subtype = 1;
  registry.add("B6BDFC", cfg);

  const std::string report = build_pulled_key_report(report_info(), state, registry);

  EXPECT_NE(report.find("[known]"), std::string::npos);
  EXPECT_EQ(report.find("io_device_id"), std::string::npos);
  EXPECT_NE(report.find("fix its system_key:"), std::string::npos) << "its YAML does not use the pulled key";
}

TEST(HubKeyPullReport, ListsTheResponderBlocksWithoutAKeyOrABanner) {
  ScanState state{};
  const uint8_t heater[NODE_ID_SIZE] = {0xB6, 0xBD, 0xFC};
  add_reply(state, heater, 0x0D, 0x01);
  DeviceRegistry registry;
  const PulledKeyReportInfo info{node_id_to_string(PAIRING_NODE), DeviceType::REMOTE_CONTROLLER, 0,
                                 test::TEST_SYSTEM_KEY, test::TEST_SYSTEM_KEY};

  const std::string report = build_hub_key_pull_report(info, state, registry);

  EXPECT_NE(report.find("answered the key pull with this hub's own key"), std::string::npos);
  EXPECT_NE(report.find("  climate:\n"), std::string::npos);
  EXPECT_NE(report.find("io_device_id: \"B6BDFC\""), std::string::npos);
  EXPECT_EQ(report.find("system_key"), std::string::npos) << "the device uses the hub key";
  EXPECT_EQ(report.find("DO NOT SHARE"), std::string::npos);
  EXPECT_EQ(report.find(detail::format_key_hex(test::TEST_SYSTEM_KEY)), std::string::npos);
}

TEST(HubKeyPullReport, IsEmptyWhenNothingAnswered) {
  ScanState state{};
  DeviceRegistry registry;
  const PulledKeyReportInfo info{node_id_to_string(PAIRING_NODE), DeviceType::REMOTE_CONTROLLER, 0,
                                 test::TEST_SYSTEM_KEY, test::TEST_SYSTEM_KEY};
  EXPECT_TRUE(build_hub_key_pull_report(info, state, registry).empty());
}

TEST(RollCallResponder, DisplayModesDecideWhereTheKeyAppears) {
  ScanState state{};
  const uint8_t src[NODE_ID_SIZE] = {0xB6, 0xBD, 0xFC};
  add_reply(state, src, 0x0D, 0x01);
  const RollCallReplyKeys keys{PULLED_KEY, test::TEST_SYSTEM_KEY, true};
  const std::string hex = detail::format_key_hex(PULLED_KEY);

  const std::string hidden =
      format_roll_call_responder(state.responders[0], "B6BDFC", nullptr, keys, RollCallKeyDisplay::HIDDEN);
  EXPECT_EQ(hidden.find(hex), std::string::npos);
  EXPECT_NE(hidden.find("key=" + detail::format_key_check_value(PULLED_KEY)), std::string::npos);

  const std::string bannered =
      format_roll_call_responder(state.responders[0], "B6BDFC", nullptr, keys, RollCallKeyDisplay::BANNERED);
  EXPECT_NE(bannered.find(hex), std::string::npos);
  EXPECT_NE(bannered.find("DO NOT SHARE"), std::string::npos);

  const std::string plain =
      format_roll_call_responder(state.responders[0], "B6BDFC", nullptr, keys, RollCallKeyDisplay::PLAIN);
  EXPECT_NE(plain.find(hex), std::string::npos);
  EXPECT_EQ(plain.find("DO NOT SHARE"), std::string::npos) << "the caller's report supplies the banner";
}

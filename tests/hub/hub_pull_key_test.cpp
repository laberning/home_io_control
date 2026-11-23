/// @file hub_pull_key_test.cpp
/// @brief The `pull_key` management action (ADR 0130): its refusals send nothing and report no pairing attempt,
/// its result per outcome, and that the key reaches the device log and never the event or "Last Pairing Result".

#include "hub_core.h"
#include "log_helpers.h"
#include "proto_commands.h"
#include "proto_constants.h"
#include "proto_frame.h"

#include "esp_random.h"
#include "management_test_fixture.h"
#include "pairing_test_support.h"
#include "test_helpers.h"
#include "stubs/radio_test_common.h"

#include <cstring>
#include <string>
#include <vector>

using namespace esphome::home_io_control;
using namespace management_test;
using namespace pairing_test;

namespace {

/// A hub with a scripted radio, a result callback counter and the API server the action publishes to.
struct PullKeyHub {
  PullKeyHub() {
    api_server.reset();
    setup_component(component, radio);
    component.set_pairing_result_callback([this] { ++published; });
  }

  /// The controller's node ID as the user types it.
  static std::string controller_id() { return node_id_to_string(DEVICE_ID); }

  /// Queue the controller's 0x32 with `key` and, when `proven`, its 0x3D over it.
  void queue_pull(const uint8_t key[AES_KEY_SIZE], bool proven) {
    const IoFrame reply = pulled_key_frame(component.node_id_, key);
    radio.queue_rx(to_packet(reply));
    if (proven)
      radio.queue_rx(to_packet(proof_frame(component.node_id_, reply, key)));
  }

  /// The event the action published, when it published exactly one.
  const std::map<std::string, std::string> &event() const {
    EXPECT_EQ(api_server.events_.size(), 1u);
    return api_server.events_.front().data;
  }

  RngGuard rng;
  esphome::api::APIServer api_server;
  esphome::api::ScopedGlobalApiServer scoped_api_server{api_server};
  TestableManagementComponent component;
  MockRadio radio;
  int published{0};
};

}  // namespace

// ============================================================================
// Refusals: nothing is sent, no pairing attempt is reported
// ============================================================================

TEST(HubPullKey, AnUninitialisedHubRefusesAndSendsNothing) {
  PullKeyHub hub;
  hub.component.initialized_ = false;

  const auto result = hub.component.management_actions_.pull_key(PullKeyHub::controller_id());

  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "hub is not initialized");
  EXPECT_TRUE(hub.radio.get_sent_data().empty());
  EXPECT_EQ(hub.published, 0);
}

TEST(HubPullKey, AnIdThatIsNotSixHexDigitsIsRefused) {
  for (const std::string bad : {"", "B6BDF", "B6BDFB0", "B6BDFG", "B6 BFB"}) {
    PullKeyHub hub;
    const auto result = hub.component.management_actions_.pull_key(bad);
    EXPECT_FALSE(result.success) << bad;
    EXPECT_EQ(result.message, "controller ID must be exactly 6 hexadecimal characters") << bad;
    EXPECT_TRUE(hub.radio.get_sent_data().empty()) << bad;
    EXPECT_EQ(hub.published, 0) << bad;
  }
}

TEST(HubPullKey, TheHubsOwnIdIsRefusedInAnyLetterCase) {
  for (const char *own : {"C0FFEE", "c0ffee", " c0FFee "}) {
    PullKeyHub hub;
    const auto result = hub.component.management_actions_.pull_key(own);
    EXPECT_FALSE(result.success) << own;
    EXPECT_NE(result.message.find("this hub's own node ID"), std::string::npos) << result.message;
    EXPECT_TRUE(hub.radio.get_sent_data().empty()) << own;
  }
}

TEST(HubPullKey, ARefusedActionReportsNoPairingAttempt) {
  PullKeyHub hub;
  hub.component.management_actions_.pull_key("C0FFEE");
  hub.component.management_actions_.pull_key("nope");

  EXPECT_EQ(hub.published, 0) << "Last Pairing Result is not refreshed";
  EXPECT_EQ(hub.component.last_pairing_end_ms_, 0U) << "the pairing end is not stamped";
  EXPECT_EQ(hub.component.pairing_telemetry().outcome(), PairingOutcome::NONE) << "the telemetry is untouched";
}

// ============================================================================
// Outcomes
// ============================================================================

TEST(HubPullKey, AProvenKeyIsSuccessAndVerifiedAndPrintedOnlyInTheLogMessage) {
  PullKeyHub hub;
  hub.queue_pull(OTHER_KEY, /*proven=*/true);

  const auto result = hub.component.management_actions_.pull_key(PullKeyHub::controller_id());

  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.verified);
  EXPECT_EQ(result.device_id, "B6BDFB");
  EXPECT_EQ(result.action, "pull_key");
  EXPECT_EQ(result.message.find(detail::format_key_hex(OTHER_KEY)), std::string::npos) << "the message has no key";
  EXPECT_NE(result.message.find("printed in the device log"), std::string::npos);
  EXPECT_NE(result.log_message.find(detail::format_key_hex(OTHER_KEY)), std::string::npos)
      << "the device log gets the key, once";
  EXPECT_EQ(sent_commands(hub.radio), (std::vector<uint8_t>{CMD_LAUNCH_KEY_TRANSFER, CMD_CHALLENGE_REQ}))
      << "no discovery, and nothing after the proof";
}

TEST(HubPullKey, TheEventAndLastPairingResultCarryTheCheckValueAndAdviceNeverTheKey) {
  PullKeyHub hub;
  hub.queue_pull(OTHER_KEY, /*proven=*/true);

  hub.component.management_actions_.api_pull_key(PullKeyHub::controller_id());

  const std::string key_hex = detail::format_key_hex(OTHER_KEY);
  const auto &event = hub.event();
  EXPECT_EQ(event.at("action"), "pull_key");
  EXPECT_EQ(event.at("device_id"), "B6BDFB");
  EXPECT_EQ(event.at("success"), "true");
  EXPECT_EQ(event.at("verified"), "true");
  for (const auto &entry : event)
    EXPECT_EQ(entry.second.find(key_hex), std::string::npos) << entry.first << " carries the key";
  EXPECT_NE(event.at("message").find("proved"), std::string::npos) << "the advice is in the event";
  EXPECT_NE(event.at("message").find("key check value " + detail::format_key_check_value(OTHER_KEY)), std::string::npos)
      << "the check value is in the event: " << event.at("message");

  EXPECT_EQ(hub.published, 1) << "Last Pairing Result is refreshed once";
  EXPECT_NE(hub.component.last_pairing_end_ms_, 0U);
  const std::string summary = hub.component.pairing_telemetry().result_sensor_string();
  EXPECT_EQ(summary.find(key_hex), std::string::npos) << summary;
  EXPECT_NE(summary.find("pull_key_proven"), std::string::npos) << summary;
  const std::string tail = "; check=" + detail::format_key_check_value(OTHER_KEY);
  EXPECT_EQ(summary.substr(summary.size() - tail.size()), tail) << "check= is the last field: " << summary;
}

TEST(HubPullKey, TheHubsOwnKeyIsSuccessButNotVerified) {
  PullKeyHub hub;
  hub.queue_pull(hub.component.system_key_, /*proven=*/false);

  const auto result = hub.component.management_actions_.pull_key(PullKeyHub::controller_id());

  EXPECT_TRUE(result.success);
  EXPECT_FALSE(result.verified);
  EXPECT_NE(result.message.find("already shares this hub's key"), std::string::npos) << result.message;
  EXPECT_TRUE(result.log_message.empty());
  EXPECT_EQ(hub.published, 1);
}

TEST(HubPullKey, ARefusalCarriesTheControllersCode) {
  PullKeyHub hub;
  hub.radio.queue_rx(to_packet(error_frame(hub.component.node_id_, RESULT_COMMAND_UNKNOWN)));

  hub.component.management_actions_.api_pull_key(PullKeyHub::controller_id());

  const auto &event = hub.event();
  EXPECT_EQ(event.at("success"), "false");
  EXPECT_EQ(event.at("verified"), "false");
  EXPECT_NE(event.at("message").find("refused"), std::string::npos) << event.at("message");
  EXPECT_NE(event.find("result_code"), event.end()) << "the controller's code is in the event";
  EXPECT_EQ(hub.published, 1) << "an attempt ran, so it is published";
}

TEST(HubPullKey, ASilentControllerIsNoKey) {
  PullKeyHub hub;

  const auto result = hub.component.management_actions_.pull_key(PullKeyHub::controller_id());

  EXPECT_FALSE(result.success);
  EXPECT_FALSE(result.verified);
  EXPECT_NE(result.message.find("sent no key"), std::string::npos) << result.message;
  EXPECT_FALSE(result.has_result_code);
}

TEST(HubPullKey, AKeyNobodyProvesIsNotProvenAndNotPrinted) {
  PullKeyHub hub;
  hub.queue_pull(OTHER_KEY, /*proven=*/false);

  const auto result = hub.component.management_actions_.pull_key(PullKeyHub::controller_id());

  EXPECT_FALSE(result.success);
  EXPECT_NE(result.message.find("nobody proved"), std::string::npos) << result.message;
  EXPECT_TRUE(result.log_message.empty());
  EXPECT_EQ(hub.component.registry_.size(), 1u) << "only the fixture's own device";
}

TEST(HubPullKey, AnUnprovenKeyAnsweredByADeviceIsProvenByTheSearch) {
  PullKeyHub hub;
  hub.queue_pull(OTHER_KEY, /*proven=*/false);
  hub.radio.queue_rx_hold_until_sent(5);  // the 0x38, three 0x3C, then the first roll-call 0x2A
  hub.radio.queue_rx(to_packet(heater_roll_call_reply(hub.component.node_id_)));

  const auto result = hub.component.management_actions_.pull_key(PullKeyHub::controller_id());

  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.verified);
  EXPECT_NE(result.message.find("devices that answered under it"), std::string::npos) << result.message;
  EXPECT_NE(result.message.find(detail::format_key_check_value(OTHER_KEY)), std::string::npos) << result.message;
  EXPECT_EQ(result.message.find(detail::format_key_hex(OTHER_KEY)), std::string::npos);
  EXPECT_TRUE(result.log_message.empty()) << "the search already printed its report";
  EXPECT_NE(hub.component.get_device("B6BDFC"), nullptr) << "registered, as in Discover & Pair";
}

/// @file exchange_quiet_window_test.cpp
/// @brief Tests that the exchange engine logs nothing while it waits for a reply it provoked, and that the
/// lines held back follow afterwards (ADR 0111).

#include "hub_exchange.h"
#include "hub_core.h"
#include "log_frame.h"
#include "proto_frame.h"
#include "proto_commands.h"
#include "proto_crypto.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"
#include "support/hold_seam.h"
#include "support/log_capture.h"

#include <cstring>
#include <string>
#include <vector>

#include "exchange_fixture.h"

using namespace esphome::home_io_control;
using namespace exchange_test;

namespace {

/// Samples a LogCapture and the frame-log hold depth at the two moments that bound a quiet window: the hub's
/// transmit and the radio handing over the scripted reply.
struct WindowProbe {
  test::LogCapture log;
  /// Log lines recorded at each transmit, in send order.
  std::vector<size_t> at_send;
  /// Log lines recorded at each hand-over of a scripted reply, in order.
  std::vector<size_t> at_deliver;

  /// The hold depth at the same moments.
  test::HoldSeam seam;

  explicit WindowProbe(MockRadio &radio)
      : seam(
            radio, [this] { at_send.push_back(log.lines().size()); },
            [this] { at_deliver.push_back(log.lines().size()); }) {}

  /// The lines logged after the last reply was handed over.
  [[nodiscard]] std::vector<test::LogCapture::Line> after_last_reply() const {
    return {log.lines().begin() + static_cast<std::ptrdiff_t>(at_deliver.back()), log.lines().end()};
  }
};

bool has_line(const std::vector<test::LogCapture::Line> &lines, const std::string &prefix) {
  for (const auto &line : lines)
    if (line.format.rfind(prefix, 0) == 0)
      return true;
  return false;
}

/// A hub on `radio`, ready to send_and_receive_().
void wire(TestableComponent &comp, MockRadio &radio) {
  comp.initialized_ = true;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);
}

}  // namespace

TEST(ExchangeQuietWindow, PlainExchangeLogsNothingBetweenTransmitAndReply) {
  TestableComponent comp;
  MockRadio radio;
  wire(comp, radio);
  WindowProbe probe(radio);

  IoFrame request{};
  create_execute_position(request, comp.node_id_, test::DST_ID, false, 100);
  radio.queue_rx(to_rx_packet(build_status_response(test::DST_ID, comp.node_id_)));

  IoFrame response{};
  ASSERT_EQ(comp.send_and_receive_(request, response, FREQ_CH2), ExchangeOutcome::SUCCESS_WITH_RESPONSE);

  ASSERT_EQ(probe.at_send.size(), 1u);
  ASSERT_EQ(probe.at_deliver.size(), 1u);
  EXPECT_EQ(probe.at_deliver[0], probe.at_send[0]) << "a line was logged while the hub waited for the reply";
  EXPECT_TRUE(has_line(probe.after_last_reply(), "Try %d answered")) << "the outcome line follows the wait";
  EXPECT_TRUE(probe.seam.held_at_every_send_and_delivery())
      << "the frame log must be held from before the request to the reply";
  EXPECT_TRUE(test::frame_log_released()) << "the hold ends with the exchange";
}

TEST(ExchangeQuietWindow, AuthenticatedExchangeLogsNothingBetweenEachTransmitAndItsReply) {
  TestableComponent comp;
  MockRadio radio;
  wire(comp, radio);
  WindowProbe probe(radio);

  IoFrame request{};
  create_execute_position(request, comp.node_id_, test::DST_ID, false, 100);
  const uint8_t chal_data[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  radio.queue_rx(to_rx_packet(build_challenge(test::DST_ID, comp.node_id_, chal_data)));
  radio.queue_rx(to_rx_packet(build_status_response(test::DST_ID, comp.node_id_)));

  IoFrame response{};
  ASSERT_EQ(comp.send_and_receive_(request, response, FREQ_CH2), ExchangeOutcome::SUCCESS_WITH_RESPONSE);

  ASSERT_EQ(probe.at_send.size(), 2u) << "request and 0x3D";
  ASSERT_EQ(probe.at_deliver.size(), 2u) << "challenge and final reply";
  EXPECT_EQ(probe.at_deliver[0], probe.at_send[0]) << "a line was logged between the request and the challenge";
  EXPECT_EQ(probe.at_send[1], probe.at_deliver[0]) << "a line was logged between the challenge and the 0x3D (#163)";
  EXPECT_EQ(probe.at_deliver[1], probe.at_send[1])
      << "a line was logged between the 0x3D and the final reply (it delays noticing the reply)";
  EXPECT_TRUE(probe.seam.quiet_from_delivery_to_next_send(0))
      << "the hold was released between the challenge and the 0x3D, which prints the held lines there (#163)";
  EXPECT_TRUE(probe.seam.held_at_every_send_and_delivery())
      << "the frame log must stay held across the challenge, the 0x3D and the final reply";
  EXPECT_TRUE(test::frame_log_released()) << "the hold ends with the exchange";
}

TEST(ExchangeQuietWindow, AuthChallengeLineFollowsTheFinalWaitBeforeTheOutcome) {
  TestableComponent comp;
  MockRadio radio;
  wire(comp, radio);
  WindowProbe probe(radio);

  IoFrame request{};
  create_execute_position(request, comp.node_id_, test::DST_ID, false, 100);
  const uint8_t chal_data[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  radio.queue_rx(to_rx_packet(build_challenge(test::DST_ID, comp.node_id_, chal_data)));
  radio.queue_rx(to_rx_packet(build_status_response(test::DST_ID, comp.node_id_)));

  IoFrame response{};
  ASSERT_EQ(comp.send_and_receive_(request, response, FREQ_CH2), ExchangeOutcome::SUCCESS_WITH_RESPONSE);

  const auto after = probe.after_last_reply();
  ASSERT_FALSE(after.empty());
  EXPECT_EQ(after.front().format.rfind("Auth challenge", 0), 0u) << "the deferred line is the first one after the wait";
}

TEST(ExchangeQuietWindow, AuthChallengeLineIsStillLoggedWhenTheFinalReplyNeverComes) {
  TestableComponent comp;
  MockRadio radio;
  wire(comp, radio);
  test::LogCapture log;

  IoFrame request{};
  create_execute_position(request, comp.node_id_, test::DST_ID, false, 100);
  const uint8_t chal_data[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  radio.queue_rx(to_rx_packet(build_challenge(test::DST_ID, comp.node_id_, chal_data)));

  IoFrame response{};
  ASSERT_EQ(comp.send_and_receive_(request, response, FREQ_CH2), ExchangeOutcome::SUCCESS_UNCONFIRMED);

  const auto &lines = log.lines();
  size_t auth = lines.size();
  size_t ended = lines.size();
  for (size_t i = 0; i < lines.size(); i++) {
    if (auth == lines.size() && lines[i].format.rfind("Auth challenge", 0) == 0)
      auth = i;
    if (ended == lines.size() && lines[i].format.rfind("Try %d ended: no matching final response", 0) == 0)
      ended = i;
  }
  ASSERT_LT(auth, lines.size()) << "the Auth challenge line is logged once per challenged try";
  ASSERT_LT(ended, lines.size());
  EXPECT_LT(auth, ended) << "the challenge line precedes the timeout line, as the exchange happened";
  EXPECT_TRUE(test::frame_log_released()) << "a silent final wait releases the hold";
}

TEST(ExchangeQuietWindow, HoldDepthStaysBalancedWhenTheRequestNeverGetsAnAnswer) {
  TestableComponent comp;
  MockRadio radio;
  wire(comp, radio);

  IoFrame request{};
  create_execute_position(request, comp.node_id_, test::DST_ID, false, 100);
  IoFrame response{};
  EXPECT_EQ(comp.send_and_receive_(request, response, FREQ_CH2), ExchangeOutcome::FAILED);
  EXPECT_TRUE(test::frame_log_released()) << "every try's first wait, a timeout, releases its hold";
}

TEST(ExchangeQuietWindow, AuthChallengeLineIsLoggedWhenTheAnswerFailsToSend) {
  // No final wait follows a 0x3D that failed to go out, so handle_authentication_() ends the window
  // itself: the deferred line must not be lost with the wait it would have followed.
  TestableComponent comp;
  MockRadio radio;
  wire(comp, radio);
  test::LogCapture log;

  IoFrame request{};
  create_execute_position(request, comp.node_id_, test::DST_ID, false, 100);
  const uint8_t chal_data[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  radio.queue_rx(to_rx_packet(build_challenge(test::DST_ID, comp.node_id_, chal_data)));
  radio.queue_tx_result(true);   // the request
  radio.queue_tx_result(false);  // the 0x3D

  IoFrame response{};
  comp.send_and_receive_(request, response, FREQ_CH2, 1);

  EXPECT_TRUE(has_line(log.lines(), "Auth challenge")) << "the challenged try still prints its line";
  EXPECT_TRUE(test::frame_log_released()) << "a try that ends at the failed 0x3D releases the hold";
}

TEST(ExchangeQuietWindow, HoldBufferKeepsEveryLineOfOneAuthenticatedExchange) {
  // The lines one authenticated exchange can produce while held: the request's TX line and the driver's
  // re-arm line, the challenge (up to four reception lines), the 0x3D's TX line and re-arm line, and the
  // final reply (up to four reception lines).
  constexpr size_t kRequestLines = 2;
  constexpr size_t kChallengeLines = 4;
  constexpr size_t kAnswerLines = 2;
  constexpr size_t kFinalReplyLines = 4;
  constexpr size_t kExchangeLines = kRequestLines + kChallengeLines + kAnswerLines + kFinalReplyLines;
  // What an SPI-radio build holds: the host build sets the larger number of a bridge build (Makefile).
  ASSERT_GE(FRAME_LOG_OWN_HELD_LINES, kExchangeLines);

  static std::vector<std::string> printed;
  printed.clear();
  FrameLogBuffer buffer([](FrameLogLevel, const char *, const char *text) { printed.emplace_back(text); });
  buffer.hold();
  for (size_t i = 0; i < kExchangeLines; i++)
    buffer.write(FrameLogLevel::INFO, "io_frame", std::to_string(i).c_str());
  EXPECT_TRUE(printed.empty());
  buffer.release();

  ASSERT_EQ(printed.size(), kExchangeLines) << "no line dropped, and no overflow note";
  for (size_t i = 0; i < kExchangeLines; i++)
    EXPECT_EQ(printed[i], std::to_string(i)) << "original order";
}

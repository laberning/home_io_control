#include <gtest/gtest.h>

#include "esphome/core/hal.h"
#include "radio_soft_phy.h"
#include "stubs/radio_test_common.h"

#include <vector>

// ============================================================================
// MockRadioTimed test suite
// ============================================================================
// MockRadio's replayed-timeline support (queue_rx_timed_after_send()/queue_rx_timed_after_bytes()
// and set_model_tx_airtime()), which the corpus replay suites build on through
// tests/support/timed_replay.h. Every case runs under a ManualClock: a timed entry is only
// meaningful when time is real.
// ============================================================================

using esphome::home_io_control::RadioRxPacket;
using esphome::home_io_control::RadioTxConfig;
using esphome::test_clock::ManualClock;
using esphome::test_clock::peek_ms;

namespace {

RadioRxPacket packet_with(uint8_t marker) {
  RadioRxPacket packet{};
  packet.len = 1;
  packet.data[0] = marker;
  return packet;
}

void send(MockRadio &radio, const std::vector<uint8_t> &bytes, uint16_t preamble = 8) {
  RadioTxConfig config{};
  config.preamble_len = preamble;
  radio.send_packet(bytes.data(), static_cast<uint8_t>(bytes.size()), config);
}

TEST(MockRadioTimed, EntryIsSilentUntilItsAnchorSendHappens) {
  ManualClock clock;
  MockRadio radio;
  radio.queue_rx_timed_after_send(packet_with(0xA1), 0, 50000);

  const uint32_t start = peek_ms();
  RadioRxPacket received{};
  EXPECT_FALSE(radio.wait_for_packet(received, 100));
  EXPECT_EQ(peek_ms() - start, 100u) << "a silent wait lasts its whole timeout";

  send(radio, {0x01});
  const uint32_t sent_at = peek_ms();
  ASSERT_TRUE(radio.wait_for_packet(received, 100)) << "the entry must survive the silent wait";
  EXPECT_EQ(received.data[0], 0xA1);
  EXPECT_EQ(peek_ms() - sent_at, 50u) << "delivered at its offset after the anchor send";
}

TEST(MockRadioTimed, ArrivalBeyondTheWindowWaitsForALaterListen) {
  ManualClock clock;
  MockRadio radio;
  radio.queue_rx_timed_after_send(packet_with(0xB2), 0, 300000);
  send(radio, {0x01});
  const uint32_t sent_at = peek_ms();

  RadioRxPacket received{};
  EXPECT_FALSE(radio.wait_for_packet(received, 100));
  EXPECT_FALSE(radio.wait_for_packet(received, 100));
  ASSERT_TRUE(radio.wait_for_packet(received, 200));
  EXPECT_EQ(peek_ms() - sent_at, 300u);
}

TEST(MockRadioTimed, ArrivalAlreadyPastIsDeliveredAtOnce) {
  ManualClock clock;
  MockRadio radio;
  radio.set_model_tx_airtime(true);
  // 5 ms after TX start, while a 1024-byte preamble is still on air: delivered when listening starts.
  radio.queue_rx_timed_after_send(packet_with(0xC3), 0, 5000);
  send(radio, {0x01}, 1024);
  const uint32_t listen_start = peek_ms();

  RadioRxPacket received{};
  ASSERT_TRUE(radio.wait_for_packet(received, 100));
  EXPECT_EQ(peek_ms(), listen_start) << "a past arrival costs no further time";
}

TEST(MockRadioTimed, BytesAnchorFollowsTheFirstMatchingSend) {
  ManualClock clock;
  MockRadio radio;
  const std::vector<uint8_t> first = {0x28};
  const std::vector<uint8_t> second = {0x31};
  radio.queue_rx_timed_after_bytes(packet_with(0xD4), second, 10000);

  send(radio, first);
  esphome::test_clock::advance_ms(40);
  send(radio, second);
  const uint32_t second_at = peek_ms();
  esphome::test_clock::advance_ms(5);
  send(radio, second);  // a retry of the same bytes must not move the anchor

  RadioRxPacket received{};
  ASSERT_TRUE(radio.wait_for_packet(received, 100));
  EXPECT_EQ(received.data[0], 0xD4);
  EXPECT_EQ(peek_ms() - second_at, 10u);
}

TEST(MockRadioTimed, AirtimeModelAdvancesTheClockOnlyWhenEnabled) {
  ManualClock clock;
  MockRadio radio;
  const std::vector<uint8_t> frame(12, 0x55);

  const uint64_t before_off = esphome::test_clock::state().now_us;
  send(radio, frame, 1024);
  EXPECT_EQ(esphome::test_clock::state().now_us, before_off) << "a send takes no time by default";

  radio.set_model_tx_airtime(true);
  const uint64_t before_on = esphome::test_clock::state().now_us;
  send(radio, frame, 1024);
  const uint32_t expected = esphome::home_io_control::io868_tx_air_time_us(1024, 12);
  EXPECT_EQ(esphome::test_clock::state().now_us - before_on, expected);
  // Preamble, 3 sync bytes, and 12 + 2 CRC bytes as 10-bit cells, at the 38.4 kbps line rate.
  EXPECT_EQ(expected, esphome::home_io_control::soft_phy_air_time_us(
                          1024 + 3 + esphome::home_io_control::soft_phy_raw_bytes_for_frame(12)));
  EXPECT_GT(expected, 213000u) << "a 1024-byte preamble alone is ~213 ms on air";
}

}  // namespace

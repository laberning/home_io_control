#include "radio_sx1262.h"
#include "radio_interface.h"
#include "proto_constants.h"

#include "esphome/core/application.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"
#include "stubs/scripted_spi.h"

#include <gtest/gtest.h>
#include <vector>

using namespace esphome::home_io_control;

// ============================================================================
// Mock SPI and GPIO for unit testing RadioSX1262::wait_for_packet
// ============================================================================

// ============================================================================
// Testable subclass of RadioSX1262
// ============================================================================

class TestableRadioSX1262 : public RadioSX1262 {
 public:
  using RadioSX1262::RadioSX1262;

  // Configure the sequence of IRQ status values returned by read_irq_status_raw(). uint32_t to
  // match SoftPhyDriverBase's shared IRQ word width (SX1262's own values only ever occupy the
  // low 16 bits).
  void set_irq_sequence(std::initializer_list<uint32_t> seq) {
    irq_seq_.assign(seq);
    irq_idx_ = 0;
  }

  // Set the packet that read_rx_packet_ should return.
  void set_expected_packet(const RadioRxPacket &pkt) { expected_packet_ = pkt; }

  // Control whether the final packet read succeeds.
  void set_read_success(bool success) { read_success_ = success; }

 protected:
  uint32_t read_irq_status_raw() override {
    if (irq_idx_ < irq_seq_.size()) {
      return irq_seq_[irq_idx_++];
    }
    return 0;
  }

  bool read_rx_packet(RadioRxPacket &packet, bool blocking_wait, uint32_t irq_status) override {
    (void) blocking_wait;
    (void) irq_status;
    if (read_success_) {
      packet = expected_packet_;
      return true;
    }
    return false;
  }

 private:
  std::vector<uint32_t> irq_seq_;
  size_t irq_idx_ = 0;
  RadioRxPacket expected_packet_{};
  bool read_success_ = true;
};

// ============================================================================
// Test cases
// ============================================================================

TEST(RadioSX1262, WaitForPacketSuccess_DioFired) {
  // Arrange: DIO fires immediately, IRQ reports RX_DONE.
  MockSpi spi;
  MockPin rst, dio1, busy(false);  // busy pin low (not busy)
  TestableRadioSX1262 radio(&spi, &rst, &dio1, &busy, 0, 0);
  radio.mark_dio_fired_from_isr();  // simulate interrupt

  RadioRxPacket pkt{};
  pkt.len = 4;
  pkt.data[0] = 0xAA;
  pkt.data[1] = 0xBB;
  pkt.data[2] = 0xCC;
  pkt.data[3] = 0xDD;
  radio.set_expected_packet(pkt);

  // Need one IRQ read after DIO path
  radio.set_irq_sequence({SX1262_IRQ_RX_DONE});

  RadioRxPacket result{};
  bool ok = radio.wait_for_packet(result, 100);  // 100ms timeout

  // DIO fired early → packet received successfully
  EXPECT_TRUE(ok) << "DIO interrupt should cause wait_for_packet to return true";
  EXPECT_EQ(result.len, 4u) << "packet length should match expected 4 bytes";
  EXPECT_EQ(result.data[0], 0xAA) << "first byte should match transmitted pattern";
  EXPECT_EQ(result.data[1], 0xBB) << "second byte should match transmitted pattern";
  EXPECT_EQ(result.data[2], 0xCC) << "third byte should match transmitted pattern";
  EXPECT_EQ(result.data[3], 0xDD) << "fourth byte should match transmitted pattern";
}

TEST(RadioSX1262, WaitForPacketSuccess_IrqOnly) {
  // Arrange: No DIO, IRQ directly indicates RX_DONE.
  MockSpi spi;
  MockPin rst, dio1, busy(false);
  TestableRadioSX1262 radio(&spi, &rst, &dio1, &busy, 0, 0);
  // DIO not fired by default.

  RadioRxPacket pkt{};
  pkt.len = 2;
  pkt.data[0] = 0x11;
  pkt.data[1] = 0x22;
  radio.set_expected_packet(pkt);

  radio.set_irq_sequence({SX1262_IRQ_RX_DONE});

  RadioRxPacket result{};
  bool ok = radio.wait_for_packet(result, 100);

  // IRQ directly reported RX_DONE without DIO → success path
  EXPECT_TRUE(ok) << "IRQ RX_DONE without DIO should yield a successful receive";
  EXPECT_EQ(result.len, 2u) << "packet length should be 2 bytes as configured";
  EXPECT_EQ(result.data[0], 0x11) << "first data byte should match expected packet";
  EXPECT_EQ(result.data[1], 0x22) << "second data byte should match expected packet";
}

TEST(RadioSX1262, WaitForPacketTimeout_NoActivity) {
  // Arrange: Neither DIO nor IRQ ever signals. Short timeout to keep test fast.
  MockSpi spi;
  MockPin rst, dio1, busy(false);
  TestableRadioSX1262 radio(&spi, &rst, &dio1, &busy, 0, 0);

  // IRQ sequence empty -> always returns 0.

  RadioRxPacket result{};
  const uint32_t feeds_before = esphome::App.feed_wdt_calls;
  bool ok = radio.wait_for_packet(result, 5);  // 5ms timeout

  // No IRQ or DIO → timeout, report failure
  EXPECT_FALSE(ok) << "absence of any radio activity should cause timeout and return false";
  // result should remain empty
  EXPECT_EQ(result.len, 0u) << "on timeout, result packet length should be zero";
  EXPECT_GT(esphome::App.feed_wdt_calls, feeds_before)
      << "a multi-millisecond blocking wait must feed the watchdog, or a real timeout resets the board";
}

TEST(RadioSX1262, WaitForPacketRaceCondition_Resolved) {
  // Arrange: First IRQ has only SYNC_WORD_VALID, second read (inside race handler) adds RX_DONE.
  MockSpi spi;
  MockPin rst, dio1, busy(false);
  TestableRadioSX1262 radio(&spi, &rst, &dio1, &busy, 0, 0);

  RadioRxPacket pkt{};
  pkt.len = 3;
  pkt.data[0] = 0xDE;
  pkt.data[1] = 0xAD;
  pkt.data[2] = 0xBE;
  radio.set_expected_packet(pkt);

  // Sequence: 1st read returns SYNC only; 2nd read returns SYNC|RX_DONE.
  radio.set_irq_sequence({SX1262_IRQ_SYNC_WORD_VALID, SX1262_IRQ_SYNC_WORD_VALID | SX1262_IRQ_RX_DONE});

  RadioRxPacket result{};
  bool ok = radio.wait_for_packet(result, 100);

  // SYNC before RX_DONE race handled by resolve_sync_race → eventual success
  EXPECT_TRUE(ok) << "SYNC_WORD_VALID followed by RX_DONE should be resolved and yield packet";
  EXPECT_EQ(result.len, 3u) << "packet length should be 3 bytes as configured";
  EXPECT_EQ(result.data[0], 0xDE) << "first byte of payload should match expected pattern";
  EXPECT_EQ(result.data[1], 0xAD) << "second byte of payload should match expected pattern";
  EXPECT_EQ(result.data[2], 0xBE) << "third byte of payload should match expected pattern";
}

TEST(RadioSX1262, WaitForPacketRaceCondition_Timeout) {
  // Arrange: SYNC appears but RX_DONE never does before timeout.
  MockSpi spi;
  MockPin rst, dio1, busy(false);
  TestableRadioSX1262 radio(&spi, &rst, &dio1, &busy, 0, 0);

  // First read: SYNC; subsequent reads always SYNC (no RX_DONE).
  radio.set_irq_sequence({SX1262_IRQ_SYNC_WORD_VALID, SX1262_IRQ_SYNC_WORD_VALID});

  RadioRxPacket result{};
  const uint32_t feeds_before = esphome::App.feed_wdt_calls;
  bool ok = radio.wait_for_packet(result, 5);  // short timeout

  // SYNC without subsequent RX_DONE within timeout → failure path
  EXPECT_FALSE(ok) << "SYNC_WORD_VALID without RX_DONE before timeout should cause failure";
  EXPECT_EQ(result.len, 0u) << "on race-condition timeout, result length should be zero";
  EXPECT_GT(esphome::App.feed_wdt_calls, feeds_before)
      << "the sync-race wait loop is a separate blocking path and must feed the watchdog too";
}

TEST(RadioSX1262, WaitForPacketReadFailure) {
  // Arrange: RX_DONE arrives but packet read fails (CRC error simulated).
  MockSpi spi;
  MockPin rst, dio1, busy(false);
  TestableRadioSX1262 radio(&spi, &rst, &dio1, &busy, 0, 0);

  radio.set_irq_sequence({SX1262_IRQ_RX_DONE});
  radio.set_read_success(false);  // read_rx_packet_ returns false

  RadioRxPacket result{};
  bool ok = radio.wait_for_packet(result, 100);

  // Even though IRQ indicated RX_DONE, the packet read itself fails (e.g., CRC)
  EXPECT_FALSE(ok) << "packet read failure (CRC error) should cause wait_for_packet to return false";
}

// ============================================================================
// is_sync_detected / is_preamble_detected tests
// ============================================================================

TEST(RadioSX1262, IsSyncDetected_True) {
  MockSpi spi;
  MockPin rst, dio1, busy(false);
  TestableRadioSX1262 radio(&spi, &rst, &dio1, &busy, 0, 0);

  radio.set_irq_sequence({SX1262_IRQ_SYNC_WORD_VALID});
  EXPECT_TRUE(radio.is_sync_detected()) << "should return true when SyncWordValid IRQ bit is set";
}

TEST(RadioSX1262, IsSyncDetected_False) {
  MockSpi spi;
  MockPin rst, dio1, busy(false);
  TestableRadioSX1262 radio(&spi, &rst, &dio1, &busy, 0, 0);

  radio.set_irq_sequence({0x0000});
  EXPECT_FALSE(radio.is_sync_detected()) << "should return false when no IRQ bits are set";
}

TEST(RadioSX1262, IsPreambleDetected_True) {
  MockSpi spi;
  MockPin rst, dio1, busy(false);
  TestableRadioSX1262 radio(&spi, &rst, &dio1, &busy, 0, 0);

  radio.set_irq_sequence({SX1262_IRQ_PREAMBLE_DETECTED});
  EXPECT_TRUE(radio.is_preamble_detected()) << "should return true when PreambleDetected IRQ bit is set";
}

TEST(RadioSX1262, IsPreambleDetected_False) {
  MockSpi spi;
  MockPin rst, dio1, busy(false);
  TestableRadioSX1262 radio(&spi, &rst, &dio1, &busy, 0, 0);

  radio.set_irq_sequence({SX1262_IRQ_RX_DONE});  // other bits set, not preamble
  EXPECT_FALSE(radio.is_preamble_detected()) << "should return false when only other IRQ bits are set";
}

TEST(RadioSX1262, IsSyncDetected_WithOtherBits) {
  MockSpi spi;
  MockPin rst, dio1, busy(false);
  TestableRadioSX1262 radio(&spi, &rst, &dio1, &busy, 0, 0);

  // Sync + preamble + RX done all set
  radio.set_irq_sequence({SX1262_IRQ_SYNC_WORD_VALID | SX1262_IRQ_PREAMBLE_DETECTED | SX1262_IRQ_RX_DONE});
  EXPECT_TRUE(radio.is_sync_detected()) << "should detect sync even when other IRQ bits are also set";
}

// ============================================================================
// UART encode/decode and CRC validation tests
// ============================================================================

TEST(RadioSX1262, UartEncodeDecodeRoundTrip) {
  // A valid IO-homecontrol frame with CRC appended should encode and decode cleanly.
  const uint8_t frame[] = {0xCE, 0x00, 0xC0, 0xFF, 0xEE, 0xAA, 0xBB, 0xCC, 0x3C, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
  const uint8_t frame_len = sizeof(frame);

  // Append CRC
  uint16_t crc = crc_ccitt(frame, frame_len);
  uint8_t frame_with_crc[32];
  memcpy(frame_with_crc, frame, frame_len);
  frame_with_crc[frame_len] = crc & 0xFF;
  frame_with_crc[frame_len + 1] = (crc >> 8) & 0xFF;

  // UART-encode
  uint8_t encoded[64] = {0};
  uint8_t encoded_len = uart_encode_packet(frame_with_crc, frame_len + 2, encoded, sizeof(encoded));
  ASSERT_GT(encoded_len, 0u);

  // Decode and verify via find_uart_probe (which includes CRC validation)
  UartProbeResult probe = find_uart_probe(encoded, encoded_len);
  ASSERT_TRUE(probe.valid) << "CRC-valid frame should be accepted by find_uart_probe";
  EXPECT_EQ(probe.frame_len, frame_len);
  EXPECT_EQ(memcmp(probe.decoded + probe.frame_start, frame, frame_len), 0)
      << "Decoded frame should match original byte-for-byte";
}

TEST(RadioSX1262, UartProbeRejectsBitErrors) {
  // Encode a valid frame, then flip a bit — CRC should reject it.
  const uint8_t frame[] = {0xCE, 0x00, 0xC0, 0xFF, 0xEE, 0xAA, 0xBB, 0xCC, 0x3C, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
  const uint8_t frame_len = sizeof(frame);

  uint16_t crc = crc_ccitt(frame, frame_len);
  uint8_t frame_with_crc[32];
  memcpy(frame_with_crc, frame, frame_len);
  frame_with_crc[frame_len] = crc & 0xFF;
  frame_with_crc[frame_len + 1] = (crc >> 8) & 0xFF;

  uint8_t encoded[64] = {0};
  uint8_t encoded_len = uart_encode_packet(frame_with_crc, frame_len + 2, encoded, sizeof(encoded));
  ASSERT_GT(encoded_len, 0u);

  // Flip a bit in the middle of the encoded stream (simulates demodulator error)
  encoded[encoded_len / 2] ^= 0x20;

  UartProbeResult probe = find_uart_probe(encoded, encoded_len);
  // The probe should either not find a valid frame, or find one at a wrong offset
  // that doesn't match our original. In either case, it must NOT return our frame
  // with corrupted bytes.
  if (probe.valid) {
    // If something was found, it should NOT match the original (wrong CRC would reject the real frame)
    bool matches_original =
        (probe.frame_len == frame_len && memcmp(probe.decoded + probe.frame_start, frame, frame_len) == 0);
    EXPECT_FALSE(matches_original) << "Bit-corrupted frame should not pass CRC validation";
  }
  // Either probe.valid==false (rejected entirely) or it found a different candidate — both are acceptable
}

TEST(RadioSX1262, UartProbeAcceptsMacTrailerFrame) {
  // Full RX path for a 1W CMD 0x30 "add controller" frame (IoFrame::has_mac), not just parse() in
  // isolation: find_uart_probe() runs the CRC search (find_crc_valid_frame, which must reach the
  // wider declared_len + HMAC_SIZE candidate length to recover a trailer-bearing frame) and
  // classifies the candidate as a plausible frame (is_plausible_uart_frame(), which accepts it via
  // the 1W bit) before this test's own parse() call re-derives the same fields from the recovered
  // bytes — mirroring UartEncodeDecodeRoundTrip above, the established pattern for driving this
  // path, extended with the trailer-specific assertions. Same field values as
  // proto_frame_test.cpp's AddControllerMacTrailerRoundTrip / tests/corpus/captures/
  // reference_1w_vectors/oneway_add_controller_kat.yaml.
  IoFrame frame{};
  init_frame(frame, /*is_2w=*/false, /*start=*/true, /*end=*/true, /*low_power=*/false);
  const uint8_t src[NODE_ID_SIZE] = {0xAB, 0xCD, 0xEF};
  const uint8_t dst[NODE_ID_SIZE] = {0x00, 0x00, 0x3F};
  set_src(frame, src);
  set_dst(frame, dst);
  const uint8_t payload[20] = {0x7E, 0x60, 0x49, 0x1F, 0x97, 0x6A, 0xDF, 0x65, 0x3D, 0xB0,
                               0xED, 0x78, 0x5E, 0x49, 0xA2, 0x01, 0x02, 0x01, 0x12, 0x34};
  ASSERT_TRUE(set_cmd(frame, CMD_ONEWAY_ADD_CONTROLLER, payload, sizeof(payload)));
  frame.has_mac = true;
  const uint8_t mac[HMAC_SIZE] = {0x19, 0xE8, 0x1E, 0xC4, 0x3D, 0x5E};
  memcpy(frame.mac, mac, HMAC_SIZE);

  uint8_t body[FRAME_MAX_WIRE_SIZE] = {0};
  const uint8_t body_len = serialize(frame, body, sizeof(body));
  ASSERT_EQ(body_len, 35) << "declared 29 bytes plus the 6-byte MAC trailer";

  uint16_t crc = crc_ccitt(body, body_len);
  uint8_t frame_with_crc[FRAME_MAX_WIRE_SIZE];
  memcpy(frame_with_crc, body, body_len);
  frame_with_crc[body_len] = crc & 0xFF;
  frame_with_crc[body_len + 1] = (crc >> 8) & 0xFF;

  uint8_t encoded[64] = {0};
  uint8_t encoded_len =
      uart_encode_packet(frame_with_crc, static_cast<uint8_t>(body_len + 2), encoded, sizeof(encoded));
  ASSERT_GT(encoded_len, 0u);

  UartProbeResult probe = find_uart_probe(encoded, encoded_len);
  ASSERT_TRUE(probe.valid) << "CRC-valid MAC-trailer frame should be accepted by find_uart_probe";
  EXPECT_EQ(probe.frame_len, body_len)
      << "recovered frame length should include the trailer, not just the declared bytes";

  IoFrame parsed{};
  ASSERT_TRUE(parse(probe.decoded + probe.frame_start, probe.frame_len, parsed))
      << "recovered bytes should re-parse as the same MAC-trailer frame";
  EXPECT_TRUE(parsed.has_mac) << "classification should see the frame as MAC-trailer-bearing after recovery";
  EXPECT_EQ(memcmp(parsed.mac, mac, HMAC_SIZE), 0) << "recovered MAC should survive the CRC search + decode path";
  EXPECT_EQ(parsed.cmd, CMD_ONEWAY_ADD_CONTROLLER);
}

TEST(RadioSX1262, UartDecodeFixedStride) {
  // Verify decode_uart_probe correctly decodes a known UART-encoded byte.
  // UART frame for byte 0xA5: start(0), 1,0,1,0,0,1,0,1, stop(1) = 10 bits
  // LSB first: bit0=1, bit1=0, bit2=1, bit3=0, bit4=0, bit5=1, bit6=0, bit7=1
  // Bitstream MSB-first in bytes: 0_10100101_1 = 0b0101001011... packed into bytes
  // Let's just use uart_encode_packet for a single byte and verify decode.
  uint8_t input[] = {0xA5};
  uint8_t encoded[2] = {0};
  uint8_t elen = uart_encode_packet(input, 1, encoded, sizeof(encoded));
  ASSERT_GT(elen, 0u);

  uint8_t decoded[4] = {0};
  uint8_t dlen = decode_uart_probe(encoded, elen, 0, decoded, sizeof(decoded));
  ASSERT_GE(dlen, 1u);
  EXPECT_EQ(decoded[0], 0xA5);
}

// ============================================================================
// Preamble unit regression: SetPacketParams' PreambleLength field is bit-denominated on this
// chip, but every caller passes a byte count (LONG_PREAMBLE, SHORT_PREAMBLE, response_preamble()).
// set_packet_params_() must convert.
// ============================================================================

TEST(RadioSX1262, SendPacketSetsPacketParamsPreambleInBits) {
  ScriptedSpi spi;
  MockPin rst, dio1, busy(false);
  TestableRadioSX1262 radio(&spi, &rst, &dio1, &busy, 0, 0);

  const uint8_t frame[] = {0xC8, 0x00, 0xAA, 0xBB, 0xCC, 0xC0, 0xFF, 0xEE, 0x31};

  RadioTxConfig cfg;
  cfg.freq_hz = FREQ_CH2;
  cfg.preamble_len = LONG_PREAMBLE;
  // As in the LR1121 TX test: send_packet reliably times out waiting for TX_DONE in a
  // synchronous host test, but SetPacketParams/WriteBuffer are both issued before that wait.
  radio.send_packet(frame, sizeof(frame), cfg);

  int write_buffer_idx = -1;
  for (size_t i = 0; i < spi.transactions().size(); i++) {
    if (!spi.transactions()[i].empty() && spi.transactions()[i][0] == SX1262_WRITE_BUFFER) {
      write_buffer_idx = static_cast<int>(i);
      break;
    }
  }
  ASSERT_GE(write_buffer_idx, 0);

  int packet_params_idx = -1;
  for (int i = write_buffer_idx - 1; i >= 0; i--) {
    if (!spi.transactions()[i].empty() && spi.transactions()[i][0] == SX1262_SET_PACKET_PARAMS) {
      packet_params_idx = i;
      break;
    }
  }
  ASSERT_GE(packet_params_idx, 0) << "SetPacketParams must be issued before WriteBuffer for TX";

  const auto &pp = spi.transactions()[packet_params_idx];
  ASSERT_EQ(pp.size(), 10u) << "opcode(1) + 9 packet-param bytes";
  const uint16_t preamble_field = (static_cast<uint16_t>(pp[1]) << 8) | pp[2];
  EXPECT_EQ(preamble_field, static_cast<uint16_t>(LONG_PREAMBLE * 8))
      << "SetPacketParams' PreambleLength is in bits: LONG_PREAMBLE (" << LONG_PREAMBLE
      << " bytes) must reach the chip as " << (LONG_PREAMBLE * 8) << " bits, not " << LONG_PREAMBLE
      << " bits (an 8x-too-short on-air preamble)";
}

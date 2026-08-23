#include "radio_sx1262.h"
#include "radio_interface.h"
#include "proto_constants.h"

#include "esphome/core/application.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"
#include "stubs/scripted_spi.h"

#include <algorithm>
#include <cstring>
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

  // Arm the hop holdoff directly — exposed here since note_reception_in_progress_() is protected
  // on RadioDriver, and this fixture's overridden read_rx_packet() below never reaches the
  // sync-without-RX_DONE branch that would normally arm it.
  void note_reception_from_test() { this->note_reception_in_progress_(); }

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
  // The length-driven receive now runs ahead of the race handler and declines on this all-zero
  // mock buffer, but it spends real air time getting there. The host hal.h stubs advance millis()
  // and micros() one unit per call, so that ~1 ms of air time costs ~1000 fake milliseconds of
  // budget; the window is sized for the stub, not for the protocol.
  bool ok = radio.wait_for_packet(result, 20000);

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
// wait_busy_()/failed_ hoisted to SoftPhyDriverBase (shared verbatim with LR1121): this chip
// never had LR1121's "already failed, don't re-run the timeout" short-circuit before the hoist —
// harmless at this chip's 10 ms timeout, but the same bug shape. Mirrors
// RadioLR1121.WaitBusyShortCircuitsAfterFailure (tests/radio_lr1121_test.cpp), proving the shared
// base-class behavior now applies here too.
// ============================================================================

TEST(RadioSX1262, WaitBusyShortCircuitsAfterFailure) {
  // BUSY pin held permanently high (never clears) simulates a genuinely dead chip.
  MockSpi spi;
  MockPin rst, dio1, busy(true);
  TestableRadioSX1262 radio(&spi, &rst, &dio1, &busy, 0, 0);

  const uint32_t feeds_before = esphome::App.feed_wdt_calls;
  radio.set_mode_standby();  // First BUSY wait times out (SX1262_BUSY_TIMEOUT_MS = 10) and sets failed_.
  ASSERT_TRUE(radio.is_failed());
  EXPECT_GT(esphome::App.feed_wdt_calls, feeds_before)
      << "the BUSY-pin wait is a distinct blocking path (SPI turnaround, not RX) and must feed too";

  // The host hal.h millis() stub is a monotonic call counter (+1 per call), so a full 10 ms
  // timeout costs ~11 calls to reach; a short-circuited call costs 0. 5 is a safe cut well below
  // "ran the loop" and well above "returned immediately".
  uint32_t const t_before_second = esphome::millis();
  radio.set_mode_standby();  // Must short-circuit instead of re-running the full timeout.
  uint32_t const t_after_second = esphome::millis();

  EXPECT_LT(t_after_second - t_before_second, 5u)
      << "a failed driver must not re-run the BUSY timeout on every subsequent command";
}

// ============================================================================
// Preamble-unmask coupled fix (radio_robustness_plan.md Experiment 1): configure_radio_() now
// includes PreambleDetected in irqMask, so read_irq_status_raw() can genuinely report it on real
// hardware. activity_irq_mask() must exclude it from what poll_until_activity_()/check_for_packet()
// treat as terminal, or a preamble mid-reception tears down RX via reset_rx_state_() before the
// frame that follows ever arrives. Same trap, same fix, as the LR1121 bug this mirrors (see
// SX1262_IRQ_ACTIVITY_MASK's doc comment in radio_sx1262.h).
// ============================================================================

TEST(RadioSX1262, WaitForPacketIgnoresPreambleOnlyActivity) {
  // Guards against poll_until_activity_() treating a preamble-only IRQ status as terminal — that
  // would make finalize_receive_() see no RX_DONE and call reset_rx_state_(), destroying the frame
  // that is still arriving. Proves the driver waits through repeated preamble-only readings and
  // still catches the later RX_DONE.
  MockSpi spi;
  MockPin rst, dio1, busy(false);
  TestableRadioSX1262 radio(&spi, &rst, &dio1, &busy, 0, 0);

  RadioRxPacket pkt{};
  pkt.len = 2;
  pkt.data[0] = 0x11;
  pkt.data[1] = 0x22;
  radio.set_expected_packet(pkt);
  radio.set_irq_sequence({SX1262_IRQ_PREAMBLE_DETECTED, SX1262_IRQ_PREAMBLE_DETECTED, SX1262_IRQ_RX_DONE});

  RadioRxPacket result{};
  bool ok = radio.wait_for_packet(result, 100);

  EXPECT_TRUE(ok) << "preamble-only readings must not be treated as terminal activity";
  EXPECT_EQ(result.len, 2u);
}

TEST(RadioSX1262, CheckForPacketPreambleOnlyDoesNotResetRx) {
  // Same trap on the non-blocking path: check_for_packet() must clear just the preamble bit (so
  // the next real completion can still raise the DIO edge) and must NOT call reset_rx_state_(),
  // which would issue SetStandby and tear down RX mid-reception.
  ScriptedSpi spi;
  MockPin rst, dio1, busy(false);
  TestableRadioSX1262 radio(&spi, &rst, &dio1, &busy, 0, 0);
  radio.mark_dio_fired_from_isr();
  radio.set_irq_sequence({SX1262_IRQ_PREAMBLE_DETECTED});

  RadioRxPacket packet{};
  bool ok = radio.check_for_packet(packet);

  EXPECT_FALSE(ok) << "preamble alone is not a complete packet yet";

  bool cleared_irq = false, called_standby = false;
  for (const auto &tx : spi.transactions()) {
    if (tx.empty())
      continue;
    if (tx[0] == SX1262_CLEAR_IRQ_STATUS)
      cleared_irq = true;
    if (tx[0] == SX1262_SET_STANDBY)
      called_standby = true;
  }
  EXPECT_TRUE(cleared_irq) << "the preamble bit must be cleared so it can re-fire";
  EXPECT_FALSE(called_standby) << "must not tear down RX via reset_rx_state_()";
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

TEST(RadioSX1262, UartProbeAcceptsEveryKnownIoCommand) {
  // Regression for a real hardware failure (2026-08-16): find_uart_probe() only accepts a
  // CRC-valid candidate whose cmd passes is_known_io_command() (radio_soft_phy.cpp/.h).
  // CMD_GET_GENERAL_INFO3_RESP (0x59) was missing from that switch, so a real, CRC-valid probe
  // reply on real hardware was silently dropped on the SX1262/LR1121 software PHY and reported
  // as a timeout; the same audit also found CMD_IDENTIFY and
  // CMD_WRITE_PRIVATE/CMD_WRITE_PRIVATE_ACK missing, affecting the shipped identify_device()
  // action (and climate writes). This test does not hand-maintain its own opcode list -- an
  // earlier version did, and that duplicate list was itself a third hand-maintained table that
  // could (and did) drift from is_known_io_command(). Instead it iterates every possible command
  // byte and calls is_known_io_command() directly, so the two can never disagree by construction;
  // what it verifies is that find_uart_probe() actually honors that answer for every byte value,
  // not a fixed sample of them.
  for (int cmd_int = 0; cmd_int <= 0xFF; cmd_int++) {
    const auto cmd = static_cast<uint8_t>(cmd_int);
    if (!is_known_io_command(cmd))
      continue;
    SCOPED_TRACE(::testing::Message() << "cmd=0x" << std::hex << static_cast<int>(cmd));
    const uint8_t frame[] = {0xCE, 0x00, 0xC0, 0xFF, 0xEE, 0xAA, 0xBB, 0xCC, cmd, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    const uint8_t frame_len = sizeof(frame);

    const uint16_t crc = crc_ccitt(frame, frame_len);
    uint8_t frame_with_crc[32];
    memcpy(frame_with_crc, frame, frame_len);
    frame_with_crc[frame_len] = crc & 0xFF;
    frame_with_crc[frame_len + 1] = (crc >> 8) & 0xFF;

    uint8_t encoded[64] = {0};
    const uint8_t encoded_len = uart_encode_packet(frame_with_crc, frame_len + 2, encoded, sizeof(encoded));
    ASSERT_GT(encoded_len, 0u);

    const UartProbeResult probe = find_uart_probe(encoded, encoded_len);
    ASSERT_TRUE(probe.valid) << "find_uart_probe() rejected a cmd that is_known_io_command() accepts: 0x" << std::hex
                             << static_cast<int>(cmd);
    EXPECT_EQ(probe.frame_len, frame_len);
    EXPECT_EQ(memcmp(probe.decoded + probe.frame_start, frame, frame_len), 0);
  }
}

TEST(RadioSX1262, NamedCommandsAreEitherKnownOrDocumentedNeverReceived) {
  // UartProbeAcceptsEveryKnownIoCommand above closes the class of bug where find_uart_probe()
  // disagrees with is_known_io_command() -- but it cannot catch the original bug itself: an
  // opcode that proto_constants.h names (so this codebase knows about it and gives it a symbol)
  // yet is_known_io_command() has never been taught to accept, because that opcode is genuinely
  // never expected to arrive on the soft-PHY receive path this codebase controls. Every such
  // opcode must be *this specific, checked, and reasoned-about list* -- not silence. An opcode
  // that's missing from both this list and is_known_io_command() fails this test, so a newly
  // named constant forces a deliberate choice (teach is_known_io_command() about it, or document
  // here why it's exempt) instead of silently falling through a soft-PHY reception gap.
  //
  // Reasoned exclusions, checked against actual usage in this codebase (not proto_constants.h's
  // doxygen alone) as of 2026-08-16:
  //   - CMD_ACTIVATE_MODE: only decoded by decode_1w_frame() (proto_codecs.cpp), and every 1W
  //     frame carries CTRL0_PROTOCOL_1W, which is_plausible_uart_frame() (radio_soft_phy.cpp)
  //     already accepts unconditionally -- is_known_io_command() is never consulted for it.
  //   - CMD_ONEWAY_ADD_CONTROLLER / CMD_ONEWAY_REMOVE: same 1W bypass as CMD_ACTIVATE_MODE above,
  //     not a gap -- both are actively received and handled (hub_oneway_key_adoption.cpp's
  //     try_adopt_oneway_key_(), pairing_advisor.cpp), just never through is_known_io_command().
  //   - CMD_SET_SENSOR / CMD_SET_SENSOR_ACK: zero references anywhere outside proto_constants.*.
  //   - CMD_DISCOVER_ALT_REQ: this codebase can transmit it (proto_commands.cpp, as an optional
  //     `pairing_discovery_commands` entry), but the reply PairingEngine actually waits for is the
  //     generic CMD_DISCOVER_RESP (0x29, already known) regardless of which discovery variant was
  //     sent -- and hub_key_extraction.cpp's try_handle_key_extraction_frame_() only recognizes
  //     CMD_DISCOVER_REQ (0x28), not this variant, so there is no live dispatch path that needs
  //     to receive 0x2E itself.
  //   - CMD_DISCOVER_ALT_RESP: captured once (velux_kux100/discover_alt_addressed_challenge_
  //     response.yaml) but "not otherwise used anywhere in this codebase" per its own doxygen --
  //     nothing dispatches on it, corpus replay doesn't go through this soft-PHY gate at all.
  //   - CMD_ADDRESS_RESP: captured once (velux_kux100/pairing_full.yaml) but we only ever
  //     transmit it (create_address_resp_device_role(), the key-extraction responder's answer to
  //     an inbound 0x36) -- we never wait for a reply carrying this opcode. CMD_ADDRESS_REQ is
  //     deliberately NOT in this list any more: the key-extraction responder now expects to
  //     receive it, so it belongs in is_known_io_command() instead (see radio_soft_phy.cpp).
  //   - CMD_LAUNCH_KEY_TRANSFER: never observed in corpus or field log; not sent or handled
  //     anywhere; used only to construct a hypothetical IV in proto_crypto_test.cpp.
  //   - CMD_UNKNOWN4A_REQ: deliberately excluded, see ADR 0024 -- nothing this codebase sends
  //     would ever draw a reply carrying this opcode.
  //   - CMD_GET_INFO1 / CMD_GET_INFO1_RESP: unimplemented, never sent, so never a reply to wait
  //     for; this codebase only uses CMD_GET_INFO2/CMD_GET_GENERAL_INFO3 (both already known).
  //   - CMD_SEND_RAW_MESSAGE / CMD_READ_GROUPS / CMD_REBOOT / CMD_SERVICE_STATUS_ACK: never
  //     observed in corpus or field log, not sent or handled anywhere in this codebase -- named
  //     purely so a future capture of one of them has a symbol to log against.
  static constexpr uint8_t NEVER_RECEIVED_ALLOWLIST[] = {
      CMD_ACTIVATE_MODE,
      CMD_ONEWAY_ADD_CONTROLLER,
      CMD_ONEWAY_REMOVE,
      CMD_SET_SENSOR,
      CMD_SET_SENSOR_ACK,
      CMD_DISCOVER_ALT_REQ,
      CMD_DISCOVER_ALT_RESP,
      CMD_ADDRESS_RESP,
      CMD_LAUNCH_KEY_TRANSFER,
      CMD_UNKNOWN4A_REQ,
      CMD_GET_INFO1,
      CMD_GET_INFO1_RESP,
      CMD_SEND_RAW_MESSAGE,
      CMD_READ_GROUPS,
      CMD_REBOOT,
      CMD_SERVICE_STATUS_ACK,
  };

  for (int cmd_int = 0; cmd_int <= 0xFF; cmd_int++) {
    const auto cmd = static_cast<uint8_t>(cmd_int);
    if (std::strcmp(command_name(cmd), "UNKNOWN_CMD") == 0)
      continue;  // Not a named opcode at all -- outside this test's scope.
    if (is_known_io_command(cmd))
      continue;
    const bool allowlisted = std::find(std::begin(NEVER_RECEIVED_ALLOWLIST), std::end(NEVER_RECEIVED_ALLOWLIST), cmd) !=
                             std::end(NEVER_RECEIVED_ALLOWLIST);
    EXPECT_TRUE(allowlisted) << "cmd=0x" << std::hex << static_cast<int>(cmd) << " (" << command_name(cmd)
                             << ") is named by command_name() but missing from both "
                                "is_known_io_command() and this test's documented allowlist -- decide "
                                "which one it belongs in";
  }
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

// ============================================================================
// TX modulation-quality erratum (datasheet §15.1): bit 2 of SX1262_REG_TX_MODULATION must be set
// for every (G)FSK transmission. The chip transmits perfectly happily without it, just with
// degraded modulation quality, so nothing but this test catches a regression.
// ============================================================================

TEST(RadioSX1262, SendPacketAppliesTxModulationWorkaroundBeforeSetTx) {
  ScriptedSpi spi;
  MockPin rst, dio1, busy(false);
  TestableRadioSX1262 radio(&spi, &rst, &dio1, &busy, 0, 0);

  const uint8_t frame[] = {0xC8, 0x00, 0xAA, 0xBB, 0xCC, 0xC0, 0xFF, 0xEE, 0x31};

  RadioTxConfig cfg;
  cfg.freq_hz = FREQ_CH2;
  cfg.preamble_len = SHORT_PREAMBLE;
  // As in SendPacketSetsPacketParamsPreambleInBits: send_packet times out waiting for TX_DONE in a
  // synchronous host test, but everything up to and including SetTx has already been issued.
  radio.send_packet(frame, sizeof(frame), cfg);

  int set_tx_idx = -1;
  for (size_t i = 0; i < spi.transactions().size(); i++) {
    if (!spi.transactions()[i].empty() && spi.transactions()[i][0] == SX1262_SET_TX) {
      set_tx_idx = static_cast<int>(i);
      break;
    }
  }
  ASSERT_GE(set_tx_idx, 0);

  int workaround_idx = -1;
  for (int i = set_tx_idx - 1; i >= 0; i--) {
    const auto &tx = spi.transactions()[i];
    if (tx.size() == 4 && tx[0] == SX1262_WRITE_REGISTER &&
        tx[1] == static_cast<uint8_t>(SX1262_REG_TX_MODULATION >> 8) &&
        tx[2] == static_cast<uint8_t>(SX1262_REG_TX_MODULATION)) {
      workaround_idx = i;
      break;
    }
  }
  ASSERT_GE(workaround_idx, 0) << "SX1262_REG_TX_MODULATION must be written before every SetTx "
                                  "(datasheet §15.1 TX modulation-quality erratum)";
  EXPECT_NE(spi.transactions()[workaround_idx][3] & SX1262_TX_MODULATION_GFSK_BIT, 0)
      << "the (G)FSK-correct value for bit 2 of SX1262_REG_TX_MODULATION is 1, not 0";
}

// ============================================================================
// Length-driven receive: RX runs in fixed-length mode at SOFT_PHY_RX_PROBE_PACKET_LEN, so
// RX_DONE lands a fixed ~10 ms after the sync word regardless of how short the frame was. A
// frame's own length is knowable from its first decoded byte, so the shared flow reads it out on
// air time instead. CRC validation is the gate; anything less falls back to the RX_DONE path.
// ============================================================================

namespace {

// Build the raw bytes the chip's data buffer holds mid-reception: the frame, its CRC-CCITT
// trailer, and the whole lot UART-packed exactly as it arrived off air.
std::vector<uint8_t> uart_packed_on_air_bytes(const std::vector<uint8_t> &frame) {
  std::vector<uint8_t> with_crc = frame;
  const uint16_t crc = crc_ccitt(frame.data(), static_cast<uint8_t>(frame.size()));
  with_crc.push_back(static_cast<uint8_t>(crc & 0xFF));
  with_crc.push_back(static_cast<uint8_t>((crc >> 8) & 0xFF));

  uint8_t encoded[RADIO_PACKET_BUFFER_SIZE] = {0};
  const uint8_t encoded_len =
      uart_encode_packet(with_crc.data(), static_cast<uint8_t>(with_crc.size()), encoded, sizeof(encoded));
  return std::vector<uint8_t>(encoded, encoded + encoded_len);
}

// A real 15-byte RS100 challenge (0x3C) — the exact frame the hub must turn around fastest, from
// tests/corpus/captures/issues/field_rs100_pairing_key_transfer_timeout.yaml.
const std::vector<uint8_t> kRs100Challenge = {0x0E, 0x00, 0xD1, 0xD4, 0xFF, 0x8C, 0x08, 0x3C,
                                              0x3C, 0x45, 0x51, 0x6F, 0xFE, 0x59, 0x80};

// A genuine on-air encoding of `frame` with one byte flipped past the header, so
// soft_phy_peek_frame_length() (which only looks at CTRL0's cell) still returns the real length and
// stage 1 proceeds — it is stage 3's CRC check that must reject it. An all-noise buffer would
// instead be rejected at stage 1 (see PeekFrameLengthRejectsNoise) and never exercise the CRC gate
// the tests using this helper target.
std::vector<uint8_t> crc_corrupted_on_air_bytes(const std::vector<uint8_t> &frame) {
  std::vector<uint8_t> corrupted = uart_packed_on_air_bytes(frame);
  EXPECT_GT(corrupted.size(), 5u);
  corrupted[5] ^= 0xFF;
  return corrupted;
}

}  // namespace

// Serves a scripted data buffer, standing in for the chip's buffer as reception progresses, and
// records every read so tests can assert what the early path actually asked for.
class EarlyRxRadioSX1262 : public RadioSX1262 {
 public:
  using RadioSX1262::RadioSX1262;

  void set_irq_sequence(std::initializer_list<uint32_t> seq) {
    irq_seq_.assign(seq);
    irq_idx_ = 0;
  }
  void set_rx_buffer(std::vector<uint8_t> buf) { rx_buffer_ = std::move(buf); }
  void set_fallback_packet(const RadioRxPacket &pkt) { fallback_packet_ = pkt; }
  using RadioSX1262::early_rx_read_offset;
  // check_for_packet()'s idle-path early completion (issue #81, Mechanism B) has no
  // caller-supplied timeout to widen the way wait_for_packet()'s tests do, so the budget itself is
  // the seam — default it to the same generous 20000 ms those tests pass, and let a test narrow it
  // to exercise the decline path (see idle_rx_completion_budget_ms()'s own doc comment for why the
  // host clock stubs make this the only workable seam).
  void set_idle_rx_completion_budget_ms(uint32_t ms) { idle_rx_completion_budget_ms_ = ms; }

  const std::vector<uint8_t> &read_lengths() const { return read_lengths_; }
  const std::vector<uint8_t> &read_offsets() const { return read_offsets_; }
  bool fallback_used() const { return fallback_used_; }

 protected:
  uint32_t read_irq_status_raw() override {
    if (irq_idx_ < irq_seq_.size())
      return irq_seq_[irq_idx_++];
    return 0;
  }

  void read_rx_buffer(uint8_t offset, uint8_t *data, uint8_t len) override {
    read_offsets_.push_back(offset);
    read_lengths_.push_back(len);
    for (uint8_t i = 0; i < len; i++)
      data[i] = i < rx_buffer_.size() ? rx_buffer_[i] : 0x00;
  }

  bool read_rx_packet(RadioRxPacket &packet, bool blocking_wait, uint32_t irq_status) override {
    (void) blocking_wait;
    (void) irq_status;
    fallback_used_ = true;
    packet = fallback_packet_;
    return packet.len > 0;
  }

  uint32_t idle_rx_completion_budget_ms() const override { return idle_rx_completion_budget_ms_; }

 private:
  std::vector<uint32_t> irq_seq_;
  size_t irq_idx_ = 0;
  std::vector<uint8_t> rx_buffer_;
  std::vector<uint8_t> read_lengths_;
  std::vector<uint8_t> read_offsets_;
  RadioRxPacket fallback_packet_{};
  bool fallback_used_ = false;
  uint32_t idle_rx_completion_budget_ms_ = 20000;
};

TEST(SoftPhy, RawBytesForFrameCoversFrameAndCrcCells) {
  // 15 protocol bytes + 2 CRC = 17 UART cells = 170 bits = 22 raw bytes.
  EXPECT_EQ(soft_phy_raw_bytes_for_frame(15), 22);
  EXPECT_EQ(soft_phy_raw_bytes_for_frame(FRAME_MIN_SIZE), 14);
  // Even the longest possible frame stays well inside the raw scratch buffer.
  EXPECT_EQ(soft_phy_raw_bytes_for_frame(FRAME_MAX_SIZE), 43);
  EXPECT_LE(soft_phy_raw_bytes_for_frame(FRAME_MAX_SIZE), RADIO_PACKET_BUFFER_SIZE);
}

TEST(SoftPhy, PeekFrameLengthRecoversLengthFromFirstUartCell) {
  const std::vector<uint8_t> raw = uart_packed_on_air_bytes(kRs100Challenge);
  ASSERT_GE(raw.size(), SOFT_PHY_EARLY_HEADER_RAW_BYTES);

  // Three raw bytes — a quarter of a millisecond of air time — is enough to learn the whole
  // frame's length, because CTRL0 bits [4:0] carry it.
  EXPECT_EQ(soft_phy_peek_frame_length(raw.data(), SOFT_PHY_EARLY_HEADER_RAW_BYTES), kRs100Challenge.size());
}

TEST(SoftPhy, PeekFrameLengthRejectsNoise) {
  const uint8_t zeros[SOFT_PHY_EARLY_HEADER_RAW_BYTES] = {0x00, 0x00, 0x00};
  EXPECT_EQ(soft_phy_peek_frame_length(zeros, sizeof(zeros)), 0) << "no stop bit — not a UART cell at any offset";

  const uint8_t ones[SOFT_PHY_EARLY_HEADER_RAW_BYTES] = {0xFF, 0xFF, 0xFF};
  EXPECT_EQ(soft_phy_peek_frame_length(ones, sizeof(ones)), 0) << "no start bit — not a UART cell at any offset";
}

TEST(SoftPhy, AirTimeIsRoundedUpToWholeBytes) {
  // 38400 bps: one byte is 208.33 µs, and the helper must never round below that.
  EXPECT_GE(soft_phy_air_time_us(1), 208u);
  EXPECT_GE(soft_phy_air_time_us(48), 10000u) << "the fixed-length RX window costs a full 10 ms";
  EXPECT_LT(soft_phy_air_time_us(22 + SOFT_PHY_EARLY_READ_MARGIN_BYTES), soft_phy_air_time_us(48))
      << "a 15-byte frame must complete well before the fixed-length window would";
}

TEST(RadioSX1262, WaitForPacketCompletesOnAirTimeWithoutRxDone) {
  MockSpi spi;
  MockPin rst, dio1, busy(false);
  EarlyRxRadioSX1262 radio(&spi, &rst, &dio1, &busy, 0, 0);

  radio.set_rx_buffer(uart_packed_on_air_bytes(kRs100Challenge));
  // SYNC_WORD_VALID only — RX_DONE never arrives. Under the old flow this receive could only
  // time out; the frame is nonetheless entirely on air and recoverable.
  radio.set_irq_sequence({SX1262_IRQ_SYNC_WORD_VALID});

  RadioRxPacket pkt{};
  // The host hal.h stubs advance millis() and micros() by one unit per call, so a real frame's
  // ~5 ms of air time costs several thousand fake milliseconds here. The timeout is sized for the
  // stub, not for the protocol.
  ASSERT_TRUE(radio.wait_for_packet(pkt, 20000)) << "a complete, CRC-valid frame must not need RX_DONE";
  EXPECT_FALSE(radio.fallback_used()) << "the RX_DONE path must not have been reached";

  ASSERT_EQ(pkt.len, kRs100Challenge.size());
  EXPECT_EQ(std::vector<uint8_t>(pkt.data, pkt.data + pkt.len), kRs100Challenge);

  // It must read from the RX base address, and read only the frame's own bytes plus margin —
  // never the full fixed-length window.
  ASSERT_GE(radio.read_lengths().size(), 2u) << "expected a header read then a whole-frame read";
  for (uint8_t offset : radio.read_offsets())
    EXPECT_EQ(offset, SX1262_RX_BUFFER_BASE);
  EXPECT_EQ(radio.read_lengths().front(), SOFT_PHY_EARLY_HEADER_RAW_BYTES);
  EXPECT_EQ(radio.read_lengths().back(),
            soft_phy_raw_bytes_for_frame(kRs100Challenge.size()) + SOFT_PHY_EARLY_READ_MARGIN_BYTES);
  EXPECT_LT(radio.read_lengths().back(), SOFT_PHY_RX_PROBE_PACKET_LEN);
}

TEST(RadioSX1262, WaitForPacketFallsBackToRxDoneWhenEarlyReadDoesNotValidate) {
  MockSpi spi;
  MockPin rst, dio1, busy(false);
  EarlyRxRadioSX1262 radio(&spi, &rst, &dio1, &busy, 0, 0);

  // A buffer that never yields a CRC-valid frame — the shape of a chip that turns out not to
  // expose its buffer mid-reception, or of a spurious sync detect.
  radio.set_rx_buffer(std::vector<uint8_t>(SOFT_PHY_RX_PROBE_PACKET_LEN, 0x00));
  radio.set_irq_sequence({SX1262_IRQ_SYNC_WORD_VALID, SX1262_IRQ_RX_DONE});

  RadioRxPacket fallback{};
  fallback.len = 4;
  fallback.data[0] = 0xDE;
  radio.set_fallback_packet(fallback);

  RadioRxPacket pkt{};
  // Generous for the same host-stub clock reason as the test above: the early path must get far
  // enough to read the buffer and reject it on its merits, not be cut short by the timeout.
  ASSERT_TRUE(radio.wait_for_packet(pkt, 20000));
  EXPECT_TRUE(radio.fallback_used()) << "a failed early read must cost latency, never the frame";
  EXPECT_EQ(pkt.len, 4);
  EXPECT_EQ(pkt.data[0], 0xDE);
}

TEST(RadioSX1262, EarlyCompletionIsOptInPerChip) {
  // The base class default keeps a chip on the RX_DONE path until it declares its buffer
  // readable mid-reception; SX1262 opts in with the RX base it already programs.
  MockSpi spi;
  MockPin rst, dio1, busy(false);
  EarlyRxRadioSX1262 radio(&spi, &rst, &dio1, &busy, 0, 0);
  EXPECT_EQ(radio.early_rx_read_offset(), SX1262_RX_BUFFER_BASE);
}

// ============================================================================
// Idle-path hop holdoff (issue #81): check_for_packet()'s sync-without-RX_DONE branch must arm
// reception_in_progress() so ExchangeEngine::maybe_hop() does not retune under a frame that is
// still arriving, and a completed reception (successful or not) must drop the holdoff again via
// reset_rx_state_().
// ============================================================================

TEST(RadioSX1262, CheckForPacketSyncWithoutRxDoneArmsHopHoldoff) {
  MockSpi spi;
  MockPin rst, dio1, busy(false);
  TestableRadioSX1262 radio(&spi, &rst, &dio1, &busy, 0, 0);
  radio.mark_dio_fired_from_isr();
  radio.set_irq_sequence({SX1262_IRQ_SYNC_WORD_VALID});

  RadioRxPacket packet{};
  bool ok = radio.check_for_packet(packet);

  EXPECT_FALSE(ok) << "SYNC_WORD_VALID without RX_DONE is not a complete packet yet";
  EXPECT_TRUE(radio.reception_in_progress())
      << "the sync-without-RX_DONE branch must record the reception so the idle-path hop holds off";
}

TEST(RadioSX1262, ResetRxStateClearsHopHoldoff) {
  // read_rx_packet() itself is overridden away from the real SPI path by this fixture (as in
  // every other test in this file), so this drives reset_rx_state_() — the single funnel every
  // "RX torn down and re-armed" path goes through, including read_rx_packet()'s own end — via
  // check_for_packet()'s catch-all branch instead: an IRQ reading that is activity (CRC_ERR) but
  // neither SYNC_WORD_VALID nor RX_DONE reaches the same real, non-overridden reset_rx_state_()
  // call that read_rx_packet() reaches on every one of its own return paths.
  MockSpi spi;
  MockPin rst, dio1, busy(false);
  TestableRadioSX1262 radio(&spi, &rst, &dio1, &busy, 0, 0);
  radio.note_reception_from_test();
  ASSERT_TRUE(radio.reception_in_progress()) << "sanity: the holdoff must start armed";

  radio.mark_dio_fired_from_isr();
  radio.set_irq_sequence({SX1262_IRQ_CRC_ERR});

  RadioRxPacket result{};
  EXPECT_FALSE(radio.check_for_packet(result));
  EXPECT_FALSE(radio.reception_in_progress())
      << "reset_rx_state_() must drop the holdoff, not leave a stale deadline blocking the next "
         "~12 ms of hopping";
}

TEST(RadioSX1262, EarlyCompletionDeclinesWindowsTooShortToFinishIn) {
  // A window smaller than the longest frame's air time cannot complete a receive either way, so
  // the early path must not spend that whole budget discovering it.
  MockSpi spi;
  MockPin rst, dio1, busy(false);
  EarlyRxRadioSX1262 radio(&spi, &rst, &dio1, &busy, 0, 0);

  radio.set_rx_buffer(uart_packed_on_air_bytes(kRs100Challenge));
  radio.set_irq_sequence({SX1262_IRQ_SYNC_WORD_VALID});

  RadioRxPacket pkt{};
  EXPECT_FALSE(radio.wait_for_packet(pkt, SOFT_PHY_EARLY_MIN_WINDOW_MS - 1));
  EXPECT_TRUE(radio.read_lengths().empty()) << "no buffer read should have been attempted at all";
}

// ============================================================================
// Idle-path early completion (issue #81, Mechanism B): check_for_packet()'s sync-without-RX_DONE
// branch, which Step 1 made non-lossy by arming the hop holdoff before falling through, now also
// attempts try_early_completion_() itself instead of leaving every sync-only poll to the following
// loop() pass. A wrong guess here must never cost the frame — only latency — so the interesting
// assertions are about what did *not* happen (no chip teardown, holdoff still armed) as much as
// about what did.
// ============================================================================

TEST(RadioSX1262, CheckForPacketSyncWithoutRxDoneCompletesEarlyOnValidCrc) {
  MockSpi spi;
  MockPin rst, dio1, busy(false);
  EarlyRxRadioSX1262 radio(&spi, &rst, &dio1, &busy, 0, 0);

  radio.mark_dio_fired_from_isr();
  radio.set_rx_buffer(uart_packed_on_air_bytes(kRs100Challenge));
  // RX_DONE deliberately absent — this is the branch that used to just return false and wait a
  // whole loop() pass (or lose the frame outright, pre-#81) for it.
  radio.set_irq_sequence({SX1262_IRQ_SYNC_WORD_VALID});

  RadioRxPacket pkt{};
  EXPECT_TRUE(radio.check_for_packet(pkt)) << "a complete, CRC-valid frame must not need to wait for RX_DONE";
  EXPECT_FALSE(radio.fallback_used()) << "the RX_DONE path must not have been reached";

  ASSERT_EQ(pkt.len, kRs100Challenge.size());
  EXPECT_EQ(std::vector<uint8_t>(pkt.data, pkt.data + pkt.len), kRs100Challenge);

  ASSERT_GE(radio.read_lengths().size(), 2u) << "expected a header read then a whole-frame read";
  for (uint8_t offset : radio.read_offsets())
    EXPECT_EQ(offset, SX1262_RX_BUFFER_BASE);
  EXPECT_FALSE(radio.reception_in_progress())
      << "try_early_completion_()'s success clears the holdoff via reset_rx_state_() (Step 1); a "
         "regression here would spuriously block hops for up to RX_HOP_HOLDOFF_US after every "
         "successful idle-path receive";
}

TEST(RadioSX1262, CheckForPacketSyncWithoutRxDoneFallsBackWhenCrcInvalid) {
  // ScriptedSpi, not MockSpi: the assertion below needs to see whether reset_rx_state_() ran, and
  // that shows up as real SPI transactions (SetStandby among them) that MockSpi discards.
  ScriptedSpi spi;
  MockPin rst, dio1, busy(false);
  EarlyRxRadioSX1262 radio(&spi, &rst, &dio1, &busy, 0, 0);

  radio.mark_dio_fired_from_isr();
  std::vector<uint8_t> corrupted = crc_corrupted_on_air_bytes(kRs100Challenge);
  radio.set_rx_buffer(corrupted);
  radio.set_irq_sequence({SX1262_IRQ_SYNC_WORD_VALID});

  RadioRxPacket pkt{};
  EXPECT_FALSE(radio.check_for_packet(pkt));
  EXPECT_FALSE(radio.fallback_used()) << "check_for_packet() must not have gone through read_rx_packet()";
  // Pins that this test actually reaches stage 3's CRC gate rather than being rejected earlier at
  // stage 1's length peek: a header read followed by a whole-frame read means stage 1 succeeded and
  // stage 2 ran, so the false above can only be stage 3's CRC check.
  ASSERT_GE(radio.read_lengths().size(), 2u) << "expected a header read then a whole-frame read";

  // Load-bearing: this is what proves the failure path leaves the chip alone (§4.1(b) of the plan)
  // rather than tearing RX down — the basis of the whole "fall-through is benign, not lossy" claim.
  for (const auto &txn : spi.transactions())
    ASSERT_FALSE(!txn.empty() && txn[0] == SX1262_SET_STANDBY) << "reset_rx_state_() must not have run";
}

TEST(RadioSX1262, CheckForPacketSyncWithoutRxDoneKeepsHopHoldoffOnFallBack) {
  MockSpi spi;
  MockPin rst, dio1, busy(false);
  EarlyRxRadioSX1262 radio(&spi, &rst, &dio1, &busy, 0, 0);

  radio.mark_dio_fired_from_isr();
  std::vector<uint8_t> corrupted = crc_corrupted_on_air_bytes(kRs100Challenge);
  radio.set_rx_buffer(corrupted);
  radio.set_irq_sequence({SX1262_IRQ_SYNC_WORD_VALID});

  RadioRxPacket pkt{};
  EXPECT_FALSE(radio.check_for_packet(pkt));
  // Pins that this test actually reaches stage 3's CRC gate rather than being rejected earlier at
  // stage 1's length peek (see the sibling FallsBackWhenCrcInvalid test's identical assertion).
  ASSERT_GE(radio.read_lengths().size(), 2u) << "expected a header read then a whole-frame read";
  // This is what pins issue #81's §0.3 recovery claim: without the holdoff still standing after
  // early completion declines, nothing in the suite would distinguish "fall-through is recoverable
  // next loop() pass" from "fall-through is lossy" — a future edit that dropped the re-arm on the
  // fall-through path would pass every other test in this file.
  EXPECT_TRUE(radio.reception_in_progress())
      << "the hop holdoff armed before the early-completion attempt must survive it declining";
}

TEST(RadioSX1262, CheckForPacketSyncWithoutRxDoneDeclinesWhenBudgetTooShort) {
  MockSpi spi;
  MockPin rst, dio1, busy(false);
  EarlyRxRadioSX1262 radio(&spi, &rst, &dio1, &busy, 0, 0);
  radio.set_idle_rx_completion_budget_ms(SOFT_PHY_EARLY_MIN_WINDOW_MS - 1);

  radio.mark_dio_fired_from_isr();
  radio.set_rx_buffer(uart_packed_on_air_bytes(kRs100Challenge));
  radio.set_irq_sequence({SX1262_IRQ_SYNC_WORD_VALID});

  RadioRxPacket pkt{};
  EXPECT_FALSE(radio.check_for_packet(pkt));
  EXPECT_TRUE(radio.read_lengths().empty())
      << "a budget below SOFT_PHY_EARLY_MIN_WINDOW_MS must decline before touching the buffer";
}

TEST(SoftPhy, EncodePadsTrailingIdleBitsHigh) {
  // 3 bytes = 30 bits = 4 encoded bytes (32 bits), so 2 bits of the last byte are line-idle time
  // rather than data. They must go out as idle-high, not as a spurious start bit.
  const uint8_t data[3] = {0x00, 0xFF, 0x5A};
  uint8_t encoded[RADIO_PACKET_BUFFER_SIZE] = {0};
  const uint8_t encoded_len = uart_encode_packet(data, sizeof(data), encoded, sizeof(encoded));
  ASSERT_EQ(encoded_len, 4);

  const uint16_t data_bits = sizeof(data) * UART_CELL_BITS;
  for (uint16_t pos = data_bits; pos < static_cast<uint16_t>(encoded_len) * 8; pos++) {
    const uint8_t bit = (encoded[pos / 8] >> (7 - (pos % 8))) & 0x01;
    EXPECT_EQ(bit, 1) << "trailing bit " << pos << " must idle high";
  }

  // Padding must not disturb the cells themselves: the stream still round-trips.
  uint8_t decoded[RADIO_PACKET_BUFFER_SIZE] = {0};
  ASSERT_EQ(decode_uart_probe(encoded, encoded_len, 0, decoded, sizeof(decoded)), sizeof(data));
  EXPECT_EQ(memcmp(decoded, data, sizeof(data)), 0);
}

TEST(SoftPhy, EncodeLeavesNoPaddingWhenCellsFillWholeBytes) {
  // 4 bytes = 40 bits = exactly 5 encoded bytes, so there is nothing to pad and the last bit
  // written is the frame's own stop bit.
  const uint8_t data[4] = {0xC8, 0x00, 0x12, 0x34};
  uint8_t encoded[RADIO_PACKET_BUFFER_SIZE] = {0};
  ASSERT_EQ(uart_encode_packet(data, sizeof(data), encoded, sizeof(encoded)), 5);

  uint8_t decoded[RADIO_PACKET_BUFFER_SIZE] = {0};
  ASSERT_EQ(decode_uart_probe(encoded, 5, 0, decoded, sizeof(decoded)), sizeof(data));
  EXPECT_EQ(memcmp(decoded, data, sizeof(data)), 0);
}

// ============================================================================
// Post-TX re-arm: the peer can answer within a millisecond or two of our carrier dropping, so the
// path back into RX carries only what it must. SetStandby is redundant (SetRxTxFallbackMode puts
// the chip in standby the instant TxDone fires) and the buffer base has not moved since init.
// ============================================================================

// Raises the TxDone interrupt the moment SetTx is issued. send_packet() clears the DIO latch
// immediately before arming, so a flag set beforehand is discarded — the interrupt has to arrive
// after. This is the only way to reach send_packet()'s success path in a synchronous host test,
// and the post-TX re-arm runs nowhere else.
class TxCompletingRadioSX1262 : public TestableRadioSX1262 {
 public:
  using TestableRadioSX1262::TestableRadioSX1262;

 protected:
  void start_tx() override {
    TestableRadioSX1262::start_tx();
    this->mark_dio_fired_from_isr();
  }
};

TEST(RadioSX1262, PostTxRearmSkipsRedundantStandbyAndBufferBase) {
  ScriptedSpi spi;
  MockPin rst, dio1, busy(false);
  TxCompletingRadioSX1262 radio(&spi, &rst, &dio1, &busy, 0, 0);
  radio.set_irq_sequence({SX1262_IRQ_TX_DONE});

  const uint8_t frame[] = {0xC8, 0x00, 0xAA, 0xBB, 0xCC, 0xC0, 0xFF, 0xEE, 0x31};
  RadioTxConfig cfg;
  cfg.freq_hz = FREQ_CH2;
  cfg.preamble_len = SHORT_PREAMBLE;
  ASSERT_TRUE(radio.send_packet(frame, sizeof(frame), cfg)) << "TxDone was driven, so this must succeed";

  int set_tx_idx = -1;
  for (size_t i = 0; i < spi.transactions().size(); i++) {
    if (!spi.transactions()[i].empty() && spi.transactions()[i][0] == SX1262_SET_TX) {
      set_tx_idx = static_cast<int>(i);
      break;
    }
  }
  ASSERT_GE(set_tx_idx, 0);

  // Everything after SetTx is the re-arm path.
  int standby = 0, buffer_base = 0, packet_params = 0, set_rx = 0;
  for (size_t i = static_cast<size_t>(set_tx_idx) + 1; i < spi.transactions().size(); i++) {
    const auto &tx = spi.transactions()[i];
    if (tx.empty())
      continue;
    if (tx[0] == SX1262_SET_STANDBY)
      standby++;
    else if (tx[0] == SX1262_SET_BUFFER_BASE_ADDRESS)
      buffer_base++;
    else if (tx[0] == SX1262_SET_PACKET_PARAMS)
      packet_params++;
    else if (tx[0] == SX1262_SET_RX)
      set_rx++;
  }

  EXPECT_EQ(standby, 0) << "the chip is already in standby via SetRxTxFallbackMode when TxDone fires";
  EXPECT_EQ(buffer_base, 0) << "the RX buffer base was written at init and has not moved";
  EXPECT_EQ(packet_params, 1) << "RX packet params must be restored — the transmission overwrote them";
  EXPECT_EQ(set_rx, 1) << "and the radio must actually re-enter RX";
}

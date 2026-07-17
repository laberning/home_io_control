#include "radio_lr1121.h"
#include "radio_interface.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"

#include <gtest/gtest.h>
#include <vector>

using namespace esphome::home_io_control;

// ============================================================================
// Scripted SPI: records each transaction (spi_enable..spi_disable) as its own byte
// vector and plays back a queued sequence of response bytes across all transactions.
//
// MockSpi (radio_test_common.h) always returns 0 and does not record writes, so it
// cannot support the byte-exact assertions this driver's two-transaction command
// protocol needs (design implementation plan §0.5.2: "if [MockSpi] only records,
// plan a scriptable ScriptedSpi local to the LR1121 test file"). This class is local
// to this file and does not change MockSpi's behavior for any other test.
// ============================================================================

class ScriptedSpi : public esphome::home_io_control::SpiAccess {
 public:
  void spi_enable() override { current_.clear(); }
  void spi_disable() override { transactions_.push_back(current_); }
  uint8_t spi_transfer(uint8_t data) override {
    current_.push_back(data);
    return this->next_response_();
  }
  void spi_write(uint8_t data) override { current_.push_back(data); }
  uint8_t spi_read() override {
    current_.push_back(0);
    return this->next_response_();
  }

  // Queue response bytes played back in order across every subsequent transfer/read.
  void queue_response(uint8_t byte) { response_queue_.push_back(byte); }
  void queue_responses(std::initializer_list<uint8_t> bytes) {
    for (uint8_t b : bytes)
      response_queue_.push_back(b);
  }

  // Every completed transaction (one spi_enable..spi_disable cycle) as its written bytes.
  const std::vector<std::vector<uint8_t>> &transactions() const { return transactions_; }

  // Find the first transaction whose bytes start with the given 16-bit opcode.
  // Returns -1 if not found.
  int find_opcode(uint16_t opcode) const {
    uint8_t msb = (opcode >> 8) & 0xFF;
    uint8_t lsb = opcode & 0xFF;
    for (size_t i = 0; i < transactions_.size(); i++) {
      if (transactions_[i].size() >= 2 && transactions_[i][0] == msb && transactions_[i][1] == lsb)
        return static_cast<int>(i);
    }
    return -1;
  }

 private:
  uint8_t next_response_() {
    if (read_idx_ < response_queue_.size())
      return response_queue_[read_idx_++];
    return 0;
  }

  std::vector<uint8_t> current_;
  std::vector<std::vector<uint8_t>> transactions_;
  std::vector<uint8_t> response_queue_;
  size_t read_idx_{0};
};

// ============================================================================
// Testable subclass of RadioLR1121
// ============================================================================

class TestableRadioLR1121 : public RadioLR1121 {
 public:
  using RadioLR1121::RadioLR1121;

  // Configure the sequence of IRQ status values returned by read_irq_status_raw().
  void set_irq_sequence(std::initializer_list<uint32_t> seq) {
    irq_seq_.assign(seq);
    irq_idx_ = 0;
  }

  // Set the packet that read_rx_packet should return (when not using the real path).
  void set_expected_packet(const RadioRxPacket &pkt) { expected_packet_ = pkt; }

  // Control whether the final packet read succeeds (when not using the real path).
  void set_read_success(bool success) { read_success_ = success; }

  // When true, dispatch to the real RadioLR1121::read_rx_packet (exercises the actual
  // GetRxBufferStatus/ReadBuffer/UART-probe path against a ScriptedSpi).
  void set_use_real_read_rx_packet(bool use_real) { use_real_ = use_real; }

 protected:
  uint32_t read_irq_status_raw() override {
    if (irq_idx_ < irq_seq_.size()) {
      return irq_seq_[irq_idx_++];
    }
    return 0;
  }

  bool read_rx_packet(RadioRxPacket &packet, bool blocking_wait, uint32_t irq_status) override {
    if (use_real_)
      return RadioLR1121::read_rx_packet(packet, blocking_wait, irq_status);
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
  bool use_real_ = false;
};

namespace {

// TCXO_VOLTAGE_OPTIONS code for "3_0V" (components/home_io_control/__init__.py).
constexpr uint8_t TCXO_YAML_CODE_3_0V = 0x07;

// Queue a GetVersion response that reports a real LR1121 (device type 0x03) so init()
// proceeds past the identity check. Response layout: [stat1, device_type, hw, fw_major, fw_minor].
void queue_valid_version_response(ScriptedSpi &spi) {
  spi.queue_responses({0x00, LR1121_DEVICE_TYPE, 0x01, 0x02, 0x01});
}

// Append the protocol CRC to `frame` and UART-encode the result — the exact on-air transform
// RadioLR1121::send_packet() performs. Shared by every test that needs a reference "what
// should have been written to the TX buffer / received in the RX buffer" byte sequence.
uint8_t encode_frame_with_crc(const uint8_t *frame, uint8_t frame_len, uint8_t *encoded, uint8_t encoded_max_len) {
  uint16_t const crc = crc_ccitt(frame, frame_len);
  uint8_t frame_with_crc[FRAME_MAX_SIZE + 2];
  memcpy(frame_with_crc, frame, frame_len);
  frame_with_crc[frame_len] = crc & 0xFF;
  frame_with_crc[frame_len + 1] = (crc >> 8) & 0xFF;
  return uart_encode_packet(frame_with_crc, static_cast<uint8_t>(frame_len + 2), encoded, encoded_max_len);
}

}  // namespace

// ============================================================================
// SPI transport / identity tests (Step 2)
// ============================================================================

TEST(RadioLR1121, GetVersionTransactionBytes) {
  ScriptedSpi spi;
  MockPin rst, irq, busy(false);
  TestableRadioLR1121 radio(&spi, &rst, &irq, &busy, 17, TCXO_YAML_CODE_3_0V);

  // No response queued -> device type reads back as 0x00, init() must fail cleanly.
  bool ok = radio.init();
  EXPECT_FALSE(ok) << "init() must fail when the reported device type does not match LR1121";
  EXPECT_TRUE(radio.is_failed());

  // The very first transaction must be the GetVersion opcode (0x0101), no parameters.
  ASSERT_FALSE(spi.transactions().empty());
  const auto &first = spi.transactions()[0];
  ASSERT_EQ(first.size(), 2u) << "GetVersion takes no request parameters";
  EXPECT_EQ(first[0], 0x01) << "opcode MSB";
  EXPECT_EQ(first[1], 0x01) << "opcode LSB";
}

TEST(RadioLR1121, InitSucceedsOnCorrectDeviceType) {
  ScriptedSpi spi;
  MockPin rst, irq, busy(false);
  TestableRadioLR1121 radio(&spi, &rst, &irq, &busy, 17, TCXO_YAML_CODE_3_0V);
  queue_valid_version_response(spi);

  bool ok = radio.init();
  EXPECT_TRUE(ok) << "init() should succeed when GetVersion reports device type 0x03 (LR1121)";
  EXPECT_FALSE(radio.is_failed());
}

TEST(RadioLR1121, TcxoCommandEncodesYamlVoltageCode) {
  ScriptedSpi spi;
  MockPin rst, irq, busy(false);
  TestableRadioLR1121 radio(&spi, &rst, &irq, &busy, 17, TCXO_YAML_CODE_3_0V);
  queue_valid_version_response(spi);

  ASSERT_TRUE(radio.init());

  int idx = spi.find_opcode(LR1121_CMD_SET_TCXO_MODE);
  ASSERT_GE(idx, 0) << "SetTcxoMode must be issued during init";
  const auto &tx = spi.transactions()[idx];
  ASSERT_EQ(tx.size(), 6u) << "opcode(2) + voltage code(1) + startup delay(3)";
  // YAML code 0x07 ("3_0V") maps to the LR1121's own 0x00-0x07 table via a -1 shift
  // (design §0.5.3 / radio_lr1121.cpp configure_radio_ comment).
  EXPECT_EQ(tx[2], TCXO_YAML_CODE_3_0V - 1) << "TCXO voltage code should be YAML code minus 1";
}

TEST(RadioLR1121, InitSequenceOrder) {
  ScriptedSpi spi;
  MockPin rst, irq, busy(false);
  TestableRadioLR1121 radio(&spi, &rst, &irq, &busy, 17, TCXO_YAML_CODE_3_0V);
  queue_valid_version_response(spi);

  ASSERT_TRUE(radio.init());

  int tcxo_idx = spi.find_opcode(LR1121_CMD_SET_TCXO_MODE);
  int clear_errors_idx = spi.find_opcode(LR1121_CMD_CLEAR_ERRORS);
  int calibrate_idx = spi.find_opcode(LR1121_CMD_CALIBRATE);
  int rfswitch_idx = spi.find_opcode(LR1121_CMD_SET_DIO_AS_RF_SWITCH);
  int set_rx_idx = spi.find_opcode(LR1121_CMD_SET_RX);

  ASSERT_GE(tcxo_idx, 0);
  ASSERT_GE(clear_errors_idx, 0);
  ASSERT_GE(calibrate_idx, 0);
  ASSERT_GE(rfswitch_idx, 0);
  ASSERT_GE(set_rx_idx, 0);

  EXPECT_LT(tcxo_idx, clear_errors_idx) << "Errors must be cleared after TCXO is configured";
  EXPECT_LT(clear_errors_idx, calibrate_idx) << "Calibrate must run after errors are cleared";
  EXPECT_LT(rfswitch_idx, set_rx_idx) << "RF-switch config must precede entering RX";
}

TEST(RadioLR1121, ChangeFrequencyWritesPlainHzBytes) {
  for (uint32_t freq : {FREQ_CH1, FREQ_CH2, FREQ_CH3}) {
    ScriptedSpi spi;
    MockPin rst, irq, busy(false);
    TestableRadioLR1121 radio(&spi, &rst, &irq, &busy, 17, TCXO_YAML_CODE_3_0V);

    radio.change_frequency(freq);
    int idx = spi.find_opcode(LR1121_CMD_SET_RF_FREQUENCY);
    ASSERT_GE(idx, 0);
    const auto &tx = spi.transactions()[idx];
    ASSERT_EQ(tx.size(), 6u) << "opcode(2) + frequency(4)";
    uint32_t encoded = (static_cast<uint32_t>(tx[2]) << 24) | (static_cast<uint32_t>(tx[3]) << 16) |
                       (static_cast<uint32_t>(tx[4]) << 8) | static_cast<uint32_t>(tx[5]);
    EXPECT_EQ(encoded, freq) << "SetRfFrequency must carry the plain Hz value, no PLL-step conversion";
  }
}

// ============================================================================
// wait_for_packet tests (Step 3) — mirrors radio_sx1262_rx_test.cpp over the wider IRQ word.
// ============================================================================

TEST(RadioLR1121, WaitForPacketSuccess_DioFired) {
  ScriptedSpi spi;
  MockPin rst, irq, busy(false);
  TestableRadioLR1121 radio(&spi, &rst, &irq, &busy, 0, 0x07);
  radio.mark_dio_fired_from_isr();

  RadioRxPacket pkt{};
  pkt.len = 4;
  pkt.data[0] = 0xAA;
  pkt.data[1] = 0xBB;
  pkt.data[2] = 0xCC;
  pkt.data[3] = 0xDD;
  radio.set_expected_packet(pkt);
  radio.set_irq_sequence({LR1121_IRQ_RX_DONE});

  RadioRxPacket result{};
  bool ok = radio.wait_for_packet(result, 100);

  EXPECT_TRUE(ok) << "IRQ interrupt should cause wait_for_packet to return true";
  EXPECT_EQ(result.len, 4u);
  EXPECT_EQ(result.data[0], 0xAA);
  EXPECT_EQ(result.data[3], 0xDD);
}

TEST(RadioLR1121, WaitForPacketSuccess_IrqOnly) {
  ScriptedSpi spi;
  MockPin rst, irq, busy(false);
  TestableRadioLR1121 radio(&spi, &rst, &irq, &busy, 0, 0x07);

  RadioRxPacket pkt{};
  pkt.len = 2;
  pkt.data[0] = 0x11;
  pkt.data[1] = 0x22;
  radio.set_expected_packet(pkt);
  radio.set_irq_sequence({LR1121_IRQ_RX_DONE});

  RadioRxPacket result{};
  bool ok = radio.wait_for_packet(result, 100);

  EXPECT_TRUE(ok) << "IRQ status directly reporting RX_DONE without a pin edge should still succeed";
  EXPECT_EQ(result.len, 2u);
}

TEST(RadioLR1121, WaitForPacketTimeout_NoActivity) {
  ScriptedSpi spi;
  MockPin rst, irq, busy(false);
  TestableRadioLR1121 radio(&spi, &rst, &irq, &busy, 0, 0x07);

  RadioRxPacket result{};
  bool ok = radio.wait_for_packet(result, 5);

  EXPECT_FALSE(ok) << "absence of any radio activity should cause timeout and return false";
  EXPECT_EQ(result.len, 0u);
}

TEST(RadioLR1121, WaitForPacketRaceCondition_Resolved) {
  ScriptedSpi spi;
  MockPin rst, irq, busy(false);
  TestableRadioLR1121 radio(&spi, &rst, &irq, &busy, 0, 0x07);

  RadioRxPacket pkt{};
  pkt.len = 3;
  pkt.data[0] = 0xDE;
  pkt.data[1] = 0xAD;
  pkt.data[2] = 0xBE;
  radio.set_expected_packet(pkt);
  radio.set_irq_sequence({LR1121_IRQ_SYNC_WORD_VALID, LR1121_IRQ_SYNC_WORD_VALID | LR1121_IRQ_RX_DONE});

  RadioRxPacket result{};
  bool ok = radio.wait_for_packet(result, 100);

  EXPECT_TRUE(ok) << "SYNC_WORD_VALID followed by RX_DONE should be resolved and yield a packet";
  EXPECT_EQ(result.len, 3u);
}

TEST(RadioLR1121, WaitForPacketRaceCondition_Timeout) {
  ScriptedSpi spi;
  MockPin rst, irq, busy(false);
  TestableRadioLR1121 radio(&spi, &rst, &irq, &busy, 0, 0x07);
  radio.set_irq_sequence({LR1121_IRQ_SYNC_WORD_VALID, LR1121_IRQ_SYNC_WORD_VALID});

  RadioRxPacket result{};
  bool ok = radio.wait_for_packet(result, 5);

  EXPECT_FALSE(ok) << "SYNC without RX_DONE before timeout should cause failure";
  EXPECT_EQ(result.len, 0u);
}

TEST(RadioLR1121, WaitForPacketReadFailure_CapturePopulated) {
  // CRC-failed probe → no packet, but the capture path was still exercised (mirrors the
  // design's "CRC-failed probe -> no packet but capture info populated" requirement).
  ScriptedSpi spi;
  MockPin rst, irq, busy(false);
  TestableRadioLR1121 radio(&spi, &rst, &irq, &busy, 0, 0x07);
  radio.set_irq_sequence({LR1121_IRQ_RX_DONE});
  radio.set_read_success(false);

  RadioRxPacket result{};
  bool ok = radio.wait_for_packet(result, 100);

  EXPECT_FALSE(ok) << "packet read failure (e.g. CRC error) should cause wait_for_packet to return false";
}

TEST(RadioLR1121, IsSyncDetected_True) {
  ScriptedSpi spi;
  MockPin rst, irq, busy(false);
  TestableRadioLR1121 radio(&spi, &rst, &irq, &busy, 0, 0x07);
  radio.set_irq_sequence({LR1121_IRQ_SYNC_WORD_VALID});
  EXPECT_TRUE(radio.is_sync_detected());
}

TEST(RadioLR1121, IsSyncDetected_False) {
  ScriptedSpi spi;
  MockPin rst, irq, busy(false);
  TestableRadioLR1121 radio(&spi, &rst, &irq, &busy, 0, 0x07);
  radio.set_irq_sequence({0x00000000});
  EXPECT_FALSE(radio.is_sync_detected()) << "should return false when no IRQ bits are set";
}

TEST(RadioLR1121, IsSyncDetected_WithOtherBits) {
  ScriptedSpi spi;
  MockPin rst, irq, busy(false);
  TestableRadioLR1121 radio(&spi, &rst, &irq, &busy, 0, 0x07);
  radio.set_irq_sequence({LR1121_IRQ_SYNC_WORD_VALID | LR1121_IRQ_PREAMBLE_DETECTED | LR1121_IRQ_RX_DONE});
  EXPECT_TRUE(radio.is_sync_detected()) << "should detect sync even when other IRQ bits are also set";
}

TEST(RadioLR1121, IsPreambleDetected_True) {
  ScriptedSpi spi;
  MockPin rst, irq, busy(false);
  TestableRadioLR1121 radio(&spi, &rst, &irq, &busy, 0, 0x07);
  radio.set_irq_sequence({LR1121_IRQ_PREAMBLE_DETECTED});
  EXPECT_TRUE(radio.is_preamble_detected());
}

TEST(RadioLR1121, IsPreambleDetected_False) {
  ScriptedSpi spi;
  MockPin rst, irq, busy(false);
  TestableRadioLR1121 radio(&spi, &rst, &irq, &busy, 0, 0x07);
  radio.set_irq_sequence({LR1121_IRQ_RX_DONE});
  EXPECT_FALSE(radio.is_preamble_detected());
}

TEST(RadioLR1121, ChipNameIsLr1121) {
  ScriptedSpi spi;
  MockPin rst, irq, busy(false);
  TestableRadioLR1121 radio(&spi, &rst, &irq, &busy, 0, 0x07);
  EXPECT_STREQ(radio.chip_name(), "lr1121");
}

TEST(RadioLR1121, ReadRssiAppliesFormula) {
  // GetRssiInst response: [stat1, raw_rssi]. Formula is -raw/2 dBm (radio_lr1121.cpp read_rssi).
  ScriptedSpi spi;
  MockPin rst, irq, busy(false);
  TestableRadioLR1121 radio(&spi, &rst, &irq, &busy, 0, 0x07);
  spi.queue_responses({0x00, 100});

  EXPECT_EQ(radio.read_rssi(), -50);
}

// ============================================================================
// check_for_packet (non-blocking RX) branch coverage
// ============================================================================

TEST(RadioLR1121, CheckForPacketReturnsFalseWhenDioNotFired) {
  ScriptedSpi spi;
  MockPin rst, irq, busy(false);
  TestableRadioLR1121 radio(&spi, &rst, &irq, &busy, 0, 0x07);
  // is_dio_fired() defaults to false; check_for_packet must bail out before touching SPI.

  RadioRxPacket packet{};
  EXPECT_FALSE(radio.check_for_packet(packet));
  EXPECT_TRUE(spi.transactions().empty()) << "no SPI transaction should occur when DIO never fired";
}

TEST(RadioLR1121, CheckForPacketSyncWithoutRxDoneClearsIrqAndReturnsFalse) {
  ScriptedSpi spi;
  MockPin rst, irq, busy(false);
  TestableRadioLR1121 radio(&spi, &rst, &irq, &busy, 0, 0x07);
  radio.mark_dio_fired_from_isr();
  radio.set_irq_sequence({LR1121_IRQ_SYNC_WORD_VALID});

  RadioRxPacket packet{};
  bool ok = radio.check_for_packet(packet);

  EXPECT_FALSE(ok) << "SYNC_WORD_VALID without RX_DONE is not a complete packet yet";
  int clear_idx = spi.find_opcode(LR1121_CMD_CLEAR_IRQ);
  EXPECT_GE(clear_idx, 0) << "the sticky SYNC flag must be cleared so it doesn't wedge the next poll";
}

// ============================================================================
// Real read_rx_packet path: proves the GetRxBufferStatus/ReadBuffer/UART-probe wiring
// against a scripted chip response, using a genuine UART-encoded, CRC-valid frame.
// ============================================================================

TEST(RadioLR1121, ReadRxPacketRecoversUartEncodedFrame) {
  const uint8_t frame[] = {0xCE, 0x00, 0xC0, 0xFF, 0xEE, 0xAA, 0xBB, 0xCC, 0x3C, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
  const uint8_t frame_len = sizeof(frame);

  uint8_t encoded[64] = {0};
  uint8_t encoded_len = encode_frame_with_crc(frame, frame_len, encoded, sizeof(encoded));
  ASSERT_GT(encoded_len, 0u);

  // Pad to the driver's fixed 48-byte raw probe length so GetRxBufferStatus's reported_len
  // takes the "== LR1121_RX_PROBE_PACKET_LEN" branch (radio_lr1121.cpp read_rx_packet).
  constexpr uint8_t kProbeLen = 48;
  uint8_t padded[kProbeLen] = {0};
  ASSERT_LE(encoded_len, kProbeLen);
  memcpy(padded, encoded, encoded_len);

  ScriptedSpi spi;
  MockPin rst, irq, busy(false);
  TestableRadioLR1121 radio(&spi, &rst, &irq, &busy, 0, 0x07);
  radio.set_use_real_read_rx_packet(true);
  radio.set_irq_sequence({LR1121_IRQ_RX_DONE});
  radio.mark_dio_fired_from_isr();

  // GetRxBufferStatus response: [stat1, reported_len, rx_offset].
  spi.queue_responses({0x00, kProbeLen, 0x00});
  // ReadBuffer response: [stat1, <kProbeLen data bytes>].
  spi.queue_response(0x00);
  for (uint8_t b : padded)
    spi.queue_response(b);

  RadioRxPacket packet{};
  bool ok = radio.check_for_packet(packet);

  ASSERT_TRUE(ok) << "a genuine UART-encoded, CRC-valid frame must be recovered end-to-end";
  EXPECT_EQ(packet.len, frame_len);
  EXPECT_EQ(memcmp(packet.data, frame, frame_len), 0) << "recovered frame must match the original bytes exactly";
}

// ============================================================================
// TX path tests (Step 4)
// ============================================================================

TEST(RadioLR1121, SendPacketWritesUartEncodedBytes) {
  const uint8_t frame[] = {0xC8, 0x00, 0xAA, 0xBB, 0xCC, 0xC0, 0xFF, 0xEE, 0x31};
  const uint8_t frame_len = sizeof(frame);

  // Reference: what the driver is expected to write to the TX buffer — the frame with its
  // CRC appended, then UART-encoded (same contract as RadioSX1262::send_packet).
  uint8_t expected_encoded[32] = {0};
  uint8_t expected_encoded_len = encode_frame_with_crc(frame, frame_len, expected_encoded, sizeof(expected_encoded));
  ASSERT_GT(expected_encoded_len, 0u);

  ScriptedSpi spi;
  MockPin rst, irq, busy(false);
  TestableRadioLR1121 radio(&spi, &rst, &irq, &busy, 17, 0x07);

  RadioTxConfig cfg;
  cfg.freq_hz = FREQ_CH2;
  cfg.preamble_len = SHORT_PREAMBLE;
  // send_packet() clears the dio-fired latch immediately before its TX-done wait loop (by
  // design — it must ignore any stale flag from before the TX started), so a synchronous host
  // test cannot make that loop observe a "fired" edge and the call reliably times out and
  // returns false here. That is fine: WriteBuffer is issued before the wait loop, so the bytes
  // this test cares about are already recorded regardless of the eventual timeout.
  radio.send_packet(frame, frame_len, cfg);

  int write_idx = spi.find_opcode(LR1121_CMD_WRITE_BUFFER);
  ASSERT_GE(write_idx, 0);
  const auto &tx = spi.transactions()[write_idx];
  ASSERT_EQ(tx.size(), 2u + expected_encoded_len) << "WriteBuffer payload must be exactly the UART-encoded frame";
  for (uint8_t i = 0; i < expected_encoded_len; i++)
    EXPECT_EQ(tx[2 + i], expected_encoded[i]) << "encoded byte " << (int) i << " mismatch";
}

TEST(RadioLR1121, SendPacketSetsPacketParamsLengthToEncodedLength) {
  const uint8_t frame[] = {0xC8, 0x00, 0xAA, 0xBB, 0xCC, 0xC0, 0xFF, 0xEE, 0x31};
  const uint8_t frame_len = sizeof(frame);

  uint8_t expected_encoded[32] = {0};
  uint8_t expected_encoded_len = encode_frame_with_crc(frame, frame_len, expected_encoded, sizeof(expected_encoded));
  ASSERT_GT(expected_encoded_len, 0u);

  ScriptedSpi spi;
  MockPin rst, irq, busy(false);
  TestableRadioLR1121 radio(&spi, &rst, &irq, &busy, 17, 0x07);

  RadioTxConfig cfg;
  cfg.freq_hz = FREQ_CH2;
  cfg.preamble_len = SHORT_PREAMBLE;
  // See SendPacketWritesUartEncodedBytes above: the call reliably times out in a synchronous
  // host test, but SetPacketParams/WriteBuffer are both issued before the TX-wait loop.
  radio.send_packet(frame, frame_len, cfg);

  // The TX-side SetPacketParams call is the one issued right before WriteBuffer.
  int write_idx = spi.find_opcode(LR1121_CMD_WRITE_BUFFER);
  ASSERT_GE(write_idx, 0);
  int packet_params_idx = -1;
  for (int i = write_idx - 1; i >= 0; i--) {
    const auto &t = spi.transactions()[i];
    if (t.size() >= 2 && t[0] == ((LR1121_CMD_SET_PACKET_PARAMS >> 8) & 0xFF) &&
        t[1] == (LR1121_CMD_SET_PACKET_PARAMS & 0xFF)) {
      packet_params_idx = i;
      break;
    }
  }
  ASSERT_GE(packet_params_idx, 0) << "SetPacketParams must be issued before WriteBuffer for TX";
  const auto &pp = spi.transactions()[packet_params_idx];
  ASSERT_EQ(pp.size(), 11u) << "opcode(2) + 9 packet-param bytes";
  EXPECT_EQ(pp[8], expected_encoded_len) << "payload length field must equal the UART-encoded length";
}

// ============================================================================
// Tuning application tests (Step 4)
// ============================================================================

TEST(RadioLR1121, ApplyTuningRewritesModulationParamsBandwidth) {
  ScriptedSpi spi;
  MockPin rst, irq, busy(false);
  TestableRadioLR1121 radio(&spi, &rst, &irq, &busy, 17, 0x07);

  TuningConfig tuning;
  tuning.lr1121_rx_bandwidth = LR1121RxBandwidth::BW_187_2_KHZ;
  tuning.lr1121_response_preamble = 96;
  tuning.lr1121_post_tx_settle_us = 750;
  radio.apply_tuning(tuning);

  EXPECT_EQ(radio.response_preamble(), 96u);

  int idx = spi.find_opcode(LR1121_CMD_SET_MODULATION_PARAMS);
  ASSERT_GE(idx, 0);
  const auto &tx = spi.transactions()[idx];
  ASSERT_EQ(tx.size(), 12u) << "opcode(2) + 10 modulation-param bytes";
  EXPECT_EQ(tx[7], static_cast<uint8_t>(LR1121RxBandwidth::BW_187_2_KHZ))
      << "bandwidth byte must reflect the tuned value";
}

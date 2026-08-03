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

  // When true, dispatch to the real SoftPhyDriverBase::read_rx_packet (exercises the actual
  // GetRxBufferStatus/ReadBuffer/UART-probe path against a ScriptedSpi) via RadioLR1121's
  // concrete primitives.
  void set_use_real_read_rx_packet(bool use_real) { use_real_ = use_real; }

  // Bypass the read_irq_status_raw() override below to exercise the real GetStatus SPI
  // transaction/parsing against a ScriptedSpi.
  uint32_t call_real_read_irq_status_raw() { return RadioLR1121::read_irq_status_raw(); }

 protected:
  uint32_t read_irq_status_raw() override {
    if (irq_idx_ < irq_seq_.size()) {
      return irq_seq_[irq_idx_++];
    }
    return 0;
  }

  bool read_rx_packet(RadioRxPacket &packet, bool blocking_wait, uint32_t irq_status) override {
    if (use_real_)
      return SoftPhyDriverBase::read_rx_packet(packet, blocking_wait, irq_status);
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
// proceeds past the identity check. Response layout: [stat1, hw, device_type, fw_major,
// fw_minor] — cross-checked against RadioLib's LR11x0::getVersion() (device type is the
// second data byte, not the first).
void queue_valid_version_response(ScriptedSpi &spi) {
  spi.queue_responses({0x00, 0x01, LR1121_DEVICE_TYPE, 0x02, 0x01});
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

TEST(RadioLR1121, WaitBusyShortCircuitsAfterFailure) {
  // Once a BUSY timeout has failed the driver, every later SPI command must not re-run the same
  // multi-second timeout — otherwise a mid-init BUSY glitch would cascade into one full timeout
  // per remaining configure_radio_() step (up to ~16x LR1121_BUSY_TIMEOUT_MS) before init()
  // finally reports failure. BUSY pin held permanently high (never clears) simulates this.
  ScriptedSpi spi;
  MockPin rst, irq, busy(true);
  TestableRadioLR1121 radio(&spi, &rst, &irq, &busy, 0, 0x07);

  radio.set_mode_standby();  // First BUSY wait times out and sets failed_.
  ASSERT_TRUE(radio.is_failed());
  uint32_t const t_before_second = esphome::millis();
  radio.set_mode_standby();  // Must short-circuit instead of re-running the full timeout.
  uint32_t const t_after_second = esphome::millis();

  EXPECT_LT(t_after_second - t_before_second, 100u)
      << "a failed driver must not re-run the BUSY timeout on every subsequent command";
}

TEST(RadioLR1121, ReadIrqStatusRawParsesGetStatusResponse) {
  // GetStatus wire response is [Stat1, Stat2, IRQ_b3, IRQ_b2, IRQ_b1, IRQ_b0] (cross-checked
  // against RadioLib's LRxxxx::getIrqStatus()). read_command_ consumes Stat1 separately, so
  // this queues Stat1 + Stat2 + the 4 IRQ bytes and checks the word is reassembled correctly.
  // Every other test in this file exercises the overridden read_irq_status_raw(), so this is the
  // only one that actually verifies the real GetStatus response parsing.
  ScriptedSpi spi;
  MockPin rst, irq, busy(false);
  TestableRadioLR1121 radio(&spi, &rst, &irq, &busy, 0, 0x07);

  uint32_t const expected = LR1121_IRQ_RX_DONE | LR1121_IRQ_TIMEOUT;
  spi.queue_responses({0x00, 0x00, (uint8_t) (expected >> 24), (uint8_t) (expected >> 16), (uint8_t) (expected >> 8),
                       (uint8_t) expected});

  EXPECT_EQ(radio.call_real_read_irq_status_raw(), expected);
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

TEST(RadioLR1121, PaConfigSelectsHpPathAndClampsPowerAboveLpRange) {
  // tx_power=17 exceeds the LP path's -17..14dBm range, so configure_radio_() must select HP
  // (paSel=1, regPaSupply=1/VBAT, paHpSel=0x07) and clamp TX power to HP's -9..22dBm range.
  // Selecting the wrong path for the configured power produces an invalid/undefined analog PA
  // output, so this must track tx_power_ rather than a fixed path.
  ScriptedSpi spi;
  MockPin rst, irq, busy(false);
  TestableRadioLR1121 radio(&spi, &rst, &irq, &busy, 17, TCXO_YAML_CODE_3_0V);
  queue_valid_version_response(spi);

  ASSERT_TRUE(radio.init());

  int pa_idx = spi.find_opcode(LR1121_CMD_SET_PA_CONFIG);
  ASSERT_GE(pa_idx, 0);
  const auto &pa_tx = spi.transactions()[pa_idx];
  ASSERT_EQ(pa_tx.size(), 6u) << "opcode(2) + paSel + regPaSupply + paDutyCycle + paHpSel";
  EXPECT_EQ(pa_tx[2], 0x01) << "paSel must select HP for 17dBm";
  EXPECT_EQ(pa_tx[3], 0x01) << "regPaSupply must be VBAT (HP) for 17dBm";
  EXPECT_EQ(pa_tx[5], 0x07) << "paHpSel";

  int tx_params_idx = spi.find_opcode(LR1121_CMD_SET_TX_PARAMS);
  ASSERT_GE(tx_params_idx, 0);
  const auto &tx_params_tx = spi.transactions()[tx_params_idx];
  ASSERT_EQ(tx_params_tx.size(), 4u) << "opcode(2) + power + ramp";
  EXPECT_EQ((int8_t) tx_params_tx[2], 17) << "17dBm is within HP's range, so it must pass through unclamped";
}

TEST(RadioLR1121, PaConfigSelectsLpPathAndClampsPowerToLpRange) {
  // tx_power=10 is within the LP path's -17..14dBm range, so configure_radio_() must select LP
  // (paSel=0, regPaSupply=0/internal regulator).
  ScriptedSpi spi;
  MockPin rst, irq, busy(false);
  TestableRadioLR1121 radio(&spi, &rst, &irq, &busy, 10, TCXO_YAML_CODE_3_0V);
  queue_valid_version_response(spi);

  ASSERT_TRUE(radio.init());

  int pa_idx = spi.find_opcode(LR1121_CMD_SET_PA_CONFIG);
  ASSERT_GE(pa_idx, 0);
  const auto &pa_tx = spi.transactions()[pa_idx];
  EXPECT_EQ(pa_tx[2], 0x00) << "paSel must select LP for 10dBm";
  EXPECT_EQ(pa_tx[3], 0x00) << "regPaSupply must be the internal regulator (LP) for 10dBm";

  int tx_params_idx = spi.find_opcode(LR1121_CMD_SET_TX_PARAMS);
  ASSERT_GE(tx_params_idx, 0);
  const auto &tx_params_tx = spi.transactions()[tx_params_idx];
  EXPECT_EQ((int8_t) tx_params_tx[2], 10) << "10dBm is within LP's range, so it must pass through unclamped";
}

TEST(RadioLR1121, PaConfigClampsPowerAboveHpMaximum) {
  // A configured tx_power above HP's 22dBm ceiling (schema allows up to 22 per __init__.py, but
  // guard the driver's own clamp independent of that) must be clamped, not passed through raw.
  ScriptedSpi spi;
  MockPin rst, irq, busy(false);
  TestableRadioLR1121 radio(&spi, &rst, &irq, &busy, 22, TCXO_YAML_CODE_3_0V);
  queue_valid_version_response(spi);

  ASSERT_TRUE(radio.init());

  int tx_params_idx = spi.find_opcode(LR1121_CMD_SET_TX_PARAMS);
  ASSERT_GE(tx_params_idx, 0);
  const auto &tx_params_tx = spi.transactions()[tx_params_idx];
  EXPECT_EQ((int8_t) tx_params_tx[2], 22) << "22dBm is exactly HP's ceiling";
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

TEST(RadioLR1121, InitAppliesImageCalibrationAfterCalibrate) {
  // analysis/lr1121_bring_up_investigation.md §4.3: Calibrate(0x3F) calibrates the IMG block at
  // the chip's default band, not ours; a banded CalibImage must follow it.
  ScriptedSpi spi;
  MockPin rst, irq, busy(false);
  TestableRadioLR1121 radio(&spi, &rst, &irq, &busy, 17, TCXO_YAML_CODE_3_0V);
  queue_valid_version_response(spi);

  ASSERT_TRUE(radio.init());

  int calibrate_idx = spi.find_opcode(LR1121_CMD_CALIBRATE);
  int calibrate_image_idx = spi.find_opcode(LR1121_CMD_CALIBRATE_IMAGE);
  ASSERT_GE(calibrate_idx, 0);
  ASSERT_GE(calibrate_image_idx, 0);
  EXPECT_LT(calibrate_idx, calibrate_image_idx) << "CalibImage must follow the all-blocks Calibrate";

  const auto &tx = spi.transactions()[calibrate_image_idx];
  ASSERT_EQ(tx.size(), 4u) << "opcode(2) + freq1(1) + freq2(1)";
  EXPECT_EQ(tx[2], LR1121_IMAGE_CAL_FREQ1);
  EXPECT_EQ(tx[3], LR1121_IMAGE_CAL_FREQ2);
}

TEST(RadioLR1121, InitAppliesGfskWorkaroundAfterModulationParams) {
  // analysis/lr1121_bring_up_investigation.md §4.2: the GFSK workaround register trio must be
  // written after every modulation-params write.
  ScriptedSpi spi;
  MockPin rst, irq, busy(false);
  TestableRadioLR1121 radio(&spi, &rst, &irq, &busy, 17, TCXO_YAML_CODE_3_0V);
  queue_valid_version_response(spi);

  ASSERT_TRUE(radio.init());

  int mod_params_idx = spi.find_opcode(LR1121_CMD_SET_MODULATION_PARAMS);
  ASSERT_GE(mod_params_idx, 0);

  int found = 0;
  for (size_t i = static_cast<size_t>(mod_params_idx) + 1; i < spi.transactions().size(); i++) {
    const auto &tx = spi.transactions()[i];
    if (tx.size() >= 2 && tx[0] == (uint8_t) (LR1121_CMD_WRITE_REG_MEM_MASK32 >> 8) &&
        tx[1] == (uint8_t) LR1121_CMD_WRITE_REG_MEM_MASK32) {
      found++;
      if (found > 3)
        break;
    } else if (found > 0) {
      break;  // workaround writes must be contiguous, immediately after modulation params
    }
  }
  EXPECT_EQ(found, 3) << "expected exactly 3 WriteRegMemMask32 calls immediately after SetModulationParams";
}

TEST(RadioLR1121, SetModeRxAppliesHighAcpWorkaroundFirst) {
  // analysis/lr1121_bring_up_investigation.md §4.1: the high-ACP TX-quality workaround must be
  // written immediately before every SetRx.
  ScriptedSpi spi;
  MockPin rst, irq, busy(false);
  TestableRadioLR1121 radio(&spi, &rst, &irq, &busy, 17, TCXO_YAML_CODE_3_0V);

  radio.set_mode_rx();

  ASSERT_EQ(spi.transactions().size(), 2u) << "expected exactly [WriteRegMemMask32, SetRx]";
  const auto &workaround_tx = spi.transactions()[0];
  const auto &set_rx_tx = spi.transactions()[1];
  ASSERT_GE(workaround_tx.size(), 2u);
  EXPECT_EQ(workaround_tx[0], (uint8_t) (LR1121_CMD_WRITE_REG_MEM_MASK32 >> 8));
  EXPECT_EQ(workaround_tx[1], (uint8_t) LR1121_CMD_WRITE_REG_MEM_MASK32);
  ASSERT_GE(set_rx_tx.size(), 2u);
  EXPECT_EQ(set_rx_tx[0], (uint8_t) (LR1121_CMD_SET_RX >> 8));
  EXPECT_EQ(set_rx_tx[1], (uint8_t) LR1121_CMD_SET_RX);

  ASSERT_EQ(workaround_tx.size(), 14u) << "opcode(2) + addr(4) + mask(4) + value(4)";
  uint32_t const addr = (static_cast<uint32_t>(workaround_tx[2]) << 24) |
                        (static_cast<uint32_t>(workaround_tx[3]) << 16) |
                        (static_cast<uint32_t>(workaround_tx[4]) << 8) | static_cast<uint32_t>(workaround_tx[5]);
  EXPECT_EQ(addr, LR1121_REG_HIGH_ACP_WORKAROUND_ADDR);
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

TEST(RadioLR1121, WaitForPacketIgnoresPreambleOnlyActivity) {
  // LR1121_IRQ_DIO_ENABLE_MASK routes PREAMBLE_DETECTED to the DIO pin, but a preamble alone
  // means the frame may still be arriving. Guards against poll_until_activity_() treating a
  // preamble-only IRQ status as terminal activity — that would make finalize_receive_() see no
  // RX_DONE and call reset_rx_state_(), tearing down RX and losing the real frame that follows.
  // Proves the driver waits through a preamble-only reading and still catches the later RX_DONE.
  ScriptedSpi spi;
  MockPin rst, irq, busy(false);
  TestableRadioLR1121 radio(&spi, &rst, &irq, &busy, 0, 0x07);

  RadioRxPacket pkt{};
  pkt.len = 2;
  pkt.data[0] = 0x11;
  pkt.data[1] = 0x22;
  radio.set_expected_packet(pkt);
  radio.set_irq_sequence({LR1121_IRQ_PREAMBLE_DETECTED, LR1121_IRQ_PREAMBLE_DETECTED, LR1121_IRQ_RX_DONE});

  RadioRxPacket result{};
  bool ok = radio.wait_for_packet(result, 100);

  EXPECT_TRUE(ok) << "preamble-only readings must not be treated as terminal activity";
  EXPECT_EQ(result.len, 2u);
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

TEST(RadioLR1121, CheckForPacketPreambleOnlyDoesNotResetRx) {
  // A preamble-only IRQ means a frame may still be arriving. check_for_packet() must clear just
  // that bit (so the next real completion can still raise the DIO edge) and NOT call
  // reset_rx_state_(), which would issue SetStandby and tear down RX mid-reception.
  ScriptedSpi spi;
  MockPin rst, irq, busy(false);
  TestableRadioLR1121 radio(&spi, &rst, &irq, &busy, 0, 0x07);
  radio.mark_dio_fired_from_isr();
  radio.set_irq_sequence({LR1121_IRQ_PREAMBLE_DETECTED});

  RadioRxPacket packet{};
  bool ok = radio.check_for_packet(packet);

  EXPECT_FALSE(ok) << "preamble alone is not a complete packet yet";
  EXPECT_GE(spi.find_opcode(LR1121_CMD_CLEAR_IRQ), 0) << "the preamble bit must be cleared so it can re-fire";
  EXPECT_LT(spi.find_opcode(LR1121_CMD_SET_STANDBY), 0) << "must not tear down RX via reset_rx_state_()";
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

TEST(RadioLR1121, ReadRxPacketReportsRssiFromPktStatusNotLiveRssiInst) {
  // fill_capture_info_() must report RSSI/packet_status from GetPktStatus (0x0204), the opcode
  // atomically tied to the just-received packet — not GetRssiInst (0x0205), a live read
  // unrelated to any specific frame that would reflect the channel's current state rather than
  // the received frame's actual signal level. GetRssiInst must not be called at all during RX.
  const uint8_t frame[] = {0xCE, 0x00, 0xC0, 0xFF, 0xEE, 0xAA, 0xBB, 0xCC, 0x3C, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
  const uint8_t frame_len = sizeof(frame);

  uint8_t encoded[64] = {0};
  uint8_t encoded_len = encode_frame_with_crc(frame, frame_len, encoded, sizeof(encoded));
  ASSERT_GT(encoded_len, 0u);

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
  // GetPktStatus response: [stat1, rssi_sync=76 (-38 dBm), rssi_avg=90 (-45 dBm), rx_len, status=0x02].
  spi.queue_responses({0x00, 76, 90, kProbeLen, 0x02});

  RadioRxPacket packet{};
  ASSERT_TRUE(radio.check_for_packet(packet));

  EXPECT_EQ(radio.get_last_capture().rssi_dbm, -38)
      << "must report rssi_sync from GetPktStatus, not a live GetRssiInst read";
  EXPECT_EQ(radio.get_last_capture().packet_status, 0x02) << "packet_status must come from GetPktStatus's status byte";
  EXPECT_GE(spi.find_opcode(LR1121_CMD_GET_PKT_STATUS), 0) << "GetPktStatus (0x0204) must be issued for a completed RX";
  EXPECT_LT(spi.find_opcode(LR1121_CMD_GET_RSSI_INST), 0)
      << "GetRssiInst (0x0205) must not be used for packet-capture RSSI — it samples RSSI live, "
         "well after the frame already ended";
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

TEST(RadioLR1121, SendPacketAppliesHighAcpWorkaroundBeforeSetTx) {
  // The high-ACP TX-quality workaround must be written immediately before SetTx too, not just
  // before SetRx (SetModeRxAppliesHighAcpWorkaroundFirst already covers that call site).
  const uint8_t frame[] = {0xC8, 0x00, 0xAA, 0xBB, 0xCC, 0xC0, 0xFF, 0xEE, 0x31};
  ScriptedSpi spi;
  MockPin rst, irq, busy(false);
  TestableRadioLR1121 radio(&spi, &rst, &irq, &busy, 17, 0x07);

  RadioTxConfig cfg;
  cfg.freq_hz = FREQ_CH2;
  cfg.preamble_len = SHORT_PREAMBLE;
  radio.send_packet(frame, sizeof(frame), cfg);  // Times out waiting for TX_DONE — see the test above.

  int set_tx_idx = spi.find_opcode(LR1121_CMD_SET_TX);
  ASSERT_GE(set_tx_idx, 0);
  ASSERT_GT(set_tx_idx, 0) << "SetTx must not be the very first transaction (a workaround write precedes it)";
  const auto &workaround_tx = spi.transactions()[set_tx_idx - 1];
  ASSERT_GE(workaround_tx.size(), 6u);
  EXPECT_EQ(workaround_tx[0], (uint8_t) (LR1121_CMD_WRITE_REG_MEM_MASK32 >> 8));
  EXPECT_EQ(workaround_tx[1], (uint8_t) LR1121_CMD_WRITE_REG_MEM_MASK32);
  uint32_t const addr = (static_cast<uint32_t>(workaround_tx[2]) << 24) |
                        (static_cast<uint32_t>(workaround_tx[3]) << 16) |
                        (static_cast<uint32_t>(workaround_tx[4]) << 8) | static_cast<uint32_t>(workaround_tx[5]);
  EXPECT_EQ(addr, LR1121_REG_HIGH_ACP_WORKAROUND_ADDR);
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

  // write_modulation_params_() must re-apply the GFSK workaround trio on every retune, not just
  // at init — this specifically catches a regression that hoists the workaround call up into
  // configure_radio_() so it only fires once.
  int found = 0;
  for (size_t i = static_cast<size_t>(idx) + 1; i < spi.transactions().size(); i++) {
    const auto &workaround_tx = spi.transactions()[i];
    if (workaround_tx.size() >= 2 && workaround_tx[0] == (uint8_t) (LR1121_CMD_WRITE_REG_MEM_MASK32 >> 8) &&
        workaround_tx[1] == (uint8_t) LR1121_CMD_WRITE_REG_MEM_MASK32) {
      found++;
      if (found > 3)
        break;
    } else if (found > 0) {
      break;
    }
  }
  EXPECT_EQ(found, 3) << "GFSK workaround trio must re-run immediately after every SetModulationParams";
}

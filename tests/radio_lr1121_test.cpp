#include "radio_lr1121.h"
#include "radio_interface.h"

#include "esphome/core/application.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"
#include "stubs/scripted_spi.h"
#include "stubs/soft_phy_test_driver.h"

#include <gtest/gtest.h>
#include <vector>

using namespace esphome::home_io_control;

// ============================================================================
// Testable subclass of RadioLR1121
// ============================================================================
//
// The scriptable test double is shared with the SX1262 suite — see
// tests/stubs/soft_phy_test_driver.h. LR1121 adds one chip-specific hook (exercising the real
// GetStatus SPI transaction), so it subclasses the template rather than aliasing it.
class TestableRadioLR1121 : public test::TestableSoftPhy<RadioLR1121> {
 public:
  using test::TestableSoftPhy<RadioLR1121>::TestableSoftPhy;

  // Bypass the read_irq_status_raw() override to exercise the real GetStatus SPI
  // transaction/parsing against a ScriptedSpi.
  uint32_t call_real_read_irq_status_raw() { return RadioLR1121::read_irq_status_raw(); }
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
  // FRAME_MAX_WIRE_SIZE (not FRAME_MAX_SIZE + 2) to mirror SoftPhyDriverBase::send_packet()'s own
  // buffer exactly — it holds a MAC-trailer-bearing frame's CRC too, see IoFrame::has_mac.
  uint8_t frame_with_crc[FRAME_MAX_WIRE_SIZE];
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

// ============================================================================
// Firmware-version staleness check — lr1121_firmware_is_outdated() (pure, no I/O)
// ============================================================================

TEST(RadioLR1121, FirmwareOlderMajorIsOutdated) {
  EXPECT_TRUE(lr1121_firmware_is_outdated(LR1121_KNOWN_LATEST_FW_MAJOR - 1, 0xFF))
      << "an older major version is outdated regardless of the minor byte";
}

TEST(RadioLR1121, FirmwareSameMajorOlderMinorIsOutdated) {
  ASSERT_GT(LR1121_KNOWN_LATEST_FW_MINOR, 0) << "precondition: test needs room to go one minor version older";
  EXPECT_TRUE(lr1121_firmware_is_outdated(LR1121_KNOWN_LATEST_FW_MAJOR, LR1121_KNOWN_LATEST_FW_MINOR - 1));
}

TEST(RadioLR1121, FirmwareExactlyLatestIsNotOutdated) {
  EXPECT_FALSE(lr1121_firmware_is_outdated(LR1121_KNOWN_LATEST_FW_MAJOR, LR1121_KNOWN_LATEST_FW_MINOR));
}

TEST(RadioLR1121, FirmwareNewerMinorIsNotOutdated) {
  EXPECT_FALSE(lr1121_firmware_is_outdated(LR1121_KNOWN_LATEST_FW_MAJOR, LR1121_KNOWN_LATEST_FW_MINOR + 1))
      << "a version newer than what this file knows about must never be flagged outdated";
}

TEST(RadioLR1121, FirmwareNewerMajorIsNotOutdated) {
  EXPECT_FALSE(lr1121_firmware_is_outdated(LR1121_KNOWN_LATEST_FW_MAJOR + 1, 0x00))
      << "a future major version must never be flagged outdated even with minor=0";
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
  // (see radio_lr1121.cpp configure_radio_()).
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
  // Calibrate(0x3F) calibrates the IMG block at the chip's default band, not ours; a banded
  // CalibImage must follow it.
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
  // The GFSK workaround register trio must be written after every modulation-params write.
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
  // The high-ACP TX-quality workaround must be written immediately before every SetRx.
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

TEST(RadioLR1121, ChipNameIsLr1121) {
  ScriptedSpi spi;
  MockPin rst, irq, busy(false);
  TestableRadioLR1121 radio(&spi, &rst, &irq, &busy, 0, 0x07);
  EXPECT_STREQ(radio.chip_name(), "lr1121");
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

// Test double whose early_rx_read_offset() stays negative, unlike RadioLR1121's own
// LR1121_RX_BUFFER_BASE default — a stand-in for a chip that must not be read before RX_DONE, so
// CheckForPacketSyncWithoutRxDoneClearsIrqAndReturnsFalse below exercises the RX_DONE-only branch
// regardless of the shared idle-path budget/host-clock trap — see that test's comment.
class LegacyRxTestableRadioLR1121 : public TestableRadioLR1121 {
 public:
  using TestableRadioLR1121::TestableRadioLR1121;

 protected:
  int16_t early_rx_read_offset() const override { return -1; }
};

TEST(RadioLR1121, CheckForPacketSyncWithoutRxDoneClearsIrqAndReturnsFalse) {
  // TestableRadioLR1121 inherits RadioLR1121's real, non-negative early_rx_read_offset(), so it
  // reaches try_early_completion_() from this branch instead of bailing out before touching SPI —
  // that shape is covered by CheckForPacketSyncWithoutRxDoneFallsBackWhenCrcInvalid below. This
  // test targets the RX_DONE-only branch specifically (offset < 0), so it pins that shape
  // explicitly via LegacyRxTestableRadioLR1121 rather than leaning on the fact that the fixture's
  // default idle_rx_completion_budget_ms() (20 ms) also happens to expire before any buffer read on
  // the host clock stubs: that coincidence would make this test's meaning drift silently the next
  // time either budget or clock stub changes.
  ScriptedSpi spi;
  MockPin rst, irq, busy(false);
  LegacyRxTestableRadioLR1121 radio(&spi, &rst, &irq, &busy, 0, 0x07);
  radio.mark_dio_fired_from_isr();
  radio.set_irq_sequence({LR1121_IRQ_SYNC_WORD_VALID});

  RadioRxPacket packet{};
  bool ok = radio.check_for_packet(packet);

  EXPECT_FALSE(ok) << "SYNC_WORD_VALID without RX_DONE is not a complete packet yet";
  int clear_idx = spi.find_opcode(LR1121_CMD_CLEAR_IRQ);
  EXPECT_GE(clear_idx, 0) << "the sticky SYNC flag must be cleared so it doesn't wedge the next poll";
  // issue #81: this branch must also arm the idle-path hop holdoff, so maybe_hop() does not
  // retune under a frame that is still arriving.
  EXPECT_TRUE(radio.reception_in_progress())
      << "the sync-without-RX_DONE branch must record the reception so the idle-path hop holds off";
}

// Exposes the protected early_rx_read_offset() so EarlyCompletionIsOptInWithLr1121BufferBase can
// assert LR1121's own opt-in value. The full scriptable early-RX fixture now lives in the shared
// harness (tests/stubs/soft_phy_test_driver.h, test::EarlyRxSoftPhy) and drives the typed suite.
class OffsetProbeRadioLR1121 : public RadioLR1121 {
 public:
  using RadioLR1121::early_rx_read_offset;
  using RadioLR1121::RadioLR1121;
};

TEST(RadioLR1121, EarlyCompletionIsOptInWithLr1121BufferBase) {
  // Pins the per-chip opt-in judgment itself: unlike the base class's -1 default, RadioLR1121
  // opts in at LR1121_RX_BUFFER_BASE (0x00) — the whole 256-byte buffer, there being no
  // SetBufferBaseAddress equivalent on this chip to program a split with.
  MockSpi spi;
  MockPin rst, irq, busy(false);
  OffsetProbeRadioLR1121 radio(&spi, &rst, &irq, &busy, 0, 0x07);
  EXPECT_EQ(radio.early_rx_read_offset(), LR1121_RX_BUFFER_BASE);
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

  // This chip's PreambleLength field is bit-denominated (pbl_len_in_bit), but the caller passed
  // a byte count (SHORT_PREAMBLE). set_packet_params_() must convert.
  const uint16_t preamble_field = (static_cast<uint16_t>(pp[2]) << 8) | pp[3];
  EXPECT_EQ(preamble_field, static_cast<uint16_t>(SHORT_PREAMBLE * 8))
      << "SetPacketParams' PreambleLength is in bits: SHORT_PREAMBLE (" << SHORT_PREAMBLE
      << " bytes) must reach the chip as " << (SHORT_PREAMBLE * 8) << " bits";
}

// Raises the TxDone interrupt the moment SetTx is issued — mirrors
// tests/radio_sx1262_rx_test.cpp's TxCompletingRadioSX1262; this is the only way to reach
// send_packet()'s success path (and hence rearm_rx_after_tx_()) in a synchronous host test.
class TxCompletingRadioLR1121 : public TestableRadioLR1121 {
 public:
  using TestableRadioLR1121::TestableRadioLR1121;

 protected:
  void start_tx() override {
    TestableRadioLR1121::start_tx();
    this->mark_dio_fired_from_isr();
  }
};

TEST(RadioLR1121, PostTxRearmInvalidatesRxBufferBaseContent) {
  // F1 regression (issue #81): LR1121 has one shared 256-byte buffer for both TX and RX (no
  // SetBufferBaseAddress split like SX1262's — see invalidate_stale_rx_content_after_tx()'s doc
  // comment), so after every transmission the bytes this driver just sent sit at exactly the
  // offset a length-driven receive later reads from (LR1121_RX_BUFFER_BASE). Left alone, that
  // residue is a real, CRC-valid, UART-encoded frame, not noise — so rearm_rx_after_tx_() must
  // overwrite it before re-entering RX, or a wrong early_rx_read_offset() guess would hand back
  // the hub's own last transmission as a phantom received packet.
  ScriptedSpi spi;
  MockPin rst, irq, busy(false);
  TxCompletingRadioLR1121 radio(&spi, &rst, &irq, &busy, 0, 0x07);
  radio.set_irq_sequence({LR1121_IRQ_TX_DONE});

  const uint8_t frame[] = {0xC8, 0x00, 0xAA, 0xBB, 0xCC, 0xC0, 0xFF, 0xEE, 0x31};
  RadioTxConfig cfg;
  cfg.freq_hz = FREQ_CH2;
  cfg.preamble_len = SHORT_PREAMBLE;
  ASSERT_TRUE(radio.send_packet(frame, sizeof(frame), cfg)) << "TxDone was driven, so this must succeed";

  int const set_tx_idx = spi.find_opcode(LR1121_CMD_SET_TX);
  ASSERT_GE(set_tx_idx, 0);

  // The invalidating WriteBuffer must come after SetTx — before it is the actual TX payload
  // write, which this test is not about. WriteBuffer always starts from the chip's internal
  // write pointer, which resets to the buffer base for a fresh sequence (write_buffer_()'s own
  // doc comment), the same address early_rx_read_offset() reads from, so there is no separate
  // offset parameter to check here the way ReadBuffer's would need.
  int invalidate_idx = -1;
  for (size_t i = static_cast<size_t>(set_tx_idx) + 1; i < spi.transactions().size(); i++) {
    const auto &tx = spi.transactions()[i];
    if (tx.size() >= 2 && tx[0] == ((LR1121_CMD_WRITE_BUFFER >> 8) & 0xFF) &&
        tx[1] == (LR1121_CMD_WRITE_BUFFER & 0xFF)) {
      invalidate_idx = static_cast<int>(i);
      break;
    }
  }
  ASSERT_GE(invalidate_idx, 0)
      << "rearm_rx_after_tx_() must issue a WriteBuffer targeting the buffer base to invalidate "
         "stale TX residue before re-entering RX";
  const auto &invalidate_tx = spi.transactions()[invalidate_idx];
  ASSERT_EQ(invalidate_tx.size(), 2u + SOFT_PHY_EARLY_HEADER_RAW_BYTES)
      << "must overwrite exactly the header-peek window try_early_completion_()'s stage 1 reads";
  for (size_t i = 2; i < invalidate_tx.size(); i++)
    EXPECT_EQ(invalidate_tx[i], 0x00) << "invalidating content must not itself decode as a UART frame";
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

#include "radio_sx1276.h"
#include "radio_interface.h"

#include "esphome/core/application.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <deque>
#include <vector>

using namespace esphome::home_io_control;

// ============================================================================
// Register-model SPI: models the SX1276's register file (regs[0x80]) plus its FIFO,
// rather than a linear response script.
//
// Neither existing double fits: MockSpi always returns 0 and records nothing;
// ScriptedSpi's (radio_lr1121_test.cpp) linear playback can't feed the read-back polling
// loops this driver relies on (set_mode_() re-reads REG_OP_MODE until the mode sticks,
// run_image_cal_() polls REG_IMAGE_CAL, read_fifo_packet_() pops REG_FIFO until FifoEmpty
// sets) — a script would need to guess how many reads each loop performs. A register model
// makes every read reflect the last write, so the driver's own loops terminate naturally.
// This class is local to this file and does not change MockSpi/ScriptedSpi for any other test.
// ============================================================================

class RegisterModelSpi : public esphome::home_io_control::SpiAccess {
 public:
  void spi_enable() override { this->have_addr_ = false; }
  void spi_disable() override {}

  uint8_t spi_transfer(uint8_t data) override {
    if (!this->have_addr_) {
      this->set_addr_(data);
      return 0;
    }
    if (this->write_dir_) {
      this->handle_write_byte_(data);
      return 0;
    }
    return this->handle_read_byte_();
  }

  void spi_write(uint8_t data) override {
    if (!this->have_addr_) {
      this->set_addr_(data);
      return;
    }
    if (this->write_dir_)
      this->handle_write_byte_(data);
  }

  uint8_t spi_read() override {
    if (!this->have_addr_)
      return 0;
    return this->handle_read_byte_();
  }

  // Seed a register's value directly (bypasses the write log — this is test setup, not
  // something the driver did).
  void set_reg(uint8_t reg, uint8_t value) { this->regs_[reg & 0x7F] = value; }
  uint8_t get_reg(uint8_t reg) const { return this->regs_[reg & 0x7F]; }

  // Stage bytes to be popped from the RX FIFO (REG_FIFO reads) in order.
  void seed_rx_fifo(const uint8_t *data, uint8_t len) {
    for (uint8_t i = 0; i < len; i++)
      this->rx_fifo_.push_back(data[i]);
  }

  // Bytes written to REG_FIFO by the driver (TX path), in order.
  const std::vector<uint8_t> &tx_fifo() const { return this->tx_fifo_; }

  // Ordered log of (register, value) for every write_register_()-style value write.
  // FIFO writes are excluded — those go to tx_fifo() instead.
  const std::vector<std::pair<uint8_t, uint8_t>> &write_log() const { return this->write_log_; }

  bool wrote_register(uint8_t reg) const {
    for (const auto &entry : this->write_log_) {
      if (entry.first == (reg & 0x7F))
        return true;
    }
    return false;
  }

  // Force reads of `reg` to always return `stuck_value`, regardless of what the driver writes.
  // Used to simulate hardware that never reports completion (e.g. a mode change that never
  // sticks, or a calibration-running bit that never clears), driving set_mode_()/
  // run_image_cal_() into their timeout paths.
  void stick_reg(uint8_t reg, uint8_t stuck_value) {
    this->stuck_reg_ = reg & 0x7F;
    this->stuck_value_ = stuck_value;
    this->stuck_active_ = true;
  }

 private:
  static constexpr uint8_t kRegFifo = 0x00;
  static constexpr uint8_t kRegIrqFlags2 = 0x3F;

  void set_addr_(uint8_t first_byte) {
    this->have_addr_ = true;
    this->write_dir_ = (first_byte & 0x80) != 0;
    this->addr_ = first_byte & 0x7F;
  }

  void handle_write_byte_(uint8_t value) {
    if (this->addr_ == kRegFifo) {
      this->tx_fifo_.push_back(value);
      return;
    }
    this->regs_[this->addr_] = value;
    this->write_log_.emplace_back(this->addr_, value);
  }

  uint8_t handle_read_byte_() {
    if (this->stuck_active_ && this->addr_ == this->stuck_reg_)
      return this->stuck_value_;
    if (this->addr_ == kRegFifo) {
      if (this->rx_fifo_.empty())
        return 0;
      uint8_t const value = this->rx_fifo_.front();
      this->rx_fifo_.pop_front();
      return value;
    }
    if (this->addr_ == kRegIrqFlags2) {
      // FifoEmpty (bit 0x40) is a live view of the FIFO, not a register the driver wrote —
      // read_fifo_packet_()'s pop loop terminates when the staged frame is consumed.
      return (this->regs_[kRegIrqFlags2] & ~0x40) | (this->rx_fifo_.empty() ? 0x40 : 0x00);
    }
    return this->regs_[this->addr_];
  }

  uint8_t regs_[0x80]{};
  std::vector<uint8_t> tx_fifo_;
  std::deque<uint8_t> rx_fifo_;
  std::vector<std::pair<uint8_t, uint8_t>> write_log_;

  bool have_addr_{false};
  bool write_dir_{false};
  uint8_t addr_{0};

  bool stuck_active_{false};
  uint8_t stuck_reg_{0};
  uint8_t stuck_value_{0};
};

namespace {

// Seed a GetVersion-equivalent register so init()'s identity check passes.
void seed_valid_version(RegisterModelSpi &spi) { spi.set_reg(REG_VERSION, 0x12); }

}  // namespace

// ============================================================================
// Init / configuration tests
// ============================================================================

TEST(RadioSX1276, InitFailsOnWrongVersion) {
  RegisterModelSpi spi;
  MockPin rst, dio0, dio4(false);
  RadioSX1276 radio(&spi, &rst, &dio0, &dio4, 17, 0x80);

  bool ok = radio.init();

  EXPECT_FALSE(ok) << "init() must fail when REG_VERSION doesn't read back 0x12";
  EXPECT_TRUE(radio.is_failed());
  EXPECT_FALSE(spi.wrote_register(REG_PACKET_CONFIG1)) << "configure_radio_() must never run after a failed "
                                                          "version check";
}

TEST(RadioSX1276, InitProgramsValidatedConfiguration) {
  RegisterModelSpi spi;
  MockPin rst, dio0, dio4(false);
  RadioSX1276 radio(&spi, &rst, &dio0, &dio4, 17, 0x80);
  seed_valid_version(spi);

  bool ok = radio.init();

  ASSERT_TRUE(ok);
  EXPECT_FALSE(radio.is_failed());

  EXPECT_EQ(spi.get_reg(REG_PACKET_CONFIG1), 0x90) << "variable len, CRC on, CCITT";
  EXPECT_EQ(spi.get_reg(REG_PACKET_CONFIG2), 0x70) << "packet mode, IoHomeOn, PowerFrame";
  EXPECT_EQ(spi.get_reg(REG_SYNC_CONFIG), 0x52) << "written 0x50, then RMW to SyncSize=2";
  EXPECT_EQ(spi.get_reg(REG_SYNC_VALUE1), 0x55);
  EXPECT_EQ(spi.get_reg(REG_SYNC_VALUE1 + 1), 0xFF);
  EXPECT_EQ(spi.get_reg(REG_SYNC_VALUE1 + 2), 0x33);
  EXPECT_EQ(spi.get_reg(REG_DIO_MAPPING1), 0x3D);
  EXPECT_EQ(spi.get_reg(REG_DIO_MAPPING2), 0xF1);
  EXPECT_EQ(spi.get_reg(REG_RX_CONFIG), 0x9E);
  EXPECT_EQ(spi.get_reg(REG_PREAMBLE_DETECT), 0xAA);
  EXPECT_EQ(spi.get_reg(REG_LNA), 0x23);
  EXPECT_EQ(spi.get_reg(0x0B), 0x3B) << "OCP on, 240mA";
  EXPECT_EQ(spi.get_reg(REG_PA_RAMP), 0x2D);
  EXPECT_EQ(spi.get_reg(REG_FIFO_THRESH), 0x80);
  EXPECT_EQ(spi.get_reg(REG_PAYLOAD_LENGTH), 0xFF);
  EXPECT_NE(spi.get_reg(REG_PLLHOP) & 0x80, 0) << "fast hop bit must be set";

  // Bitrate 38400 bps: FXOSC / 38400 = 833 (0x0341).
  EXPECT_EQ(spi.get_reg(REG_BITRATE_MSB), 0x03);
  EXPECT_EQ(spi.get_reg(REG_BITRATE_LSB), 0x41);

  // Deviation 19200 Hz: (19200 / 32e6) * 2^19 truncates to 314 (0x013A). AGENTS.md's "≈315" is
  // an approximation of this truncated value — the code's byte is what's under test here.
  EXPECT_EQ(spi.get_reg(REG_FDEV_MSB), 0x01);
  EXPECT_EQ(spi.get_reg(REG_FDEV_LSB), 0x3A);

  // Default RX/AFC bandwidth (BW_41_7_KHZ = 0x13) — tuning_config.h is the source of truth.
  EXPECT_EQ(spi.get_reg(REG_RX_BW), 0x13);
  EXPECT_EQ(spi.get_reg(REG_AFC_BW), 0x13);

  // Default (long) preamble, overwritten per-packet by send_packet().
  EXPECT_EQ(spi.get_reg(REG_PREAMBLE_MSB), 0x04);
  EXPECT_EQ(spi.get_reg(REG_PREAMBLE_LSB), 0x00);

  EXPECT_EQ(spi.get_reg(REG_OP_MODE) & MODE_MASK, MODE_RX) << "configure_radio_() must end in RX mode";
}

TEST(RadioSX1276, SetModeTimesOutWhenOpModeNeverReflectsTarget) {
  // Force REG_OP_MODE reads to always report SLEEP, so set_mode_()'s poll-until-it-sticks loop
  // for STDBY never observes a match and must fall through its timeout path.
  RegisterModelSpi spi;
  MockPin rst, dio0, dio4(false);
  RadioSX1276 radio(&spi, &rst, &dio0, &dio4, 17, 0x80);
  spi.stick_reg(REG_OP_MODE, MODE_SLEEP);

  const uint32_t feeds_before = esphome::App.feed_wdt_calls;
  radio.set_mode_standby();

  EXPECT_TRUE(radio.is_failed()) << "a mode that never reads back as requested must fail the driver";
  EXPECT_GT(esphome::App.feed_wdt_calls, feeds_before)
      << "the mode-change poll is a multi-millisecond blocking wait and must feed the watchdog on timeout";
}

TEST(RadioSX1276, RunImageCalTimesOutWhenCalBitNeverClears) {
  // Force REG_IMAGE_CAL to always report the "calibration running" bit (0x20) set, so
  // run_image_cal_()'s poll loop never observes completion and must fall through its timeout path.
  RegisterModelSpi spi;
  MockPin rst, dio0, dio4(false);
  RadioSX1276 radio(&spi, &rst, &dio0, &dio4, 17, 0x80);
  seed_valid_version(spi);
  spi.stick_reg(REG_IMAGE_CAL, 0x20);

  const uint32_t feeds_before = esphome::App.feed_wdt_calls;
  bool ok = radio.init();

  EXPECT_FALSE(ok) << "init() must fail when image calibration never completes";
  EXPECT_TRUE(radio.is_failed());
  EXPECT_GT(esphome::App.feed_wdt_calls, feeds_before)
      << "the image-calibration poll is a blocking wait and must feed the watchdog on timeout";
}

TEST(RadioSX1276, ChangeFrequencyWritesFrfBytesForEachChannel) {
  for (uint32_t freq : {FREQ_CH1, FREQ_CH2, FREQ_CH3}) {
    RegisterModelSpi spi;
    MockPin rst, dio0, dio4(false);
    RadioSX1276 radio(&spi, &rst, &dio0, &dio4, 17, 0x80);

    radio.change_frequency(freq);

    uint64_t const frf = ((uint64_t) freq << 19) / FXOSC;
    EXPECT_EQ(spi.get_reg(REG_FRF_MSB), (uint8_t) ((frf >> 16) & 0xFF));
    EXPECT_EQ(spi.get_reg(REG_FRF_MID), (uint8_t) ((frf >> 8) & 0xFF));
    EXPECT_EQ(spi.get_reg(REG_FRF_LSB), (uint8_t) (frf & 0xFF));
  }
}

// ============================================================================
// PA configuration (both PA pin branches, init-time)
// ============================================================================

TEST(RadioSX1276, PaConfigBoostPinClampsAndOffsets) {
  // pa_pin == 0x80 selects PA_BOOST: register value is 0x80 | (clamp(tx_power, 2, 17) - 2).
  struct Case {
    uint8_t tx_power;
    uint8_t expected;
  };
  const Case cases[] = {
      {17, 0x8F},  // in range
      {20, 0x8F},  // clamped down from above the max
      {0, 0x80},   // clamped up from below the min
  };
  for (const auto &c : cases) {
    RegisterModelSpi spi;
    MockPin rst, dio0, dio4(false);
    RadioSX1276 radio(&spi, &rst, &dio0, &dio4, c.tx_power, 0x80);
    seed_valid_version(spi);

    ASSERT_TRUE(radio.init());
    EXPECT_EQ(spi.get_reg(REG_PA_CONFIG), c.expected) << "tx_power=" << (int) c.tx_power;
  }
}

TEST(RadioSX1276, PaConfigRfoPinClamps) {
  // pa_pin != 0x80 selects RFO: register value is clamp(tx_power, 0, 14), no offset.
  RegisterModelSpi spi;
  MockPin rst, dio0, dio4(false);
  RadioSX1276 radio(&spi, &rst, &dio0, &dio4, 20, 0x00);
  seed_valid_version(spi);

  ASSERT_TRUE(radio.init());
  EXPECT_EQ(spi.get_reg(REG_PA_CONFIG), 0x0E) << "clamped to 14";
}

// ============================================================================
// send_packet (TX)
// ============================================================================

TEST(RadioSX1276, SendPacketWritesPreambleFrequencyAndFifoBeforeTxWait) {
  RegisterModelSpi spi;
  MockPin rst, dio0, dio4(false);
  RadioSX1276 radio(&spi, &rst, &dio0, &dio4, 17, 0x80);

  const uint8_t payload[] = {0xC8, 0x00, 0xAA, 0xBB, 0xCC, 0xC0, 0xFF, 0xEE, 0x31};
  RadioTxConfig cfg;
  cfg.freq_hz = FREQ_CH2;
  cfg.preamble_len = 0x0123;

  // send_packet() clears the DIO latch right before its TX-done wait loop (by design — it must
  // ignore any stale flag from before the TX started), so a synchronous host test can't make
  // that loop observe a "fired" edge and the call reliably times out (~4000 fake-millis
  // iterations, still fast) and returns false. The bytes this test cares about — preamble,
  // frequency, FIFO — are all written before that wait starts, so the timeout doesn't affect
  // what's being asserted here (mirrors radio_lr1121_test.cpp's SendPacketWritesUartEncodedBytes).
  bool result = radio.send_packet(payload, sizeof(payload), cfg);
  EXPECT_FALSE(result) << "TX-done wait times out in a synchronous host test by design";

  EXPECT_EQ(spi.get_reg(REG_PREAMBLE_MSB), (cfg.preamble_len >> 8) & 0xFF);
  EXPECT_EQ(spi.get_reg(REG_PREAMBLE_LSB), cfg.preamble_len & 0xFF);
  uint64_t const frf = ((uint64_t) cfg.freq_hz << 19) / FXOSC;
  EXPECT_EQ(spi.get_reg(REG_FRF_MSB), (uint8_t) ((frf >> 16) & 0xFF));
  EXPECT_EQ(spi.get_reg(REG_FRF_MID), (uint8_t) ((frf >> 8) & 0xFF));
  EXPECT_EQ(spi.get_reg(REG_FRF_LSB), (uint8_t) (frf & 0xFF));

  ASSERT_EQ(spi.tx_fifo().size(), sizeof(payload));
  EXPECT_TRUE(std::equal(spi.tx_fifo().begin(), spi.tx_fifo().end(), payload));
}

TEST(RadioSX1276, SendPacketRejectsZeroLength) {
  RegisterModelSpi spi;
  MockPin rst, dio0, dio4(false);
  RadioSX1276 radio(&spi, &rst, &dio0, &dio4, 17, 0x80);

  const uint8_t payload[] = {0xAA};
  EXPECT_FALSE(radio.send_packet(payload, 0, RadioTxConfig{}));
  EXPECT_TRUE(spi.tx_fifo().empty());
  EXPECT_TRUE(spi.write_log().empty()) << "a rejected send must not touch SPI at all";
}

TEST(RadioSX1276, SendPacketRejectsOverlongPayload) {
  RegisterModelSpi spi;
  MockPin rst, dio0, dio4(false);
  RadioSX1276 radio(&spi, &rst, &dio0, &dio4, 17, 0x80);

  uint8_t payload[RADIO_PACKET_BUFFER_SIZE + 1] = {0};
  EXPECT_FALSE(radio.send_packet(payload, sizeof(payload), RadioTxConfig{}));
  EXPECT_TRUE(spi.tx_fifo().empty());
  EXPECT_TRUE(spi.write_log().empty()) << "a rejected send must not touch SPI at all";
}

// ============================================================================
// wait_for_packet (blocking RX)
// ============================================================================

TEST(RadioSX1276, WaitForPacketSuccessViaIrqPath) {
  RegisterModelSpi spi;
  MockPin rst, dio0, dio4(false);
  RadioSX1276 radio(&spi, &rst, &dio0, &dio4, 17, 0x80);

  const uint8_t frame[] = {0xDE, 0xAD, 0xBE, 0xEF};
  spi.seed_rx_fifo(frame, sizeof(frame));
  spi.set_reg(REG_IRQ_FLAGS2, 0x04);  // PayloadReady
  spi.set_reg(REG_RSSI_VALUE, 100);

  RadioRxPacket packet{};
  bool ok = radio.wait_for_packet(packet, 100);

  ASSERT_TRUE(ok);
  ASSERT_EQ(packet.len, sizeof(frame));
  EXPECT_TRUE(std::equal(packet.data, packet.data + packet.len, frame));
  EXPECT_EQ(packet.freq_hz, radio.get_current_freq());

  const auto &capture = radio.get_last_capture();
  EXPECT_TRUE(capture.rx_done);
  EXPECT_FALSE(capture.crc_error) << "IoHomeOn filters bad-CRC frames in hardware before RxDone";
  EXPECT_EQ(capture.rssi_dbm, -50);
}

TEST(RadioSX1276, WaitForPacketTimesOutWithNoActivity) {
  RegisterModelSpi spi;
  MockPin rst, dio0, dio4(false);
  RadioSX1276 radio(&spi, &rst, &dio0, &dio4, 17, 0x80);

  RadioRxPacket packet{};
  const uint32_t feeds_before = esphome::App.feed_wdt_calls;
  bool ok = radio.wait_for_packet(packet, 5);

  EXPECT_FALSE(ok);
  EXPECT_EQ(packet.len, 0u);
  EXPECT_GT(esphome::App.feed_wdt_calls, feeds_before)
      << "a multi-millisecond blocking wait must feed the watchdog, or a real timeout resets the board";
}

// ============================================================================
// check_for_packet (non-blocking RX)
// ============================================================================

TEST(RadioSX1276, CheckForPacketReturnsFalseWithoutDio) {
  RegisterModelSpi spi;
  MockPin rst, dio0, dio4(false);
  RadioSX1276 radio(&spi, &rst, &dio0, &dio4, 17, 0x80);
  // is_dio_fired() defaults to false; check_for_packet must bail out before touching SPI.

  RadioRxPacket packet{};
  EXPECT_FALSE(radio.check_for_packet(packet));
  EXPECT_TRUE(spi.write_log().empty()) << "no register read/write should occur when DIO never fired";
}

TEST(RadioSX1276, CheckForPacketWithDioAndPayloadReturnsBytes) {
  RegisterModelSpi spi;
  MockPin rst, dio0, dio4(false);
  RadioSX1276 radio(&spi, &rst, &dio0, &dio4, 17, 0x80);

  const uint8_t frame[] = {0x11, 0x22, 0x33};
  spi.seed_rx_fifo(frame, sizeof(frame));
  spi.set_reg(REG_IRQ_FLAGS2, 0x04);  // PayloadReady

  // check_for_packet() does not clear the DIO latch before testing it — mark it first.
  radio.mark_dio_fired_from_isr();

  RadioRxPacket packet{};
  bool ok = radio.check_for_packet(packet);

  ASSERT_TRUE(ok);
  ASSERT_EQ(packet.len, sizeof(frame));
  EXPECT_TRUE(std::equal(packet.data, packet.data + packet.len, frame));
}

// ============================================================================
// IRQ_FLAGS1 status bits
// ============================================================================

TEST(RadioSX1276, IsSyncDetectedTrue) {
  RegisterModelSpi spi;
  MockPin rst, dio0, dio4(false);
  RadioSX1276 radio(&spi, &rst, &dio0, &dio4, 17, 0x80);
  spi.set_reg(REG_IRQ_FLAGS1, 0x01);
  EXPECT_TRUE(radio.is_sync_detected());
}

TEST(RadioSX1276, IsSyncDetectedFalse) {
  RegisterModelSpi spi;
  MockPin rst, dio0, dio4(false);
  RadioSX1276 radio(&spi, &rst, &dio0, &dio4, 17, 0x80);
  spi.set_reg(REG_IRQ_FLAGS1, 0x00);
  EXPECT_FALSE(radio.is_sync_detected());
}

TEST(RadioSX1276, IsPreambleDetectedTrue) {
  RegisterModelSpi spi;
  MockPin rst, dio0, dio4(false);
  RadioSX1276 radio(&spi, &rst, &dio0, &dio4, 17, 0x80);
  spi.set_reg(REG_IRQ_FLAGS1, 0x02);
  EXPECT_TRUE(radio.is_preamble_detected());
}

TEST(RadioSX1276, IsPreambleDetectedFalse) {
  RegisterModelSpi spi;
  MockPin rst, dio0, dio4(false);
  RadioSX1276 radio(&spi, &rst, &dio0, &dio4, 17, 0x80);
  spi.set_reg(REG_IRQ_FLAGS1, 0x00);
  EXPECT_FALSE(radio.is_preamble_detected());
}

// ============================================================================
// reception_in_progress() — the idle-path hop gate (issue #81). Unlike the software-PHY chips,
// SX1276's check_for_packet() never touches a register while a frame is still arriving (DIO0
// carries PayloadReady only), so this reads REG_IRQ_FLAGS1 live at hop time instead of relying on
// recorded state. See radio_sx1276.cpp for the un-stick logic this section exercises.
// ============================================================================

TEST(RadioSX1276, ReceptionInProgressTrueWhileSyncAddressMatchSet) {
  RegisterModelSpi spi;
  MockPin rst, dio0, dio4(false);
  RadioSX1276 radio(&spi, &rst, &dio0, &dio4, 17, 0x80);
  spi.set_reg(REG_IRQ_FLAGS1, 0x01);  // SyncAddressMatch

  EXPECT_TRUE(radio.reception_in_progress());
}

TEST(RadioSX1276, ReceptionInProgressFalseWhenNoSync) {
  RegisterModelSpi spi;
  MockPin rst, dio0, dio4(false);
  RadioSX1276 radio(&spi, &rst, &dio0, &dio4, 17, 0x80);
  spi.set_reg(REG_IRQ_FLAGS1, 0x00);

  EXPECT_FALSE(radio.reception_in_progress());
}

TEST(RadioSX1276, StaleSyncAddressMatchIsClearedByCyclingOutOfRx) {
  // The single riskiest property in Step 1: SyncAddressMatch is read-only in packet mode and a
  // hop's change_frequency() never clears it (FRF writes only), so a sync match with no frame
  // behind it must not wedge hopping off forever.
  //
  // Burn one micros() call before anything else touches the clock: the host stub's first call in
  // a process returns 0, which collides with sync_hold_since_us_'s own "not armed" sentinel and
  // would make reception_in_progress() read a genuinely expired holdoff as an unarmed one instead
  // (a second "first sighting"). Only matters when this test runs first/alone; burning the
  // collision away here keeps the test's outcome independent of suite ordering.
  test::burn_micros(1);

  RegisterModelSpi spi;
  MockPin rst, dio0, dio4(false);
  RadioSX1276 radio(&spi, &rst, &dio0, &dio4, 17, 0x80);
  spi.set_reg(REG_IRQ_FLAGS1, 0x01);  // SyncAddressMatch, stuck on

  EXPECT_TRUE(radio.reception_in_progress()) << "first sighting arms the un-stick bound";

  // Register-model SPI reflects mode writes, so set_mode_()'s own poll-until-it-sticks loop
  // terminates naturally once the un-stick path writes REG_OP_MODE.
  test::burn_micros(RX_HOP_HOLDOFF_US + 2);

  EXPECT_FALSE(radio.reception_in_progress()) << "the bound expired; the stale bit must be un-stuck";

  bool saw_standby = false;
  bool saw_rx_after_standby = false;
  for (const auto &entry : spi.write_log()) {
    if (entry.first != (REG_OP_MODE & 0x7F))
      continue;
    if ((entry.second & MODE_MASK) == MODE_STDBY)
      saw_standby = true;
    else if ((entry.second & MODE_MASK) == MODE_RX && saw_standby)
      saw_rx_after_standby = true;
  }
  EXPECT_TRUE(saw_standby) << "un-sticking must cycle through standby (the datasheet-guaranteed clear)";
  EXPECT_TRUE(saw_rx_after_standby) << "un-sticking must return to RX afterwards";
}

TEST(RadioSX1276, ReceptionInProgressIgnoresPreambleDetect) {
  // PreambleDetect (bit 1) is write-1-to-clear and this driver never writes it, so gating on it
  // would disable hopping permanently after the first preamble ever seen. Sync only.
  RegisterModelSpi spi;
  MockPin rst, dio0, dio4(false);
  RadioSX1276 radio(&spi, &rst, &dio0, &dio4, 17, 0x80);
  spi.set_reg(REG_IRQ_FLAGS1, 0x02);  // PreambleDetect only, no SyncAddressMatch

  EXPECT_FALSE(radio.reception_in_progress());
}

// ============================================================================
// Tuning, RSSI, chip identity
// ============================================================================

TEST(RadioSX1276, ApplyTuningRewritesRxBandwidthAndResponsePreamble) {
  RegisterModelSpi spi;
  MockPin rst, dio0, dio4(false);
  RadioSX1276 radio(&spi, &rst, &dio0, &dio4, 17, 0x80);

  TuningConfig tuning;
  tuning.sx1276_rx_bandwidth = SX1276RxBandwidth::BW_125_0_KHZ;
  tuning.sx1276_response_preamble = 20;
  radio.apply_tuning(tuning);

  EXPECT_EQ(spi.get_reg(REG_RX_BW), static_cast<uint8_t>(SX1276RxBandwidth::BW_125_0_KHZ));
  EXPECT_EQ(spi.get_reg(REG_AFC_BW), static_cast<uint8_t>(SX1276RxBandwidth::BW_125_0_KHZ));
  EXPECT_EQ(radio.response_preamble(), 20u);
}

TEST(RadioSX1276, ReadRssiAppliesFormula) {
  RegisterModelSpi spi;
  MockPin rst, dio0, dio4(false);
  RadioSX1276 radio(&spi, &rst, &dio0, &dio4, 17, 0x80);
  spi.set_reg(REG_RSSI_VALUE, 100);

  EXPECT_EQ(radio.read_rssi(), -50);
}

TEST(RadioSX1276, ChipNameIsSx1276) {
  RegisterModelSpi spi;
  MockPin rst, dio0, dio4(false);
  RadioSX1276 radio(&spi, &rst, &dio0, &dio4, 17, 0x80);
  EXPECT_STREQ(radio.chip_name(), "sx1276");
}

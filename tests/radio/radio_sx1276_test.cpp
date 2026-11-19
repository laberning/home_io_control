#include "radio_sx1276.h"
#include "radio_interface.h"

#include "esphome/core/application.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"

#include <functional>
#include <gtest/gtest.h>
#include <algorithm>
#include <deque>
#include <string>
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

  // Run `hook` on the `nth` (1-based) read of `reg` and answer that read with 0, then behave
  // normally. Models a task that stalls between the driver's register sample and its clock sample
  // (the hook advances the clock; the 0 is the stale sample the stalled task then acts on).
  void on_nth_read(uint8_t reg, uint32_t nth, std::function<void()> hook) {
    this->hook_reg_ = reg & 0x7F;
    this->hook_nth_ = nth;
    this->hook_reads_ = 0;
    this->hook_ = std::move(hook);
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
    if (this->hook_ && this->addr_ == this->hook_reg_ && ++this->hook_reads_ == this->hook_nth_) {
      auto hook = std::move(this->hook_);
      this->hook_ = nullptr;
      hook();
      return 0;
    }
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

  std::function<void()> hook_;
  uint8_t hook_reg_{0};
  uint32_t hook_nth_{0};
  uint32_t hook_reads_{0};

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
  ASSERT_NE(radio.failure_reason(), nullptr) << "the hub prints this from dump_config, so it must be recorded";
  EXPECT_NE(std::string(radio.failure_reason()).find("not found"), std::string::npos);
  EXPECT_FALSE(spi.wrote_register(REG_PACKET_CONFIG1)) << "configure_radio_() must never run after a failed "
                                                          "version check";
}

namespace {
/// Estimated air time of a `frame_len`-byte frame (15 by default) behind `preamble_len` programmed preamble bytes.
uint32_t sx1276_air_time_us(uint16_t preamble_len, uint8_t frame_len = 15) {
  RegisterModelSpi spi;
  MockPin rst, dio0, dio4(false);
  RadioSX1276 radio(&spi, &rst, &dio0, &dio4, 17, 0x80);
  RadioTxConfig cfg{};
  cfg.preamble_len = preamble_len;
  return radio.tx_air_time_us(frame_len, cfg);
}
}  // namespace

/// The SX1276 sends every preamble byte as a 10-bit cell, so its estimate is the shared 868 model
/// with 10 bits per preamble byte, not the software PHY's 8.
TEST(RadioSX1276, TxAirTimeCountsTenBitsPerPreambleByte) {
  EXPECT_EQ(sx1276_air_time_us(LONG_PREAMBLE), io868_tx_air_time_us(LONG_PREAMBLE, 15, SX1276_PREAMBLE_BITS_PER_BYTE));
  EXPECT_GT(sx1276_air_time_us(LONG_PREAMBLE),
            io868_tx_air_time_us(LONG_PREAMBLE, 15, SOFT_PHY_PREAMBLE_BITS_PER_BYTE));
}

/// Measured bursts: 32 and 80 preamble bytes occupied 332 and 812 preamble bits, so 48 more bytes
/// add 480 bit periods; 2048 bytes plus the 15-byte frame took 539.0 ms in total.
TEST(RadioSX1276, TxAirTimeMatchesMeasuredBursts) {
  const double us_per_bit = 1e6 / 38400.0;
  EXPECT_NEAR(sx1276_air_time_us(80) - sx1276_air_time_us(32), 480 * us_per_bit, 2.0);
  EXPECT_NEAR(sx1276_air_time_us(2048), 539000.0, 539000.0 * 0.02);
}

/// Whole measured bursts, short and long: a 12-byte frame behind 32 preamble bytes took 13.32 ms, the
/// 15-byte frame behind 2048 bytes 539.04 ms. The model leaves out a few sync-word alternation bits, so
/// it runs a fraction of a millisecond short at every length; the per-byte count is what keeps the
/// long burst inside that same millisecond (at 8 bits per byte it would be 107 ms short).
TEST(RadioSX1276, TxAirTimeIsWithinAMillisecondOfEveryMeasuredBurst) {
  EXPECT_NEAR(sx1276_air_time_us(32, 12), 13321.0, 1000.0);
  EXPECT_NEAR(sx1276_air_time_us(2048), 539042.0, 1000.0);
}

/// The soft-PHY chips send 8 bits per preamble byte: 2048 bytes plus a 15-byte frame took 432.4 ms.
TEST(SoftPhy, Io868AirTimeAtEightBitsPerPreambleByteMatchesMeasuredBurst) {
  EXPECT_NEAR(io868_tx_air_time_us(2048, 15, SOFT_PHY_PREAMBLE_BITS_PER_BYTE), 432400.0, 432400.0 * 0.02);
}

/// The TX-done wait is the estimated air time plus the whole guard, at the longest preamble, and the
/// estimate covers every one of the 10-bit preamble cells the chip sends, so the guard stays slack.
TEST(RadioSX1276, TxDoneTimeoutAtLongestPreambleExceedsAirTimeByTheGuard) {
  const uint32_t air_us = sx1276_air_time_us(4096);
  const uint32_t air_ms_rounded_up = (air_us + 999) / 1000;
  EXPECT_EQ(tx_done_timeout_ms(air_us), air_ms_rounded_up + TX_DONE_GUARD_MS);
  EXPECT_GE(tx_done_timeout_ms(air_us) * 1000u, air_us + TX_DONE_GUARD_MS * 1000u);
  const double preamble_on_air_us = 4096 * 10 * (1e6 / 38400.0);
  EXPECT_GE(air_us, preamble_on_air_us) << "the estimate must not stop short of the preamble the chip sends";
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

  // Deviation 19200 Hz: (19200 / 32e6) * 2^19 truncates to 314 (0x013A). A commonly-quoted "≈315"
  // is an approximation of this truncated value — the code's byte is what's under test here.
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
  ASSERT_NE(radio.failure_reason(), nullptr);
  EXPECT_NE(std::string(radio.failure_reason()).find("operating mode"), std::string::npos);
  EXPECT_GT(esphome::App.feed_wdt_calls, feeds_before)
      << "the mode-change poll is a multi-millisecond blocking wait and must feed the watchdog on timeout";
}

TEST(RadioSX1276, FirstFailureReasonSurvivesALaterFailure) {
  // set_mode_standby() times out first. init() then fails a second time on the version check; the
  // reason must keep naming the root cause, not the later symptom.
  RegisterModelSpi spi;
  MockPin rst, dio0, dio4(false);
  RadioSX1276 radio(&spi, &rst, &dio0, &dio4, 17, 0x80);
  spi.stick_reg(REG_OP_MODE, MODE_SLEEP);
  radio.set_mode_standby();
  ASSERT_TRUE(radio.is_failed());
  const char *const root_cause = radio.failure_reason();
  ASSERT_NE(root_cause, nullptr);

  EXPECT_FALSE(radio.init());  // version register reads back wrong: a second, different failure

  EXPECT_EQ(radio.failure_reason(), root_cause);
  EXPECT_NE(std::string(radio.failure_reason()).find("operating mode"), std::string::npos);
}

TEST(RadioSX1276, SetModeSurvivesPreemptionBeforeTheModeReadsBack) {
  // ManualClock is safe here: the hook moves the clock once, and the register model reflects the
  // mode write on every later read, so the poll cannot spin without time advancing.
  esphome::test_clock::ManualClock clock;
  RegisterModelSpi spi;
  MockPin rst, dio0, dio4(false);
  RadioSX1276 radio(&spi, &rst, &dio0, &dio4, 17, 0x80);
  // Read 1 is set_mode_()'s read-modify-write; read 2 is the first poll sample, which reports a
  // stale mode while the task stalls far past the 50 ms timeout.
  spi.on_nth_read(REG_OP_MODE, 2, [&] { esphome::test_clock::advance_ms(500); });
  radio.set_mode_standby();

  EXPECT_FALSE(radio.is_failed()) << "a chip that reached the mode during the stall must not be failed";
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
  ASSERT_NE(radio.failure_reason(), nullptr);
  EXPECT_NE(std::string(radio.failure_reason()).find("calibration"), std::string::npos);
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
  // that loop observe a "fired" edge and the call reliably times out (after the frame's air time
  // plus TX_DONE_GUARD_MS of fake millis, still fast) and returns false. The bytes this test cares about — preamble,
  // frequency, FIFO — are all written before that wait starts, so the timeout doesn't affect
  // what's being asserted here (mirrors radio_lr1121_test.cpp's SendPacketWritesUartEncodedBytes).
  TxResult result = radio.send_packet(payload, sizeof(payload), cfg);
  EXPECT_FALSE(result.sent()) << "TX-done wait times out in a synchronous host test by design";

  EXPECT_EQ(spi.get_reg(REG_PREAMBLE_MSB), (cfg.preamble_len >> 8) & 0xFF);
  EXPECT_EQ(spi.get_reg(REG_PREAMBLE_LSB), cfg.preamble_len & 0xFF);
  uint64_t const frf = ((uint64_t) cfg.freq_hz << 19) / FXOSC;
  EXPECT_EQ(spi.get_reg(REG_FRF_MSB), (uint8_t) ((frf >> 16) & 0xFF));
  EXPECT_EQ(spi.get_reg(REG_FRF_MID), (uint8_t) ((frf >> 8) & 0xFF));
  EXPECT_EQ(spi.get_reg(REG_FRF_LSB), (uint8_t) (frf & 0xFF));

  ASSERT_EQ(spi.tx_fifo().size(), sizeof(payload));
  EXPECT_TRUE(std::equal(spi.tx_fifo().begin(), spi.tx_fifo().end(), payload));
}

TEST(RadioSX1276, TxDoneWaitGivesUpOneGuardAfterTheFrameShouldHaveEnded) {
  // ManualClock is safe: the wait loop's delayMicroseconds() step advances it on every pass, and the
  // register model reflects every mode write, so no poll can spin without time advancing.
  esphome::test_clock::ManualClock clock;
  for (uint16_t preamble : {SHORT_PREAMBLE, LONG_PREAMBLE}) {
    RegisterModelSpi spi;
    MockPin rst, dio0, dio4(false);
    RadioSX1276 radio(&spi, &rst, &dio0, &dio4, 17, 0x80);
    const uint8_t payload[] = {0xC8, 0x00, 0xAA, 0xBB, 0xCC, 0xC0, 0xFF, 0xEE, 0x31};
    RadioTxConfig cfg;
    cfg.freq_hz = FREQ_CH2;
    cfg.preamble_len = preamble;
    uint32_t const air_ms = (radio.tx_air_time_us(sizeof(payload), cfg) + 999) / 1000;

    uint32_t const start = esphome::millis();
    EXPECT_FALSE(radio.send_packet(payload, sizeof(payload), cfg).sent());
    uint32_t const elapsed = esphome::millis() - start;

    EXPECT_GT(elapsed, air_ms) << "a frame still on air must not be declared lost (preamble " << preamble << ")";
    EXPECT_LE(elapsed, tx_done_timeout_ms(air_ms * 1000) + 2)
        << "a chip that never reports TX-done costs one guard past the air time (preamble " << preamble << ")";
  }
}

namespace {

/// RadioSX1276 whose RSSI reading is scripted and records the channel it was taken on.
class CcaProbeSX1276 : public RadioSX1276 {
 public:
  using RadioSX1276::RadioSX1276;
  int16_t level{-120};
  std::vector<uint32_t> read_freqs;
  int16_t read_rssi() override {
    this->read_freqs.push_back(this->get_current_freq());
    return this->level;
  }
};

}  // namespace

TEST(RadioSX1276, BusyChannelOnTheTxFrequencySendsNothing) {
  RegisterModelSpi spi;
  MockPin rst, dio0, dio4(false);
  CcaProbeSX1276 radio(&spi, &rst, &dio0, &dio4, 17, 0x80);
  radio.change_frequency(FREQ_CH1);  // where an idle hop left the receiver
  radio.level = -60;

  const uint8_t payload[] = {0x88, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x03};
  RadioTxConfig cfg{};
  cfg.freq_hz = FREQ_CH2;
  cfg.cca_threshold_dbm = -90;
  const TxResult result = radio.send_packet(payload, sizeof(payload), cfg);

  EXPECT_EQ(result.status, TxResult::Status::CHANNEL_BUSY);
  EXPECT_EQ(result.cca_level_dbm, -60);
  EXPECT_EQ(radio.read_freqs, std::vector<uint32_t>{FREQ_CH2}) << "retuned to the TX channel before measuring";
  EXPECT_TRUE(spi.tx_fifo().empty()) << "a busy channel must not reach the FIFO";
}

TEST(RadioSX1276, MeasureChannelLevelReadsTheRequestedChannelWithoutTransmitting) {
  RegisterModelSpi spi;
  MockPin rst, dio0, dio4(false);
  CcaProbeSX1276 radio(&spi, &rst, &dio0, &dio4, 17, 0x80);
  radio.change_frequency(FREQ_CH1);
  radio.level = -77;

  EXPECT_EQ(radio.measure_channel_level(FREQ_CH3), -77);
  EXPECT_EQ(radio.measure_channel_level(FREQ_CH3), -77);

  EXPECT_EQ(radio.read_freqs, (std::vector<uint32_t>{FREQ_CH3, FREQ_CH3})) << "retuned once, then read in place";
  EXPECT_EQ(radio.get_current_freq(), FREQ_CH3);
  EXPECT_TRUE(spi.tx_fifo().empty()) << "measuring never reaches the FIFO";
  EXPECT_EQ(radio.cca_settle_us(), SX1276_CCA_SETTLE_US);
}

TEST(RadioSX1276, ClearChannelGoesOnToTransmit) {
  RegisterModelSpi spi;
  MockPin rst, dio0, dio4(false);
  CcaProbeSX1276 radio(&spi, &rst, &dio0, &dio4, 17, 0x80);
  radio.level = -100;

  const uint8_t payload[] = {0x88, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x03};
  RadioTxConfig cfg{};
  cfg.freq_hz = FREQ_CH2;
  cfg.cca_threshold_dbm = -90;
  // TX-done never fires in this host harness, so the send ends FAILED after reaching the FIFO.
  EXPECT_EQ(radio.send_packet(payload, sizeof(payload), cfg).status, TxResult::Status::FAILED);
  EXPECT_EQ(spi.tx_fifo().size(), sizeof(payload));
}

TEST(RadioSX1276, SendPacketRejectsZeroLength) {
  RegisterModelSpi spi;
  MockPin rst, dio0, dio4(false);
  RadioSX1276 radio(&spi, &rst, &dio0, &dio4, 17, 0x80);

  const uint8_t payload[] = {0xAA};
  EXPECT_FALSE(radio.send_packet(payload, 0, RadioTxConfig{}).sent());
  EXPECT_TRUE(spi.tx_fifo().empty());
  EXPECT_TRUE(spi.write_log().empty()) << "a rejected send must not touch SPI at all";
}

TEST(RadioSX1276, SendPacketRejectsOverlongPayload) {
  RegisterModelSpi spi;
  MockPin rst, dio0, dio4(false);
  RadioSX1276 radio(&spi, &rst, &dio0, &dio4, 17, 0x80);

  uint8_t payload[RADIO_PACKET_BUFFER_SIZE + 1] = {0};
  EXPECT_FALSE(radio.send_packet(payload, sizeof(payload), RadioTxConfig{}).sent());
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
  // Deliberately legacy-mode (not ManualClock): set_mode_() below busy-polls millis() in a loop
  // with no other exit condition (radio_sx1276.cpp), so under a clock that only moves when told to
  // it would spin until test_clock's guard aborts the test. See hal.h's ManualClock doc comment.
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

namespace {
/// True when the log holds a RestartRxWithPllLock write to REG_RX_CONFIG, the receiver restart.
bool saw_receiver_restart(const RegisterModelSpi &spi) {
  for (const auto &entry : spi.write_log()) {
    if (entry.first == (REG_RX_CONFIG & 0x7F) && entry.second == (RX_CONFIG_VALUE | 0x20))
      return true;
  }
  return false;
}
}  // namespace

TEST(RadioSX1276, ReceptionInProgressHoldsWhileAFreshPreambleIsComing) {
  // A start preamble is up to LONG_PREAMBLE long and its sync word comes at the end: the hop must
  // stay on the channel for it, or the frame is caught only if the hop happens to be back here
  // when the sync word arrives.
  esphome::test_clock::ManualClock clock;
  RegisterModelSpi spi;
  MockPin rst, dio0, dio4(false);
  RadioSX1276 radio(&spi, &rst, &dio0, &dio4, 17, 0x80);
  spi.set_reg(REG_OP_MODE, MODE_RX);
  spi.set_reg(REG_IRQ_FLAGS1, 0x02);  // PreambleDetect only, no SyncAddressMatch yet

  EXPECT_TRUE(radio.reception_in_progress()) << "first sighting of a preamble holds the channel";
  esphome::test_clock::advance_ms(SX1276_IDLE_PREAMBLE_HOLD_MS - 1);
  EXPECT_TRUE(radio.reception_in_progress()) << "still inside the first slice";
  EXPECT_FALSE(saw_receiver_restart(spi)) << "no restart inside the slice";
}

TEST(RadioSX1276, IdleHoldIsShortSoAFalseDetectionDoesNotParkTheHop) {
  // A strong transmitter on the adjacent channel sets PreambleDetect here and no frame follows.
  // The hold has to end after a slice, not after the longest preamble: with the continuous idle
  // loop a 250 ms park lost whole bursts on the channel the receiver should have been on.
  esphome::test_clock::ManualClock clock;
  RegisterModelSpi spi;
  MockPin rst, dio0, dio4(false);
  RadioSX1276 radio(&spi, &rst, &dio0, &dio4, 17, 0x80);
  spi.set_reg(REG_OP_MODE, MODE_RX);
  spi.set_reg(REG_IRQ_FLAGS1, 0x02);
  ASSERT_TRUE(radio.reception_in_progress());

  esphome::test_clock::advance_ms(SX1276_IDLE_PREAMBLE_HOLD_MS);
  ASSERT_TRUE(radio.reception_in_progress()) << "the slice ended: renewed once";
  spi.set_reg(REG_IRQ_FLAGS1, 0x00);  // the restart cleared PreambleDetect and nothing set it again
  esphome::test_clock::advance_ms(SX1276_IDLE_RENEW_REDETECT_MS);

  EXPECT_FALSE(radio.reception_in_progress()) << "no preamble again: the hop goes on, well under 250 ms";
}

TEST(RadioSX1276, IdleHoldRenewsOnTheSameChannelWhileThePreambleIsDetectedAgain) {
  esphome::test_clock::ManualClock clock;
  RegisterModelSpi spi;
  MockPin rst, dio0, dio4(false);
  RadioSX1276 radio(&spi, &rst, &dio0, &dio4, 17, 0x80);
  spi.set_reg(REG_OP_MODE, MODE_RX);
  spi.set_reg(REG_IRQ_FLAGS1, 0x02);
  const uint32_t freq = radio.get_current_freq();
  ASSERT_TRUE(radio.reception_in_progress());

  esphome::test_clock::advance_ms(SX1276_IDLE_PREAMBLE_HOLD_MS);
  EXPECT_TRUE(radio.reception_in_progress());
  EXPECT_TRUE(saw_receiver_restart(spi)) << "RestartRxWithPllLock on the held channel";
  EXPECT_EQ(radio.get_current_freq(), freq);

  esphome::test_clock::advance_ms(1);  // the still-arriving preamble sets PreambleDetect again
  EXPECT_TRUE(radio.reception_in_progress());
  esphome::test_clock::advance_ms(SX1276_IDLE_PREAMBLE_HOLD_MS - 1);
  EXPECT_TRUE(radio.reception_in_progress()) << "the new slice runs its full length";
}

TEST(RadioSX1276, RunOfRenewedIdleHoldsIsBoundedByTheChainLimit) {
  // The old flag-still-set-counts-as-a-new-sighting behaviour is bounded now: a detector that keeps
  // firing releases the hop after the chain limit, which covers a wake-up preamble.
  esphome::test_clock::ManualClock clock;
  RegisterModelSpi spi;
  MockPin rst, dio0, dio4(false);
  RadioSX1276 radio(&spi, &rst, &dio0, &dio4, 17, 0x80);
  spi.set_reg(REG_OP_MODE, MODE_RX);
  spi.set_reg(REG_IRQ_FLAGS1, 0x02);

  uint32_t held_ms = 0;
  while (radio.reception_in_progress() && held_ms < 10 * IDLE_PREAMBLE_HOLD_CHAIN_MS) {
    esphome::test_clock::advance_ms(1);
    held_ms++;
  }
  EXPECT_GE(held_ms, IDLE_PREAMBLE_HOLD_CHAIN_MS) << "long enough for a wake-up preamble";
  EXPECT_LT(held_ms, IDLE_PREAMBLE_HOLD_CHAIN_MS + SX1276_IDLE_PREAMBLE_HOLD_MS) << "and then the hop goes on";
}

TEST(RadioSX1276, IdleHoldSurvivesTheMillisWrapAndRenewsAcrossIt) {
  esphome::test_clock::ManualClock clock(0xFFFFFFFFu - 9);  // the hold starts 10 ms before millis() wraps
  RegisterModelSpi spi;
  MockPin rst, dio0, dio4(false);
  RadioSX1276 radio(&spi, &rst, &dio0, &dio4, 17, 0x80);
  spi.set_reg(REG_OP_MODE, MODE_RX);
  spi.set_reg(REG_IRQ_FLAGS1, 0x02);
  ASSERT_TRUE(radio.reception_in_progress());

  esphome::test_clock::advance_ms(SX1276_IDLE_PREAMBLE_HOLD_MS - 1);
  EXPECT_TRUE(radio.reception_in_progress());
  EXPECT_FALSE(saw_receiver_restart(spi)) << "the wrap must not end the slice early";
  esphome::test_clock::advance_ms(1);
  EXPECT_TRUE(radio.reception_in_progress());
  EXPECT_TRUE(saw_receiver_restart(spi)) << "renewed at the slice end, across the wrap";
}

TEST(RadioSX1276, RetuneToAnotherChannelEndsARenewalInProgress) {
  esphome::test_clock::ManualClock clock;
  RegisterModelSpi spi;
  MockPin rst, dio0, dio4(false);
  RadioSX1276 radio(&spi, &rst, &dio0, &dio4, 17, 0x80);
  spi.set_reg(REG_OP_MODE, MODE_RX);
  spi.set_reg(REG_IRQ_FLAGS1, 0x02);
  ASSERT_TRUE(radio.reception_in_progress());
  esphome::test_clock::advance_ms(SX1276_IDLE_PREAMBLE_HOLD_MS);
  ASSERT_TRUE(radio.reception_in_progress());  // renew: waiting for the preamble again

  radio.change_frequency(FREQ_CH3);
  spi.set_reg(REG_IRQ_FLAGS1, 0x00);
  EXPECT_FALSE(radio.reception_in_progress()) << "the wait for the old channel's preamble does not carry over";
}

TEST(RadioSX1276, ReceivedFrameEndsTheIdleHoldEvenIfTheHopNeverSawItsSync) {
  // A long loop pass can hide the frame's SyncAddressMatch from the hop; the frame itself still ends
  // the hold, so the hop does not sit out the rest of the slice on a channel that is quiet again.
  esphome::test_clock::ManualClock clock;
  RegisterModelSpi spi;
  MockPin rst, dio0, dio4(false);
  RadioSX1276 radio(&spi, &rst, &dio0, &dio4, 17, 0x80);
  spi.set_reg(REG_OP_MODE, MODE_RX);
  spi.set_reg(REG_IRQ_FLAGS1, 0x02);
  ASSERT_TRUE(radio.reception_in_progress());

  const uint8_t frame[] = {0x11, 0x22, 0x33};
  spi.seed_rx_fifo(frame, sizeof(frame));
  spi.set_reg(REG_IRQ_FLAGS2, 0x04);  // PayloadReady
  radio.mark_dio_fired_from_isr();
  RadioRxPacket packet{};
  ASSERT_TRUE(radio.check_for_packet(packet));

  spi.set_reg(REG_IRQ_FLAGS1, 0x00);  // the receiver restarted after the frame
  esphome::test_clock::advance_ms(1);
  EXPECT_FALSE(radio.reception_in_progress()) << "the frame the preamble announced has arrived";
}

TEST(RadioSX1276, ChangeFrequencyInRxRestartsTheReceiver) {
  // A FastHop retune writes FRF only; restarting the chain clears PreambleDetect and re-arms
  // AGC/AFC for the new channel, which is what makes the preamble hold above meaningful.
  RegisterModelSpi spi;
  MockPin rst, dio0, dio4(false);
  RadioSX1276 radio(&spi, &rst, &dio0, &dio4, 17, 0x80);
  spi.set_reg(REG_OP_MODE, MODE_RX);

  radio.change_frequency(FREQ_CH3);

  bool restarted = false;
  for (const auto &entry : spi.write_log())
    restarted |= entry.first == (REG_RX_CONFIG & 0x7F) && entry.second == (RX_CONFIG_VALUE | 0x20);
  EXPECT_TRUE(restarted) << "RestartRxWithPllLock after the retune";
}

TEST(RadioSX1276, ChangeFrequencyOutsideRxDoesNotTouchTheReceiver) {
  // send_packet() retunes in standby before TX; there is no receiver chain to restart then.
  RegisterModelSpi spi;
  MockPin rst, dio0, dio4(false);
  RadioSX1276 radio(&spi, &rst, &dio0, &dio4, 17, 0x80);
  spi.set_reg(REG_OP_MODE, MODE_STDBY);

  radio.change_frequency(FREQ_CH3);

  for (const auto &entry : spi.write_log())
    EXPECT_NE(entry.first, REG_RX_CONFIG & 0x7F) << "no receiver restart outside RX";
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
  tuning.response_preamble_ms = 5.0F;  // 192 bit periods: 19 bytes at 10 bit periods each
  radio.apply_tuning(tuning);

  EXPECT_EQ(spi.get_reg(REG_RX_BW), static_cast<uint8_t>(SX1276RxBandwidth::BW_125_0_KHZ));
  EXPECT_EQ(spi.get_reg(REG_AFC_BW), static_cast<uint8_t>(SX1276RxBandwidth::BW_125_0_KHZ));
  EXPECT_EQ(radio.response_preamble(), 19u);
}

TEST(RadioSX1276, ReadRssiAppliesFormula) {
  RegisterModelSpi spi;
  MockPin rst, dio0, dio4(false);
  RadioSX1276 radio(&spi, &rst, &dio0, &dio4, 17, 0x80);
  spi.set_reg(REG_RSSI_VALUE, 100);

  EXPECT_EQ(radio.read_rssi(), -50);
}

// ============================================================================
// Capture RSSI: the level at sync-word detection, not at the end of the frame. RegRssiValue is
// live, so a read at PayloadReady sees the noise floor behind the frame.
// ============================================================================

namespace {
constexpr uint8_t kRssiInFrame = 84;      // -42 dBm
constexpr uint8_t kRssiAfterFrame = 168;  // -84 dBm
}  // namespace

TEST(RadioSX1276, BlockingCaptureReportsTheLevelSeenAtSync) {
  RegisterModelSpi spi;
  MockPin rst, dio0, dio4(false);
  RadioSX1276 radio(&spi, &rst, &dio0, &dio4, 17, 0x80);
  const uint8_t frame[] = {0xDE, 0xAD, 0xBE, 0xEF};
  spi.seed_rx_fifo(frame, sizeof(frame));
  spi.set_reg(REG_IRQ_FLAGS1, 0x01);  // SyncAddressMatch already up at the first poll
  spi.set_reg(REG_RSSI_VALUE, kRssiInFrame);
  // The frame ends during the first poll: PayloadReady rises and the level falls to the floor.
  spi.on_nth_read(REG_IRQ_FLAGS2, 1, [&] {
    spi.set_reg(REG_IRQ_FLAGS2, 0x04);
    spi.set_reg(REG_RSSI_VALUE, kRssiAfterFrame);
  });

  RadioRxPacket packet{};
  ASSERT_TRUE(radio.wait_for_packet(packet, 100));

  EXPECT_EQ(radio.get_last_capture().rssi_dbm, -42) << "the end-of-frame read would report -84";
}

TEST(RadioSX1276, BlockingCaptureFallsBackToTheLiveReadWithoutASyncSighting) {
  RegisterModelSpi spi;
  MockPin rst, dio0, dio4(false);
  RadioSX1276 radio(&spi, &rst, &dio0, &dio4, 17, 0x80);
  const uint8_t frame[] = {0x11, 0x22};
  spi.seed_rx_fifo(frame, sizeof(frame));
  spi.set_reg(REG_IRQ_FLAGS2, 0x04);  // already complete, sync flag never polled
  spi.set_reg(REG_RSSI_VALUE, 100);

  RadioRxPacket packet{};
  ASSERT_TRUE(radio.wait_for_packet(packet, 100));

  EXPECT_EQ(radio.get_last_capture().rssi_dbm, -50);
}

TEST(RadioSX1276, IdleCaptureReportsTheLevelLatchedByTheHopGate) {
  RegisterModelSpi spi;
  MockPin rst, dio0, dio4(false);
  RadioSX1276 radio(&spi, &rst, &dio0, &dio4, 17, 0x80);
  spi.set_reg(REG_IRQ_FLAGS1, 0x01);
  spi.set_reg(REG_RSSI_VALUE, kRssiInFrame);
  ASSERT_TRUE(radio.reception_in_progress()) << "first sighting of the sync word latches the level";

  // The frame completes: the level falls, PayloadReady rises, DIO0 fires.
  spi.set_reg(REG_RSSI_VALUE, kRssiAfterFrame);
  spi.set_reg(REG_IRQ_FLAGS2, 0x04);
  const uint8_t frame[] = {0x11, 0x22, 0x33};
  spi.seed_rx_fifo(frame, sizeof(frame));
  radio.mark_dio_fired_from_isr();

  RadioRxPacket packet{};
  ASSERT_TRUE(radio.check_for_packet(packet));
  EXPECT_EQ(radio.get_last_capture().rssi_dbm, -42);
}

TEST(RadioSX1276, IdleCaptureFallsBackToTheLiveReadWhenSyncWasNeverSeen) {
  RegisterModelSpi spi;
  MockPin rst, dio0, dio4(false);
  RadioSX1276 radio(&spi, &rst, &dio0, &dio4, 17, 0x80);
  spi.set_reg(REG_IRQ_FLAGS2, 0x04);
  spi.set_reg(REG_RSSI_VALUE, kRssiAfterFrame);
  const uint8_t frame[] = {0x11};
  spi.seed_rx_fifo(frame, sizeof(frame));
  radio.mark_dio_fired_from_isr();

  RadioRxPacket packet{};
  ASSERT_TRUE(radio.check_for_packet(packet));
  EXPECT_EQ(radio.get_last_capture().rssi_dbm, -84);
}

TEST(RadioSX1276, ALatchedLevelNeverLeaksIntoTheNextCapture) {
  RegisterModelSpi spi;
  MockPin rst, dio0, dio4(false);
  RadioSX1276 radio(&spi, &rst, &dio0, &dio4, 17, 0x80);
  spi.set_reg(REG_IRQ_FLAGS1, 0x01);
  spi.set_reg(REG_RSSI_VALUE, kRssiInFrame);
  ASSERT_TRUE(radio.reception_in_progress());
  spi.set_reg(REG_IRQ_FLAGS2, 0x04);
  const uint8_t first[] = {0x11};
  spi.seed_rx_fifo(first, sizeof(first));
  radio.mark_dio_fired_from_isr();
  RadioRxPacket packet{};
  ASSERT_TRUE(radio.check_for_packet(packet));

  // A second frame arrives and is only seen at PayloadReady.
  spi.set_reg(REG_IRQ_FLAGS1, 0x00);
  spi.set_reg(REG_RSSI_VALUE, 120);  // -60 dBm
  const uint8_t second[] = {0x22};
  spi.seed_rx_fifo(second, sizeof(second));
  radio.mark_dio_fired_from_isr();
  ASSERT_TRUE(radio.check_for_packet(packet));

  EXPECT_EQ(radio.get_last_capture().rssi_dbm, -60);
}

TEST(RadioSX1276, ALatchedLevelDoesNotSurviveARetune) {
  RegisterModelSpi spi;
  MockPin rst, dio0, dio4(false);
  RadioSX1276 radio(&spi, &rst, &dio0, &dio4, 17, 0x80);
  spi.set_reg(REG_IRQ_FLAGS1, 0x01);
  spi.set_reg(REG_RSSI_VALUE, kRssiInFrame);
  ASSERT_TRUE(radio.reception_in_progress());

  radio.change_frequency(FREQ_CH3);  // the receiver restarts on another channel
  spi.set_reg(REG_IRQ_FLAGS1, 0x00);
  spi.set_reg(REG_RSSI_VALUE, 120);
  spi.set_reg(REG_IRQ_FLAGS2, 0x04);
  const uint8_t frame[] = {0x11};
  spi.seed_rx_fifo(frame, sizeof(frame));
  radio.mark_dio_fired_from_isr();

  RadioRxPacket packet{};
  ASSERT_TRUE(radio.check_for_packet(packet));
  EXPECT_EQ(radio.get_last_capture().rssi_dbm, -60);
}

TEST(RadioSX1276, ALatchedLevelDoesNotSurviveTheSyncFlagGoing) {
  RegisterModelSpi spi;
  MockPin rst, dio0, dio4(false);
  RadioSX1276 radio(&spi, &rst, &dio0, &dio4, 17, 0x80);
  spi.set_reg(REG_IRQ_FLAGS1, 0x01);
  spi.set_reg(REG_RSSI_VALUE, kRssiInFrame);
  ASSERT_TRUE(radio.reception_in_progress());

  spi.set_reg(REG_IRQ_FLAGS1, 0x00);  // a sync match with no frame behind it, gone
  EXPECT_FALSE(radio.reception_in_progress());
  spi.set_reg(REG_RSSI_VALUE, 120);
  spi.set_reg(REG_IRQ_FLAGS2, 0x04);
  const uint8_t frame[] = {0x11};
  spi.seed_rx_fifo(frame, sizeof(frame));
  radio.mark_dio_fired_from_isr();

  RadioRxPacket packet{};
  ASSERT_TRUE(radio.check_for_packet(packet));
  EXPECT_EQ(radio.get_last_capture().rssi_dbm, -60);
}

TEST(RadioSX1276, ABlockingWaitForgetsTheLevelOfAFrameTheChipDropped) {
  RegisterModelSpi spi;
  MockPin rst, dio0, dio4(false);
  RadioSX1276 radio(&spi, &rst, &dio0, &dio4, 17, 0x80);
  spi.set_reg(REG_IRQ_FLAGS1, 0x01);  // first frame: sync seen at the first poll
  spi.set_reg(REG_RSSI_VALUE, kRssiInFrame);
  // A bad CRC makes the chip drop that frame (SyncAddressMatch gone at the second poll); a second
  // frame, seen only at PayloadReady, completes in the same wait.
  spi.on_nth_read(REG_IRQ_FLAGS1, 2, [&] {
    spi.set_reg(REG_RSSI_VALUE, 120);  // -60 dBm
    spi.set_reg(REG_IRQ_FLAGS2, 0x04);
  });
  const uint8_t frame[] = {0x11};
  spi.seed_rx_fifo(frame, sizeof(frame));

  RadioRxPacket packet{};
  ASSERT_TRUE(radio.wait_for_packet(packet, 100));
  EXPECT_EQ(radio.get_last_capture().rssi_dbm, -60) << "the dropped frame's -42 dBm must not be reported";
}

TEST(RadioSX1276, ALatchedLevelDoesNotSurviveAnRxRestart) {
  RegisterModelSpi spi;
  MockPin rst, dio0, dio4(false);
  RadioSX1276 radio(&spi, &rst, &dio0, &dio4, 17, 0x80);
  spi.set_reg(REG_IRQ_FLAGS1, 0x01);
  spi.set_reg(REG_RSSI_VALUE, kRssiInFrame);
  ASSERT_TRUE(radio.reception_in_progress());

  radio.set_mode_standby();  // leaving RX ends the reception the level belonged to
  radio.set_mode_rx();
  spi.set_reg(REG_RSSI_VALUE, 120);
  spi.set_reg(REG_IRQ_FLAGS2, 0x04);
  const uint8_t frame[] = {0x11};
  spi.seed_rx_fifo(frame, sizeof(frame));
  radio.mark_dio_fired_from_isr();

  RadioRxPacket packet{};
  ASSERT_TRUE(radio.check_for_packet(packet));
  EXPECT_EQ(radio.get_last_capture().rssi_dbm, -60);
}

TEST(RadioSX1276, ReadRssiStaysALiveReadAfterASyncLatch) {
  RegisterModelSpi spi;
  MockPin rst, dio0, dio4(false);
  RadioSX1276 radio(&spi, &rst, &dio0, &dio4, 17, 0x80);
  spi.set_reg(REG_IRQ_FLAGS1, 0x01);
  spi.set_reg(REG_RSSI_VALUE, kRssiInFrame);
  ASSERT_TRUE(radio.reception_in_progress());
  spi.set_reg(REG_RSSI_VALUE, kRssiAfterFrame);

  EXPECT_EQ(radio.read_rssi(), -84) << "the channel check measures the channel now";
}

TEST(RadioSX1276, ChipNameIsSx1276) {
  RegisterModelSpi spi;
  MockPin rst, dio0, dio4(false);
  RadioSX1276 radio(&spi, &rst, &dio0, &dio4, 17, 0x80);
  EXPECT_STREQ(radio.chip_name(), "sx1276");
}

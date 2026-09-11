/// @file radio_sx1262_test.cpp
/// @brief SX1262 init/configure_radio_() coverage: TCXO voltage code, the pre-CALIBRATE
///        device-error clear, the bounded XOSC-start retry ladder, the bare-crystal path, and the
///        device-error bit decoder.
///
/// Mirrors the harness/mocking style of tests/radio_lr1121_test.cpp (ScriptedSpi + a testable
/// subclass); SX1262 opcodes are a single byte, so transactions are matched on transaction[0]
/// rather than ScriptedSpi::find_opcode(), which assumes the LR1121's 16-bit opcodes.

#include "radio_lr1121.h"
#include "radio_sx1262.h"

#include "esphome/core/application.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"
#include "stubs/scripted_spi.h"
#include "stubs/soft_phy_test_driver.h"

#include <gtest/gtest.h>
#include <functional>
#include <string>
#include <vector>

using namespace esphome::home_io_control;

namespace {

using TestableRadioSX1262 = test::TestableSoftPhy<RadioSX1262>;
using TestableRadioLR1121 = test::TestableSoftPhy<RadioLR1121>;

// TCXO_VOLTAGE_OPTIONS code for "1_8V" — the schema default (components/home_io_control/__init__.py,
// hub_core.h DEFAULT_TCXO_VOLTAGE_SETTING_1P8V). 0-based: it is the chip's own SetDIO3AsTCXOCtrl /
// SetTcxoMode voltage code with no mapping.
constexpr uint8_t TCXO_CODE_1_8V = 0x02;

// Index of the first recorded transaction whose opcode byte is `opcode`, or -1.
int first_tx_with_opcode(const ScriptedSpi &spi, uint8_t opcode) {
  const auto &txs = spi.transactions();
  for (size_t i = 0; i < txs.size(); i++) {
    if (!txs[i].empty() && txs[i][0] == opcode)
      return static_cast<int>(i);
  }
  return -1;
}

// Number of recorded transactions whose opcode byte is `opcode`.
int count_tx_with_opcode(const ScriptedSpi &spi, uint8_t opcode) {
  int n = 0;
  for (const auto &tx : spi.transactions()) {
    if (!tx.empty() && tx[0] == opcode)
      n++;
  }
  return n;
}

// 24-bit startup-delay field of a SetDIO3AsTCXOCtrl transaction (bytes 2..4, big-endian).
uint32_t tcxo_startup_ticks(const std::vector<uint8_t> &tx) {
  return (static_cast<uint32_t>(tx[2]) << 16) | (static_cast<uint32_t>(tx[3]) << 8) | tx[4];
}

// ScriptedSpi that makes the first `reads_to_fault` GetDeviceErrors reads report `errors` and the
// rest report 0 — so the XOSC-start retry ladder can be driven through its escalation path without
// hand-counting every unrelated SPI byte into the flat response queue. `on_tcxo_write`, when set,
// is invoked once per SetDIO3AsTCXOCtrl transaction; `on_error_read(n)` once per GetDeviceErrors
// read with its 1-based index — hooks a test can use to snapshot driver state or arm a slow BUSY
// pin at a precise point in init().
class DeviceErrorSpi : public ScriptedSpi {
 public:
  uint16_t errors{0};
  int reads_to_fault{0};
  std::function<void()> on_tcxo_write;
  std::function<void(int)> on_error_read;

  void spi_enable() override {
    ScriptedSpi::spi_enable();
    txn_idx_ = 0;
    txn_is_err_read_ = false;
  }

  uint8_t spi_transfer(uint8_t data) override {
    const uint8_t recorded = ScriptedSpi::spi_transfer(data);
    const int idx = txn_idx_++;
    if (idx == 0) {
      if (data == esphome::home_io_control::SX1262_GET_DEVICE_ERRORS)
        txn_is_err_read_ = (err_reads_seen_++ < reads_to_fault);
      else if (data == esphome::home_io_control::SX1262_SET_DIO3_AS_TCXO_CTRL && on_tcxo_write)
        on_tcxo_write();
      return recorded;
    }
    if (txn_is_err_read_ && idx == 2)
      return static_cast<uint8_t>(errors >> 8);  // read_opcode_: opcode, NOP, MSB, LSB
    if (txn_is_err_read_ && idx == 3) {
      if (on_error_read)
        on_error_read(err_reads_seen_);  // fired after the read completes, before init()'s next step
      return static_cast<uint8_t>(errors & 0xFF);
    }
    return recorded;
  }

 private:
  int txn_idx_{0};
  bool txn_is_err_read_{false};
  int err_reads_seen_{0};
};

// MockPin that reads BUSY-high for `high_reads` polls once armed, then low. Arming is deferred to a
// precise point in init() (via a DeviceErrorSpi hook) so the wait_busy_() calls before it — which
// run on the default 10 ms budget — are not tripped.
class ArmableBusyPin : public MockPin {
 public:
  ArmableBusyPin() : MockPin(false) {}
  void arm(int high_reads) { remaining_ = high_reads; }
  bool digital_read() override {
    if (remaining_ > 0) {
      remaining_--;
      return true;
    }
    return false;
  }

 private:
  int remaining_{0};
};

}  // namespace

// ============================================================================
// Item 1 — TCXO voltage code is passed through unmapped (0-based enum)
// ============================================================================

TEST(RadioSX1262Init, TcxoSetupParamsCarryVoltageCodeByteVerbatim) {
  ScriptedSpi spi;
  MockPin rst, dio1, busy(false);
  TestableRadioSX1262 radio(&spi, &rst, &dio1, &busy, 0, TCXO_CODE_1_8V);

  ASSERT_TRUE(radio.init());

  int idx = first_tx_with_opcode(spi, SX1262_SET_DIO3_AS_TCXO_CTRL);
  ASSERT_GE(idx, 0) << "SetDIO3AsTCXOCtrl must be issued during init";
  const auto &tx = spi.transactions()[idx];
  ASSERT_EQ(tx.size(), 5u) << "opcode(1) + voltage(1) + 24-bit startup delay(3)";
  EXPECT_EQ(tx[1], TCXO_CODE_1_8V) << "the YAML voltage code is the chip code directly — no mapping on the SX1262 path";
}

TEST(RadioSX1262Init, TcxoVoltageByteMatchesLr1121ForSameYamlCode) {
  // Cross-driver: one TCXO_VOLTAGE_OPTIONS integer must reach the chip as the same voltage byte on
  // both paths. Both are now identity (the LR1121's historical `- 1` is gone).
  for (uint8_t code : {uint8_t{0x00}, TCXO_CODE_1_8V, uint8_t{0x07}}) {
    ScriptedSpi sx_spi;
    MockPin sx_rst, sx_dio1, sx_busy(false);
    TestableRadioSX1262 sx(&sx_spi, &sx_rst, &sx_dio1, &sx_busy, 0, code);
    ASSERT_TRUE(sx.init());
    int sx_idx = first_tx_with_opcode(sx_spi, SX1262_SET_DIO3_AS_TCXO_CTRL);
    ASSERT_GE(sx_idx, 0);
    const uint8_t sx_voltage_byte = sx_spi.transactions()[sx_idx][1];

    ScriptedSpi lr_spi;
    MockPin lr_rst, lr_irq, lr_busy(false);
    TestableRadioLR1121 lr(&lr_spi, &lr_rst, &lr_irq, &lr_busy, 17, code);
    lr_spi.queue_responses({0x00, 0x01, LR1121_DEVICE_TYPE, 0x02, 0x01});  // valid GetVersion
    ASSERT_TRUE(lr.init());
    int lr_idx = lr_spi.find_opcode(LR1121_CMD_SET_TCXO_MODE);
    ASSERT_GE(lr_idx, 0);
    const uint8_t lr_voltage_byte = lr_spi.transactions()[lr_idx][2];  // after the 2-byte opcode

    EXPECT_EQ(sx_voltage_byte, code) << "SX1262 must pass YAML code 0x" << std::hex << (int) code
                                     << " through unmapped";
    EXPECT_EQ(lr_voltage_byte, code) << "LR1121 must pass YAML code 0x" << std::hex << (int) code
                                     << " through unmapped";
    EXPECT_EQ(sx_voltage_byte, lr_voltage_byte) << "both drivers must agree on the chip voltage byte";
  }
}

// ============================================================================
// Item 2 / Item 5 — device errors are cleared between SetDIO3AsTCXOCtrl and CALIBRATE
// ============================================================================

TEST(RadioSX1262Init, ClearsDeviceErrorsBetweenTcxoSetupAndCalibrate) {
  ScriptedSpi spi;
  MockPin rst, dio1, busy(false);
  TestableRadioSX1262 radio(&spi, &rst, &dio1, &busy, 0, TCXO_CODE_1_8V);

  ASSERT_TRUE(radio.init());

  int tcxo_idx = first_tx_with_opcode(spi, SX1262_SET_DIO3_AS_TCXO_CTRL);
  int clear_idx = first_tx_with_opcode(spi, SX1262_CLEAR_DEVICE_ERRORS);
  int calibrate_idx = first_tx_with_opcode(spi, SX1262_CALIBRATE);

  ASSERT_GE(tcxo_idx, 0);
  ASSERT_GE(clear_idx, 0)
      << "the expected POR flags (XOSC_START_ERR/IMG_CALIB_ERR) must be cleared, not just warned about";
  ASSERT_GE(calibrate_idx, 0);
  EXPECT_LT(tcxo_idx, clear_idx) << "the clear belongs after SetDIO3AsTCXOCtrl";
  EXPECT_LT(clear_idx, calibrate_idx) << "and before CALIBRATE, so calibration runs on a clean error latch";
}

// ============================================================================
// Item 5 — happy path issues exactly one TCXO setup + one CALIBRATE, no extra log
// ============================================================================

TEST(RadioSX1262Init, HappyPathIssuesSingleTcxoSetupAndCalibrate) {
  // ScriptedSpi returns 0 for GetDeviceErrors, so XOSC_START_ERR reads clear after the first
  // attempt and the retry ladder breaks immediately — identical wire traffic to the old
  // single-shot bring-up.
  ScriptedSpi spi;
  MockPin rst, dio1, busy(false);
  TestableRadioSX1262 radio(&spi, &rst, &dio1, &busy, 0, TCXO_CODE_1_8V);

  ASSERT_TRUE(radio.init());

  EXPECT_EQ(count_tx_with_opcode(spi, SX1262_SET_DIO3_AS_TCXO_CTRL), 1) << "one attempt on the happy path";
  EXPECT_EQ(count_tx_with_opcode(spi, SX1262_CALIBRATE), 1) << "one all-blocks CALIBRATE on the happy path";

  int idx = first_tx_with_opcode(spi, SX1262_SET_DIO3_AS_TCXO_CTRL);
  ASSERT_GE(idx, 0);
  EXPECT_EQ(spi.transactions()[idx][1], TCXO_CODE_1_8V) << "voltage byte unchanged by the retry rework";
  // First attempt uses the 5 ms rung: 5000 us / 15.625 us = 320 ticks = 0x000140.
  EXPECT_EQ(spi.transactions()[idx][2], 0x00);
  EXPECT_EQ(spi.transactions()[idx][3], 0x01);
  EXPECT_EQ(spi.transactions()[idx][4], 0x40);
}

// ============================================================================
// Item 5 — the retry ladder escalates on a persistent XOSC_START_ERR
// ============================================================================

TEST(RadioSX1262Init, RetryLadderEscalatesWhenXoscStartErrPersists) {
  DeviceErrorSpi spi;
  spi.errors = SX1262_DEV_ERR_XOSC_START;
  spi.reads_to_fault = 100;  // every GetDeviceErrors read reports the TCXO as not started
  MockPin rst, dio1, busy(false);
  TestableRadioSX1262 radio(&spi, &rst, &dio1, &busy, 0, TCXO_CODE_1_8V);

  ASSERT_TRUE(radio.init()) << "a stuck TCXO must log and continue, never brick the component";
  EXPECT_FALSE(radio.is_failed());

  std::vector<uint32_t> ticks;
  for (const auto &tx : spi.transactions()) {
    if (!tx.empty() && tx[0] == SX1262_SET_DIO3_AS_TCXO_CTRL)
      ticks.push_back(tcxo_startup_ticks(tx));
  }
  ASSERT_EQ(ticks.size(), 3u) << "all three rungs are attempted before giving up";
  EXPECT_EQ(count_tx_with_opcode(spi, SX1262_CALIBRATE), 3) << "one CALIBRATE per rung";
  // 15.625 us/tick: 5 ms -> 320, 10 ms -> 640, 50 ms -> 3200. Strictly increasing.
  EXPECT_EQ(ticks[0], 320u);
  EXPECT_EQ(ticks[1], 640u);
  EXPECT_EQ(ticks[2], 3200u);
  EXPECT_LT(ticks[0], ticks[1]);
  EXPECT_LT(ticks[1], ticks[2]);
}

TEST(RadioSX1262Init, RetryLadderStopsAsSoonAsXoscStartErrClears) {
  DeviceErrorSpi spi;
  spi.errors = SX1262_DEV_ERR_XOSC_START;
  spi.reads_to_fault = 1;  // first attempt still faulted, second reads clean
  MockPin rst, dio1, busy(false);
  TestableRadioSX1262 radio(&spi, &rst, &dio1, &busy, 0, TCXO_CODE_1_8V);

  ASSERT_TRUE(radio.init());
  EXPECT_FALSE(radio.is_failed());
  EXPECT_EQ(count_tx_with_opcode(spi, SX1262_SET_DIO3_AS_TCXO_CTRL), 2)
      << "the ladder stops the moment XOSC_START_ERR reads clear — no needless third rung";
}

// ============================================================================
// Item 5 (review r3) — wait_busy_() budget is widened per rung and restored after
// ============================================================================
// The chip holds BUSY through the whole programmed startup window plus calibration before it can
// latch XOSC_START_ERR. If wait_busy_() kept its steady-state 10 ms budget, rung 3's 50 ms window
// would trip a BUSY timeout, latch failed_, and brick the component — the exact case the ladder
// exists to rescue. These pin the widen (per rung) and the restore semantics.

TEST(RadioSX1262Init, BusyTimeoutIsWidenedToEachRungDuringTcxoBringUp) {
  DeviceErrorSpi spi;
  spi.errors = SX1262_DEV_ERR_XOSC_START;
  spi.reads_to_fault = 100;  // force the full 3-rung escalation
  MockPin rst, dio1, busy(false);
  TestableRadioSX1262 radio(&spi, &rst, &dio1, &busy, 0, TCXO_CODE_1_8V);

  std::vector<uint32_t> budget_at_rung;
  spi.on_tcxo_write = [&] { budget_at_rung.push_back(radio.busy_timeout_ms_for_test()); };

  ASSERT_TRUE(radio.init());
  ASSERT_FALSE(radio.is_failed());

  // TCXO_BUSY_MARGIN_MS (25) over each rung's programmed startup window (5 / 10 / 50 ms).
  ASSERT_EQ(budget_at_rung.size(), 3u);
  EXPECT_EQ(budget_at_rung[0], 5u + 25u);
  EXPECT_EQ(budget_at_rung[1], 10u + 25u);
  EXPECT_EQ(budget_at_rung[2], 50u + 25u);
}

TEST(RadioSX1262Init, BusyTimeoutRestoredToDefaultAfterHappyBringUp) {
  ScriptedSpi spi;  // GetDeviceErrors reads 0 — rung 1 succeeds
  MockPin rst, dio1, busy(false);
  TestableRadioSX1262 radio(&spi, &rst, &dio1, &busy, 0, TCXO_CODE_1_8V);

  ASSERT_TRUE(radio.init());
  EXPECT_EQ(radio.busy_timeout_ms_for_test(), SX1262_BUSY_TIMEOUT_MS)
      << "a one-attempt bring-up must leave the steady-state BUSY budget in place";
}

TEST(RadioSX1262Init, BusyTimeoutKeepsRescuedRungCeilingAfterEscalation) {
  // A board that only starts on rung 2/3 re-pays that same startup window at configure_radio_()
  // step 4 (STDBY_XOSC). Restoring the 10 ms default here would then time out step 5 and brick a
  // chip the ladder just rescued, so the widened ceiling for the last rung is kept.
  DeviceErrorSpi spi;
  spi.errors = SX1262_DEV_ERR_XOSC_START;
  spi.reads_to_fault = 100;
  MockPin rst, dio1, busy(false);
  TestableRadioSX1262 radio(&spi, &rst, &dio1, &busy, 0, TCXO_CODE_1_8V);

  ASSERT_TRUE(radio.init());
  EXPECT_EQ(radio.busy_timeout_ms_for_test(), 50u + 25u)
      << "after escalating to the 50 ms rung the BUSY ceiling stays wide for the rest of init";
}

// End-to-end: a slow TCXO that only starts on rung 3, then holds BUSY again for ~40 polls when
// configure_radio_() step 4 re-enters STDBY_XOSC. Under the round-2 code (restore to the 10 ms
// default after the ladder) that step-4 wait_busy_() times out and latches failed_; the kept
// ceiling is what carries it through. Revert either the per-rung widen or the kept ceiling and
// this init() returns false.
TEST(RadioSX1262Init, KeptCeilingCarriesASlowTcxoThroughStandbyXosc) {
  DeviceErrorSpi spi;
  spi.errors = SX1262_DEV_ERR_XOSC_START;
  spi.reads_to_fault = 100;  // full 3-rung escalation
  MockPin rst, dio1;
  ArmableBusyPin busy;
  TestableRadioSX1262 radio(&spi, &rst, &dio1, &busy, 0, TCXO_CODE_1_8V);

  // Arm right after the 3rd (last) GetDeviceErrors read of the ladder — i.e. after that rung's
  // wait_busy_() calls have run on a low pin, and before configure_radio_() step 4's STDBY_XOSC.
  // 40 > the 10 ms default budget, < the 75 ms kept ceiling.
  spi.on_error_read = [&](int n) {
    if (n == 3)
      busy.arm(40);
  };

  ASSERT_TRUE(radio.init()) << "the kept BUSY ceiling must carry a rung-3-rescued TCXO through STDBY_XOSC";
  EXPECT_FALSE(radio.is_failed());
  EXPECT_EQ(count_tx_with_opcode(spi, SX1262_SET_DIO3_AS_TCXO_CTRL), 3);
}

// End-to-end companion to the white-box widen test: rung 3 itself holds BUSY for ~40 polls (past
// the 10 ms default, inside the 75 ms rung-3 budget) during its post-SetDIO3 clear/calibrate. Drop
// the per-rung widen and this wait_busy_() times out -> failed_ -> init() returns false.
TEST(RadioSX1262Init, WidenedRungBudgetCarriesASlowThirdRung) {
  DeviceErrorSpi spi;
  spi.errors = SX1262_DEV_ERR_XOSC_START;
  spi.reads_to_fault = 100;
  MockPin rst, dio1;
  ArmableBusyPin busy;
  TestableRadioSX1262 radio(&spi, &rst, &dio1, &busy, 0, TCXO_CODE_1_8V);

  int tcxo_writes = 0;
  spi.on_tcxo_write = [&] {
    if (++tcxo_writes == 3)
      busy.arm(40);  // consumed by rung 3's own clear-device-errors wait_busy_()
  };

  ASSERT_TRUE(radio.init()) << "rung 3's widened budget must absorb a slow post-SetDIO3 BUSY hold";
  EXPECT_FALSE(radio.is_failed());
  EXPECT_EQ(count_tx_with_opcode(spi, SX1262_SET_DIO3_AS_TCXO_CTRL), 3);
}

// ============================================================================
// Item 6 — tcxo_voltage: none skips all TCXO programming
// ============================================================================

TEST(RadioSX1262Init, BareCrystalSkipsTcxoSetupButStillCalibrates) {
  ScriptedSpi spi;
  MockPin rst, dio1, busy(false);
  TestableRadioSX1262 radio(&spi, &rst, &dio1, &busy, 0, TCXO_VOLTAGE_NONE);

  ASSERT_TRUE(radio.init());

  EXPECT_EQ(count_tx_with_opcode(spi, SX1262_SET_DIO3_AS_TCXO_CTRL), 0)
      << "a bare-crystal board must not drive DIO3 as a TCXO control";
  EXPECT_GE(first_tx_with_opcode(spi, SX1262_CALIBRATE), 0) << "calibration still runs, off the plain crystal";
  // Standby-XOSC is still entered after calibration (SET_STANDBY with param 0x01 appears at least
  // twice: the RC start and the XOSC switch).
  EXPECT_GE(count_tx_with_opcode(spi, SX1262_SET_STANDBY), 2);
}

// ============================================================================
// Item 4 — GetDeviceErrors bit → name decoder
// ============================================================================

TEST(Sx1262DeviceErrorDecoder, ZeroIsNone) {
  char buf[SX1262_DEVICE_ERROR_STR_SIZE];
  sx1262_format_device_errors(0, buf, sizeof(buf));
  EXPECT_STREQ(buf, "none");
}

TEST(Sx1262DeviceErrorDecoder, SingleBitIsItsName) {
  char buf[SX1262_DEVICE_ERROR_STR_SIZE];
  sx1262_format_device_errors(SX1262_DEV_ERR_XOSC_START, buf, sizeof(buf));
  EXPECT_STREQ(buf, "XOSC_START_ERR");
}

TEST(Sx1262DeviceErrorDecoder, MultipleBitsAreJoinedInTableOrder) {
  char buf[SX1262_DEVICE_ERROR_STR_SIZE];
  sx1262_format_device_errors(SX1262_DEV_ERR_PLL_LOCK | SX1262_DEV_ERR_XOSC_START, buf, sizeof(buf));
  EXPECT_STREQ(buf, "XOSC_START_ERR|PLL_LOCK_ERR");
}

TEST(Sx1262DeviceErrorDecoder, UnmappedBitsShowAsUnknown) {
  char buf[SX1262_DEVICE_ERROR_STR_SIZE];
  sx1262_format_device_errors(0x0080, buf, sizeof(buf));  // 0x0080 has no name in the datasheet
  EXPECT_STREQ(buf, "UNKNOWN_0x0080");
}

TEST(Sx1262DeviceErrorDecoder, KnownAndUnknownBitsCombine) {
  char buf[SX1262_DEVICE_ERROR_STR_SIZE];
  sx1262_format_device_errors(SX1262_DEV_ERR_IMG_CALIB | 0x8000, buf, sizeof(buf));
  EXPECT_STREQ(buf, "IMG_CALIB_ERR|UNKNOWN_0x8000");
}

TEST(Sx1262DeviceErrorDecoder, EveryNamedBitAppears) {
  const uint16_t all = SX1262_DEV_ERR_RC64K_CALIB | SX1262_DEV_ERR_RC13M_CALIB | SX1262_DEV_ERR_PLL_CALIB |
                       SX1262_DEV_ERR_ADC_CALIB | SX1262_DEV_ERR_IMG_CALIB | SX1262_DEV_ERR_XOSC_START |
                       SX1262_DEV_ERR_PLL_LOCK | SX1262_DEV_ERR_PA_RAMP;
  char buf[SX1262_DEVICE_ERROR_STR_SIZE];
  sx1262_format_device_errors(all, buf, sizeof(buf));
  const std::string s = buf;
  for (const char *name : {"RC64K_CALIB_ERR", "RC13M_CALIB_ERR", "PLL_CALIB_ERR", "ADC_CALIB_ERR", "IMG_CALIB_ERR",
                           "XOSC_START_ERR", "PLL_LOCK_ERR", "PA_RAMP_ERR"}) {
    EXPECT_NE(s.find(name), std::string::npos) << "missing " << name;
  }
  EXPECT_EQ(s.find("UNKNOWN"), std::string::npos) << "all eight bits are named — no UNKNOWN tail";
}

TEST(Sx1262DeviceErrorDecoder, TinyBufferStaysNulTerminatedWithoutOverflow) {
  char buf[8] = {'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x'};
  sx1262_format_device_errors(SX1262_DEV_ERR_XOSC_START | SX1262_DEV_ERR_PLL_LOCK, buf, sizeof(buf));
  EXPECT_EQ(buf[sizeof(buf) - 1], '\0') << "must never write past the caller's buffer";
  EXPECT_LT(std::string(buf).size(), sizeof(buf));
}

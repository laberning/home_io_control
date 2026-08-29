#include "radio_lr1121.h"
#include "radio_sx1262.h"
#include "radio_interface.h"

#include "esphome/core/application.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"
#include "stubs/scripted_spi.h"
#include "stubs/soft_phy_test_driver.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <vector>

using namespace esphome::home_io_control;

// ============================================================================
// Shared SoftPhyDriverBase behavior — one typed suite, run for every software-PHY chip.
//
// SoftPhyDriverBase has one implementation, so it gets one test suite. Before this file, the
// base class's RX race resolution, sync/preamble predicates, wait-busy short-circuit, hop-holdoff
// lifecycle and length-driven ("early completion") receive were each tested twice — once in
// radio_sx1262_rx_test.cpp and once, copy-pasted with the IRQ constants swapped, in
// radio_lr1121_test.cpp — and the two copies had already drifted apart (#81's fix pair added
// 122 lines of SX1262 test in one commit, then 248 largely-parallel LR1121 lines in the next).
// Each TYPED_TEST below runs against both chips through the traits struct, so no shared body ever
// names a chip constant and a base-class behavior can no longer be verified for one chip only.
//
// Genuinely chip-specific tests (LR1121's GetVersion/PA-config/init-order, SX1262's
// sync-word-register/TX-modulation-workaround, the UART codec tests) stay in the per-chip files.
// ============================================================================

namespace {

// TCXO_VOLTAGE_OPTIONS code for "3_0V" (components/home_io_control/__init__.py). LR1121's
// constructor takes the YAML code directly; SX1262 ignores this argument's exact value here.
constexpr uint8_t kTcxoYamlCode3_0V = 0x07;

struct Sx1262Traits {
  using Driver = RadioSX1262;
  static constexpr uint8_t kTxPower = 0;
  static constexpr uint8_t kTcxo = 0;

  static uint32_t rx_done() { return SX1262_IRQ_RX_DONE; }
  static uint32_t sync_valid() { return SX1262_IRQ_SYNC_WORD_VALID; }
  static uint32_t preamble() { return SX1262_IRQ_PREAMBLE_DETECTED; }
  static uint32_t crc_err() { return SX1262_IRQ_CRC_ERR; }
  static uint8_t rx_buffer_base() { return SX1262_RX_BUFFER_BASE; }

  // The host millis() stub is a +1-per-call counter, so a full BUSY timeout costs
  // ~SX1262_BUSY_TIMEOUT_MS+1 counts to reach and a short-circuited call costs 0. This slack is
  // safely below "ran the loop" and above "returned immediately"; LR1121's own timeout is longer.
  static uint32_t busy_short_circuit_slack() { return 5; }

  static bool saw_standby(const ScriptedSpi &spi) {
    for (const auto &tx : spi.transactions())
      if (!tx.empty() && tx[0] == SX1262_SET_STANDBY)
        return true;
    return false;
  }
  static bool saw_cleared_irq(const ScriptedSpi &spi) {
    for (const auto &tx : spi.transactions())
      if (!tx.empty() && tx[0] == SX1262_CLEAR_IRQ_STATUS)
        return true;
    return false;
  }

  // GetRssiInst on SX1262 clocks out opcode + NOP status byte before the raw RSSI byte
  // (read_opcode_); LR1121 clocks out Stat1 first (read_command_). The base-class formula under
  // test is the same either way — this trait only frames the chip's own wire read.
  static void queue_rssi_raw(ScriptedSpi &spi, uint8_t raw) { spi.queue_responses({0x00, 0x00, raw}); }
};

struct Lr1121Traits {
  using Driver = RadioLR1121;
  static constexpr uint8_t kTxPower = 0;
  static constexpr uint8_t kTcxo = kTcxoYamlCode3_0V;

  static uint32_t rx_done() { return LR1121_IRQ_RX_DONE; }
  static uint32_t sync_valid() { return LR1121_IRQ_SYNC_WORD_VALID; }
  static uint32_t preamble() { return LR1121_IRQ_PREAMBLE_DETECTED; }
  static uint32_t crc_err() { return LR1121_IRQ_CRC_ERR; }
  static uint8_t rx_buffer_base() { return LR1121_RX_BUFFER_BASE; }

  static uint32_t busy_short_circuit_slack() { return 100; }

  static bool saw_standby(const ScriptedSpi &spi) { return spi.find_opcode(LR1121_CMD_SET_STANDBY) >= 0; }
  static bool saw_cleared_irq(const ScriptedSpi &spi) { return spi.find_opcode(LR1121_CMD_CLEAR_IRQ) >= 0; }

  static void queue_rssi_raw(ScriptedSpi &spi, uint8_t raw) { spi.queue_responses({0x00, raw}); }
};

using ChipTypes = ::testing::Types<Sx1262Traits, Lr1121Traits>;

}  // namespace

template<class Traits> class SoftPhyDriver : public ::testing::Test {
 protected:
  using Traits_ = Traits;
  using Driver = typename Traits::Driver;
  using Testable = test::TestableSoftPhy<Driver>;
  using EarlyRx = test::EarlyRxSoftPhy<Driver>;
};

TYPED_TEST_SUITE(SoftPhyDriver, ChipTypes);

// ---------------------------------------------------------------------------
// wait_for_packet(): DIO / IRQ / timeout / race resolution
// ---------------------------------------------------------------------------

TYPED_TEST(SoftPhyDriver, WaitForPacketSuccess_DioFired) {
  MockSpi spi;
  MockPin rst, dio1, busy(false);
  typename TestFixture::Testable radio(&spi, &rst, &dio1, &busy, TypeParam::kTxPower, TypeParam::kTcxo);
  radio.mark_dio_fired_from_isr();

  RadioRxPacket pkt{};
  pkt.len = 4;
  pkt.data[0] = 0xAA;
  pkt.data[1] = 0xBB;
  pkt.data[2] = 0xCC;
  pkt.data[3] = 0xDD;
  radio.set_expected_packet(pkt);
  radio.set_irq_sequence({TypeParam::rx_done()});

  RadioRxPacket result{};
  EXPECT_TRUE(radio.wait_for_packet(result, 100)) << "a DIO edge with RX_DONE must yield the packet";
  ASSERT_EQ(result.len, 4u);
  EXPECT_EQ(result.data[0], 0xAA);
  EXPECT_EQ(result.data[1], 0xBB);
  EXPECT_EQ(result.data[2], 0xCC);
  EXPECT_EQ(result.data[3], 0xDD);
}

TYPED_TEST(SoftPhyDriver, WaitForPacketSuccess_IrqOnly) {
  MockSpi spi;
  MockPin rst, dio1, busy(false);
  typename TestFixture::Testable radio(&spi, &rst, &dio1, &busy, TypeParam::kTxPower, TypeParam::kTcxo);

  RadioRxPacket pkt{};
  pkt.len = 2;
  pkt.data[0] = 0x11;
  pkt.data[1] = 0x22;
  radio.set_expected_packet(pkt);
  radio.set_irq_sequence({TypeParam::rx_done()});

  RadioRxPacket result{};
  EXPECT_TRUE(radio.wait_for_packet(result, 100)) << "IRQ RX_DONE without a pin edge must still succeed";
  ASSERT_EQ(result.len, 2u);
  EXPECT_EQ(result.data[0], 0x11);
  EXPECT_EQ(result.data[1], 0x22);
}

TYPED_TEST(SoftPhyDriver, WaitForPacketTimeout_NoActivity) {
  MockSpi spi;
  MockPin rst, dio1, busy(false);
  typename TestFixture::Testable radio(&spi, &rst, &dio1, &busy, TypeParam::kTxPower, TypeParam::kTcxo);

  RadioRxPacket result{};
  const uint32_t feeds_before = esphome::App.feed_wdt_calls;
  EXPECT_FALSE(radio.wait_for_packet(result, 5)) << "no radio activity must time out and return false";
  EXPECT_EQ(result.len, 0u);
  EXPECT_GT(esphome::App.feed_wdt_calls, feeds_before) << "a multi-millisecond blocking wait must feed the watchdog";
}

TYPED_TEST(SoftPhyDriver, WaitForPacketIgnoresPreambleOnlyActivity) {
  // poll_until_activity_() must not treat a preamble-only IRQ status as terminal — doing so would
  // make finalize_receive_() see no RX_DONE and call reset_rx_state_(), tearing down RX and losing
  // the frame still arriving. The driver must wait through repeated preamble-only readings and
  // still catch the later RX_DONE.
  MockSpi spi;
  MockPin rst, dio1, busy(false);
  typename TestFixture::Testable radio(&spi, &rst, &dio1, &busy, TypeParam::kTxPower, TypeParam::kTcxo);

  RadioRxPacket pkt{};
  pkt.len = 2;
  pkt.data[0] = 0x11;
  pkt.data[1] = 0x22;
  radio.set_expected_packet(pkt);
  radio.set_irq_sequence({TypeParam::preamble(), TypeParam::preamble(), TypeParam::rx_done()});

  RadioRxPacket result{};
  EXPECT_TRUE(radio.wait_for_packet(result, 100)) << "preamble-only readings must not be treated as terminal";
  EXPECT_EQ(result.len, 2u);
}

TYPED_TEST(SoftPhyDriver, WaitForPacketRaceCondition_Resolved) {
  MockSpi spi;
  MockPin rst, dio1, busy(false);
  typename TestFixture::Testable radio(&spi, &rst, &dio1, &busy, TypeParam::kTxPower, TypeParam::kTcxo);

  RadioRxPacket pkt{};
  pkt.len = 3;
  pkt.data[0] = 0xDE;
  pkt.data[1] = 0xAD;
  pkt.data[2] = 0xBE;
  radio.set_expected_packet(pkt);
  radio.set_irq_sequence({TypeParam::sync_valid(), TypeParam::sync_valid() | TypeParam::rx_done()});

  RadioRxPacket result{};
  // The length-driven receive runs ahead of the race handler and declines on this all-zero mock
  // buffer, but spends real air time getting there. The host hal.h stubs advance millis()/micros()
  // one unit per call, so ~1 ms of air time costs ~1000 fake ms of budget; the window is sized for
  // the stub, not for the protocol.
  EXPECT_TRUE(radio.wait_for_packet(result, 20000))
      << "SYNC_WORD_VALID followed by RX_DONE must be resolved and yield the packet";
  ASSERT_EQ(result.len, 3u);
  EXPECT_EQ(result.data[0], 0xDE);
  EXPECT_EQ(result.data[1], 0xAD);
  EXPECT_EQ(result.data[2], 0xBE);
}

TYPED_TEST(SoftPhyDriver, WaitForPacketRaceCondition_Timeout) {
  MockSpi spi;
  MockPin rst, dio1, busy(false);
  typename TestFixture::Testable radio(&spi, &rst, &dio1, &busy, TypeParam::kTxPower, TypeParam::kTcxo);
  radio.set_irq_sequence({TypeParam::sync_valid(), TypeParam::sync_valid()});

  RadioRxPacket result{};
  const uint32_t feeds_before = esphome::App.feed_wdt_calls;
  EXPECT_FALSE(radio.wait_for_packet(result, 5)) << "SYNC without a following RX_DONE must fail";
  EXPECT_EQ(result.len, 0u);
  EXPECT_GT(esphome::App.feed_wdt_calls, feeds_before)
      << "the sync-race wait loop is a separate blocking path and must feed the watchdog too";
}

TYPED_TEST(SoftPhyDriver, WaitForPacketReadFailure) {
  // RX_DONE arrives but the packet read itself fails (CRC error): wait_for_packet() must return
  // false. The capture path is still exercised on the way through.
  MockSpi spi;
  MockPin rst, dio1, busy(false);
  typename TestFixture::Testable radio(&spi, &rst, &dio1, &busy, TypeParam::kTxPower, TypeParam::kTcxo);
  radio.set_irq_sequence({TypeParam::rx_done()});
  radio.set_read_success(false);

  RadioRxPacket result{};
  EXPECT_FALSE(radio.wait_for_packet(result, 100)) << "a failed packet read (CRC error) must return false";
}

// ---------------------------------------------------------------------------
// is_sync_detected() / is_preamble_detected()
// ---------------------------------------------------------------------------

TYPED_TEST(SoftPhyDriver, IsSyncDetected_True) {
  MockSpi spi;
  MockPin rst, dio1, busy(false);
  typename TestFixture::Testable radio(&spi, &rst, &dio1, &busy, TypeParam::kTxPower, TypeParam::kTcxo);
  radio.set_irq_sequence({TypeParam::sync_valid()});
  EXPECT_TRUE(radio.is_sync_detected());
}

TYPED_TEST(SoftPhyDriver, IsSyncDetected_False) {
  MockSpi spi;
  MockPin rst, dio1, busy(false);
  typename TestFixture::Testable radio(&spi, &rst, &dio1, &busy, TypeParam::kTxPower, TypeParam::kTcxo);
  radio.set_irq_sequence({0x00000000});
  EXPECT_FALSE(radio.is_sync_detected()) << "no IRQ bits set → no sync";
}

TYPED_TEST(SoftPhyDriver, IsSyncDetected_WithOtherBits) {
  MockSpi spi;
  MockPin rst, dio1, busy(false);
  typename TestFixture::Testable radio(&spi, &rst, &dio1, &busy, TypeParam::kTxPower, TypeParam::kTcxo);
  radio.set_irq_sequence({TypeParam::sync_valid() | TypeParam::preamble() | TypeParam::rx_done()});
  EXPECT_TRUE(radio.is_sync_detected()) << "sync must be detected even with other bits also set";
}

TYPED_TEST(SoftPhyDriver, IsPreambleDetected_True) {
  MockSpi spi;
  MockPin rst, dio1, busy(false);
  typename TestFixture::Testable radio(&spi, &rst, &dio1, &busy, TypeParam::kTxPower, TypeParam::kTcxo);
  radio.set_irq_sequence({TypeParam::preamble()});
  EXPECT_TRUE(radio.is_preamble_detected());
}

TYPED_TEST(SoftPhyDriver, IsPreambleDetected_False) {
  MockSpi spi;
  MockPin rst, dio1, busy(false);
  typename TestFixture::Testable radio(&spi, &rst, &dio1, &busy, TypeParam::kTxPower, TypeParam::kTcxo);
  radio.set_irq_sequence({TypeParam::rx_done()});
  EXPECT_FALSE(radio.is_preamble_detected()) << "only other IRQ bits set → no preamble";
}

// ---------------------------------------------------------------------------
// wait_busy_() / failed_ short-circuit — SoftPhyDriverBase hoisted this so both chips share the
// "already failed, don't re-run the full BUSY timeout on every later command" guard that only one
// chip had before the hoist (see radio_soft_phy_driver_base.h's own doc comment).
// ---------------------------------------------------------------------------

TYPED_TEST(SoftPhyDriver, WaitBusyShortCircuitsAfterFailure) {
  MockSpi spi;
  MockPin rst, dio1, busy(true);  // BUSY held permanently high → a genuinely dead chip
  typename TestFixture::Testable radio(&spi, &rst, &dio1, &busy, TypeParam::kTxPower, TypeParam::kTcxo);

  const uint32_t feeds_before = esphome::App.feed_wdt_calls;
  radio.set_mode_standby();  // First BUSY wait times out and sets failed_.
  ASSERT_TRUE(radio.is_failed());
  EXPECT_GT(esphome::App.feed_wdt_calls, feeds_before)
      << "the BUSY-pin wait is a distinct blocking path (SPI turnaround, not RX) and must feed too";

  const uint32_t t_before = esphome::millis();
  radio.set_mode_standby();  // Must short-circuit instead of re-running the full timeout.
  const uint32_t t_after = esphome::millis();
  EXPECT_LT(t_after - t_before, TypeParam::busy_short_circuit_slack())
      << "a failed driver must not re-run the BUSY timeout on every subsequent command";
}

// ---------------------------------------------------------------------------
// Idle-path hop holdoff (issue #81): reset_rx_state_() — the single funnel every "RX torn down and
// re-armed" path goes through — must drop reception_in_progress() so the idle-path hop is not
// blocked by a stale deadline.
// ---------------------------------------------------------------------------

TYPED_TEST(SoftPhyDriver, ResetRxStateClearsHopHoldoff) {
  MockSpi spi;
  MockPin rst, dio1, busy(false);
  typename TestFixture::Testable radio(&spi, &rst, &dio1, &busy, TypeParam::kTxPower, TypeParam::kTcxo);
  radio.note_reception_from_test();
  ASSERT_TRUE(radio.reception_in_progress()) << "sanity: the holdoff must start armed";

  radio.mark_dio_fired_from_isr();
  // An IRQ reading that is activity (CRC_ERR) but neither SYNC_WORD_VALID nor RX_DONE reaches
  // check_for_packet()'s catch-all branch, which funnels through the real reset_rx_state_().
  radio.set_irq_sequence({TypeParam::crc_err()});

  RadioRxPacket result{};
  EXPECT_FALSE(radio.check_for_packet(result));
  EXPECT_FALSE(radio.reception_in_progress())
      << "reset_rx_state_() must drop the holdoff, not leave a stale deadline blocking the next "
         "~12 ms of hopping";
}

TYPED_TEST(SoftPhyDriver, CheckForPacketPreambleOnlyDoesNotResetRx) {
  // A preamble-only IRQ means a frame may still be arriving. check_for_packet() must clear just
  // that bit (so the next real completion can still raise the DIO edge) and must NOT call
  // reset_rx_state_(), which would issue SetStandby and tear down RX mid-reception.
  ScriptedSpi spi;
  MockPin rst, dio1, busy(false);
  typename TestFixture::Testable radio(&spi, &rst, &dio1, &busy, TypeParam::kTxPower, TypeParam::kTcxo);
  radio.mark_dio_fired_from_isr();
  radio.set_irq_sequence({TypeParam::preamble()});

  RadioRxPacket packet{};
  EXPECT_FALSE(radio.check_for_packet(packet)) << "preamble alone is not a complete packet yet";
  EXPECT_TRUE(TypeParam::saw_cleared_irq(spi)) << "the preamble bit must be cleared so it can re-fire";
  EXPECT_FALSE(TypeParam::saw_standby(spi)) << "must not tear down RX via reset_rx_state_()";
}

// ---------------------------------------------------------------------------
// Idle-path early completion (issue #81, Mechanism B): check_for_packet()'s sync-without-RX_DONE
// branch attempts try_early_completion_() itself instead of leaving every sync-only poll to the
// following loop() pass. A wrong guess must never cost the frame — only latency.
// ---------------------------------------------------------------------------

namespace {

// The raw bytes the chip's data buffer holds mid-reception: the frame, its CRC-CCITT trailer, and
// the whole lot UART-packed exactly as it arrived off air.
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

// A genuine on-air encoding of `frame` with one byte flipped past the header, so
// soft_phy_peek_frame_length() (which only reads CTRL0's cell) still returns the real length and
// stage 1 proceeds — it is stage 3's CRC check that must reject it.
std::vector<uint8_t> crc_corrupted_on_air_bytes(const std::vector<uint8_t> &frame) {
  std::vector<uint8_t> corrupted = uart_packed_on_air_bytes(frame);
  EXPECT_GT(corrupted.size(), 5u);
  corrupted[5] ^= 0xFF;
  return corrupted;
}

// A real 15-byte RS100 challenge (0x3C) — the frame the hub must turn around fastest, from
// tests/corpus/captures/issues/field_rs100_pairing_key_transfer_timeout.yaml.
const std::vector<uint8_t> kRs100Challenge = {0x0E, 0x00, 0xD1, 0xD4, 0xFF, 0x8C, 0x08, 0x3C,
                                              0x3C, 0x45, 0x51, 0x6F, 0xFE, 0x59, 0x80};

}  // namespace

TYPED_TEST(SoftPhyDriver, CheckForPacketSyncWithoutRxDoneCompletesEarlyOnValidCrc) {
  MockSpi spi;
  MockPin rst, dio1, busy(false);
  typename TestFixture::EarlyRx radio(&spi, &rst, &dio1, &busy, TypeParam::kTxPower, TypeParam::kTcxo);

  radio.mark_dio_fired_from_isr();
  radio.set_rx_buffer(uart_packed_on_air_bytes(kRs100Challenge));
  // RX_DONE deliberately absent — this is the branch that used to just return false and wait a
  // whole loop() pass (or lose the frame outright, pre-#81) for it.
  radio.set_irq_sequence({TypeParam::sync_valid()});

  RadioRxPacket pkt{};
  EXPECT_TRUE(radio.check_for_packet(pkt)) << "a complete, CRC-valid frame must not need to wait for RX_DONE";
  EXPECT_FALSE(radio.fallback_used()) << "the RX_DONE path must not have been reached";

  ASSERT_EQ(pkt.len, kRs100Challenge.size());
  EXPECT_EQ(std::vector<uint8_t>(pkt.data, pkt.data + pkt.len), kRs100Challenge);

  ASSERT_GE(radio.read_lengths().size(), 2u) << "expected a header read then a whole-frame read";
  for (uint8_t offset : radio.read_offsets())
    EXPECT_EQ(offset, TypeParam::rx_buffer_base());
  EXPECT_FALSE(radio.reception_in_progress())
      << "try_early_completion_()'s success clears the holdoff via reset_rx_state_(); a regression "
         "here would spuriously block hops for up to RX_HOP_HOLDOFF_US after every successful "
         "idle-path receive";
}

TYPED_TEST(SoftPhyDriver, CheckForPacketSyncWithoutRxDoneFallsBackWhenCrcInvalid) {
  ScriptedSpi spi;  // the assertion below needs to see whether reset_rx_state_() issued SetStandby
  MockPin rst, dio1, busy(false);
  typename TestFixture::EarlyRx radio(&spi, &rst, &dio1, &busy, TypeParam::kTxPower, TypeParam::kTcxo);

  radio.mark_dio_fired_from_isr();
  radio.set_rx_buffer(crc_corrupted_on_air_bytes(kRs100Challenge));
  radio.set_irq_sequence({TypeParam::sync_valid()});

  RadioRxPacket pkt{};
  EXPECT_FALSE(radio.check_for_packet(pkt));
  EXPECT_FALSE(radio.fallback_used()) << "check_for_packet() must not have gone through read_rx_packet()";
  // A header read followed by a whole-frame read means stage 1 succeeded and stage 2 ran, so the
  // false above can only be stage 3's CRC check — not an earlier length-peek rejection.
  ASSERT_GE(radio.read_lengths().size(), 2u) << "expected a header read then a whole-frame read";
  // Load-bearing: the failure path must leave the chip alone rather than tearing RX down — the
  // basis of the whole "fall-through is benign, not lossy" claim (issue #81 §0.3).
  EXPECT_FALSE(TypeParam::saw_standby(spi)) << "reset_rx_state_() must not have run";
  EXPECT_TRUE(radio.reception_in_progress())
      << "the hop holdoff armed before the early-completion attempt must survive it declining";
}

// ---------------------------------------------------------------------------
// Step 14 — behaviors that had drifted to one chip only. All are SoftPhyDriverBase behavior; the
// difference each depended on (early_rx_read_offset(), the real read_rx_packet() seam, the shared
// RSSI formula, preamble_latched_at_timeout_) is reachable through a virtual or the shared harness,
// so both chips now assert them.
// ---------------------------------------------------------------------------

TYPED_TEST(SoftPhyDriver, WaitForPacketTimeoutPreservesPreambleForGuard) {
  // poll_until_activity_()'s timeout branch calls reset_rx_state_(), which clears the whole live
  // IRQ word before listen()'s preamble_or_sync_incoming() guard can read it. The base class keeps
  // a one-shot snapshot (preamble_latched_at_timeout_) so the guard still sees a preamble latched
  // right up to the timeout, but that snapshot must not survive being read once.
  MockSpi spi;
  MockPin rst, dio1, busy(false);
  typename TestFixture::Testable radio(&spi, &rst, &dio1, &busy, TypeParam::kTxPower, TypeParam::kTcxo);

  radio.set_irq_sequence({TypeParam::preamble(), TypeParam::preamble(), 0x0000});

  RadioRxPacket result{};
  EXPECT_FALSE(radio.wait_for_packet(result, 1)) << "no RX_DONE/sync ever arrived, so this dwell must time out";
  EXPECT_TRUE(radio.is_preamble_detected())
      << "the guard must still see the preamble latched right up to the timeout, even though "
         "reset_rx_state_() has already cleared the chip's live IRQ word";
  EXPECT_FALSE(radio.is_preamble_detected())
      << "the snapshot must not survive being read once — a stale detection must not suppress "
         "hopping on a later, unrelated dwell";
}

TYPED_TEST(SoftPhyDriver, WaitForPacketCompletesOnAirTimeWithoutRxDone) {
  MockSpi spi;
  MockPin rst, dio1, busy(false);
  typename TestFixture::EarlyRx radio(&spi, &rst, &dio1, &busy, TypeParam::kTxPower, TypeParam::kTcxo);

  radio.set_rx_buffer(uart_packed_on_air_bytes(kRs100Challenge));
  // SYNC_WORD_VALID only — RX_DONE never arrives. Under the old flow this receive could only time
  // out; the frame is nonetheless entirely on air and recoverable.
  radio.set_irq_sequence({TypeParam::sync_valid()});

  RadioRxPacket pkt{};
  ASSERT_TRUE(radio.wait_for_packet(pkt, 20000)) << "a complete, CRC-valid frame must not need RX_DONE";
  EXPECT_FALSE(radio.fallback_used()) << "the RX_DONE path must not have been reached";

  ASSERT_EQ(pkt.len, kRs100Challenge.size());
  EXPECT_EQ(std::vector<uint8_t>(pkt.data, pkt.data + pkt.len), kRs100Challenge);

  ASSERT_GE(radio.read_lengths().size(), 2u) << "expected a header read then a whole-frame read";
  for (uint8_t offset : radio.read_offsets())
    EXPECT_EQ(offset, TypeParam::rx_buffer_base());
  EXPECT_EQ(radio.read_lengths().front(), SOFT_PHY_EARLY_HEADER_RAW_BYTES);
  EXPECT_EQ(radio.read_lengths().back(),
            soft_phy_raw_bytes_for_frame(kRs100Challenge.size()) + SOFT_PHY_EARLY_READ_MARGIN_BYTES);
  EXPECT_LT(radio.read_lengths().back(), SOFT_PHY_RX_PROBE_PACKET_LEN);
}

TYPED_TEST(SoftPhyDriver, WaitForPacketFallsBackToRxDoneWhenEarlyReadDoesNotValidate) {
  MockSpi spi;
  MockPin rst, dio1, busy(false);
  typename TestFixture::EarlyRx radio(&spi, &rst, &dio1, &busy, TypeParam::kTxPower, TypeParam::kTcxo);

  // A buffer that never yields a CRC-valid frame — a chip that turns out not to expose its buffer
  // mid-reception, or a spurious sync detect.
  radio.set_rx_buffer(std::vector<uint8_t>(SOFT_PHY_RX_PROBE_PACKET_LEN, 0x00));
  radio.set_irq_sequence({TypeParam::sync_valid(), TypeParam::rx_done()});

  RadioRxPacket fallback{};
  fallback.len = 4;
  fallback.data[0] = 0xDE;
  radio.set_fallback_packet(fallback);

  RadioRxPacket pkt{};
  ASSERT_TRUE(radio.wait_for_packet(pkt, 20000));
  EXPECT_TRUE(radio.fallback_used()) << "a failed early read must cost latency, never the frame";
  EXPECT_EQ(pkt.len, 4);
  EXPECT_EQ(pkt.data[0], 0xDE);
}

TYPED_TEST(SoftPhyDriver, EarlyCompletionDeclinesWindowsTooShortToFinishIn) {
  // A window smaller than the longest frame's air time cannot complete a receive either way, so
  // the early path must not spend that whole budget discovering it.
  //
  // Weaker than it reads: with this short a window, stage 1's wait_for_air_time() also bails before
  // any read_rx_buffer(), so read_lengths().empty() cannot on its own distinguish the up-front
  // `timeout_ms < SOFT_PHY_EARLY_MIN_WINDOW_MS` guard from that downstream bail. Telling them apart
  // would need the fixture to record a decline reason (a harness change out of scope here). Treat
  // this as "no buffer read happened", not as proof the up-front guard specifically fired.
  MockSpi spi;
  MockPin rst, dio1, busy(false);
  typename TestFixture::EarlyRx radio(&spi, &rst, &dio1, &busy, TypeParam::kTxPower, TypeParam::kTcxo);

  radio.set_rx_buffer(uart_packed_on_air_bytes(kRs100Challenge));
  radio.set_irq_sequence({TypeParam::sync_valid()});

  RadioRxPacket pkt{};
  EXPECT_FALSE(radio.wait_for_packet(pkt, SOFT_PHY_EARLY_MIN_WINDOW_MS - 1));
  EXPECT_TRUE(radio.read_lengths().empty()) << "no buffer read should have been attempted at all";
}

TYPED_TEST(SoftPhyDriver, CheckForPacketSyncWithoutRxDoneKeepsHopHoldoffOnFallBack) {
  MockSpi spi;
  MockPin rst, dio1, busy(false);
  typename TestFixture::EarlyRx radio(&spi, &rst, &dio1, &busy, TypeParam::kTxPower, TypeParam::kTcxo);

  radio.mark_dio_fired_from_isr();
  radio.set_rx_buffer(crc_corrupted_on_air_bytes(kRs100Challenge));
  radio.set_irq_sequence({TypeParam::sync_valid()});

  RadioRxPacket pkt{};
  EXPECT_FALSE(radio.check_for_packet(pkt));
  ASSERT_GE(radio.read_lengths().size(), 2u) << "expected a header read then a whole-frame read";
  // Without the holdoff still standing after early completion declines, nothing would distinguish
  // "fall-through is recoverable next loop() pass" from "fall-through is lossy" (issue #81 §0.3).
  //
  // This pins "still armed", not "re-armed": note_reception_in_progress_() already ran a few lines
  // earlier in check_for_packet(), and the host clock stubs never advance far enough to reach the
  // holdoff deadline, so the fall-through re-arm that refreshes a deadline try_early_completion_()
  // partly consumed is not itself exercised here — that is an elapsed-time property the +1-per-call
  // stubs cannot express.
  EXPECT_TRUE(radio.reception_in_progress())
      << "the hop holdoff armed before the early-completion attempt must survive it declining";
}

TYPED_TEST(SoftPhyDriver, CheckForPacketSyncWithoutRxDoneDeclinesWhenBudgetTooShort) {
  MockSpi spi;
  MockPin rst, dio1, busy(false);
  typename TestFixture::EarlyRx radio(&spi, &rst, &dio1, &busy, TypeParam::kTxPower, TypeParam::kTcxo);
  radio.set_idle_rx_completion_budget_ms(SOFT_PHY_EARLY_MIN_WINDOW_MS - 1);

  radio.mark_dio_fired_from_isr();
  radio.set_rx_buffer(uart_packed_on_air_bytes(kRs100Challenge));
  radio.set_irq_sequence({TypeParam::sync_valid()});

  RadioRxPacket pkt{};
  EXPECT_FALSE(radio.check_for_packet(pkt));
  EXPECT_TRUE(radio.read_lengths().empty())
      << "a budget below SOFT_PHY_EARLY_MIN_WINDOW_MS must decline before touching the buffer";
  // Same caveat as EarlyCompletionDeclinesWindowsTooShortToFinishIn: "no buffer read happened" is
  // all this pins. The up-front budget guard and stage 1's air-time bail both leave read_lengths()
  // empty; separating them needs a decline reason the fixture does not record.
}

TYPED_TEST(SoftPhyDriver, RealTwoPassReceiveArmsThenClearsHopHoldoff) {
  // The actual two-pass sequence the whole #81 fix depends on: pass 1 arms the holdoff from the
  // sync-only branch, pass 2 delivers (or tears down) via the real, non-overridden read_rx_packet()
  // — whose every return path funnels through reset_rx_state_() and clears the holdoff. The shared
  // harness's set_use_real_read_rx_packet() seam makes this reachable for both chips now.
  ScriptedSpi spi;
  MockPin rst, dio1, busy(false);
  typename TestFixture::Testable radio(&spi, &rst, &dio1, &busy, TypeParam::kTxPower, TypeParam::kTcxo);
  radio.set_use_real_read_rx_packet(true);
  radio.set_irq_sequence({TypeParam::sync_valid(), TypeParam::rx_done()});

  radio.mark_dio_fired_from_isr();
  RadioRxPacket packet{};
  EXPECT_FALSE(radio.check_for_packet(packet));
  EXPECT_TRUE(radio.reception_in_progress()) << "the sync-only pass must arm the holdoff";

  radio.mark_dio_fired_from_isr();
  (void) radio.check_for_packet(packet);
  EXPECT_FALSE(radio.reception_in_progress())
      << "the real RX_DONE pass must clear the holdoff armed by the sync-only pass";
}

TYPED_TEST(SoftPhyDriver, ReadRssiAppliesFormula) {
  // read_rssi()'s -raw/2 dBm formula lives in SoftPhyDriverBase; only the single raw byte's wire
  // read is chip-specific (GetRssiInst on both, framed differently — see queue_rssi_raw()).
  ScriptedSpi spi;
  MockPin rst, dio1, busy(false);
  typename TestFixture::Testable radio(&spi, &rst, &dio1, &busy, TypeParam::kTxPower, TypeParam::kTcxo);
  TypeParam::queue_rssi_raw(spi, 100);

  EXPECT_EQ(radio.read_rssi(), -50);
}

#include "radio_sx1262.h"
#include "radio_interface.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"

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

  // Configure the sequence of IRQ status values returned by read_irq_status_raw().
  void set_irq_sequence(std::initializer_list<uint16_t> seq) {
    irq_seq_.assign(seq);
    irq_idx_ = 0;
  }

  // Set the packet that read_rx_packet_ should return.
  void set_expected_packet(const RadioRxPacket &pkt) { expected_packet_ = pkt; }

  // Control whether the final packet read succeeds.
  void set_read_success(bool success) { read_success_ = success; }

 protected:
  uint16_t read_irq_status_raw() override {
    if (irq_idx_ < irq_seq_.size()) {
      return irq_seq_[irq_idx_++];
    }
    return 0;
  }

  bool read_rx_packet(RadioRxPacket &packet, bool blocking_wait, uint16_t irq_status) override {
    (void) blocking_wait;
    (void) irq_status;
    if (read_success_) {
      packet = expected_packet_;
      return true;
    }
    return false;
  }

 private:
  std::vector<uint16_t> irq_seq_;
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
  bool ok = radio.wait_for_packet(result, 5);  // 5ms timeout

  // No IRQ or DIO → timeout, report failure
  EXPECT_FALSE(ok) << "absence of any radio activity should cause timeout and return false";
  // result should remain empty
  EXPECT_EQ(result.len, 0u) << "on timeout, result packet length should be zero";
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
  bool ok = radio.wait_for_packet(result, 5);  // short timeout

  // SYNC without subsequent RX_DONE within timeout → failure path
  EXPECT_FALSE(ok) << "SYNC_WORD_VALID without RX_DONE before timeout should cause failure";
  EXPECT_EQ(result.len, 0u) << "on race-condition timeout, result length should be zero";
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

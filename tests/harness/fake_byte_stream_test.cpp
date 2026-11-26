/// @file
/// @brief FakeByteStream: scripted bytes, partial reads, and time that passes only on empty polls.

#include <vector>

#include <gtest/gtest.h>

#include "fake_byte_stream.h"

using esphome::test_clock::ManualClock;

TEST(FakeByteStream, ReadReturnsQueuedBytesWithoutSpendingTime) {
  ManualClock clock;
  FakeByteStream stream;
  stream.push_rx({1, 2, 3});
  uint64_t const before = esphome::test_clock::state().now_us;

  uint8_t buf[8];
  EXPECT_EQ(stream.available(), 3u);
  EXPECT_EQ(stream.read(buf, sizeof(buf)), 3u);
  EXPECT_EQ(esphome::test_clock::state().now_us, before);
  EXPECT_EQ(stream.empty_polls(), 0u);
}

TEST(FakeByteStream, EveryEmptyPollAdvancesTheClockOneTick) {
  ManualClock clock;
  FakeByteStream stream;
  uint64_t const before = esphome::test_clock::state().now_us;

  uint8_t buf[4];
  EXPECT_EQ(stream.available(), 0u);
  EXPECT_EQ(stream.read(buf, sizeof(buf)), 0u);
  EXPECT_EQ(esphome::test_clock::state().now_us, before + 2 * FakeByteStream::TICK_US);
  EXPECT_EQ(stream.empty_polls(), 2u);
}

TEST(FakeByteStream, WithTheLegacyClockAnEmptyPollJustReturns) {
  FakeByteStream stream;
  uint8_t buf[4];
  EXPECT_EQ(stream.read(buf, sizeof(buf)), 0u);
  EXPECT_EQ(stream.empty_polls(), 1u);
}

TEST(FakeByteStream, BytesPerReadSplitsWhatIsDelivered) {
  ManualClock clock;
  FakeByteStream stream;
  stream.set_bytes_per_read(2);
  stream.push_rx({1, 2, 3, 4, 5});
  uint8_t buf[8];
  EXPECT_EQ(stream.available(), 2u);
  EXPECT_EQ(stream.read(buf, sizeof(buf)), 2u);
  EXPECT_EQ(stream.read(buf, sizeof(buf)), 2u);
  EXPECT_EQ(stream.read(buf, sizeof(buf)), 1u);
  EXPECT_EQ(stream.queued(), 0u);
}

TEST(FakeByteStream, ReadHonoursTheCallersLimit) {
  ManualClock clock;
  FakeByteStream stream;
  stream.push_rx({1, 2, 3});
  uint8_t buf[2];
  EXPECT_EQ(stream.read(buf, 1), 1u);
  EXPECT_EQ(stream.queued(), 2u);
}

TEST(FakeByteStream, ThePumpRunsBeforeAPollAndAfterATick) {
  ManualClock clock;
  FakeByteStream stream;
  std::vector<uint64_t> pump_times;
  uint64_t const start = esphome::test_clock::state().now_us;
  stream.set_pump([&] {
    pump_times.push_back(esphome::test_clock::state().now_us);
    // The peer emits a byte the moment the clock passes 100 µs after the start.
    if (esphome::test_clock::state().now_us >= start + 100 && stream.queued() == 0)
      stream.push_rx({0x42});
  });
  uint8_t byte = 0;
  size_t polls = 0;
  while (stream.read(&byte, 1) == 0) {
    ASSERT_LT(++polls, 10u);
  }
  EXPECT_EQ(byte, 0x42);
  EXPECT_GE(esphome::test_clock::state().now_us - start, 100u);
  ASSERT_GE(pump_times.size(), 2u);
  EXPECT_EQ(pump_times[0], start) << "pumped before the first poll";
  EXPECT_EQ(pump_times[1], start + FakeByteStream::TICK_US) << "and again after the tick";
}

TEST(FakeByteStream, WritesAreRecordedAndForwardedToThePeer) {
  FakeByteStream stream;
  std::vector<uint8_t> seen;
  stream.set_write_handler([&](const uint8_t *data, size_t n) { seen.insert(seen.end(), data, data + n); });
  const uint8_t a[] = {1, 2};
  const uint8_t b[] = {3};
  stream.write(a, sizeof(a));
  stream.write(b, sizeof(b));
  stream.flush();
  EXPECT_EQ(stream.written(), (std::vector<uint8_t>{1, 2, 3}));
  EXPECT_EQ(seen, stream.written());
  EXPECT_EQ(stream.flush_count(), 1u);
  stream.clear_written();
  EXPECT_TRUE(stream.written().empty());
}

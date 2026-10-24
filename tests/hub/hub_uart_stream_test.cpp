/// @file
/// @brief HubUartStream over the UART stub: reads return what is buffered, writes pass through.

#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "hub_uart_stream.h"

using esphome::home_io_control::ByteStreamAccess;
using esphome::home_io_control::HubUartStream;
using esphome::uart::UARTComponent;
using esphome::uart::UARTFlushResult;

namespace {

/// A UART bus with a scripted receive buffer that, like ESPHome's, hands over all of a request
/// or nothing.
class FakeUart : public UARTComponent {
 public:
  void write_array(const uint8_t *data, size_t len) override { written.insert(written.end(), data, data + len); }
  bool read_array(uint8_t *data, size_t len) override {
    read_requests.push_back(len);
    if (len > rx.size())
      return false;
    std::memcpy(data, rx.data(), len);
    rx.erase(rx.begin(), rx.begin() + static_cast<std::ptrdiff_t>(len));
    return true;
  }
  size_t available() override { return rx.size(); }
  UARTFlushResult flush() override {
    flushes++;
    return UARTFlushResult::UART_FLUSH_RESULT_SUCCESS;
  }

  std::vector<uint8_t> rx;
  std::vector<uint8_t> written;
  std::vector<size_t> read_requests;
  int flushes{0};
};

}  // namespace

TEST(HubUartStream, ReadReturnsWhatIsBufferedAndNoMore) {
  FakeUart uart;
  HubUartStream stream;
  stream.set_uart_parent(&uart);
  uart.rx = {1, 2, 3};

  uint8_t buf[8] = {};
  EXPECT_EQ(stream.available(), 3u);
  EXPECT_EQ(stream.read(buf, sizeof(buf)), 3u) << "asks for the 3 that are there, not for 8";
  EXPECT_EQ(buf[2], 3);
  ASSERT_EQ(uart.read_requests.size(), 1u);
  EXPECT_EQ(uart.read_requests[0], 3u);
}

TEST(HubUartStream, ReadHonoursTheCallersLimit) {
  FakeUart uart;
  HubUartStream stream;
  stream.set_uart_parent(&uart);
  uart.rx = {1, 2, 3, 4, 5};

  uint8_t buf[2] = {};
  EXPECT_EQ(stream.read(buf, sizeof(buf)), 2u);
  EXPECT_EQ(uart.rx.size(), 3u);
  EXPECT_EQ(buf[0], 1);
  EXPECT_EQ(buf[1], 2);
}

TEST(HubUartStream, ReadOnAnEmptyBufferNeverTouchesTheBus) {
  FakeUart uart;
  HubUartStream stream;
  stream.set_uart_parent(&uart);
  uint8_t buf[4];
  EXPECT_EQ(stream.read(buf, sizeof(buf)), 0u);
  EXPECT_TRUE(uart.read_requests.empty());
}

TEST(HubUartStream, WriteAndFlushPassThrough) {
  FakeUart uart;
  HubUartStream stream;
  stream.set_uart_parent(&uart);
  const uint8_t out[] = {9, 8, 7};
  stream.write(out, sizeof(out));
  stream.write(out, 0);
  stream.flush();
  EXPECT_EQ(uart.written, (std::vector<uint8_t>{9, 8, 7}));
  EXPECT_EQ(uart.flushes, 1);
}

TEST(HubUartStream, WithoutABusItIsQuiet) {
  HubUartStream stream;
  EXPECT_FALSE(stream.has_uart());
  uint8_t buf[2];
  EXPECT_EQ(stream.available(), 0u);
  EXPECT_EQ(stream.read(buf, sizeof(buf)), 0u);
  stream.write(buf, sizeof(buf));
  stream.flush();
}

TEST(HubUartStream, IsUsableThroughTheByteStreamInterface) {
  FakeUart uart;
  HubUartStream stream;
  stream.set_uart_parent(&uart);
  EXPECT_TRUE(stream.has_uart());
  ByteStreamAccess &access = stream;
  uart.rx = {0x55};
  uint8_t byte = 0;
  EXPECT_EQ(access.read(&byte, 1), 1u);
  EXPECT_EQ(byte, 0x55);
}

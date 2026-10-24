/// @file
/// @brief The hub's byte stream over an ESPHome UART.
/// @ingroup hioc_hub
///
/// Adapts `uart::UARTDevice` to the `ByteStreamAccess` a serial radio driver reads and writes
/// through. This is the only file that includes ESPHome's UART headers: the radio layer stays
/// independent of the UART framework, the way the SPI drivers stay independent of the SPI one.
/// Compiled only under `IOHOME_BRIDLE`.

#pragma once

// IOHOME_BRIDLE is only visible after something pulls in esphome/core/defines.h (ESPHome codegen's
// cg.add_define() lands there, not as a compiler -D flag): this include must run before the #ifdef,
// or the file silently compiles to nothing in a real build although the host tests (which pass
// -DIOHOME_BRIDLE) are fine.
#include "esphome/core/hal.h"

#ifdef IOHOME_BRIDLE

#include "esphome/components/uart/uart.h"
#include "radio_interface.h"

namespace esphome::home_io_control {

/// @brief `ByteStreamAccess` over the UART the `uart_id:` option names.
///
/// The hub owns one and hands the UART bus to it with `set_uart_parent()` during code
/// generation. Reads never wait: ESPHome's `read_array()` is all-or-nothing, so the stream asks
/// for exactly the bytes that `available()` reports.
class HubUartStream : public ByteStreamAccess, public uart::UARTDevice {
 public:
  /// Whether a UART bus has been attached; the composition root refuses to build a driver
  /// without one.
  [[nodiscard]] bool has_uart() const { return this->parent_ != nullptr; }

  [[nodiscard]] size_t available() override;
  size_t read(uint8_t *buf, size_t max) override;
  void write(const uint8_t *buf, size_t n) override;
  void flush() override;
  /// The baud rate of the `uart:` block (1 Mbaud when no bus is attached).
  [[nodiscard]] uint32_t baud_rate() const override;
};

}  // namespace esphome::home_io_control

#endif  // IOHOME_BRIDLE

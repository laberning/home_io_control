/// @file
/// @brief The hub's byte stream over an ESPHome UART.
/// @ingroup hioc_hub

// IOHOME_BRIDLE is only visible after something pulls in esphome/core/defines.h (ESPHome codegen's
// cg.add_define() lands there, not as a compiler -D flag): this include must run before the #ifdef,
// or the file silently compiles to nothing in a real build although the host tests (which pass
// -DIOHOME_BRIDLE) are fine.
#include "esphome/core/hal.h"

#ifdef IOHOME_BRIDLE

#include "hub_uart_stream.h"

#include <algorithm>

namespace esphome::home_io_control {

size_t HubUartStream::available() { return this->has_uart() ? uart::UARTDevice::available() : 0; }

size_t HubUartStream::read(uint8_t *buf, size_t max) {
  size_t const n = std::min(this->available(), max);
  if (n == 0)
    return 0;
  return this->read_array(buf, n) ? n : 0;
}

void HubUartStream::write(const uint8_t *buf, size_t n) {
  if (this->has_uart() && n > 0)
    this->write_array(buf, n);
}

void HubUartStream::flush() {
  if (this->has_uart())
    uart::UARTDevice::flush();
}

uint32_t HubUartStream::baud_rate() const {
  return this->has_uart() ? this->parent_->get_baud_rate() : ByteStreamAccess::baud_rate();
}

}  // namespace esphome::home_io_control

#endif  // IOHOME_BRIDLE

#pragma once

#include <cstddef>
#include <cstdint>

namespace esphome {
namespace uart {

enum class UARTFlushResult {
  UART_FLUSH_RESULT_SUCCESS,
  UART_FLUSH_RESULT_TIMEOUT,
  UART_FLUSH_RESULT_FAILED,
  UART_FLUSH_RESULT_ASSUMED_SUCCESS,
};

/// The bus a UARTDevice talks through. Host tests subclass it with a scripted link.
class UARTComponent {
 public:
  virtual ~UARTComponent() = default;
  virtual void write_array(const uint8_t *data, size_t len) = 0;
  virtual bool read_array(uint8_t *data, size_t len) = 0;
  virtual size_t available() = 0;
  virtual UARTFlushResult flush() = 0;
  uint32_t get_baud_rate() const { return this->baud_rate_; }
  void set_baud_rate(uint32_t baud_rate) { this->baud_rate_ = baud_rate; }

 protected:
  uint32_t baud_rate_{1000000};
};

class UARTDevice {
 public:
  void set_uart_parent(UARTComponent *parent) { this->parent_ = parent; }
  void write_array(const uint8_t *data, size_t len) { this->parent_->write_array(data, len); }
  bool read_array(uint8_t *data, size_t len) { return this->parent_->read_array(data, len); }
  size_t available() { return this->parent_->available(); }
  UARTFlushResult flush() { return this->parent_->flush(); }

 protected:
  UARTComponent *parent_{nullptr};
};

}  // namespace uart
}  // namespace esphome

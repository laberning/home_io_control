#pragma once

#include <cstdint>
#include <string>

namespace esphome {
namespace gpio {

enum Flags : uint8_t {
  FLAG_NONE = 0x00,
  FLAG_INPUT = 0x01,
  FLAG_OUTPUT = 0x02,
  // not needed
};

enum InterruptMode {
  INTERRUPT_RISING_EDGE = 0,
  INTERRUPT_FALLING_EDGE = 1,
  INTERRUPT_ANY_EDGE = 2,
  INTERRUPT_LOW_LEVEL = 3,
  INTERRUPT_HIGH_LEVEL = 4,
};

}  // namespace gpio

class GPIOPin {
 public:
  virtual ~GPIOPin() = default;
  virtual void setup() {}
  virtual void pin_mode(gpio::Flags flags) { (void) flags; }
  virtual bool digital_read() { return false; }
  virtual void digital_write(bool value) { (void) value; }
  virtual void attach_interrupt(void (*handler)(void *), void *arg, gpio::InterruptMode mode) {
    (void) handler;
    (void) arg;
    (void) mode;
  }
  virtual bool is_internal() const { return true; }
  virtual bool is_inverted() const { return false; }
  int get_pin() const { return -1; }
};

class InternalGPIOPin : public GPIOPin {
 public:
  // Inherit constructors
  using GPIOPin::GPIOPin;
};

// Simple no-op LOG_PIN macro
#define LOG_PIN(prefix, pin) ((void) 0)
#define LOG_PIN_DRIVER(...) ((void) 0)

}  // namespace esphome

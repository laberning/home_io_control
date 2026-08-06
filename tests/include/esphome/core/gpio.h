#pragma once

#include <cstdint>
#include <string>

namespace esphome {
namespace gpio {

enum Flags : uint8_t {
  FLAG_NONE = 0x00,
  FLAG_INPUT = 0x01,
  FLAG_OUTPUT = 0x02,
  FLAG_OPEN_DRAIN = 0x04,
  FLAG_PULLUP = 0x08,
  FLAG_PULLDOWN = 0x10,
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
  // Real GPIOPin::get_flags() returns the pin's *configured* mode (from YAML, e.g.
  // pullup:/pulldown:), independent of any transient pin_mode() calls -- see MockPin
  // (radio_test_common.h) for a stub that models that distinction.
  virtual gpio::Flags get_flags() const { return gpio::FLAG_NONE; }
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
  using GPIOPin::GPIOPin;
  using GPIOPin::attach_interrupt;  // unhide base overload set

  // Typed overload mirrors esphome::InternalGPIOPin — no-op in unit tests.
  template<typename T> void attach_interrupt(void (*func)(T *), T *arg, gpio::InterruptMode type) const {}
};

// Simple no-op LOG_PIN macro
#define LOG_PIN(prefix, pin) ((void) 0)
#define LOG_PIN_DRIVER(...) ((void) 0)

}  // namespace esphome

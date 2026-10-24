#pragma once

#include <cstdint>
#include <functional>

namespace esphome {
namespace spi {

enum BitOrder { BIT_ORDER_MSB_FIRST };
enum ClockPolarity { CLOCK_POLARITY_LOW };
enum ClockPhase { CLOCK_PHASE_LEADING };
enum DataRate { DATA_RATE_8MHZ };

class SPIDeviceBase {
 public:
  void enable() {}
  void disable() {}
  uint8_t transfer_byte(uint8_t data) { return data; }
  void write_byte(uint8_t data) { (void) data; }
  uint8_t read_byte() { return 0; }
  void spi_setup() {}

 protected:
  // The real SPIClient::parent_ is null until an spi: bus is wired in; hub setup() checks it.
  void *parent_{nullptr};
};

template<BitOrder B, ClockPolarity CP, ClockPhase CH, DataRate D> class SPIDevice : public SPIDeviceBase {};

}  // namespace spi
}  // namespace esphome

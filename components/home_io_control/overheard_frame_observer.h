#pragma once

/// @file overheard_frame_observer.h
/// @brief Observer interface for parsed frames an exchange wait hears and ignores as unrelated.
/// @ingroup hioc_hub
///
/// While ExchangeEngine waits for a device's reply it still hears every other frame on the channel:
/// a wall remote's press, another controller's traffic. The wait ignores them and keeps listening,
/// but they are still frames somebody else's handling may want. An OverheardFrameObserver attached
/// to the engine is told about each one without the engine knowing who is listening or why (the
/// hub keeps the one-way ones for its remote handling, hub_core.h). It follows the same pattern as
/// TransmitObserver for the transmit side.

#include "proto_frame.h"
#include "radio_interface.h"

#include <cstdint>

namespace esphome {
namespace home_io_control {

/// @brief Receives the frames an exchange wait ignores as unrelated to its exchange.
///
/// The callback runs synchronously inside the wait, on the main loop: keep it cheap, never
/// transmit from it and never start another exchange.
class OverheardFrameObserver {
 public:
  virtual ~OverheardFrameObserver() = default;

  /// A wait heard a parsed frame that does not belong to its exchange and kept listening. The engine
  /// does not filter by protocol or sender: a one-way remote's frame and a foreign two-way frame
  /// both arrive here.
  /// @param frame    The parsed frame.
  /// @param packet   The raw packet it came from (frequency, length, bytes).
  /// @param rssi_dbm RSSI of that reception, in dBm, as the radio reported it for this packet, or
  ///                 RSSI_UNKNOWN_DBM when the radio had no level of the frame's own. The observer
  ///                 needs no radio access of its own to get it.
  /// @param now      millis() when the wait heard it.
  virtual void on_overheard_frame(const IoFrame &frame, const RadioRxPacket &packet, int16_t rssi_dbm,
                                  uint32_t now) = 0;
};

}  // namespace home_io_control
}  // namespace esphome

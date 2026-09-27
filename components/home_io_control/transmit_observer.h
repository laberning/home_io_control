#pragma once

/// @file transmit_observer.h
/// @brief Observer interface for every frame the exchange engine puts on air.
/// @ingroup hioc_hub
///
/// ExchangeEngine::transmit_frame() is the single TX choke point of the hub: 2W exchanges,
/// pairing, and 1W bursts all transmit through it. A TransmitObserver attached to the engine sees
/// each listen-before-talk deferral and each frame the radio accepted, without the engine knowing
/// who is listening or why. PairingTelemetry is the observer during a pairing attempt.

#include "proto_frame.h"
#include "radio_interface.h"

#include <cstdint>

namespace esphome {
namespace home_io_control {

/// @brief Receives transmit events from ExchangeEngine::transmit_frame().
///
/// Both callbacks default to no-ops, so an observer overrides only what it records. They run
/// synchronously inside transmit_frame(), on the main loop: keep them cheap and never transmit
/// from one.
class TransmitObserver {
 public:
  virtual ~TransmitObserver() = default;

  /// The channel was busy, so transmit_frame() backs off once before trying again.
  /// @param rssi_dbm The RSSI reading that deferred the transmit.
  virtual void on_lbt_defer(int16_t rssi_dbm) {}

  /// The radio accepted a frame for transmission (send_packet() returned true). Not called when
  /// serialization or send_packet() fails.
  /// @param frame    The logical frame that was sent.
  /// @param config   The TX shape it was sent with (frequency, preamble length).
  /// @param wire_len Serialized length in bytes as handed to the radio.
  virtual void on_transmit(const IoFrame &frame, const RadioTxConfig &config, uint8_t wire_len) {}
};

}  // namespace home_io_control
}  // namespace esphome

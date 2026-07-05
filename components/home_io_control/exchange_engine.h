#pragma once

/// @file exchange_engine.h
/// @brief Self-contained authenticated exchange engine for IO-Homecontrol 2W.
/// @ingroup hioc_hub
///
/// ExchangeEngine encapsulates the outbound authenticated exchange state
/// machine and the inbound challenge-response authentication path. It owns:
///   - `send_and_receive()` — retry loop with challenge/response support.
///   - `authenticate_request()` — verify a device-initiated command via HMAC.
///   - Transmit with LBT (listen-before-talk) and frequency hopping.
///   - Exchange debug snapshot captured on every attempt.
///
/// The engine holds double-pointer and raw-pointer references to its owner's
/// state (radio driver, node/system key, tuning config) so that changes made
/// by the hub after construction (e.g., radio driver allocation in setup(),
/// direct member writes in unit tests) are automatically visible.
///
/// Pairing flows (`hub_pairing.cpp`) call the hub's thin `transmit_frame_()`
/// and `hop_frequency_()` wrappers which delegate here, so no pairing call
/// site needs to change.

#include "hub_exchange.h"
#include "hub_decisions.h"
#include "proto_frame.h"
#include "proto_timing.h"
#include "radio_interface.h"
#include "tuning_config.h"

#include <cstdint>

namespace esphome {
namespace home_io_control {

/// @brief Authenticated exchange engine — outbound and inbound protocol flows.
///
/// All timing constants (retry count/delay, response windows) are unchanged
/// from the hub; this class is a pure refactoring move with no behavioral delta.
class ExchangeEngine {
 public:
  /// Construct the engine with double-pointer indirection into the hub's
  /// RadioDriver pointer and direct pointers to the node/key byte arrays and
  /// the tuning config. Pointers must remain valid for the lifetime of the
  /// engine (guaranteed because hub owns all referenced members).
  /// @param radio_ptr   Address of the hub's `RadioDriver *radio_` member.
  /// @param node_id     Pointer to the hub's `node_id_[NODE_ID_SIZE]` array.
  /// @param system_key  Pointer to the hub's `system_key_[AES_KEY_SIZE]` array.
  /// @param tuning      Pointer to the hub's `TuningConfig tuning_` member.
  ExchangeEngine(RadioDriver **radio_ptr, const uint8_t *node_id, const uint8_t *system_key,
                 const TuningConfig *tuning);

  // Non-copyable; the hub owns exactly one engine tied to its member addresses.
  ExchangeEngine(const ExchangeEngine &) = delete;
  ExchangeEngine &operator=(const ExchangeEngine &) = delete;

  // -------------------------------------------------------------------------
  // Core exchange API
  // -------------------------------------------------------------------------

  /// Execute an outbound authenticated exchange with retry.
  /// @param request  Frame to transmit (cmd + endpoints already filled).
  /// @param response Populated on success.
  /// @param freq     RF channel frequency (Hz).
  /// @return true if device responded within retry budget; false otherwise.
  bool send_and_receive(const IoFrame &request, IoFrame &response, uint32_t freq);

  /// Authenticate an inbound device command via 0x3C challenge / 0x3D HMAC.
  /// @param request The received inbound frame (e.g., CMD_STATUS_UPDATE).
  /// @param freq    RF channel the frame arrived on.
  /// @return true if HMAC verified; false on timeout or mismatch.
  bool authenticate_request(const IoFrame &request, uint32_t freq);

  // -------------------------------------------------------------------------
  // Infrastructure delegated from the hub
  // -------------------------------------------------------------------------

  /// Transmit a raw IoFrame with LBT and the given preamble length.
  /// @param frame    Frame to transmit.
  /// @param freq     RF frequency in Hz.
  /// @param preamble Preamble length (LONG_PREAMBLE or SHORT_PREAMBLE).
  /// @return true if the radio accepted the packet; false otherwise.
  bool transmit_frame(const IoFrame &frame, uint32_t freq, uint16_t preamble);

  /// Advance to the next IO-Homecontrol channel (CH1→CH2→CH3→CH1).
  /// Respects the protocol-defined minimum dwell time (HOP_TIME_US).
  void hop_frequency();

  /// Unconditionally hop only if the minimum dwell has elapsed.
  /// Called from the hub's `loop()` to honour passive channel scanning.
  void maybe_hop();

  /// Reset the hop-timer (called after radio init in hub setup()).
  void reset_hop_timestamp();

  // -------------------------------------------------------------------------
  // Exchange debug snapshot
  // -------------------------------------------------------------------------

  /// @brief Snapshot of the last exchange attempt for diagnostics.
  struct DebugInfo {
    const char *stage{"idle"};         ///< Last recorded stage label.
    uint8_t tries{0};                  ///< Retry count (1-based).
    uint8_t request_cmd{0};            ///< Command ID of the original request.
    bool saw_challenge{false};         ///< True if a 0x3C was seen during this exchange.
    bool capture_valid{false};         ///< True if radio capture is meaningful.
    bool capture_rx_done{false};       ///< True if RxDone IRQ fired.
    bool capture_crc_error{false};     ///< True if CRC error flagged (SX1262).
    uint32_t capture_freq_hz{0};       ///< RF frequency of the captured packet.
    uint16_t capture_irq_status{0};    ///< Raw IRQ register value.
    uint8_t capture_packet_status{0};  ///< Chip packet-status byte.
    uint8_t capture_reported_len{0};   ///< Length reported by radio packet engine.
    uint8_t capture_frame_len{0};      ///< Parsed protocol frame length.
    int16_t capture_rssi_dbm{0};       ///< RSSI of the captured packet (dBm).
  };

  /// Clear the debug snapshot and record the upcoming request command.
  void reset_debug(uint8_t request_cmd);

  /// Update the debug snapshot with the current stage and radio capture.
  void record_debug(const char *stage, uint8_t tries, bool saw_challenge);

  /// Log the debug snapshot as a WARN-level structured line.
  /// @param device_id Human-readable device identifier for the log line.
  void log_debug(const char *device_id) const;

  /// Read-only access to the current debug snapshot.
  [[nodiscard]] const DebugInfo &get_debug() const { return debug_; }

 private:
  // --- Outbound exchange step helpers --------------------------------------

  /// Transmit one request attempt and update context state on failure.
  bool transmit_request_(const IoFrame &request, uint32_t freq, uint16_t preamble,
                         exchange::OutboundExchangeContext &ctx);

  /// Block until the first response arrives or the wait window expires.
  decisions::ExchangeFirstResponseDisposition wait_for_first_response_(const IoFrame &request,
                                                                       exchange::OutboundExchangeContext &ctx);

  /// Send the 0x3D challenge response after receiving a 0x3C from the device.
  bool handle_authentication_(const IoFrame &request, uint32_t freq, exchange::OutboundExchangeContext &ctx);

  /// Block until the final authenticated response arrives or the window expires.
  decisions::ExchangeFinalResponseDisposition wait_for_final_response_(const IoFrame &request,
                                                                       exchange::OutboundExchangeContext &ctx);

  // --- Dependencies (back-references into the hub) -------------------------

  RadioDriver **radio_ptr_;     ///< Double-pointer: *radio_ptr_ is always the hub's active driver.
  const uint8_t *node_id_;      ///< Hub's node_id_[NODE_ID_SIZE] array.
  const uint8_t *system_key_;   ///< Hub's system_key_[AES_KEY_SIZE] array.
  const TuningConfig *tuning_;  ///< Hub's live TuningConfig (read on every LBT check).

  // --- Engine state --------------------------------------------------------

  uint32_t last_hop_us_{0};  ///< Timestamp of the last channel hop (µs, from micros()).
  DebugInfo debug_{};        ///< Snapshot updated throughout each exchange attempt.
};

}  // namespace home_io_control
}  // namespace esphome

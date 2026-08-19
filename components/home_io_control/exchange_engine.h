#pragma once

/// @file exchange_engine.h
/// @brief Self-contained authenticated exchange engine for IO-Homecontrol 2W.
/// @ingroup hioc_hub
///
/// ExchangeEngine encapsulates the outbound authenticated exchange state
/// machine and the inbound challenge-response authentication path. It owns:
///   - `send_and_receive()` — retry loop with challenge/response support.
///   - `authenticate_request()` — verify a device-initiated command via HMAC.
///   - `collect_broadcast_responses()` — send once, report every matching reply in a window.
///   - Transmit with LBT (listen-before-talk) and frequency hopping.
///   - Exchange debug snapshot captured on every attempt.
///
/// The engine holds double-pointer and raw-pointer references to its owner's
/// state (radio driver, node/system key, tuning config) so that changes made
/// by the hub after construction (e.g., radio driver allocation in setup(),
/// direct member writes in unit tests) are automatically visible.
///
/// All transmit and channel-hop traffic funnels through this engine: the hub's
/// thin `transmit_frame_()` / `hop_frequency_()` wrappers delegate here, and
/// `PairingEngine` holds a direct reference for its own exchanges.

#include "hub_exchange.h"
#include "hub_decisions.h"
#include "pairing_telemetry.h"
#include "proto_frame.h"
#include "proto_timing.h"
#include "radio_interface.h"
#include "tuning_config.h"

#include <cstdint>
#include <functional>

namespace esphome {
namespace home_io_control {

/// @brief Authenticated exchange engine — outbound and inbound protocol flows.
///
/// All timing constants (retry count/delay, response windows) come from
/// proto_timing.h; per-chip dwell overrides are queried from the RadioDriver.
/// @brief What an outbound exchange actually achieved.
///
/// Deliberately not a bool: "the device accepted the command" and "the device told us what
/// happened" are different facts, and some devices only ever deliver the first.
///
/// Some devices challenge a command, authenticate it, execute it, and then transmit nothing for
/// several seconds — up to a dozen — reporting via an asynchronous status update later instead of
/// closing the exchange with a synchronous reply, all well outside the exchange's own response
/// window. Other devices on the same protocol close the exchange properly with a synchronous 0x04
/// (see tests/corpus/captures/somfy_awning/exchange_open_sx1276.yaml), so the four-frame exchange
/// is real — just not universal, and a caller cannot assume either shape from the command alone.
///
/// SUCCESS_UNCONFIRMED exists so that silence after a real authentication is not treated the same
/// as a request the device may never have heard at all: the two need different retry rules (see
/// decisions::retry_after_unconfirmed_accept_is_safe()) and different reporting to the caller.
enum class ExchangeOutcome : uint8_t {
  FAILED,                 ///< No usable reply; the device may never have heard the request.
  SUCCESS_WITH_RESPONSE,  ///< Device replied; the caller's `response` frame is populated.
  SUCCESS_UNCONFIRMED,    ///< Device authenticated the request — so it received and accepted it —
                          ///< but sent no final response. `response` is NOT populated. Callers that
                          ///< need payload (key exchange) must treat this as failure; callers that
                          ///< only need "the command landed" should treat it as success. For every
                          ///< command but CMD_EXECUTE, this outcome is only returned after the full
                          ///< retry budget is spent — see retry_after_unconfirmed_accept_is_safe().
};

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
  /// @param response Populated only for @ref ExchangeOutcome::SUCCESS_WITH_RESPONSE.
  /// @param freq     RF channel frequency (Hz).
  /// @return What the device actually told us — see @ref ExchangeOutcome.
  ExchangeOutcome send_and_receive(const IoFrame &request, IoFrame &response, uint32_t freq);

  /// Authenticate an inbound device command via 0x3C challenge / 0x3D HMAC.
  /// @param request The received inbound frame (e.g., CMD_STATUS_UPDATE).
  /// @param freq    RF channel the frame arrived on.
  /// @return true if HMAC verified; false on timeout or mismatch.
  bool authenticate_request(const IoFrame &request, uint32_t freq);

  /// @brief Invoked for each matching broadcast reply, as it arrives.
  /// @param frame    Parsed reply frame (responder's address is `frame.src`).
  /// @param rssi_dbm RSSI of this reply.
  using BroadcastReplyHandler = std::function<void(const IoFrame &frame, int16_t rssi_dbm)>;

  /// Transmit `request` once and hand every matching broadcast reply to `on_reply` within
  /// `window_ms`.
  ///
  /// Unlike send_and_receive(), this neither retries the transmit nor performs any
  /// authentication — a broadcast roll-call has no per-transaction proof, so replies are
  /// informational only (see the caller's protocol notes). Every candidate packet is parsed
  /// and checked against `expected_cmd` and `node_id_` (as the frame's destination); anything
  /// that fails `parse()`, carries a different `cmd`, or is not addressed to us is ignored
  /// without ending collection. The receiver leaves the request channel before the first
  /// listen and alternates between the other two for the rest of the window — unlike
  /// wait_for_first_response_(), which holds the request channel for the whole wait — because a
  /// broadcast reply does not come back on the channel that asked for it (1 of 149 measured),
  /// while a unicast reply does. Replies on the request channel are therefore not caught by this
  /// loop.
  ///
  /// This method stores nothing and imposes no capacity: it neither buffers replies nor
  /// deduplicates them, so the same responder answering twice within one window invokes
  /// `on_reply` twice. Storage, deduplication, and any capacity limit belong to the caller,
  /// which knows how little of each reply it actually needs to keep — see
  /// `ManagementActions::scan_paired_devices()`, which decodes each frame on arrival into a
  /// compact record rather than retaining whole frames. Collection always runs to the deadline.
  /// @param request       Frame to transmit once (cmd + endpoints already filled).
  /// @param freq          RF channel frequency (Hz) for the initial transmit.
  /// @param expected_cmd  Command byte a reply must carry to be considered.
  /// @param window_ms     How long to listen after the transmit, in milliseconds. The caller
  ///                      owns this budget explicitly (rather than this method reading
  ///                      `tuning_->pairing_discovery_wait_ms` itself) because a caller that
  ///                      transmits more than once needs to divide one total time budget across
  ///                      several calls — see `ManagementActions::scan_paired_devices()`.
  /// @param on_reply      Invoked once per matching reply, before the next packet is awaited, so
  ///                      the handler must be cheap and must not block. Keep captures to a few
  ///                      pointers: small callables avoid std::function's heap fallback on the
  ///                      implementations this project builds against.
  /// @return Number of matching replies handed to `on_reply` (duplicates included).
  uint8_t collect_broadcast_responses(const IoFrame &request, uint32_t freq, uint8_t expected_cmd, uint32_t window_ms,
                                      const BroadcastReplyHandler &on_reply);

  /// @brief The one listen primitive every radio wait loop in this project is built on.
  ///
  /// Listens for up to `spec.window_ms`, applying `spec.policy` (hold the current channel, rotate
  /// all three, or rotate skipping the request channel), and hands every packet the radio
  /// delivers to `on_frame` before deciding whether to keep waiting. See @ref ListenPolicy for the
  /// measurements behind each policy and @ref ListenSpec for what each field controls.
  ///
  /// Parses each received packet into `frame`, so on ListenOutcome::ACCEPTED the caller's `frame`
  /// already holds the accepted frame and `packet` already holds its raw bytes — no copy is
  /// needed. A packet that fails to parse is still handed to `on_frame` (with a null `parsed`
  /// pointer), so a caller that wants to log or count unparsable frames still can.
  ///
  /// Any richer result than accept/refuse/timeout — a disposition with more than three values, a
  /// captured "did we see any traffic at all" flag — is the caller's business: capture it in
  /// `on_frame`'s closure and return ACCEPT/IGNORE. `ListenOutcome` itself never grows a fourth
  /// value; that is how a shared primitive would turn back into one loop per caller.
  ///
  /// @param spec    How this listen window is to be spent.
  /// @param packet  Scratch space for the whole listen: holds the last received packet on
  ///   return. On ACCEPTED that is the accepted packet; on ABORTED, the one `on_frame` refused;
  ///   on TIMED_OUT, whatever arrived last (or the caller's initial value, if nothing did).
  /// @param frame   Same lifetime as `packet`, parsed from it: holds the last received frame on
  ///   return, with the same ACCEPTED/ABORTED/TIMED_OUT correspondence as `packet` above.
  /// @param on_frame Invoked for every packet the radio delivers; decides whether to accept,
  ///   abort, or keep listening. See @ref ReplyHandler.
  /// @return ACCEPTED or ABORTED as `on_frame` decided, or TIMED_OUT if `spec.window_ms` elapsed
  ///   first.
  ListenOutcome listen(const ListenSpec &spec, RadioRxPacket &packet, IoFrame &frame, const ReplyHandler &on_frame);

  // -------------------------------------------------------------------------
  // Infrastructure delegated from the hub
  // -------------------------------------------------------------------------

  /// Transmit a raw IoFrame with LBT and the given preamble length.
  /// @param frame    Frame to transmit.
  /// @param freq     RF frequency in Hz.
  /// @param preamble Preamble length (LONG_PREAMBLE or SHORT_PREAMBLE).
  /// @return true if the radio accepted the packet; false otherwise.
  bool transmit_frame(const IoFrame &frame, uint32_t freq, uint16_t preamble);

  /// @brief Advance the receiver one step along the protocol's channel rotation
  ///   (CH1→CH2→CH3→CH1).
  /// @param skip_freq Channel to pass over, or 0 to rotate through all three. Used by the
  ///   broadcast roll-call, whose replies never come back on the channel that asked.
  void hop_frequency(uint32_t skip_freq = 0);

  /// Unconditionally hop only if the minimum dwell has elapsed.
  /// Called from the hub's `loop()` to honour passive channel scanning.
  void maybe_hop();

  /// Reset the hop-timer (called after radio init in hub setup()).
  void reset_hop_timestamp();

  // -------------------------------------------------------------------------
  // Pairing telemetry hook
  // -------------------------------------------------------------------------

  /// Attach a telemetry recorder so transmit_frame()'s LBT loop records defer events.
  /// Set by PairingEngine for the duration of a `discover_and_pair()` attempt only — nullptr
  /// (the default) for every non-pairing exchange, which is the common case and stays a no-op.
  /// @param telemetry Non-owning pointer, or nullptr to detach.
  void set_pairing_telemetry(PairingTelemetry *telemetry) { this->pairing_telemetry_ = telemetry; }

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
    bool capture_crc_error{false};     ///< True if CRC error flagged (chip-dependent; see RadioCaptureInfo::crc_error).
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
  // --- listen() helper -------------------------------------------------------

  /// Retune per `skip` (see hop_frequency()) and fire `spec.on_hop` if set. Factored out of
  /// listen() purely to keep that function's cognitive complexity under the clang-tidy
  /// threshold — a member function call doesn't add to the caller's complexity the way an
  /// inline lambda definition does.
  /// @param skip Channel to pass over, or 0 to rotate through all three — see hop_frequency().
  /// @param spec The listen this hop belongs to; only `on_hop` is read.
  void listen_hop_(uint32_t skip, const ListenSpec &spec);

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

  RadioDriver **radio_ptr_;                       ///< Double-pointer: *radio_ptr_ is always the hub's active driver.
  const uint8_t *node_id_;                        ///< Hub's node_id_[NODE_ID_SIZE] array.
  const uint8_t *system_key_;                     ///< Hub's system_key_[AES_KEY_SIZE] array.
  const TuningConfig *tuning_;                    ///< Hub's live TuningConfig (read on every LBT check).
  PairingTelemetry *pairing_telemetry_{nullptr};  ///< Set only during a pairing attempt; see set_pairing_telemetry().

  // --- Engine state --------------------------------------------------------

  uint32_t last_hop_us_{0};  ///< Timestamp of the last channel hop (µs, from micros()).
  DebugInfo debug_{};        ///< Snapshot updated throughout each exchange attempt.
};

}  // namespace home_io_control
}  // namespace esphome

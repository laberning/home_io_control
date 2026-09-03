#pragma once

/// @file key_extraction_responder.h
/// @brief "Recover System Key" (key extraction) — device-role responder collaborator.
/// @ingroup hioc_hub
///
/// The impure side of the key-extraction feature: arming/disarming, throwaway node-ID generation,
/// the 10-minute auto-off timer, the post-extraction grace window, transmitting device-role
/// replies, and the security-sensitive result log block. The pure state-transition decisions live
/// in pairing_responder.h/.cpp (unchanged); this collaborator owns a
/// pairing_responder::ResponderContext and dispatches the six RX branches through
/// try_handle_frame(), called from process_received_packet_() (hub_status.cpp).

#include "hub_hooks.h"
#include "pairing_responder.h"
#include "proto_frame.h"

#include "esphome/core/hal.h"  // millis() for the inline awaiting_reply()

#include <cstdint>
#include <functional>

namespace esphome {
namespace home_io_control {

// Forward declarations — full definitions included only in key_extraction_responder.cpp.
struct TuningConfig;
class RadioDriver;
class DeviceRegistry;

/// @brief Device-role responder for the "Recover System Key" feature.
///
/// Constructed once by IOHomeControlComponent; non-copyable because it is wired with injected
/// callbacks and references into hub member addresses (mirrors ManagementActions / ExchangeEngine).
/// @ingroup hioc_hub
class KeyExtractionResponder {
 public:
  /// @param node_id           Hub's real 3-byte node ID (throwaway-ID collision check).
  /// @param radio             Double pointer to the hub's active radio driver, so a test's
  ///                          `comp.radio_ = &mock` propagates (mirrors ExchangeEngine).
  /// @param tuning            Runtime tuning config, owned by the hub
  ///                          (`cold_broadcast_reply_preamble`).
  /// @param registry          Device registry, for the throwaway-ID collision check only.
  /// @param transmit          How to put a reply frame on air (see TransmitFrameFn).
  /// @param schedule_auto_off Named-timeout scheduler for the 10-minute arm window and the
  ///                          post-extraction grace window (see NamedTimeoutFn).
  KeyExtractionResponder(const uint8_t *node_id, RadioDriver **radio, const TuningConfig *tuning,
                         DeviceRegistry &registry, TransmitFrameFn transmit, NamedTimeoutFn schedule_auto_off);

  /// Non-copyable — holds injected callbacks and references into hub member addresses.
  KeyExtractionResponder(const KeyExtractionResponder &) = delete;
  KeyExtractionResponder &operator=(const KeyExtractionResponder &) = delete;

  /// @brief Arm or disarm the "Recover System Key" (key extraction) responder.
  ///
  /// Arming picks a fresh throwaway node ID, resets the pairing_responder state machine to
  /// ARMED_IDLE, and schedules a 10-minute auto-off. While armed, the 0x28/0x2C/0x31/0x32 branches
  /// in process_received_packet_() emulate an unpaired device so a user's existing hub can pair to
  /// it and hand over its node_id/system_key (see pairing_responder.h). Disarming — manual, via
  /// the HA switch, on successful extraction, or on auto-off — immediately stops those branches
  /// from responding; it never touches the real device registry or the hub's own node_id_/
  /// system_key_. This is the body that was IOHomeControlComponent::set_key_extraction_armed().
  /// @param armed Desired state.
  void set_armed(bool armed);

  /// Register a callback invoked whenever the key-extraction armed state changes — manual
  /// toggle, successful extraction, or auto-off timeout — so the switch entity can keep its
  /// displayed state in sync when the responder disarms itself rather than the user. Single-slot.
  /// @param cb Callable receiving the new armed state.
  void set_armed_callback(std::function<void(bool)> cb) { this->armed_callback_ = std::move(cb); }

  /// Dispatch a frame to the responder if it's one of its 0x28/0x2C/0x31/0x32/0x36/0x3C frames and
  /// the responder is armed. Kept a separate function (rather than inlined into
  /// process_received_packet_()) purely to keep that function's cognitive complexity under the
  /// clang-tidy threshold, mirroring PairingEngine::record_discovery_rx_telemetry_()'s reason for
  /// existing.
  /// @param frame Parsed inbound frame.
  /// @return true if the frame was handled (caller should stop further dispatch for it).
  [[nodiscard]] bool try_handle_frame(const IoFrame &frame);

  /// Generate a random throwaway node ID for one key-extraction arm cycle, avoiding collisions
  /// with the broadcast addresses, this hub's own real node ID, and any registered device.
  /// @param out Output: 3-byte node ID.
  void generate_throwaway_id(uint8_t out[NODE_ID_SIZE]);

  /// (Re)arm the post-extraction grace window that replaces the old immediate disarm-on-extraction:
  /// called once when the key is first recovered, and again on every sign of hub progress after
  /// that (an inbound 0x36, an outbound 0x3D) so a slow multi-retry hub isn't cut off mid-round.
  /// Uses the same named-timer replace-on-reschedule idiom as the 10-minute auto-off timer — see
  /// key_extraction_responder.cpp for why a naive "only disarm if DISARMED" guard inside the
  /// callback is not enough once a manual disarm-and-rearm can happen inside the window.
  void arm_post_extraction_grace();

  /// True whenever the responder has replied at least once, is waiting on the hub's next step, and
  /// that wait is still within its bounded hold window — i.e. `key_extraction_ctx_.state` is
  /// neither DISARMED (feature unused) nor ARMED_IDLE (armed, but no discovery request seen yet),
  /// AND `key_extraction_hold_deadline_ms_` has not yet passed. loop() uses this to hold CH2
  /// instead of running the generic idle-hop scan, and to defer background status polls, while an
  /// attempt is in flight.
  ///
  /// The deadline is a plain timestamp, not a named `set_timeout()` timer: releasing the CH2 hold
  /// is purely a radio-scheduling optimization (see key_extraction_hold_deadline_ms_'s own doc
  /// comment for why it is deliberately decoupled from `key_extraction_ctx_.state` itself), so
  /// nothing needs to fire a callback when it lapses — the next loop() iteration simply stops
  /// taking the CH2-hold branch on its own. Default-constructed, the deadline is 0, which is always
  /// in the past relative to any real `millis()` reading once the device has been running — so a
  /// mid-exchange state reached without the deadline having been (re)set (e.g. a reply-builder
  /// failure between the state guard and the deadline update) safely never holds CH2, rather than
  /// holding it unboundedly.
  ///
  /// Defined inline: defer_background_poll_() calls this every loop() iteration.
  [[nodiscard]] bool awaiting_reply() const {
    return this->key_extraction_ctx_.state != pairing_responder::ResponderState::DISARMED &&
           this->key_extraction_ctx_.state != pairing_responder::ResponderState::ARMED_IDLE &&
           millis() < this->key_extraction_hold_deadline_ms_;
  }

  /// State for the current "Accept Foreign Pairing" (key-extraction) arm cycle; DISARMED by
  /// default so a fresh boot never responds to foreign pairing traffic. See pairing_responder.h.
  /// Public so the host tests that script individual RX branches can preset and inspect it.
  pairing_responder::ResponderContext key_extraction_ctx_;
  /// Deadline (millis()) until which loop() should hold CH2 for the key-extraction responder,
  /// deliberately independent of `key_extraction_ctx_.state` itself. Set (not "armed" — this is a
  /// plain timestamp, not a named `set_timeout()` timer) on every sign of hub progress: the three
  /// pre-extraction reply handlers (handle_discover_(), handle_discover_confirm_(),
  /// handle_key_init_(), key_extraction_responder.cpp) push it out by
  /// KEY_EXTRACTION_MID_ATTEMPT_TIMEOUT_MS, and arm_post_extraction_grace() pushes it out by
  /// KEY_EXTRACTION_POST_EXTRACT_GRACE_MS so the hold also covers the (much longer)
  /// post-extraction address-verification phase it governs.
  ///
  /// Deliberately independent of `key_extraction_ctx_.state`: the pure guards in
  /// pairing_responder.cpp decide whether an inbound frame is accepted by checking `state` alone,
  /// never this deadline, so a real (slower) hub's next frame still completes the exchange
  /// correctly even if it arrives after the hold has expired. Coupling the two — letting the
  /// deadline also force `state` back to ARMED_IDLE — would silently discard that live protocol
  /// progress instead of just releasing the radio hold. See awaiting_reply()'s doc comment for how
  /// this is consumed.
  uint32_t key_extraction_hold_deadline_ms_{0};

 private:
  /// Handle an inbound CMD_DISCOVER_REQ (0x28) while the responder is armed.
  void handle_discover_(const IoFrame &frame);
  /// Handle an inbound CMD_DISCOVER_CONFIRM (0x2C) addressed to our throwaway node ID while armed.
  void handle_discover_confirm_(const IoFrame &frame);
  /// Handle an inbound CMD_KEY_INIT (0x31) addressed to our throwaway node ID while armed.
  void handle_key_init_(const IoFrame &frame);
  /// Handle an inbound CMD_KEY_TRANSFER (0x32) addressed to our throwaway node ID while armed.
  void handle_key_transfer_(const IoFrame &frame);
  /// Handle an inbound CMD_ADDRESS_REQ (0x36) addressed to our throwaway node ID while armed.
  /// Some hubs (Velux KLR200) send this after completing the key exchange, to verify the backbone
  /// address they were given — see pairing_responder::on_address_req().
  void handle_address_req_(const IoFrame &frame);
  /// Handle an inbound CMD_CHALLENGE_REQ (0x3C) addressed to our throwaway node ID while armed and
  /// in SENT_ADDRESS_RESP — the hub-issued challenge against our own CMD_ADDRESS_RESP, closing the
  /// address-verification round a CMD_ADDRESS_REQ opened. See
  /// pairing_responder::on_address_challenge().
  void handle_address_challenge_(const IoFrame &frame);
  /// Transmit a key-extraction reply frame on all 3 IO-homecontrol channels, using the radio
  /// driver's response_preamble() rather than a fixed SHORT_PREAMBLE/LONG_PREAMBLE constant —
  /// long enough that a channel-hopping receiver reliably lands on it, short enough that 3
  /// sequential transmissions don't block the main loop for the better part of a second (see the
  /// implementation comment in key_extraction_responder.cpp for the hardware-confirmed reasoning).
  /// Shared by every RX handler so the preamble choice and channel list are defined once.
  void broadcast_reply_(const IoFrame &frame);
  /// Emit the security-sensitive "system key extracted" log block (see redaction.h — this is the
  /// one deliberate, explicit exception to that file's masking, not a loosening of it).
  void log_result_();

  const uint8_t *node_id_;
  RadioDriver **radio_;
  const TuningConfig *tuning_;
  DeviceRegistry &registry_;
  TransmitFrameFn transmit_;
  NamedTimeoutFn schedule_auto_off_;
  std::function<void(bool)> armed_callback_;
};

}  // namespace home_io_control
}  // namespace esphome

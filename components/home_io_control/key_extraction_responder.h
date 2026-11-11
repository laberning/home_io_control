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
#include "proto_commands.h"
#include "proto_constants.h"
#include "proto_frame.h"
#include "proto_sizes.h"
#include "proto_timing.h"

#include "esphome/core/hal.h"  // millis() for the inline awaiting_reply()

#include <cstdint>
#include <functional>
#include <string>

namespace esphome {
namespace home_io_control {

/// How long after receiving a frame it answers the responder stays on that frame's channel: the
/// window a device keeps a session open for the controller's next frame (DEVICE_SESSION_WAIT_BASE_MS
/// plus the turnaround time the responder advertises in its Multi Information Byte). The
/// peer's in-session frame (the `0x32` after our `0x3C`, the `0x3C` after our `0x37`) comes on that
/// channel within it, with a short preamble. After it the responder scans all three channels again,
/// as a device does once its session ends: the peer's next *request* is a new transmission that may
/// go out on any channel.
/// \todo Measure on hardware how soon a third-party hub's in-session frame follows our reply.
/// @param multi_info The Multi Information Byte the responder advertises.
/// @return The session window in milliseconds.
static constexpr uint32_t key_extraction_session_window_ms(uint8_t multi_info) {
  return DEVICE_SESSION_WAIT_BASE_MS + att_class_ms(discovery_att_class(multi_info));
}

/// The session window for the default Multi Information Byte (KEY_EXTRACTION_DISCOVER_RESP_FLAGS).
static constexpr uint32_t KEY_EXTRACTION_SESSION_WINDOW_MS =
    key_extraction_session_window_ms(KEY_EXTRACTION_DISCOVER_RESP_FLAGS);

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
  /// @param rx_freq_hz Channel the frame was received on (RadioRxPacket::freq_hz); 0 if unknown,
  ///                   which counts as CH2. Answering the frame makes it the session channel.
  /// @return true if the frame was handled (caller should stop further dispatch for it).
  [[nodiscard]] bool try_handle_frame(const IoFrame &frame, uint32_t rx_freq_hz = 0);

  /// Channel of the session the responder last replied in: the channel the frame it answered came
  /// in on, CH2 until it has answered anything. Replies go out on it (the `0x29` ends on it), and
  /// loop() stays on it while in_session_hold().
  [[nodiscard]] uint32_t session_freq() const { return this->session_freq_; }

  /// True while awaiting_reply() and the frame the responder last answered was received less than
  /// the session window ago (key_extraction_session_window_ms() of the byte the last discovery answer
  /// advertised): loop() then stays on session_freq() for the peer's in-session frame. Once false
  /// (while still awaiting a reply), loop() scans all three channels.
  [[nodiscard]] bool in_session_hold() const {
    return this->awaiting_reply() && (millis() - this->session_rx_ms_) < this->session_window_ms_;
  }

  /// Session window for the Multi Information Byte the last discovery answer advertised.
  [[nodiscard]] uint32_t session_window_ms() const { return this->session_window_ms_; }

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
  /// AND `key_extraction_hold_deadline_ms_` has not yet passed. While it is true, loop() defers
  /// background status polls and, instead of the generic idle hop, listens for the peer's next frame:
  /// on session_freq() during in_session_hold(), then with a rotating listen over all three channels.
  ///
  /// The deadline is a plain timestamp, not a named `set_timeout()` timer: ending this listen is
  /// purely a radio-scheduling optimization (see key_extraction_hold_deadline_ms_'s own doc
  /// comment for why it is deliberately decoupled from `key_extraction_ctx_.state` itself), so
  /// nothing needs to fire a callback when it lapses — the next loop() iteration simply stops
  /// taking the responder-listen branch on its own. Default-constructed, the deadline is 0, which
  /// is always in the past relative to any real `millis()` reading once the device has been running — so a
  /// mid-exchange state reached without the deadline having been (re)set (e.g. a reply-builder
  /// failure between the state guard and the deadline update) safely never takes the radio, rather
  /// than holding it unboundedly.
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
  /// Deadline (millis()) until which loop() listens for the key-extraction responder's peer,
  /// deliberately independent of `key_extraction_ctx_.state` itself. Set (not "armed" — this is a
  /// plain timestamp, not a named `set_timeout()` timer) on every sign of hub progress: the three
  /// pre-extraction reply handlers (handle_discover_(), handle_discover_confirm_(),
  /// handle_key_init_(), key_extraction_responder.cpp) push it out by
  /// KEY_EXTRACTION_MID_ATTEMPT_TIMEOUT_MS, and arm_post_extraction_grace() pushes it out by
  /// KEY_EXTRACTION_POST_EXTRACT_GRACE_MS so the hold also covers the (much longer)
  /// post-extraction node-verification phase it governs.
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
  /// Advertise the type word from `key_extraction_type_word` (the answer to an open discovery).
  void set_configured_type_();
  /// Handle an inbound CMD_DISCOVER_CONFIRM (0x2C) addressed to our throwaway node ID while armed.
  void handle_discover_confirm_(const IoFrame &frame);
  /// Handle an inbound CMD_KEY_INIT (0x31) addressed to our throwaway node ID while armed.
  void handle_key_init_(const IoFrame &frame);
  /// Handle an inbound CMD_KEY_TRANSFER (0x32) addressed to our throwaway node ID while armed.
  void handle_key_transfer_(const IoFrame &frame);
  /// Handle an inbound CMD_NODE_VERIFY_REQ (0x36) addressed to our throwaway node ID while armed.
  /// Some hubs (Velux KLR200) send this after completing the key exchange, to verify the backbone
  /// address they were given — see pairing_responder::on_node_verify_req().
  void handle_node_verify_req_(const IoFrame &frame);
  /// Handle an inbound CMD_CHALLENGE_REQ (0x3C) addressed to our throwaway node ID while armed and
  /// in SENT_NODE_VERIFY_RESP — the hub-issued challenge against our own CMD_NODE_VERIFY_RESP, closing the
  /// node-verification round a CMD_NODE_VERIFY_REQ opened. See
  /// pairing_responder::on_node_verify_challenge().
  void handle_node_verify_challenge_(const IoFrame &frame);
  /// Transmit a key-extraction reply to a frame addressed to us and open the session hold
  /// (session_freq(), in_session_hold()). It goes out once, on the channel that frame came in on,
  /// with the driver's response_preamble().
  void send_reply_(const IoFrame &frame);
  /// Answer a broadcast search the way a device does: once, in a random slot from 1 to N of the
  /// search's window (discovery_slot_count()), on a random one of the two channels the search did
  /// not come in on, with `cold_broadcast_reply_preamble`. Timed through the named-timeout
  /// scheduler, so the loop is never blocked while the slot runs down. The answer is dropped if
  /// the attempt has moved on (disarm, re-arm) before its slot comes.
  /// \todo Confirm on hardware that third-party controllers (TaHoma, KIG 300, KLR 200, Freebox)
  /// accept a slotted answer.
  void schedule_discover_reply_(const IoFrame &search, const IoFrame &resp);
  /// Emit the security-sensitive "system key extracted" log block (see redaction.h — this is the
  /// one deliberate, explicit exception to that file's masking, not a loosening of it).
  void log_result_();

  const uint8_t *node_id_;
  RadioDriver **radio_;
  const TuningConfig *tuning_;
  /// Channel of the frame try_handle_frame() is dispatching, so the handler that answers it can
  /// make that channel the session's (send_reply_()).
  uint32_t rx_freq_hz_{FREQ_CH2};
  /// millis() when try_handle_frame() received that frame.
  uint32_t rx_ms_{0};
  /// Channel of the session the responder last replied in; see session_freq().
  uint32_t session_freq_{FREQ_CH2};
  /// millis() when the frame the responder last answered was received; see in_session_hold().
  uint32_t session_rx_ms_{0};
  /// Session window for the Multi Information Byte the last discovery answer advertised.
  uint32_t session_window_ms_{KEY_EXTRACTION_SESSION_WINDOW_MS};
  /// The `0x29` waiting for its slot (schedule_discover_reply_()).
  IoFrame pending_discover_resp_{};
  DeviceRegistry &registry_;
  TransmitFrameFn transmit_;
  NamedTimeoutFn schedule_auto_off_;
  std::function<void(bool)> armed_callback_;
};

namespace detail {

/// @brief Build the ready-to-paste 2W system-key-extraction report: `node_id:`/`system_key:` as a
/// `home_io_control:` YAML block.
///
/// Pure — takes already-decoded values, performs no I/O — so it is directly unit-testable without
/// a live radio, mirroring build_oneway_adoption_report() (oneway_key_adoption.h); the two features
/// end up sharing report *structure* as well as format_key_hex(). The caller
/// (KeyExtractionResponder::log_result_()) logs the result through log_multiline_result() and
/// nowhere else — this is the single intentional place the recovered `system_key` is formatted for
/// display, a deliberate exception to redaction.h's masking.
///
/// The emitted keys must track the hub's own `CONFIG_SCHEMA` (`__init__.py`) by hand. `make
/// yaml-emitter-sync` (scripts/check-yaml-emitters.py) catches drift between the two by
/// cross-referencing this function's emitted key names against that schema statically.
/// @param node_id Recovered hub node_id, 3 bytes.
/// @param key Recovered system key, 16 bytes.
/// @return Multi-line report text, ready to pass to log_multiline_result().
std::string build_key_extraction_report(const uint8_t node_id[NODE_ID_SIZE], const uint8_t key[AES_KEY_SIZE]);

}  // namespace detail

}  // namespace home_io_control
}  // namespace esphome

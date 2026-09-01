#pragma once

/// @file oneway_transmitter.h
/// @brief One-way (1W) transmit collaborator.
/// @ingroup hioc_hub
///
/// The third object that drives the radio, alongside ExchangeEngine and PairingEngine (ADR 0004)
/// — and the only one that awaits nothing. A 1W command has no reply, no challenge and no
/// acknowledgement: the frame goes out and that is the whole interaction. Everything this class
/// does follows from that, most of all the repetition, which is the only reliability mechanism
/// available when nothing can report a miss.

#include "oneway_controller.h"
#include "oneway_sequence_store.h"
#include "proto_device_model.h"
#include "proto_frame.h"

#include <cstdint>
#include <functional>
#include <string>

namespace esphome {
namespace home_io_control {

/// @brief How the transmitter puts a frame on air.
///
/// Injected rather than taken as a collaborator reference so this class depends on the *ability*
/// to transmit rather than on whichever object currently owns the radio. The hub wires it to its
/// own `transmit_frame_`; a test wires it to a recorder and needs no radio at all.
/// @param frame Frame to serialize and transmit.
/// @param freq RF channel frequency in Hz.
/// @param preamble Preamble length in bytes.
/// @return true if the frame reached the radio.
using OneWayTransmitFn = std::function<bool(const IoFrame &frame, uint32_t freq, uint16_t preamble)>;

/// @brief What a 1W command attempt did — the only feedback this feature can ever produce.
///
/// 1W has no reply, so nothing here says a device acted; it says what the hub transmitted. That is
/// the half the hub can know, and without it a user with a wrong key, a desynced counter or a
/// missing enrollment sees nothing at all.
/// @ingroup hioc_hub
struct OneWayCommandReport {
  std::string controller_id;                    ///< Identity that transmitted (empty if unresolved).
  std::string intent;                           ///< Decoded intent, e.g. "STOP" or "CLOSE".
  DeviceType target_type{DeviceType::UNKNOWN};  ///< Device class addressed.
  uint16_t sequence{0};                         ///< Sequence consumed; meaningless unless sequence_reserved.
  bool sequence_reserved{false};                ///< True if a sequence was consumed (0 is a valid sequence).
  bool transmitted{false};                      ///< True if at least one copy reached the radio.
};

/// @brief Invoked once per attempted 1W command, successful or not.
using OneWayCommandReportFn = std::function<void(const OneWayCommandReport &report)>;

/// @brief Sends 1W commands as the repeated bursts real remotes send.
/// @ingroup hioc_hub
class OneWayTransmitter {
 public:
  /// @param transmit How to put a frame on air; must stay valid for this object's lifetime.
  explicit OneWayTransmitter(OneWayTransmitFn transmit) : transmit_(std::move(transmit)) {}

  // === Controller identities ===

  /// @brief Register a configured controller identity. Called once per `oneway_controllers:` entry.
  ///
  /// Config only — it does not touch persistent storage, because generated wiring runs before
  /// preferences are usable. setup() is what opens each identity's counter.
  /// @param identity Fully-resolved identity (address and key already decided at schema time).
  void add_identity(const OneWayControllerIdentity &identity) { this->identities_.add(identity); }

  /// @brief Open each registered identity's persistent sequence counter.
  /// Call once from the hub's `setup()`, never from generated wiring.
  void setup();

  /// @return The configured controller identities.
  [[nodiscard]] const OneWayControllerRegistry &identities() const { return this->identities_; }

  /// @brief Register the callback that receives a report after every command attempt.
  /// @param callback Invoked once per logical command, including failed ones — a command that
  ///        never left the hub is exactly the case a user needs to see, and 1W will not tell them.
  void set_command_report_callback(OneWayCommandReportFn callback) { this->report_ = std::move(callback); }

  // === Commands ===

  /// @brief Send a named command as the identity's controller.
  ///
  /// Resolves the identity, reserves exactly one sequence for the whole command, builds and signs
  /// the frame with that identity's key, and bursts it.
  ///
  /// **Addresses a device class, not a device.** Every device of `io_device_type` in range that
  /// holds the signing key acts on it — that is what 1W is, not a limitation to work around. Two
  /// devices of one class are separable only if they can be given separate identities.
  /// @param controller_id YAML handle of the controller identity to transmit as.
  /// @param cmd Named command (STOP, FAVORITE, VENT). CoverCommand::FORCE_OPEN has no 1W
  ///        encoding and cannot be built — see create_1w_execute_command() (proto_commands.h).
  /// @return true if at least one copy reached the radio; false if the identity is unknown, the
  ///         sequence could not be reserved, or the frame could not be built.
  bool send_command(const std::string &controller_id, CoverCommand cmd);

  /// @brief Send a numeric position as the identity's controller.
  ///
  /// Same contract as send_command(). Every position 0–100 is ordinary; none is a special code.
  /// @param controller_id YAML handle of the controller identity to transmit as.
  /// @param position Target position 0–100 (0 = fully open, 100 = fully closed).
  /// @return true if at least one copy reached the radio.
  bool send_position(const std::string &controller_id, uint8_t position);

  /// @brief Transmit one already-built, already-signed 1W frame as a burst.
  ///
  /// Sends the frame ONEWAY_BURST_REPEATS times, ONEWAY_BURST_INTERVAL_MS apart, on FREQ_CH2 with
  /// LONG_PREAMBLE — the cadence real remotes use (proto_timing.h).
  ///
  /// **It retransmits identical bytes.** The sequence and the MAC were fixed by the caller before
  /// this was called, and all copies must carry them unchanged: a device treats one sequence as
  /// one command, so four copies bearing four sequences are four commands, of which it will
  /// accept one and reject three as replays. This function therefore never rebuilds a frame,
  /// never touches a sequence counter, and takes the frame by const reference so it cannot.
  ///
  /// **It blocks for the whole burst**, feeding the watchdog in the gaps. The three inter-copy
  /// gaps alone are 3 * ONEWAY_BURST_INTERVAL_MS = ~120 ms of pure delay; add each of the four
  /// copies' own airtime and the wall-clock total this function blocks for is closer to ~160 ms
  /// (proto_timing.h's ONEWAY_BURST_INTERVAL_MS comment has the same two numbers). Per ADR 0013
  /// all radio work happens on the ESPHome loop and the operation queue is the concurrency model;
  /// an authenticated 2W exchange already blocks far longer than this. Scheduling the repeats
  /// through a timeout would add a second concurrency model and would let a queued 2W exchange
  /// interleave between copies of one command.
  /// @param frame Signed 1W frame to send.
  /// @return true if at least one copy reached the radio. Partial success is still reported as
  ///         success because it is genuinely what the caller wants to know — with no reply frame,
  ///         "some copies went out" is the most any layer here can ever establish, and a device
  ///         needs only one of them.
  bool send_burst(const IoFrame &frame);

  /// @brief Register this identity as a controller on every device of its class currently in
  /// association mode: `0x39` (remove, self-directed) immediately followed by `0x30` (add, no MAC
  /// trailer) — the documented 1W pairing handshake.
  ///
  /// **The `0x30` half's MAC trailer is configurable** via the identity's `enrollment_with_mac:`
  /// (default `false`, i.e. no MAC at all — not "MAC inline", there is no inline form for this
  /// command, see create_1w_add_controller()'s `@warning`, proto_commands.h). Most real hardware
  /// captures this project holds carry no MAC, but a real Izymo has separately been shown to
  /// accept the MAC-bearing form too (the published documentation vector's own shape) — untested
  /// manufacturers may need either.
  ///
  /// **Sends `0x39` then `0x30`, back to back, one burst each** —
  /// `reference/iown-homecontrol/docs/linklayer.md:396`'s "1W Discovery" sequence diagram, matched
  /// by a real Smoove capture landing the two 128 ms apart within one gesture
  /// (`tests/corpus/captures/enrollment/somfy_smoove_enrollment_add_and_remove_controller_sx1276.yaml`; see also
  /// `analysis/completed/oneway_1w_support_plan.md` Step 13). This `0x39` carries only this
  /// identity's own `src` address — nothing on the wire lets it name a different controller — so
  /// it can only clear this identity's own prior entry, never someone else's; ADR 0026's
  /// additive-registration property is unaffected by sending it here.
  /// @param controller_id YAML handle of the controller identity to register.
  /// @return true if the `0x30` half reached the radio (the credential that actually registers
  ///         this identity). A failed `0x39` prelude only logs a warning and does not block it.
  bool send_enrollment(const std::string &controller_id);

  /// @brief Un-register this identity from every device of its class currently in association
  /// mode (CMD 0x39) alone — also the prelude send_enrollment() fires before its own `0x30`.
  ///
  /// Reachable directly through the explicitly-named `oneway_remove_controller` native API
  /// action, for un-enrolling without immediately re-enrolling.
  ///
  /// @warning **Unconfirmed standalone on real hardware.** Firing `0x39` alone (outside the
  /// enrollment handshake) has had no observable effect on this project's test hardware; the
  /// leading hypothesis is that it needs the same association-mode window enrollment does. See
  /// ADR 0026 § Consequences.
  /// @param controller_id YAML handle of the controller identity to remove.
  /// @return true if at least one copy reached the radio.
  bool send_unenrollment(const std::string &controller_id);

 private:
  /// Shared tail of send_command()/send_position()/send_enrollment()/send_unenrollment(): reserve
  /// one sequence, then burst whatever `build` makes of it. The reservation happens **once per
  /// logical command** and outside the burst loop — a sequence per frame would turn one press into
  /// four commands, of which a device accepts one and rejects three.
  /// @param explicit_intent Overrides the report's decoded intent (decode_1w_frame() cannot label
  ///        a 0x30/0x39, so send_enrollment()/send_unenrollment() pass "ENROLL"/"UNENROLL" here;
  ///        empty means "derive from the built frame as usual", every other caller's behavior).
  bool send_(const std::string &controller_id,
             const std::function<bool(IoFrame &, const OneWayControllerIdentity &, uint16_t)> &build,
             const char *explicit_intent = "");

  /// Emit a report for an attempt that never got as far as a frame.
  void report_failure_(const std::string &controller_id, uint16_t sequence, bool sequence_reserved);

  OneWayTransmitFn transmit_;
  OneWayCommandReportFn report_;
  OneWayControllerRegistry identities_;
  OneWaySequenceStore sequences_;
};

}  // namespace home_io_control
}  // namespace esphome

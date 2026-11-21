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
#include "proto_codecs.h"
#include "proto_device_kinds.h"
#include "proto_frame.h"
#include "tuning_config.h"

#include <array>
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

/// @brief Turns a preamble duration (ms) into programmed bytes on the active radio
/// (RadioDriver::preamble_bytes_for_ms()); the transmitter has no radio of its own.
using OneWayPreambleBytesFn = std::function<uint16_t(float ms)>;

/// @brief The inverse of OneWayPreambleBytesFn (RadioDriver::preamble_ms_for_bytes()): how long a
/// programmed preamble lasts on the active radio. Only the TX log of the legacy shape's fixed
/// `LONG_PREAMBLE` needs it.
using OneWayPreambleMsFn = std::function<float(uint16_t bytes)>;

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
  /// The frame that went out, decoded as a receiver would read it (wire values: an inverted kind's
  /// mirrored position included). `has_intent` is false for enrollment traffic, a failure and a frame
  /// carrying no command; the hub follows a transmitted report only when it is true (ADR 0116).
  OneWayFrameInfo wire{};
};

/// @brief Invoked once per attempted 1W command, successful or not.
using OneWayCommandReportFn = std::function<void(const OneWayCommandReport &report)>;

/// @brief The two preamble durations of a burst and the radio's conversions for them.
///
/// Built per burst from the live tuning (never cached), so a change of `start_preamble_ms` or
/// `low_power_wake_ms` applies to the next command.
/// @ingroup hioc_hub
struct OneWayPreambleRules {
  float start_ms;                      ///< `start_preamble_ms`: a normal copy.
  float wake_ms;                       ///< `low_power_wake_ms`: the wake-up copy of a `LOW_POWER` identity.
  OneWayPreambleBytesFn bytes_for_ms;  ///< Duration to programmed bytes on the active radio.
  OneWayPreambleMsFn ms_for_bytes;     ///< Programmed bytes to duration on the active radio.
};

/// @brief What one copy of a burst transmits with: the programmed length and its duration.
/// @ingroup hioc_hub
struct OneWayCopyPreamble {
  uint16_t bytes;  ///< Programmed preamble length handed to the transmit callback.
  float ms;        ///< The same preamble as a duration, for the TX log.
};

/// @brief Resolve one copy's shape (oneway_burst_copy_shape()) to the preamble it transmits with.
///
/// The one place the shape-to-preamble mapping is written (ADR 0115): a `NORMAL` copy lasts the
/// start preamble, the wake-up copy of a `LOW_POWER` identity lasts `low_power_wake_ms`, both through
/// the radio's own conversion so they last the same on every radio. Only the wake-up copies of the
/// `LEGACY_LONG` shape keep a fixed programmed length (`LONG_PREAMBLE`), which keeps that shape's
/// air time per radio unchanged. A duration taken from `low_power_wake_ms` always converts to at
/// least `LONG_PREAMBLE` bytes (the setting's floor), so every wake-up copy is also labelled a
/// wake-up (is_wake_preamble()). send_burst(), format_oneway_preamble_list() and the tests all go
/// through this function, so the burst, its log line and anything asserting on them cannot resolve
/// the mapping differently.
/// @param shape A copy's shape, from oneway_burst_copy_shape().
/// @param power_class The identity's power class the shape came from.
/// @param rules Current durations and the radio's conversions.
/// @return The copy's programmed length and duration.
/// @ingroup hioc_hub
OneWayCopyPreamble oneway_copy_preamble(OneWayCopyShape shape, OneWayPowerClass power_class,
                                        const OneWayPreambleRules &rules);

/// @brief Render the preamble duration of each copy of a burst in milliseconds, e.g. `"515/10/10/10"`.
///
/// Milliseconds rather than programmed bytes, so a report reads the same on every board (a byte is
/// 8 or 10 bit periods depending on the radio). Rounded to a tenth of a millisecond, with the decimal
/// left out of a whole value (`"515/3.5/3.5/3.5"`, `"213.3/213.3/213.3/213.3"`). Built from
/// oneway_burst_copy_shape() and oneway_copy_preamble() -- the exact functions send_burst() itself
/// calls per copy -- so the TX log line can never disagree with what went on air. Pure and
/// free-standing so it is unit-testable: the host ESP_LOG stub discards its arguments, so a
/// formatter buried in the log call could not be asserted on at all.
/// @param power_class The identity's power class.
/// @param rules Current durations and the radio's conversions.
/// @return Slash-separated durations in ms, one per copy, in burst order.
/// @ingroup hioc_hub
std::string format_oneway_preamble_list(OneWayPowerClass power_class, const OneWayPreambleRules &rules);

/// @brief Sends 1W commands as the repeated bursts real remotes send.
/// @ingroup hioc_hub
class OneWayTransmitter {
 public:
  /// @param transmit How to put a frame on air; must stay valid for this object's lifetime.
  /// @param tuning Hub's live TuningConfig; must outlive this object. Read per burst (never
  ///        cached) so a live change to `start_preamble_ms` (the Home Assistant number entity)
  ///        takes effect on the next command without a reboot -- the same pattern `ExchangeEngine`
  ///        already uses for the identical field.
  /// @param preamble_bytes Converts a preamble duration (`start_preamble_ms`, `low_power_wake_ms`) to
  ///        bytes on the active radio; must stay valid for this object's lifetime.
  /// @param preamble_ms The inverse conversion, for the TX log of the legacy shape's fixed
  ///        `LONG_PREAMBLE` copies; must stay valid for this object's lifetime.
  OneWayTransmitter(OneWayTransmitFn transmit, const TuningConfig *tuning, OneWayPreambleBytesFn preamble_bytes,
                    OneWayPreambleMsFn preamble_ms)
      : transmit_(std::move(transmit)),
        preamble_bytes_(std::move(preamble_bytes)),
        preamble_ms_(std::move(preamble_ms)),
        tuning_(tuning) {}

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
  /// has enrolled this identity acts on it, and only those: a device ignores a frame from a source
  /// it was not taught, however well it is signed. Two devices of one class are separable only if
  /// they can be given separate identities.
  /// @param controller_id YAML handle of the controller identity to transmit as.
  /// @param cmd Named command (STOP, FAVORITE, VENT). CoverCommand::FORCE_OPEN has no 1W
  ///        encoding and cannot be built — see create_1w_execute_command() (proto_commands.h).
  /// @return true if at least one copy reached the radio; false if the identity is unknown, the
  ///         sequence could not be reserved, or the frame could not be built.
  bool send_command(const std::string &controller_id, CoverCommand cmd);

  /// @brief Send a numeric position as the identity's controller.
  ///
  /// Same contract as send_command(). Every position 0–100 is ordinary; none is a special code.
  /// The position is a **user position**: for an identity where effective_inverted() is true the
  /// wire value is mirrored (`100 - position`), while the report and the TX log still name the
  /// user's intent (OPEN stays OPEN).
  /// @param controller_id YAML handle of the controller identity to transmit as.
  /// @param position Target position 0–100 (0 = fully open, 100 = fully closed), before any mirroring.
  /// @return true if at least one copy reached the radio.
  bool send_position(const std::string &controller_id, uint8_t position);

  /// @brief Transmit one already-built, already-signed 1W frame as a burst.
  ///
  /// Sends the frame ONEWAY_BURST_REPEATS times, ONEWAY_BURST_INTERVAL_MS apart, on FREQ_CH2.
  /// `power_class` decides each copy's preamble and CTRL1 via oneway_burst_copy_shape() (ADR 0038);
  /// a required parameter, never defaulted — a default here would silently reintroduce a
  /// hard-coded preamble at a new call site, exactly the mistake ADR 0029 records.
  ///
  /// **It retransmits identical bytes, except CTRL1 and the preamble.** The sequence and the MAC
  /// were fixed by the caller before this was called, and every copy carries them unchanged: a
  /// device treats one sequence as one command, so four copies bearing four sequences are four
  /// commands, of which it will accept one and reject three as replays. This function therefore
  /// never rebuilds the frame's cmd/data/sequence/MAC and never touches a sequence counter — it
  /// only sets or clears `CTRL1_LOW_POWER` per copy (clearing it too, whatever the builder
  /// produced: the transmitter owns this bit unconditionally) and picks that copy's preamble. This
  /// is safe because the 1W MAC span covers only cmd+data (never CTRL1) and the CRC is computed
  /// per transmission by the driver, so neither authenticates or depends on CTRL1.
  ///
  /// **It blocks for the whole burst**, feeding the watchdog in the gaps. The three inter-copy
  /// gaps alone are 3 * ONEWAY_BURST_INTERVAL_MS = ~120 ms of pure delay; how much airtime the
  /// four copies themselves add depends on `power_class` and, for `LEGACY_LONG`, on the radio, because
  /// the same `LONG_PREAMBLE` byte count lasts about 213 ms on an SX1262 or LR1121 and about 267 ms
  /// on an SX1276: `LEGACY_LONG` puts it on every copy, ≈1.0 s per burst on an SX1262 or LR1121 and
  /// ≈1.2 s on an SX1276 (the latter measured in issue #74's logs); `ALWAYS_ALIVE` puts the live
  /// `start_preamble_ms` on every copy, ≈200 ms per burst (measured on SX1276, issue #74);
  /// `LOW_POWER` sits between the two (one wake-up copy of `low_power_wake_ms`, three normal), ≈0.72 s
  /// with the default 515 ms wake-up on every radio (ADR 0115, ADR 0118). Per ADR 0013 all radio work happens on the
  /// ESPHome loop and the operation queue is the concurrency model; an authenticated 2W exchange already blocks far
  /// longer than this. Scheduling the repeats through a timeout would add a second concurrency model and would let a
  /// queued 2W exchange interleave between copies of one command.
  /// @param frame Signed 1W frame to send.
  /// @param power_class Which preamble/CTRL1 shape each copy gets (ADR 0038).
  /// @param intent_label Optional label for the TX log line when the frame's own decoded intent
  ///        would mislead (a mirrored position). nullptr = log the decoded intent.
  /// @return true if at least one copy reached the radio. Partial success is still reported as
  ///         success because it is genuinely what the caller wants to know — with no reply frame,
  ///         "some copies went out" is the most any layer here can ever establish, and a device
  ///         needs only one of them.
  bool send_burst(const IoFrame &frame, OneWayPowerClass power_class, const char *intent_label = nullptr);

  /// @brief Register this identity as a controller on every device currently in association mode
  /// (the receiver's own association-mode gesture, ADR 0026: a multi-second PROG hold on a Somfy
  /// actuator, or a ~1 s Gear press on an already-registered VELUX control followed by the
  /// product's own ready sequence), using the gesture its
  /// manufacturer expects (`resolve_oneway_wire_profile()`, ADR 0032).
  ///
  /// **`EnrollGesture::SOMFY`** (somfy / unset / any unprofiled vendor): `0x39` (remove,
  /// self-directed) then `0x30` (add) — the 1W handshake real remotes send, both to the all-devices
  /// address, one burst each, matched by a real Smoove capture landing the two 128 ms apart
  /// (`tests/corpus/captures/enrollment/somfy_smoove_enrollment_add_and_remove_controller_sx1276.yaml`).
  ///
  /// **`EnrollGesture::VELUX_KLI`** (manufacturer velux): `0x39` to the all-devices address, then
  /// a `0x30` burst to **each** class in `effective_enrollment_classes()` under one shared
  /// sequence, then a STOP and a DOWN EXECUTE to the all-devices address at the VELUX ACEI — the
  /// KLI manual's STOP-then-DOWN registration completion. Matches the issue #74 KLI 310 capture
  /// and has enrolled a VELUX SML roller shutter and KLI 312 interior blinds on
  /// real hardware (the closing DOWN is the visible success signal, unless the cover already sits
  /// fully closed). The STOP+DOWN frames themselves
  /// are not matched against a VELUX capture
  /// (`tests/corpus/captures/enrollment/synthetic_enrollment_velux_kli_prog_sweep.yaml`).
  ///
  /// **The `0x30`'s MAC trailer** is configurable via `enrollment_with_mac:` (default `false`, no
  /// MAC — see create_1w_add_controller()'s `@warning`). Real VELUX (#74) and real Somfy captures
  /// both use the no-MAC form, and no real device has been shown to accept the MAC-bearing one. The option needs an
  /// SX1262 or LR1121: the SX1276 cannot send the trailer, so the build rejects it there (ADR 0113).
  ///
  /// **Blocks for the whole gesture** feeding the watchdog in the gaps. The VELUX path is 6 bursts
  /// (`0x39` + 3-class `0x30` sweep + STOP + DOWN); with `LEGACY_LONG` each burst carries
  /// `LONG_PREAMBLE` on every copy, ≈6–7 s total (SX1276 logs in issue #74 measured ~7.4 s); ~1.2 s with
  /// `low_power: false` (ADR 0038, measured on SX1276 in issue #74). This is a user-initiated,
  /// once-per-device action, the same shape as the pairing button (`pairing_discovery_wait_ms` →
  /// 5000).
  /// @param controller_id YAML handle of the controller identity to register.
  /// @return true if the credential frame(s) that actually register this identity reached the
  ///         radio — the `0x30` (SOMFY) or the sweep (VELUX_KLI). The VELUX STOP+DOWN follow-up is
  ///         skipped entirely if the sweep transmitted nothing; a failed `0x39` prelude, or a
  ///         partial STOP/DOWN after a good sweep, only logs and does not flip this.
  bool send_enrollment(const std::string &controller_id);

  /// @brief Un-register this identity from every device currently in association mode, addressed
  /// to the all-devices address (CMD 0x39) alone — also the prelude send_enrollment() fires before its own `0x30`.
  ///
  /// Reachable directly through the explicitly-named `oneway_remove_controller` native API
  /// action, for un-enrolling without immediately re-enrolling.
  ///
  /// @warning **Works only inside the receiver's programming window.** Confirmed on a Somfy Izymo
  /// dimmer: sent while the Izymo was in its PROG association window, the Izymo blinked and then
  /// ignored this identity's commands; sent with no window open, it changed nothing
  /// (`tests/corpus/captures/enrollment/somfy_izymo_dimmer_enrollment_remove_controller_in_prog_window_sx1276.yaml`).
  /// Other receivers are untested. See ADR 0026 § Consequences.
  /// @param controller_id YAML handle of the controller identity to remove.
  /// @return true if at least one copy reached the radio.
  bool send_unenrollment(const std::string &controller_id);

 private:
  /// Shared tail of send_command()/send_position()/send_enrollment()/send_unenrollment(): reserve
  /// one sequence, then burst whatever `build` makes of it. The reservation happens **once per
  /// logical command** and outside the burst loop — a sequence per frame would turn one press into
  /// four commands, of which a device accepts one and rejects three.
  /// @param explicit_intent Overrides the report's decoded intent and the TX log's (decode_1w_frame()
  ///        cannot label a 0x30/0x39, so send_enrollment()/send_unenrollment() pass "ENROLL"/"UNENROLL"
  ///        here; send_position() passes the user's intent because the frame carries the mirrored
  ///        wire value). Empty means "derive from the built frame as usual".
  /// @param enrollment True for the frames of an enrollment gesture: the report then carries no wire
  ///        intent, so nothing the hub follows reacts to them.
  bool send_(const std::string &controller_id,
             const std::function<bool(IoFrame &, const OneWayControllerIdentity &, uint16_t)> &build,
             const char *explicit_intent = "", bool enrollment = false);

  /// send_enrollment()'s two gestures, split so each stays simple. The dispatcher resolves the
  /// identity once and hands it down.
  bool send_somfy_enrollment_(const OneWayControllerIdentity &identity);
  bool send_velux_kli_enrollment_(const OneWayControllerIdentity &identity);

  /// Reserve **one** sequence, then 0x30-enroll to each non-UNKNOWN class in `classes` under that
  /// one sequence — the VELUX class sweep a real KLI remote sends. One report for the whole sweep.
  /// @return true if at least one class's burst reached the radio.
  bool send_enroll_sweep_(const OneWayControllerIdentity &identity, const std::array<DeviceType, 3> &classes);

  /// The one place a OneWayCommandReport is built and fired (no-op without a callback). Every
  /// report — success, sweep, or failure — goes through here so a new field on the struct, or a
  /// change to how a field is chosen, lands in exactly one spot.
  void report_attempt_(const std::string &controller_id, const std::string &intent, DeviceType target_type,
                       uint16_t sequence, bool sequence_reserved, bool transmitted, const OneWayFrameInfo &wire = {});

  /// Emit a report for an attempt that never got as far as a frame.
  void report_failure_(const std::string &controller_id, uint16_t sequence, bool sequence_reserved);

  OneWayTransmitFn transmit_;
  OneWayPreambleBytesFn preamble_bytes_;
  OneWayPreambleMsFn preamble_ms_;
  OneWayCommandReportFn report_;
  OneWayControllerRegistry identities_;
  OneWaySequenceStore sequences_;
  const TuningConfig *tuning_;  ///< Hub's live TuningConfig; read per burst, never cached.
};

}  // namespace home_io_control
}  // namespace esphome

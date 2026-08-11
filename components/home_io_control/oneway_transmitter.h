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
  uint16_t sequence{0};                         ///< Sequence consumed; 0 if none was reserved.
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

  /// @return The sequence store, for diagnostics and the resync path.
  [[nodiscard]] OneWaySequenceStore &sequences() { return this->sequences_; }

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
  /// @param cmd Named command (STOP, FAVORITE, VENT, FORCE_OPEN).
  /// @return true if at least one copy reached the radio; false if the identity is unknown, the
  ///         sequence could not be reserved, or the frame could not be built.
  bool send_command(const std::string &controller_id, CoverCommand cmd);

  /// @brief Send a numeric position as the identity's controller.
  ///
  /// Same contract as send_command(). Note that position 50 is indistinguishable from FORCE_OPEN
  /// on the wire — see create_1w_execute_position() (proto_commands.h).
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
  /// **It blocks for the whole ~120 ms**, feeding the watchdog in the gaps. Per ADR 0013 all radio
  /// work happens on the ESPHome loop and the operation queue is the concurrency model; an
  /// authenticated 2W exchange already blocks far longer than this. Scheduling the repeats through
  /// a timeout would add a second concurrency model and would let a queued 2W exchange interleave
  /// between copies of one command.
  /// @param frame Signed 1W frame to send.
  /// @return true if at least one copy reached the radio. Partial success is still reported as
  ///         success because it is genuinely what the caller wants to know — with no reply frame,
  ///         "some copies went out" is the most any layer here can ever establish, and a device
  ///         needs only one of them.
  bool send_burst(const IoFrame &frame);

 private:
  /// Shared tail of send_command()/send_position(): reserve one sequence, then burst whatever
  /// `build` makes of it. The reservation happens **once per logical command** and outside the
  /// burst loop — a sequence per frame would turn one press into four commands, of which a device
  /// accepts one and rejects three.
  bool send_(const std::string &controller_id,
             const std::function<bool(IoFrame &, const OneWayControllerIdentity &, uint16_t)> &build);

  /// Emit a report for an attempt that never got as far as a frame.
  void report_failure_(const std::string &controller_id, uint16_t sequence);

  OneWayTransmitFn transmit_;
  OneWayCommandReportFn report_;
  OneWayControllerRegistry identities_;
  OneWaySequenceStore sequences_;
};

}  // namespace home_io_control
}  // namespace esphome

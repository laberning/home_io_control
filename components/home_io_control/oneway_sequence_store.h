#pragma once

/// @file oneway_sequence_store.h
/// @brief Persistent rolling-sequence counters for one-way (1W) transmit.
/// @ingroup hioc_hub
///
/// 1W has no reply, no challenge and no acknowledgement. Its entire replay defence is a rolling
/// 16-bit sequence carried in every frame and mixed into the authenticator's IV: a device
/// remembers the highest sequence it has accepted from each transmitter and rejects anything at
/// or below it. That makes this counter the one piece of state the hub cannot afford to lose,
/// and the reason it is the only thing the component writes to persistent storage — see ADR 0025,
/// which records that exception to ADR 0018 and draws its boundary.
///
/// The failure is asymmetric, and every rule below follows from it:
///
/// - **Skipping sequences is safe.** A device accepts a forward jump within its acceptance
///   window, so a counter that runs ahead costs nothing.
/// - **Reusing one is not.** The device rejects it as a replay.
/// - **Falling behind is unrecoverable without intervention.** Every later command is rejected
///   too, permanently, and *silently* — 1W emits no error, so a hub with a stale counter keeps
///   transmitting well-formed, correctly-signed frames that nothing acts on.
///
/// This class is the only place in the component that increments or persists a sequence.

#include "proto_sizes.h"

#include "esphome/core/preferences.h"

#include <cstdint>
#include <vector>

namespace esphome {
namespace home_io_control {

// === Sequence-safety bounds ===

/// How far ahead of its stored high-water mark a device will still accept a jump. 1000 is the
/// window the reference receiver applies (`reference/iohomecontrol`), and it is the ceiling every
/// forward skip in this file has to stay under: a jump past it fails exactly like a stale
/// counter, and just as silently.
static constexpr uint16_t ONEWAY_SEQUENCE_ACCEPTANCE_WINDOW = 1000;

/// How many sequences one flash write reserves.
///
/// Persisting on every command would write flash on every button press. Reserving a block
/// instead trades that for a bounded forward skip: an unclean reboot forfeits the block's unused
/// remainder, which is the safe direction. Small on purpose — see the static_assert below for
/// the bound, and raise it only if that bound still holds.
static constexpr uint16_t ONEWAY_SEQUENCE_STRIDE = 8;

/// How many consecutive unclean reboots the stride must survive while staying inside a device's
/// acceptance window. Eight is far past plausible; the point is headroom, not precision.
static constexpr uint16_t ONEWAY_SEQUENCE_REBOOT_HEADROOM = 8;

static_assert(ONEWAY_SEQUENCE_STRIDE * ONEWAY_SEQUENCE_REBOOT_HEADROOM < ONEWAY_SEQUENCE_ACCEPTANCE_WINDOW,
              "the stride must let many unclean reboots in a row still land inside a device's "
              "acceptance window — a larger stride buys fewer flash writes and pays in silent desync");

// === The store ===

/// @brief Per-controller-identity rolling sequence counters, persisted across reboots.
/// @ingroup hioc_hub
///
/// Keyed on the transmitting node address, not on the command: real capture logs show one remote
/// running a single counter across command types (0x01 → 0x00 → 0x20 at 2416 → 2417 → 2418), so
/// a per-command counter would not match what devices track.
///
/// One logical command consumes exactly one sequence. next() is called once, by whatever builds
/// the command — never inside a repeat loop. A 4-frame burst carrying four different sequences
/// is not one command to a device, and burns counter space four times as fast.
class OneWaySequenceStore {
 public:
  /// @brief Register a controller identity and load its persisted counter.
  ///
  /// Call once per identity at setup. With nothing persisted the counter starts at
  /// `initial_sequence`; otherwise it resumes from whichever of the two is *higher*. That keeps
  /// `initial_sequence` usable as the day-one remedy for a desynced device — raise it, reflash,
  /// and the counter jumps ahead — while making it structurally unable to drag a live counter
  /// backwards into replay territory. Use seed() when moving backwards is what you actually mean.
  /// @param node_id 3-byte source address this identity transmits as.
  /// @param initial_sequence Value to start from when nothing is persisted, or to jump forward to.
  void add_identity(const uint8_t node_id[NODE_ID_SIZE], uint16_t initial_sequence);

  /// @brief Reserve and return the next sequence for an identity.
  ///
  /// Durably reserves before returning: when the in-RAM block is exhausted this persists the end
  /// of the next block *and syncs it to flash* before handing anything back, so a caller can never
  /// transmit a value the hub has not already committed to never reusing. A crash between the
  /// write and the transmit costs one skipped sequence; the reverse ordering would risk a reuse.
  /// @param node_id Identity's 3-byte source address.
  /// @param out Output: the sequence to transmit.
  /// @return false if the address is not a registered identity, or if the reservation could not
  ///         be persisted — in both cases nothing is handed out and nothing may be transmitted.
  bool next(const uint8_t node_id[NODE_ID_SIZE], uint16_t &out);

  /// @brief Force an identity's counter to a specific value and persist it immediately.
  ///
  /// Unlike add_identity(), this *may* move the counter backwards, which is the whole point —
  /// re-seed from a sequence observed on air, or from a user's estimate, when a counter has
  /// desynced from the device. Today's documented remedy for a desynced counter is
  /// `initial_sequence:` (docs/home_io_control.md troubleshooting), which goes through
  /// add_identity() at boot instead.
  /// @param node_id Identity's 3-byte source address.
  /// @param value Next sequence to hand out.
  /// @return false if the address is not registered or the write could not be persisted.
  bool seed(const uint8_t node_id[NODE_ID_SIZE], uint16_t value);

  /// @brief The next sequence this identity would hand out, without reserving it.
  ///
  /// Unlike next(), this does not commit to transmitting the value it returns — it does not
  /// reserve, persist, or advance the counter.
  /// @param node_id Identity's 3-byte source address.
  /// @param out Output: the sequence next() would return.
  /// @return false if the address is not a registered identity.
  bool peek(const uint8_t node_id[NODE_ID_SIZE], uint16_t &out) const;

 private:
  /// One identity's counter: the value to hand out next, the end of the block already reserved
  /// in flash, and the preference backing it.
  struct Counter {
    uint8_t node_id[NODE_ID_SIZE]{};
    uint16_t next{0};      ///< Next value to hand out.
    uint16_t reserved{0};  ///< First value NOT covered by the persisted reservation.
    ESPPreferenceObject pref;
  };

  Counter *find_(const uint8_t node_id[NODE_ID_SIZE]);
  const Counter *find_(const uint8_t node_id[NODE_ID_SIZE]) const;

  std::vector<Counter> counters_;
};

}  // namespace home_io_control
}  // namespace esphome

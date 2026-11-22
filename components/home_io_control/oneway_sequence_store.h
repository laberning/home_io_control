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
/// - **Skipping sequences is safe.** A device accepts a forward jump (a Somfy receiver any higher
///   number, ADR 0120), so a counter that runs ahead by a little costs nothing.
/// - **Reusing one is not.** The device rejects it as a replay.
/// - **Falling behind is unrecoverable without intervention.** Every later command is rejected
///   too, permanently, and *silently* — 1W emits no error, so a hub with a stale counter keeps
///   transmitting well-formed, correctly-signed frames that nothing acts on.
///
/// This class is the only place in the component that increments or persists a sequence.

#include "proto_sizes.h"

#include "esphome/core/preferences.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace esphome {
namespace home_io_control {

// === Sequence-safety bounds ===

/// The hub's own ceiling for every automatic forward skip in this file: stride times reboot headroom must stay
/// below it (see the static_assert). It is a deliberately small bound, not a device's window.
///
/// The measured rule, from a bench run on one Somfy Izymo: the device obeys a one-way command
/// with any higher sequence number, compared as a plain number (tested up to +32000), and ignores
/// the same number, a lower one and any step across the 16-bit wrap (65535 to 0). So a forward
/// jump is not limited by a window on that device, and an identity whose counter wraps is lost
/// to it (ADR 0120). Other receivers (VELUX in particular) are not measured; the small bound keeps
/// the hub's own skips safe if one of them accepts only a short step ahead.
/// \todo Measure a VELUX receiver.
static constexpr uint16_t ONEWAY_SEQUENCE_MAX_AUTOMATIC_SKIP = 100;

/// How many sequences one flash write reserves.
///
/// Persisting on every command would write flash on every button press. Reserving a block
/// instead trades that for a bounded forward skip: an unclean reboot forfeits the block's unused
/// remainder, which is the safe direction. Small on purpose — see the static_assert below for
/// the bound, and raise it only if that bound still holds.
static constexpr uint16_t ONEWAY_SEQUENCE_STRIDE = 8;

/// How many consecutive unclean reboots the stride must survive while staying below
/// `ONEWAY_SEQUENCE_MAX_AUTOMATIC_SKIP`. Eight is far past plausible; the point is headroom, not precision.
static constexpr uint16_t ONEWAY_SEQUENCE_REBOOT_HEADROOM = 8;

/// Half the 16-bit sequence range: the largest forward step (exclusive) `initial_sequence` may make at boot.
static constexpr uint16_t ONEWAY_SEQUENCE_HALF_RING = 0x8000;

/// Largest sequence number; the counter wraps to 0 after it.
static constexpr uint16_t ONEWAY_SEQUENCE_MAX = 0xFFFF;

static_assert(ONEWAY_SEQUENCE_STRIDE * ONEWAY_SEQUENCE_REBOOT_HEADROOM < ONEWAY_SEQUENCE_MAX_AUTOMATIC_SKIP,
              "the stride must let many unclean reboots in a row still stay below the hub's skip "
              "ceiling — a larger stride buys fewer flash writes and risks silent desync on a strict receiver");

// === Boot rule ===

/// @brief Where a one-way counter resumes at boot (ADR 0114).
///
/// `initial_sequence` (the YAML value) wins when nothing is persisted, or when it is above the persisted value by
/// less than half the 16-bit range: `persisted < initial_sequence < persisted + 0x8000`, compared as plain numbers.
/// In every other case the persisted value stands. Forward is the only direction YAML can move a counter.
///
/// The comparison is deliberately not ring arithmetic. On the ring a small `initial_sequence` is "ahead" of every
/// persisted value more than half the range above it, so the default `initial_sequence: 0` would reset any counter
/// past 32768 at the next boot. As plain numbers it never can; and a counter that has wrapped past 65535 to a small
/// value is not dragged back to an `initial_sequence` half the range or more above it.
/// \todo A wrapped counter below `initial_sequence` (by less than half the range) still jumps forward to it once at
/// the next boot. A Somfy receiver ignores every number after a wrap anyway (ADR 0120); other receivers are not
/// measured. Telling such a wrapped counter apart from a deliberate YAML edit needs state the store does not keep yet.
/// @param persisted The persisted counter, or `std::nullopt` when nothing is persisted for the identity.
/// @param initial_sequence The identity's configured start value.
/// @return The value the counter starts from.
constexpr uint16_t resolve_boot_sequence(std::optional<uint16_t> persisted, uint16_t initial_sequence) {
  if (!persisted.has_value())
    return initial_sequence;
  const bool above = initial_sequence > *persisted;
  const bool within_half = initial_sequence - *persisted < ONEWAY_SEQUENCE_HALF_RING;
  return (above && within_half) ? initial_sequence : *persisted;
}

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
  /// Call once per identity at setup. The start value is chosen by resolve_boot_sequence() (ADR 0114):
  /// `initial_sequence` when nothing is persisted or when it is above the persisted value by less than half
  /// the 16-bit range, the persisted value otherwise. That keeps `initial_sequence` usable as the day-one
  /// remedy for a desynced device — raise it, reflash, and the counter jumps ahead — while making it
  /// structurally unable to drag a live counter backwards into replay territory, including a counter that
  /// has wrapped past 65535. Use seed() when moving backwards is what you actually mean.
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
  /// desynced from the device. The native API action `oneway_set_sequence` reaches it through the
  /// transmitter; `initial_sequence:` is the YAML form and goes through add_identity() at boot instead.
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

#pragma once

/// @file oneway_receiver.h
/// @brief State and rules for frames overheard from one-way (1W) remotes: burst window, duplicate
/// filter, pairing-gesture sighting and the sender allowlist.
/// @ingroup hioc_hub
///
/// Holds no hub pointer and calls nothing: the hub feeds it each overheard frame with a timestamp and
/// the radio's RSSI and asks it the questions it needs answered. What stays with the hub is what needs
/// the device registry, the poll policy or the API: following a linked remote and firing the sender
/// event. The pure rules this class applies live in decisions_oneway.h.

#include "decisions_oneway.h"
#include "pairing_telemetry.h"
#include "proto_codecs.h"
#include "proto_frame.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace esphome {
namespace home_io_control {

/// Hold queued background polls back for this long after any 1W frame, so a poll the hub itself
/// scheduled does not occupy the (half-duplex) radio while the remote is still transmitting.
/// Comfortably longer than the gap between two copies of one burst, even with a wake-up preamble on
/// every copy (about 0.3 s); short enough to barely shift poll timing
/// otherwise. Only background polls yield — see decisions::defer_background_poll_for_1w_activity().
static constexpr uint32_t ONEWAY_QUIET_PERIOD_MS = 700;
/// Hard cap on how long sustained 1W traffic may hold a background poll back in total, measured
/// from the start of the burst rather than the most recent frame. ONEWAY_QUIET_PERIOD_MS re-arms on
/// every frame, so without this cap traffic arriving faster than the quiet period apart — a stuck
/// remote, a chatty neighbour's sensor — could starve background polls, and therefore device state
/// freshness, indefinitely. Comfortably above a legitimate burst, which lasts up to about 1.2 s when every
/// copy carries a wake-up preamble; bounds the worst case
/// without meaningfully affecting normal operation.
static constexpr uint32_t ONEWAY_POLL_DEFER_CAP_MS = 5000;
/// Suppress a repeated 1W log/poll for the same remote *and the same intent* within this window.
/// Wide on purpose: it collapses the 4× reliability burst into one logical press. A frame that
/// carries a rolling sequence to the same destination is a repeat only when the sequence is the same,
/// so two quick presses are two; any other frame (a private beacon, a discovery sweep, one trigger
/// swept across device classes) is a repeat when sender, command, intent or destination and time
/// match. A *different* intent from the same remote (a stop after a move) is not a duplicate and
/// passes through immediately — see decisions::is_duplicate_1w_frame().
static constexpr uint32_t ONEWAY_DEDUP_WINDOW_MS = 2000;

/// How many 1W frames heard inside the hub's own exchanges are kept for the remote handling. A remote
/// repeats a press 4 times, so 4 covers one press heard entirely inside one exchange; a fifth frame
/// drops the oldest.
static constexpr size_t ONEWAY_KEPT_FRAMES_MAX = 4;

/// A 1W frame heard while an exchange was waiting, with what the remote handling needs from its
/// arrival.
struct KeptOneWayFrame {
  IoFrame frame{};       ///< The parsed frame.
  uint32_t heard_ms{0};  ///< millis() when the exchange wait heard it.
  int16_t rssi_dbm{0};   ///< RSSI of the capture, in dBm, or RSSI_UNKNOWN_DBM.
};

/// @brief Receive-side state of the 1W frames the hub overhears.
///
/// Five jobs, each a few lines of state:
/// - the **burst window**: when the current burst of 1W traffic started and when it last advanced,
///   which gates background polls (see defers_background_poll());
/// - the **duplicate filter**: one logical press arrives as many repeats (is_new_press());
/// - the **pairing-gesture sighting** a later discover_and_pair() attempt seeds its telemetry with
///   (recent_pairing_sighting());
/// - the **sender allowlist** that decides which senders may reach Home Assistant as an event;
/// - the **kept frames**: 1W frames an exchange wait heard and ignored, held until the exchange has
///   ended and the hub takes them for the same handling as an idle frame (keep_overheard(), pop_kept()).
///
/// The order in which the hub calls these matters and is the hub's to keep: note_frame() comes
/// before the duplicate filter so a repeat still counts as activity, and is_new_press() needs the
/// decoded frame.
/// @ingroup hioc_hub
class OneWayReceiver {
 public:
  /// Record that a 1W frame just went out on the radio, ours or someone else's.
  ///
  /// A gap of a full quiet period or more since the previous frame starts a new burst window rather
  /// than extending the old one (which would make ONEWAY_POLL_DEFER_CAP_MS fire on the very next
  /// frame). Also called for the hub's own 1W transmissions: our own burst should defer background
  /// polls exactly like a remote's does, because it puts the same traffic on the same shared channel.
  /// @param now millis() at which the frame was seen or sent.
  void note_activity(uint32_t now);

  /// Record an overheard 1W frame: activity, plus the pairing-gesture sighting when the frame is one.
  ///
  /// Meant to run before the duplicate filter, so a repeated gesture frame still refreshes the
  /// sighting's timestamp and RSSI. The discovery telemetry window alone does not catch such a
  /// gesture: it only starts recording when an attempt begins, and a PROG press completed just
  /// before "Discover & Pair" is pressed would be invisible to the pairing advisor.
  /// @param frame Parsed frame with the CTRL0 1W bit set.
  /// @param now millis() at which the frame was seen.
  /// @param rssi_dbm RSSI of the capture, in dBm, or RSSI_UNKNOWN_DBM.
  void note_frame(const IoFrame &frame, uint32_t now, int16_t rssi_dbm);

  /// The duplicate filter. A repeat of the previous press inside ONEWAY_DEDUP_WINDOW_MS is dropped;
  /// anything else becomes the new reference.
  ///
  /// Frames with a rolling sequence are compared by it (see decisions::is_duplicate_1w_frame()). Otherwise the
  /// key includes the decoded intent bytes, so a stop right after a move is a new press; for a
  /// frame without a decoded intent it includes the destination, so each class of a multi-class sweep
  /// is kept (see decisions::OneWayDedupState).
  /// @param frame The overheard frame.
  /// @param info Its decoded content (decode_1w_frame()).
  /// @param now millis() at which the frame was seen.
  /// @return true for a new press, false for a repeat of the previous one.
  [[nodiscard]] bool is_new_press(const IoFrame &frame, const OneWayFrameInfo &info, uint32_t now);

  /// Keep a 1W frame an exchange wait heard, for pop_kept(). A frame that is not 1W is not kept
  /// (decisions::keep_overheard_frame_for_oneway_handling()). At ONEWAY_KEPT_FRAMES_MAX frames the
  /// oldest is dropped with a debug line.
  /// @param frame Parsed frame heard during an exchange wait.
  /// @param now millis() when it was heard.
  /// @param rssi_dbm RSSI of the capture, in dBm, or RSSI_UNKNOWN_DBM.
  void keep_overheard(const IoFrame &frame, uint32_t now, int16_t rssi_dbm);

  /// Take the oldest kept frame out of the store. Call until it returns false to drain; neither
  /// keeping nor taking allocates.
  /// @param out Receives the frame when one was kept.
  /// @return false when nothing is kept (@p out is untouched).
  [[nodiscard]] bool pop_kept(KeptOneWayFrame &out);

  /// @return true when frames are kept and waiting for pop_kept().
  [[nodiscard]] bool has_kept() const { return this->kept_count_ != 0; }

  /// Whether a queued background poll should wait because a 1W remote is still transmitting.
  /// Binds the burst window to decisions::defer_background_poll_for_1w_activity().
  /// @param next_is_background True when the next queued operation is a background poll.
  /// @param now millis() now.
  [[nodiscard]] bool defers_background_poll(bool next_is_background, uint32_t now) const;

  /// Allow @p sender_id to reach Home Assistant as a sender event.
  /// @param sender_id Node ID of the 1W sender, as the upper-case hex string node_id_to_string() gives.
  void add_exposed_sender(const std::string &sender_id) { this->exposed_senders_.push_back(sender_id); }

  /// @return true when @p sender_id is on the allowlist. The list is empty by default: a sender must
  /// be opted in explicitly, since 1W broadcasts carry no ownership marker and a neighbour's remote
  /// is heard as easily as the user's own.
  [[nodiscard]] bool is_exposed(const std::string &sender_id) const;

  /// @return The latest pairing-gesture sighting; `seen_ms == 0` when none since boot. The reference
  /// stays valid for the receiver's lifetime, so PairingEngine can hold it.
  [[nodiscard]] const RecentOneWayPairingSighting &recent_pairing_sighting() const { return this->sighting_; }

  /// @return millis() of the latest 1W frame of any kind, duplicates included; 0 until one is seen.
  [[nodiscard]] uint32_t last_activity_ms() const { return this->last_activity_ms_; }

 private:
  /// millis() of the most recent 1W frame, including ones dropped as duplicates: a repeat still
  /// means the remote is transmitting.
  uint32_t last_activity_ms_{0};
  /// millis() of the first frame of the current burst; bounds the deferral via ONEWAY_POLL_DEFER_CAP_MS.
  uint32_t first_activity_ms_{0};
  /// Identity of the last press that passed the duplicate filter.
  decisions::OneWayDedupState last_press_{};
  /// Latest pairing-gesture frame seen on the passive receive path.
  RecentOneWayPairingSighting sighting_{};
  /// Ring of the frames kept by keep_overheard(): the oldest is at kept_head_, kept_count_ are in use.
  std::array<KeptOneWayFrame, ONEWAY_KEPT_FRAMES_MAX> kept_{};
  size_t kept_head_{0};   ///< Index in kept_ of the oldest kept frame.
  size_t kept_count_{0};  ///< Frames kept, at most ONEWAY_KEPT_FRAMES_MAX.
  /// Config-time allowlist (populated once from YAML), not a per-frame allocation.
  std::vector<std::string> exposed_senders_;
};

}  // namespace home_io_control
}  // namespace esphome

#pragma once

/// @file operation_queue.h
/// @brief Pending-operation queue with per-type coalescing and deduplication.
/// @ingroup hioc_hub
///
/// Owns the PendingOperationType / PendingOperation types and the coalescing
/// rules applied when operations are enqueued. Pure data and logic — no ESPHome
/// dependencies, fully host-testable.

#include "proto_device_model.h"

#include <cstdint>
#include <deque>
#include <optional>
#include <string>

namespace esphome {
namespace home_io_control {

/// Position value written for binary ON commands (light on, switch on, lock unlock).
static constexpr uint8_t BINARY_ENTITY_ON_POSITION = 0;
/// Position value written for binary OFF commands (light off, switch off, lock lock).
static constexpr uint8_t BINARY_ENTITY_OFF_POSITION = 100;

/// @brief Discriminator for entries in the pending-operation deque.
enum class PendingOperationType : uint8_t {
  SET_POSITION,           ///< set_device_position call (position 0–100 or special values).
  SET_TILT,               ///< set_device_tilt call (tilt percentage 0–100).
  SET_POSITION_AND_TILT,  ///< Combined set_device_position_and_tilt call.
  DEVICE_COMMAND,         ///< Named device command (STOP, FAVORITE, VENT).
  SET_LIGHT_STATE,        ///< set_light_state call (binary on/off).
  SET_LOCK_STATE,         ///< set_lock_state call (locked/unlocked).
  SET_SWITCH_STATE,       ///< set_switch_state call (binary on/off).
  ONEWAY_COMMAND,         ///< 1W named command sent as a controller identity.
  ONEWAY_POSITION,        ///< 1W numeric position sent as a controller identity.
  ONEWAY_ENROLL,          ///< 1W add-controller (0x30) registering an identity.
  ONEWAY_UNENROLL,        ///< 1W remove-controller (0x39) un-registering an identity.
  REQUEST_STATUS,         ///< request_device_status call (poll for current position).
  REQUEST_NAME,           ///< request_device_name call (poll for stored device name).
  DISCOVER_AND_PAIR,      ///< discover_and_pair call (starts 3-phase pairing flow).
};

/// @brief A single queued operation to be dispatched from loop().
struct PendingOperation {
  PendingOperationType type;  ///< Operation type (determines which handler to invoke).
  /// Target device ID (hex string, e.g., "123ABC") — **except** for the ONEWAY_* types, where it
  /// carries the controller-identity handle instead. 1W addresses a device class, not a node, so
  /// there is no device ID to put here; the identity is what the operation is bound to. The field
  /// is reused rather than duplicated because every queue entry would otherwise grow a second
  /// string for the benefit of two operation types.
  std::string device_id;
  uint8_t position{0};                       ///< Position/tilt value (0–100) or binary state (ON=0, OFF=100).
  uint8_t tilt{0};                           ///< Tilt value for SET_POSITION_AND_TILT (0–100).
  CoverCommand command{CoverCommand::STOP};  ///< Named command for DEVICE_COMMAND operations.
};

/// @brief Serialized pending-operation queue with coalescing, deduplication, and two-band ordering.
///
/// **Ordering:** DISCOVER_AND_PAIR > control operations > background polls.
/// Control operations (SET_*, DEVICE_COMMAND) are inserted before any queued REQUEST_STATUS /
/// REQUEST_NAME entries so user-visible commands execute with minimum queue latency.
/// DISCOVER_AND_PAIR always front-inserts ahead of all other entries.
///
/// **Stale-poll drop:** enqueueing a control operation for device X drops any queued
/// REQUEST_STATUS for X, since the command reply supersedes it and re-arms tracking.
///
/// Coalescing rules (both directions):
/// - SET_POSITION arriving while SET_TILT is pending for the same device → SET_POSITION_AND_TILT.
/// - SET_TILT arriving while SET_POSITION is pending for the same device → SET_POSITION_AND_TILT.
///
/// Deduplication rules:
/// - At most one REQUEST_STATUS per device.
/// - At most one REQUEST_NAME per device.
/// - At most one DISCOVER_AND_PAIR (any pending REQUEST_STATUS / REQUEST_NAME entries are flushed
///   and the discover entry is pushed to the front to minimize pairing-window latency).
///
/// All coalescing and deduplication log messages are suppressed here — callers are responsible
/// for emitting them because OperationQueue must remain free of ESPHome logging dependencies.
class OperationQueue {
 public:
  // --- Position / tilt (with coalescing) ---

  /// Enqueue SET_POSITION, or coalesce with a pending SET_TILT into SET_POSITION_AND_TILT.
  /// @return true if coalesced (caller should log the merge), false if a new entry was pushed.
  bool enqueue_set_position(const std::string &device_id, uint8_t position);

  /// Enqueue SET_TILT, or coalesce with a pending SET_POSITION into SET_POSITION_AND_TILT.
  /// @return true if coalesced (caller should log the merge), false if a new entry was pushed.
  bool enqueue_set_tilt(const std::string &device_id, uint8_t tilt_percent);

  /// Enqueue SET_POSITION_AND_TILT directly (no coalescing needed).
  void enqueue_set_position_and_tilt(const std::string &device_id, uint8_t position, uint8_t tilt_percent);

  // --- Named cover command ---
  void enqueue_device_command(const std::string &device_id, CoverCommand cmd);

  // --- Binary entity commands (no coalescing) ---
  /// Enqueue SET_LIGHT_STATE with an arbitrary IO position (0-100), for dimmable lights.
  /// enqueue_set_light_state() is a thin binary-position wrapper around this.
  void enqueue_set_light_position(const std::string &device_id, uint8_t position);
  void enqueue_set_light_state(const std::string &device_id, bool on);
  void enqueue_set_lock_state(const std::string &device_id, bool locked);
  void enqueue_set_switch_state(const std::string &device_id, bool on);

  // --- 1W commands (no coalescing, no deduplication) ---

  /// Enqueue a 1W named command for a controller identity.
  ///
  /// Deliberately neither coalesced nor deduplicated, unlike their 2W counterparts. A 2W command
  /// can be superseded because the device reports back what it did; a 1W command cannot be
  /// confirmed at all, so dropping one is dropping a press the user made with nothing to notice
  /// it. Each press also consumes its own sequence, and merging two would leave a gap that looks
  /// like a lost frame to a device tracking the counter.
  /// @param controller_id Controller-identity handle (see PendingOperation::device_id).
  /// @param cmd Named command to send.
  void enqueue_oneway_command(const std::string &controller_id, CoverCommand cmd);

  /// Enqueue a 1W numeric position for a controller identity.
  /// @param controller_id Controller-identity handle (see PendingOperation::device_id).
  /// @param position Target position 0–100.
  void enqueue_oneway_position(const std::string &controller_id, uint8_t position);

  /// Enqueue a 1W enrollment (add-controller) for a controller identity. Same no-coalesce,
  /// no-dedup contract as the other 1W ops, for the same reason.
  /// @param controller_id Controller-identity handle (see PendingOperation::device_id).
  void enqueue_oneway_enroll(const std::string &controller_id);

  /// Enqueue a 1W un-enrollment (remove-controller) for a controller identity.
  /// @param controller_id Controller-identity handle (see PendingOperation::device_id).
  void enqueue_oneway_unenroll(const std::string &controller_id);

  // --- Background polls (with deduplication) ---

  /// Enqueue REQUEST_STATUS, suppressing duplicates.
  /// @return true if pushed, false if a duplicate was already pending.
  bool enqueue_request_status(const std::string &device_id);

  /// Enqueue REQUEST_NAME, suppressing duplicates.
  /// @return true if pushed, false if a duplicate was already pending.
  bool enqueue_request_name(const std::string &device_id);

  // --- Pairing (dedup + priority front-push) ---

  /// Enqueue DISCOVER_AND_PAIR with elevated priority.
  /// Flushes pending REQUEST_STATUS / REQUEST_NAME entries and pushes to front.
  /// @return true if pushed, false if a DISCOVER_AND_PAIR was already pending.
  bool enqueue_discover_and_pair();

  // --- Queue access ---
  [[nodiscard]] std::optional<PendingOperation> pop();
  [[nodiscard]] bool empty() const;
  [[nodiscard]] std::size_t size() const;

  // --- Read-only inspection ---
  [[nodiscard]] const PendingOperation &front() const;
  [[nodiscard]] const PendingOperation &back() const;
  [[nodiscard]] const PendingOperation &operator[](std::size_t index) const;
  [[nodiscard]] std::deque<PendingOperation>::const_iterator begin() const;
  [[nodiscard]] std::deque<PendingOperation>::const_iterator end() const;

  /// True for background poll types (REQUEST_STATUS, REQUEST_NAME) that yield to control operations.
  /// Public because the dispatch gate in loop() defers *only* background work when a 1W remote is
  /// transmitting; a user command must never be held back for it.
  [[nodiscard]] static bool is_background_op(PendingOperationType t);

 private:
  std::deque<PendingOperation> queue_;
  /// Insert a control operation before the first background entry; also drops any queued
  /// REQUEST_STATUS for the same device since the incoming command supersedes it.
  void push_control_(PendingOperation op);
};

}  // namespace home_io_control
}  // namespace esphome

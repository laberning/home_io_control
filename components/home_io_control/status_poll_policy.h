#pragma once

/// @file status_poll_policy.h
/// @brief Per-device poll scheduling, failure backoff, and follow-up-poll state machine.
/// @ingroup hioc_hub
///
/// Owns all per-device poll bookkeeping previously spread across IoDevice fields,
/// hub_internal.h helpers, and multiple hub implementation files. Pure logic with
/// injected timestamps — fully host-testable without a clock mock.

#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace esphome {
namespace home_io_control {

/// @name Background-poll retry delays after silent failures (no reply received)
/// @{
static constexpr uint32_t STATUS_RETRY_AFTER_FAIL_MS = 5000;         ///< First retry after a silent failure.
static constexpr uint32_t STATUS_RETRY_AFTER_FAIL_STEP2_MS = 15000;  ///< Second retry after a silent failure.
static constexpr uint32_t STATUS_RETRY_AFTER_FAIL_STEP3_MS = 30000;  ///< Third retry after a silent failure.
static constexpr uint32_t STATUS_RETRY_AFTER_FAIL_STEP4_MS = 60000;  ///< Fourth retry after a silent failure.
static constexpr uint32_t STATUS_RETRY_AFTER_FAIL_MAX_MS =
    300000;  ///< Steady-state backoff after many silent failures.
/// @}

/// @name Background-poll retry delays after auth-shaped failures (0x3C challenge seen)
/// @{
static constexpr uint32_t STATUS_AUTH_RETRY_AFTER_FAIL_MS = 30000;  ///< First retry after a challenge-seen failure.
static constexpr uint32_t STATUS_AUTH_RETRY_AFTER_FAIL_STEP2_MS = 120000;  ///< Second retry.
static constexpr uint32_t STATUS_AUTH_RETRY_AFTER_FAIL_MAX_MS = 300000;  ///< Steady-state after repeated auth failures.
/// @}

/// Hard stop for bounded follow-up polling after a command or remote activity.
static constexpr uint32_t MAX_TRACKED_STATUS_POLL_WINDOW_MS = 600000;
/// Delay before the first post-boot status request from an entity.
static constexpr uint32_t INITIAL_STATUS_REQUEST_DELAY_MS = 5000;
/// Delay before polling after overheard remote traffic.
static constexpr uint32_t REMOTE_ACTIVITY_STATUS_POLL_DELAY_MS = 2000;

/// @brief Per-device poll scheduling state, moved out of IoDevice.
struct PollTracking {
  uint32_t interval_ms{0};                    ///< Configured follow-up poll interval (0 = one-shot settle only).
  uint32_t next_update{0};                    ///< Absolute millis() timestamp for the next poll; 0 = idle.
  uint32_t poll_deadline{0};                  ///< Hard stop for bounded follow-up polling.
  uint8_t status_poll_failures{0};            ///< Consecutive background poll failures (silent — no reply).
  uint8_t auth_poll_failures{0};              ///< Consecutive background poll failures that saw a 0x3C challenge.
  bool single_follow_up_poll_pending{false};  ///< One-shot legacy settle poll still pending.
};

/// @brief Per-hub poll scheduling and failure-backoff policy.
///
/// Maintains a PollTracking entry per device. All poll bookkeeping that previously
/// lived in IoDevice fields, hub_internal.h inline helpers, and scattered hub
/// implementation files now lives here. Callers inject 'now' timestamps so the
/// class is fully testable without a hardware clock.
class StatusPollPolicy {
 public:
  // --- Interval configuration ---
  /// Set the configured follow-up poll interval (ms; 0 = legacy one-shot settle only).
  void set_interval(const std::string &device_id, uint32_t interval_ms);
  /// Return the configured interval (0 when not set or unconfigured).
  [[nodiscard]] uint32_t get_interval(const std::string &device_id) const;

  // --- Tracking lifecycle ---
  /// Begin bounded follow-up polling after a command or remote activity.
  /// No-op when the device has no configured interval (interval_ms == 0).
  void begin_tracking(const std::string &device_id, uint32_t initial_delay_ms, uint32_t now);
  /// Stop all polling for a device and reset failure counts. Preserves interval_ms.
  void clear(const std::string &device_id);

  // --- One-shot settle flag ---
  /// Mark whether a one-shot legacy settle poll is still pending.
  void set_one_shot_pending(const std::string &device_id, bool pending);
  [[nodiscard]] bool is_one_shot_pending(const std::string &device_id) const;
  void clear_one_shot_pending(const std::string &device_id);

  // --- Next-update scheduling ---
  /// Schedule the next poll at an absolute millis() timestamp.
  void set_next_update(const std::string &device_id, uint32_t abs_time);
  [[nodiscard]] uint32_t get_next_update(const std::string &device_id) const;

  // --- Failure tracking ---
  /// Record a failed background poll; apply backoff or clear tracking if the window expired.
  /// Returns the backoff delay applied in ms, or 0 if tracking was cleared.
  uint32_t on_exchange_failed(const std::string &device_id, bool auth_like, uint32_t now);
  /// Reset failure streaks after a successful exchange response.
  void clear_failure_streaks(const std::string &device_id);
  [[nodiscard]] uint8_t get_status_poll_failures(const std::string &device_id) const;
  [[nodiscard]] uint8_t get_auth_poll_failures(const std::string &device_id) const;
  [[nodiscard]] uint32_t get_poll_deadline(const std::string &device_id) const;

  // --- Queries ---
  /// True when the device has an active bounded polling window (interval set, deadline not expired).
  [[nodiscard]] bool is_tracking_active(const std::string &device_id, uint32_t now) const;

  // --- Due-device scan ---
  /// Return and consume the first device whose next_update is overdue.
  /// Clears expired tracking entries as a side effect. Returns nullopt if nothing is due.
  [[nodiscard]] std::optional<std::string> pop_due_device(uint32_t now);

 private:
  std::map<std::string, PollTracking> tracking_;

  /// Compute retry delay from failure count and failure class.
  static uint32_t retry_delay_ms(uint8_t consecutive_failures, bool auth_like);
};

}  // namespace home_io_control
}  // namespace esphome

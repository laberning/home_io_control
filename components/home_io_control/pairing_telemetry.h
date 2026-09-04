#pragma once

/// @file pairing_telemetry.h
/// @brief Structured per-attempt telemetry recorder for the pairing flow.
/// @ingroup hioc_hub
///
/// PairingTelemetry is a fixed-size recorder owned by the hub and shared (by pointer) with
/// ExchangeEngine and PairingEngine, mirroring how TuningConfig is injected into both. It
/// records every radio-visible event during a `discover_and_pair()` attempt — TX, RX (accepted
/// and rejected), LBT defers, and hop/phase transitions — so a single attempt can be summarized
/// as a human-readable log block and a frozen, machine-readable "Last Pairing Result" string.
///
/// Telemetry events store only cmd/src/rssi/phase metadata, never frame payload bytes, so key
/// material cannot appear here by construction — there is no redaction to apply because there
/// is nothing to redact.

#include "hub_pairing.h"
#include "proto_device_model.h"
#include "proto_frame.h"

#include <cstdint>
#include <string>

namespace esphome {
namespace home_io_control {

/// @brief Kind of a recorded telemetry event.
enum class PairingTelemetryEventKind : uint8_t {
  TX,         ///< We transmitted a frame.
  RX,         ///< We received and accepted a frame for the current wait (including a seeded
              ///< pre-window sighting — see PairingTelemetryEvent::aux).
  RX_REJECT,  ///< We received a frame that parsed but was rejected (wrong source, wrong command, etc.).
  LBT_DEFER,  ///< A listen-before-talk check deferred a transmit because the channel was busy.
  PHASE,      ///< The pairing state machine advanced to a new phase.
  // Deliberately no HOP kind: a channel hop is counted via PairingTelemetry::hop_count() only,
  // never stored as an event — see record_hop()'s doc comment for why.
};

/// @brief Maximum number of events recorded per pairing attempt.
///
/// Events beyond this bound are still counted in PairingTelemetry::heard_count() but not
/// stored — the array is a fixed-size ring-free buffer (first N events), not a true ring.
static constexpr uint8_t PAIRING_TELEMETRY_MAX_EVENTS = 32;

/// @brief One recorded telemetry event.
struct PairingTelemetryEvent {
  uint32_t millis_offset{0};  ///< millis() at record time, relative to PairingTelemetry::begin().
  PairingTelemetryEventKind kind{PairingTelemetryEventKind::PHASE};  ///< What happened.
  uint8_t cmd{0};                    ///< Frame command byte (TX/RX/RX_REJECT); unused otherwise.
  uint8_t src_node[NODE_ID_SIZE]{};  ///< Sender node ID (RX/RX_REJECT only); zero otherwise.
  uint8_t dst_node[NODE_ID_SIZE]{};  ///< Destination node ID (RX/RX_REJECT only); zero otherwise.
  int16_t rssi{0};                   ///< RSSI in dBm (RX/RX_REJECT/LBT_DEFER, including a seeded
                                     ///< pre-window RX — see PairingTelemetry::record_recent_one_way_sighting());
                                     ///< zero otherwise.
  uint8_t aux{0};                    ///< Kind-specific extra byte: PHASE -> pairing::PairingState value; LBT_DEFER ->
                                     ///< retry number reached; RX -> 1 if this is a seeded pre-window sighting rather
                                     ///< than a live in-window capture (0 for both a live RX and every RX_REJECT);
                                     ///< unused (0) for TX.
  bool oneway{false};                ///< CTRL0 1W-protocol bit (RX/RX_REJECT only); false otherwise.
};

/// @brief A 1W pairing-gesture frame observed on the hub's normal passive RX path, remembered so
/// a fresh `discover_and_pair()` attempt can seed its telemetry with it (issue #27/#65: the
/// discovery telemetry window only starts recording at `PairingTelemetry::begin()`, so a PROG
/// press completed just before "Discover & Pair" is pressed could otherwise be invisible to the
/// pairing advisor even though the radio heard it just fine). `seen_ms == 0` means none has been
/// observed since boot. Owned by the hub, written from hub_status.cpp's passive RX path, read by
/// PairingEngine at the start of every attempt.
struct RecentOneWayPairingSighting {
  uint8_t src[NODE_ID_SIZE]{};  ///< Sender node ID.
  uint8_t dst[NODE_ID_SIZE]{};  ///< Destination node ID (the 1W broadcast address, in practice).
  uint8_t cmd{0};               ///< Frame command byte.
  int16_t rssi{0};              ///< RSSI in dBm at the time it was overheard.
  uint32_t seen_ms{0};          ///< millis() the frame was seen; 0 if none seen since boot.
};

/// @brief Final disposition of a pairing attempt, used by the result sensor string.
enum class PairingOutcome : uint8_t {
  NONE,                 ///< No attempt has completed yet (initial state).
  PAIRED,               ///< All three phases completed successfully.
  NO_RESPONSE,          ///< No device responded to discovery.
  INVALID_RESPONSE,     ///< Discovery saw traffic but nothing valid.
  KEY_EXCHANGE_FAILED,  ///< Discovery succeeded but the key exchange did not complete.
  CONFIG_FAILED,        ///< Key exchange succeeded but SetConfig1 failed (still counted as paired).
};

/// @brief Fixed-size per-attempt telemetry recorder for the pairing flow.
/// @ingroup hioc_hub
///
/// Owned by the hub, reset at the start of every `discover_and_pair()` call. Not thread-safe —
/// pairing is a single blocking call on the main loop, matching the rest of this component.
class PairingTelemetry {
 public:
  /// Reset all state and start a new attempt. Call once at `discover_and_pair()` entry.
  void begin();

  /// Record that we transmitted a frame.
  /// @param cmd Frame command byte.
  void record_tx(uint8_t cmd);
  /// Record that we received and accepted a frame.
  /// @param frame Parsed frame (cmd, src, dst, and the 1W protocol bit are recorded — `dst` and
  ///               the 1W bit feed PairingAdvisor's ONE_WAY_PAIRING_TRAFFIC and
  ///               FOREIGN_CONTROLLER_PAIRING detection, see pairing_advisor.h).
  /// @param rssi RSSI in dBm.
  void record_rx(const IoFrame &frame, int16_t rssi);
  /// Record that we received a frame that parsed but was rejected by a classifier.
  /// @param frame Parsed frame (cmd, src, dst, and the 1W protocol bit are recorded — `dst` and
  ///               the 1W bit feed PairingAdvisor's ONE_WAY_PAIRING_TRAFFIC and
  ///               FOREIGN_CONTROLLER_PAIRING detection, see pairing_advisor.h).
  /// @param rssi RSSI in dBm.
  void record_rx_reject(const IoFrame &frame, int16_t rssi);
  /// Record a listen-before-talk defer (channel busy).
  /// @param rssi RSSI in dBm that triggered the defer.
  void record_lbt_defer(int16_t rssi);
  /// Record a frequency hop while waiting. The exchange engine does not expose which channel
  /// index a hop landed on, so this only marks that a hop happened, not where to.
  ///
  /// Counted via hop_count() only, deliberately **not** stored in the fixed event array: at the
  /// current 5-7 ms hop slice, a multi-second discovery window produces far more hops than
  /// PAIRING_TELEMETRY_MAX_EVENTS, which used to exhaust the buffer with hop events alone within
  /// about a second of essentially every real attempt (issue #27). This did **not** produce a false
  /// `rf_silent` — heard_count() is incremented in record_() before the capacity check, so it still
  /// counts an RX/RX_REJECT that arrives after the array is full. What it did do: once the array
  /// was full, PairingAdvisor's `1w_traffic`/`channel_busy`/`foreign_controller` passes — which all
  /// scan events(), not heard_count() — could only ever see whatever RX/RX_REJECT evidence had
  /// landed in roughly the first ~200 ms of the attempt, and truncated() was true on nearly every
  /// real pairing attempt. A hop carries no per-event information worth itemizing (no channel
  /// index, no RSSI) anyway; the total count is enough.
  void record_hop();
  /// Record a pairing state-machine phase transition.
  /// @param phase New phase.
  void set_phase(pairing::PairingState phase);
  /// Record the final outcome of the attempt.
  /// @param outcome Final disposition.
  void set_outcome(PairingOutcome outcome);
  /// Record the successfully paired device, if any.
  /// @param node_id Paired device's node ID.
  /// @param type Paired device's type.
  void set_paired_device(const uint8_t node_id[NODE_ID_SIZE], DeviceType type);
  /// Increment the discovery attempt counter (one call per discovery command retry).
  void increment_discovery_attempt();
  /// Record the advisor's short advice codes for the `;advice=` result-sensor field.
  /// @param codes Comma-separated short codes (e.g. "1w_traffic,channel_busy"), or empty if none.
  void set_advice_codes(const std::string &codes) { this->advice_codes_ = codes; }

  /// Seed telemetry with a 1W pairing-gesture frame observed shortly before this attempt began.
  /// Recorded as an RX event with its real RSSI, marked seeded (PairingTelemetryEvent::aux = 1) so
  /// log_summary() can label it honestly instead of implying a live in-window capture, and so
  /// PairingAdvisor's channel_busy pass — which is specifically about *live* contention during
  /// this attempt — can exclude it while ONE_WAY_PAIRING_TRAFFIC still picks it up like any other
  /// RX. It counts toward heard_count(). Timestamped at the start of the attempt (millis_offset
  /// ≈ 0), since its real arrival time — before begin() — isn't preserved; the seeded marker is
  /// what tells a reader not to read that as "heard 0 ms into this attempt". The caller (
  /// PairingEngine::discover_and_pair()) is responsible for deciding whether `sighting` is recent
  /// enough to seed with — this method always records what it's given.
  /// @param sighting The overheard frame to seed telemetry with.
  void record_recent_one_way_sighting(const RecentOneWayPairingSighting &sighting);

  /// @return Recorded events (up to PAIRING_TELEMETRY_MAX_EVENTS), oldest first.
  [[nodiscard]] const PairingTelemetryEvent *events() const { return this->events_; }
  /// @return Number of events actually stored (<= PAIRING_TELEMETRY_MAX_EVENTS).
  [[nodiscard]] uint8_t event_count() const { return this->event_count_; }
  /// @return Total RX events seen, including ones beyond the storage capacity.
  [[nodiscard]] uint16_t heard_count() const { return this->heard_count_; }
  /// @return Total number of channel hops during the attempt. Not itemized in events() — see
  /// record_hop()'s doc comment for why.
  [[nodiscard]] uint32_t hop_count() const { return this->hop_count_; }
  /// @return true if any event (of any kind — TX/RX/RX_REJECT/LBT_DEFER/HOP/PHASE) was dropped
  /// because the fixed event array was full. Deliberately not derived from
  /// `heard_count() > event_count()` — the two counters measure different populations
  /// (`heard_count()` is RX/RX_REJECT only; `event_count()` is stored events of every kind), so
  /// that comparison misses truncation whenever most stored events aren't RX/RX_REJECT.
  [[nodiscard]] bool truncated() const { return this->truncated_; }
  /// @return Highest phase reached during the attempt.
  [[nodiscard]] pairing::PairingState phase() const { return this->phase_; }
  /// @return Final outcome, or PairingOutcome::NONE if no attempt has completed yet.
  [[nodiscard]] PairingOutcome outcome() const { return this->outcome_; }
  /// @return Number of discovery command attempts made.
  [[nodiscard]] uint8_t discovery_attempts() const { return this->discovery_attempts_; }
  /// @return Total listen-before-talk retries consumed across the whole attempt.
  [[nodiscard]] uint8_t lbt_retries() const { return this->lbt_retries_; }
  /// @return true if a device was successfully paired this attempt.
  [[nodiscard]] bool has_paired_device() const { return this->has_paired_device_; }
  /// @return Paired device's node ID; only meaningful if has_paired_device() is true.
  [[nodiscard]] const uint8_t *paired_node_id() const { return this->paired_node_id_; }
  /// @return Paired device's type; only meaningful if has_paired_device() is true.
  [[nodiscard]] DeviceType paired_device_type() const { return this->paired_device_type_; }
  /// @return Duration of the attempt so far (or total, once complete) in milliseconds.
  [[nodiscard]] uint32_t duration_ms() const;

  /// Emit a multi-line human-readable summary via ESP_LOGI. Call once, at the end of the attempt.
  void log_summary() const;

  /// @brief Render the frozen `v1;` machine-readable result string.
  ///
  /// Format (never change without bumping the version tag — the Phase 2 rig's read-back
  /// contract parses this exactly; the space after each `;` is intentional — Home Assistant's
  /// UI only line-wraps this sensor's long value at whitespace, so a fields-only-separated-by-`;`
  /// string renders as one unbroken, unreadable line):
  /// `v1; outcome=<...>; phase=<...>; node=<XXXXXX|->; type=<...|->; attempts=<n>; lbt=<n>; dur_ms=<n>; heard=<n>; `
  /// `advice=<comma-separated codes|none>`
  /// @return The formatted result string.
  [[nodiscard]] std::string result_sensor_string() const;

 private:
  /// Append an event to the fixed array (dropped silently past capacity; heard_count_ still counts it).
  void record_(PairingTelemetryEventKind kind, uint8_t cmd, const uint8_t *src, const uint8_t *dst, int16_t rssi,
               uint8_t aux, bool oneway);

  PairingTelemetryEvent events_[PAIRING_TELEMETRY_MAX_EVENTS]{};
  uint8_t event_count_{0};
  uint16_t heard_count_{0};
  uint32_t hop_count_{0};
  bool truncated_{false};
  uint32_t start_ms_{0};
  uint32_t end_ms_{0};
  bool ended_{false};
  pairing::PairingState phase_{pairing::PairingState::IDLE};
  PairingOutcome outcome_{PairingOutcome::NONE};
  uint8_t discovery_attempts_{0};
  uint8_t lbt_retries_{0};
  bool has_paired_device_{false};
  uint8_t paired_node_id_[NODE_ID_SIZE]{};
  DeviceType paired_device_type_{DeviceType::UNKNOWN};
  std::string advice_codes_;
};

}  // namespace home_io_control
}  // namespace esphome

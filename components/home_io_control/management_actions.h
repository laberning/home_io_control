#pragma once

/// @file management_actions.h
/// @brief Hub-level management operations exposed as Home Assistant actions.
/// @ingroup hioc_hub
///
/// ManagementActions owns the family of advanced, non-entity hub actions (device rename,
/// identify, force-open, ...) and the native API registration that wires them into Home
/// Assistant. The opt-in diagnostic probes live in DiagnosticProbes (diagnostic_probes.h), which
/// ManagementActions owns and registers the two probe actions for. Every action is registered through one data-driven
/// API service descriptor (detail::ManagementServiceDescriptor); adding a new action does not require a new descriptor
/// class. The descriptor holds a ManagementActions* and calls only public methods on it, so no friend declarations into
/// the hub are needed.
///
/// IOHomeControlComponent exposes this collaborator through thin protected wrappers
/// (api_rename_device_, api_identify_device_, api_force_open_device_, api_scan_paired_devices_,
/// register_management_actions_).

#include "device_registry.h"
#include "diagnostic_probes.h"
#include "exchange_engine.h"
#include "management_helpers.h"
#include "proto_constants.h"

#include <cstdint>
#include <string>

namespace esphome {
namespace home_io_control {

// Forward declarations — full definitions included only in management_actions.cpp.
class IOHomeControlComponent;
struct TuningConfig;

/// Encapsulates hub-level management operations exposed as Home Assistant actions.
///
/// ManagementActions is constructed once by IOHomeControlComponent. It stores pointers
/// and references into hub members; it is non-copyable for the same reason as ExchangeEngine.
/// @ingroup hioc_hub
class ManagementActions {
 public:
  /// Construct with all required collaborators.
  ///
  /// @param node_id      Controller 3-byte node ID, owned by the hub.
  /// @param system_key   Controller 16-byte AES system key, owned by the hub. Only
  ///                     scan_paired_devices() uses this — it builds a self-authenticating
  ///                     CMD_DISCOVER_SPE_REQ itself, unlike rename/identify/force-open, which
  ///                     authenticate through ExchangeEngine's challenge-response.
  /// @param tuning       Runtime tuning config, owned by the hub. Only scan_paired_devices()
  ///                     uses this — it reuses `pairing_discovery_wait_ms` as the listen window
  ///                     for each of its own per-channel attempts, and reads `scan_power_classes`
  ///                     to decide which passes to run.
  /// @param engine       Shared exchange engine for radio transactions.
  /// @param registry     Device registry for device lookups.
  /// @param initialized  Pointer to the hub's initialized flag.
  /// @param hub          Hub pointer for ESPHome API calls and public hub methods.
  ManagementActions(const uint8_t *node_id, const uint8_t *system_key, const TuningConfig *tuning,
                    ExchangeEngine &engine, DeviceRegistry &registry, const bool *initialized,
                    IOHomeControlComponent *hub);

  /// Non-copyable — stores references and pointers into hub member addresses.
  ManagementActions(const ManagementActions &) = delete;
  ManagementActions &operator=(const ManagementActions &) = delete;

  /// Register all management actions (rename, identify, force-open, ...) with ESPHome's
  /// native API server.
  void register_actions();

  /// Native API callback: rename a device and publish the result as a HA event.
  void api_rename_device(const std::string &device_id, const std::string &new_name);

  /// Rename a registered device and verify the result by reading the name back.
  /// @param device_id Target device ID (hex string, case-insensitive).
  /// @param new_name  Requested UTF-8 device name.
  /// @return Structured result describing success, verification, and any error.
  ManagementActionResult rename_device(const std::string &device_id, const std::string &new_name);

  /// Native API callback: trigger a device's physical identify and publish the result as a HA event.
  void api_identify_device(const std::string &device_id);

  /// @brief Trigger a registered device's physical identify (brief jog/flash).
  ///
  /// Deliberately performs no device-type gating beyond "is it registered on this hub" — identify
  /// exists precisely to let a user work out what an unknown registry entry physically is, so
  /// restricting it to specific device types would defeat the purpose. A CMD_ERROR_RESP reply is
  /// treated as a successful trigger (not a failure): per the protocol, devices may answer an
  /// identify request that way and still perform the jog.
  /// @param device_id Target device ID (hex string, case-insensitive).
  /// @return Structured result describing success and any validation failure. `verified` is
  ///         always false — there is no readback that confirms a physical jog happened.
  ManagementActionResult identify_device(const std::string &device_id);

  /// Native API callback: force-open a device and publish the result as a HA event.
  void api_force_open_device(const std::string &device_id);

  /// @brief Move a registered cover device to fully open at elevated priority, intended to
  /// bypass wind/rain soft locks.
  ///
  /// Safety-sensitive: this deliberately requires an explicit `device_id` (a hub-level action,
  /// not a permanent per-device button) because it can override legitimate environmental
  /// protection. Queues the command onto the hub's normal cover-command dispatch path
  /// (IOHomeControlComponent::queue_device_command()), which already owns capability gating,
  /// poll tracking, and backoff — this method does not talk to the radio directly. The result
  /// therefore only confirms that the command was queued; the actual movement outcome arrives
  /// later via the device's normal cover-state/polling pipeline, so `verified` is always false.
  /// @note The elevated-priority mechanism (see create_force_open() in proto_commands.cpp) is
  ///       confirmed on real hardware to correctly move a device to fully open, but bypassing
  ///       an *active* environmental lock is still experimental — only that the device accepts
  ///       the frame and opens when nothing is locking it has been observed so far.
  /// @param device_id Target device ID (hex string, case-insensitive).
  /// @return Structured result describing whether the command was queued.
  ManagementActionResult force_open_device(const std::string &device_id);

  /// Native API callback: run a roll-call scan and publish the result as a HA event.
  void api_scan_paired_devices();

  /// @brief Native API callback: queue a 1W position for a controller identity.
  ///
  /// An action rather than an entity per percentage (ADR 0006). `position` arrives as a string
  /// because every argument on this action surface is a string; it is parsed and range-checked
  /// here, and a bad value is reported rather than coerced — a malformed position that silently
  /// became 0 would send a fully-open command to a whole device class.
  ///
  /// The published result says only that the command was *queued*. 1W has no reply, so nothing
  /// downstream can ever upgrade that to "the device moved"; the per-identity "Last 1W Command"
  /// sensor reports what was transmitted.
  /// @param controller_id Controller-identity handle from `oneway_controllers:`.
  /// @param position Target position 0-100 as a decimal string.
  void api_oneway_set_position(const std::string &controller_id, const std::string &position);

  /// @brief Native API callback: queue a standalone 1W un-enrollment (remove-controller, CMD
  /// 0x39) for a controller identity.
  ///
  /// The direct caller of `0x39` in this codebase (ADR 0006 — a rare rollback path gets an
  /// action, not a permanent entity) — the "Enroll" button also fires this same frame, but only
  /// as the documented prelude immediately before its own `0x30`, never on its own. Same "queued,
  /// not confirmed" framing as api_oneway_set_position() — 1W has no reply, so nothing here can
  /// ever say a device actually forgot this identity.
  ///
  /// @warning **Works only inside the receiver's programming window.** Confirmed on a Somfy Izymo
  /// dimmer only: put the receiver in its programming mode (PROG window), then call this action;
  /// the Izymo blinks and stops obeying this identity. With no window open the frame changes
  /// nothing, and the hub keeps controlling the Izymo. Other receivers are untested.
  /// @param controller_id Controller-identity handle from `oneway_controllers:`.
  void api_oneway_remove_controller(const std::string &controller_id);

  /// @brief Broadcast a roll-call and report every device that answers.
  ///
  /// Not a discovery mechanism for new devices: only devices that already hold this hub's
  /// system key answer a CMD_DISCOVER_SPE_REQ (the 0x2A payload is self-authenticating — 6
  /// random bytes plus a 6-byte HMAC over the command byte, computed with `system_key_`), so a
  /// device that has never paired with this hub stays silent. See
  /// `tests/corpus/captures/discovery/somfy_awning_discovery_spe_paired_rollcall.yaml` for a captured
  /// exchange. Never writes DeviceRegistry — every responder is only looked up, never
  /// registered; an unknown responder usually means a device paired earlier whose YAML config
  /// was never saved, not an intruder. `result.device_id` stays empty (there is no single
  /// target) and `verified` stays false (nothing here is read back). `success` is true whenever
  /// the broadcast went out, including with zero replies — "nothing answered" is a valid
  /// result, not a failure. `result.message` carries the full multi-line report: a header line
  /// (devices detected, how many known vs. unknown), then a `Known:` section followed by an
  /// `Unknown:` section (either omitted if empty) — known responders get a summary line each,
  /// unknown responders additionally get a lead-in sentence and a ready-to-paste YAML block.
  /// Devices are grouped by known/unknown rather than left in arrival order, since the two
  /// groups need different follow-up and interleaving them made an unknown responder easy to
  /// miss between known ones. At most `SCAN_MAX_REPLIES` distinct responders are reported; if
  /// more answer, the report says so explicitly rather than quietly listing a subset.
  ///
  /// Sweeps two passes, low-power first, then always-alive — six attempts total (three channels
  /// per pass): CH2/CH1/CH3 with `CTRL1 = LOW_POWER | ACK` and `LONG_PREAMBLE` (the header a real
  /// VELUX KLR300 uses for its own roll-call, whose low-power shutters answered it), then
  /// CH2/CH1/CH3 with `CTRL1 = 0x00` and the tunable `normal_start_preamble`. Low-power first so a sleeper that answers
  /// late is still collected by the later always-alive windows; in the opposite order a late
  /// low-power reply would fall off the end of the scan. The low-power pass listens on all three
  /// channels including the one it transmitted on (real VELUX low-power replies have been
  /// observed landing there); the always-alive pass listens only on the two it did not transmit
  /// on (a Somfy always-alive reply lands on the request channel about 1 in 149 times). The
  /// `scan_power_classes` tuning select (default `both`) narrows this to one pass, for bisecting
  /// an installation in the field or restoring the shorter single-pass scan on an install with no
  /// solar/battery devices. Each attempt gets its own full `pairing_discovery_wait_ms` listen
  /// window, merging distinct responders across all six attempts — a paired device only hears the
  /// broadcast if it happens to be awake on the channel the hub transmits on at that instant, and
  /// real hardware testing found that single-channel duty-cycling paired devices are not reliably
  /// caught by a one-shot broadcast. A responder that answers more than one attempt still appears
  /// exactly once. Each attempt is a single transmit followed by its own window, never
  /// back-to-back transmits, so it does not reintroduce the different failure mode a
  /// multi-channel-burst transmit caused elsewhere: firing several long-preamble transmits
  /// back-to-back with no listening in between blew through the tight per-try response wait
  /// windows and broke exchanges in both directions (see
  /// `KeyExtractionResponder::send_reply_()`'s implementation comment).
  ///
  /// This still blocks the caller for the full six-window duration (roughly
  /// `6 × pairing_discovery_wait_ms` plus three ~213 ms LONG_PREAMBLE transmits, ~12.7 s at the
  /// 2000 ms default) and therefore trips ESPHome's "operation took a long time" warning on
  /// *every* invocation. That warning uses a per-component ratchet
  /// (`Component::should_warn_of_blocking()`): it starts at 50 ms and, each time it fires, raises
  /// its own threshold to the observed duration plus a margin — but the threshold is stored in
  /// centiseconds in a `uint8_t`, so it saturates at **2550 ms**. Anything that blocks longer than
  /// that can never ratchet out of warning range; doubling the scan adds no new warning class,
  /// since the single-pass scan was already permanently past the cap.
  ///
  /// Accepting that is a deliberate tradeoff (confirmed on real hardware 2026-08-10). A shorter
  /// fixed window — 500 ms per attempt, ~2.4 s for a three-attempt sweep (a single-pass total
  /// under the 2550 ms cap) — was tried: it would have gone quiet after one warning, but it also
  /// caused real, correctly-decoded replies from registered devices to arrive after the window
  /// had already closed, where the passive path drops them (`hub_status.cpp`'s `unhandled_cmd`
  /// catch-all). Losing devices from the report is worse than a recurring log line, so the long
  /// window won. Getting both would require restructuring this action to run across multiple
  /// scheduled `loop()` ticks so no single blocking unit approaches 2550 ms — not done here.
  /// @return Structured result whose `message` is the full report.
  ManagementActionResult scan_paired_devices();

  /// Native API callback: run a single diagnostic probe and publish the result as a HA event.
  void api_probe_device(const std::string &device_id, const std::string &probe, const std::string &index);

  /// @brief Send a single diagnostic probe frame to a paired device; delegates to
  /// DiagnosticProbes::probe_device(), which documents the probe names, the gating and the result.
  /// @param device_id Target device ID (hex string, case-insensitive).
  /// @param probe Probe name.
  /// @param index Probe index, a decimal or `0x`-prefixed hex string.
  /// @return Structured result carrying the raw reply.
  ManagementActionResult probe_device(const std::string &device_id, const std::string &probe, const std::string &index);

  /// Native API callback: run a bounded probe sweep and publish the result as a HA event.
  void api_probe_sweep(const std::string &device_id, const std::string &probe, const std::string &first_index,
                       const std::string &last_index);

  /// @brief Walk a bounded index range, one probe per index; delegates to DiagnosticProbes::probe_sweep().
  /// @param device_id Target device ID (hex string, case-insensitive).
  /// @param probe Probe name.
  /// @param first_index First index (inclusive).
  /// @param last_index Last index (inclusive).
  /// @return Structured result with one line per index.
  ManagementActionResult probe_sweep(const std::string &device_id, const std::string &probe,
                                     const std::string &first_index, const std::string &last_index);

  /// Native API callback: run a heating/climate function and publish the result as a HA event.
  void api_heating_control(const std::string &device_id, const std::string &function, const std::string &value);

  /// @brief Send one 2W heating/climate function (CMD_WRITE_PRIVATE 0x20) to a registered climate
  /// device.
  ///
  /// A real user feature, but an unvalidated one — the register protocol has never been exercised
  /// against real Atlantic/Thermor/Sauter hardware (see docs/configuration/climate.md's experimental
  /// banner). It is NOT diagnostic-gated; the guard is documentation, not `diagnostic_probes:`.
  ///
  /// `function` is one of `power_on` / `set_temperature` / `set_mode` / `set_presence` /
  /// `set_open_window_detection` / `midnight_sync` / `window_open`. `value` is parsed per function:
  /// a float in degrees Celsius (7.0-28.0) for `set_temperature`, `auto|manual|prog|off` for
  /// `set_mode`, `on|off` for `set_presence` and `set_open_window_detection`, ignored for the
  /// register reads. A malformed value is reported, never coerced.
  ///
  /// Delegates to IOHomeControlComponent::send_heating_command() (and, for `window_open`,
  /// read_heating_register()) so the entity and the action share one transmit path. `verified` is
  /// always false: an acknowledged write is not the device showing the new value. `window_open`
  /// reports the heater's window sensor in the result message; `power_on` and `midnight_sync` log
  /// their ACK payload at DEBUG.
  /// @param device_id Target device ID (hex string, case-insensitive).
  /// @param function Heating function name (see above).
  /// @param value Function-specific value string (see above).
  /// @return Structured result describing success and any validation/exchange failure.
  ManagementActionResult heating_control(const std::string &device_id, const std::string &function,
                                         const std::string &value);

  /// Publish a management result as one or more structured log lines (one call per line of
  /// `result.message`, see log_multiline_result() in log_helpers.h) and a Home Assistant event.
  void publish_result(const ManagementActionResult &result);

 private:
  /// @brief Shared preamble for every management action: validate the hub and device ID,
  /// then resolve the target device (see resolve_management_device()).
  ///
  /// Initializes `result` with the action name and normalized device ID, then checks (in
  /// order) that the hub is initialized, that `device_id` is a well-formed 6-hex-digit ID,
  /// and that it names a device registered on this hub. On any failure, sets `result.message`
  /// to a stable, caller-facing string and returns nullptr; callers should return `result`
  /// immediately in that case.
  /// @param action Action name to record on `result` (e.g. MANAGEMENT_ACTION_RENAME_DEVICE).
  /// @param device_id Requested device ID (hex string, case-insensitive, as received from HA).
  /// @param result Populated with the action name and normalized device ID, plus an error
  ///               message on failure.
  /// @return Pointer to the resolved device, or nullptr if any validation step failed.
  IoDevice *resolve_device_(const char *action, const std::string &device_id, ManagementActionResult &result);

  /// @brief Send an authenticated request and report a stock timeout failure on no response.
  ///
  /// Shared by every action that talks to the device directly (rename, identify): both send
  /// their built request via ExchangeEngine::send_and_receive() and, on failure, log the same
  /// exchange debug snapshot and report the same "no valid response to <action_verb> request"
  /// message shape. Actions that don't talk to the radio directly (force-open, which queues
  /// through the hub's cover-command dispatch path instead) have no use for this helper.
  /// @param request Frame to send.
  /// @param response Populated with the device's reply on success.
  /// @param action_verb Bare verb used only in the failure message, e.g. "rename" or "identify".
  /// @param result Result to update with `message` on failure; `result.device_id` must already be
  ///               set (via resolve_device_()) since it is used for the debug log.
  /// @return true if a response was received; false on timeout (result.message set, debug
  ///         snapshot logged).
  bool send_authenticated_request_(const IoFrame &request, IoFrame &response, const char *action_verb,
                                   ManagementActionResult &result);

  const uint8_t *node_id_;
  const uint8_t *system_key_;
  const TuningConfig *tuning_;
  ExchangeEngine &engine_;
  DeviceRegistry &registry_;
  const bool *initialized_;
  IOHomeControlComponent *hub_;
  DiagnosticProbes probes_;  ///< The opt-in probes; the registration table routes the two probe actions here.
};

}  // namespace home_io_control
}  // namespace esphome

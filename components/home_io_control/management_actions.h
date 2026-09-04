#pragma once

/// @file management_actions.h
/// @brief Hub-level management operations exposed as Home Assistant actions.
/// @ingroup hioc_hub
///
/// ManagementActions owns the family of advanced, non-entity hub actions (device rename,
/// identify, force-open, ...) and the native API registration that wires them into Home
/// Assistant. Every action is registered through one data-driven API service descriptor
/// (detail::ManagementServiceDescriptor); adding a new action does not require a new
/// descriptor class. The descriptor holds a ManagementActions* and calls only public
/// methods on it, so no friend declarations into the hub are needed.
///
/// IOHomeControlComponent exposes this collaborator through thin protected wrappers
/// (api_rename_device_, api_identify_device_, api_force_open_device_, api_scan_paired_devices_,
/// register_management_actions_).

#include "exchange_engine.h"
#include "device_registry.h"

#include <cstdint>
#include <string>

namespace esphome {
namespace home_io_control {

// Forward declarations — full definitions included only in management_actions.cpp.
class IOHomeControlComponent;
struct TuningConfig;

/// Result of a hub-level management action such as rename.
///
/// Returned by ManagementActions::rename_device() and re-exported from the hub
/// as IOHomeControlComponent::ManagementActionResult via a public type alias.
struct ManagementActionResult {
  bool success{false};           ///< Whether the requested management action succeeded.
  bool verified{false};          ///< Whether a follow-up readback confirmed the applied state.
  bool has_result_code{false};   ///< True when result_code contains a decoded CMD_ERROR_RESP byte.
  uint8_t result_code{0};        ///< Optional CMD_ERROR_RESP result byte.
  std::string action;            ///< Action name, e.g. "rename_device".
  std::string device_id;         ///< Target IO-homecontrol device ID.
  std::string message;           ///< Human-readable outcome summary.
  std::string requested_name;    ///< Requested normalized UTF-8 name for rename actions.
  std::string applied_name;      ///< Verified cached UTF-8 name after a readback, when available.
  std::string probe_name;        ///< Probe name for probe_device()/probe_sweep(), e.g. "private_fn".
  std::string probe_index;       ///< Requested index argument, as received (before parsing).
  bool has_response_cmd{false};  ///< True when response_cmd contains a probe reply's command byte.
  uint8_t response_cmd{0};       ///< Probe reply's command byte (distinct from a CMD_ERROR_RESP
                                 ///< result_code above, which is a different byte with a
                                 ///< different meaning).
  std::string response_hex;      ///< Probe reply's full raw wire hex, for pasting into
                                 ///< scripts/corpus/ingest.py.
  bool terminal_refusal{false};  ///< True when probe_device() failed for a reason that will
                                 ///< recur identically for every remaining index in a sweep
                                 ///< (diagnostic probes not enabled, device moving) — set only by
                                 ///< probe_device(), read only by probe_sweep() to decide
                                 ///< whether to stop early. A structured flag rather than
                                 ///< probe_sweep() pattern-matching `message` text, which would
                                 ///< silently break if that text were ever reworded.
};

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
  ///                     for each of its own per-channel attempts.
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
  /// @warning **Unconfirmed standalone on real hardware.** An enrolled device has been observed
  /// to ignore this frame sent alone — the hub kept controlling it afterwards. The frame itself
  /// matches every real captured `0x39` this project holds (payload shape, MAC span), so the most
  /// likely explanation is the same physical gate enrollment has: a device may only act on `0x39`
  /// while its receiver is in the 2 s PROG association-mode window, and the failed attempt was
  /// fired without it — untested, not ruled out. Do not treat this action as a confirmed rollback
  /// until it is retested with that gesture.
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
  /// Transmits the request once per channel (CH2, then CH1, then CH3), each with its own full
  /// `pairing_discovery_wait_ms` listen window, merging distinct responders across attempts — a
  /// paired device only hears the broadcast if it happens to be awake on the channel the hub
  /// transmits on at that instant, and real hardware testing found that single-channel
  /// duty-cycling paired devices are not reliably caught by a one-shot broadcast. A responder
  /// that answers more than one attempt still appears exactly once. Each attempt is a single
  /// transmit followed by its own window, never back-to-back transmits, so it does not
  /// reintroduce the different failure mode a 3-channel-burst transmit caused elsewhere: firing
  /// three long-preamble transmits back-to-back with no listening in between blew through the
  /// tight per-try response wait windows and broke exchanges in both directions (see
  /// `KeyExtractionResponder::broadcast_reply_()`'s doc comment).
  ///
  /// This still blocks the caller for the full three-window duration (roughly
  /// `3 × pairing_discovery_wait_ms`, ~6 s at the 2000 ms default) and therefore trips ESPHome's
  /// "operation took a long time" warning on *every* invocation. That warning uses a per-component
  /// ratchet (`Component::should_warn_of_blocking()`): it starts at 50 ms and, each time it fires,
  /// raises its own threshold to the observed duration plus a margin — but the threshold is stored
  /// in centiseconds in a `uint8_t`, so it saturates at **2550 ms**. Anything that blocks longer
  /// than that can never ratchet out of warning range.
  ///
  /// Accepting that is a deliberate tradeoff (confirmed on real hardware 2026-08-10). A shorter
  /// fixed window (500 ms/attempt, ~2.4 s total) was tried: it would have gone quiet after one
  /// warning, since 2.4 s sits under the 2550 ms cap — but it also caused real, correctly-decoded
  /// replies from registered devices to arrive after the window had already closed, where the
  /// passive path drops them (`hub_status.cpp`'s `unhandled_cmd` catch-all). Losing devices from
  /// the report is worse than a recurring log line, so the long window won. Getting both would
  /// require restructuring this action to run across multiple scheduled `loop()` ticks so no single
  /// blocking unit approaches 2550 ms — not done here.
  /// @return Structured result whose `message` is the full report.
  ManagementActionResult scan_paired_devices();

  /// Native API callback: run a single diagnostic probe and publish the result as a HA event.
  void api_probe_device(const std::string &device_id, const std::string &probe, const std::string &index);

  /// @brief Send a single diagnostic probe frame to an already-paired device and report the raw
  /// reply.
  ///
  /// Protocol-research instrumentation for opcodes this codebase has not decoded — see
  /// docs/radio_diagnostics.md and ADR 0024. Refuses unless
  /// this build was configured with `diagnostic_probes: true`
  /// (IOHomeControlComponent::diagnostic_probes_enabled()), and separately refuses while the
  /// target device is moving (`!effective_is_stopped(*dev)`) — an unknown frame into a
  /// mid-transaction device state machine is the one avoidable way a read-shaped probe could
  /// cause harm. That is the device's last *reported* movement state *or* a standing hub
  /// prediction that it is moving (a command the hub just issued, not yet confirmed): both
  /// warrant the same refusal. It is always "stopped" (never refuses) for a light/switch, and the
  /// reported state can be stale if the device was last moved from a physical remote the hub
  /// never saw. Routes
  /// the response straight into `result.message` as raw hex; it
  /// is never passed to the hub's status decoder (`update_device_status_()` is not reachable
  /// from this class at all — see hub_core.h), so a probe can never be misread as a position
  /// update. `verified` is always false: the whole point is that the reply's meaning is not yet
  /// known.
  ///
  /// `probe` selects the frame builder:
  ///   - "private_fn": create_private_function(), `index` = function ID (Q0).
  ///   - "private_fn_sub": create_private_function() at function ID 0x09, `index` = the second
  ///     payload byte (data[1]) — an undecoded parameter-addressing byte no project has ever
  ///     sent as non-zero.
  ///   - "status_ext": create_get_status_extended() at the field-observed selector 0x80,
  ///     `index` = block/N (Q1). The selector-0x20 tilt control is create_get_status_tilt()
  ///     (already shipped) and is not part of this probe.
  ///   - "status_ext_fn6" / "status_ext_fn9": create_get_status_extended() at selector 0x80 with
  ///     function ID 0x06 / 0x09 instead of the usual 0x03, `index` = block/N. The 4-byte shape
  ///     is field-observed; the function ID in it is not.
  ///   - "get_info1": create_get_info1() (CMD_GET_INFO1 0x54, no payload), `index` ignored.
  ///   - "get_info2": create_get_info2() (CMD_GET_INFO2 0x56, no payload), `index` ignored.
  ///   - "general_info3": create_general_info3(), `index` ignored (Q2).
  ///   - "private2": create_private2_read() long form, `index` = modifier (Q3).
  ///   - "private2_short": create_private2_read() short form, `index` = modifier (Q3).
  /// There is deliberately no "unknown4a" probe and no builder for CMD_UNKNOWN4A_REQ (0x4A)
  /// anywhere in this codebase — see ADR 0024 for the standing safety decision.
  ///
  /// `index` is a string, not an int, because ManagementServiceDescriptor::
  /// encode_list_service_response() hardcodes every native-API action argument as
  /// SERVICE_ARG_TYPE_STRING (see the .cpp) — parsed here via parse_probe_index(), which
  /// accepts both "6" and "0x06".
  /// @param device_id Target device ID (hex string, case-insensitive).
  /// @param probe One of the probe names above.
  /// @param index Function ID / selector block / modifier, depending on `probe`; a decimal or
  ///        `0x`-prefixed hex string. Ignored for "general_info3".
  /// @return Structured result whose `message` carries the attempt count, and on a reply, the
  ///         response command byte and full raw hex.
  ManagementActionResult probe_device(const std::string &device_id, const std::string &probe, const std::string &index);

  /// Native API callback: run a bounded probe sweep and publish the result as a HA event.
  void api_probe_sweep(const std::string &device_id, const std::string &probe, const std::string &first_index,
                       const std::string &last_index);

  /// @brief Walk a bounded index range, one probe_device() call per index, in one user gesture.
  ///
  /// Answers Q0/Q1/Q3 (the multi-value probes) without one native-API call per value. Bounded to
  /// PROBE_SWEEP_MAX_INDICES indices and spaced PROBE_SWEEP_DELAY_MS apart — this transmits on a
  /// shared ISM band to what may be a battery device, so a tight loop is antisocial and would
  /// also keep the device awake and skew results. Each index costs up to
  /// EXCHANGE_RETRY_COUNT frames (proto_timing.h), same as a single probe_device() call.
  ///
  /// @note This blocks the ESPHome loop (API, other components, OTA) for the whole sweep, same
  ///       blocking-exchange model as every other management action (ADR 0013) — but a full
  ///       sweep multiplies it: worst case (every index silent) is roughly
  ///       `PROBE_SWEEP_MAX_INDICES * (EXCHANGE_RETRY_COUNT * exchange_start_response_wait_ms +
  ///       (EXCHANGE_RETRY_COUNT-1) * EXCHANGE_RETRY_DELAY_MS + PROBE_SWEEP_DELAY_MS)`, on the
  ///       order of a minute at default tuning and longer if `exchange_start_response_wait_ms`
  ///       is raised. Accepted deliberately for this maintainer-triggered, explicitly-opted-in
  ///       diagnostic (`diagnostic_probes: true`) rather than restructured into scheduled steps — see
  ///       docs/radio_diagnostics.md's "Diagnostic probes" section, which states this to the
  ///       user rather than leaving it to be discovered as a frozen dashboard.
  /// @param device_id Target device ID (hex string, case-insensitive).
  /// @param probe One of probe_device()'s probe names.
  /// @param first_index First index in the sweep (inclusive), same string format as probe_device().
  /// @param last_index Last index in the sweep (inclusive).
  /// @return Structured result whose `message` is one line per index: answered / error-coded /
  ///         silent / refused.
  ManagementActionResult probe_sweep(const std::string &device_id, const std::string &probe,
                                     const std::string &first_index, const std::string &last_index);

  /// Native API callback: run a heating/climate function and publish the result as a HA event.
  void api_heating_control(const std::string &device_id, const std::string &function, const std::string &value);

  /// @brief Send one 2W heating/climate function (CMD_WRITE_PRIVATE 0x20) to a registered climate
  /// device.
  ///
  /// A real user feature, but an unvalidated one — the protocol is derived from the iohomecontrol
  /// project's Cozytouch support and has never been exercised against real Atlantic/Thermor/Sauter
  /// hardware (see docs/home_io_control.md's experimental banner). It is NOT diagnostic-gated;
  /// the guard is documentation, not `diagnostic_probes:`.
  ///
  /// `function` is one of `power_on` / `set_temperature` / `set_mode` / `set_presence` /
  /// `set_window` / `midnight_sync`. `value` is parsed per function: a float in degrees Celsius
  /// (7.0-28.0) for `set_temperature`, `auto|manual|prog|off` for `set_mode`, `on|off` for
  /// `set_presence`, `open|close` for `set_window`, ignored for the other two. A malformed value
  /// is reported, never coerced.
  ///
  /// Delegates to IOHomeControlComponent::send_heating_command() so the entity and the action
  /// share exactly one transmit path (see that method). `verified` is always false: the `set_*`
  /// functions are write-only — nothing decodes what the radiator did into an entity. (`power_on`
  /// and `midnight_sync` are register reads; their ACK payload is logged at DEBUG but not decoded.)
  /// @param device_id Target device ID (hex string, case-insensitive).
  /// @param function Heating function name (see above).
  /// @param value Function-specific value string (see above).
  /// @return Structured result describing success and any validation/exchange failure.
  ManagementActionResult heating_control(const std::string &device_id, const std::string &function,
                                         const std::string &value);

  /// Publish a management result as one or more structured log lines (one call per line of
  /// `result.message`, see log_multiline_result() in the .cpp) and a Home Assistant event.
  void publish_result(const ManagementActionResult &result);

 private:
  /// @brief Shared preamble for every management action: validate the hub and device ID,
  /// then resolve the target device.
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
};

}  // namespace home_io_control
}  // namespace esphome

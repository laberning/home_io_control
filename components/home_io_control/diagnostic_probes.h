#pragma once

/// @file diagnostic_probes.h
/// @brief Opt-in diagnostic probes: single frames and bounded sweeps for protocol research.
/// @ingroup hioc_hub
///
/// DiagnosticProbes owns probe_device(), probe_sweep() and the data-transport read behind the
/// `read_object` probe, with the table of probe names and frame builders. ManagementActions owns
/// one instance and registers the two native API actions that drive it; the actions exist only
/// with `diagnostic_probes: true` (docs/diagnostic-probes.md, ADR 0024).
///
/// The class reaches the hub through DiagnosticProbesHub, three injected calls, and includes no
/// hub header. None of the three applies a status to a device record, so a probe reply stays
/// raw: it can never be misread as a position update.

#include "device_registry.h"
#include "exchange_engine.h"
#include "management_helpers.h"
#include "proto_constants.h"
#include "proto_frame.h"

#include <cstdint>
#include <functional>
#include <string>

namespace esphome {
namespace home_io_control {

class RadioDriver;

/// Native API action names of the two probe actions, shared with ManagementActions' registration table.
inline constexpr const char *MANAGEMENT_ACTION_PROBE_DEVICE = "probe_device";
inline constexpr const char *MANAGEMENT_ACTION_PROBE_SWEEP = "probe_sweep";

/// @brief The only three things a probe needs from the hub, injected by ManagementActions.
struct DiagnosticProbesHub {
  std::function<bool()> probes_enabled;                     ///< True when `diagnostic_probes: true` was configured.
  std::function<RadioDriver *()> radio;                     ///< The radio, for the structured capture log line.
  std::function<bool(const IoFrame &)> record_reply_event;  ///< Records a device event report on the "Last Device
                                                            ///< Event" sensor; true when the frame was one.
};

/// @brief Sends diagnostic probe frames to already-paired devices and reports the raw replies.
///
/// Non-copyable for the same reason as ManagementActions: it holds references into hub members.
/// @ingroup hioc_hub
class DiagnosticProbes {
 public:
  /// Construct with the collaborators a probe exchange needs.
  /// @param node_id      Controller 3-byte node ID, owned by the hub.
  /// @param engine       Shared exchange engine for radio transactions.
  /// @param registry     Device registry for device lookups.
  /// @param initialized  Pointer to the hub's initialized flag.
  /// @param hub          The three injected hub calls.
  DiagnosticProbes(const uint8_t *node_id, ExchangeEngine &engine, DeviceRegistry &registry, const bool *initialized,
                   DiagnosticProbesHub hub);

  DiagnosticProbes(const DiagnosticProbes &) = delete;
  DiagnosticProbes &operator=(const DiagnosticProbes &) = delete;

  /// @brief Send a single diagnostic probe frame to an already-paired device and report the raw
  /// reply.
  ///
  /// Protocol-research instrumentation for opcodes this codebase has not decoded — see
  /// docs/diagnostic-probes.md and ADR 0024. Refuses unless
  /// this build was configured with `diagnostic_probes: true`
  /// (DiagnosticProbesHub::probes_enabled), and separately refuses while the
  /// target device is moving (`!effective_is_stopped(*dev)`) — an unknown frame into a
  /// mid-transaction device state machine is the one avoidable way a read-shaped probe could
  /// cause harm. That is the device's last *reported* movement state *or* a standing hub
  /// prediction that it is moving (a command the hub just issued, not yet confirmed): both
  /// warrant the same refusal. It is always "stopped" (never refuses) for a light/switch, and the
  /// reported state can be stale if the device was last moved from a physical remote the hub
  /// never saw. Routes the response straight into `result.message` as raw hex; it is never passed
  /// to the hub's status decoder (this class holds no hub pointer, only the three calls of
  /// DiagnosticProbesHub, and none of them applies a status), so a probe can never be misread as a
  /// position update. `verified` is always false: the whole point is that the reply's meaning is not yet
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
  ///   - "status_mp_fp": create_status_mp_fp_read() (CMD_PRIVATE function 0x01, FPI1 mask 0xFE:
  ///     main parameter plus FP1-FP7), `index` ignored. The reply is reported raw like every
  ///     probe reply and never applied to the device record (ADR 0024).
  ///   - "status_fp_high": the same request with FPI1 0x00 (main parameter only) and `index` as the FPI2
  ///     bitmap (bit 7 = FP9 ... bit 0 = FP16, 1 to 7 bits set; anything else is rejected). Not sweepable, an
  ///     index is a bitmap. The FPI2 variant has not been seen on air; the reply is reported raw and never applied.
  ///   - "limitation": create_limitation_status_read() (CMD_LIMITATION_STATUS_REQ 0x25), `index` 0 =
  ///     resulting minimum, 1 = resulting maximum (any other index is rejected, a sweep reaching
  ///     past 1 is rejected whole). The reply is reported raw plus a decode labelled "assumed
  ///     layout" in `ManagementActionResult::decoded`; it is never applied to the device record.
  ///   - "subscribe_event": create_subscribe_event() (CMD_SUBSCRIBE_EVENT 0x73), `index` 0 = battery
  ///     level event 0x2001, 1 = local sensor event 0x2005 (any other index is rejected). It changes
  ///     device state: it stores a subscription with no confirmed way to remove it, so it is not
  ///     sweepable and the builder accepts only those two events (ADR 0060). It is sent
  ///     once: a try the device authenticated is never repeated, so silence after acceptance is
  ///     reported as "accepted but no response". Whatever the device reports afterwards is recorded
  ///     by the device-event recorder, never applied to the device record.
  ///   - "unsubscribe_event": create_unsubscribe_event(), the same request with action byte 0x00,
  ///     same index mapping and the same guards. An untested guess at undoing a subscription; whether
  ///     it works shows only in whether the device's reports stop.
  ///   - "read_object": the data-transport read of ADR 0062. `index` is a row of TRANSFER_READABLE_OBJECTS:
  ///     0 = actuator capacity record (object 0x8100), 1 = actuator state record (0x8103), 2 = name record
  ///     (0x8003, there to prove the flow before the other two are trusted), 3 = general info 1 (0x8001),
  ///     4 = general info 2 (0x8002), 5 to 29 = further class-0 and Somfy manufacturer objects. One call
  ///     opens the object (CMD_TRANSFER_OPEN), pulls its blocks (CMD_UNKNOWN4A_REQ, counting down) and closes it; the
  ///     bytes are reported raw in `response_hex` and logged, never interpreted or applied to the device record. Not
  ///     sweepable; an object announced longer than PROBE_READ_OBJECT_MAX_BYTES is closed unread. The write twin of the
  ///     block (0x48, and a 0x4A that carries data) has no builder.
  ///   - "register_00" / "register_01": a read of register `0x00<index>` / `0x01<index>` over CMD_WRITE_PRIVATE
  ///     (`0C 60 <hi> <lo>`, ADR 0063). The direction byte is the read constant and cannot be set to the write
  ///     direction by any input (encode_heating_register_read()). Refused for a device that is neither a
  ///     register-protocol heater nor a domestic hot water device (device_accepts_register_probe()); sweepable.
  ///     A `0x21` that echoes the register also reports its value bytes raw in `decoded`. Family `0x00xx` is
  ///     untested; `0x01xx` is the one climate support reads.
  ///
  /// `index` is a string, not an int, because ManagementServiceDescriptor::
  /// encode_list_service_response() hardcodes every native-API action argument as
  /// SERVICE_ARG_TYPE_STRING (see management_actions.cpp) — parsed here via parse_probe_index()
  /// (diagnostic_probes.cpp), which accepts both "6" and "0x06".
  /// @param device_id Target device ID (hex string, case-insensitive).
  /// @param probe One of the probe names above.
  /// @param index Function ID / selector block / modifier, depending on `probe`; a decimal or
  ///        `0x`-prefixed hex string. Ignored for "general_info3".
  /// @return Structured result whose `message` carries the attempt count, and on a reply, the
  ///         response command byte and full raw hex.
  ManagementActionResult probe_device(const std::string &device_id, const std::string &probe, const std::string &index);

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
  ///       docs/diagnostic-probes.md, which states this to the
  ///       user rather than leaving it to be discovered as a frozen dashboard.
  /// @param device_id Target device ID (hex string, case-insensitive).
  /// @param probe One of probe_device()'s probe names.
  /// @param first_index First index in the sweep (inclusive), same string format as probe_device().
  /// @param last_index Last index in the sweep (inclusive).
  /// @return Structured result whose `message` is one line per index: answered / error-coded /
  ///         silent / refused.
  ManagementActionResult probe_sweep(const std::string &device_id, const std::string &probe,
                                     const std::string &first_index, const std::string &last_index);

 private:
  /// @brief Second half of the `read_object` probe: given the device's accepted CMD_TRANSFER_OPEN
  /// answer, pull every block, close the transfer and report what was read.
  ///
  /// Always closes a transfer the device opened, also after a failed block, so the device is not left
  /// holding an open handle. Writes the outcome into `result` (`response_hex`, `decoded`, `message`);
  /// the bytes are never decoded further and never reach the device record.
  /// @param dev The probed device.
  /// @param open_reply The device's answer to the open; anything but a CMD_TRANSFER_OPEN_RESP is left to
  ///                   the caller's own report and ends the read here.
  /// @param object The object that was opened (for the log and the message).
  /// @param result Receives the outcome appended to the first-exchange text already in it.
  void read_object_blocks_(const IoDevice &dev, const IoFrame &open_reply, const TransferObject &object,
                           ManagementActionResult &result);

  const uint8_t *node_id_;
  ExchangeEngine &engine_;
  DeviceRegistry &registry_;
  const bool *initialized_;
  DiagnosticProbesHub hub_;
};

}  // namespace home_io_control
}  // namespace esphome

#pragma once

/// @file roll_call.h
/// @brief The roll-call sweep: one keyed `0x2A` broadcast, repeated over channels and power classes.
/// @ingroup hioc_hub
///
/// Scan Paired Devices and (in a later change) a pull-key pairing both ask "which devices answer a
/// roll-call under this key?". This module is that question, with the key passed in explicitly:
/// the frame shapes, the channels, the reply decoding and the de-duplication live here once.

#include "device_registry.h"
#include "exchange_engine.h"
#include "proto_commands.h"
#include "proto_device_types.h"
#include "tuning_config.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace esphome {
namespace home_io_control {

/// @brief Maximum distinct responders reported by one scan_paired_devices() call.
///
/// Sized against the loop task's stack, which is where this runs: ESPHome spawns its own
/// `loopTask` with `ESPHOME_LOOP_TASK_STACK_SIZE` (8192 B) — not the 3.5 KB ESP-IDF main task.
/// `ScanResponder` is 12 B, so 24 slots cost ≈288 B (~3.5 % of that stack) in a single array;
/// replies are decoded on arrival rather than buffered as whole 33 B `IoFrame`s, which is what
/// makes a limit this size cheap. Installs with 20+ actuators are real, so 8 (the original
/// value, chosen when whole frames were retained) was too low to be useful. Overflow beyond
/// this is reported in the scan's own output, never silent.
inline constexpr uint8_t SCAN_MAX_REPLIES = 24;

/// @brief One roll-call responder, decoded on arrival.
///
/// Deliberately holds only what the report needs, so the fixed array stays small: the raw
/// `IoFrame` is discarded as soon as decode_discovery_response() has run, and the hex device-ID
/// string is rebuilt from `src` at format time rather than stored (a `std::string` member would
/// cost more per entry than this whole struct).
struct ScanResponder {
  uint8_t src[NODE_ID_SIZE];  ///< Responder's node ID; also the dedup key.
  DeviceType type;            ///< Decoded device type.
  uint8_t subtype;            ///< Decoded device subtype.
  int16_t rssi_dbm;           ///< RSSI of the reply that produced this entry.
  uint8_t manufacturer;       ///< Raw manufacturer ID; name via manufacturer_name().
  uint8_t flags;              ///< Multi Information Byte; decode with DISCOVERY_FLAGS_* masks.
  bool has_extended;          ///< Whether manufacturer/flags above are present.
  bool metadata_complete;     ///< Whether type/subtype were present in the payload.
  uint8_t key_slot;           ///< Which key of the sweep it answered under: 0 is the hub key, n the n-th
                              ///< per-device key (RollCallKeys).
};

/// @brief Channels scan_paired_devices() transmits its roll-call request on, one attempt per
/// channel, per pass (see @ref ROLL_CALL_PASSES).
///
/// A single broadcast on one fixed channel only reaches a paired device that happens to be
/// awake and listening on that exact channel at that exact instant — real hardware testing
/// found paired devices duty-cycle across all three channels independently of the hub, so a
/// one-shot broadcast on CH2 alone misses whichever devices are elsewhere in their cycle right
/// then. Retrying the same request on the other two channels gives every device up to three
/// chances per pass to be listening when the hub transmits. CH2 first since it is the protocol's
/// designated TX channel (see FREQ_CH2's doc comment) and therefore the most likely to catch a
/// reply on the first attempt.
///
/// This TX retry is a duty-cycle workaround, not a receive-side fix. The hub's listen path
/// separately extends its dwell on a detected preamble/sync rather than hopping mid-frame, which
/// is what keeps it from dropping replies it does hear.
inline constexpr uint32_t SCAN_CHANNELS[] = {FREQ_CH2, FREQ_CH1, FREQ_CH3};
inline constexpr uint8_t SCAN_CHANNEL_COUNT = sizeof(SCAN_CHANNELS) / sizeof(SCAN_CHANNELS[0]);

/// @brief One roll-call frame shape, and how to listen for its replies.
///
/// `low_power` for the frame builder is derived from `power_save_mode` (`== POWER_SAVE_LOW_POWER`)
/// rather than stored separately, and the pass name used in logs is
/// `power_save_mode_name(power_save_mode)` — one vocabulary, shared with the responder's own
/// self-reported power-save byte, instead of a second spelling table.
struct RollCallPass {
  uint8_t power_save_mode;     ///< POWER_SAVE_LOW_POWER or POWER_SAVE_ALWAYS_ALIVE: the class this pass calls.
  bool ack_capable;            ///< Set CTRL1_ACK on the 0x2A.
  ListenPolicy listen_policy;  ///< Channel policy for this pass's reply windows.
};

/// @brief The roll-call's two fixed frame shapes, low-power first.
///
/// Low-power pass first: a sleeper that answers late is still collected by the later
/// always-alive windows (collect_broadcast_responses() accepts any 0x2B addressed to us,
/// regardless of which pass is currently running); in the opposite order a late low-power reply
/// would fall off the end of the scan with nothing left to catch it.
///
/// Low-power pass, `CTRL1 = 0x30` (LOW_POWER | ACK): the exact header a real VELUX KLR300 uses
/// for its own roll-call, whose low-power shutters answered it. Setting CTRL1_LOW_POWER selects
/// LONG_PREAMBLE through the shared start-preamble rule (request_preamble_for()) — no preamble is
/// picked here. `ROTATE_ALL_CHANNELS`: those VELUX low-power replies were observed landing on the
/// request channel, which a skipping listen would miss entirely.
///
/// Always-alive pass, `CTRL1 = 0x00` at `normal_start_preamble`: an always-alive VELUX
/// installation ignores a 1024-byte start preamble and answers the 32-byte one; Somfy roll-call
/// replies land on the request channel about 1 in 149 times, so `ROTATE_SKIPPING_REQUEST` keeps
/// the window on the channels replies actually use.
///
/// Both shapes are fixed, named constants: `pairing_discovery_ack_capable` belongs to Discover &
/// Pair and does not influence the roll-call either way.
inline constexpr RollCallPass ROLL_CALL_PASSES[] = {
    {POWER_SAVE_LOW_POWER, /*ack_capable=*/true, ListenPolicy::ROTATE_ALL_CHANNELS},
    {POWER_SAVE_ALWAYS_ALIVE, /*ack_capable=*/false, ListenPolicy::ROTATE_SKIPPING_REQUEST},
};

/// A broadcast has no pinned conversation to hold a channel for, so HOLD_REQUEST_CHANNEL can
/// never be a valid roll-call listen policy — checked at compile time rather than at every call
/// site.
constexpr bool no_roll_call_pass_holds_request_channel() {
  return std::ranges::all_of(ROLL_CALL_PASSES, [](const RollCallPass &pass) {
    return pass.listen_policy != ListenPolicy::HOLD_REQUEST_CHANNEL;
  });
}
static_assert(no_roll_call_pass_holds_request_channel(),
              "a broadcast roll-call pass has no pinned conversation to hold the request channel for");

/// @brief Outcome of one roll-call attempt or sweep, so the caller can surface a build failure.
enum class RollCallAttemptResult : uint8_t {
  OK,            ///< Request built and transmitted (with or without replies).
  BUILD_FAILED,  ///< create_discovery_request() failed; the caller should abort the whole scan.
};

/// @brief Accumulated state of one scan, threaded through every sweep.
struct ScanState {
  ScanResponder responders[SCAN_MAX_REPLIES];  ///< Distinct responders, in arrival order.
  uint8_t count{0};                            ///< Entries in use.
  bool truncated{false};                       ///< A responder was dropped because the array was full.
};

/// @brief How many of ROLL_CALL_PASSES `selection` actually calls.
/// @param selection The scan_power_classes tunable.
uint8_t count_enabled_passes(ScanPowerClasses selection);

/// @brief Outcome of add_scan_responder(), so callers can tell a harmless repeat from real loss.
enum class ScanAddResult : uint8_t {
  ADDED,      ///< New responder recorded.
  DUPLICATE,  ///< Already recorded from an earlier reply; nothing changed.
  FULL,       ///< Dropped: SCAN_MAX_REPLIES distinct responders already recorded.
};

/// @brief Record a responder unless its address is already present.
///
/// Deduplication is by node ID across the whole scan (all keys, all passes), so a device that
/// answers several of the attempts still yields one entry. The duplicate check runs before the
/// capacity check so that repeat replies from already-recorded devices never look like overflow
/// once the array is full.
/// @param responders Accumulated array, appended to in place.
/// @param count In: entries already present. Out: updated count.
/// @param capacity Maximum entries `responders` can hold.
/// @param frame Reply frame to decode and store.
/// @param rssi_dbm RSSI of that reply.
/// @param key_slot The key slot of the sweep the reply arrived in.
/// @param entry Out: the newly recorded entry (ADDED), the already-recorded entry with the same
///        `src` (DUPLICATE), or nullptr (FULL) — lets a caller read back the stored
///        manufacturer/flags without decoding the frame a second time.
/// @return Which of the three outcomes occurred.
ScanAddResult add_scan_responder(ScanResponder *responders, uint8_t &count, uint8_t capacity, const IoFrame &frame,
                                 int16_t rssi_dbm, uint8_t key_slot, const ScanResponder *&entry);

/// @brief Run the full roll-call sweep under one key: every enabled pass on every channel.
///
/// Outer loop over passes (low-power first, see ROLL_CALL_PASSES), skipping any pass the
/// scan_power_classes tunable excludes; inner loop over SCAN_CHANNELS. Every attempt always runs,
/// even once the array is full: stopping early would silently skip channels/passes, which is
/// exactly the single-channel behaviour the repeated attempts exist to avoid.
/// @param engine Exchange engine that transmits and collects the replies.
/// @param node_id The hub's node ID.
/// @param key The AES_KEY_SIZE-byte key the request is built with; only devices holding it answer.
/// @param key_slot Slot recorded on every responder this sweep adds (0 = hub key).
/// @param tuning Live tuning (reply window, power-class selection).
/// @param state Accumulated scan state, appended to.
/// @param attempt In/out: the global 1-based attempt counter across sweeps (0 before the first).
/// @param total_attempts Attempts the whole scan will run, for the "n/N" log line.
/// @return BUILD_FAILED if a request could not be built; OK otherwise.
RollCallAttemptResult run_roll_call_sweep(ExchangeEngine &engine, const uint8_t *node_id, const uint8_t *key,
                                          uint8_t key_slot, const TuningConfig &tuning, ScanState &state,
                                          uint8_t &attempt, uint8_t total_attempts);

/// @brief The keys a responder line needs: the one a responder answered under, and the hub's.
struct RollCallReplyKeys {
  const uint8_t *answered;  ///< The key of the sweep the responder answered in (always set).
  const uint8_t *hub;       ///< The hub key (slot 0).
  bool per_device;          ///< True when `answered` is a per-device key, not the hub's.
};

/// @brief How a responder line shows the key it answered under.
enum class RollCallKeyDisplay : uint8_t {
  HIDDEN,    ///< Never print the key: its check value only. For anything Home Assistant records.
  BANNERED,  ///< Print the key in a snippet, wrapped in the line's own DO-NOT-SHARE block.
  PLAIN,     ///< Print the key in a snippet with no framing: the caller's report supplies one banner.
};

/// @brief Format one roll-call responder's report line(s).
///
/// Known responders get a single summary line, plus an indented `hint:` line when their
/// self-reported power-save class disagrees with the registered device's YAML `low_power`
/// property (the responder must carry extended metadata for the comparison to be possible at
/// all), or when they answered under a key other than the one their YAML uses. Unknown responders
/// get the same summary line plus a lead-in sentence and a ready-to-paste YAML block (or, if the
/// decoded type has no ESPHome platform yet, a "not supported yet" line instead of a blank) — the
/// same "paste this in" framing a successful pairing prints. A responder found under a per-device
/// key gets that key's check value on its summary line; `display` decides whether its snippet
/// carries the key itself.
/// @param responder Decoded responder record.
/// @param device_id Hex device ID string for this responder, rebuilt from `responder.src`.
/// @param registered The matching registry entry, or nullptr if this responder is not known.
/// @param keys Which key the responder answered under and what the hub key is.
/// @param display Whether and how the key is printed.
/// @return The report text, ending in a newline.
std::string format_roll_call_responder(const ScanResponder &responder, const std::string &device_id,
                                       const IoDevice *registered, const RollCallReplyKeys &keys,
                                       RollCallKeyDisplay display);

/// @brief What build_pulled_key_report() says about the device that handed over a key.
struct PulledKeyReportInfo {
  std::string pairing_device_id;  ///< Hex node ID of the device that sent the key.
  DeviceType type;                ///< Its node type from discovery.
  uint8_t subtype;                ///< Its subtype from discovery.
  const uint8_t *key;             ///< The pulled key (AES_KEY_SIZE bytes) it proved it holds.
  const uint8_t *hub_key;         ///< The hub's own key.
};

/// @brief Build the report for a pull that returned the hub's own key from a node no platform drives.
///
/// The devices that hold the hub key were found by a roll-call under it, or by a direct read of the
/// address after an Atlantic pairing node. Their blocks need no key and no banner, so they read like a
/// scan's unknown responders.
/// @param info The device that sent the key.
/// @param state The devices found under the hub key.
/// @param registry Device registry before any registration: a responder the YAML declares is known; one
///        an earlier pairing in this boot added is not (DeviceRegistry::get_declared()).
/// @return The report, or an empty string when nothing answered.
std::string build_hub_key_pull_report(const PulledKeyReportInfo &info, const ScanState &state,
                                      DeviceRegistry &registry);

/// @brief Build the one report a proven key pull prints after looking for the devices that hold the key.
///
/// One DO-NOT-SHARE banner frames the whole report (device log only; it carries the key). Each
/// responder gets its summary line and a paste-ready block with `system_key:` added
/// (RollCallKeyDisplay::PLAIN); a responder of a type no platform drives yet gets the "not supported
/// yet" line and its key. With no responder, the report gives the key once for the pairing node
/// itself. Nothing here registers a device.
/// @param info The device that sent the key.
/// @param state The devices found under that key (roll-call, or the direct read of the next address).
/// @param registry Device registry as it stands before any registration: a responder the YAML declares
///        is reported as known; one an earlier pairing in this boot added gets its block again.
std::string build_pulled_key_report(const PulledKeyReportInfo &info, const ScanState &state, DeviceRegistry &registry);

}  // namespace home_io_control
}  // namespace esphome

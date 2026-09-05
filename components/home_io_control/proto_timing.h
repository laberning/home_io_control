#pragma once

/// @file proto_timing.h
/// @brief Physical-layer radio and timing parameters for the IO-Homecontrol protocol.
/// @ingroup hioc_protocol

#include <cstdint>

namespace esphome {
namespace home_io_control {

// ============================================================================
// Physical Layer — Radio Parameters
// ============================================================================

/// The protocol uses 3 frequency channels in the 868 MHz ISM band.
/// IO-Homecontrol uses 3 channels in the 868 MHz SRD band. In 1W (one-way) mode,
/// only CH2 is used. In 2W (two-way) mode, the controller hops across all three
/// channels every ~2.7ms when idle. Commands are sent on CH2; responses may arrive
/// on any channel within the exchange wait window.
static constexpr uint32_t FREQ_CH1 = 868250000;  ///< Channel 1: 868.25 MHz (2W only)
static constexpr uint32_t FREQ_CH2 = 868950000;  ///< Channel 2: 868.95 MHz (1W and 2W, TX channel)
static constexpr uint32_t FREQ_CH3 = 869850000;  ///< Channel 3: 869.85 MHz (2W only)

/// Preamble is a sequence of 0xAA bytes that precedes every frame.
/// A directed start frame to a **low-power** target (`CTRL1_LOW_POWER` set) uses the long preamble
/// (1024 bytes = 8192 bits) as a wake-up burst for its duty-cycled receiver; every other start
/// frame uses the runtime-tunable `normal_start_preamble`, which an always-listening receiver
/// detects fine. Subsequent frames in the same exchange use a short preamble (8 bytes) since both
/// sides are already on the same channel. The exchange engine derives all of this from the frame
/// (exchange_engine.cpp).
static constexpr uint16_t LONG_PREAMBLE = 1024;  ///< 1024 bytes: wake-up burst for a low-power start frame
static constexpr uint16_t SHORT_PREAMBLE = 8;    ///< 8 bytes for response/continuation frames

/// Default for `TuningConfig::cold_broadcast_reply_preamble` — preamble for a *broadcast* reply to
/// a frame the peer caught via a rotating/hopping listen (currently: only the key-extraction
/// responder's 0x29, the sole `start=true` frame any device-role builder in this codebase
/// produces) — long enough to be reliably caught by a hopping receiver, short enough that
/// broadcasting it across all 3 channels doesn't block the loop the way LONG_PREAMBLE does.
///
/// Sized above the ~13.9 ms real-device discovery-reply preamble this project's own SX1262/LR1121
/// discovery hop-slice tuning was measured against (issue #65 SDR analysis) — that measurement is
/// what "reliably caught by a hopping receiver" is calibrated to here. 80 bytes ≈ 16.7 ms at the
/// protocol's 38400 bps line rate (soft_phy_air_time_us()),
/// budgeted as a starting point, not yet hardware-validated for this exact chip/scenario
/// combination. Runtime-tunable via `cold_broadcast_reply_preamble` for exactly that reason.
static constexpr uint16_t COLD_BROADCAST_REPLY_PREAMBLE = 80;

/// Default for `TuningConfig::normal_start_preamble` — the preamble in front of a directed *start*
/// frame whose target is **not** a low-power / duty-cycled device (`CTRL1_LOW_POWER` clear). An
/// always-listening receiver does not need the ~213 ms `LONG_PREAMBLE` wake-up burst, and some
/// receivers never lock onto one that long; a normal start frame gets this shorter preamble
/// instead, matching what a reference hub sends to an always-alive device.
///
/// 32 bytes = 256 bits sits inside the preamble band the iown-homecontrol documentation describes
/// (256 bits in its radio notes; 128 bits is the "Long PPDU" preamble in its link-layer notes),
/// well above the ~12-byte response preamble a
/// short-turnaround chip uses, ~6.7 ms of air time, and two orders of magnitude below the 1024-byte
/// burst. 8 bytes is a proven lower bound against paired always-alive devices but nothing bounds
/// where a start frame stops being heard, so 32 is the defensible middle — and `normal_start_preamble`
/// is a live tuning knob so a wrong guess costs a number change, not a rebuild.
static constexpr uint16_t NORMAL_START_PREAMBLE = 32;

/// Default for `TuningConfig::pairing_discovery_preamble` — the preamble on the pairing discovery
/// broadcast (`CMD_DISCOVER_REQ`/`CMD_DISCOVER_ALT_REQ`, 0x28/0x2E). Defaults to `LONG_PREAMBLE`,
/// unchanged from historical behavior: a factory-fresh device in learning mode is exactly the kind
/// of duty-cycled receiver `LONG_PREAMBLE` exists to wake. But unlike every other directed start
/// frame (see `NORMAL_START_PREAMBLE`'s history, issue #87 — some always-alive receivers never
/// lock onto a preamble this long), the discovery broadcast can't be made power-class-aware the
/// same way: discovery exists to learn a device before anything is known about it, so it still
/// unconditionally pays `LONG_PREAMBLE`'s ~213 ms even against an always-listening target. Issue
/// #27 (Somfy Sunea IO devices repeatedly failing to answer discovery) raised this as a plausible,
/// unconfirmed contributor. Runtime-tunable via `pairing_discovery_preamble` so that hypothesis is
/// testable without a rebuild — not yet hardware-confirmed as a fix for any specific device.
static constexpr uint16_t PAIRING_DISCOVERY_PREAMBLE = LONG_PREAMBLE;

/// Preamble/sync linger extension for a rotating listen (`ListenSpec::linger_dwell_ms`): how much
/// longer to stay on a channel once a frame is visibly incoming, so a hop doesn't cut it off
/// mid-reception. Sized to a frame's air time, not to a hop slice, so it does not need to change
/// when a chip's hop slice does. Shared by every rotating listen (pairing discovery, broadcast
/// roll-call): both wait for the same class of short protocol frame, so there is no measured
/// reason for them to differ.
static constexpr uint32_t PREAMBLE_LINGER_DWELL_MS = 15;
// Chip-specific defaults (response preamble, post-TX settle, per-chip discovery hop slices
// for SX1262 and LR1121) live beside their TuningConfig fields in tuning_config.h; the
// SX1262/LR1121 exchange dwell constants live in radio_sx1262.h / radio_lr1121.h respectively.
// Kept out of the generic protocol layer per the radio-timing layering cleanup — this header
// holds only chip-neutral protocol values.

/// Timing constants for frequency hopping and response waiting.
static constexpr int32_t HOP_TIME_US = 2700;      ///< Time per channel when hopping (2.7ms)
static constexpr int32_t RESPONSE_WAIT_MS = 500;  ///< Wait for response to non-start frame

/// Wait for a response to a start frame — the first frame of an exchange, and the one a sleeping
/// device has just been woken by.
///
/// This device class replies within a few milliseconds of the carrier dropping, or not at all —
/// it is fast-or-never, not slow. A failure therefore shows up as `saw_challenge=0` with no frame
/// received at all, rather than as a late arrival, so a longer window cannot fix a device that
/// genuinely fails to answer.
///
/// 400 ms sits comfortably above every directly measured reply while keeping a failed exchange
/// inside EXCHANGE_TOTAL_BUDGET_MS, so a dead device does not block the ESPHome loop past its own
/// warning threshold (ADR 0013). Raise `exchange_start_response_wait_ms` from YAML if a device ever
/// genuinely answers late — but check `wait_ms` in the logs first, since a fast-or-never device is a
/// turnaround problem that a longer window cannot fix.
static constexpr int32_t RESPONSE_START_WAIT_MS = 400;

static constexpr int32_t RESPONSE_AUTH_WAIT_MS =
    RESPONSE_WAIT_MS;                                    ///< Wait for final response after challenge response
static constexpr int32_t EXCHANGE_RETRY_DELAY_MS = 250;  ///< Gap between retries within one HA command
static constexpr uint8_t EXCHANGE_RETRY_COUNT = 3;       ///< Attempts per command before reporting failure

/// Exchange tries for a status poll the scheduler owns — every status poll issued while
/// StatusPollPolicy is tracking the device, which today is every status poll this component can
/// produce (there is no user-facing "refresh status" button or action; if one is ever added, it
/// must not take this branch).
///
/// Such a poll's failure is re-armed by the backoff ladder (STATUS_RETRY_AFTER_FAIL_MS and its
/// successors), so the ladder *is* its retry mechanism; stacking EXCHANGE_RETRY_COUNT blocking
/// in-exchange tries on top of it buys no freshness and costs ~1.6 s of blocked loop() while a
/// device is unresponsive (e.g. an actuator mid-manoeuvre) — the settle poll fires seconds after a
/// command, squarely inside the manoeuvre, so keeping it a single try is what lets a STOP a user
/// presses mid-move dispatch promptly. A poll with no ladder behind it keeps the full
/// EXCHANGE_RETRY_COUNT. See SCHEDULED_POLL_RETRY_GRACE_FIRST_FAILURE below for the one place this
/// trade-off is deliberately bought back.
static constexpr uint8_t SCHEDULED_POLL_MAX_TRIES = 1;

/// Ladder positions at which a scheduler-owned status poll gets the full EXCHANGE_RETRY_COUNT back.
///
/// The single try above is right at both ends of the backoff ladder and wrong in the middle. At the
/// first slot after a command the device is still executing the manoeuvre: its silence is expected,
/// retries cannot change that, and blocking loop() for the full retry product would delay a STOP
/// the user presses mid-move. Once a device has missed several slots in a row it is unreachable
/// rather than merely asleep, and retries are just as pointless. In between sits the slot where the
/// manoeuvre has just ended and the device is awake again but duty-cycled — one 400 ms listen
/// samples its receive window once; EXCHANGE_RETRY_COUNT tries sample it three times, ~870 ms apart,
/// and a success there also clears the failure streak and ends the backoff.
///
/// Counted in consecutive silent failures already recorded when the poll is dispatched, so 0 is the
/// post-command settle poll and 1..3 are the ~5 s / ~15 s / ~30 s ladder slots after it — roughly
/// t+8 s to t+53 s, spanning every cover travel time this project has measured. An auth-shaped
/// streak is excluded entirely: a device that answers with a 0x3C challenge is awake, so extra
/// tries buy no wake-up, and an auth try is the most expensive shape the engine runs.
static constexpr uint8_t SCHEDULED_POLL_RETRY_GRACE_FIRST_FAILURE = 1;
static constexpr uint8_t SCHEDULED_POLL_RETRY_GRACE_LAST_FAILURE = 3;

/// Wall-clock ceiling on one whole exchange, retries included.
///
/// EXCHANGE_RETRY_COUNT tries x (long preamble + response window + retry gap) is what actually
/// determines how long a failing command blocks the ESPHome loop, and that blocking also starves
/// the receive path the rest of the exchange depends on. ESPHome itself warns when one operation
/// takes longer than 2550 ms (ADR 0013); this budget must stay under that threshold.
///
/// So the retry count is a maximum, not a promise: a try only starts if the exchange has budget
/// left. At the current 400 ms response window all three tries still fit (~2.3 s); raising the
/// window well past the default is what starts trimming retries, since three full tries stop being
/// affordable at that point — one long listen is the better trade there anyway.
static constexpr uint16_t EXCHANGE_TOTAL_BUDGET_MS = 2500;

/// One-way (1W) transmit cadence.
///
/// A 1W command is fire-and-forget: nothing replies, so there is no acknowledgement to retry on
/// and no way to learn a frame was missed. Repetition *is* the reliability mechanism — real
/// remotes send the same frame four times, and a receiving device treats the set as one command
/// because all four carry the same sequence. These are protocol values shared by every 1W
/// transmitter, not radio tuning: they do not vary by chip and must not be moved into a driver or
/// a TuningConfig field.
///
/// Both values come from the reference implementation, which sets them on adjacent lines when it
/// forges a 1W packet: `packet->repeat = 4` and `packet->repeatTime = 40` in its 1W remote. The
/// capture logs embedded in that same source show
/// consecutive copies of one burst arriving roughly 25 ms apart, which is not a contradiction:
/// those are receive-side timestamps of a burst whose configured gap is 40 ms, so they measure
/// something else. Do not "correct" 40 down to 25 on the strength of them.
///
/// Erring long would in any case be the safe direction — a device needs only one of the four
/// copies to land, so a wider spacing costs nothing and leaves more room on a shared band.
///
/// The resulting burst duration — see ONEWAY_BURST_INTERVAL_MS's own comment and send_burst()'s
/// doxygen (oneway_transmitter.h) for the two numbers this decomposes into — is what
/// ONEWAY_QUIET_PERIOD_MS (status_poll_policy.h) is sized against when it holds background polls
/// back during 1W activity.
static constexpr uint8_t ONEWAY_BURST_REPEATS = 4;  ///< Copies of each 1W command sent per press.
/// Gap between those copies. Three gaps between four copies is 3 * 40 = 120 ms of pure *delay*;
/// adding each of the four copies' own airtime brings the wall-clock burst closer to ~160 ms.
/// send_burst()'s doxygen (oneway_transmitter.h) states both — they are not competing claims about
/// the same number, just two different things measured on the same burst.
static constexpr uint32_t ONEWAY_BURST_INTERVAL_MS = 40;

/// Listen-before-talk (LBT) parameters for ETSI EN 300 220 compliance.
/// Before transmitting, the radio checks that the channel RSSI is below the
/// threshold. If the channel is busy, TX is deferred by LBT_RETRY_DELAY_MS
/// up to LBT_MAX_RETRIES times.
static constexpr int16_t LBT_RSSI_THRESHOLD_DBM = -90;  ///< Channel-free threshold (ETSI: ≤ -90 dBm)
static constexpr uint8_t LBT_MAX_RETRIES = 5;           ///< Max carrier-sense attempts before TX anyway
static constexpr uint8_t LBT_RETRY_DELAY_MS = 5;        ///< Backoff between LBT checks (≥ 5ms per ETSI)

/// Canonical defaults for the chip-neutral runtime-tunable pairing/discovery parameters.
///
/// These are the single source of truth for the diagnostics tuning layer: `TuningConfig`
/// initializes its fields from them, and the ESPHome YAML schema falls back to them when a
/// key is omitted. Adjust a value here and both the compiled default and the documented
/// YAML default follow. Chip-specific tuning defaults live in tuning_config.h instead.
/// See @ref hioc_tuning.
static constexpr uint16_t PAIRING_DISCOVERY_WAIT_MS = 2000;  ///< Wait window after sending each discovery command.
static constexpr uint16_t PAIRING_DISCOVERY_INITIAL_DWELL_MS = 300;  ///< Dwell on CH2 before discovery hopping begins.
static constexpr uint8_t PAIRING_KEY_EXCHANGE_RETRIES = 3;  ///< Retries for the authenticated key-exchange phase.

}  // namespace home_io_control
}  // namespace esphome

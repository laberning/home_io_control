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
/// The first frame in an exchange uses a long preamble (1024 bytes = 8192 bits)
/// so the receiver has time to detect it while hopping. Subsequent frames in the
/// same exchange use a short preamble (8 bytes) since both sides are already
/// on the same channel. Solar-powered devices need the long preamble to wake up.
static constexpr uint16_t LONG_PREAMBLE = 1024;  ///< 1024 bytes for initial/start frames
static constexpr uint16_t SHORT_PREAMBLE = 8;    ///< 8 bytes for response/continuation frames
// Chip-specific defaults (response preamble, post-TX settle, per-chip discovery hop slices
// for SX1262 and LR1121) live beside their TuningConfig fields in tuning_config.h; the
// SX1262/LR1121 exchange dwell constants live in radio_sx1262.h / radio_lr1121.h respectively.
// Kept out of the generic protocol layer per the radio-timing layering cleanup — this header
// holds only chip-neutral protocol values.

/// Timing constants for frequency hopping and response waiting.
static constexpr int32_t HOP_TIME_US = 2700;             ///< Time per channel when hopping (2.7ms)
static constexpr int32_t RESPONSE_CHANNEL_WAIT_MS = 50;  ///< Per-channel dwell while waiting for an exchange response
static constexpr int32_t RESPONSE_WAIT_MS = 500;         ///< Wait for response to non-start frame

/// Wait for a response to a start frame — the first frame of an exchange, and the one a sleeping
/// device has just been woken by.
///
/// Sized from measurement, not from a worst case. On 2026-08-14 the hub logged its own
/// request→reply latency (`Auth challenge ... wait_ms=`) across a run against a Somfy RS100: 235,
/// 486, 486, 236, 486 ms — try 1 always ~235 ms, try 2 always ~486 ms, which is 235 plus the
/// EXCHANGE_RETRY_DELAY_MS gap. Zero variance. Subtract the 213 ms long preamble and the device is
/// answering within a few milliseconds of the carrier dropping.
///
/// The device is therefore *not* slow: it replies almost immediately or not at all, and a failure
/// shows up as `saw_challenge=0` with no frame received at all rather than as a late arrival. An
/// earlier 1000 ms value here was set from a much larger apparent latency spread (29 ms–3052 ms),
/// which turned out to be an artifact of pairing frames by proximity in a third-party sniff with
/// three controllers sharing one channel — the pairings were wrong. Nothing was ever caught later
/// than ~235 ms once the hub measured it directly.
///
/// 400 ms is comfortably above every observed reply while keeping a failed exchange inside
/// EXCHANGE_TOTAL_BUDGET_MS, so a dead device no longer blocks the ESPHome loop past its own
/// warning threshold (ADR 0013). Raise `exchange_start_response_wait_ms` from YAML if a device
/// ever genuinely answers late — but check `wait_ms` in the logs first, because a fast-or-never
/// device is a turnaround problem and a longer window cannot fix it.
static constexpr int32_t RESPONSE_START_WAIT_MS = 400;

static constexpr int32_t RESPONSE_AUTH_WAIT_MS =
    RESPONSE_WAIT_MS;                                    ///< Wait for final response after challenge response
static constexpr int32_t EXCHANGE_RETRY_DELAY_MS = 250;  ///< Gap between retries within one HA command
static constexpr uint8_t EXCHANGE_RETRY_COUNT = 3;       ///< Attempts per command before reporting failure

/// Wall-clock ceiling on one whole exchange, retries included.
///
/// EXCHANGE_RETRY_COUNT tries x (long preamble + response window + retry gap) is what actually
/// determines how long a failing command blocks the ESPHome loop (ADR 0013). Once
/// RESPONSE_START_WAIT_MS grew to cover slow devices, three full tries reached ~4.2 s and tripped
/// ESPHome's own "took a long time for an operation" warning (threshold 2550 ms) on every failure
/// -- observed in the field on 2026-08-14 with a hub driving ~18 shutters, where that blocking
/// also starves the receive path the rest of the exchange depends on.
///
/// So the retry count is a maximum, not a promise: a try only starts if the exchange has budget
/// left. At the current 400 ms window all three tries still fit (~2.3 s); it only starts trimming
/// them if the window is raised well past the default, which is exactly when three full tries stop
/// being affordable. One long listen is the better trade there anyway.
static constexpr uint16_t EXCHANGE_TOTAL_BUDGET_MS = 2500;

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

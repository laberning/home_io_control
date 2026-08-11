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
static constexpr int32_t RESPONSE_START_WAIT_MS = 300;   ///< Wait for response to start frame (longer)
static constexpr int32_t RESPONSE_AUTH_WAIT_MS =
    RESPONSE_WAIT_MS;                                    ///< Wait for final response after challenge response
static constexpr int32_t EXCHANGE_RETRY_DELAY_MS = 250;  ///< Gap between retries within one HA command
static constexpr uint8_t EXCHANGE_RETRY_COUNT = 3;       ///< Attempts per command before reporting failure

/// One-way (1W) transmit cadence.
///
/// A 1W command is fire-and-forget: nothing replies, so there is no acknowledgement to retry on
/// and no way to learn a frame was missed. Repetition *is* the reliability mechanism — real
/// remotes send the same frame four times, 40 ms apart, and a receiving device treats the set as
/// one command because all four carry the same sequence. These are protocol values shared by
/// every 1W transmitter, not radio tuning: they do not vary by chip and must not be moved into a
/// driver or a TuningConfig field.
///
/// The resulting ~160 ms burst is what ONEWAY_QUIET_PERIOD_MS (status_poll_policy.h) is sized
/// against when it holds background polls back during 1W activity.
static constexpr uint8_t ONEWAY_BURST_REPEATS = 4;        ///< Copies of each 1W command sent per press.
static constexpr uint32_t ONEWAY_BURST_INTERVAL_MS = 40;  ///< Gap between those copies.

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

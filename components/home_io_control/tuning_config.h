#pragma once

/// @file tuning_config.h
/// @brief Runtime tuning configuration for pairing and radio diagnostics.
/// @ingroup hioc_tuning
///
/// Provides a centralized, non-persistent tuning layer used by the hub and radio
/// drivers. The `TuningConfig` struct carries all tunable parameters and helpers
/// for logging and applying defaults.

#include "proto_frame.h"
#include "proto_timing.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace esphome {
namespace home_io_control {

/// @brief Valid SX1262 RX bandwidth options (kHz register values).
///
/// The numeric values are the register-encoded (double-sideband) bandwidth selectors used by
/// `RadioSX1262::set_rx_bandwidth()` — a regular (mantissa, exponent) grid, with the bandwidth
/// roughly doubling per group.
///
/// Byte-for-byte identical to `LR1121RxBandwidth` below, since both chips share the same Semtech
/// GFSK bandwidth grid; the `Sx1262AndLr1121BandwidthTablesAgree` test pins that. If these two
/// tables ever need to diverge for a real chip difference, say why here.
enum class SX1262RxBandwidth : uint8_t {
  BW_39_0_KHZ = 0x1C,   ///< 39.0 kHz — narrowest; closest to the SX1276's validated 41.7 kHz.
  BW_46_9_KHZ = 0x14,   ///< 46.9 kHz — narrow.
  BW_58_6_KHZ = 0x0C,   ///< 58.6 kHz — default; the narrowest value validated on real hardware here.
  BW_78_2_KHZ = 0x1B,   ///< 78.2 kHz — just above the ~77 kHz Carson figure for this waveform.
  BW_117_3_KHZ = 0x0B,  ///< 117.3 kHz — the former default.
  BW_156_2_KHZ = 0x1A,  ///< 156.2 kHz.
  BW_187_2_KHZ = 0x12,  ///< 187.2 kHz — widest selectable option.
};

/// @brief Valid SX1276 RX bandwidth options (RegRxBw register bytes).
///
/// The numeric values are the SX1276 RegRxBw encodings (RxBwMant in bits[4:3], RxBwExp in
/// bits[2:0]) written verbatim to both REG_RX_BW and REG_AFC_BW. Double-sideband bandwidth =
/// FXOSC / (RxBwMant * 2^(RxBwExp+2)) with FXOSC = 32 MHz. Narrower rejects more out-of-band
/// noise (higher sensitivity); wider tolerates more LO frequency offset.
enum class SX1276RxBandwidth : uint8_t {
  BW_20_8_KHZ = 0x14,   ///< 20.8 kHz — narrowest; maximal noise rejection, least LO-offset tolerance.
  BW_41_7_KHZ = 0x13,   ///< 41.7 kHz — default (validated against real devices).
  BW_62_5_KHZ = 0x03,   ///< 62.5 kHz.
  BW_83_3_KHZ = 0x12,   ///< 83.3 kHz.
  BW_125_0_KHZ = 0x02,  ///< 125.0 kHz — widest selectable option.
};

/// @brief Valid LR1121 RX bandwidth options (register values).
///
/// Byte-for-byte identical to `SX1262RxBandwidth` — both chips use the same Semtech GFSK
/// bandwidth grid — and kept as a distinct enum only so each driver's options can diverge if a
/// future chip's table does. See `Sx1262AndLr1121BandwidthTablesAgree`, which pins the two tables
/// together; if they ever need to differ for a real chip difference, say why here.
enum class LR1121RxBandwidth : uint8_t {
  BW_39_0_KHZ = 0x1C,   ///< 39.0 kHz — narrowest; close to SX1276's validated 41.7 kHz default.
  BW_46_9_KHZ = 0x14,   ///< 46.9 kHz — narrow.
  BW_58_6_KHZ = 0x0C,   ///< 58.6 kHz.
  BW_78_2_KHZ = 0x1B,   ///< 78.2 kHz.
  BW_117_3_KHZ = 0x0B,  ///< 117.3 kHz — default.
  BW_156_2_KHZ = 0x1A,  ///< 156.2 kHz — wider tolerance for LO offset.
  BW_187_2_KHZ = 0x12,  ///< 187.2 kHz — widest selectable option.
};

/// @brief Discovery request command codes.
enum class DiscoveryCommand : uint8_t {
  DISCOVER = 0x28,      ///< Standard broadcast discovery request (to 0x00003B).
  DISCOVER_SPE = 0x2A,  ///< SPE roll-call request (self-authenticating; see CMD_DISCOVER_SPE_REQ).
                        ///< Deliberately **not** offered as a `pairing_discovery_commands` preset
                        ///< in tuning.py: only devices that already hold the system key answer it,
                        ///< so it can never help reach a device in learning mode. Retained here
                        ///< because the command byte is still needed to *send* a roll-call, and
                        ///< because discovery_command_from_string() stays permissive enough to
                        ///< parse it — do not remove either as dead code.
  DISCOVER_ALT = 0x2E,  ///< Alternate broadcast discovery (to 0x00003F), with optional payload byte.
};

// Chip-specific radio defaults live here beside the TuningConfig fields they initialize — the
// single source of truth for defaults. proto_timing.h holds only chip-neutral protocol values;
// the radio drivers consume these via apply_tuning / hop_dwell_ms.

/// SX1262-specific preamble for response/continuation frames within an exchange.
///
/// Applies to any tight-turnaround frame sent immediately after receiving from the device —
/// 0x3D challenge responses, 0x32 key transfers after receiving 0x3C, and any future protocol
/// frame in that position — giving the peer's receiver time to lock back on after the SX1262's
/// own TX→RX turnaround.
///
/// 8 bytes was validated on real hardware (Heltec V3.2 ↔ Device actuator) as the minimum that
/// gives reliable lock-on without perturbing exchange timing; it matches the protocol's own
/// nominal SHORT_PREAMBLE floor.
///
/// This constant is byte-denominated, like every other preamble value in this codebase
/// (LONG_PREAMBLE, SHORT_PREAMBLE) — `RadioSX1262::set_packet_params_()` is the one place that
/// converts to the chip's bit-denominated SetPacketParams field, right before the value leaves
/// for the wire.
static constexpr uint16_t SX1262_RESPONSE_PREAMBLE = 8;

/// SX1262-specific post-TX settling delay before re-entering RX.
///
/// The SX1262 GFSK demodulator needs time to stabilize after TX before it can
/// reliably receive the next frame. A 500 µs delay was validated as the minimum
/// that prevents challenge-byte corruption during pairing and tight-turnaround
/// authenticated exchanges.
static constexpr uint16_t SX1262_POST_TX_SETTLE_US = 500;

/// SX1276 preamble for response/continuation frames within an exchange.
///
/// The SX1276 originally reused the protocol's 8-byte SHORT_PREAMBLE for reply frames. A
/// slightly longer 12-byte preamble was found on real hardware to improve the peer device's
/// lock-on without measurably affecting exchange timing, so 12 is the default. Runtime-tunable
/// down to SHORT_PREAMBLE or up for a marginal-range install.
static constexpr uint16_t SX1276_RESPONSE_PREAMBLE = 12;

/// Per-channel dwell while SX1276 pairing discovery hops across channels.
///
/// The SX1276 supports FastHop (no standby transition), so a short slice keeps
/// discovery sweeping all three channels quickly.
static constexpr uint16_t SX1276_DISCOVERY_HOP_SLICE_MS = 5;

/// Per-channel dwell while SX1262 pairing discovery hops across channels.
///
/// SX1262 frequency changes require a standby→SetRfFrequency→RX cycle, unlike the SX1276's
/// FastHop, but that changes only how much a single retune costs, not how long the radio should
/// then sit still: a short dwell fits more retunes into the listen window than a long one, so the
/// receiver is more often already parked on the right channel by the time a reply starts. The
/// preamble/sync linger guard (`ListenSpec::linger_on_preamble`) is what makes a dwell this short
/// safe — it keeps a caught reply from being cut off mid-reception by extending the dwell instead
/// of hopping away. A dwell in the low single-digit milliseconds performs best; shorter than that,
/// coverage degrades gradually and only truly collapses at `0`, where `wait_for_packet(..., 0)`
/// returns before any guard can observe activity at all. `7` is the best-performing value found in
/// that range.
static constexpr uint16_t SX1262_DISCOVERY_HOP_SLICE_MS = 7;

/// LR1121-specific preamble for response/continuation frames within an exchange.
///
/// Seeded from the SX1262-validated default (design doc §3.2: "seed every timing/tuning
/// default from the validated SX1262 values ... they encode protocol-side realities more
/// than chip quirks"). Not yet independently validated on LR1121 hardware — see the
/// implementation plan's Step 7 (loopback tuning), which folds a measured value back here.
static constexpr uint16_t LR1121_RESPONSE_PREAMBLE = SX1262_RESPONSE_PREAMBLE;

/// LR1121-specific post-TX settling delay before re-entering RX.
///
/// Seeded from the SX1262-validated default; same rationale as @ref LR1121_RESPONSE_PREAMBLE.
static constexpr uint16_t LR1121_POST_TX_SETTLE_US = SX1262_POST_TX_SETTLE_US;

/// Per-channel dwell while LR1121 pairing discovery hops across channels.
///
/// Measured independently on LR1121 hardware rather than inherited from
/// @ref SX1262_DISCOVERY_HOP_SLICE_MS — that constant's short-dwell reasoning applies equally
/// here, but the two chips are validated separately and could in principle diverge, so this stays
/// its own literal rather than an alias.
static constexpr uint16_t LR1121_DISCOVERY_HOP_SLICE_MS = 7;

/// @brief All runtime tunable parameters for pairing and radio diagnostics.
///
/// Values reset to their defaults on every boot. Each field initializes from a
/// canonical constant — chip-neutral defaults live in `proto_timing.h`, chip-specific
/// defaults in this header — shared with the ESPHome YAML schema. The hub stores a single
/// `TuningConfig` instance and passes it to the radio driver and pairing flow. UI
/// callbacks update the hub instance directly; the snapshot helpers emit the current
/// values in YAML-compatible form so a working combination can be copied back into the
/// configuration file.
struct TuningConfig {
  // --- Radio / physical layer ---
  /// SX1262 RX bandwidth selector.
  ///
  /// 58.6 kHz: narrower rejects more noise, and reception improves as the filter narrows on this
  /// waveform. Matches the SX1276's long-validated 41.7 kHz default on the identical waveform. A
  /// wide default would exist only to tolerate local-oscillator offset across the TX->RX
  /// turnaround, but that turnaround is now a measured ~390 us plus a 500 us settle, well within
  /// what the narrow filter tolerates.
  SX1262RxBandwidth sx1262_rx_bandwidth{SX1262RxBandwidth::BW_58_6_KHZ};
  uint16_t sx1262_response_preamble{SX1262_RESPONSE_PREAMBLE};            ///< SX1262 response preamble in bytes.
  uint16_t sx1262_post_tx_settle_us{SX1262_POST_TX_SETTLE_US};            ///< Delay after SX1262 TX before RX (µs).
  SX1276RxBandwidth sx1276_rx_bandwidth{SX1276RxBandwidth::BW_41_7_KHZ};  ///< SX1276 RX bandwidth selector.
  uint16_t sx1276_response_preamble{SX1276_RESPONSE_PREAMBLE};            ///< SX1276 response preamble in bytes.
  uint16_t sx1276_discovery_hop_slice_ms{
      SX1276_DISCOVERY_HOP_SLICE_MS};  ///< Per-channel dwell while SX1276 discovery hops.
  uint16_t sx1262_discovery_hop_slice_ms{
      SX1262_DISCOVERY_HOP_SLICE_MS};  ///< Per-channel dwell while SX1262 discovery hops.
  LR1121RxBandwidth lr1121_rx_bandwidth{LR1121RxBandwidth::BW_117_3_KHZ};  ///< LR1121 RX bandwidth selector.
  uint16_t lr1121_response_preamble{LR1121_RESPONSE_PREAMBLE};             ///< LR1121 response preamble in bytes.
  uint16_t lr1121_post_tx_settle_us{LR1121_POST_TX_SETTLE_US};             ///< Delay after LR1121 TX before RX (µs).
  uint16_t lr1121_discovery_hop_slice_ms{
      LR1121_DISCOVERY_HOP_SLICE_MS};  ///< Per-channel dwell while LR1121 discovery hops.
  uint16_t cold_broadcast_reply_preamble{
      COLD_BROADCAST_REPLY_PREAMBLE};  ///< Preamble for a start-flagged key-extraction broadcast reply (0x29).
  uint16_t normal_start_preamble{
      NORMAL_START_PREAMBLE};                ///< Preamble for a directed start frame to a non-low-power target.
  uint8_t lbt_max_retries{LBT_MAX_RETRIES};  ///< LBT retries before forced TX.
  int16_t lbt_rssi_threshold_dbm{LBT_RSSI_THRESHOLD_DBM};  ///< LBT channel-free threshold (dBm).

  // --- Exchange response windows ---
  // How long the hub listens for a device's reply. Tunable because the right value is a property
  // of the *device*, not of the radio or the protocol: observed reply latencies on one network
  // span two orders of magnitude (a mains Oximo 40 at ~23 ms, a solar RS100 at 29-3052 ms). See
  // RESPONSE_START_WAIT_MS for the measurements behind the defaults.
  uint16_t exchange_start_response_wait_ms{
      RESPONSE_START_WAIT_MS};  ///< Reply window for a start frame (wakes a sleeping device).
  uint16_t exchange_response_wait_ms{
      RESPONSE_WAIT_MS};  ///< Reply window for continuation frames and the post-auth final response.
  uint16_t exchange_total_budget_ms{EXCHANGE_TOTAL_BUDGET_MS};  ///< Ceiling on one whole exchange, retries included.

  // --- Pairing protocol ---
  std::vector<DiscoveryCommand> pairing_discovery_commands{
      DiscoveryCommand::DISCOVER};                                       ///< Ordered discovery commands.
  std::vector<uint8_t> pairing_discovery_destination{0x00, 0x00, 0x00};  ///< Destination when auto=false.
  bool pairing_discovery_destination_auto{true};  ///< When true, map commands to conventional addresses.
  uint8_t pairing_discovery_payload{0};           ///< Optional payload byte (used for 0x2E).
  bool pairing_discovery_payload_enabled{false};  ///< Whether the optional payload is enabled.
  bool pairing_discovery_low_power{false};        ///< Set LOW_POWER flag in discovery frames.
  uint16_t pairing_discovery_wait_ms{
      PAIRING_DISCOVERY_WAIT_MS};  ///< Total wait window after sending discovery commands.
  uint16_t pairing_discovery_initial_dwell_ms{
      PAIRING_DISCOVERY_INITIAL_DWELL_MS};  ///< Initial dwell on CH2 before discovery hopping begins.
  uint8_t pairing_key_exchange_retries{
      PAIRING_KEY_EXCHANGE_RETRIES};  ///< Retries for the authenticated key exchange phase.

  // --- Internal state ---
  bool active{false};  ///< True when the YAML `tuning:` block is present.
};

/// @brief One selectable RX bandwidth: the chip's register byte and its nominal kHz value.
///
/// The per-chip bandwidth conversion functions below (SX1262/SX1276/LR1121) are all the same
/// logic over a different table of these; they are thin typed wrappers around the three generic
/// helpers that follow, which own the register-byte lookup, the "%.1f" formatting, and the
/// input normalization once instead of three times.
struct BandwidthOption {
  uint8_t reg;
  float khz;
};

/// @brief Look up the kHz value for a register byte in a bandwidth table.
/// @param table Bandwidth option table.
/// @param n Number of entries in @p table.
/// @param reg Register byte to look up.
/// @param fallback kHz value returned when @p reg is not in the table.
float bandwidth_to_khz(const BandwidthOption *table, size_t n, uint8_t reg, float fallback);

/// @brief Format a kHz value as its YAML/UI option string (bare number, one decimal, e.g. "117.3").
std::string bandwidth_to_string(float khz);

/// @brief Parse a YAML/UI bandwidth string against a table, returning the matching register byte.
///
/// Accepts surrounding whitespace and a trailing "kHz" suffix in any case, and accepts both the
/// one-decimal spelling ("39.0") and the integer-truncated spelling ("39") of every table entry.
/// @return The register byte, or std::nullopt when the string matches no entry.
std::optional<uint8_t> bandwidth_from_string(const BandwidthOption *table, size_t n, const std::string &value);

/// @brief A pointer/size view over one chip's RX-bandwidth option table.
struct BandwidthTableView {
  const BandwidthOption *data;
  size_t size;
};

/// @brief The SX1262 RX-bandwidth option table. Exposed so tests derive their legal-option
/// lists from the same source production uses rather than re-listing them by hand.
BandwidthTableView sx1262_bandwidth_table();
/// @brief The SX1276 RX-bandwidth option table (see sx1262_bandwidth_table()).
BandwidthTableView sx1276_bandwidth_table();
/// @brief The LR1121 RX-bandwidth option table (see sx1262_bandwidth_table()).
BandwidthTableView lr1121_bandwidth_table();

/// @brief Convert a bandwidth enum to the numeric kHz value used in YAML/logs.
/// @param bw SX1262 bandwidth enum.
/// @return Floating-point kHz value (39.0, 46.9, 58.6, 78.2, 117.3, 156.2, or 187.2).
float sx1262_bandwidth_to_khz(SX1262RxBandwidth bw);

/// @brief Format a bandwidth enum as its YAML/UI option string (bare kHz number, e.g. "117.3").
/// @param bw SX1262 bandwidth enum.
/// @return Bandwidth rendered with one decimal place and a "kHz" suffix.
std::string sx1262_bandwidth_to_string(SX1262RxBandwidth bw);

/// @brief Convert a YAML bandwidth string to the enum value.
/// @param value YAML string such as "117.3kHz" or "156.2kHz".
/// @return Bandwidth enum, or std::nullopt on invalid input.
std::optional<SX1262RxBandwidth> sx1262_bandwidth_from_string(const std::string &value);

/// @brief Convert an SX1276 bandwidth enum to the numeric kHz value used in YAML/logs.
/// @param bw SX1276 bandwidth enum.
/// @return Floating-point kHz value (20.8, 41.7, 62.5, 83.3, or 125.0).
float sx1276_bandwidth_to_khz(SX1276RxBandwidth bw);

/// @brief Format an SX1276 bandwidth enum as its YAML/UI option string (bare kHz number).
/// @param bw SX1276 bandwidth enum.
/// @return Bandwidth rendered with one decimal place (e.g. "41.7").
std::string sx1276_bandwidth_to_string(SX1276RxBandwidth bw);

/// @brief Convert a YAML SX1276 bandwidth string to the enum value.
/// @param value YAML string such as "41.7" or "83.3kHz".
/// @return Bandwidth enum, or std::nullopt on invalid input.
std::optional<SX1276RxBandwidth> sx1276_bandwidth_from_string(const std::string &value);

/// @brief Convert an LR1121 bandwidth enum to the numeric kHz value used in YAML/logs.
/// @param bw LR1121 bandwidth enum.
/// @return Floating-point kHz value (39.0, 46.9, 58.6, 78.2, 117.3, 156.2, or 187.2).
float lr1121_bandwidth_to_khz(LR1121RxBandwidth bw);

/// @brief Format an LR1121 bandwidth enum as its YAML/UI option string (bare kHz number).
/// @param bw LR1121 bandwidth enum.
/// @return Bandwidth rendered with one decimal place (e.g. "117.3").
std::string lr1121_bandwidth_to_string(LR1121RxBandwidth bw);

/// @brief Convert a YAML LR1121 bandwidth string to the enum value.
/// @param value YAML string such as "117.3" or "39.0kHz".
/// @return Bandwidth enum, or std::nullopt on invalid input.
std::optional<LR1121RxBandwidth> lr1121_bandwidth_from_string(const std::string &value);

/// @brief Format the current tuning configuration as a one-line YAML-compatible snapshot.
///
/// Only values that differ from the default are emitted, so the line can be copied
/// back into YAML with minimal editing. When all values are default, an empty string
/// is returned.
///
/// @param cfg Current tuning configuration.
/// @return One-line snapshot string, or empty when no overrides are active.
std::string tuning_config_snapshot(const TuningConfig &cfg);

/// @brief Format the current tuning configuration as a full one-line snapshot.
///
/// Emits every non-default value, regardless of whether it was changed from YAML or
/// from the HA UI. This is logged at the start of every pairing attempt.
///
/// @param cfg Current tuning configuration.
/// @return One-line snapshot string, or a "defaults" marker when nothing is overridden.
std::string tuning_config_full_snapshot(const TuningConfig &cfg);

/// @brief Format a single tuning update for the log.
///
/// @param name YAML key name of the updated parameter.
/// @param value YAML-compatible string representation of the new value.
/// @return One-line log string suitable for ESP_LOGI.
std::string tuning_update_log_line(const std::string &name, const std::string &value);

/// @brief Resolve the destination address for a discovery command.
///
/// When `destination_auto` is true, returns the conventional address for the
/// given command code. Otherwise returns the configured `destination` value.
///
/// @param command Discovery command code.
/// @param destination_auto Whether to use the automatic mapping.
/// @param destination Explicit 3-byte destination when auto is false.
/// @return Pointer to the resolved 3-byte node ID.
const uint8_t *resolve_discovery_destination(uint8_t command, bool destination_auto,
                                             const uint8_t destination[NODE_ID_SIZE]);

/// @brief Parse a discovery command string (e.g., "0x28") into the enum.
/// @param value String to parse.
/// @return Discovery command enum, or std::nullopt on invalid input.
std::optional<DiscoveryCommand> discovery_command_from_string(const std::string &value);

/// @brief Format a discovery command enum for YAML/logs.
/// @param cmd Discovery command enum.
/// @return Lowercase hex string such as "0x28".
std::string discovery_command_to_string(DiscoveryCommand cmd);

/// @brief Format a destination option for YAML/logs.
///
/// @param destination_auto Whether automatic destination mapping is used.
/// @param destination Explicit 3-byte destination when auto is false.
/// @return "auto" or a hex address such as "0x00003B".
std::string discovery_destination_to_string(bool destination_auto, const uint8_t destination[NODE_ID_SIZE]);

/// @brief Format a payload option for YAML/logs.
///
/// @param payload_enabled Whether the optional payload is enabled.
/// @param payload The payload byte when enabled.
/// @return "none" or a hex byte such as "0x00".
std::string discovery_payload_to_string(bool payload_enabled, uint8_t payload);

/// @brief Format the ordered discovery command list for logs.
///
/// @param commands Ordered list of discovery commands.
/// @return Bracketed comma-separated list such as "[0x28,0x2E]".
std::string discovery_commands_to_string(const std::vector<DiscoveryCommand> &commands);

/// @brief Format the ordered discovery command list as a UI/preset option string.
///
/// Matches the comma-separated `select` option labels (no brackets) so a boot-time
/// snapshot round-trips to a selectable dropdown value.
///
/// @param commands Ordered list of discovery commands.
/// @return Comma-separated list such as "0x28,0x2E" (empty string when the list is empty).
std::string discovery_commands_to_csv(const std::vector<DiscoveryCommand> &commands);

}  // namespace home_io_control
}  // namespace esphome

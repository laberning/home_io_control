/// @file tuning_config.cpp
/// @brief Runtime tuning configuration helpers and snapshot formatting.
/// @ingroup hioc_tuning

#include "tuning_config.h"

#include "proto_constants.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace esphome {
namespace home_io_control {

namespace {

/// Default tuning configuration used for diffing and reset behavior.
const TuningConfig DEFAULTS{};

/// Buffer size for a formatted single hex byte such as "0x2E" (4 chars + slack).
constexpr size_t HEX_BYTE_STR_SIZE = 8;
/// Buffer size for a formatted 3-byte hex address such as "0x00003B".
constexpr size_t HEX_ADDR_STR_SIZE = 16;
/// Buffer size for a formatted bandwidth string such as "117.3kHz".
constexpr size_t BANDWIDTH_STR_SIZE = 16;

/// Numeric kHz value for each SX1262 RX bandwidth option.
constexpr float BW_KHZ_58_6 = 58.6F;
constexpr float BW_KHZ_78_2 = 78.2F;
constexpr float BW_KHZ_117_3 = 117.3F;
constexpr float BW_KHZ_156_2 = 156.2F;
constexpr float BW_KHZ_187_2 = 187.2F;

/// Numeric kHz value for each SX1276 RX bandwidth option.
constexpr float BW_KHZ_20_8 = 20.8F;
constexpr float BW_KHZ_41_7 = 41.7F;
constexpr float BW_KHZ_62_5 = 62.5F;
constexpr float BW_KHZ_83_3 = 83.3F;
constexpr float BW_KHZ_125_0 = 125.0F;

/// Numeric kHz value for each LR1121 RX bandwidth option.
constexpr float BW_KHZ_39_0 = 39.0F;
constexpr float BW_KHZ_46_9 = 46.9F;

}  // namespace

float sx1262_bandwidth_to_khz(SX1262RxBandwidth bw) {
  switch (bw) {
    case SX1262RxBandwidth::BW_39_0_KHZ:
      return BW_KHZ_39_0;
    case SX1262RxBandwidth::BW_46_9_KHZ:
      return BW_KHZ_46_9;
    case SX1262RxBandwidth::BW_58_6_KHZ:
      return BW_KHZ_58_6;
    case SX1262RxBandwidth::BW_78_2_KHZ:
      return BW_KHZ_78_2;
    case SX1262RxBandwidth::BW_117_3_KHZ:
      return BW_KHZ_117_3;
    case SX1262RxBandwidth::BW_156_2_KHZ:
      return BW_KHZ_156_2;
    case SX1262RxBandwidth::BW_187_2_KHZ:
      return BW_KHZ_187_2;
  }
  return BW_KHZ_117_3;
}

std::string sx1262_bandwidth_to_string(SX1262RxBandwidth bw) {
  char buf[BANDWIDTH_STR_SIZE];
  snprintf(buf, sizeof(buf), "%.1f", sx1262_bandwidth_to_khz(bw));
  return std::string(buf);
}

std::optional<SX1262RxBandwidth> sx1262_bandwidth_from_string(const std::string &value) {
  std::string normalized = value;
  // Normalize: strip whitespace and a trailing "kHz"/"khz" suffix.
  normalized.erase(std::remove_if(normalized.begin(), normalized.end(), ::isspace), normalized.end());
  for (char &c : normalized)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (normalized.size() > 3 && normalized.ends_with("khz"))
    normalized.resize(normalized.size() - 3);

  if (normalized == "39.0" || normalized == "39")
    return SX1262RxBandwidth::BW_39_0_KHZ;
  if (normalized == "46.9" || normalized == "46")
    return SX1262RxBandwidth::BW_46_9_KHZ;
  if (normalized == "58.6" || normalized == "58")
    return SX1262RxBandwidth::BW_58_6_KHZ;
  if (normalized == "78.2" || normalized == "78")
    return SX1262RxBandwidth::BW_78_2_KHZ;
  if (normalized == "117.3" || normalized == "117")
    return SX1262RxBandwidth::BW_117_3_KHZ;
  if (normalized == "156.2" || normalized == "156")
    return SX1262RxBandwidth::BW_156_2_KHZ;
  if (normalized == "187.2" || normalized == "187")
    return SX1262RxBandwidth::BW_187_2_KHZ;
  return std::nullopt;
}

float sx1276_bandwidth_to_khz(SX1276RxBandwidth bw) {
  switch (bw) {
    case SX1276RxBandwidth::BW_20_8_KHZ:
      return BW_KHZ_20_8;
    case SX1276RxBandwidth::BW_41_7_KHZ:
      return BW_KHZ_41_7;
    case SX1276RxBandwidth::BW_62_5_KHZ:
      return BW_KHZ_62_5;
    case SX1276RxBandwidth::BW_83_3_KHZ:
      return BW_KHZ_83_3;
    case SX1276RxBandwidth::BW_125_0_KHZ:
      return BW_KHZ_125_0;
  }
  return BW_KHZ_41_7;
}

std::string sx1276_bandwidth_to_string(SX1276RxBandwidth bw) {
  char buf[BANDWIDTH_STR_SIZE];
  snprintf(buf, sizeof(buf), "%.1f", sx1276_bandwidth_to_khz(bw));
  return std::string(buf);
}

std::optional<SX1276RxBandwidth> sx1276_bandwidth_from_string(const std::string &value) {
  std::string normalized = value;
  // Normalize: strip whitespace and a trailing "kHz"/"khz" suffix.
  normalized.erase(std::remove_if(normalized.begin(), normalized.end(), ::isspace), normalized.end());
  for (char &c : normalized)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (normalized.size() > 3 && normalized.ends_with("khz"))
    normalized.resize(normalized.size() - 3);

  if (normalized == "20.8" || normalized == "20")
    return SX1276RxBandwidth::BW_20_8_KHZ;
  if (normalized == "41.7" || normalized == "41")
    return SX1276RxBandwidth::BW_41_7_KHZ;
  if (normalized == "62.5" || normalized == "62")
    return SX1276RxBandwidth::BW_62_5_KHZ;
  if (normalized == "83.3" || normalized == "83")
    return SX1276RxBandwidth::BW_83_3_KHZ;
  if (normalized == "125.0" || normalized == "125")
    return SX1276RxBandwidth::BW_125_0_KHZ;
  return std::nullopt;
}

float lr1121_bandwidth_to_khz(LR1121RxBandwidth bw) {
  switch (bw) {
    case LR1121RxBandwidth::BW_39_0_KHZ:
      return BW_KHZ_39_0;
    case LR1121RxBandwidth::BW_46_9_KHZ:
      return BW_KHZ_46_9;
    case LR1121RxBandwidth::BW_58_6_KHZ:
      return BW_KHZ_58_6;
    case LR1121RxBandwidth::BW_78_2_KHZ:
      return BW_KHZ_78_2;
    case LR1121RxBandwidth::BW_117_3_KHZ:
      return BW_KHZ_117_3;
    case LR1121RxBandwidth::BW_156_2_KHZ:
      return BW_KHZ_156_2;
    case LR1121RxBandwidth::BW_187_2_KHZ:
      return BW_KHZ_187_2;
  }
  return BW_KHZ_117_3;
}

std::string lr1121_bandwidth_to_string(LR1121RxBandwidth bw) {
  char buf[BANDWIDTH_STR_SIZE];
  snprintf(buf, sizeof(buf), "%.1f", lr1121_bandwidth_to_khz(bw));
  return std::string(buf);
}

std::optional<LR1121RxBandwidth> lr1121_bandwidth_from_string(const std::string &value) {
  std::string normalized = value;
  // Normalize: strip whitespace and a trailing "kHz"/"khz" suffix.
  normalized.erase(std::remove_if(normalized.begin(), normalized.end(), ::isspace), normalized.end());
  for (char &c : normalized)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (normalized.size() > 3 && normalized.ends_with("khz"))
    normalized.resize(normalized.size() - 3);

  if (normalized == "39.0" || normalized == "39")
    return LR1121RxBandwidth::BW_39_0_KHZ;
  if (normalized == "46.9" || normalized == "46")
    return LR1121RxBandwidth::BW_46_9_KHZ;
  if (normalized == "58.6" || normalized == "58")
    return LR1121RxBandwidth::BW_58_6_KHZ;
  if (normalized == "78.2" || normalized == "78")
    return LR1121RxBandwidth::BW_78_2_KHZ;
  if (normalized == "117.3" || normalized == "117")
    return LR1121RxBandwidth::BW_117_3_KHZ;
  if (normalized == "156.2" || normalized == "156")
    return LR1121RxBandwidth::BW_156_2_KHZ;
  if (normalized == "187.2" || normalized == "187")
    return LR1121RxBandwidth::BW_187_2_KHZ;
  return std::nullopt;
}

std::optional<DiscoveryCommand> discovery_command_from_string(const std::string &value) {
  std::string normalized = value;
  for (char &c : normalized)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (normalized == "0x28" || normalized == "discover")
    return DiscoveryCommand::DISCOVER;
  if (normalized == "0x2a" || normalized == "discover_spe")
    return DiscoveryCommand::DISCOVER_SPE;
  if (normalized == "0x2e" || normalized == "discover_alt")
    return DiscoveryCommand::DISCOVER_ALT;
  return std::nullopt;
}

std::string discovery_command_to_string(DiscoveryCommand cmd) {
  char buf[HEX_BYTE_STR_SIZE];
  snprintf(buf, sizeof(buf), "0x%02X", static_cast<uint8_t>(cmd));
  return std::string(buf);
}

std::string discovery_commands_to_csv(const std::vector<DiscoveryCommand> &commands) {
  std::string result;
  for (size_t i = 0; i < commands.size(); ++i) {
    if (i > 0)
      result += ',';
    result += discovery_command_to_string(commands[i]);
  }
  return result;
}

std::string discovery_commands_to_string(const std::vector<DiscoveryCommand> &commands) {
  return "[" + discovery_commands_to_csv(commands) + "]";
}

const uint8_t *resolve_discovery_destination(uint8_t command, bool destination_auto,
                                             const uint8_t destination[NODE_ID_SIZE]) {
  if (!destination_auto)
    return destination;
  // Conventional destinations from the protocol / reference captures: standard and SPE
  // discovery broadcast to 0x00003B; alternate discovery (0x2E) broadcasts to 0x00003F.
  switch (command) {
    case CMD_DISCOVER_REQ:
    case CMD_DISCOVER_SPE_REQ:
      return BROADCAST_DISCOVER;
    case CMD_DISCOVER_ALT_REQ:
      return BROADCAST_DISCOVER_ALT;
    default:
      return BROADCAST_DISCOVER;
  }
}

std::string discovery_destination_to_string(bool destination_auto, const uint8_t destination[NODE_ID_SIZE]) {
  if (destination_auto)
    return "auto";
  char buf[HEX_ADDR_STR_SIZE];
  snprintf(buf, sizeof(buf), "0x%02X%02X%02X", destination[0], destination[1], destination[2]);
  return std::string(buf);
}

std::string discovery_payload_to_string(bool payload_enabled, uint8_t payload) {
  if (!payload_enabled)
    return "none";
  char buf[HEX_BYTE_STR_SIZE];
  snprintf(buf, sizeof(buf), "0x%02X", payload);
  return std::string(buf);
}

std::string tuning_update_log_line(const std::string &name, const std::string &value) {
  return "Tuning updated via HA: " + name + "=" + value;
}

std::string tuning_config_snapshot(const TuningConfig &cfg) {
  std::string result;

  if (cfg.sx1262_rx_bandwidth != DEFAULTS.sx1262_rx_bandwidth)
    result += " sx1262_rx_bandwidth=" + sx1262_bandwidth_to_string(cfg.sx1262_rx_bandwidth);
  if (cfg.sx1262_response_preamble != DEFAULTS.sx1262_response_preamble)
    result += " sx1262_response_preamble=" + std::to_string(cfg.sx1262_response_preamble);
  if (cfg.sx1262_post_tx_settle_us != DEFAULTS.sx1262_post_tx_settle_us)
    result += " sx1262_post_tx_settle_us=" + std::to_string(cfg.sx1262_post_tx_settle_us);
  if (cfg.sx1276_rx_bandwidth != DEFAULTS.sx1276_rx_bandwidth)
    result += " sx1276_rx_bandwidth=" + sx1276_bandwidth_to_string(cfg.sx1276_rx_bandwidth);
  if (cfg.sx1276_response_preamble != DEFAULTS.sx1276_response_preamble)
    result += " sx1276_response_preamble=" + std::to_string(cfg.sx1276_response_preamble);
  if (cfg.sx1276_discovery_hop_slice_ms != DEFAULTS.sx1276_discovery_hop_slice_ms)
    result += " sx1276_discovery_hop_slice_ms=" + std::to_string(cfg.sx1276_discovery_hop_slice_ms);
  if (cfg.sx1262_discovery_hop_slice_ms != DEFAULTS.sx1262_discovery_hop_slice_ms)
    result += " sx1262_discovery_hop_slice_ms=" + std::to_string(cfg.sx1262_discovery_hop_slice_ms);
  if (cfg.lr1121_rx_bandwidth != DEFAULTS.lr1121_rx_bandwidth)
    result += " lr1121_rx_bandwidth=" + lr1121_bandwidth_to_string(cfg.lr1121_rx_bandwidth);
  if (cfg.lr1121_response_preamble != DEFAULTS.lr1121_response_preamble)
    result += " lr1121_response_preamble=" + std::to_string(cfg.lr1121_response_preamble);
  if (cfg.lr1121_post_tx_settle_us != DEFAULTS.lr1121_post_tx_settle_us)
    result += " lr1121_post_tx_settle_us=" + std::to_string(cfg.lr1121_post_tx_settle_us);
  if (cfg.lr1121_discovery_hop_slice_ms != DEFAULTS.lr1121_discovery_hop_slice_ms)
    result += " lr1121_discovery_hop_slice_ms=" + std::to_string(cfg.lr1121_discovery_hop_slice_ms);
  if (cfg.lbt_max_retries != DEFAULTS.lbt_max_retries)
    result += " lbt_max_retries=" + std::to_string(cfg.lbt_max_retries);
  if (cfg.lbt_rssi_threshold_dbm != DEFAULTS.lbt_rssi_threshold_dbm)
    result += " lbt_rssi_threshold_dbm=" + std::to_string(cfg.lbt_rssi_threshold_dbm);

  if (cfg.pairing_discovery_commands != DEFAULTS.pairing_discovery_commands)
    result += " pairing_discovery_commands=" + discovery_commands_to_string(cfg.pairing_discovery_commands);
  // The default is auto; an explicit destination is the only non-default state worth emitting
  // (the destination bytes are unused while auto is set).
  if (!cfg.pairing_discovery_destination_auto) {
    result +=
        " pairing_discovery_destination=" + discovery_destination_to_string(cfg.pairing_discovery_destination_auto,
                                                                            cfg.pairing_discovery_destination.data());
  }
  if (cfg.pairing_discovery_payload_enabled != DEFAULTS.pairing_discovery_payload_enabled ||
      cfg.pairing_discovery_payload != DEFAULTS.pairing_discovery_payload) {
    result += " pairing_discovery_payload=" +
              discovery_payload_to_string(cfg.pairing_discovery_payload_enabled, cfg.pairing_discovery_payload);
  }
  if (cfg.pairing_discovery_low_power != DEFAULTS.pairing_discovery_low_power)
    result += " pairing_discovery_low_power=" + std::string(cfg.pairing_discovery_low_power ? "true" : "false");
  if (cfg.pairing_discovery_wait_ms != DEFAULTS.pairing_discovery_wait_ms)
    result += " pairing_discovery_wait_ms=" + std::to_string(cfg.pairing_discovery_wait_ms);
  if (cfg.pairing_discovery_initial_dwell_ms != DEFAULTS.pairing_discovery_initial_dwell_ms)
    result += " pairing_discovery_initial_dwell_ms=" + std::to_string(cfg.pairing_discovery_initial_dwell_ms);
  if (cfg.pairing_key_exchange_retries != DEFAULTS.pairing_key_exchange_retries)
    result += " pairing_key_exchange_retries=" + std::to_string(cfg.pairing_key_exchange_retries);

  return result;
}

std::string tuning_config_full_snapshot(const TuningConfig &cfg) {
  const std::string result = tuning_config_snapshot(cfg);
  if (result.empty())
    return "Tuning: defaults active";
  return "Tuning overrides active:" + result;
}

}  // namespace home_io_control
}  // namespace esphome

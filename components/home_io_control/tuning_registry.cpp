/// @file tuning_registry.cpp
/// @brief Parameter tables and lookup helpers for the tuning registry.
/// @ingroup hioc_tuning
///
/// The getter/setter lambdas below carry all per-parameter parsing (bandwidth enum
/// decode, the comma-separated command list with whitespace trimming, the
/// destination/payload/low-power mapping). Keeping them here makes the hub's tuning
/// dispatch methods pure table lookups.

#include "tuning_registry.h"

#include "proto_codecs.h"  // ADDRESS_SUFFIX_DISCOVERY / ADDRESS_SUFFIX_BROADCAST

#include <cstddef>
#include <iterator>

namespace esphome {
namespace home_io_control {

// === Numeric parameters (17) ===================================================
// Each row narrows the incoming float to the field's storage type, matching the
// original static_cast in update_tuning_number().
static constexpr TuningNumberParam NUMBER_PARAMS[] = {
    {"sx1262_response_preamble", [](const TuningConfig &t) { return static_cast<float>(t.sx1262_response_preamble); },
     [](TuningConfig &t, float v) { t.sx1262_response_preamble = static_cast<uint16_t>(v); }, true},
    {"sx1262_post_tx_settle_us", [](const TuningConfig &t) { return static_cast<float>(t.sx1262_post_tx_settle_us); },
     [](TuningConfig &t, float v) { t.sx1262_post_tx_settle_us = static_cast<uint16_t>(v); }, true},
    {"sx1276_response_preamble", [](const TuningConfig &t) { return static_cast<float>(t.sx1276_response_preamble); },
     [](TuningConfig &t, float v) { t.sx1276_response_preamble = static_cast<uint16_t>(v); }, true},
    {"sx1276_discovery_hop_slice_ms",
     [](const TuningConfig &t) { return static_cast<float>(t.sx1276_discovery_hop_slice_ms); },
     [](TuningConfig &t, float v) { t.sx1276_discovery_hop_slice_ms = static_cast<uint16_t>(v); }, false},
    {"sx1262_discovery_hop_slice_ms",
     [](const TuningConfig &t) { return static_cast<float>(t.sx1262_discovery_hop_slice_ms); },
     [](TuningConfig &t, float v) { t.sx1262_discovery_hop_slice_ms = static_cast<uint16_t>(v); }, false},
    {"lr1121_response_preamble", [](const TuningConfig &t) { return static_cast<float>(t.lr1121_response_preamble); },
     [](TuningConfig &t, float v) { t.lr1121_response_preamble = static_cast<uint16_t>(v); }, true},
    {"lr1121_post_tx_settle_us", [](const TuningConfig &t) { return static_cast<float>(t.lr1121_post_tx_settle_us); },
     [](TuningConfig &t, float v) { t.lr1121_post_tx_settle_us = static_cast<uint16_t>(v); }, true},
    {"lr1121_discovery_hop_slice_ms",
     [](const TuningConfig &t) { return static_cast<float>(t.lr1121_discovery_hop_slice_ms); },
     [](TuningConfig &t, float v) { t.lr1121_discovery_hop_slice_ms = static_cast<uint16_t>(v); }, false},
    // false: unlike sx1262/sx1276/lr1121_response_preamble, this field is not chip-specific and
    // is never cached into a radio driver via apply_tuning() — key_extraction_responder.cpp reads it
    // straight out of TuningConfig at TX time, the same way the *_discovery_hop_slice_ms fields
    // above are read live rather than applied.
    {"cold_broadcast_reply_preamble",
     [](const TuningConfig &t) { return static_cast<float>(t.cold_broadcast_reply_preamble); },
     [](TuningConfig &t, float v) { t.cold_broadcast_reply_preamble = static_cast<uint16_t>(v); }, false},
    // false: not chip-specific and never cached into a radio driver via apply_tuning() — the
    // exchange engine reads it straight out of TuningConfig at TX time, the same as
    // cold_broadcast_reply_preamble above.
    {"normal_start_preamble", [](const TuningConfig &t) { return static_cast<float>(t.normal_start_preamble); },
     [](TuningConfig &t, float v) { t.normal_start_preamble = static_cast<uint16_t>(v); }, false},
    {"lbt_max_retries", [](const TuningConfig &t) { return static_cast<float>(t.lbt_max_retries); },
     [](TuningConfig &t, float v) { t.lbt_max_retries = static_cast<uint8_t>(v); }, false},
    {"lbt_rssi_threshold_dbm", [](const TuningConfig &t) { return static_cast<float>(t.lbt_rssi_threshold_dbm); },
     [](TuningConfig &t, float v) { t.lbt_rssi_threshold_dbm = static_cast<int16_t>(v); }, false},
    {"exchange_start_response_wait_ms",
     [](const TuningConfig &t) { return static_cast<float>(t.exchange_start_response_wait_ms); },
     [](TuningConfig &t, float v) { t.exchange_start_response_wait_ms = static_cast<uint16_t>(v); }, false},
    {"exchange_response_wait_ms", [](const TuningConfig &t) { return static_cast<float>(t.exchange_response_wait_ms); },
     [](TuningConfig &t, float v) { t.exchange_response_wait_ms = static_cast<uint16_t>(v); }, false},
    {"exchange_total_budget_ms", [](const TuningConfig &t) { return static_cast<float>(t.exchange_total_budget_ms); },
     [](TuningConfig &t, float v) { t.exchange_total_budget_ms = static_cast<uint16_t>(v); }, false},
    {"pairing_discovery_wait_ms", [](const TuningConfig &t) { return static_cast<float>(t.pairing_discovery_wait_ms); },
     [](TuningConfig &t, float v) { t.pairing_discovery_wait_ms = static_cast<uint16_t>(v); }, false},
    {"pairing_discovery_initial_dwell_ms",
     [](const TuningConfig &t) { return static_cast<float>(t.pairing_discovery_initial_dwell_ms); },
     [](TuningConfig &t, float v) { t.pairing_discovery_initial_dwell_ms = static_cast<uint16_t>(v); }, false},
    {"pairing_key_exchange_retries",
     [](const TuningConfig &t) { return static_cast<float>(t.pairing_key_exchange_retries); },
     [](TuningConfig &t, float v) { t.pairing_key_exchange_retries = static_cast<uint8_t>(v); }, false},
};

// === Select parameters (7) =====================================================
// The setters return false only when the option string is unparseable AND leaving the
// value untouched is the intended behavior (currently just the bandwidth enum). The
// destination/payload setters return false for unrecognized values so no radio re-apply
// is triggered; that has no observable effect since neither applies to the radio.
static constexpr TuningSelectParam SELECT_PARAMS[] = {
    {"sx1262_rx_bandwidth", [](const TuningConfig &t) { return sx1262_bandwidth_to_string(t.sx1262_rx_bandwidth); },
     [](TuningConfig &t, const std::string &v) -> bool {
       auto bw = sx1262_bandwidth_from_string(v);
       if (!bw.has_value())
         return false;
       t.sx1262_rx_bandwidth = bw.value();
       return true;
     },
     true},
    {"sx1276_rx_bandwidth", [](const TuningConfig &t) { return sx1276_bandwidth_to_string(t.sx1276_rx_bandwidth); },
     [](TuningConfig &t, const std::string &v) -> bool {
       auto bw = sx1276_bandwidth_from_string(v);
       if (!bw.has_value())
         return false;
       t.sx1276_rx_bandwidth = bw.value();
       return true;
     },
     true},
    {"lr1121_rx_bandwidth", [](const TuningConfig &t) { return lr1121_bandwidth_to_string(t.lr1121_rx_bandwidth); },
     [](TuningConfig &t, const std::string &v) -> bool {
       auto bw = lr1121_bandwidth_from_string(v);
       if (!bw.has_value())
         return false;
       t.lr1121_rx_bandwidth = bw.value();
       return true;
     },
     true},
    {"pairing_discovery_commands",
     [](const TuningConfig &t) { return discovery_commands_to_csv(t.pairing_discovery_commands); },
     [](TuningConfig &t, const std::string &v) -> bool {
       t.pairing_discovery_commands.clear();
       // Support both individual commands and comma-separated preset strings.
       std::size_t start = 0;
       while (start < v.size()) {
         const std::size_t comma = v.find(',', start);
         std::string token = v.substr(start, comma - start);
         // Trim whitespace.
         token.erase(0, token.find_first_not_of(" \t"));
         token.erase(token.find_last_not_of(" \t") + 1);
         auto cmd = discovery_command_from_string(token);
         if (cmd.has_value())
           t.pairing_discovery_commands.push_back(cmd.value());
         if (comma == std::string::npos)
           break;
         start = comma + 1;
       }
       return true;
     },
     false},
    {"pairing_discovery_destination",
     [](const TuningConfig &t) {
       return discovery_destination_to_string(t.pairing_discovery_destination_auto,
                                              t.pairing_discovery_destination.data());
     },
     [](TuningConfig &t, const std::string &v) -> bool {
       if (v == "auto") {
         t.pairing_discovery_destination_auto = true;
         return true;
       }
       if (v == "0x00003B") {
         t.pairing_discovery_destination_auto = false;
         t.pairing_discovery_destination = {0x00, 0x00, ADDRESS_SUFFIX_DISCOVERY};
         return true;
       }
       if (v == "0x00003F") {
         t.pairing_discovery_destination_auto = false;
         t.pairing_discovery_destination = {0x00, 0x00, ADDRESS_SUFFIX_BROADCAST};
         return true;
       }
       return false;
     },
     false},
    {"pairing_discovery_payload",
     [](const TuningConfig &t) {
       return discovery_payload_to_string(t.pairing_discovery_payload_enabled, t.pairing_discovery_payload);
     },
     [](TuningConfig &t, const std::string &v) -> bool {
       if (v == "none") {
         t.pairing_discovery_payload_enabled = false;
         return true;
       }
       if (v == "0x00") {
         t.pairing_discovery_payload_enabled = true;
         t.pairing_discovery_payload = 0x00;
         return true;
       }
       return false;
     },
     false},
    {"pairing_discovery_low_power",
     [](const TuningConfig &t) -> std::string { return t.pairing_discovery_low_power ? "On" : "Off"; },
     [](TuningConfig &t, const std::string &v) -> bool {
       t.pairing_discovery_low_power = (v == "On");
       return true;
     },
     false},
};

const TuningNumberParam *find_tuning_number(const std::string &name) {
  for (const auto &param : NUMBER_PARAMS) {
    if (name == param.name)
      return &param;
  }
  return nullptr;
}

const TuningSelectParam *find_tuning_select(const std::string &name) {
  for (const auto &param : SELECT_PARAMS) {
    if (name == param.name)
      return &param;
  }
  return nullptr;
}

const TuningNumberParam *tuning_number_params_begin() { return std::begin(NUMBER_PARAMS); }
const TuningNumberParam *tuning_number_params_end() { return std::end(NUMBER_PARAMS); }
const TuningSelectParam *tuning_select_params_begin() { return std::begin(SELECT_PARAMS); }
const TuningSelectParam *tuning_select_params_end() { return std::end(SELECT_PARAMS); }

}  // namespace home_io_control
}  // namespace esphome

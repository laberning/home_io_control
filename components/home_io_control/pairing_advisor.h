#pragma once

/// @file pairing_advisor.h
/// @brief Read-only advisor that turns PairingTelemetry into actionable diagnostics.
/// @ingroup hioc_hub
///
/// PairingAdvisor is a pure, stateless consumer of a completed PairingTelemetry attempt: it
/// never touches the radio or the pairing state machine, only inspects the events already
/// recorded. It exists to convert the most common field failures (issue #27: a 1W remote
/// mistaken for 2W learning mode; a busy channel; a foreign controller mid-pairing; dead RF)
/// from a hex-dump exercise into a self-explaining WARN line.

#include "pairing_telemetry.h"
#include "proto_frame.h"

#include <cstdint>
#include <string>

namespace esphome {
namespace home_io_control {
namespace advisor {

/// @brief Actionable diagnosis derived from a completed pairing attempt's telemetry.
enum class PairingAdviceCode : uint8_t {
  NONE,                        ///< Nothing actionable to report.
  ONE_WAY_PAIRING_TRAFFIC,     ///< A 1W remote is pairing to the target — not 2W learning mode.
  CHANNEL_BUSY_LBT_DELAYED,    ///< LBT retries were consumed while a source kept repeating; the
                               ///< reported subject_node is a best-effort correlation (LBT's
                               ///< carrier-sense doesn't parse frames, so the actual jammer may
                               ///< not be among the parsed events attributed here).
  FOREIGN_CONTROLLER_PAIRING,  ///< A discovery response was seen addressed to another controller.
  RF_SILENT,                   ///< Nothing at all was heard during the whole window.
};

/// @brief One piece of advice, with the node/RSSI it pertains to (if any).
struct PairingAdvice {
  PairingAdviceCode code{PairingAdviceCode::NONE};
  uint8_t subject_node[NODE_ID_SIZE]{};  ///< Node the advice is about; zero if not applicable.
  int16_t rssi{0};                       ///< RSSI associated with the observation; zero if not applicable.
};

/// @brief Maximum number of advice entries a single attempt can produce (one slot per non-NONE code).
static constexpr uint8_t PAIRING_ADVICE_MAX = 4;

/// @brief Inspect a completed pairing attempt's telemetry and produce actionable advice.
///
/// Read-only: never mutates telemetry, never touches the radio or pairing state machine.
/// @param telemetry    Completed (or in-progress) telemetry for one pairing attempt.
/// @param own_node_id  This controller's node ID, used to detect discovery responses addressed
///                     to a different controller.
/// @param out          Output buffer, must hold at least PAIRING_ADVICE_MAX entries.
/// @return Number of advice entries written to `out` (0 if nothing actionable was found).
uint8_t analyze_pairing_telemetry(const PairingTelemetry &telemetry, const uint8_t own_node_id[NODE_ID_SIZE],
                                  PairingAdvice out[PAIRING_ADVICE_MAX]);

/// @return Short, stable code string for the `;advice=` result-sensor field (e.g. "1w_traffic").
const char *pairing_advice_code_name(PairingAdviceCode code);

/// @return The full actionable, human-readable WARN message for one piece of advice.
std::string pairing_advice_message(const PairingAdvice &advice);

}  // namespace advisor
}  // namespace home_io_control
}  // namespace esphome

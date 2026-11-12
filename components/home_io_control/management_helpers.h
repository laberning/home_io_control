#pragma once

/// @file management_helpers.h
/// @brief Result type and argument/result helpers shared by ManagementActions and DiagnosticProbes.
/// @ingroup hioc_hub
///
/// Both classes take native-API string arguments, resolve a device from them and report through
/// ManagementActionResult. The pieces they have in common live here, once: the result record,
/// device-ID normalisation, the byte-hex formatter, the CMD_ERROR_RESP decoder and the device
/// resolution preamble. Nothing here reaches the hub.

#include "device_registry.h"
#include "proto_codecs.h"
#include "proto_constants.h"
#include "proto_frame.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>

namespace esphome {
namespace home_io_control {

/// Result of a hub-level management action such as rename.
///
/// Returned by ManagementActions::rename_device() and DiagnosticProbes::probe_device(), and re-exported from the hub
/// as IOHomeControlComponent::ManagementActionResult via a public type alias.
struct ManagementActionResult {
  bool success{false};           ///< Whether the requested management action succeeded.
  bool verified{false};          ///< Whether a follow-up readback confirmed the applied state.
  bool has_result_code{false};   ///< True when result_code contains a decoded CMD_ERROR_RESP byte.
  uint8_t result_code{0};        ///< Optional CMD_ERROR_RESP result byte.
  std::string action;            ///< Action name, e.g. "rename_device".
  std::string device_id;         ///< Target IO-homecontrol device ID.
  std::string message;           ///< Human-readable outcome summary. Published as the action-result event, which
                                 ///< Home Assistant records: it never carries key material.
  std::string log_message;       ///< When non-empty, the device log shows this instead of `message`. For the
                                 ///< one report that may print a device key (scan_paired_devices()); the key
                                 ///< stays in the log and out of the event.
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
  std::string decoded;           ///< Rendering of the probe reply for the replies this codebase can
                                 ///< decode (currently a limitation reply, labelled "assumed layout");
                                 ///< empty otherwise. Reported beside the raw hex, never instead of it.
  bool terminal_refusal{false};  ///< True when probe_device() failed for a reason that will
                                 ///< recur identically for every remaining index in a sweep
                                 ///< (diagnostic probes not enabled, device moving) — set only by
                                 ///< probe_device(), read only by probe_sweep() to decide
                                 ///< whether to stop early. A structured flag rather than
                                 ///< probe_sweep() pattern-matching `message` text, which would
                                 ///< silently break if that text were ever reworded.
};

// === Shared helpers ===

/// Size of the scratch buffer format_hex_byte() prints into: two digits plus the terminator, with headroom.
inline constexpr size_t RESULT_CODE_BUFFER_SIZE = 5;

/// @brief Trim and uppercase a device-ID argument as received from the native API.
inline std::string normalize_device_id_argument(const std::string &device_id) {
  std::string normalized = trim_ascii_whitespace(device_id);
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
  return normalized;
}

/// @brief Format a byte as two uppercase hex digits, no prefix. Named neutrally rather than
/// after any one caller: used for CMD_ERROR_RESP result codes, probe reply/index command bytes,
/// and sweep index values alike -- none of those is a "result code" except the first.
inline std::string format_hex_byte(uint8_t value) {
  std::array<char, RESULT_CODE_BUFFER_SIZE> buffer{};
  std::snprintf(buffer.data(), buffer.size(), "%02X", value);
  return std::string(buffer.data());
}

/// @brief A result that carries only the action name and the target device ID.
inline ManagementActionResult make_management_result(const std::string &action, const std::string &device_id) {
  ManagementActionResult result;
  result.action = action;
  result.device_id = device_id;
  return result;
}

/// @brief Decode a CMD_ERROR_RESP frame's result code into `result`.
///
/// Populates has_result_code/result_code but deliberately leaves `result.message` untouched:
/// rename and identify_device report different wording for the same decoded code, so message
/// composition stays with each caller. On an empty error response, sets a stock message itself
/// (there is no code to report) and returns false; callers should treat that the same way as a
/// decoded code, just without result-code-specific wording.
/// @param response Frame whose cmd is CMD_ERROR_RESP.
/// @param result Result to populate.
/// @return true if a result code was decoded, false if the response carried no data.
inline bool apply_error_response(const IoFrame &response, ManagementActionResult &result) {
  if (response.data_len == 0) {
    result.message = "device returned an empty error response";
    return false;
  }
  result.has_result_code = true;
  result.result_code = response.data[0];
  return true;
}

/// @brief Shared preamble for every device-addressed action: validate the hub and device ID,
/// then resolve the target device.
///
/// Replaces `result` with the action name and normalized device ID, then checks (in order) that
/// the hub is initialized, that `device_id` is a well-formed 6-hex-digit ID, and that it names a
/// device registered on this hub. On any failure, sets `result.message` to a stable,
/// caller-facing string and returns nullptr; callers should return `result` immediately.
/// @param action Action name to record on `result`.
/// @param device_id Requested device ID (hex string, case-insensitive, as received from HA).
/// @param initialized The hub's initialized flag.
/// @param registry Device registry to look the device up in.
/// @param result Populated with the action name and normalized device ID, plus an error message
///               on failure.
/// @return Pointer to the resolved device, or nullptr if any validation step failed.
inline IoDevice *resolve_management_device(const char *action, const std::string &device_id, bool initialized,
                                           DeviceRegistry &registry, ManagementActionResult &result) {
  const std::string normalized_device_id = normalize_device_id_argument(device_id);
  result = make_management_result(action, normalized_device_id);

  if (!initialized) {
    result.message = "hub is not initialized";
    return nullptr;
  }

  uint8_t parsed_device_id[NODE_ID_SIZE]{};
  if (!hex_to_bytes(normalized_device_id, parsed_device_id, NODE_ID_SIZE)) {
    result.message = "device ID must be exactly 6 hexadecimal characters";
    return nullptr;
  }

  auto *dev = registry.get(normalized_device_id);
  if (dev == nullptr) {
    result.message = "device is not registered on this hub";
    return nullptr;
  }

  return dev;
}

}  // namespace home_io_control
}  // namespace esphome

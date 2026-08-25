/// @file management_actions.cpp
/// @brief Hub-level management actions such as device rename, identify, and force-open.
/// @ingroup hioc_hub
///
/// This file owns advanced management operations that are not part of the normal
/// entity surface. Operations are exposed as ESPHome native API actions so Home
/// Assistant can trigger them without adding always-visible helper entities. All
/// actions share one API service descriptor (detail::ManagementServiceDescriptor);
/// adding a new action is a registration call, not a new descriptor class.

#include "management_actions.h"

#include "hub_internal.h"  // brings in hub_core.h + all internal helpers + logging
#include "proto_commands.h"

#include "esphome/core/application.h"
#include "esphome/core/hal.h"

#if defined(USE_API_USER_DEFINED_ACTIONS) && defined(USE_API_CUSTOM_SERVICES)
#include "esphome/core/helpers.h"
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <vector>

namespace esphome {
namespace home_io_control {

namespace {

constexpr const char *MANAGEMENT_ACTION_RENAME_DEVICE = "rename_device";
constexpr const char *MANAGEMENT_ACTION_IDENTIFY_DEVICE = "identify_device";
constexpr const char *MANAGEMENT_ACTION_FORCE_OPEN_DEVICE = "force_open_device";
constexpr const char *MANAGEMENT_ACTION_SCAN_PAIRED_DEVICES = "scan_paired_devices";
constexpr const char *MANAGEMENT_ACTION_ONEWAY_SET_POSITION = "oneway_set_position";
constexpr const char *MANAGEMENT_ACTION_ONEWAY_REMOVE_CONTROLLER = "oneway_remove_controller";
constexpr const char *MANAGEMENT_ACTION_PROBE_DEVICE = "probe_device";
constexpr const char *MANAGEMENT_ACTION_PROBE_SWEEP = "probe_sweep";
constexpr const char *MANAGEMENT_RESULT_EVENT = "esphome.home_io_control_action_result";
constexpr size_t RESULT_CODE_BUFFER_SIZE = 5;
constexpr size_t UNEXPECTED_RESPONSE_MESSAGE_BUFFER_SIZE = 64;

// --- Diagnostic probe names (probe_device()/probe_sweep() `probe` argument) ---
constexpr const char *PROBE_NAME_PRIVATE_FN = "private_fn";          ///< Q0: create_private_function().
constexpr const char *PROBE_NAME_STATUS_EXT = "status_ext";          ///< Q1: create_get_status_extended().
constexpr const char *PROBE_NAME_GENERAL_INFO3 = "general_info3";    ///< Q2: create_general_info3().
constexpr const char *PROBE_NAME_PRIVATE2 = "private2";              ///< Q3 long form: create_private2_read().
constexpr const char *PROBE_NAME_PRIVATE2_SHORT = "private2_short";  ///< Q3 short form: create_private2_read().
/// Extended-CMD_PRIVATE selector "status_ext" probes: the field-observed, never-decoded 0x80.
/// Selector 0x20 (tilt) already has a permanent builder/action (create_get_status_tilt()) and is
/// not part of this probe.
constexpr uint8_t PROBE_STATUS_EXT_SELECTOR = 0x80;
/// Bound on probe_sweep()'s index range — this transmits on a shared ISM band to what may be a
/// battery device; an unbounded sweep is antisocial and would keep the device awake needlessly.
constexpr uint8_t PROBE_SWEEP_MAX_INDICES = 16;
/// Spacing between probe_sweep() indices. Sweep steps are sequential and blocking, so they never
/// overlap regardless of this value; it exists for ISM-band duty cycling and to give a battery
/// device real idle time between reads.
constexpr uint32_t PROBE_SWEEP_DELAY_MS = 1000;

/// @brief Uniform wrapper signature every probe builder is adapted to, so PROBE_TABLE below can
/// hold one function-pointer type regardless of each create_*() builder's own parameter list.
using ProbeBuilderFn = bool (*)(IoFrame &, const uint8_t *, const uint8_t *, uint8_t index);

bool build_probe_private_fn(IoFrame &f, const uint8_t *own, const uint8_t *dst, uint8_t index) {
  return create_private_function(f, own, dst, index);
}
bool build_probe_status_ext(IoFrame &f, const uint8_t *own, const uint8_t *dst, uint8_t index) {
  return create_get_status_extended(f, own, dst, PROBE_STATUS_EXT_SELECTOR, index);
}
bool build_probe_general_info3(IoFrame &f, const uint8_t *own, const uint8_t *dst, uint8_t /*index*/) {
  return create_general_info3(f, own, dst, /*low_power=*/true);
}
bool build_probe_private2_long(IoFrame &f, const uint8_t *own, const uint8_t *dst, uint8_t index) {
  return create_private2_read(f, own, dst, index, /*long_form=*/true, /*low_power=*/true);
}
bool build_probe_private2_short(IoFrame &f, const uint8_t *own, const uint8_t *dst, uint8_t index) {
  return create_private2_read(f, own, dst, index, /*long_form=*/false, /*low_power=*/true);
}

/// @brief One row per probe_device()/probe_sweep() `probe` argument value.
struct ProbeDescriptor {
  const char *name;
  bool needs_index;  ///< False only for "general_info3" -- its builder takes no index/selector.
  ProbeBuilderFn builder;
};

/// @brief The full set of probes probe_device()/probe_sweep() can dispatch to.
///
/// Single source of truth for probe names, replacing what used to be an if/else-if chain with
/// the "index must be..." error message written out once per branch. Adding, removing, or
/// renaming a probe is a one-line change here rather than a change to both probe_device()'s
/// dispatch and its error message. Deliberately has no "unknown4a" row -- see ADR 0024.
constexpr ProbeDescriptor PROBE_TABLE[] = {
    {PROBE_NAME_PRIVATE_FN, true, build_probe_private_fn},
    {PROBE_NAME_STATUS_EXT, true, build_probe_status_ext},
    {PROBE_NAME_GENERAL_INFO3, false, build_probe_general_info3},
    {PROBE_NAME_PRIVATE2, true, build_probe_private2_long},
    {PROBE_NAME_PRIVATE2_SHORT, true, build_probe_private2_short},
};
constexpr uint8_t PROBE_TABLE_SIZE = sizeof(PROBE_TABLE) / sizeof(PROBE_TABLE[0]);

/// @brief Look up a probe by name.
/// @return Pointer into PROBE_TABLE, or nullptr if `probe` names none of its rows.
const ProbeDescriptor *find_probe_descriptor(const std::string &probe) {
  for (const auto &descriptor : PROBE_TABLE) {
    if (probe == descriptor.name)
      return &descriptor;
  }
  return nullptr;
}

/// @brief The error message probe_device()/probe_sweep() report for an unrecognized `probe`
/// argument, built from PROBE_TABLE so the list of names can never drift out of sync with it.
std::string unknown_probe_message(const std::string &probe) {
  std::string names;
  for (uint8_t i = 0; i < PROBE_TABLE_SIZE; i++) {
    if (i > 0)
      names += (i + 1 == PROBE_TABLE_SIZE) ? ", or " : ", ";
    names += PROBE_TABLE[i].name;
  }
  return "unknown probe \"" + probe + "\" (expected " + names + ")";
}

/// @brief Maximum distinct responders reported by one scan_paired_devices() call.
///
/// Sized against the loop task's stack, which is where this runs: ESPHome spawns its own
/// `loopTask` with `ESPHOME_LOOP_TASK_STACK_SIZE` (8192 B) — not the 3.5 KB ESP-IDF main task.
/// `ScanResponder` is 12 B, so 24 slots cost ≈288 B (~3.5 % of that stack) in a single array;
/// replies are decoded on arrival rather than buffered as whole 33 B `IoFrame`s, which is what
/// makes a limit this size cheap. Installs with 20+ actuators are real, so 8 (the original
/// value, chosen when whole frames were retained) was too low to be useful. Overflow beyond
/// this is reported in the scan's own output, never silent.
constexpr uint8_t SCAN_MAX_REPLIES = 24;

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
  bool inverted;              ///< Decoded position-inversion flag.
  int16_t rssi_dbm;           ///< RSSI of the reply that produced this entry.
  uint8_t manufacturer;       ///< Raw manufacturer ID; name via manufacturer_name().
  uint8_t flags;              ///< Multi Information Byte; decode with DISCOVERY_FLAGS_* masks.
  bool has_extended;          ///< Whether manufacturer/flags above are present.
  bool metadata_complete;     ///< Whether type/subtype were present in the payload.
};

/// @brief Channels scan_paired_devices() transmits its roll-call request on, one attempt each.
///
/// A single broadcast on one fixed channel only reaches a paired device that happens to be
/// awake and listening on that exact channel at that exact instant — real hardware testing
/// found paired devices duty-cycle across all three channels independently of the hub, so a
/// one-shot broadcast on CH2 alone misses whichever devices are elsewhere in their cycle right
/// then. Retrying the same request on the other two channels gives every device up to three
/// chances to be listening when the hub transmits. CH2 first since it is the protocol's
/// designated TX channel (see FREQ_CH2's doc comment) and therefore the most likely to catch a
/// reply on the first attempt.
///
/// This TX retry is not compensating for a receive-side bug: for a long time the larger loss
/// was on the hub's own listen path (devices answered ~83% of attempts, the hub only received
/// ~36% of those replies, from a blind channel rotation), which has since been fixed by having
/// the listen extend its dwell on a detected preamble/sync instead of hopping mid-frame.
constexpr uint32_t SCAN_CHANNELS[] = {FREQ_CH2, FREQ_CH1, FREQ_CH3};
constexpr uint8_t SCAN_CHANNEL_COUNT = sizeof(SCAN_CHANNELS) / sizeof(SCAN_CHANNELS[0]);

}  // namespace

namespace detail {

#if defined(USE_API_USER_DEFINED_ACTIONS) && defined(USE_API_CUSTOM_SERVICES)
/// @brief Native API descriptor shared by every management action.
///
/// ESPHome 2026.x does not expose the generated YAML action helper runtime to external
/// components, so Home IO Control registers the action descriptor directly with APIServer.
/// This keeps the HA action surface identical to native ESPHome actions while avoiding
/// the unresolved link path behind CustomAPIDevice::register_service().
///
/// One descriptor class serves every action: it is parametrized by name, argument list,
/// and a callback that unpacks the request's string args and forwards them to a
/// ManagementActions method. The callback captures a ManagementActions* and calls only
/// public methods on it, so no friend declaration into the hub is needed. Adding action
/// N+1 is therefore a new register_user_service() call in register_actions(), not a new
/// descriptor class.
class ManagementServiceDescriptor : public api::UserServiceDescriptor {
 public:
  ManagementServiceDescriptor(const char *name, std::vector<const char *> arg_names,
                              std::function<void(const api::ExecuteServiceRequest &)> callback)
      : name_(name), key_(fnv1_hash(name)), arg_names_(std::move(arg_names)), callback_(std::move(callback)) {}

  api::ListEntitiesServicesResponse encode_list_service_response() override {
    api::ListEntitiesServicesResponse response;
    response.name = StringRef(this->name_);
    response.key = this->key_;
    response.supports_response = api::enums::SUPPORTS_RESPONSE_NONE;
    response.args.init(this->arg_names_.size());
    for (const char *arg_name : this->arg_names_) {
      auto &arg = response.args.emplace_back();
      arg.name = StringRef(arg_name);
      arg.type = api::enums::SERVICE_ARG_TYPE_STRING;
    }
    return response;
  }

  bool execute_service(const api::ExecuteServiceRequest &request) override {
    if (request.key != this->key_ || request.args.size() != this->arg_names_.size())
      return false;
    this->callback_(request);
    return true;
  }

#ifdef USE_API_USER_DEFINED_ACTION_RESPONSES
  bool execute_service(const api::ExecuteServiceRequest &request, uint32_t) override {
    return this->execute_service(request);
  }
#endif

 protected:
  const char *name_;
  uint32_t key_;
  std::vector<const char *> arg_names_;
  std::function<void(const api::ExecuteServiceRequest &)> callback_;
};
#endif

}  // namespace detail

// --- Helper free functions (file-local) ---

static std::string normalize_device_id_argument(const std::string &device_id) {
  std::string normalized = trim_ascii_whitespace(device_id);
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
  return normalized;
}

static std::string bool_to_string(bool value) { return value ? "true" : "false"; }

/// @brief Format a byte as two uppercase hex digits, no prefix. Named neutrally rather than
/// after any one caller: used for CMD_ERROR_RESP result codes, probe reply/index command bytes,
/// and sweep index values alike -- none of those is a "result code" except the first.
static std::string format_hex_byte(uint8_t value) {
  std::array<char, RESULT_CODE_BUFFER_SIZE> buffer{};
  std::snprintf(buffer.data(), buffer.size(), "%02X", value);
  return std::string(buffer.data());
}

static ManagementActionResult make_management_result(const std::string &action, const std::string &device_id) {
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
static bool apply_error_response(const IoFrame &response, ManagementActionResult &result) {
  if (response.data_len == 0) {
    result.message = "device returned an empty error response";
    return false;
  }
  result.has_result_code = true;
  result.result_code = response.data[0];
  return true;
}

/// @brief Parse a probe_device()/probe_sweep() index argument into a byte.
///
/// Accepts both a bare decimal string ("6") and a "0x"-prefixed hex string ("0x06") — the two
/// shapes a Home Assistant user is likely to type when copying a value out of a captured frame
/// dump. Rejects anything else (empty string, trailing garbage, out-of-range value) rather than
/// silently defaulting to 0, since a wrong index silently sent as 0 would misrepresent what was
/// actually probed.
/// @param text Argument as received from the native API call.
/// @param out Parsed byte on success; left untouched on failure.
/// @return true if `text` is exactly one well-formed byte value.
static bool parse_probe_index(const std::string &text, uint8_t &out) {
  if (text.empty())
    return false;
  // A leading "0" followed by another digit (e.g. "010") is valid octal to strtoul() below and
  // would silently probe a different index than a user copying a byte value out of a hex dump
  // intended -- reject it explicitly rather than relying on strtoul()'s octal parsing, which only
  // rejects the invalid-octal-digit cases ("08", "09"), not the valid ones.
  if (text.size() > 1 && text[0] == '0' && text[1] != 'x' && text[1] != 'X')
    return false;
  errno = 0;
  char *end = nullptr;
  // Base 0: strtoul() itself recognizes a "0x"/"0X" prefix as hex and a bare "0" as decimal
  // zero, which is exactly the "6" / "0x06" / "0" shape probes need to accept.
  const uint32_t value = std::strtoul(text.c_str(), &end, 0);
  if (end != text.c_str() + text.size())
    return false;
  if (errno == ERANGE || value > std::numeric_limits<uint8_t>::max())
    return false;
  out = static_cast<uint8_t>(value);
  return true;
}

// --- ManagementActions ---

ManagementActions::ManagementActions(const uint8_t *node_id, const uint8_t *system_key, const TuningConfig *tuning,
                                     ExchangeEngine &engine, DeviceRegistry &registry, const bool *initialized,
                                     IOHomeControlComponent *hub)
    : node_id_(node_id),
      system_key_(system_key),
      tuning_(tuning),
      engine_(engine),
      registry_(registry),
      initialized_(initialized),
      hub_(hub) {}

void ManagementActions::register_actions() {
#if defined(USE_API_USER_DEFINED_ACTIONS) && defined(USE_API_CUSTOM_SERVICES)
  if (api::global_api_server == nullptr) {
    ESP_LOGW(detail::TAG, "Native API server not available, management actions will not be registered");
    return;
  }
  api::global_api_server->register_user_service(new detail::ManagementServiceDescriptor(  // NOLINT
      MANAGEMENT_ACTION_RENAME_DEVICE, {"device_id", "new_name"}, [this](const api::ExecuteServiceRequest &request) {
        this->api_rename_device(request.args[0].string_.str(), request.args[1].string_.str());
      }));
  api::global_api_server->register_user_service(new detail::ManagementServiceDescriptor(  // NOLINT
      MANAGEMENT_ACTION_IDENTIFY_DEVICE, {"device_id"},
      [this](const api::ExecuteServiceRequest &request) { this->api_identify_device(request.args[0].string_.str()); }));
  api::global_api_server->register_user_service(new detail::ManagementServiceDescriptor(  // NOLINT
      MANAGEMENT_ACTION_FORCE_OPEN_DEVICE, {"device_id"}, [this](const api::ExecuteServiceRequest &request) {
        this->api_force_open_device(request.args[0].string_.str());
      }));
  api::global_api_server->register_user_service(new detail::ManagementServiceDescriptor(  // NOLINT
      MANAGEMENT_ACTION_SCAN_PAIRED_DEVICES, {},
      [this](const api::ExecuteServiceRequest &) { this->api_scan_paired_devices(); }));
  // `position` arrives as a string because ManagementServiceDescriptor exposes every argument as
  // SERVICE_ARG_TYPE_STRING. Widening it to typed arguments would change a shipped API surface
  // for one new parameter, so this action parses instead — and rejects loudly, since an
  // unparseable position that silently became 0 would send a fully-open command.
  api::global_api_server->register_user_service(new detail::ManagementServiceDescriptor(  // NOLINT
      MANAGEMENT_ACTION_ONEWAY_SET_POSITION, {"controller_id", "position"},
      [this](const api::ExecuteServiceRequest &request) {
        this->api_oneway_set_position(request.args[0].string_.str(), request.args[1].string_.str());
      }));
  api::global_api_server->register_user_service(new detail::ManagementServiceDescriptor(  // NOLINT
      MANAGEMENT_ACTION_ONEWAY_REMOVE_CONTROLLER, {"controller_id"}, [this](const api::ExecuteServiceRequest &request) {
        this->api_oneway_remove_controller(request.args[0].string_.str());
      }));
  // Registered only when diagnostic_probes: true was set in YAML, so the action list stays clean
  // on a default build -- diagnostic_probes_enabled() already holds its final YAML-configured
  // value here: __init__.py's to_code() emits set_diagnostic_probes_enabled() as a plain
  // property-setter call in generated main.cpp, which runs before App.setup() calls this
  // component's setup() (and therefore this method), not after.
  if (hub_->diagnostic_probes_enabled()) {
    api::global_api_server->register_user_service(new detail::ManagementServiceDescriptor(  // NOLINT
        MANAGEMENT_ACTION_PROBE_DEVICE, {"device_id", "probe", "index"},
        [this](const api::ExecuteServiceRequest &request) {
          this->api_probe_device(request.args[0].string_.str(), request.args[1].string_.str(),
                                 request.args[2].string_.str());
        }));
    api::global_api_server->register_user_service(new detail::ManagementServiceDescriptor(  // NOLINT
        MANAGEMENT_ACTION_PROBE_SWEEP, {"device_id", "probe", "first_index", "last_index"},
        [this](const api::ExecuteServiceRequest &request) {
          this->api_probe_sweep(request.args[0].string_.str(), request.args[1].string_.str(),
                                request.args[2].string_.str(), request.args[3].string_.str());
        }));
  }
#endif
}

IoDevice *ManagementActions::resolve_device_(const char *action, const std::string &device_id,
                                             ManagementActionResult &result) {
  const std::string normalized_device_id = normalize_device_id_argument(device_id);
  result = make_management_result(action, normalized_device_id);

  if (!*initialized_) {
    result.message = "hub is not initialized";
    return nullptr;
  }

  uint8_t parsed_device_id[NODE_ID_SIZE]{};
  if (!hex_to_bytes(normalized_device_id, parsed_device_id, NODE_ID_SIZE)) {
    result.message = "device ID must be exactly 6 hexadecimal characters";
    return nullptr;
  }

  auto *dev = registry_.get(normalized_device_id);
  if (dev == nullptr) {
    result.message = "device is not registered on this hub";
    return nullptr;
  }

  return dev;
}

bool ManagementActions::send_authenticated_request_(const IoFrame &request, IoFrame &response, const char *action_verb,
                                                    ManagementActionResult &result) {
  const ExchangeOutcome outcome = engine_.send_and_receive(request, response, FREQ_CH2);
  if (outcome == ExchangeOutcome::SUCCESS_WITH_RESPONSE)
    return true;
  engine_.log_debug(result.device_id.c_str());
  // A management action's whole point is the payload it reads back (a name, an info block), so an
  // unconfirmed acceptance still cannot satisfy the caller — but it is a materially different
  // situation from silence, and saying so saves the user chasing a link problem that isn't one.
  result.message = outcome == ExchangeOutcome::SUCCESS_UNCONFIRMED
                       ? std::string("device accepted the ") + action_verb + " request but sent no response"
                       : std::string("no valid response to ") + action_verb + " request";
  return false;
}

void ManagementActions::api_rename_device(const std::string &device_id, const std::string &new_name) {
  publish_result(rename_device(device_id, new_name));
}

void ManagementActions::publish_result(const ManagementActionResult &result) {
  // device_id is empty for actions with no single target (e.g. scan_paired_devices' roll-call);
  // the "for device %s" clause is omitted rather than rendering as "for device :".
  const bool has_device = !result.device_id.empty();
  std::string prefix = "Management action " + result.action;
  if (has_device)
    prefix += " for device " + result.device_id;
  prefix += result.success ? ": " : " failed: ";
  detail::log_multiline_result(detail::TAG, !result.success, prefix, result.message);

  if (!hub_->is_connected())
    return;

  std::map<std::string, std::string> event_data{{"action", result.action},
                                                {"device_id", result.device_id},
                                                {"success", bool_to_string(result.success)},
                                                {"verified", bool_to_string(result.verified)},
                                                {"message", result.message}};

  if (!result.requested_name.empty())
    event_data["requested_name"] = result.requested_name;
  if (!result.applied_name.empty())
    event_data["applied_name"] = result.applied_name;
  if (result.has_result_code) {
    event_data["result_code"] = format_hex_byte(result.result_code);
    event_data["result_code_name"] = command_result_name(result.result_code);
  }
  if (!result.probe_name.empty()) {
    event_data["probe"] = result.probe_name;
    // probe_index is empty only until a sweep has parsed its range; both probe_device() and
    // probe_sweep() fill it before publishing, so an absent key means "no index applies" rather
    // than "the index was blank".
    if (!result.probe_index.empty())
      event_data["index"] = result.probe_index;
  }
  if (result.has_response_cmd) {
    event_data["response_cmd"] = format_hex_byte(result.response_cmd);
    event_data["response_cmd_name"] = command_name(result.response_cmd);
  }
  if (!result.response_hex.empty())
    event_data["response_hex"] = result.response_hex;

  hub_->fire_homeassistant_event(MANAGEMENT_RESULT_EVENT, event_data);
}

ManagementActionResult ManagementActions::rename_device(const std::string &device_id, const std::string &new_name) {
  ManagementActionResult result;
  auto *dev = resolve_device_(MANAGEMENT_ACTION_RENAME_DEVICE, device_id, result);
  if (dev == nullptr)
    return result;

  uint8_t payload[DEVICE_NAME_WRITE_PAYLOAD_SIZE];
  std::string normalized_name;
  const DeviceNameValidationError name_error = encode_device_name_payload(new_name, payload, normalized_name);
  result.requested_name = normalized_name.empty() ? trim_ascii_whitespace(new_name) : normalized_name;
  if (name_error != DeviceNameValidationError::NONE) {
    result.message = device_name_validation_error_description(name_error);
    return result;
  }

  IoFrame request;
  if (!create_set_name(request, node_id_, dev->node_id, payload)) {
    result.message = "failed to build rename request";
    return result;
  }

  IoFrame response;
  if (!send_authenticated_request_(request, response, "rename", result))
    return result;

  if (response.cmd == CMD_ERROR_RESP) {
    if (apply_error_response(response, result)) {
      result.message =
          std::string(command_result_name(result.result_code)) + ": " + command_result_description(result.result_code);
    }
    return result;
  }

  if (response.cmd != CMD_SET_NAME_RESP) {
    std::array<char, UNEXPECTED_RESPONSE_MESSAGE_BUFFER_SIZE> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "unexpected rename response 0x%02X", response.cmd);
    result.message = buffer.data();
    return result;
  }

  result.success = true;
  result.message = "rename acknowledged by device";

  if (!hub_->request_device_name(result.device_id)) {
    result.message = "rename acknowledged but verification readback failed";
    return result;
  }

  auto *updated_device = registry_.get(result.device_id);
  if (updated_device != nullptr)
    result.applied_name = updated_device->name;

  if (result.applied_name == normalized_name) {
    result.verified = true;
    result.message = "rename verified by device readback";
    return result;
  }

  result.message = "rename acknowledged but readback did not match the requested name";
  return result;
}

void ManagementActions::api_identify_device(const std::string &device_id) {
  publish_result(identify_device(device_id));
}

ManagementActionResult ManagementActions::identify_device(const std::string &device_id) {
  ManagementActionResult result;
  // Deliberately no device-type gating beyond "registered on this hub" — see the doxygen note on
  // the declaration for why.
  auto *dev = resolve_device_(MANAGEMENT_ACTION_IDENTIFY_DEVICE, device_id, result);
  if (dev == nullptr)
    return result;

  IoFrame request;
  if (!create_identify(request, node_id_, dev->node_id)) {
    result.message = "failed to build identify request";
    return result;
  }

  IoFrame response;
  if (!send_authenticated_request_(request, response, "identify", result))
    return result;

  if (response.cmd == CMD_ERROR_RESP) {
    // Deliberate deviation from rename's error handling: a device may answer CMD_IDENTIFY with
    // CMD_ERROR_RESP and still have performed the jog, so this counts as success, not failure.
    result.success = true;
    if (apply_error_response(response, result)) {
      result.message = "identify triggered (device reported " + std::string(command_result_name(result.result_code)) +
                       ": " + command_result_description(result.result_code) + ")";
    } else {
      result.message = "identify triggered (device returned an empty error response)";
    }
    return result;
  }

  // Any other endpoint-matched reply counts as acknowledgment; unlike rename there is no specific
  // response command to check against, and no readback exists to set `verified`.
  result.success = true;
  result.message = "identify acknowledged by device";
  return result;
}

void ManagementActions::api_force_open_device(const std::string &device_id) {
  publish_result(force_open_device(device_id));
}

ManagementActionResult ManagementActions::force_open_device(const std::string &device_id) {
  ManagementActionResult result;
  auto *dev = resolve_device_(MANAGEMENT_ACTION_FORCE_OPEN_DEVICE, device_id, result);
  if (dev == nullptr)
    return result;

  // Delegate to the hub's normal cover-command dispatch path (capability gating, poll tracking,
  // settle handling, backoff already live there) instead of talking to the radio directly.
  if (!hub_->queue_device_command(result.device_id, CoverCommand::FORCE_OPEN)) {
    result.message = "device does not accept cover commands";
    return result;
  }

  result.success = true;
  result.message =
      "force open queued (elevated-priority open; wind/rain lock bypass unconfirmed; movement result arrives via "
      "cover state)";
  return result;
}

void ManagementActions::api_scan_paired_devices() { publish_result(scan_paired_devices()); }

void ManagementActions::api_oneway_set_position(const std::string &controller_id, const std::string &position) {
  // device_id carries the controller-identity handle: 1W addresses a class, so there is no device
  // to name, and the identity is what the caller actually chose.
  ManagementActionResult result = make_management_result(MANAGEMENT_ACTION_ONEWAY_SET_POSITION, controller_id);

  if (hub_->oneway_controllers().get(controller_id) == nullptr) {
    result.message = "no oneway_controllers identity with that id";
    publish_result(result);
    return;
  }

  const std::string trimmed = trim_ascii_whitespace(position);
  if (trimmed.empty() || trimmed.find_first_not_of("0123456789") != std::string::npos) {
    result.message = "position must be a whole number between 0 and 100";
    publish_result(result);
    return;
  }
  const unsigned long parsed = strtoul(trimmed.c_str(), nullptr, 10);  // NOLINT(google-runtime-int)
  if (parsed > ONEWAY_POSITION_FULLY_CLOSED) {
    result.message = "position must be between 0 and 100";
    publish_result(result);
    return;
  }

  hub_->send_oneway_position(controller_id, static_cast<uint8_t>(parsed));
  result.success = true;
  // Deliberately "queued", not "sent" or "applied": 1W reports nothing back, and neither should
  // this. See the "Last 1W Command" sensor for what was actually transmitted.
  result.message = "1W position command queued";
  publish_result(result);
}

void ManagementActions::api_oneway_remove_controller(const std::string &controller_id) {
  // device_id carries the controller-identity handle, same convention as api_oneway_set_position().
  ManagementActionResult result = make_management_result(MANAGEMENT_ACTION_ONEWAY_REMOVE_CONTROLLER, controller_id);

  if (hub_->oneway_controllers().get(controller_id) == nullptr) {
    result.message = "no oneway_controllers identity with that id";
    publish_result(result);
    return;
  }

  hub_->send_oneway_unenroll(controller_id);
  result.success = true;
  // "Queued", not "removed": 1W has no reply, so nothing here can ever confirm a device actually
  // forgot this identity — same framing as every other 1W action result.
  result.message = "1W remove-controller (0x39) queued";
  publish_result(result);
}

/// @brief Format one roll-call responder's report line(s).
///
/// Known responders get a single summary line; unknown responders get the same summary line
/// plus a lead-in sentence and a ready-to-paste YAML block (or, if the decoded type has no
/// ESPHome platform, an explanatory line instead of a blank) — the same "paste this in" framing
/// a successful pairing prints.
/// @param responder Decoded responder record.
/// @param device_id Hex device ID string for this responder, rebuilt from `responder.src`.
/// @param known True if this device is already registered on this hub.
static std::string format_scan_reply_line(const ScanResponder &responder, const std::string &device_id, bool known) {
  std::string line = "  " + device_id + ": " + device_type_name(responder.type) +
                     " subtype=" + std::to_string(responder.subtype) + " rssi=" + std::to_string(responder.rssi_dbm) +
                     "dBm";
  if (responder.has_extended) {
    uint8_t const att = discovery_att_class(responder.flags);
    uint8_t const power_save = discovery_power_save_mode(responder.flags);
    line += std::string(" manufacturer=") + manufacturer_name(responder.manufacturer) +
            " turnaround=" + att_class_name(att) + " power_save=" + power_save_mode_name(power_save);
  }
  line += known ? " [known]\n" : " [unknown]\n";

  if (known)
    return line;

  const std::string snippet = build_device_yaml_snippet(responder.type, responder.subtype, device_id,
                                                        responder.metadata_complete, responder.inverted);
  if (!snippet.empty())
    return line + "    Paste this into your YAML to register it:\n" + snippet;

  return line + "    no ready-to-paste YAML: no ESPHome platform for io_device_type: " +
         format_device_type_for_yaml(responder.type) + "\n";
}

/// @brief Outcome of add_scan_responder(), so callers can tell a harmless repeat from real loss.
enum class ScanAddResult : uint8_t {
  ADDED,      ///< New responder recorded.
  DUPLICATE,  ///< Already recorded from an earlier reply; nothing changed.
  FULL,       ///< Dropped: SCAN_MAX_REPLIES distinct responders already recorded.
};

/// @brief Record a responder unless its address is already present.
///
/// Deduplication is by node ID across the whole scan, so a device that answers several of the
/// three attempts — or twice inside one attempt — still yields one entry. The duplicate check
/// runs before the capacity check so that repeat replies from already-recorded devices never
/// look like overflow once the array is full.
/// @param responders Accumulated array, appended to in place.
/// @param count In: entries already present. Out: updated count.
/// @param capacity Maximum entries `responders` can hold.
/// @param frame Reply frame to decode and store.
/// @param rssi_dbm RSSI of that reply.
/// @return Which of the three outcomes occurred.
static ScanAddResult add_scan_responder(ScanResponder *responders, uint8_t &count, uint8_t capacity,
                                        const IoFrame &frame, int16_t rssi_dbm) {
  for (uint8_t i = 0; i < count; i++) {
    if (memcmp(responders[i].src, frame.src, NODE_ID_SIZE) == 0)
      return ScanAddResult::DUPLICATE;
  }
  if (count >= capacity)
    return ScanAddResult::FULL;

  // decode_discovery_response() also produces the hex device-ID string, which is deliberately
  // discarded here and rebuilt from `src` when the report is formatted. Keeping it would mean a
  // std::string per entry — more memory than the entire ScanResponder — to save re-deriving six
  // characters that fit in a small-string buffer. Do not "optimise" this by adding a string member.
  IoDevice device{};
  std::string unused_device_id;
  const DiscoveryResponseInfo info = decode_discovery_response(frame, device, unused_device_id);

  ScanResponder &entry = responders[count++];
  memcpy(entry.src, frame.src, NODE_ID_SIZE);
  entry.type = device.type;
  entry.subtype = device.subtype;
  entry.inverted = device.inverted;
  entry.rssi_dbm = rssi_dbm;
  entry.manufacturer = info.manufacturer;
  entry.flags = info.flags;
  entry.has_extended = info.has_extended;
  entry.metadata_complete = info.metadata_complete;
  return ScanAddResult::ADDED;
}

ManagementActionResult ManagementActions::scan_paired_devices() {
  ManagementActionResult result = make_management_result(MANAGEMENT_ACTION_SCAN_PAIRED_DEVICES, "");

  if (!*initialized_) {
    result.message = "hub is not initialized";
    return result;
  }

  // One attempt per channel (see SCAN_CHANNELS), deduplicating responders across attempts — a
  // device that is awake and replies to more than one attempt must still only appear once in the
  // report. Each attempt gets a fresh request (create_discovery_request() draws a new random
  // nonce every call) rather than replaying the same frame three times. Every channel is always
  // tried, even once the array is full: stopping early would silently skip channels, which is
  // exactly the single-channel behaviour the three attempts exist to avoid.
  ScanResponder responders[SCAN_MAX_REPLIES];
  uint8_t count = 0;
  bool truncated = false;
  for (uint8_t attempt = 0; attempt < SCAN_CHANNEL_COUNT; attempt++) {
    IoFrame request;
    if (!create_discovery_request(request, node_id_, CMD_DISCOVER_SPE_REQ, BROADCAST_DISCOVER, /*low_power=*/false,
                                  /*payload_enabled=*/false, /*payload=*/0, system_key_)) {
      result.message = "failed to build roll-call request";
      return result;
    }

    const uint8_t before = count;
    const uint8_t heard = engine_.collect_broadcast_responses(
        request, SCAN_CHANNELS[attempt], CMD_DISCOVER_SPE_RESP, tuning_->pairing_discovery_wait_ms,
        [&responders, &count, &truncated](const IoFrame &frame, int16_t rssi_dbm) {
          if (add_scan_responder(responders, count, SCAN_MAX_REPLIES, frame, rssi_dbm) == ScanAddResult::FULL) {
            truncated = true;
          }
        });
    // Diagnostic only (not part of the user-facing report). Reporting heard and new separately is
    // what makes it useful: "heard 3, 0 new" means devices are answering every attempt (so the
    // extra channels are redundant here). The Hz below is the TX channel for this attempt only —
    // collect_broadcast_responses() listens with ROTATE_SKIPPING_REQUEST (see SCAN_CHANNELS' doc
    // comment), so replies are never received on it. "heard 0" therefore says nothing about that
    // channel specifically; it means neither of the *other* two channels caught a reply in this
    // attempt's window.
    const uint8_t new_count = count - before;
    ESP_LOGD(detail::TAG, "Roll-call attempt %u/%u (tx %" PRIu32 " Hz, %u ms window): %u repl%s heard, %u new",
             attempt + 1, SCAN_CHANNEL_COUNT, SCAN_CHANNELS[attempt], tuning_->pairing_discovery_wait_ms, heard,
             heard == 1 ? "y" : "ies", new_count);
  }

  // Grouped into known-first, unknown-second rather than interleaved in arrival order: the two
  // groups need very different follow-up (nothing to do vs. paste a YAML block), so burying an
  // unknown responder between two known ones makes it easy to miss.
  std::string known_body;
  std::string unknown_body;
  uint8_t unknown_count = 0;
  for (uint8_t i = 0; i < count; i++) {
    const std::string device_id = node_id_to_string(responders[i].src);
    const bool known = registry_.get(device_id) != nullptr;
    if (known) {
      known_body += format_scan_reply_line(responders[i], device_id, known);
    } else {
      unknown_count++;
      unknown_body += format_scan_reply_line(responders[i], device_id, known);
    }
  }

  std::string body;
  if (!known_body.empty())
    body += "Known:\n" + known_body;
  if (!unknown_body.empty())
    body += "Unknown:\n" + unknown_body;

  result.success = true;
  result.message = "Roll-call: " + std::to_string(count) + " device" + (count == 1 ? "" : "s") + " detected (" +
                   std::to_string(count - unknown_count) + " known, " + std::to_string(unknown_count) + " unknown)\n";
  // Surfaced in the report itself, not only the log: a scan that silently listed a subset would
  // look like devices had gone missing.
  if (truncated) {
    result.message += "NOTE: more than " + std::to_string(SCAN_MAX_REPLIES) +
                      " devices answered; the list below is truncated. Re-run to see whether other devices "
                      "appear, and raise SCAN_MAX_REPLIES if this install really is larger.\n";
  }
  result.message += body;
  return result;
}

void ManagementActions::api_probe_device(const std::string &device_id, const std::string &probe,
                                         const std::string &index) {
  publish_result(probe_device(device_id, probe, index));
}

ManagementActionResult ManagementActions::probe_device(const std::string &device_id, const std::string &probe,
                                                       const std::string &index) {
  // resolve_device_() reassigns its `result` argument wholesale (via make_management_result()),
  // so it must write into its own object rather than the one already carrying probe_name/index --
  // matches probe_sweep()'s resolve_result pattern below.
  ManagementActionResult resolve_result;
  auto *dev = resolve_device_(MANAGEMENT_ACTION_PROBE_DEVICE, device_id, resolve_result);
  if (dev == nullptr) {
    resolve_result.probe_name = probe;
    resolve_result.probe_index = index;
    return resolve_result;
  }

  ManagementActionResult result = resolve_result;
  result.probe_name = probe;
  result.probe_index = index;

  if (!hub_->diagnostic_probes_enabled()) {
    result.message = "diagnostic probes are not enabled; set diagnostic_probes: true in YAML";
    result.terminal_refusal = true;
    return result;
  }
  // An unknown frame into a mid-transaction device state machine is the one avoidable way a
  // read-shaped probe could cause harm.
  if (!dev->is_stopped) {
    result.message = "device is moving; refusing to probe mid-transaction";
    result.terminal_refusal = true;
    return result;
  }

  const ProbeDescriptor *descriptor = find_probe_descriptor(probe);
  if (descriptor == nullptr) {
    result.message = unknown_probe_message(probe);
    return result;
  }

  uint8_t index_byte = 0;
  if (descriptor->needs_index && !parse_probe_index(index, index_byte)) {
    result.message = "index must be a decimal or 0x-prefixed byte value (0-255)";
    return result;
  }

  // low_power=true for every probe that takes it as a parameter: this codebase does not track
  // per-device power class (see create_get_name()'s call site in hub_operations.cpp for the same
  // reasoning), and the corpus shows real devices accepting the flag whether or not they need it.
  IoFrame request;
  if (!descriptor->builder(request, node_id_, dev->node_id, index_byte)) {
    result.message = "failed to build probe request";
    return result;
  }

  IoFrame response;
  // A probe exists to read back a payload, so -- like a status poll or name read, and unlike a
  // bare CMD_EXECUTE -- SUCCESS_UNCONFIRMED (device accepted the request but sent nothing back)
  // does not satisfy the caller here.
  const ExchangeOutcome outcome = engine_.send_and_receive(request, response, FREQ_CH2);
  if (outcome != ExchangeOutcome::SUCCESS_WITH_RESPONSE) {
    engine_.log_debug(result.device_id.c_str());
    result.message = outcome == ExchangeOutcome::SUCCESS_UNCONFIRMED
                         ? "device accepted the probe request but sent no response"
                         : "no reply after " + std::to_string(EXCHANGE_RETRY_COUNT) +
                               " attempts (device asleep, unreachable, or silently ignoring this opcode)";
    return result;
  }

  // Report the raw reply, not an interpretation -- the whole point of a probe is that we do not
  // know what these bytes mean yet. This never touches update_device_status_(): ManagementActions
  // has no access to that protected hub method at all (see hub_core.h), so a probe reply can
  // never be misread as a position update.
  uint8_t raw[FRAME_MAX_SIZE] = {0};
  const uint8_t raw_len = serialize(response, raw, sizeof(raw));
  char hex[FRAME_LOG_HEX_BUFFER_SIZE];
  render_frame_hex_redacted(raw, raw_len, hex, sizeof(hex));

  // Log through the same "io_capture" structured tag every other received frame uses
  // (log_component_capture(), hub_internal.h) rather than relying on IOHOME_FRAME_LOG -- that
  // build flag is opt-in (only set in the loopback/monitor configs under config/), and the
  // always-on receive path (process_received_packet_(), gated on !busy_) never sees a probe
  // reply at all, since the reply is consumed here, inside a blocking send_and_receive() call,
  // while busy_ is still true. Without this call a probe reply would only appear in the action's
  // hex message/event on a default build -- not a form scripts/corpus/ingest.py parses -- despite
  // the reply already having gone out over an authenticated, radio-verified exchange.
  detail::log_component_capture(hub_->get_radio(), "probe_rx", raw, raw_len, &response);

  result.success = true;
  result.has_response_cmd = true;
  result.response_cmd = response.cmd;
  result.response_hex = hex;
  result.message = "probe \"" + probe + "\" reply cmd=0x" + format_hex_byte(response.cmd) + " (" +
                   command_name(response.cmd) + ") hex=" + hex;
  // The status-reply table is a decoded part of the protocol, unlike the probe payload itself --
  // unlike the payload bytes, a result code is not something the probe exists to discover.
  if (response.cmd == CMD_ERROR_RESP && apply_error_response(response, result)) {
    result.message += " [" + std::string(command_result_name(result.result_code)) + ": " +
                      command_result_description(result.result_code) + "]";
  }
  return result;
}

void ManagementActions::api_probe_sweep(const std::string &device_id, const std::string &probe,
                                        const std::string &first_index, const std::string &last_index) {
  publish_result(probe_sweep(device_id, probe, first_index, last_index));
}

ManagementActionResult ManagementActions::probe_sweep(const std::string &device_id, const std::string &probe,
                                                      const std::string &first_index, const std::string &last_index) {
  ManagementActionResult result =
      make_management_result(MANAGEMENT_ACTION_PROBE_SWEEP, normalize_device_id_argument(device_id));
  result.probe_name = probe;

  // Validate the device and the probe name once, up front. Neither can start succeeding partway
  // through a range: an unregistered device or an unrecognized probe name fails identically for
  // every index, so without this check the loop below would run the full span and append the
  // same error line up to PROBE_SWEEP_MAX_INDICES times.
  ManagementActionResult resolve_result;
  if (resolve_device_(MANAGEMENT_ACTION_PROBE_SWEEP, device_id, resolve_result) == nullptr) {
    result.message = resolve_result.message;
    return result;
  }
  const ProbeDescriptor *descriptor = find_probe_descriptor(probe);
  if (descriptor == nullptr) {
    result.message = unknown_probe_message(probe);
    return result;
  }
  if (!descriptor->needs_index) {
    result.message = "probe \"" + probe + "\" takes no index; use probe_device";
    return result;
  }

  uint8_t first = 0;
  uint8_t last = 0;
  if (!parse_probe_index(first_index, first) || !parse_probe_index(last_index, last)) {
    result.message = "first_index/last_index must be decimal or 0x-prefixed byte values (0-255)";
    return result;
  }
  if (last < first) {
    result.message = "last_index must be >= first_index";
    return result;
  }
  const uint32_t span = static_cast<uint32_t>(last) - first + 1;
  if (span > PROBE_SWEEP_MAX_INDICES) {
    result.message =
        "sweep range too wide: " + std::to_string(span) + " indices, max " + std::to_string(PROBE_SWEEP_MAX_INDICES);
    return result;
  }

  std::string report;
  uint32_t answered_count = 0;
  bool stopped_early = false;
  for (uint32_t idx = first; idx <= last; idx++) {
    if (idx != first) {
      App.feed_wdt();
      delay(PROBE_SWEEP_DELAY_MS);
    }
    const std::string index_str = std::to_string(idx);
    const ManagementActionResult step = probe_device(device_id, probe, index_str);
    report += "index=0x" + format_hex_byte(static_cast<uint8_t>(idx)) + ": ";
    if (step.success) {
      answered_count++;
      report += "cmd=0x" + format_hex_byte(step.response_cmd) + " hex=" + step.response_hex + "\n";
    } else {
      report += step.message + "\n";
    }
    // terminal_refusal (diagnostic probes not enabled, device moving) applies to every remaining
    // index the same way it applied to this one -- stop rather than repeat it N more times. A
    // structured flag, not a search over `step.message`: probe_device() sets it explicitly, so
    // rewording a refusal message can never silently break this check.
    if (step.terminal_refusal) {
      report += "(stopping sweep: " + step.message + ")\n";
      stopped_early = true;
      break;
    }
  }

  // A sweep that ran its full requested range is a success even if nothing answered -- "no
  // device answered any index" is a valid, reportable outcome, the same philosophy
  // scan_paired_devices() uses for zero replies. A sweep cut short by a terminal refusal did not
  // complete what was asked of it and must not report success.
  result.success = !stopped_early;
  // Renders the same 0x-prefixed form as the report body, so the Home Assistant event carries the
  // swept range rather than a blank `index` field left over from the per-index probe_device()
  // results this loop discards.
  result.probe_index = "0x" + format_hex_byte(first) + "-0x" + format_hex_byte(last);
  result.message = "Sweep \"" + probe + "\" over [0x" + format_hex_byte(first) + ",0x" + format_hex_byte(last) +
                   "]: " + std::to_string(answered_count) + " answered\n" + report;
  return result;
}

}  // namespace home_io_control
}  // namespace esphome

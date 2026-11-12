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

#include "entity_helpers.h"
#include "hub_core.h"
#include "log_helpers.h"
#include "proto_commands.h"
#include "proto_device_types.h"
#include "roll_call.h"

#include "esphome/core/hal.h"

#if defined(USE_API_USER_DEFINED_ACTIONS) && defined(USE_API_CUSTOM_SERVICES)
#include "esphome/core/helpers.h"
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <span>
#include <type_traits>
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
constexpr const char *MANAGEMENT_ACTION_HEATING_CONTROL = "heating_control";
constexpr const char *MANAGEMENT_RESULT_EVENT = "esphome.home_io_control_action_result";
constexpr size_t UNEXPECTED_RESPONSE_MESSAGE_BUFFER_SIZE = 64;
constexpr size_t HEATING_RANGE_MESSAGE_BUFFER_SIZE = 64;

}  // namespace

namespace detail {

#if defined(USE_API_USER_DEFINED_ACTIONS) && defined(USE_API_CUSTOM_SERVICES)

/// True when the installed ESPHome's `UserServiceDescriptor::encode_list_service_response()`
/// takes a `std::span<char>` scratch buffer (present in some 2026.9 betas, absent on stable and
/// current dev). Detected from the base class itself so no version gate can drift.
template<typename Base>
concept EncodeTakesScratch = requires(Base &base) { base.encode_list_service_response(std::span<char>{}); };

/// @brief Bridges the version-dependent `encode_list_service_response` signature.
///
/// Only the specialization matching the installed base is ever instantiated (the `Base`
/// parameter keeps the `override` checks dependent), and both forms forward to
/// `build_list_response()`, which the concrete descriptor implements.
template<bool TakesScratch, typename Base = api::UserServiceDescriptor> class EncodeListAdapter : public Base {
 public:
  api::ListEntitiesServicesResponse encode_list_service_response() override { return this->build_list_response(); }

 protected:
  virtual api::ListEntitiesServicesResponse build_list_response() = 0;
};

template<typename Base> class EncodeListAdapter<true, Base> : public Base {
 public:
  api::ListEntitiesServicesResponse encode_list_service_response(std::span<char> /*scratch*/) override {
    return this->build_list_response();
  }

 protected:
  virtual api::ListEntitiesServicesResponse build_list_response() = 0;
};

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
class ManagementServiceDescriptor : public EncodeListAdapter<EncodeTakesScratch<api::UserServiceDescriptor>> {
 public:
  ManagementServiceDescriptor(const char *name, std::vector<const char *> arg_names,
                              std::function<void(const api::ExecuteServiceRequest &)> callback)
      : name_(name), key_(fnv1_hash(name)), arg_names_(std::move(arg_names)), callback_(std::move(callback)) {}

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
  /// Wrap a literal in whatever string type the message field uses: StringRef on older
  /// ESPHome, ProgmemStringRef (explicit from StringRef, not assignable) on newer.
  template<typename T> static T make_ref(const char *s) {
    if constexpr (std::is_assignable_v<T &, StringRef>) {
      return StringRef(s);
    } else {
      return T(StringRef(s));
    }
  }

  // name_ and arg_names_ point at ordinary (non-PROGMEM) .rodata string literals, so the
  // refs stay valid after return on any target and any scratch buffer is unused.
  api::ListEntitiesServicesResponse build_list_response() override {
    api::ListEntitiesServicesResponse response;
    response.name = make_ref<decltype(response.name)>(this->name_);
    response.key = this->key_;
    response.supports_response = api::enums::SUPPORTS_RESPONSE_NONE;
    response.args.init(this->arg_names_.size());
    for (const char *arg_name : this->arg_names_) {
      auto &arg = response.args.emplace_back();
      arg.name = make_ref<decltype(arg.name)>(arg_name);
      arg.type = api::enums::SERVICE_ARG_TYPE_STRING;
    }
    return response;
  }

  const char *name_;
  uint32_t key_;
  std::vector<const char *> arg_names_;
  std::function<void(const api::ExecuteServiceRequest &)> callback_;
};

#endif

}  // namespace detail

// --- Helper free functions (file-local) ---

static std::string bool_to_string(bool value) { return value ? "true" : "false"; }

/// @brief Lowercase + ASCII-trim a native-API string argument.
static std::string normalize_lower_argument(const std::string &value) {
  std::string normalized = trim_ascii_whitespace(value);
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return normalized;
}

/// @brief A `value` token that maps to a fixed float (used by set_mode / set_presence /
/// set_open_window_detection).
struct HeatingNamedValue {
  const char *token;
  float value;
};

/// @brief Match `token` against `table`; on a hit set `out` and return true.
static bool match_heating_named_value(const std::string &token, const HeatingNamedValue *table, size_t table_len,
                                      float &out) {
  for (size_t i = 0; i < table_len; i++) {
    if (token == table[i].token) {
      out = table[i].value;
      return true;
    }
  }
  return false;
}

/// @brief Parse `set_temperature`'s value into degrees Celsius, range-checked.
static bool parse_heating_temperature(const std::string &value, float &value_out, std::string &error) {
  const std::string text = trim_ascii_whitespace(value);
  errno = 0;
  // NOLINTNEXTLINE(misc-const-correctness): strtoul()/strtof() take a `char **` end pointer.
  char *end = nullptr;
  const float parsed = std::strtof(text.c_str(), &end);
  if (text.empty() || end != text.c_str() + text.size() || errno == ERANGE || !std::isfinite(parsed)) {
    error = "temperature must be a number in degrees Celsius";
    return false;
  }
  if (parsed < HEATING_TEMP_MIN_C || parsed > HEATING_TEMP_MAX_C) {
    std::array<char, HEATING_RANGE_MESSAGE_BUFFER_SIZE> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "temperature must be between %.1f and %.1f",
                  static_cast<double>(HEATING_TEMP_MIN_C), static_cast<double>(HEATING_TEMP_MAX_C));
    error = buffer.data();
    return false;
  }
  value_out = parsed;
  return true;
}

/// @brief Parse the (`function`, `value`) argument pair of the `heating_control` action.
///
/// Every native-API argument is a string (ManagementServiceDescriptor hardcodes
/// SERVICE_ARG_TYPE_STRING), so this turns the two strings into a HeatingFunction plus the float
/// encode_heating_payload() expects: degrees C for `set_temperature`, a HeatingMode value for
/// `set_mode`, 0/1 for `set_presence` / `set_open_window_detection`, and an ignored 0 for the
/// register reads (`power_on`, `midnight_sync`, `window_open`). On any malformed input it fills `error` with a
/// caller-facing message and returns false rather than coercing a value. Kept next to the action (not in proto_heating)
/// because the climate entity receives typed enums from Home Assistant and needs no string
/// parsing.
/// @param function Function name argument.
/// @param value Value argument.
/// @param fn_out Parsed function on success.
/// @param value_out Parsed value on success (0 for the value-less functions).
/// @param error Caller-facing message on failure.
/// @return true on a fully valid pair.
static bool parse_heating_arguments(const std::string &function, const std::string &value, HeatingFunction &fn_out,
                                    float &value_out, std::string &error) {
  static constexpr HeatingNamedValue MODE_VALUES[] = {
      {"auto", static_cast<float>(HeatingMode::AUTO)},
      {"manual", static_cast<float>(HeatingMode::MANUAL)},
      {"prog", static_cast<float>(HeatingMode::PROG)},
      {"off", static_cast<float>(HeatingMode::OFF)},
  };
  static constexpr HeatingNamedValue PRESENCE_VALUES[] = {{"on", 1.0F}, {"off", 0.0F}};
  static constexpr HeatingNamedValue DETECTION_VALUES[] = {{"on", 1.0F}, {"off", 0.0F}};

  const std::string fn = normalize_lower_argument(function);
  value_out = 0.0F;

  if (fn == "power_on") {
    fn_out = HeatingFunction::POWER_ON;
    return true;
  }
  if (fn == "midnight_sync") {
    fn_out = HeatingFunction::MIDNIGHT_SYNC;
    return true;
  }
  if (fn == "set_temperature") {
    fn_out = HeatingFunction::SET_TEMPERATURE;
    return parse_heating_temperature(value, value_out, error);
  }
  if (fn == "set_mode") {
    fn_out = HeatingFunction::SET_MODE;
    if (match_heating_named_value(normalize_lower_argument(value), MODE_VALUES, std::size(MODE_VALUES), value_out))
      return true;
    error = "mode must be one of auto, manual, prog, off";
    return false;
  }
  if (fn == "set_presence") {
    fn_out = HeatingFunction::SET_PRESENCE;
    if (match_heating_named_value(normalize_lower_argument(value), PRESENCE_VALUES, std::size(PRESENCE_VALUES),
                                  value_out))
      return true;
    error = "presence must be 'on' or 'off'";
    return false;
  }
  if (fn == "set_open_window_detection") {
    fn_out = HeatingFunction::SET_OPEN_WINDOW_DETECTION;
    if (match_heating_named_value(normalize_lower_argument(value), DETECTION_VALUES, std::size(DETECTION_VALUES),
                                  value_out))
      return true;
    error = "open-window detection must be 'on' or 'off'";
    return false;
  }
  if (fn == "window_open") {
    fn_out = HeatingFunction::WINDOW_OPEN;
    return true;
  }

  error = "unknown heating function '" + trim_ascii_whitespace(function) + "'";
  return false;
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
      hub_(hub),
      probes_(node_id, engine, registry, initialized,
              DiagnosticProbesHub{[hub]() { return hub->diagnostic_probes_enabled(); },
                                  [hub]() { return hub->get_radio(); },
                                  [hub](const IoFrame &reply) { return hub->record_probe_reply_event(reply); }}) {}

void ManagementActions::register_actions() {
#if defined(USE_API_USER_DEFINED_ACTIONS) && defined(USE_API_CUSTOM_SERVICES)
  if (api::global_api_server == nullptr) {
    ESP_LOGW(detail::TAG, "Native API server not available, management actions will not be registered");
    return;
  }

  // One row per user-visible action: name, argument-name list (every argument is exposed as
  // SERVICE_ARG_TYPE_STRING — `oneway_set_position` parses its "position" string itself rather
  // than widening a shipped API surface), whether it is gated behind `diagnostic_probes: true`,
  // and the callback that unpacks its arguments. Adding an action is one row here.
  struct ActionReg {
    const char *name;
    std::vector<const char *> arg_names;
    bool diagnostic_only;
    std::function<void(const api::ExecuteServiceRequest &)> callback;
  };
  const ActionReg actions[] = {
      {MANAGEMENT_ACTION_RENAME_DEVICE,
       {"device_id", "new_name"},
       false,
       [this](const api::ExecuteServiceRequest &r) {
         this->api_rename_device(r.args[0].string_.str(), r.args[1].string_.str());
       }},
      {MANAGEMENT_ACTION_IDENTIFY_DEVICE,
       {"device_id"},
       false,
       [this](const api::ExecuteServiceRequest &r) { this->api_identify_device(r.args[0].string_.str()); }},
      {MANAGEMENT_ACTION_FORCE_OPEN_DEVICE,
       {"device_id"},
       false,
       [this](const api::ExecuteServiceRequest &r) { this->api_force_open_device(r.args[0].string_.str()); }},
      {MANAGEMENT_ACTION_SCAN_PAIRED_DEVICES,
       {},
       false,
       [this](const api::ExecuteServiceRequest &) { this->api_scan_paired_devices(); }},
      {MANAGEMENT_ACTION_ONEWAY_SET_POSITION,
       {"controller_id", "position"},
       false,
       [this](const api::ExecuteServiceRequest &r) {
         this->api_oneway_set_position(r.args[0].string_.str(), r.args[1].string_.str());
       }},
      {MANAGEMENT_ACTION_ONEWAY_REMOVE_CONTROLLER,
       {"controller_id"},
       false,
       [this](const api::ExecuteServiceRequest &r) { this->api_oneway_remove_controller(r.args[0].string_.str()); }},
      {MANAGEMENT_ACTION_HEATING_CONTROL,
       {"device_id", "function", "value"},
       false,  // a real user feature, gated by documentation not by diagnostic_probes:
       [this](const api::ExecuteServiceRequest &r) {
         this->api_heating_control(r.args[0].string_.str(), r.args[1].string_.str(), r.args[2].string_.str());
       }},
      {MANAGEMENT_ACTION_PROBE_DEVICE,
       {"device_id", "probe", "index"},
       true,
       [this](const api::ExecuteServiceRequest &r) {
         this->api_probe_device(r.args[0].string_.str(), r.args[1].string_.str(), r.args[2].string_.str());
       }},
      {MANAGEMENT_ACTION_PROBE_SWEEP,
       {"device_id", "probe", "first_index", "last_index"},
       true,
       [this](const api::ExecuteServiceRequest &r) {
         this->api_probe_sweep(r.args[0].string_.str(), r.args[1].string_.str(), r.args[2].string_.str(),
                               r.args[3].string_.str());
       }},
  };

  // The two probe actions are registered only when diagnostic_probes: true was set in YAML, so the
  // action list stays clean on a default build. diagnostic_probes_enabled() already holds its
  // final YAML-configured value here: __init__.py's to_code() emits set_diagnostic_probes_enabled()
  // as a plain property-setter call in generated main.cpp, which runs before App.setup() calls this
  // component's setup() (and therefore this method), not after.
  const bool probes_enabled = hub_->diagnostic_probes_enabled();
  for (const auto &action : actions) {
    if (action.diagnostic_only && !probes_enabled)
      continue;
    api::global_api_server->register_user_service(  // NOLINT
        new detail::ManagementServiceDescriptor(action.name, action.arg_names, action.callback));
  }
#endif
}

IoDevice *ManagementActions::resolve_device_(const char *action, const std::string &device_id,
                                             ManagementActionResult &result) {
  return resolve_management_device(action, device_id, *initialized_, registry_, result);
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
  detail::log_multiline_result(detail::TAG, !result.success, prefix,
                               result.log_message.empty() ? result.message : result.log_message);

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
  if (!create_set_name(request, node_id_, dev->node_id, dev->low_power, payload)) {
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
  if (!create_identify(request, node_id_, dev->node_id, dev->low_power)) {
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

void ManagementActions::api_heating_control(const std::string &device_id, const std::string &function,
                                            const std::string &value) {
  publish_result(heating_control(device_id, function, value));
}

ManagementActionResult ManagementActions::heating_control(const std::string &device_id, const std::string &function,
                                                          const std::string &value) {
  ManagementActionResult result;
  auto *dev = resolve_device_(MANAGEMENT_ACTION_HEATING_CONTROL, device_id, result);
  if (dev == nullptr)
    return result;

  HeatingFunction fn{};
  float encoded_value = 0.0F;
  std::string parse_error;
  if (!parse_heating_arguments(function, value, fn, encoded_value, parse_error)) {
    result.message = parse_error;
    return result;
  }

  // Capability gate via the predicate only — no inline device-type or vendor list.
  if (!device_supports_climate_control(dev->type, dev->subtype)) {
    result.message = "device is not a climate device";
    return result;
  }
  if (!device_uses_heating_registers(dev->type, dev->subtype)) {
    result.message = "device is commanded with heating levels, not private registers";
    return result;
  }

  // Snapshot any pre-existing CMD_ERROR_RESP code so a stale one (e.g. an old wind lockout from a
  // different command) is not misattributed to this send.
  const uint8_t prior_result_code = dev->last_result_code;
  const uint32_t prior_result_at_ms = dev->last_result_at_ms;

  // A register with a decoded value (the window sensor) is read through the entity's read path, so
  // the action shows exactly what the climate card would; every other function is one send. Both
  // paths share the hub's exchange helper, and a CMD_ERROR_RESP is recorded on the device record by it.
  float read_value = 0.0F;
  const bool read_with_value = fn == HeatingFunction::WINDOW_OPEN;
  const bool acknowledged = read_with_value
                                ? hub_->read_heating_register(result.device_id, fn, &read_value, EXCHANGE_RETRY_COUNT)
                                : hub_->send_heating_command(result.device_id, fn, encoded_value);
  if (!acknowledged) {
    result.message = std::string("device did not acknowledge the ") + heating_function_name(fn) + " command";
    if (const auto *updated = registry_.get(result.device_id);
        updated != nullptr && updated->last_result_code != 0 &&
        (updated->last_result_code != prior_result_code || updated->last_result_at_ms != prior_result_at_ms)) {
      result.has_result_code = true;
      result.result_code = updated->last_result_code;
      result.message =
          std::string(command_result_name(result.result_code)) + ": " + command_result_description(result.result_code);
    }
    return result;
  }

  result.success = true;
  // verified stays false: an acknowledged write is the device saying it took the frame, not the
  // device showing the new value; the climate entity reads the register back for that.
  if (read_with_value) {
    result.message =
        std::string("heating ") + heating_function_name(fn) + ": window " + (read_value != 0.0F ? "open" : "closed");
  } else {
    result.message = std::string("heating ") + heating_function_name(fn) + " acknowledged by device";
  }
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

  // Parse the position string here and reject loudly on anything unparseable. Do NOT relax this to
  // atoi()/strtoul()-with-default: a value that silently became 0 would send a fully-open command
  // to every actuator bound to this 1W identity.
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

/// @brief The keys one scan sweeps with: slot 0 is the hub key, the rest are the distinct per-device keys.
using ScanKeys = std::vector<std::array<uint8_t, AES_KEY_SIZE>>;

/// @brief Collect the keys to sweep: the hub key, then each per-device key that differs from it and
/// from every key already collected. A per-device key equal to the hub key changes nothing, so it
/// never earns a sweep of its own.
static ScanKeys collect_scan_keys(const uint8_t *hub_key, DeviceRegistry &registry) {
  ScanKeys keys(1);
  memcpy(keys[0].data(), hub_key, AES_KEY_SIZE);
  for (auto &entry : registry) {
    const IoDevice &dev = entry.second;
    if (!dev.has_system_key)
      continue;
    const bool known = std::ranges::any_of(
        keys, [&dev](const auto &key) { return memcmp(key.data(), dev.system_key, AES_KEY_SIZE) == 0; });
    if (known)
      continue;
    keys.emplace_back();
    memcpy(keys.back().data(), dev.system_key, AES_KEY_SIZE);
  }
  return keys;
}

/// @brief How many registered devices exchange with `key`: those with it as their own key, or, for the
/// hub key, those with no key of their own (or one equal to the hub's).
static size_t count_devices_using_key(DeviceRegistry &registry, const uint8_t *hub_key, const uint8_t *key) {
  const bool is_hub_key = memcmp(key, hub_key, AES_KEY_SIZE) == 0;
  size_t count = 0;
  for (auto &entry : registry) {
    const IoDevice &dev = entry.second;
    const uint8_t *device_key = dev.has_system_key ? dev.system_key : hub_key;
    if (memcmp(device_key, key, AES_KEY_SIZE) == 0 && (is_hub_key || dev.has_system_key))
      count++;
  }
  return count;
}

/// @brief Build the final report: known/unknown grouping, header, the selection NOTE, and the
/// truncation NOTE.
static std::string build_scan_report(const ScanState &state, DeviceRegistry &registry, ScanPowerClasses selection,
                                     const ScanKeys &keys, bool reveal_keys) {
  // Grouped into known-first, unknown-second rather than interleaved in arrival order: the two
  // groups need very different follow-up (nothing to do vs. paste a YAML block), so burying an
  // unknown responder between two known ones makes it easy to miss.
  std::string known_body;
  std::string unknown_body;
  uint8_t unknown_count = 0;
  for (uint8_t i = 0; i < state.count; i++) {
    const std::string device_id = node_id_to_string(state.responders[i].src);
    // A device a pairing added in this boot still needs its block, so it counts as unknown here.
    const IoDevice *registered = registry.get_declared(device_id);
    const uint8_t slot = state.responders[i].key_slot;
    const RollCallReplyKeys reply_keys{keys[slot].data(), keys[0].data(), slot != 0};
    const RollCallKeyDisplay display = reveal_keys ? RollCallKeyDisplay::BANNERED : RollCallKeyDisplay::HIDDEN;
    if (registered != nullptr) {
      known_body += format_roll_call_responder(state.responders[i], device_id, registered, reply_keys, display);
    } else {
      unknown_count++;
      unknown_body += format_roll_call_responder(state.responders[i], device_id, nullptr, reply_keys, display);
    }
  }

  std::string body;
  if (!known_body.empty())
    body += "Known:\n" + known_body;
  if (!unknown_body.empty())
    body += "Unknown:\n" + unknown_body;

  std::string message = "Roll-call: " + std::to_string(state.count) + " device" + (state.count == 1 ? "" : "s") +
                        " detected (" + std::to_string(state.count - unknown_count) + " known, " +
                        std::to_string(unknown_count) + " unknown)\n";
  // A non-default selection must say so in the report itself, not just the log: a user who left
  // this tunable on always_alive must not read a missing solar/battery device as "gone". Derived
  // from ROLL_CALL_PASSES rather than a switch/literal, so a third power class would not need a
  // second place taught about it.
  if (selection != ScanPowerClasses::BOTH) {
    for (const RollCallPass &pass : ROLL_CALL_PASSES) {
      if (scan_power_classes_include(selection, pass.power_save_mode))
        continue;
      message += "NOTE: scan_power_classes=" + scan_power_classes_to_string(selection) + ": " +
                 power_save_mode_name(pass.power_save_mode) + " devices were not called.\n";
    }
  }
  // Surfaced in the report itself, not only the log: a scan that silently listed a subset would
  // look like devices had gone missing.
  if (state.truncated) {
    message += "NOTE: more than " + std::to_string(SCAN_MAX_REPLIES) +
               " devices answered; the list below is truncated. Re-run to see whether other devices "
               "appear, and raise SCAN_MAX_REPLIES if this install really is larger.\n";
  }
  message += body;
  return message;
}

ManagementActionResult ManagementActions::scan_paired_devices() {
  ManagementActionResult result = make_management_result(MANAGEMENT_ACTION_SCAN_PAIRED_DEVICES, "");

  if (!*initialized_) {
    result.message = "hub is not initialized";
    return result;
  }

  // One full sweep (run_roll_call_sweep(): every enabled pass on every channel) per distinct key:
  // the hub key first, then each different per-device key. A device answers only a roll-call built
  // with the key it holds, so a device that keeps its own key is invisible to the hub-key sweep.
  // Attempt numbers are global and 1-based across the sweeps actually run, matching the "n/N" log
  // line and the per-reply `attempt=` field.
  const ScanPowerClasses selection = tuning_->scan_power_classes;
  const ScanKeys keys = collect_scan_keys(system_key_, registry_);
  const uint8_t total_attempts = count_enabled_passes(selection) * SCAN_CHANNEL_COUNT * keys.size();
  ScanState state{};
  uint8_t attempt = 0;
  for (size_t slot = 0; slot < keys.size(); slot++) {
    const size_t using_key = count_devices_using_key(registry_, system_key_, keys[slot].data());
    ESP_LOGI(detail::TAG, "Roll-call with key check value %s (%u registered device%s)",
             detail::format_key_check_value(keys[slot].data()).c_str(), static_cast<unsigned>(using_key),
             using_key == 1 ? "" : "s");
    if (run_roll_call_sweep(engine_, node_id_, keys[slot].data(), static_cast<uint8_t>(slot), *tuning_, state, attempt,
                            total_attempts) == RollCallAttemptResult::BUILD_FAILED) {
      result.message = "failed to build roll-call request";
      return result;
    }
  }

  result.success = true;
  // Two renderings of one report. The event message goes to Home Assistant, whose recorder stores it, so
  // it must not carry a device key; the device log gets the version with the key, bannered.
  result.message = build_scan_report(state, registry_, selection, keys, /*reveal_keys=*/false);
  if (keys.size() > 1)  // with the hub key alone there is no key to reveal, so one report serves both
    result.log_message = build_scan_report(state, registry_, selection, keys, /*reveal_keys=*/true);
  return result;
}

void ManagementActions::api_probe_device(const std::string &device_id, const std::string &probe,
                                         const std::string &index) {
  publish_result(probe_device(device_id, probe, index));
}

ManagementActionResult ManagementActions::probe_device(const std::string &device_id, const std::string &probe,
                                                       const std::string &index) {
  return probes_.probe_device(device_id, probe, index);
}

void ManagementActions::api_probe_sweep(const std::string &device_id, const std::string &probe,
                                        const std::string &first_index, const std::string &last_index) {
  publish_result(probe_sweep(device_id, probe, first_index, last_index));
}

ManagementActionResult ManagementActions::probe_sweep(const std::string &device_id, const std::string &probe,
                                                      const std::string &first_index, const std::string &last_index) {
  return probes_.probe_sweep(device_id, probe, first_index, last_index);
}

}  // namespace home_io_control
}  // namespace esphome

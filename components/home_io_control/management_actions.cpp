/// @file management_actions.cpp
/// @brief Hub-level management actions such as device rename.
/// @ingroup hioc_hub
///
/// This file owns advanced management operations that are not part of the normal
/// entity surface. Operations are exposed as ESPHome native API actions so Home
/// Assistant can trigger them without adding always-visible helper entities.

#include "management_actions.h"

#include "hub_internal.h"  // brings in hub_core.h + all internal helpers + logging
#include "proto_commands.h"

#if defined(USE_API_USER_DEFINED_ACTIONS) && defined(USE_API_CUSTOM_SERVICES)
#include "esphome/core/helpers.h"
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <map>

namespace esphome {
namespace home_io_control {

namespace {

constexpr const char *MANAGEMENT_ACTION_RENAME_DEVICE = "rename_device";
constexpr const char *MANAGEMENT_RESULT_EVENT = "esphome.home_io_control_action_result";
constexpr size_t RESULT_CODE_BUFFER_SIZE = 5;
constexpr size_t UNEXPECTED_RESPONSE_MESSAGE_BUFFER_SIZE = 64;

}  // namespace

namespace detail {

#if defined(USE_API_USER_DEFINED_ACTIONS) && defined(USE_API_CUSTOM_SERVICES)
/// @brief Native API descriptor for the rename action.
///
/// ESPHome 2026.x does not expose the generated YAML action helper runtime to external
/// components, so Home IO Control registers the action descriptor directly with APIServer.
/// This keeps the HA action surface identical to native ESPHome actions while avoiding
/// the unresolved link path behind CustomAPIDevice::register_service().
///
/// The descriptor now holds ManagementActions* (not IOHomeControlComponent*) and calls
/// only public methods, removing the need for a friend declaration on the hub.
class RenameDeviceServiceDescriptor : public api::UserServiceDescriptor {
 public:
  explicit RenameDeviceServiceDescriptor(ManagementActions *actions)
      : actions_(actions), key_(fnv1_hash(MANAGEMENT_ACTION_RENAME_DEVICE)) {}

  api::ListEntitiesServicesResponse encode_list_service_response() override {
    api::ListEntitiesServicesResponse response;
    response.name = StringRef(MANAGEMENT_ACTION_RENAME_DEVICE);
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
    this->actions_->api_rename_device(request.args[0].string_.str(), request.args[1].string_.str());
    return true;
  }

#ifdef USE_API_USER_DEFINED_ACTION_RESPONSES
  bool execute_service(const api::ExecuteServiceRequest &request, uint32_t) override {
    return this->execute_service(request);
  }
#endif

 protected:
  ManagementActions *actions_;
  uint32_t key_;
  const std::array<const char *, 2> arg_names_{"device_id", "new_name"};
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

static std::string format_result_code(uint8_t result_code) {
  std::array<char, RESULT_CODE_BUFFER_SIZE> buffer{};
  std::snprintf(buffer.data(), buffer.size(), "%02X", result_code);
  return std::string(buffer.data());
}

static ManagementActionResult make_management_result(const std::string &action, const std::string &device_id) {
  ManagementActionResult result;
  result.action = action;
  result.device_id = device_id;
  return result;
}

// --- ManagementActions ---

ManagementActions::ManagementActions(const uint8_t *node_id, ExchangeEngine &engine, DeviceRegistry &registry,
                                     const bool *initialized, IOHomeControlComponent *hub)
    : node_id_(node_id), engine_(engine), registry_(registry), initialized_(initialized), hub_(hub) {}

void ManagementActions::register_actions() {
#if defined(USE_API_USER_DEFINED_ACTIONS) && defined(USE_API_CUSTOM_SERVICES)
  if (api::global_api_server == nullptr) {
    ESP_LOGW(detail::TAG, "Native API server not available, rename action will not be registered");
    return;
  }
  api::global_api_server->register_user_service(new detail::RenameDeviceServiceDescriptor(this));  // NOLINT
#endif
}

void ManagementActions::api_rename_device(const std::string &device_id, const std::string &new_name) {
  publish_result(rename_device(device_id, new_name));
}

void ManagementActions::publish_result(const ManagementActionResult &result) {
  if (result.success) {
    ESP_LOGI(detail::TAG, "Management action %s for device %s: %s", result.action.c_str(), result.device_id.c_str(),
             result.message.c_str());
  } else {
    ESP_LOGW(detail::TAG, "Management action %s for device %s failed: %s", result.action.c_str(),
             result.device_id.c_str(), result.message.c_str());
  }

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
    event_data["result_code"] = format_result_code(result.result_code);
    event_data["result_code_name"] = command_result_name(result.result_code);
  }

  hub_->fire_homeassistant_event(MANAGEMENT_RESULT_EVENT, event_data);
}

ManagementActionResult ManagementActions::rename_device(const std::string &device_id, const std::string &new_name) {
  const std::string normalized_device_id = normalize_device_id_argument(device_id);
  ManagementActionResult result = make_management_result(MANAGEMENT_ACTION_RENAME_DEVICE, normalized_device_id);

  if (!*initialized_) {
    result.message = "hub is not initialized";
    return result;
  }

  uint8_t parsed_device_id[NODE_ID_SIZE]{};
  if (!hex_to_bytes(normalized_device_id, parsed_device_id, NODE_ID_SIZE)) {
    result.message = "device ID must be exactly 6 hexadecimal characters";
    return result;
  }

  auto *dev = registry_.get(normalized_device_id);
  if (dev == nullptr) {
    result.message = "device is not registered on this hub";
    return result;
  }

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
  if (!engine_.send_and_receive(request, response, FREQ_CH2)) {
    engine_.log_debug(normalized_device_id.c_str());
    result.message = "no valid response to rename request";
    return result;
  }

  if (response.cmd == CMD_ERROR_RESP) {
    if (response.data_len == 0) {
      result.message = "device returned an empty error response";
      return result;
    }
    result.has_result_code = true;
    result.result_code = response.data[0];
    result.message =
        std::string(command_result_name(result.result_code)) + ": " + command_result_description(result.result_code);
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

  if (!hub_->request_device_name(normalized_device_id)) {
    result.message = "rename acknowledged but verification readback failed";
    return result;
  }

  auto *updated_device = registry_.get(normalized_device_id);
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

}  // namespace home_io_control
}  // namespace esphome

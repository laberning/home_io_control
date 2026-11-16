/// @file heater_controller.cpp
/// @brief Heating and hot water: the register heater's writes and reads, the water heater's reads and writes.
/// @ingroup hioc_hub
///
/// See heater_controller.h. The decoding of a heater's status answer is in heater_controller_status.cpp.

#include "heater_controller.h"

#include "entity_helpers.h"
#include "decisions_attempts.h"
#include "decisions_status.h"
#include "log_frame.h"
#include "proto_commands.h"
#include "proto_constants.h"
#include "proto_device_kinds.h"
#include "proto_device_types.h"
#include "proto_dhw.h"
#include "proto_heating.h"
#include "proto_heating_profiles.h"
#include "proto_private_registers.h"
#include "proto_timing.h"
#include "status_poll_policy.h"

#include "esphome/core/log.h"

#include <cmath>
#include <iterator>
#include <optional>

namespace esphome {
namespace home_io_control {

namespace {

/// The log label of a water heater register the poll reads.
const char *dhw_register_label(uint8_t index) {
  switch (index) {
    case DHW_REG_WATER_TEMPERATURE:
      return "dhw_water_temperature";
    case DHW_REG_MODE:
      return "dhw_mode";
    case DHW_REG_HEATING:
      return "dhw_heating";
    case DHW_REG_BOOST_DURATION:
      return "dhw_boost_duration";
    case DHW_REG_AWAY_DURATION:
      return "dhw_away_duration";
    case DHW_REG_ENERGY:
      return "dhw_energy";
    case DHW_REG_BRAND:
      return "dhw_brand";
    case DHW_REG_STATE:
      return "dhw_state";
    case DHW_REG_ACTIVE_TARGET:
      return "dhw_active_target";
    case DHW_REG_TARGET_SHOWERS:
      return "dhw_target_showers";
    case DHW_REG_SHOWERS_AVAILABLE:
      return "dhw_showers_available";
    case DHW_REG_HEATING_POWER:
      return "dhw_heating_power";
    case DHW_REG_BOOST_FLAG:
      return "dhw_boost_flag";
    case DHW_REG_AWAY_FLAG:
      return "dhw_away_flag";
    default:
      return "dhw_register";
  }
}

/// Fold a boost or away duration register into the readings (kept and logged; the state is FP16, ADR 0081).
bool store_dhw_duration(const std::string &device_id, DhwReadings &readings, uint8_t index, const uint8_t *value,
                        size_t len) {
  uint8_t duration = 0;
  if (!decode_dhw_duration(value, len, duration))
    return false;
  const bool boost = index == DHW_REG_BOOST_DURATION;
  (boost ? readings.boost_duration : readings.away_duration) = duration;
  ESP_LOGD(detail::TAG, "Device %s: hot water %s duration %u", device_id.c_str(), boost ? "boost" : "away",
           static_cast<unsigned>(duration));
  return true;
}

/// Fold the brand letter into the readings, logging it the first time.
bool store_dhw_brand(const std::string &device_id, DhwReadings &readings, const uint8_t *value, size_t len) {
  if (len < 1)
    return false;
  if (readings.brand < 0) {
    // One letter per brand; logged once so a field report says which make answered.
    const char letter = (value[0] >= ' ' && value[0] <= '~') ? static_cast<char>(value[0]) : '?';
    ESP_LOGI(detail::TAG, "Device %s reports hot water brand '%c' (0x%02X)", device_id.c_str(), letter, value[0]);
  }
  readings.brand = value[0];
  return true;
}

/// Fold the state register of an electric storage water heater into the readings: the state itself, and the
/// boost and away it implies (this model has no operating-mode word to report them).
bool store_dhw_state(const std::string &device_id, DhwReadings &readings, const uint8_t *value, size_t len) {
  DhwState state = DhwState::AUTO;
  if (!decode_dhw_state(value, len, state))
    return false;
  const auto raw = static_cast<int16_t>(state);
  if (readings.state != raw) {
    ESP_LOGI(detail::TAG, "Device %s: hot water state %d (boost %s, away %s)", device_id.c_str(), raw,
             state == DhwState::BOOST ? "on" : "off", state == DhwState::AWAY ? "on" : "off");
  }
  readings.state = raw;
  readings.boost = state == DhwState::BOOST ? 1 : 0;
  readings.away = state == DhwState::AWAY ? 1 : 0;
  return true;
}

/// Fold a shower count of an electric storage water heater into the readings.
bool store_dhw_showers(const std::string &device_id, DhwReadings &readings, uint8_t index, const uint8_t *value,
                       size_t len) {
  uint8_t showers = 0;
  if (!decode_dhw_showers(value, len, showers))
    return false;
  const bool available = index == DHW_REG_SHOWERS_AVAILABLE;
  int16_t &reading = available ? readings.showers_available : readings.showers_target;
  if (reading != showers) {
    ESP_LOGI(detail::TAG, "Device %s: hot water showers %s %u", device_id.c_str(),
             available ? "available" : "aimed for", static_cast<unsigned>(showers));
  }
  reading = showers;
  return true;
}

/// Fold the heating power of an electric storage water heater into the readings. The element is what heats
/// on this model, so the power also says whether it runs; there is no heat pump to report.
bool store_dhw_heating_power(const std::string &device_id, DhwReadings &readings, const uint8_t *value, size_t len) {
  uint16_t watts = 0;
  if (!decode_dhw_heating_power(value, len, watts))
    return false;
  if (readings.power_w != watts)
    ESP_LOGI(detail::TAG, "Device %s: hot water heating power %u W", device_id.c_str(), static_cast<unsigned>(watts));
  readings.power_w = watts;
  readings.heating = watts != 0 ? 1 : 0;
  readings.element = readings.heating;
  return true;
}

/// Fold one decoded water heater register into the readings.
/// @return false when the value is not one the decoder takes (the reading is left as it was).
bool store_dhw_reading(const std::string &device_id, DhwReadings &readings, uint8_t index, const uint8_t *value,
                       size_t len) {
  switch (index) {
    case DHW_REG_WATER_TEMPERATURE:
      return decode_dhw_water_temperature(value, len, readings.water_c);
    case DHW_REG_MODE: {
      DhwMode mode = DhwMode::AUTO;
      if (!decode_dhw_mode(value, len, mode))
        return false;
      readings.mode = static_cast<int16_t>(mode);
      return true;
    }
    case DHW_REG_HEATING: {
      DhwHeatingState state;
      if (!decode_dhw_heating_state(value, len, state))
        return false;
      readings.heating = state.heating ? 1 : 0;
      readings.element = state.element ? 1 : 0;
      readings.heat_pump = state.heat_pump ? 1 : 0;
      return true;
    }
    case DHW_REG_BOOST_DURATION:
    case DHW_REG_AWAY_DURATION:
      return store_dhw_duration(device_id, readings, index, value, len);
    case DHW_REG_ENERGY: {
      uint32_t wh = 0;
      if (!decode_dhw_energy_wh(value, len, wh))
        return false;
      readings.energy_wh = wh;
      return true;
    }
    case DHW_REG_BRAND:
      return store_dhw_brand(device_id, readings, value, len);
    case DHW_REG_STATE:
      return store_dhw_state(device_id, readings, value, len);
    case DHW_REG_ACTIVE_TARGET: {
      float celsius = NAN;
      if (!decode_dhw_active_target(value, len, celsius))
        return false;
      if (readings.target_c != celsius)
        ESP_LOGI(detail::TAG, "Device %s: hot water target %.1f C", device_id.c_str(), static_cast<double>(celsius));
      readings.target_c = celsius;
      return true;
    }
    case DHW_REG_TARGET_SHOWERS:
    case DHW_REG_SHOWERS_AVAILABLE:
      return store_dhw_showers(device_id, readings, index, value, len);
    case DHW_REG_HEATING_POWER:
      return store_dhw_heating_power(device_id, readings, value, len);
    case DHW_REG_BOOST_FLAG:
    case DHW_REG_AWAY_FLAG: {
      uint8_t flag = 0;
      if (!decode_dhw_duration(value, len, flag))
        return false;
      (index == DHW_REG_BOOST_FLAG ? readings.boost_flag : readings.away_flag) = flag;
      ESP_LOGI(detail::TAG, "Device %s: hot water %s flag reads %u", device_id.c_str(),
               index == DHW_REG_BOOST_FLAG ? "boost" : "away", static_cast<unsigned>(flag));
      return true;
    }
    default:
      return false;
  }
}

/// Log names of the two features the operating-mode word switches; their wire facts are
/// dhw_operating_feature_wire()'s.
struct DhwOperatingFeatureNames {
  const char *feature;   ///< The feature and its EXECUTE.
  const char *duration;  ///< Its duration write.
};

/// One row per DhwOperatingFeature, in its order.
constexpr DhwOperatingFeatureNames DHW_OPERATING_FEATURE_NAMES[] = {
    {"hot water boost", "hot water boost duration"},
    {"hot water away", "hot water away duration"},
};
static_assert(std::size(DHW_OPERATING_FEATURE_NAMES) == static_cast<size_t>(DhwOperatingFeature::AWAY) + 1,
              "one row per DhwOperatingFeature");

}  // namespace

HeaterController::HeaterController(const uint8_t *node_id, const bool *initialized, RadioDriver *const *radio,
                                   DeviceRegistry &registry, StatusPollPolicy &poll_policy, ExchangeEngine &engine,
                                   HeaterControllerHub hub)
    : node_id_(node_id),
      initialized_(initialized),
      radio_(radio),
      registry_(registry),
      policy_(poll_policy),
      engine_(engine),
      hub_(std::move(hub)) {}

// === Register heater ===

bool HeaterController::set_heating_level(const std::string &device_id, uint8_t level) {
  // The same funnel as the other execute-family operations: gate, frame, exchange, and a settle poll
  // that reads the level back.
  return this->hub_.execute(
      device_id, "heating level", [](const IoDevice &d) { return device_uses_heating_levels(d.type, d.subtype); },
      "heating-level climate device",
      [this, level](IoFrame &request, const IoDevice &d) {
        return create_execute_heating_level(request, this->node_id_, d.node_id, d.low_power, level);
      },
      HeaterExecuteKind::WITH_ROLLBACK);
}

bool HeaterController::set_heating_temperature(const std::string &device_id, uint16_t main_parameter,
                                               const HeatingFp &fp) {
  return this->hub_.execute(
      device_id, "heating temperature",
      [](const IoDevice &d) { return device_uses_heating_temperature(d.type, d.subtype); },
      "temperature-controlled climate device",
      [this, main_parameter, fp](IoFrame &request, const IoDevice &d) {
        switch (fp.kind) {
          case HeatingFpKind::FP9:
            return create_execute_heating_with_mode_flags(request, this->node_id_, d.node_id, d.low_power,
                                                          main_parameter, fp.value);
          case HeatingFpKind::FP16:
            return create_execute_heating_with_fp16(request, this->node_id_, d.node_id, d.low_power, main_parameter,
                                                    fp.value);
          case HeatingFpKind::NONE:
            break;
        }
        return create_execute_heating_main_parameter(request, this->node_id_, d.node_id, d.low_power, main_parameter);
      },
      HeaterExecuteKind::WITH_ROLLBACK);
}

bool HeaterController::execute_heating_plan(const std::string &device_id, const HeatingWritePlan &plan,
                                            const std::function<void()> &on_accepted) {
  bool accepted = false;
  switch (plan.kind) {
    case HeatingPlanKind::REGISTER:
      accepted = this->send_heating_command(device_id, plan.register_fn, plan.register_value);
      break;
    case HeatingPlanKind::LEVEL:
      accepted = this->set_heating_level(device_id, plan.level);
      break;
    case HeatingPlanKind::TEMPERATURE:
      accepted = this->set_heating_temperature(device_id, plan.main_parameter, plan.fp);
      break;
    case HeatingPlanKind::NONE:
      break;
  }
  if (!accepted)
    return false;
  if (on_accepted)
    on_accepted();
  if (plan.read_back)
    this->read_back_heating_register_(device_id, plan.read_back_fn);
  return true;
}

void HeaterController::set_device_temperature_range(const std::string &device_id, float min_c, float max_c) {
  IoDevice *dev = this->registry_.get(device_id);
  if (dev == nullptr)
    return;
  dev->temperature_range_min_c = min_c;
  dev->temperature_range_max_c = max_c;
}

bool HeaterController::exchange_heating_(const std::string &device_id, const char *label, const IoFrame &request,
                                         IoFrame &response, uint8_t max_tries) {
  // A read and a write of the same register share the label, so the log names which one it was: a
  // read-back that fails after an acknowledged write must not read as a refused write.
  const bool is_read = request.data_len > 1 && request.data[1] == PRIVATE_REGISTER_DIRECTION_GET;
  const char *direction = is_read ? "read" : "write";
  ESP_LOGI(detail::TAG, "Sending heating %s %s to device %s", direction, label, device_id.c_str());

  // The shared bookkeeping (exchange counters, link health of a failed or unconfirmed ending, the
  // result code of an error reply, device event reports) runs inside the hub's exchange. The heater
  // poll keeps its own backoff ladder, so the exchange schedules none.
  const bool replied = this->hub_.exchange(device_id, request, response, max_tries);
  IoDevice *dev = this->registry_.get(device_id);
  if (replied && dev != nullptr) {
    // A plain reply is not stamped by the exchange (covers get it from the status decoder, which a
    // heater reply never reaches), so a heater that answers still counts as heard.
    detail::update_link_health(*dev, *this->radio_);
    if (response.cmd == CMD_WRITE_PRIVATE_ACK)
      detail::clear_command_result(*dev);
    this->registry_.notify(device_id);
  }
  const bool acknowledged = replied && response.cmd == CMD_WRITE_PRIVATE_ACK;

  if (!acknowledged) {
    this->engine_.log_debug(device_id.c_str());
    ESP_LOGW(detail::TAG, "Heating %s %s: no CMD_WRITE_PRIVATE_ACK from device %s", direction, label,
             device_id.c_str());
    return false;
  }

  // Logging the ack payload lets a field tester read back what the heater reports even for the
  // registers nothing decodes (`power_on`, `midnight_sync`).
  char ack_payload_hex[FRAME_LOG_HEX_BUFFER_SIZE];
  bytes_to_hex(response.data, response.data_len, ack_payload_hex, sizeof(ack_payload_hex));
  ESP_LOGD(detail::TAG, "heating %s %s ACK payload: %s", direction, label, ack_payload_hex);
  return true;
}

bool HeaterController::send_heating_command(const std::string &device_id, HeatingFunction fn, float value) {
  const IoDevice *dev = this->registry_.get(device_id);
  if (dev == nullptr || !*this->initialized_)
    return false;

  // Capability gate: only via the predicate, never an inline device-type or vendor list.
  if (!device_uses_heating_registers(dev->type, dev->subtype)) {
    detail::log_rejected_operation(device_id, *dev, "heating command", "heater with private registers");
    return false;
  }

  uint8_t payload[PRIVATE_REGISTER_PAYLOAD_MAX_SIZE];
  const size_t payload_len = encode_heating_payload(fn, value, payload);
  if (payload_len == 0) {
    ESP_LOGW(detail::TAG, "Heating command %s for device %s rejected: value %.2f out of range",
             heating_function_name(fn), device_id.c_str(), value);
    return false;
  }

  IoFrame request;
  if (!create_write_private(request, this->node_id_, dev->node_id, dev->low_power, payload, payload_len)) {
    ESP_LOGW(detail::TAG, "Heating command %s for device %s: failed to build frame", heating_function_name(fn),
             device_id.c_str());
    return false;
  }

  IoFrame response;
  // A write is something the user waits on: the full retry budget.
  if (!this->exchange_heating_(device_id, heating_function_name(fn), request, response, EXCHANGE_RETRY_COUNT))
    return false;
  // The regulator answers the new setpoint or mode only after a while: poll the heater again soon, so
  // heating / idle catches up without waiting a whole poll interval.
  this->schedule_poll(device_id, HEATING_POST_WRITE_POLL_DELAY_MS);
  return true;
}

bool HeaterController::read_private_register(const std::string &device_id, uint8_t reg_hi, uint8_t reg_lo,
                                             const char *label, IoFrame &response, uint8_t max_tries) {
  const IoDevice *dev = this->registry_.get(device_id);
  if (dev == nullptr || !*this->initialized_)
    return false;

  // A read only ever goes to a device that takes register reads at all.
  if (!device_accepts_register_probe(dev->type, dev->subtype)) {
    detail::log_rejected_operation(device_id, *dev, "register read", "heater with private registers");
    return false;
  }

  uint8_t payload[PRIVATE_REGISTER_PAYLOAD_MAX_SIZE];
  const size_t payload_len = encode_private_register_read(reg_hi, reg_lo, payload);
  IoFrame request;
  if (!create_write_private(request, this->node_id_, dev->node_id, dev->low_power, payload, payload_len)) {
    ESP_LOGW(detail::TAG, "Heating read %s for device %s: failed to build frame", label, device_id.c_str());
    return false;
  }

  if (!this->exchange_heating_(device_id, label, request, response, max_tries))
    return false;
  if (!private_reply_echoes_register(response, reg_hi, reg_lo)) {
    char payload_hex[FRAME_LOG_HEX_BUFFER_SIZE];
    bytes_to_hex(response.data, response.data_len, payload_hex, sizeof(payload_hex));
    ESP_LOGD(detail::TAG, "heating %s reply from device %s is not for register 0x%02X%02X: %s", label,
             device_id.c_str(), reg_hi, reg_lo, payload_hex);
    return false;
  }
  return true;
}

bool HeaterController::read_heating_register(const std::string &device_id, HeatingFunction fn, float *value_out,
                                             uint8_t max_tries) {
  return this->read_and_store_heating_register_(device_id, fn, value_out, max_tries, /*follow_comfort=*/false);
}

void HeaterController::read_back_heating_register_(const std::string &device_id, HeatingFunction fn) {
  this->read_and_store_heating_register_(device_id, fn, nullptr, EXCHANGE_RETRY_COUNT, /*follow_comfort=*/true);
}

bool HeaterController::read_and_store_heating_register_(const std::string &device_id, HeatingFunction fn,
                                                        float *value_out, uint8_t max_tries, bool follow_comfort) {
  const IoDevice *gate = this->registry_.get(device_id);
  if (gate == nullptr || !*this->initialized_)
    return false;

  if (!device_uses_heating_registers(gate->type, gate->subtype)) {
    detail::log_rejected_operation(device_id, *gate, "heating read", "heater with private registers");
    return false;
  }

  uint8_t register_low = 0;
  if (!heating_function_is_readable(fn) || !heating_function_register(fn, register_low)) {
    ESP_LOGW(detail::TAG, "Heating read %s for device %s rejected: not a readable register", heating_function_name(fn),
             device_id.c_str());
    return false;
  }

  IoFrame response;
  if (!this->read_private_register(device_id, HEATING_REGISTER_HIGH_BYTE, register_low, heating_function_name(fn),
                                   response, max_tries))
    return false;

  float value = NAN;
  if (!decode_heating_reply(response, fn, value)) {
    char payload_hex[FRAME_LOG_HEX_BUFFER_SIZE];
    bytes_to_hex(response.data, response.data_len, payload_hex, sizeof(payload_hex));
    ESP_LOGD(detail::TAG, "heating %s reply from device %s carries no value: %s", heating_function_name(fn),
             device_id.c_str(), payload_hex);
    return false;
  }

  // The exchange blocks and the registry may have been touched by a callback meanwhile, so the record
  // is looked up again.
  IoDevice *dev = this->registry_.get(device_id);
  if (dev == nullptr)
    return false;
  const HeatingProfile *profile = find_heating_profile(*dev);
  // The read-back of a comfort write carries the temperature in force along, so the card shows the new
  // target now instead of after the next poll of that register.
  std::optional<float> in_force;
  if (follow_comfort && profile != nullptr && profile->programme_period != nullptr &&
      fn == profile->programme_period->comfort_fn)
    in_force = in_force_after_comfort_change(*profile, dev->heating, value);
  dev->heating.store(fn, value);
  if (in_force.has_value())
    dev->heating.store(profile->programme_period->in_force_fn, *in_force);
  // A reading that left the programme drops the temperature in force before anything shows it.
  if (profile != nullptr)
    clear_inactive_programme_reading(*profile, dev->heating);
  this->registry_.notify(device_id);
  if (value_out != nullptr)
    *value_out = value;
  return true;
}

void HeaterController::schedule_poll(const std::string &device_id, uint32_t delay_ms) {
  uint8_t node_id[NODE_ID_SIZE];
  if (!hex_to_bytes(device_id, node_id, NODE_ID_SIZE))
    return;  // malformed device_id: the caller passes one already validated by the registry
  this->hub_.set_timeout_by_id(decisions::heating_poll_timer_id(node_id), delay_ms,
                               [this, device_id]() { this->hub_.queue_status_request(device_id); });
}

// === Water heater (device_feature::WATER_HEATER) ===

bool HeaterController::read_dhw_register(const std::string &device_id, uint8_t index, const char *label,
                                         IoFrame &response, uint8_t max_tries) {
  const IoDevice *dev = this->registry_.get(device_id);
  if (dev == nullptr || !*this->initialized_)
    return false;
  // A family-0x00 read only ever goes to a water heater: a radiator's family is 0x01.
  if (!has_feature(dev->features, device_feature::WATER_HEATER)) {
    detail::log_rejected_operation(device_id, *dev, "hot water register read", "water heater");
    return false;
  }
  return this->read_private_register(device_id, DHW_REGISTER_HIGH_BYTE, index, label, response, max_tries);
}

bool HeaterController::read_dhw_reading(const std::string &device_id, uint8_t index, uint8_t max_tries) {
  IoFrame response;
  const char *label = dhw_register_label(index);
  if (!this->read_dhw_register(device_id, index, label, response, max_tries))
    return false;
  const uint8_t *value = nullptr;
  size_t len = 0;
  // read_private_register() has checked the echo, so this only locates the value bytes.
  if (!decode_dhw_register_reply(response, index, value, len))
    return false;
  // The exchange blocks and the registry may have been touched by a callback meanwhile.
  IoDevice *dev = this->registry_.get(device_id);
  if (dev == nullptr)
    return false;
  if (!store_dhw_reading(device_id, dev->dhw, index, value, len)) {
    char payload_hex[FRAME_LOG_HEX_BUFFER_SIZE];
    bytes_to_hex(response.data, response.data_len, payload_hex, sizeof(payload_hex));
    ESP_LOGD(detail::TAG, "%s reply from device %s carries no value this codec takes: %s", label, device_id.c_str(),
             payload_hex);
    return true;
  }
  this->registry_.notify(device_id);
  return true;
}

bool HeaterController::read_dhw_status(const std::string &device_id, uint8_t max_tries) {
  const IoDevice *dev = this->registry_.get(device_id);
  if (dev == nullptr)
    return false;
  // The main parameter plus FP15 and FP16 in one exchange. The default FP1-FP7 read is not used: the heater
  // holds none of them and answers it with result code 5. The reply goes through the hub's status decoder,
  // whose water heater branch stores the target, the capabilities and the boost and away state.
  IoFrame request;
  return create_status_mp_fp_read(request, this->node_id_, dev->node_id, dev->low_power, DHW_STATUS_FPI1,
                                  DHW_STATUS_FPI2) &&
         this->hub_.status_read(device_id, request, 0, max_tries);
}

bool HeaterController::refuse_unknown_dhw_write_(const std::string &device_id, const char *operation) {
  const IoDevice *dev = this->registry_.get(device_id);
  if (dev == nullptr || !dhw_state_in_registers(dev->model))
    return false;
  ESP_LOGW(detail::TAG,
           "%s for device %s not sent: how an electric storage water heater takes it is not known; use its "
           "display",
           operation, device_id.c_str());
  return true;
}

bool HeaterController::set_water_heater_target(const std::string &device_id, float celsius) {
  if (this->refuse_unknown_dhw_write_(device_id, "Hot water target"))
    return false;
  uint16_t main_parameter = 0;
  if (!encode_dhw_target(celsius, main_parameter)) {
    ESP_LOGW(detail::TAG, "Hot water target %.1f C for device %s rejected: outside %.0f-%.0f C",
             static_cast<double>(celsius), device_id.c_str(), static_cast<double>(DHW_TARGET_MIN_C),
             static_cast<double>(DHW_TARGET_MAX_C));
    return false;
  }
  // The settle poll the execute operation arms reads the target back.
  return this->execute_dhw_(device_id, "hot water target", [this, main_parameter](IoFrame &request, const IoDevice &d) {
    return create_execute_heating_main_parameter(request, this->node_id_, d.node_id, d.low_power, main_parameter);
  });
}

bool HeaterController::execute_dhw_(const std::string &device_id, const char *action,
                                    const HeaterExecuteBuildFn &build) {
  // The bare execute operation and not the cover-shaped funnel: a water heater never moves, so the
  // optimistic-overlay rollback and the moving evidence of a cover command do not apply.
  const bool accepted = this->hub_.execute(
      device_id, action, [](const IoDevice &d) { return has_feature(d.features, device_feature::WATER_HEATER); },
      "water heater", build, HeaterExecuteKind::TRY);
  // The heating state follows a change only after a while, as on a heater after a write.
  if (accepted)
    this->schedule_poll(device_id, HEATING_POST_WRITE_POLL_DELAY_MS);
  return accepted;
}

const IoDevice *HeaterController::water_heater_for_write_(const std::string &device_id, const char *operation) {
  const IoDevice *dev = this->registry_.get(device_id);
  if (dev == nullptr || !*this->initialized_)
    return nullptr;
  if (!has_feature(dev->features, device_feature::WATER_HEATER)) {
    detail::log_rejected_operation(device_id, *dev, operation, "water heater");
    return nullptr;
  }
  return dev;
}

bool HeaterController::write_dhw_register_(const std::string &device_id, const char *operation, uint8_t index,
                                           const uint8_t *payload, size_t payload_len) {
  const IoDevice *dev = this->water_heater_for_write_(device_id, operation);
  if (dev == nullptr)
    return false;

  IoFrame request;
  if (!create_write_private(request, this->node_id_, dev->node_id, dev->low_power, payload, payload_len)) {
    ESP_LOGW(detail::TAG, "%s for device %s: failed to build frame", operation, device_id.c_str());
    return false;
  }

  IoFrame response;
  // A write is something the user waits on: the full retry budget.
  if (!this->exchange_heating_(device_id, dhw_register_label(index), request, response, EXCHANGE_RETRY_COUNT))
    return false;
  // A heater that keeps the value echoes it in the ack; one that ignores the write acks something else
  // (issue #198), so the ack is logged for a field report.
  const uint8_t *ack_value = nullptr;
  size_t ack_len = 0;
  if (decode_dhw_register_reply(response, index, ack_value, ack_len)) {
    char ack_hex[FRAME_LOG_HEX_BUFFER_SIZE];
    bytes_to_hex(ack_value, ack_len, ack_hex, sizeof(ack_hex));
    ESP_LOGI(detail::TAG, "Device %s: %s write acknowledged with value [%s]", device_id.c_str(), operation, ack_hex);
  }
  // The entity shows what the heater holds, not what was sent; the heating state follows later.
  this->read_dhw_reading(device_id, index, EXCHANGE_RETRY_COUNT);
  this->schedule_poll(device_id, HEATING_POST_WRITE_POLL_DELAY_MS);
  return true;
}

bool HeaterController::set_water_heater_mode(const std::string &device_id, DhwMode mode) {
  uint8_t payload[PRIVATE_REGISTER_PAYLOAD_MAX_SIZE];
  const size_t payload_len = encode_dhw_mode_write(static_cast<uint8_t>(mode), payload);
  if (payload_len == 0) {
    ESP_LOGW(detail::TAG, "Hot water mode %u for device %s rejected: not a mode the heater reports",
             static_cast<unsigned>(mode), device_id.c_str());
    return false;
  }
  const IoDevice *dev = this->registry_.get(device_id);
  if (dev != nullptr && !dhw_model_offers_mode(dev->model, mode)) {
    ESP_LOGW(detail::TAG, "Hot water mode %u for device %s rejected: not a mode this model reports",
             static_cast<unsigned>(mode), device_id.c_str());
    return false;
  }
  const bool in_registers = dev != nullptr && dhw_state_in_registers(dev->model);
  if (!this->write_dhw_register_(device_id, "hot water mode", DHW_REG_MODE, payload, payload_len))
    return false;
  // The heater chooses its own target in AUTO and holds its manual one otherwise, so the target changes
  // with the mode: read it now rather than at the next poll. A miss here does not undo the write.
  if (in_registers) {
    this->read_dhw_reading(device_id, DHW_REG_STATE, EXCHANGE_RETRY_COUNT);
    this->read_dhw_reading(device_id, DHW_REG_ACTIVE_TARGET, EXCHANGE_RETRY_COUNT);
    return true;
  }
  this->read_dhw_status(device_id, EXCHANGE_RETRY_COUNT);
  return true;
}

bool HeaterController::set_dhw_operating_mode_(const std::string &device_id, DhwOperatingFeature which, bool on) {
  const DhwOperatingFeatureNames &names = DHW_OPERATING_FEATURE_NAMES[static_cast<size_t>(which)];
  const DhwOperatingFeatureWire &wire = dhw_operating_feature_wire(which);
  const IoDevice *dev = this->water_heater_for_write_(device_id, names.feature);
  if (dev == nullptr)
    return false;
  // Once FP15 has been read the heater says what it supports; before that the write is allowed to try.
  if (dev->dhw.capabilities >= 0 && (static_cast<uint16_t>(dev->dhw.capabilities) & wire.capability) == 0) {
    ESP_LOGW(detail::TAG, "%s for device %s refused: the heater does not report it as supported (FP15 0x%04X)",
             names.feature, device_id.c_str(), static_cast<unsigned>(dev->dhw.capabilities));
    return false;
  }

  if (on) {
    // The duration goes first (ADR 0081). The heater has been seen to ignore it, and a missed ack is not
    // fatal either: the FP16 EXECUTE below is what switches the mode.
    uint8_t payload[PRIVATE_REGISTER_PAYLOAD_MAX_SIZE];
    const size_t payload_len = encode_dhw_duration_write(which, payload);
    if (!this->write_dhw_register_(device_id, names.duration, wire.duration_register, payload, payload_len)) {
      ESP_LOGW(detail::TAG, "Device %s: %s write not acknowledged, sending the mode anyway", device_id.c_str(),
               names.duration);
    }
  }

  const uint16_t word = on ? wire.on_word : DHW_OPERATING_MODE_OFF;
  // The off word ends boost and away alike.
  const bool accepted =
      this->execute_dhw_(device_id, on ? names.feature : "hot water boost and away off",
                         [this, word](IoFrame &request, const IoDevice &d) {
                           return create_execute_heating_with_fp16(request, this->node_id_, d.node_id, d.low_power,
                                                                   MAIN_PARAMETER_KEEP, word);
                         });
  if (!accepted)
    return false;
  // The entity shows what FP16 reports, never the command: read it at once. A miss does not undo the write,
  // and the poll execute_dhw_() moved reads it again.
  this->read_dhw_status(device_id, EXCHANGE_RETRY_COUNT);
  return true;
}

bool HeaterController::set_dhw_register_feature_(const std::string &device_id, DhwOperatingFeature which, bool on) {
  const bool boost = which == DhwOperatingFeature::BOOST;
  const char *flag_name = boost ? "hot water boost flag" : "hot water away flag";
  const uint8_t flag_register = boost ? DHW_REG_BOOST_FLAG : DHW_REG_AWAY_FLAG;
  IoDevice *dev = this->registry_.get(device_id);
  if (dev == nullptr)
    return false;
  // The read-back below must be this write's, not an older one.
  (boost ? dev->dhw.boost_flag : dev->dhw.away_flag) = -1;

  uint8_t payload[PRIVATE_REGISTER_PAYLOAD_MAX_SIZE];
  size_t payload_len = encode_dhw_flag_write(which, on, payload);
  if (!this->write_dhw_register_(device_id, flag_name, flag_register, payload, payload_len))
    return false;

  // write_dhw_register_() has read the flag back. A heater that did not keep it is sent the state itself.
  dev = this->registry_.get(device_id);
  if (dev == nullptr)
    return false;
  const int16_t held = boost ? dev->dhw.boost_flag : dev->dhw.away_flag;
  if (held != (on ? 1 : 0)) {
    // A boost runs in automatic mode only; an absence ends in the mode register 0x0006 holds.
    DhwState state = boost ? DhwState::BOOST : DhwState::AWAY;
    if (!on)
      state = boost || dev->dhw.mode == static_cast<int16_t>(DhwMode::AUTO) ? DhwState::AUTO : DhwState::MANUAL;
    ESP_LOGW(detail::TAG, "Device %s: the %s did not keep the value (it reads %d), writing state %u instead",
             device_id.c_str(), flag_name, held, static_cast<unsigned>(state));
    payload_len = encode_dhw_state_write(state, payload);
    if (!this->write_dhw_register_(device_id, "hot water state", DHW_REG_STATE, payload, payload_len))
      return false;
  }
  // The entity shows what the heater reports, never the command. A miss here does not undo the write.
  this->read_dhw_reading(device_id, DHW_REG_STATE, EXCHANGE_RETRY_COUNT);
  this->read_dhw_reading(device_id, DHW_REG_ACTIVE_TARGET, EXCHANGE_RETRY_COUNT);
  return true;
}

bool HeaterController::set_water_heater_boost(const std::string &device_id, bool on) {
  if (const IoDevice *dev = this->water_heater_for_write_(device_id, "hot water boost");
      dev != nullptr && dhw_state_in_registers(dev->model))
    return this->set_dhw_register_feature_(device_id, DhwOperatingFeature::BOOST, on);
  return this->set_dhw_operating_mode_(device_id, DhwOperatingFeature::BOOST, on);
}

bool HeaterController::set_water_heater_away(const std::string &device_id, bool on) {
  if (const IoDevice *dev = this->water_heater_for_write_(device_id, "hot water away");
      dev != nullptr && dhw_state_in_registers(dev->model))
    return this->set_dhw_register_feature_(device_id, DhwOperatingFeature::AWAY, on);
  return this->set_dhw_operating_mode_(device_id, DhwOperatingFeature::AWAY, on);
}

// === Regular polls ===

bool HeaterController::polls(const IoDevice &dev) {
  return find_heating_profile(dev) != nullptr || has_feature(dev.features, device_feature::WATER_HEATER);
}

bool HeaterController::poll(const std::string &device_id) {
  const IoDevice *dev = this->registry_.get(device_id);
  if (dev == nullptr)
    return false;
  // A heater with private registers has no 0x03 status: its state is a handful of registers.
  const HeatingProfile *heating = find_heating_profile(*dev);
  if (heating != nullptr && heating->path == HeatingControlPath::PRIVATE_REGISTERS)
    return this->poll_registers_(device_id);
  // A water heater answers a status read for its target, and keeps the rest in family-0x00 registers.
  if (has_feature(dev->features, device_feature::WATER_HEATER))
    return this->poll_dhw_(device_id);
  // A heating-level or temperature heater is polled with the ordinary status read.
  if (heating != nullptr)
    return this->poll_status_read_(device_id);
  return false;
}

void HeaterController::check_heater_model(const std::string &device_id, uint8_t max_tries) {
  IoDevice *dev = this->registry_.get(device_id);
  if (dev == nullptr || dev->model_checked || dev->model_check_attempts >= decisions::HEATER_MODEL_CHECK_MAX_RUNS ||
      !device_offers_models(dev->type, dev->subtype))
    return;
  dev->model_check_attempts++;

  IoFrame name_reply;
  const bool name_read = this->read_private_register(device_id, HEATING_REGISTER_HIGH_BYTE, HEATING_REGISTER_MODEL_NAME,
                                                     "model_name", name_reply, max_tries);

  // A towel dryer's poll has already read its mode register; any other heater is asked once here.
  dev = this->registry_.get(device_id);
  if (dev == nullptr)
    return;
  bool mode_answered = dev->heating.towel_mode >= 0;
  bool mode_has_value = mode_answered;
  uint8_t mode_register = 0;
  if (!mode_answered && heating_function_register(HeatingFunction::TOWEL_MODE, mode_register)) {
    IoFrame mode_reply;
    mode_answered = this->read_private_register(device_id, HEATING_REGISTER_HIGH_BYTE, mode_register, "towel_mode",
                                                mode_reply, max_tries);
    mode_has_value = mode_answered && mode_reply.data_len > PRIVATE_REGISTER_VALUE_OFFSET;
  }
  // Report only from a run in which both reads were answered; otherwise the next poll tries again.
  if (!name_read || !mode_answered)
    return;
  dev = this->registry_.get(device_id);
  if (dev == nullptr)
    return;
  dev->model_checked = true;

  const std::string name = decisions::format_heater_model_name(name_reply.data + PRIVATE_REGISTER_VALUE_OFFSET,
                                                               name_reply.data_len - PRIVATE_REGISTER_VALUE_OFFSET);
  ESP_LOGI(detail::TAG, "Device %s reports model name \"%s\"", device_id.c_str(),
           name.empty() ? "(none)" : name.c_str());

  const DeviceTypeInfo *towel = find_device_model_info(dev->type, dev->subtype, DeviceModel::TOWEL_DRYER);
  const char *towel_name = towel != nullptr ? towel->name : "towel_dryer";
  switch (decisions::classify_heater_model_hint(dev->model, mode_has_value)) {
    case decisions::HeaterModelHint::LOOKS_LIKE_RADIATOR:
      ESP_LOGW(detail::TAG,
               "Device %s is declared with io_model: %s, but it holds no towel-dryer mode (register 0x0136 is "
               "empty): it looks like a radiator. Remove io_model: from its climate entry",
               device_id.c_str(), towel_name);
      break;
    case decisions::HeaterModelHint::LOOKS_LIKE_TOWEL_DRYER:
      ESP_LOGW(detail::TAG,
               "Device %s holds a towel-dryer mode (register 0x0136): it looks like a towel dryer. Set "
               "io_model: %s on its climate entry for its modes, Boost and Drying",
               device_id.c_str(), towel_name);
      break;
    case decisions::HeaterModelHint::NONE:
      break;
  }
}

void HeaterController::poll_programme_period_(const std::string &device_id, const HeatingProfile &profile,
                                              uint8_t max_tries) {
  const IoDevice *dev = this->registry_.get(device_id);
  if (dev == nullptr || !programme_period_active(profile, dev->heating))
    return;
  const HeatingProgrammePeriod &period = *profile.programme_period;
  if (!this->read_heating_register(device_id, period.in_force_fn, nullptr, max_tries))
    return;
  // A temperature that fits neither period means the eco setback is not what the heater holds now (it was
  // changed on the display): forget it, so the boot registers read it again.
  IoDevice *updated = this->registry_.get(device_id);
  if (updated != nullptr && resolve_programme_period(profile, updated->heating).preset_index < 0)
    updated->heating.clear(period.setback_fn);
}

bool HeaterController::poll_registers_(const std::string &device_id) {
  const IoDevice *dev = this->registry_.get(device_id);
  if (dev == nullptr)
    return false;
  // The registers one poll reads come from the device's profile, so there is no per-device list here.
  const HeatingProfile *profile = find_heating_profile(*dev);
  if (profile == nullptr || profile->path != HeatingControlPath::PRIVATE_REGISTERS)
    return false;

  return this->run_poll_(device_id, PollLadder::RUN_BY_POLL, [this, &device_id, profile](uint8_t max_tries) {
    return this->read_poll_registers_(device_id, *profile, max_tries);
  });
}

bool HeaterController::read_poll_registers_(const std::string &device_id, const HeatingProfile &profile,
                                            uint8_t max_tries) {
  bool ok = true;
  size_t answered = 0;
  for (const HeatingFunction fn : profile.poll_registers) {
    if (!this->read_heating_register(device_id, fn, nullptr, max_tries)) {
      ok = false;
      break;
    }
    answered++;
  }

  // The temperature in force is read only while the heater's own programme runs: outside it the register
  // holds no temperature, and the card needs no read.
  if (ok && profile.programme_period != nullptr)
    this->poll_programme_period_(device_id, profile, max_tries);

  // A part the heater may not have (a night light): read every poll once the core registers answered,
  // and a miss is not a poll failure (the next poll asks again).
  for (const HeatingFunction fn : profile.extra_poll_registers) {
    if (!ok)
      break;
    this->read_heating_register(device_id, fn, nullptr, max_tries);
  }

  // Properties and settings that rarely change are read until they have been read once. A miss here
  // is not a poll failure; the next poll asks again.
  for (const HeatingFunction fn : profile.boot_registers) {
    if (!ok)
      break;
    const IoDevice *current = this->registry_.get(device_id);
    if (current != nullptr && !current->heating.has(fn))
      this->read_heating_register(device_id, fn, nullptr, max_tries);
  }

  // Once the device has answered a read: the one-off model check. It runs even when a later register of
  // the poll failed, because a towel dryer declared on a radiator fails exactly there (its mode register
  // holds no value) and is what the check exists to point out.
  if (answered > 0)
    this->check_heater_model(device_id, max_tries);
  return ok;
}

// === Poll cadence ===

bool HeaterController::run_poll_(const std::string &device_id, PollLadder ladder, const PollBody &body) {
  const IoDevice *dev = this->registry_.get(device_id);
  if (dev == nullptr)
    return false;  // no longer registered: no timer either

  bool ok = false;
  if (*this->initialized_) {
    // The failure ladder lives in StatusPollPolicy and only runs inside a tracking window. Open one
    // unless one is already running: begin_tracking() resets the failure counters, which would flatten
    // the ladder to its first step on every retry.
    if (!this->policy_.is_tracking_active(device_id, millis()))
      this->policy_.begin_tracking(device_id, 0, millis());

    // A poll always runs inside a tracking window, so the ladder is its retry mechanism: the heater
    // try band (decisions::scheduled_poll_max_tries()), not a cover's settling plan.
    const uint8_t max_tries = decisions::scheduled_poll_max_tries(this->policy_.get_status_poll_failures(device_id),
                                                                  this->policy_.get_auth_poll_failures(device_id));
    ok = body(max_tries);
    if (ladder == PollLadder::RUN_BY_POLL) {
      if (ok) {
        this->policy_.clear(device_id);
      } else {
        this->hub_.poll_backoff(device_id, this->engine_.get_debug().saw_challenge);
      }
    }
  }
  // The regular cadence continues whatever this poll found, or whether it could run at all; a failed
  // poll is additionally retried sooner by the ladder above.
  this->schedule_poll(device_id, heater_poll_interval_ms(this->policy_.get_interval(device_id)));
  return ok;
}

bool HeaterController::poll_status_read_(const std::string &device_id) {
  // The status read's own failure ladder (execute_request_and_update_()) is the retry, so the poll
  // leaves the ladder to the body.
  return this->run_poll_(device_id, PollLadder::RUN_BY_BODY, [this, &device_id](uint8_t max_tries) {
    const IoDevice *current = this->registry_.get(device_id);
    IoFrame request;
    // The generic status request: no heater row carries TILT, so the cover's tilt variant never applies.
    if (current == nullptr || !create_get_status(request, this->node_id_, current->node_id, current->low_power))
      return false;
    return this->hub_.status_read(device_id, request, STATUS_RETRY_AFTER_FAIL_MS, max_tries);
  });
}

// === Water heater poll ===

namespace {

/// The registers every water heater poll reads, after the status read, in this order.
constexpr uint8_t DHW_POLL_REGISTERS[] = {DHW_REG_WATER_TEMPERATURE, DHW_REG_MODE,          DHW_REG_HEATING,
                                          DHW_REG_BOOST_DURATION,    DHW_REG_AWAY_DURATION, DHW_REG_ENERGY};
/// The registers the poll of an electric storage water heater reads, in this order. It reads no status: this
/// model's status reply carries nothing, and registers 0x0021, 0x0000 and 0x0005 answer with no value.
constexpr uint8_t DHW_ELECTRIC_POLL_REGISTERS[] = {
    DHW_REG_WATER_TEMPERATURE, DHW_REG_MODE,              DHW_REG_STATE,         DHW_REG_ACTIVE_TARGET,
    DHW_REG_TARGET_SHOWERS,    DHW_REG_SHOWERS_AVAILABLE, DHW_REG_HEATING_POWER, DHW_REG_ENERGY};

}  // namespace

bool HeaterController::poll_dhw_(const std::string &device_id) {
  const IoDevice *dev = this->registry_.get(device_id);
  if (dev == nullptr || !has_feature(dev->features, device_feature::WATER_HEATER))
    return false;

  const bool in_registers = dhw_state_in_registers(dev->model);
  return this->run_poll_(device_id, PollLadder::RUN_BY_POLL, [this, &device_id, in_registers](uint8_t max_tries) {
    bool ok = in_registers || this->read_dhw_status(device_id, max_tries);

    const uint8_t *registers = in_registers ? DHW_ELECTRIC_POLL_REGISTERS : DHW_POLL_REGISTERS;
    const size_t count = in_registers ? sizeof(DHW_ELECTRIC_POLL_REGISTERS) : sizeof(DHW_POLL_REGISTERS);
    for (size_t i = 0; ok && i < count; i++)
      ok = this->read_dhw_reading(device_id, registers[i], max_tries);

    // The brand never changes: read until it has been read once. A miss is not a poll failure.
    const IoDevice *current = this->registry_.get(device_id);
    if (ok && current != nullptr && current->dhw.brand < 0)
      this->read_dhw_reading(device_id, DHW_REG_BRAND, max_tries);
    return ok;
  });
}

}  // namespace home_io_control
}  // namespace esphome

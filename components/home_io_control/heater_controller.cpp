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
  if (plan.read_back) {
    float read_back_c = NAN;
    const HeatingRead read_back = this->read_back_heating_register_(device_id, plan.read_back_fn, &read_back_c);
    // A refused override changes nothing else: the comfort setpoint stays, and the next poll shows what the
    // heater holds. A read-back with no reply is already logged by the exchange and proves no refusal.
    if (plan.temporary_override && read_back == HeatingRead::EMPTY) {
      ESP_LOGW(detail::TAG, "Device %s did not take the temporary override of %.1f C: its register reads empty",
               device_id.c_str(), static_cast<double>(plan.register_value));
    } else if (plan.temporary_override && read_back == HeatingRead::VALUE &&
               !heating_override_taken(plan.register_value, read_back_c)) {
      ESP_LOGW(detail::TAG, "Device %s did not take the temporary override of %.1f C: its register reads %.1f C",
               device_id.c_str(), static_cast<double>(plan.register_value), static_cast<double>(read_back_c));
    }
    if (read_back == HeatingRead::VALUE)
      this->show_temperature_in_force_after_write_(device_id, plan);
  }
  return true;
}

void HeaterController::show_temperature_in_force_after_write_(const std::string &device_id,
                                                              const HeatingWritePlan &plan) {
  const IoDevice *dev = this->registry_.get(device_id);
  const HeatingProfile *profile = dev != nullptr ? find_heating_profile(*dev) : nullptr;
  // Only a write of the mode or of the timed run can start the programme; the read-back of an override already
  // carries the value in force, and a comfort-setpoint write is made where the programme does not run.
  if (profile == nullptr || profile->programme_period == nullptr ||
      (plan.read_back_fn != profile->mode_fn && plan.read_back_fn != profile->preset_fn))
    return;
  // EXCHANGE_RETRY_COUNT, as the read before a target: the user waits for the card. The poll's own try band
  // applies to its reads only. poll_temperature_in_force_() sends nothing unless the programme runs on the
  // stored readings and the heater has not answered without a value, and handles a silent answer; the next poll
  // asks again. On a dryer the register this write did not touch is an older reading, so an answer without a
  // value may come from a heater that runs something else: it marks nothing, and the target's own read decides.
  this->poll_temperature_in_force_(device_id, *profile, EXCHANGE_RETRY_COUNT, InForceEmptyAnswer::CHANGES_NOTHING);
}

bool HeaterController::read_programme_state(const std::string &device_id) {
  const IoDevice *dev = this->registry_.get(device_id);
  const HeatingProfile *profile = dev != nullptr ? find_heating_profile(*dev) : nullptr;
  if (profile == nullptr || profile->programme_period == nullptr)
    return false;
  // The mode first, then (while it shows the programme) the timed run on top of it. A heater that does not
  // answer, or answers without a value, is asked nothing more: the caller writes nothing.
  if (this->read_and_store_heating_register_(device_id, profile->mode_fn, nullptr, EXCHANGE_RETRY_COUNT,
                                             /*follow_override=*/false) != HeatingRead::VALUE)
    return false;
  // The exchange blocks; a record removed meanwhile has nothing left to write to.
  dev = this->registry_.get(device_id);
  if (dev == nullptr)
    return false;
  const std::optional<HeatingFunction> timed_run = timed_run_register(*profile, dev->heating);
  if (timed_run && this->read_and_store_heating_register_(device_id, *timed_run, nullptr, EXCHANGE_RETRY_COUNT,
                                                          /*follow_override=*/false) != HeatingRead::VALUE)
    return false;
  // The temperature in force is asked only while the heater has not answered it yet (the poll refreshes it
  // otherwise). poll_temperature_in_force_() reads it only while the programme runs; a silent heater stays
  // UNKNOWN, which the plan refuses.
  dev = this->registry_.get(device_id);
  if (dev != nullptr && dev->temperature_in_force == TemperatureInForceSupport::UNKNOWN)
    this->poll_temperature_in_force_(device_id, *profile, EXCHANGE_RETRY_COUNT, InForceEmptyAnswer::MARKS_ABSENT);
  this->read_period_for_target_(device_id, *profile);
  return true;
}

void HeaterController::read_period_for_target_(const std::string &device_id, const HeatingProfile &profile) {
  IoDevice *dev = this->registry_.get(device_id);
  // The period decides only where a target goes while the programme runs on a heater that takes the override in
  // an eco period only; every other target needs no level.
  if (dev == nullptr || !profile.programme_period->override_in_eco_only ||
      dev->temperature_in_force != TemperatureInForceSupport::REPORTED ||
      !programme_period_active(profile, dev->heating))
    return;
  // The stored level may be one or two polls old, and an unanswered read leaves it as it was, so forget it first:
  // a level that is there afterwards came from this read.
  dev->heating.level = -1;
  this->read_heating_level_(device_id, EXCHANGE_RETRY_COUNT);
}

void HeaterController::set_device_temperature_range(const std::string &device_id, float min_c, float max_c) {
  IoDevice *dev = this->registry_.get(device_id);
  if (dev == nullptr)
    return;
  dev->temperature_range_min_c = min_c;
  dev->temperature_range_max_c = max_c;
}

void HeaterController::enable_central_heating_mode_read(const std::string &device_id) {
  IoDevice *dev = this->registry_.get(device_id);
  if (dev != nullptr && has_feature(dev->features, device_feature::CENTRAL_HEATING_MODE))
    dev->central_heating_mode_read = CentralHeatingModeRead::ASKED;
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
  return this->read_and_store_heating_register_(device_id, fn, value_out, max_tries, /*follow_override=*/false) ==
         HeatingRead::VALUE;
}

HeaterController::HeatingRead HeaterController::read_back_heating_register_(const std::string &device_id,
                                                                            HeatingFunction fn, float *value_out) {
  return this->read_and_store_heating_register_(device_id, fn, value_out, EXCHANGE_RETRY_COUNT,
                                                /*follow_override=*/true);
}

HeaterController::HeatingRead HeaterController::read_and_store_heating_register_(const std::string &device_id,
                                                                                 HeatingFunction fn, float *value_out,
                                                                                 uint8_t max_tries,
                                                                                 bool follow_override) {
  const IoDevice *gate = this->registry_.get(device_id);
  if (gate == nullptr || !*this->initialized_)
    return HeatingRead::NO_ANSWER;

  if (!device_uses_heating_registers(gate->type, gate->subtype)) {
    detail::log_rejected_operation(device_id, *gate, "heating read", "heater with private registers");
    return HeatingRead::NO_ANSWER;
  }

  uint8_t register_low = 0;
  if (!heating_function_is_readable(fn) || !heating_function_register(fn, register_low)) {
    ESP_LOGW(detail::TAG, "Heating read %s for device %s rejected: not a readable register", heating_function_name(fn),
             device_id.c_str());
    return HeatingRead::NO_ANSWER;
  }

  IoFrame response;
  if (!this->read_private_register(device_id, HEATING_REGISTER_HIGH_BYTE, register_low, heating_function_name(fn),
                                   response, max_tries))
    return HeatingRead::NO_ANSWER;

  float value = NAN;
  if (!decode_heating_reply(response, fn, value)) {
    char payload_hex[FRAME_LOG_HEX_BUFFER_SIZE];
    bytes_to_hex(response.data, response.data_len, payload_hex, sizeof(payload_hex));
    ESP_LOGD(detail::TAG, "heating %s reply from device %s carries no value: %s", heating_function_name(fn),
             device_id.c_str(), payload_hex);
    return HeatingRead::EMPTY;
  }

  // The exchange blocks and the registry may have been touched by a callback meanwhile, so the record
  // is looked up again.
  IoDevice *dev = this->registry_.get(device_id);
  if (dev == nullptr)
    return HeatingRead::NO_ANSWER;
  const HeatingProfile *profile = find_heating_profile(*dev);
  // The read-back of an override carries the temperature in force along, so the card shows the new target
  // now instead of after the next poll of that register.
  std::optional<float> in_force;
  if (follow_override && profile != nullptr && profile->programme_period != nullptr &&
      fn == profile->programme_period->derogation_fn && value >= HEATING_TEMP_MIN_C) {
    // A written override is the temperature the heater holds now; 0 (none active) leaves the reading alone.
    in_force = value;
  }
  dev->heating.store(fn, value);
  if (in_force.has_value())
    dev->heating.store(profile->programme_period->in_force_fn, *in_force);
  // A reading that left the programme drops the temperature in force before anything shows it.
  if (profile != nullptr)
    clear_inactive_programme_reading(*profile, dev->heating);
  this->registry_.notify(device_id);
  if (value_out != nullptr)
    *value_out = value;
  return HeatingRead::VALUE;
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

bool HeaterController::set_water_heater_target(const std::string &device_id, float celsius) {
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
  if (!this->write_dhw_register_(device_id, "hot water mode", DHW_REG_MODE, payload, payload_len))
    return false;
  // The heater chooses its own target in AUTO and holds its manual one otherwise, so the target changes
  // with the mode: read it now rather than at the next poll. A miss here does not undo the write.
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

bool HeaterController::set_water_heater_boost(const std::string &device_id, bool on) {
  return this->set_dhw_operating_mode_(device_id, DhwOperatingFeature::BOOST, on);
}

bool HeaterController::set_water_heater_away(const std::string &device_id, bool on) {
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

void HeaterController::poll_temperature_in_force_(const std::string &device_id, const HeatingProfile &profile,
                                                  uint8_t max_tries, InForceEmptyAnswer empty_answer) {
  const IoDevice *dev = this->registry_.get(device_id);
  if (dev == nullptr || dev->temperature_in_force == TemperatureInForceSupport::ABSENT ||
      !programme_period_active(profile, dev->heating))
    return;
  const HeatingFunction fn = profile.programme_period->in_force_fn;
  const HeatingRead read =
      this->read_and_store_heating_register_(device_id, fn, nullptr, max_tries, /*follow_override=*/false);
  // A missed read (no reply) says nothing about the register and leaves what the heater has shown before.
  if (read == HeatingRead::NO_ANSWER)
    return;
  IoDevice *current = this->registry_.get(device_id);
  if (current == nullptr)
    return;
  if (read == HeatingRead::VALUE) {
    current->temperature_in_force = TemperatureInForceSupport::REPORTED;
    return;
  }
  // A heater that has answered with a value has the register: a later answer without one is a reading without
  // a value, like a missed read, and the next poll asks again. A read that went partly by older readings
  // (CHANGES_NOTHING) cannot tell a missing register from a heater outside its programme.
  if (current->temperature_in_force == TemperatureInForceSupport::REPORTED ||
      empty_answer == InForceEmptyAnswer::CHANGES_NOTHING)
    return;
  // The heater answered without a value: it has no such register. The read is not repeated until the next boot,
  // and its targets keep writing the comfort setpoint.
  current->temperature_in_force = TemperatureInForceSupport::ABSENT;
  ESP_LOGI(detail::TAG, "Device %s reports no temperature in force (%s): its target stays the comfort setpoint",
           device_id.c_str(), heating_function_name(fn));
}

void HeaterController::poll_central_heating_mode_(const std::string &device_id, uint8_t max_tries) {
  const IoDevice *dev = this->registry_.get(device_id);
  if (dev == nullptr || dev->central_heating_mode_read != CentralHeatingModeRead::ASKED)
    return;
  constexpr HeatingFunction fn = HeatingFunction::CENTRAL_HEATING_MODE;
  if (this->read_and_store_heating_register_(device_id, fn, nullptr, max_tries, /*follow_override=*/false) !=
      HeatingRead::EMPTY)
    return;
  IoDevice *current = this->registry_.get(device_id);
  if (current == nullptr)
    return;
  current->heating.clear(fn);
  current->central_heating_mode_read = CentralHeatingModeRead::ABSENT;
  ESP_LOGI(detail::TAG, "Device %s reports no central-heating mode (%s): its sensor stays without a state",
           device_id.c_str(), heating_function_name(fn));
}

void HeaterController::read_heating_level_(const std::string &device_id, uint8_t max_tries) {
  const IoDevice *dev = this->registry_.get(device_id);
  IoFrame request;
  // The status read of the main parameter; the reply's main parameter is the heating level. The failure is
  // not a poll failure: the registers answered, and the next poll asks again.
  if (dev == nullptr || !create_status_mp_fp_read(request, this->node_id_, dev->node_id, dev->low_power))
    return;
  this->hub_.status_read(device_id, request, 0, max_tries);
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
    this->poll_temperature_in_force_(device_id, profile, max_tries, InForceEmptyAnswer::MARKS_ABSENT);

  // A setting the heater's own controller changes, or a part the heater may not have (a night light): read
  // every poll once the core registers answered, and a miss is not a poll failure (the next poll asks again).
  for (const HeatingFunction fn : profile.extra_poll_registers) {
    if (!ok)
      break;
    this->read_heating_register(device_id, fn, nullptr, max_tries);
  }

  // The central-heating mode is read only where the YAML declares its sensor; a miss is not a poll failure.
  if (ok)
    this->poll_central_heating_mode_(device_id, max_tries);

  // Properties and settings that rarely change are read until they have been read once. A miss here
  // is not a poll failure; the next poll asks again.
  for (const HeatingFunction fn : profile.boot_registers) {
    if (!ok)
      break;
    const IoDevice *current = this->registry_.get(device_id);
    if (current != nullptr && !current->heating.has(fn))
      this->read_heating_register(device_id, fn, nullptr, max_tries);
  }

  // The heating level of a heater with a programme comes from its status read, in every mode: what a heater is
  // polled for depends on its profile alone, never on which entities the YAML declares.
  if (ok && profile.programme_period != nullptr && profile.programme_period->reports_level)
    this->read_heating_level_(device_id, max_tries);

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

}  // namespace

bool HeaterController::poll_dhw_(const std::string &device_id) {
  const IoDevice *dev = this->registry_.get(device_id);
  if (dev == nullptr || !has_feature(dev->features, device_feature::WATER_HEATER))
    return false;

  return this->run_poll_(device_id, PollLadder::RUN_BY_POLL, [this, &device_id](uint8_t max_tries) {
    bool ok = this->read_dhw_status(device_id, max_tries);

    for (size_t i = 0; ok && i < sizeof(DHW_POLL_REGISTERS); i++)
      ok = this->read_dhw_reading(device_id, DHW_POLL_REGISTERS[i], max_tries);

    // The brand never changes: read until it has been read once. A miss is not a poll failure.
    const IoDevice *current = this->registry_.get(device_id);
    if (ok && current != nullptr && current->dhw.brand < 0)
      this->read_dhw_reading(device_id, DHW_REG_BRAND, max_tries);
    return ok;
  });
}

}  // namespace home_io_control
}  // namespace esphome

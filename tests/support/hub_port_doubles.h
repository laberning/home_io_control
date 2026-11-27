#pragma once

/// @file hub_port_doubles.h
/// @brief Test doubles of the three hub ports (hub_ports.h) for the entity platform tests.
///
/// A platform test builds the entity, wires it to a double with `set_parent(&double)` and asserts on what
/// the double recorded; no hub object is involved. The entity takes from the double each port the double
/// implements, so a test that only needs the device port derives from DevicePortDouble alone, and one that
/// also needs heating derives from both.
///
/// DevicePortDouble owns a real DeviceRegistry, so device storage, subscriber fan-out and optimistic state
/// run production code. Device IDs must be valid hex node IDs (e.g. "ABC123"): DeviceRegistry::add() rejects
/// anything else and get_device() would then return nullptr.

#include "device_registry.h"
#include "entity_helpers.h"
#include "hub_ports.h"
#include "operation_queue.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace test {

using namespace esphome::home_io_control;

/// @brief Double of HubDevicePort: a real device registry, recorded queued operations and recorded
/// poll intervals.
class DevicePortDouble : public HubDevicePort {
 public:
  // === Registry (production code) ===

  void add_device(const std::string &device_id, const DeviceConfig &cfg) override {
    this->registry_.add(device_id, cfg);
  }
  void add_device(const std::string &device_id) override { this->registry_.add(device_id); }
  IoDevice *get_device(const std::string &device_id) override { return this->registry_.get(device_id); }
  void register_device_callback(DeviceUpdateCallback cb) override { this->registry_.subscribe(std::move(cb)); }
  void set_device_travel_speed(const std::string &device_id, TravelSpeed speed) override {
    this->registry_.set_travel_speed(device_id, speed);
  }
  bool apply_optimistic_target(const std::string &device_id, float target_io_position) override {
    return this->registry_.apply_optimistic_target(device_id, target_io_position);
  }
  bool apply_optimistic_stop(const std::string &device_id) override {
    // Restorable, as in the hub: the entity's own STOP is about to be sent and can fail.
    return this->registry_.apply_optimistic_stop(device_id, /*restorable=*/true);
  }
  bool apply_optimistic_tilt(const std::string &device_id, float tilt_percent) override {
    return this->registry_.apply_optimistic_tilt(device_id, tilt_percent);
  }
  [[nodiscard]] bool radio_stopped_responding() const override { return this->radio_stopped; }
  [[nodiscard]] std::string describe_last_commander(const IoDevice &dev) const override {
    return detail::describe_last_commander(dev, this->hub_node_id);
  }

  // === Poll intervals (recorded) ===
  // Like the hub, an interval is kept only for a device that is registered.

  void set_device_status_poll_interval(const std::string &device_id, uint32_t interval_ms) override {
    this->record_interval_(this->status_poll_intervals_, device_id, interval_ms);
  }
  void set_device_status_refresh_interval(const std::string &device_id, uint32_t interval_ms) override {
    this->record_interval_(this->status_refresh_intervals_, device_id, interval_ms);
  }
  void set_device_reading_poll_interval(const std::string &device_id, DeviceReading reading,
                                        uint32_t interval_ms) override {
    if (this->registry_.get(device_id) != nullptr)
      this->reading_poll_intervals_[{device_id, reading}] = interval_ms;
  }

  /// @return The interval last set for the device, or 0 when none was set or the device is not registered.
  [[nodiscard]] uint32_t status_poll_interval(const std::string &id) const {
    return lookup_(status_poll_intervals_, id);
  }
  [[nodiscard]] uint32_t status_refresh_interval(const std::string &id) const {
    return lookup_(status_refresh_intervals_, id);
  }
  /// @return The interval last set for a scheduled reading of the device, or 0 when none was set.
  [[nodiscard]] uint32_t reading_poll_interval(const std::string &id, DeviceReading reading) const {
    const auto it = reading_poll_intervals_.find({id, reading});
    return it != reading_poll_intervals_.end() ? it->second : 0;
  }

  // === Queued operations (recorded) ===
  // As in the hub's queue, a light level and a light on/off are both a SET_LIGHT_STATE carrying a position
  // (on and off use the binary position encoding).

  void queue_request_device_status(const std::string &device_id) override {
    this->queued_.push_back({PendingOperationType::REQUEST_STATUS, device_id, 0});
  }
  void queue_request_device_name(const std::string &device_id) override {
    this->queued_.push_back({PendingOperationType::REQUEST_NAME, device_id, 0});
  }
  bool queue_device_command(const std::string &device_id, CoverCommand cmd) override {
    PendingOperation op{};
    op.type = PendingOperationType::DEVICE_COMMAND;
    op.device_id = device_id;
    op.command = cmd;
    this->queued_.push_back(op);
    return true;
  }
  void queue_set_device_position(const std::string &device_id, uint8_t position) override {
    this->queued_.push_back({PendingOperationType::SET_POSITION, device_id, position});
  }
  bool queue_set_device_position_automatic(const std::string &device_id, uint8_t position) override {
    this->queued_.push_back({PendingOperationType::SET_POSITION_AUTOMATIC, device_id, position});
    return true;
  }
  void queue_set_device_tilt(const std::string &device_id, uint8_t tilt_percent) override {
    this->queued_.push_back({PendingOperationType::SET_TILT, device_id, tilt_percent});
  }
  void queue_set_device_position_and_tilt(const std::string &device_id, uint8_t position,
                                          uint8_t tilt_percent) override {
    this->queued_.push_back({PendingOperationType::SET_POSITION_AND_TILT, device_id, position, tilt_percent});
  }
  void queue_set_light_position(const std::string &device_id, uint8_t position) override {
    this->queued_.push_back({PendingOperationType::SET_LIGHT_STATE, device_id, position});
  }
  void queue_set_light_state(const std::string &device_id, bool on) override {
    this->queued_.push_back({PendingOperationType::SET_LIGHT_STATE, device_id, binary_position_(on)});
  }
  void queue_set_fan_position(const std::string &device_id, uint8_t position) override {
    this->queued_.push_back({PendingOperationType::SET_FAN_POSITION, device_id, position});
  }
  void queue_set_switch_state(const std::string &device_id, bool on) override {
    this->queued_.push_back({PendingOperationType::SET_SWITCH_STATE, device_id, binary_position_(on)});
  }
  void queue_set_lock_state(const std::string &device_id, bool locked) override {
    // The hub's lock encoding: locked is the "off" position, unlocked the "on" one.
    this->queued_.push_back({PendingOperationType::SET_LOCK_STATE, device_id, binary_position_(!locked)});
  }

  /// @return Everything the entities queued, oldest first.
  [[nodiscard]] const std::deque<PendingOperation> &queued_operations() const { return this->queued_; }

  // === Test controls ===

  /// Publish `dev` to the entity callbacks as if it had just arrived from the radio.
  /// @param device_id Device the update is for; must already be registered via add_device().
  /// @param dev Device state to publish.
  /// @param cache_device True to leave `dev` in the registry afterwards; false to publish it as a one-off and
  ///        restore the previously stored state, so a test can drive a sequence of updates without
  ///        disturbing the device it set up.
  void trigger_device_update(const std::string &device_id, const IoDevice &dev, bool cache_device = false) {
    const IoDevice *stored = this->registry_.get(device_id);
    // notify() publishes what the registry holds, so the update has to be stored to be seen.
    const bool was_registered = stored != nullptr;
    const IoDevice previous = was_registered ? *stored : IoDevice{};
    // The record is stored as given: put() keeps the stored reading store of the record it replaces, and the test
    // hands over the readings it wants shown.
    this->registry_.put(device_id, dev);
    this->registry_.get(device_id)->reading_values = dev.reading_values;
    this->registry_.notify(device_id);
    // Restore only when there was something to restore: putting back a default IoDevice for a device that
    // was never registered would leave a phantom with a zero node ID.
    if (!cache_device && was_registered) {
      this->registry_.put(device_id, previous);
      this->registry_.get(device_id)->reading_values = previous.reading_values;
    }
  }

  /// Publish what the registry holds for the device to the entity callbacks, as the hub does when a field
  /// the test set on the stored record changes.
  /// @param device_id Device whose stored record is published.
  void notify_device_update(const std::string &device_id) { this->registry_.notify(device_id); }

  /// Withdraw an optimistic prediction, as the hub does when the command it predicted fails.
  bool rollback_optimistic(const std::string &device_id) { return this->registry_.rollback_optimistic(device_id); }

  bool radio_stopped{false};                 ///< What radio_stopped_responding() returns.
  uint8_t hub_node_id[3]{0xC0, 0xFF, 0xEE};  ///< Named by describe_last_commander().

 protected:
  DeviceRegistry registry_;  ///< Production registry; derived doubles may read it directly.

 private:
  using IntervalMap = std::map<std::string, uint32_t>;

  static uint8_t binary_position_(bool on) { return on ? BINARY_ENTITY_ON_POSITION : BINARY_ENTITY_OFF_POSITION; }
  static uint32_t lookup_(const IntervalMap &map, const std::string &id) {
    const auto it = map.find(id);
    return it == map.end() ? 0 : it->second;
  }
  void record_interval_(IntervalMap &map, const std::string &device_id, uint32_t interval_ms) {
    if (this->registry_.get(device_id) == nullptr)
      return;
    map[device_id] = interval_ms;
  }

  IntervalMap status_poll_intervals_;
  IntervalMap status_refresh_intervals_;
  std::map<std::pair<std::string, DeviceReading>, uint32_t> reading_poll_intervals_;
  std::deque<PendingOperation> queued_;
};

/// @brief Double of HubHeatingPort: records writes and plans, answers reads from a script.
class HeatingPortDouble : public HubHeatingPort {
 public:
  /// One register write the entity asked for.
  struct Write {
    std::string device_id;
    DeviceRegister fn;
    float value;
  };

  bool send_heating_command(const std::string &device_id, DeviceRegister fn, float value) override {
    this->writes.push_back({device_id, fn, value});
    return this->write_result && this->refused_writes.count(fn) == 0;
  }

  bool read_heating_register(const std::string &, DeviceRegister fn, float *value_out, uint8_t max_tries) override {
    this->reads.push_back(fn);
    this->read_tries.push_back(max_tries);
    const auto reply = this->read_replies.find(fn);
    if (reply == this->read_replies.end())
      return false;
    if (value_out != nullptr)
      *value_out = reply->second;
    return true;
  }

  bool execute_heating_plan(const std::string &, const HeatingWritePlan &plan,
                            const std::function<void()> &on_accepted) override {
    this->plans.push_back(plan);
    if (!this->write_result)
      return false;
    if (on_accepted)
      on_accepted();
    return true;
  }

  bool read_programme_state(const std::string &device_id) override {
    this->programme_state_reads.push_back(device_id);
    return this->programme_state_answer;
  }

  void set_device_temperature_range(const std::string &, float min_c, float max_c) override {
    this->temperature_ranges.emplace_back(min_c, max_c);
  }

  void enable_device_reading(const std::string &device_id, DeviceReading reading) override {
    this->device_readings.emplace_back(device_id, reading);
  }

  [[nodiscard]] HeatingCardRange water_heater_target_range(const std::string &) const override {
    return this->water_heater_range;
  }
  [[nodiscard]] std::optional<float> water_heater_target_step(const std::string &) const override {
    return this->water_heater_step;
  }
  [[nodiscard]] const char *water_heater_mode_name(const std::string &, DhwMode mode) const override {
    return this->water_heater_mode_names != nullptr ? this->water_heater_mode_names(static_cast<uint8_t>(mode))
                                                    : nullptr;
  }
  bool set_water_heater_target(const std::string &, float celsius) override {
    this->water_heater_targets.push_back(celsius);
    return this->write_result;
  }
  bool set_water_heater_mode(const std::string &, DhwMode mode) override {
    this->water_heater_modes.push_back(mode);
    return this->write_result;
  }
  bool set_water_heater_boost(const std::string &, bool on) override {
    this->water_heater_boosts.push_back(on);
    return this->write_result;
  }
  bool set_water_heater_away(const std::string &, bool on) override {
    this->water_heater_aways.push_back(on);
    return this->write_result;
  }

  std::vector<Write> writes;                                           ///< Register writes, in order.
  std::vector<DeviceRegister> reads;                                   ///< Registers read, in order.
  std::vector<uint8_t> read_tries;                                     ///< The attempt budget of each read.
  std::vector<HeatingWritePlan> plans;                                 ///< Plans handed to execute_heating_plan().
  std::vector<std::string> programme_state_reads;                      ///< Devices passed to read_programme_state().
  std::vector<std::pair<float, float>> temperature_ranges;             ///< (min, max) pairs set.
  std::vector<std::pair<std::string, DeviceReading>> device_readings;  ///< (device, reading) asked to be read.
  std::vector<float> water_heater_targets;
  /// The range the card offers; the Calypso's, which is the range of its target row.
  HeatingCardRange water_heater_range{DHW_TARGET_MIN_C, DHW_TARGET_MAX_C};
  /// The step the card offers, from the heater's target row; none for a target that takes any value, as the Calypso's.
  std::optional<float> water_heater_step{std::nullopt};
  /// The names of the heater's mode row; the Calypso's.
  const char *(*water_heater_mode_names)(uint8_t value) = dhw_mode_name;
  std::vector<DhwMode> water_heater_modes;
  std::vector<bool> water_heater_boosts;
  std::vector<bool> water_heater_aways;
  bool programme_state_answer{true};             ///< What read_programme_state() answers: the heater said what it runs.
  bool write_result{true};                       ///< Whether the device accepts a write.
  std::set<DeviceRegister> refused_writes;       ///< Registers refused on top of write_result.
  std::map<DeviceRegister, float> read_replies;  ///< Value each register reads as; a missing one fails the read.
};

/// @brief Double of HubControlPort: records the calls and holds the registered callbacks.
class ControlPortDouble : public HubControlPort {
 public:
  void queue_discover_and_pair() override { ++this->discover_presses; }
  void trigger_scan_paired_devices() override { ++this->scan_presses; }
  void set_pairing_result_callback(std::function<void()> cb) override { this->pairing_result_callback = std::move(cb); }
  [[nodiscard]] std::string pairing_result_text() const override { return this->pairing_text; }

  void set_key_extraction_armed(bool armed) override { this->key_extraction_armed = armed; }
  void set_key_extraction_armed_callback(std::function<void(bool)> cb) override {
    this->key_extraction_armed_callback = std::move(cb);
  }
  void set_oneway_key_adoption_armed(bool armed) override { this->oneway_key_adoption_armed = armed; }
  void set_oneway_key_adoption_armed_callback(std::function<void(bool)> cb) override {
    this->oneway_key_adoption_armed_callback = std::move(cb);
  }

  uint32_t send_oneway_action(const std::string &controller_id, OneWayButtonAction action) override {
    this->oneway_actions.emplace_back(controller_id, action);
    return this->take_oneway_ticket_();
  }
  uint32_t send_oneway_position(const std::string &controller_id, uint8_t position) override {
    this->oneway_positions.emplace_back(controller_id, position);
    return this->take_oneway_ticket_();
  }
  /// The ticket of the oldest queued one-way operation that has not been reported yet (the real queue reports in
  /// order), or 0 when none waits: a command sent by something this double did not see.
  uint32_t next_unreported_oneway_ticket() {
    return this->reported_oneway_tickets < this->oneway_tickets.size()
               ? this->oneway_tickets[this->reported_oneway_tickets++]
               : 0;
  }
  void send_oneway_enroll(const std::string &controller_id) override {
    this->oneway_enrollments.push_back(controller_id);
  }
  void add_oneway_command_report_callback(OneWayCommandReportFn callback) override {
    this->oneway_report_callbacks.push_back(std::move(callback));
  }

  void add_oneway_press_callback(std::vector<std::string> senders, OneWayPressFn callback) override {
    this->oneway_press_subscriptions.push_back({std::move(senders), std::move(callback)});
  }
  [[nodiscard]] bool oneway_identity_inverted(const std::string &controller_id) const override {
    return this->inverted_oneway_identities.count(controller_id) != 0;
  }

  [[nodiscard]] const std::string &last_device_event() const override { return this->device_event; }
  void set_device_event_callback(std::function<void()> cb) override { this->device_event_callback = std::move(cb); }

#ifdef IOHOME_LR1121_FIRMWARE_UPDATE
  void trigger_lr1121_firmware_update() override { ++this->lr1121_updates; }
#ifdef IOHOME_LR1121_BOOTLOADER_UPDATE
  void set_bootloader_rewrite_allowed(bool allowed) override { this->bootloader_rewrite_allowed = allowed; }
#endif
#endif

  /// Deliver a remote's press to the subscribers that listed its sender, as the hub does after its duplicate filter.
  void press_oneway_remote(const std::string &sender, const OneWayFrameInfo &press) const {
    for (const auto &subscription : this->oneway_press_subscriptions) {
      for (const auto &listed : subscription.senders) {
        if (listed == sender)
          subscription.callback(press);
      }
    }
  }

  /// Deliver a one-way report to every subscriber, as the hub does after each command attempt.
  void report_oneway_command(const OneWayCommandReport &report) const {
    for (const auto &callback : this->oneway_report_callbacks)
      callback(report);
  }

  int discover_presses{0};
  int scan_presses{0};
  int lr1121_updates{0};
  bool key_extraction_armed{false};
  bool oneway_key_adoption_armed{false};
  bool bootloader_rewrite_allowed{false};
  std::string pairing_text;  ///< What pairing_result_text() returns.
  std::string device_event;  ///< What last_device_event() returns.
  std::function<void()> pairing_result_callback;
  std::function<void()> device_event_callback;
  std::function<void(bool)> key_extraction_armed_callback;
  std::function<void(bool)> oneway_key_adoption_armed_callback;
  /// A ticket that is never 0 and differs per queued operation, as the real queue hands out.
  uint32_t take_oneway_ticket_() {
    this->oneway_tickets.push_back(static_cast<uint32_t>(this->oneway_tickets.size()) + 1U);
    return this->oneway_tickets.back();
  }
  std::vector<uint32_t> oneway_tickets;  ///< Ticket handed out for each queued action or position, in order.
  size_t reported_oneway_tickets{0};     ///< How many of oneway_tickets next_unreported_oneway_ticket() has given out.
  std::vector<std::pair<std::string, OneWayButtonAction>> oneway_actions;  ///< (controller, action), in order.
  std::vector<std::pair<std::string, uint8_t>> oneway_positions;  ///< (controller, position percent), in order.
  std::vector<std::string> oneway_enrollments;
  std::vector<OneWayCommandReportFn> oneway_report_callbacks;
  /// One press subscription: the senders it listed and its callback.
  struct OnewayPressSubscription {
    std::vector<std::string> senders;
    OneWayPressFn callback;
  };
  std::vector<OnewayPressSubscription> oneway_press_subscriptions;
  std::set<std::string> inverted_oneway_identities;  ///< Handles oneway_identity_inverted() answers true for.
};

}  // namespace test

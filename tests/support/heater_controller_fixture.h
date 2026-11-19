#pragma once

/// @file heater_controller_fixture.h
/// @brief Test double for the hub side of HeaterController: a real engine on a MockRadio, recording fakes.
///
/// HeaterControllerFixture owns a MockRadio, a real ExchangeEngine, a DeviceRegistry and one HeaterController,
/// and wires the controller's injected hub calls to itself:
///   - `exchange` runs ExchangeEngine::send_and_receive() on FREQ_CH2 with the given `max_tries`, so the
///     bytes sent are the ones the hub sends; it reports a reply only for a frame that is not CMD_ERROR_RESP.
///     The hub's bookkeeping (counters, result codes, device event reports) is not part of this double: a
///     test that needs it goes through the hub's forwarders.
///   - `execute` applies the operation's guard, builds the request and records it; it does not transmit.
///   - `status_read` records the request and answers with a configured result; it decodes nothing, except that a
///     configured `status_reply_level` is stored as the heating level the status decoder would store.
///   - `poll_backoff` records the call and moves the real StatusPollPolicy onto its retry ladder, as the hub does.
///   - the timer and the queued status request are recorded.
/// A test needs no hub object.

#include "device_registry.h"
#include "exchange_engine.h"
#include "heater_controller.h"
#include "proto_commands.h"
#include "proto_frame.h"
#include "status_poll_policy.h"
#include "tuning_config.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"

#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace heater_controller_test {

using namespace esphome::home_io_control;

/// One call of the injected `execute`.
struct RecordedExecute {
  std::string device_id;
  std::string action;
  std::string rejection_profile;
  HeaterExecuteKind kind;
  bool built;       ///< The guard accepted the device and the builder produced a frame.
  IoFrame request;  ///< The frame the builder produced (valid when `built`).
};

/// One call of the injected `status_read`.
struct RecordedStatusRead {
  std::string device_id;
  IoFrame request;
  uint32_t retry_after_fail_ms;
  uint8_t max_tries;
};

/// One call of the injected `set_timeout_by_id`.
struct RecordedTimer {
  uint32_t id;
  uint32_t delay_ms;
  std::function<void()> callback;
};

/// Hub-free host for a HeaterController; see the file comment.
class HeaterControllerFixture {
 public:
  HeaterControllerFixture()
      : radio_ptr(&radio),
        engine(&radio_ptr, node_id, system_key, &tuning),
        controller(node_id, &initialized, &radio_ptr, registry, poll_policy, engine, make_hub()) {}

  HeaterControllerFixture(const HeaterControllerFixture &) = delete;
  HeaterControllerFixture &operator=(const HeaterControllerFixture &) = delete;

  /// Register "ABC123" as an entity with `config` would (the registry resolves the features from the
  /// declared type and entity class itself).
  /// @return The stored record.
  IoDevice *add_device(const DeviceConfig &config = {}) {
    registry.add("ABC123", config);
    return registry.get("ABC123");
  }

  /// Register "ABC123" and give it a kind, as the INFO2 path does.
  /// @return The stored record.
  IoDevice *add_device_of_kind(DeviceType type, uint8_t subtype = 0) {
    IoDevice *dev = add_device();
    if (dev != nullptr)
      test::set_kind(*dev, type, subtype);
    return dev;
  }

  /// Queue a frame from the device 0xABC123 to the hub as the radio's next reception.
  void queue_reply(uint8_t cmd, const std::vector<uint8_t> &payload) {
    IoFrame frame{};
    init_frame(frame, true, false, true, false);
    const uint8_t device_node_id[3] = {0xAB, 0xC1, 0x23};
    set_dst(frame, node_id);
    set_src(frame, device_node_id);
    set_cmd(frame, cmd, payload.data(), payload.size());
    radio.queue_rx(test::make_rx_packet(frame));
  }

  /// Queue a CMD_WRITE_PRIVATE_ACK carrying `payload` (the echo of a register and its value bytes).
  void queue_register_ack(const std::vector<uint8_t> &payload) { queue_reply(CMD_WRITE_PRIVATE_ACK, payload); }

  // Declaration order is initialisation order: the engine needs the radio pointer, the controller needs both.
  MockRadio radio;
  RadioDriver *radio_ptr;
  TuningConfig tuning{};
  uint8_t node_id[NODE_ID_SIZE] = {0xC0, 0xFF, 0xEE};
  uint8_t system_key[AES_KEY_SIZE] = {0xD1, 0x74, 0x34, 0x93, 0xFA, 0x94, 0x38, 0x45,
                                      0xAC, 0x43, 0x50, 0xEE, 0xFF, 0x34, 0x29, 0x34};
  bool initialized{true};
  DeviceRegistry registry;
  StatusPollPolicy poll_policy;
  ExchangeEngine engine;
  HeaterController controller;

  bool execute_result{true};      ///< What the fake `execute` answers when it built a frame.
  bool status_read_result{true};  ///< What the fake `status_read` answers.
  /// The heating level the device's reply to a status read carries, stored on its record as the hub's status
  /// decoder does; -1 (the default): the read gets no level, and the record is left as it was.
  int16_t status_reply_level{-1};
  std::vector<RecordedExecute> executes;
  std::vector<RecordedStatusRead> status_reads;
  std::vector<RecordedTimer> timers;
  std::vector<std::pair<std::string, bool>> backoffs;  ///< The `poll_backoff` calls: device and auth_like.
  std::vector<std::string> queued_status_requests;

 private:
  HeaterControllerHub make_hub() {
    HeaterControllerHub hub;
    hub.exchange = [this](const std::string &, const IoFrame &request, IoFrame &response, uint8_t max_tries) {
      const ExchangeOutcome outcome = engine.send_and_receive(request, response, FREQ_CH2, max_tries);
      return outcome == ExchangeOutcome::SUCCESS_WITH_RESPONSE && response.cmd != CMD_ERROR_RESP;
    };
    hub.execute = [this](const std::string &device_id, const char *action, const HeaterExecuteAcceptsFn &accepts,
                         const char *rejection_profile, const HeaterExecuteBuildFn &build, HeaterExecuteKind kind) {
      RecordedExecute call{device_id, action, rejection_profile, kind, false, IoFrame{}};
      const IoDevice *dev = registry.get(device_id);
      if (dev != nullptr && accepts(*dev))
        call.built = build(call.request, *dev);
      executes.push_back(call);
      return call.built && execute_result;
    };
    hub.status_read = [this](const std::string &device_id, const IoFrame &request, uint32_t retry_after_fail_ms,
                             uint8_t max_tries) {
      status_reads.push_back({device_id, request, retry_after_fail_ms, max_tries});
      IoDevice *dev = registry.get(device_id);
      if (dev != nullptr && status_reply_level >= 0)
        dev->heating.level = status_reply_level;
      return status_read_result;
    };
    hub.poll_backoff = [this](const std::string &device_id, bool auth_like) {
      backoffs.emplace_back(device_id, auth_like);
      poll_policy.on_exchange_failed(device_id, auth_like, esphome::millis(), false);
    };
    hub.set_timeout_by_id = [this](uint32_t id, uint32_t delay_ms, std::function<void()> callback) {
      timers.push_back({id, delay_ms, std::move(callback)});
    };
    hub.queue_status_request = [this](const std::string &device_id) { queued_status_requests.push_back(device_id); };
    return hub;
  }
};

}  // namespace heater_controller_test

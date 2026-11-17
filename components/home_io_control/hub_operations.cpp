#include "hub_internal.h"

#include "decisions_exchange.h"
#include "decisions_attempts.h"
#include "decisions_status.h"
#include "proto_commands.h"
#include "proto_device_kinds.h"
#include "proto_device_types.h"

#include <cmath>
#include <algorithm>
#include <cinttypes>
#include <cstdio>

/// @file hub_operations.cpp
/// @brief High-level command execution and queued operation dispatch.
/// @ingroup hioc_hub
///
/// This file owns the outbound user-facing operations on the hub:
/// - cover position and tilt commands,
/// - explicit status requests,
/// - light/switch semantic wrappers,
/// - queued dispatch on the main loop.
///
/// Keeping these methods out of hub_core.cpp makes it easier to reason about the
/// difference between lifecycle/polling logic and the explicit actions initiated
/// by Home Assistant entities.

namespace esphome {
namespace home_io_control {

namespace {

/// Stack-buffer size for a pre-formatted execute action phrase such as
/// "position=100%% tilt=100%%".
constexpr size_t EXECUTE_ACTION_BUF_SIZE = 40;

/// Wire-scale "fully open" position for a FORCE_OPEN. A normal actuator reads fully open as 0;
/// an IoDevice::inverted actuator (a horizontal awning, say) reads it as 100.
constexpr uint8_t FORCE_OPEN_WIRE_POSITION = 0;
constexpr uint8_t FORCE_OPEN_WIRE_POSITION_INVERTED = 100;

/// Does a position or force-open EXECUTE to this device carry the speed block (the 8-byte form)?
/// Only when the kind may take it and a speed was chosen: without a choice the frame is the 6-byte
/// main-parameter form a wall pad sends, which leaves the speed to the motor.
bool sends_speed_block(const IoDevice &d) {
  return has_feature(d.features, device_feature::EXTENDED_POSITION_FRAME) &&
         d.travel_speed != TravelSpeed::MOTOR_DEFAULT;
}

/// @brief Return the human-readable verb for a position-style command.
/// @param dev Device receiving the command.
/// @param position Requested execute position.
/// @return Log-friendly action string such as "open", "turn on", or "lock".
const char *position_command_action(const IoDevice &dev, uint8_t position) {
  if (position == POS_STOP)
    return "stop";

  if (!detail::is_binary_entity_position(position))
    return "set position";

  bool const active_state = position == BINARY_ENTITY_ON_POSITION;
  switch (device_capability_class(dev.type, dev.subtype)) {
    case DeviceCapabilityClass::LIGHT:
    case DeviceCapabilityClass::SWITCH:
    case DeviceCapabilityClass::HEATER_LEVEL:
      return active_state ? "turn on" : "turn off";
    case DeviceCapabilityClass::LOCK:
      return active_state ? "unlock" : "lock";
    case DeviceCapabilityClass::UNKNOWN:
    case DeviceCapabilityClass::COVER:
    case DeviceCapabilityClass::CLIMATE:
    case DeviceCapabilityClass::SENSOR:
    case DeviceCapabilityClass::BEACON:
    default:
      return active_state ? "open" : "close";
  }
}

/// @brief Return the effective profile label for a device's "Sending ... (profile=...)" logs.
/// @param dev Device receiving the command.
/// @return "dimmable_light" for a LIGHT-class device with device_feature::LEVELS (the YAML `dimmable:`
///         choice, which the wire protocol has no signal for); device_operation_profile_name() for
///         every other device.
const char *operation_profile_name(const IoDevice &dev) {
  if (has_feature(dev.features, device_feature::LEVELS) &&
      device_capability_class(dev.type, dev.subtype) == DeviceCapabilityClass::LIGHT)
    return "dimmable_light";
  return device_operation_profile_name(dev.type, dev.subtype);
}

/// @brief Return the accepted entity/profile label for rejected execute-position logs.
/// @param dev Device the command was rejected for.
/// @param position Requested execute position.
/// @return Expected profile label for detail::log_rejected_operation(); names the missing feature
///         when the device only lacks intermediate positions.
const char *position_rejection_profile(const IoDevice &dev, uint8_t position) {
  // A LIGHT-class device only reaches rejection for a genuinely out-of-range value or, without
  // `dimmable:`, an intermediate one — the fix there isn't "needs cover_position", it's "needs a
  // value in 0-100" (or `dimmable: true`).
  DeviceCapabilityClass const capability_class = device_capability_class(dev.type, dev.subtype);
  if (capability_class == DeviceCapabilityClass::LIGHT || capability_class == DeviceCapabilityClass::HEATER_LEVEL)
    return "0-100";
  if (!has_feature(dev.features, device_feature::LEVELS) && position != POS_STOP &&
      !detail::is_binary_entity_position(position))
    return "cover with intermediate positions (LEVELS feature)";
  return detail::is_binary_entity_position(position) ? "cover_position or binary_on_off" : "cover_position";
}

/// @brief The queue-time capability guard for one family of queued operation.
///
/// Every `queue_*` method applies the same early-reject as its execute-time counterpart, for fast
/// user feedback, with a "queued ..." rejection noun. This table is the one place those pairings
/// are recorded, so a deliberate asymmetry is a visible row rather than an accident. The lone
/// `queue_*` method with a guard that is NOT a row here is `queue_device_command`: it returns
/// bool and uses the command name as its rejection noun, so it keeps its guard inline (see there).
/// NOTE the asymmetry `queue_set_device_position` carries a COVER guard that `set_device_position`
/// deliberately does *not* — light/switch/lock all funnel through `set_device_position`, so an
/// entity-class guard there would break them; at queue time each entity has its own method.
struct QueueGuard {
  bool (*accepts)(const IoDevice &dev);  ///< false → reject this operation for this device.
  const char *rejection_noun;            ///< e.g. "queued cover command".
  const char *expected;                  ///< e.g. "cover entity".
};

constexpr QueueGuard QUEUE_GUARD_COVER{
    [](const IoDevice &d) { return detail::known_device_matches_entity_class(d, DeviceCapabilityClass::COVER); },
    "queued cover command", "cover entity"};
constexpr QueueGuard QUEUE_GUARD_TILT{[](const IoDevice &d) { return detail::known_device_accepts_execute_tilt(d); },
                                      "queued tilt command", "tilt-capable cover"};
constexpr QueueGuard QUEUE_GUARD_POSITION_AND_TILT{
    [](const IoDevice &d) { return detail::known_device_accepts_execute_tilt(d); }, "queued position+tilt command",
    "tilt-capable cover"};
constexpr QueueGuard QUEUE_GUARD_LIGHT{
    [](const IoDevice &d) { return detail::known_device_matches_entity_class(d, DeviceCapabilityClass::LIGHT); },
    "queued light command", "light entity"};
constexpr QueueGuard QUEUE_GUARD_FAN{
    [](const IoDevice &d) { return detail::known_device_matches_entity_class(d, DeviceCapabilityClass::HEATER_LEVEL); },
    "queued fan command", "exterior heating (fan entity)"};
constexpr QueueGuard QUEUE_GUARD_LOCK{
    [](const IoDevice &d) { return detail::known_device_matches_entity_class(d, DeviceCapabilityClass::LOCK); },
    "queued lock command", "lock entity"};
constexpr QueueGuard QUEUE_GUARD_SWITCH{
    [](const IoDevice &d) { return detail::known_device_matches_entity_class(d, DeviceCapabilityClass::SWITCH); },
    "queued switch command", "switch entity"};
constexpr QueueGuard QUEUE_GUARD_STATUS{
    [](const IoDevice &d) { return detail::known_device_supports_status_requests(d); }, "queued status request",
    "status-capable actuator"};

/// @brief Apply one QueueGuard. Returns true (and logs) when the operation must be rejected.
///
/// Matches the historical guard exactly: an unregistered/unknown device (dev == nullptr) is *not*
/// rejected here — it passes through so discovery and imported devices keep working.
bool queue_guard_rejects(IOHomeControlComponent *hub, const std::string &device_id, const QueueGuard &guard) {
  const IoDevice *dev = hub->get_device(device_id);
  if (dev != nullptr && !guard.accepts(*dev)) {
    detail::log_rejected_operation(device_id, *dev, guard.rejection_noun, guard.expected);
    return true;
  }
  return false;
}

}  // namespace

void IOHomeControlComponent::arm_execute_confirmation_poll_(const std::string &device_id, bool for_stop) {
  uint32_t const existing = this->poll_policy_.get_next_update(device_id);
  uint32_t const delay_ms = settle_delay_ms(this->poll_policy_.get_interval(device_id), 0, for_stop);
  this->begin_status_poll_tracking_(device_id, delay_ms);
  if (existing != 0 && existing < millis() + delay_ms)
    this->poll_policy_.set_next_update(device_id, existing);
  if (for_stop)
    this->poll_policy_.mark_stop_settle(device_id);
}

// A device event report (decisions::is_device_event_report(): a 0x70, or a 0x71 carrying a
// subscribed event) from the very device we asked can arrive while we listen for the reply, and
// classify_exchange_first_response() accepts any endpoint-matching frame as that reply. It answers
// none of our requests, so it is recorded and never applied. The device is awake and transmitting,
// so the request is asked once more rather than counted as a timeout (which would start the poll
// backoff for a device that did answer, just with something else). An EXECUTE that is not safe to
// repeat is not sent again: it ends as unanswered, like any lost exchange. The one event report that
// does answer the request is a subscription's answer (decisions::is_subscription_answer()): it is the
// reading the sensor polls ask for, so it is returned as the reply.
ExchangeOutcome IOHomeControlComponent::exchange_past_device_event_(const IoFrame &request, IoFrame &response,
                                                                    const decisions::AttemptRequest &attempts) {
  ExchangeOutcome outcome = this->send_and_receive_(request, response, FREQ_CH2, attempts);
  if (outcome != ExchangeOutcome::SUCCESS_WITH_RESPONSE || !decisions::is_device_event_report(response) ||
      decisions::is_subscription_answer(request, response))
    return outcome;
  this->record_device_event_(response);
  if (request.cmd == CMD_EXECUTE && !decisions::is_repeatable_execute(request))
    return ExchangeOutcome::FAILED;
  outcome = this->send_and_receive_(request, response, FREQ_CH2, attempts);
  if (outcome != ExchangeOutcome::SUCCESS_WITH_RESPONSE || !decisions::is_device_event_report(response) ||
      decisions::is_subscription_answer(request, response))
    return outcome;
  this->record_device_event_(response);
  return ExchangeOutcome::FAILED;
}

// Run an authenticated request on the standard command channel and do the per-outcome bookkeeping
// every caller shares; what the reply means is left to the caller.
IOHomeControlComponent::RequestResult IOHomeControlComponent::exchange_and_record_(const std::string &device_id,
                                                                                   const IoFrame &request,
                                                                                   IoFrame &response,
                                                                                   const RequestOptions &options) {
  const bool warn_on_no_response = options.warn_on_no_response;
  const uint32_t retry_after_fail_ms = options.retry_after_fail_ms;
  const ExchangeOutcome outcome = this->exchange_past_device_event_(request, response, options.attempts);
  // An unconfirmed acceptance means the device authenticated the request but never closed the
  // exchange. Whether that counts as success depends entirely on what the request was *for*:
  //   - a command (CMD_EXECUTE) is treated as done. The device has it, and most devices that end
  //     this way report the result in their own status update a few seconds later. It is not proof
  //     the command was carried out, though, so the engine has already sent it once more where the
  //     silence was an anomaly (decisions::retry_after_unconfirmed_accept_is_safe()), and the
  //     confirmation poll corrects the state if the device did not act;
  //   - a status poll or a name read exists to obtain a payload. Getting none means the question
  //     went unanswered, so it stays a failure and takes the auth-shaped poll backoff: a quick
  //     retry the first time, a long one when it repeats (StatusPollPolicy::retry_delay_ms()); a
  //     poll of a moving device skips its first misses at the normal cadence first.
  const bool unconfirmed_counts_as_success = request.cmd == CMD_EXECUTE;

  // Recorded once, here, for every outcome — deliberately above the split below, because the rule
  // is about the outcome and not about how this request chooses to classify it. The two branches
  // disagree on whether an unconfirmed acceptance is a failure; they must not disagree on whether it
  // happened. The unconfirmed count is the only record of the CMD_EXECUTE case, which reports
  // success, and it is what separates "the device never heard us" from "it heard us and the reply
  // was lost" in the branch that does count as a failure.
  if (IoDevice *dev = this->registry_.get(device_id); dev != nullptr)
    detail::record_exchange_outcome(*dev, request.cmd, outcome);

  if (outcome == ExchangeOutcome::FAILED ||
      (outcome == ExchangeOutcome::SUCCESS_UNCONFIRMED && !unconfirmed_counts_as_success)) {
    const auto &dbg = this->exchange_engine_.get_debug();
    if (IoDevice *dev = this->registry_.get(device_id); dev != nullptr) {
      detail::record_exchange_timeout(*dev, dbg.tries);
      // A 0x3C challenge seen during the exchange is itself a frame from this device: the exchange
      // still failed (our challenge-response or the device's final reply was lost), but the device
      // demonstrably transmitted. Stamp link health the same way the explicit-refusal branch does,
      // or a device that challenges every attempt yet never closes the exchange ages exactly like
      // a powered-off one.
      if (dbg.saw_challenge)
        detail::update_link_health(*dev, this->radio_);
      this->notify_device_update_(device_id);
    }
    if (retry_after_fail_ms != 0) {
      // A single-try poll of a device the hub believes is travelling lost one frame: it keeps the
      // normal cadence before the backoff ladder starts (MOVING_POLL_MISSES_SKIPPED). A device that
      // reported STOPPED off its target and awaits the confirming poll is at rest, not travelling,
      // and a poll that already had several tries keeps the ladder.
      const IoDevice *dev = this->registry_.get(device_id);
      const bool device_moving = request.cmd == CMD_PRIVATE && options.attempts.is_progress_look() && dev != nullptr &&
                                 !effective_is_stopped(*dev) && !detail::is_settling(*dev) &&
                                 !device_has_no_motion(dev->type, dev->subtype);
      this->schedule_background_poll_backoff_(device_id, dbg.saw_challenge, device_moving);
    }
    this->log_exchange_debug_(device_id.c_str());
    if (warn_on_no_response) {
      ESP_LOGW(detail::TAG, "Command 0x%02X failed for device %s: no valid response (stage=%s tries=%u)", request.cmd,
               device_id.c_str(), dbg.stage, dbg.tries);
    }
    return RequestResult::FAILED;
  }

  if (outcome == ExchangeOutcome::SUCCESS_UNCONFIRMED) {
    // The device challenged the request, so it received it; whether it also received our challenge
    // answer — and therefore acted — is not observable from here (see the WAIT_FINAL_RESPONSE
    // branch in ExchangeEngine::send_and_receive()). There is no frame to parse, and inventing a
    // position from a request we only know was *challenged* would be worse than leaving the last
    // known state alone — the device's own asynchronous status update supplies the real one, and
    // that path authenticates now. Clear the failure streaks: this was not a failure.
    //
    // Log the snapshot anyway. This branch is the one exchange ending that prints nothing at all
    // otherwise, so a device that routinely stops replying here leaves no trace to diagnose, and
    // its final_rx_* fields (did the radio receive anything during the final wait?) are what
    // separates a lost answer from a lost reply. Its cap_* fields describe the device's challenge,
    // not that wait.
    this->log_exchange_unconfirmed_debug_(device_id.c_str());
    if (retry_after_fail_ms != 0)
      this->poll_policy_.clear_failure_streaks(device_id);
    if (IoDevice *dev = this->registry_.get(device_id); dev != nullptr) {
      detail::update_link_health(*dev, this->radio_);
      this->notify_device_update_(device_id);
    }
    return RequestResult::UNCONFIRMED;
  }

  if (response.cmd == CMD_ERROR_RESP) {
    this->handle_error_response_(device_id, request, response, retry_after_fail_ms);
    return RequestResult::ERROR_REPLY;
  }

  if (retry_after_fail_ms != 0)
    this->poll_policy_.clear_failure_streaks(device_id);
  return RequestResult::REPLY;
}

// Execute an authenticated request and, on a reply, feed it back through the normal inbound status
// parser so all state normalization stays in one place.
bool IOHomeControlComponent::execute_request_and_update_(const std::string &device_id, const IoFrame &request,
                                                         bool warn_on_no_response, uint32_t retry_after_fail_ms,
                                                         const decisions::AttemptRequest &attempts) {
  IoFrame response;
  switch (
      this->exchange_and_record_(device_id, request, response, {warn_on_no_response, retry_after_fail_ms, attempts})) {
    case RequestResult::FAILED:
    case RequestResult::ERROR_REPLY:
      return false;
    case RequestResult::UNCONFIRMED:
      return true;
    case RequestResult::REPLY:
      break;
  }

  // The immediate reply to our own CMD_EXECUTE (position/tilt/stop/favorite/vent) is not
  // trustworthy for target/current position on at least some devices — see
  // update_device_status_()'s trust_position doc comment. Every other request we send
  // (status poll, get name, ...) keeps trusting its reply as before.
  this->update_device_status_(response, request.cmd != CMD_EXECUTE);
  if (request.cmd == CMD_EXECUTE && response.data_len > 0) {
    if (IoDevice *dev = this->registry_.get(device_id); dev != nullptr)
      detail::log_execute_result_code(*dev, device_id, request, response.data[0]);
  }
  return true;
}

bool IOHomeControlComponent::handle_error_response_(const std::string &device_id, const IoFrame &request,
                                                    const IoFrame &response, uint32_t retry_after_fail_ms) {
  IoDevice *dev = this->registry_.get(device_id);
  // An explicit refusal is still a reply from the device, but no caller feeds it to
  // update_device_status_(), so stamp link health here to keep update_link_health()'s "every frame
  // from a registered device" contract.
  if (dev != nullptr)
    detail::update_link_health(*dev, this->radio_);
  if (response.data_len == 0) {
    detail::log_frame_issue(this, this->node_id_, "rx", "unsupported_payload", response, frame_length(response));
  } else if (dev != nullptr) {
    detail::record_command_result(*dev, device_id, response.data[0], request.cmd, true);
  } else {
    detail::log_command_result(device_id, response.data[0], request.cmd, true);
  }
  if (dev != nullptr)
    this->notify_device_update_(device_id);
  if (retry_after_fail_ms != 0)
    this->schedule_background_poll_backoff_(device_id, this->exchange_engine_.get_debug().saw_challenge);
  return false;
}

bool IOHomeControlComponent::run_execute_operation_(const std::string &device_id, const ExecuteRequestSpec &spec,
                                                    const std::function<bool(const IoDevice &)> &accepts,
                                                    const char *rejection_profile,
                                                    const std::function<bool(IoFrame &, const IoDevice &)> &build) {
  // Every false return means this command will not happen: an unregistered or not-yet-initialized
  // device, a profile guard rejection, a builder failure, or an exchange that ended with no valid
  // response or an explicit CMD_ERROR_RESP. In all of them the prediction the entity applied at
  // control() time must be withdrawn, or the Home Assistant cover animates a movement that is not
  // occurring — indefinitely, since only a frame from the device can settle it.
  const bool accepted = this->try_execute_operation_(device_id, spec, accepts, rejection_profile, build);
  if (accepted && spec.settle_as_stop) {
    this->registry_.confirm_optimistic_stop(device_id);
    // The moving evidence is left to the STOP's own ack (apply_private_response_status()): a motor
    // that reports "stopped" clears it, one still winding down keeps it, because right after a STOP
    // a VELUX SSL can still be in the travelling state that hears only the short preamble. An ack
    // with no status to decode leaves the evidence as it was; a settle poll that draws nothing spends it.
  } else if (accepted) {
    // An accepted movement command means the receiver is travelling now, whether or not the reply
    // carried a status to decode (ExchangeOutcome::SUCCESS_UNCONFIRMED has none). Stamped here,
    // after the exchange, and not when the optimistic overlay is applied before it: see
    // note_moving_evidence().
    if (IoDevice *dev = this->registry_.get(device_id); dev != nullptr)
      note_moving_evidence(*dev, millis());
  } else {
    this->registry_.rollback_optimistic(device_id, /*failed_stop=*/spec.settle_as_stop);
  }
  return accepted;
}

bool IOHomeControlComponent::try_execute_operation_(const std::string &device_id, const ExecuteRequestSpec &spec,
                                                    const std::function<bool(const IoDevice &)> &accepts,
                                                    const char *rejection_profile,
                                                    const std::function<bool(IoFrame &, const IoDevice &)> &build) {
  auto *dev = this->get_device(device_id);
  if (dev == nullptr || !this->initialized_)
    return false;

  // Once a device family is known, use the profile helpers to reject YAML/entity mismatches
  // before they hit the radio path. Unknown types still pass through so discovery and imported
  // devices keep working as before.
  if (!accepts(*dev)) {
    detail::log_rejected_operation(device_id, *dev, spec.action, rejection_profile);
    return false;
  }

  this->begin_status_poll_tracking_(device_id, this->poll_policy_.get_interval(device_id));

  ESP_LOGI(detail::TAG, "Sending %s to device %s (profile=%s)", spec.action, device_id.c_str(),
           operation_profile_name(*dev));

  IoFrame request;
  if (!build(request, *dev)) {
    this->poll_policy_.clear(device_id);
    return false;
  }
  // A command whose copies have one effect takes the command plan (as many attempts as fit the
  // budget to a mains device); a stored-position or toggle-like command keeps today's three.
  const decisions::AttemptRequest attempts = decisions::is_repeat_safe_command(request)
                                                 ? decisions::AttemptRequest::of_kind(decisions::ReadKind::COMMAND)
                                                 : decisions::AttemptRequest{};
  if (!this->execute_request_and_update_(device_id, request, true, 0, attempts)) {
    this->schedule_background_poll_backoff_(device_id, this->exchange_engine_.get_debug().saw_challenge);
    return false;
  }
  this->arm_execute_confirmation_poll_(device_id, spec.settle_as_stop);
  return true;
}

bool IOHomeControlComponent::set_device_position(const std::string &device_id, uint8_t position) {
  const auto *dev = this->get_device(device_id);
  if (dev == nullptr)
    return false;
  // action and rejection profile depend on this device's class and the requested value, so
  // resolve them here where dev is in scope; run_execute_operation_() re-checks dev/initialized_.
  return this->run_execute_operation_(
      device_id, {position_command_action(*dev, position), position == POS_STOP},
      [position](const IoDevice &d) { return detail::known_device_accepts_execute_position(d, position); },
      position_rejection_profile(*dev, position),
      [this, position](IoFrame &request, const IoDevice &d) {
        if (!sends_speed_block(d))
          return create_execute_main_parameter(request, this->node_id_, d.node_id, d.low_power, position);
        return create_execute_position(request, this->node_id_, d.node_id, d.low_power, position,
                                       d.travel_speed == TravelSpeed::SILENT);
      });
}

bool IOHomeControlComponent::execute_device_command_(const std::string &device_id, CoverCommand cmd) {
  // A named position (favourite, ventilation) goes out only to a device whose features include its
  // bit; a motor without it would answer PARAMETER_UNKNOWN (issue #163), so the command is refused
  // here, through the same rejection and rollback path as a position.
  const NamedPosition *named = find_named_position(cmd);
  char expected[EXECUTE_ACTION_BUF_SIZE] = "cover entity";
  if (named != nullptr)
    snprintf(expected, sizeof(expected), "cover with %s position", named->name);
  return this->run_execute_operation_(
      device_id, {cover_command_name(cmd), cmd == CoverCommand::STOP},
      [named](const IoDevice &d) {
        return detail::known_device_matches_entity_class(d, DeviceCapabilityClass::COVER) &&
               (named == nullptr || device_accepts_main_parameter(d.features, named->main_parameter));
      },
      expected,
      [this, cmd](IoFrame &request, const IoDevice &d) {
        // FORCE_OPEN needs the device's own wire-scale "fully open" position (0, or 100 for an
        // IoDevice::inverted device such as a horizontal awning) — create_execute_command() has no
        // device access to resolve that, so it is built separately here where d is in scope. The
        // silent profile is a functional parameter, so only a device with the extended frame gets it.
        if (cmd != CoverCommand::FORCE_OPEN) {
          return create_execute_command(request, this->node_id_, d.node_id, d.low_power, cmd,
                                        d.travel_speed == TravelSpeed::SILENT &&
                                            has_feature(d.features, device_feature::EXTENDED_POSITION_FRAME));
        }
        const uint8_t open_position = d.inverted ? FORCE_OPEN_WIRE_POSITION_INVERTED : FORCE_OPEN_WIRE_POSITION;
        return sends_speed_block(d)
                   ? create_force_open(request, this->node_id_, d.node_id, d.low_power, open_position)
                   : create_execute_main_parameter(request, this->node_id_, d.node_id, d.low_power, open_position,
                                                   /*force_open_priority=*/true);
      });
}

bool IOHomeControlComponent::set_device_tilt(const std::string &device_id, uint8_t tilt_percent) {
  char action[EXECUTE_ACTION_BUF_SIZE];
  snprintf(action, sizeof(action), "tilt=%u%%", tilt_percent);
  return this->run_execute_operation_(
      device_id, {action, false}, [](const IoDevice &d) { return detail::known_device_accepts_execute_tilt(d); },
      "tilt-capable cover",
      [this, tilt_percent](IoFrame &request, const IoDevice &d) {
        return create_execute_tilt(request, this->node_id_, d.node_id, d.low_power, tilt_percent);
      });
}

bool IOHomeControlComponent::set_device_position_and_tilt(const std::string &device_id, uint8_t position,
                                                          uint8_t tilt_percent) {
  char action[EXECUTE_ACTION_BUF_SIZE];
  snprintf(action, sizeof(action), "position=%u%% tilt=%u%%", position, tilt_percent);
  return this->run_execute_operation_(
      device_id, {action, false}, [](const IoDevice &d) { return detail::known_device_accepts_execute_tilt(d); },
      "tilt-capable cover",
      [this, position, tilt_percent](IoFrame &request, const IoDevice &d) {
        return create_execute_position_and_tilt(request, this->node_id_, d.node_id, d.low_power, position,
                                                tilt_percent);
      });
}

// A cover's status poll is a settling read (several attempts inside the exchange budget) when it
// decides where the cover ended up: after an accepted STOP, confirming a stop off target, in the
// ladder's retry band, or with no ladder behind it (a one-off poll, the opt-in status refresh).
// Every other poll the scheduler owns only looks at a device that is probably still busy: one
// attempt, the backoff ladder is its retry.
decisions::AttemptRequest IOHomeControlComponent::cover_poll_attempts_(const std::string &device_id,
                                                                       bool scheduler_managed, bool state_unknown) {
  if (!scheduler_managed) {
    const auto attempts = decisions::AttemptRequest::of_kind(decisions::ReadKind::SETTLING);
    return state_unknown ? attempts.with_state_unknown() : attempts;
  }
  // Asked before take_settling_read(), which consumes the mark.
  const bool predicted_end = this->poll_policy_.is_predicted_end_read(device_id);
  const decisions::AttemptRequest attempts = decisions::AttemptRequest::of_kind(decisions::scheduled_poll_read_kind(
      this->poll_policy_.get_status_poll_failures(device_id), this->poll_policy_.get_auth_poll_failures(device_id),
      this->poll_policy_.take_settling_read(device_id)));
  return predicted_end ? attempts.with_wake_preamble_first() : attempts;
}

bool IOHomeControlComponent::request_device_status(const std::string &device_id) {
  auto *dev = this->get_device(device_id);
  if (dev == nullptr)
    return false;

  // A heater is polled on its own cadence, which continues even when this poll can not run (the hub
  // is not initialised yet): the controller owns that, the hub only hands the poll over.
  if (HeaterController::polls(*dev))
    return this->heater_controller_.poll(device_id);

  if (!this->initialized_)
    return false;
  if (!detail::known_device_supports_status_requests(*dev)) {
    detail::log_rejected_operation(device_id, *dev, "status request", "status-capable actuator");
    return false;
  }
  // Consumed by every ordinary status read, so a catch-up mark never outlives the read it was meant
  // for. It shapes only a cover's one-off read: a device the follow-up window tracks keeps the ladder's
  // plan (cover_poll_attempts_()). A heater's poll leaves it, as its try band ignores it; the tracking
  // window that poll opens or clears drops it.
  const bool state_unknown = this->poll_policy_.take_state_unknown(device_id);

  IoFrame request;
  // Tilt-capable covers need the extended 0x03200100 status request so the response includes
  // the reliable 16-byte tilt block. Other devices stay on the shorter generic request.
  bool const request_ok = has_feature(dev->features, device_feature::TILT)
                              ? create_get_status_tilt(request, this->node_id_, dev->node_id, dev->low_power)
                              : create_get_status(request, this->node_id_, dev->node_id, dev->low_power);
  if (!request_ok)
    return false;
  const bool scheduler_managed = this->poll_policy_.is_tracking_active(device_id, millis());
  const uint32_t retry_after_fail_ms = scheduler_managed ? STATUS_RETRY_AFTER_FAIL_MS : 0;
  const decisions::AttemptRequest attempts = this->cover_poll_attempts_(device_id, scheduler_managed, state_unknown);
  const bool answered = this->execute_request_and_update_(device_id, request, false, retry_after_fail_ms, attempts);
  // The mark is set only for a catch-up read (ADR 0080), whose miss is retried once (ADR 0095).
  if (state_unknown && answered) {
    this->poll_policy_.on_catchup_read_answered(device_id);
  } else if (state_unknown) {
    this->poll_policy_.on_catchup_read_unanswered(device_id, millis(), this->tuning_.remote_activity_catchup_poll_ms);
  }
  return answered;
}

namespace {

/// Count one periodic poll that brought no reading. A poll the device left unanswered is asked
/// again after PERIODIC_POLL_MISS_RETRY_MS first and counts only if the retry is unanswered too;
/// any other miss (an error reply, an answer of another shape) counts at once.
/// @return True while the last reading is no longer usable.
bool count_poll_miss(PeriodicPollPolicy &policy, const std::string &device_id, bool unanswered, const char *label) {
  if (!unanswered)
    return policy.on_poll_failed(device_id);
  const bool unusable = policy.on_poll_unanswered(device_id, millis());
  if (policy.retry_armed(device_id)) {
    ESP_LOGD(detail::TAG, "Device %s: %s poll unanswered, asking again in %" PRIu32 " s", device_id.c_str(), label,
             PERIODIC_POLL_MISS_RETRY_MS / 1000);
  }
  return unusable;
}

}  // namespace

// === Periodic readings ===

namespace {

/// @brief How one periodic reading is asked for, decoded, stored and logged.
///
/// One row per reading; request_periodic_reading_() and apply_periodic_reading_() hold the rules
/// every reading shares (wait while the device moves, stamp link health, count misses), and a row
/// holds only what differs. A reading is a number, `std::nullopt` standing for "no usable reading".
struct PeriodicReading {
  const char *label;        ///< Names the reading in log lines ("sun level") and in count_poll_miss().
  const char *capture_tag;  ///< Tag of the captured reply frame in the component capture log.
  /// Build the request for @p dev.
  bool (*build)(IoFrame &request, const uint8_t *own, const IoDevice &dev);
  /// Decode an answered poll; @p detail_text receives text for the state-change log line.
  std::optional<uint16_t> (*decode)(const std::string &device_id, const IoFrame &response, std::string &detail_text);
  /// Whether an answer of another shape than the decoder reads is warned about once (else it is a
  /// quiet miss; the decoder logs it).
  bool warn_undecoded;
  /// Warn that the device answered the request with an error.
  void (*warn_refused)(const std::string &device_id);
  /// The value the device record shows now.
  std::optional<uint16_t> (*shown)(const IoDevice &dev);
  /// Replace the value the device record shows.
  void (*store)(IoDevice &dev, const std::optional<uint16_t> &reading);
  /// A value as the state-change log line prints it.
  std::string (*format)(const std::optional<uint16_t> &reading);
  /// Whether an answer equal to the shown value is logged at debug level.
  bool log_unchanged;
};

/// A numeric reading for the state-change log line: the number followed by @p unit, or "unknown".
std::string format_level(const std::optional<uint16_t> &reading, const char *unit) {
  return reading.has_value() ? std::to_string(*reading) + unit : "unknown";
}

const PeriodicReading RAIN_READING = {
    .label = "rain sensor",
    .capture_tag = "limitation_rx",
    .build =
        [](IoFrame &request, const uint8_t *own, const IoDevice &dev) {
          return create_limitation_status_read(request, own, dev.node_id, LimitationType::MINIMUM, dev.low_power);
        },
    .decode = [](const std::string &device_id, const IoFrame &response,
                 std::string &detail_text) -> std::optional<uint16_t> {
      LimitationStatus status;
      if (response.cmd != CMD_LIMITATION_STATUS_RESP || !decode_limitation_status(response, status)) {
        ESP_LOGD(detail::TAG, "Device %s: unexpected reply 0x%02X to the limitation read", device_id.c_str(),
                 response.cmd);
        return std::nullopt;
      }
      ESP_LOGD(detail::TAG, "Device %s: %s", device_id.c_str(), detail::describe_limitation_reply(response).c_str());
      detail_text = " (minimum limit " + detail::format_limitation_value(status.value_raw) + ")";
      const RainSensorState state = decisions::rain_state_from_limitation_reply(status);
      return state == RainSensorState::UNKNOWN ? std::nullopt : std::optional<uint16_t>(static_cast<uint16_t>(state));
    },
    .warn_undecoded = false,
    .warn_refused =
        [](const std::string &device_id) {
          ESP_LOGW(detail::TAG, "Device %s answers the limitation read with an error; its rain sensor stays unknown",
                   device_id.c_str());
        },
    .shown = [](const IoDevice &dev) -> std::optional<uint16_t> {
      return dev.rain_sensor == RainSensorState::UNKNOWN
                 ? std::nullopt
                 : std::optional<uint16_t>(static_cast<uint16_t>(dev.rain_sensor));
    },
    .store =
        [](IoDevice &dev, const std::optional<uint16_t> &reading) {
          dev.rain_sensor = reading.has_value() ? static_cast<RainSensorState>(*reading) : RainSensorState::UNKNOWN;
        },
    .format =
        [](const std::optional<uint16_t> &reading) {
          return std::string(rain_sensor_state_name(reading.has_value() ? static_cast<RainSensorState>(*reading)
                                                                        : RainSensorState::UNKNOWN));
        },
    .log_unchanged = false,
};

/// Warn that the device refused an event subscription (the sun and battery polls).
void warn_event_refused(const std::string &device_id, const char *label) {
  ESP_LOGW(detail::TAG, "Device %s refuses the %s subscription; its %s stays unknown", device_id.c_str(), label, label);
}

const PeriodicReading SUN_READING = {
    .label = "sun level",
    .capture_tag = "event_reading_rx",
    .build =
        [](IoFrame &request, const uint8_t *own, const IoDevice &dev) {
          return create_subscribe_event(request, own, dev.node_id, EVENT_LOCAL_SENSOR, dev.low_power);
        },
    .decode = [](const std::string &, const IoFrame &response, std::string &) -> std::optional<uint16_t> {
      uint16_t value = 0;
      return decode_sun_level_event(response, value) ? std::optional<uint16_t>(value) : std::nullopt;
    },
    .warn_undecoded = true,
    .warn_refused = [](const std::string &device_id) { warn_event_refused(device_id, "sun level"); },
    .shown = [](const IoDevice &dev) { return dev.sun_level; },
    .store = [](IoDevice &dev, const std::optional<uint16_t> &reading) { dev.sun_level = reading; },
    .format = [](const std::optional<uint16_t> &reading) { return format_level(reading, ""); },
    .log_unchanged = true,
};

const PeriodicReading BATTERY_READING = {
    .label = "battery level",
    .capture_tag = "event_reading_rx",
    .build =
        [](IoFrame &request, const uint8_t *own, const IoDevice &dev) {
          return create_subscribe_event(request, own, dev.node_id, EVENT_BATTERY_LEVEL_CHANGE, dev.low_power);
        },
    .decode = [](const std::string &, const IoFrame &response, std::string &) -> std::optional<uint16_t> {
      uint8_t level = 0;
      return decode_battery_level_event(response, level) ? std::optional<uint16_t>(level) : std::nullopt;
    },
    .warn_undecoded = true,
    .warn_refused = [](const std::string &device_id) { warn_event_refused(device_id, "battery level"); },
    .shown = [](const IoDevice &dev) { return dev.battery_level; },
    .store = [](IoDevice &dev, const std::optional<uint16_t> &reading) { dev.battery_level = reading; },
    .format = [](const std::optional<uint16_t> &reading) { return format_level(reading, " %"); },
    .log_unchanged = true,
};

/// The row of a reading poll (RAIN_SENSOR, SUN_LEVEL or BATTERY_LEVEL).
const PeriodicReading &periodic_reading_row(IOHomeControlComponent::PeriodicPoll reading) {
  switch (reading) {
    case IOHomeControlComponent::PeriodicPoll::RAIN_SENSOR:
      return RAIN_READING;
    case IOHomeControlComponent::PeriodicPoll::SUN_LEVEL:
      return SUN_READING;
    default:
      return BATTERY_READING;
  }
}

}  // namespace

bool IOHomeControlComponent::request_device_limitation(const std::string &device_id) {
  return this->request_periodic_reading_(device_id, PeriodicPoll::RAIN_SENSOR);
}

bool IOHomeControlComponent::request_device_sun_level(const std::string &device_id) {
  return this->request_periodic_reading_(device_id, PeriodicPoll::SUN_LEVEL);
}

bool IOHomeControlComponent::request_device_battery_level(const std::string &device_id) {
  return this->request_periodic_reading_(device_id, PeriodicPoll::BATTERY_LEVEL);
}

// The sun and battery requests are subscriptions: the device answers with the event's current value,
// and that answer is the reading (ADR 0064). Every poll sends the subscription again; the device
// stores it each time, which the RS100 Solar io has accepted repeatedly (issue #156).
bool IOHomeControlComponent::request_periodic_reading_(const std::string &device_id, PeriodicPoll reading_kind) {
  IoDevice *dev = this->registry_.get(device_id);
  if (dev == nullptr || !this->initialized_)
    return false;
  const PeriodicReading &row = periodic_reading_row(reading_kind);
  PeriodicPollPolicy &policy = this->periodic_poll_policy_(reading_kind);

  // Not a failure and not a reading: a moving device is left alone (the same caution the diagnostic
  // probe applies), and asking again soon is cheaper than waiting out a whole interval.
  if (!effective_is_stopped(*dev)) {
    ESP_LOGD(detail::TAG, "Device %s: %s poll deferred, device is moving", device_id.c_str(), row.label);
    policy.defer(device_id, PERIODIC_POLL_MOVING_RETRY_MS, millis());
    return false;
  }

  IoFrame request;
  if (!row.build(request, this->node_id_, *dev))
    return false;

  // retry_after_fail_ms = 0 keeps the status-poll backoff ladder out of the way; a miss is counted
  // by this poll's own schedule instead.
  IoFrame response;
  const RequestResult result =
      this->exchange_and_record_(device_id, request, response, {false, 0, PERIODIC_POLL_MAX_TRIES});

  // A reply consumed inside the blocking exchange never reaches the normal receive path.
  dev = this->registry_.get(device_id);
  if (dev == nullptr)
    return false;
  std::optional<uint16_t> reading;
  std::string detail_text;
  if (result == RequestResult::REPLY) {
    // The device transmitted, whatever it said: stamp link health the way the status path does.
    detail::update_link_health(*dev, this->radio_);
    uint8_t raw[FRAME_MAX_SIZE] = {0};
    const uint8_t raw_len = serialize(response, raw, sizeof(raw));
    detail::log_component_capture(this->radio_, row.capture_tag, raw, raw_len, &response);
    reading = row.decode(device_id, response, detail_text);
    if (!reading.has_value() && row.warn_undecoded && policy.first_error_reply(device_id)) {
      char hex[FRAME_LOG_HEX_BUFFER_SIZE];
      bytes_to_hex(response.data, response.data_len, hex, sizeof(hex));
      ESP_LOGW(detail::TAG,
               "Device %s answers the %s poll with cmd=0x%02X payload=%s, a shape not decoded; its %s "
               "stays unknown",
               device_id.c_str(), row.label, response.cmd, hex, row.label);
    }
  } else if (result == RequestResult::ERROR_REPLY && policy.first_error_reply(device_id)) {
    row.warn_refused(device_id);
  }

  this->apply_periodic_reading_(device_id, reading_kind, reading, detail_text, result == RequestResult::FAILED);
  return reading.has_value();
}

void IOHomeControlComponent::apply_periodic_reading_(const std::string &device_id, PeriodicPoll reading_kind,
                                                     const std::optional<uint16_t> &reading,
                                                     const std::string &detail_text, bool unanswered) {
  IoDevice *dev = this->registry_.get(device_id);
  if (dev == nullptr)
    return;
  const PeriodicReading &row = periodic_reading_row(reading_kind);
  PeriodicPollPolicy &policy = this->periodic_poll_policy_(reading_kind);
  const std::optional<uint16_t> before = row.shown(*dev);
  std::optional<uint16_t> now = before;
  if (reading.has_value()) {
    policy.on_poll_succeeded(device_id);
    now = reading;
  } else if (count_poll_miss(policy, device_id, unanswered, row.label)) {
    now.reset();  // the last reading is too old to keep showing
  }
  if (now != before) {
    ESP_LOGI(detail::TAG, "Device %s: %s %s -> %s%s", device_id.c_str(), row.label, row.format(before).c_str(),
             row.format(now).c_str(), detail_text.c_str());
    row.store(*dev, now);
  } else if (reading.has_value() && row.log_unchanged) {
    ESP_LOGD(detail::TAG, "Device %s: %s %s (unchanged)", device_id.c_str(), row.label, row.format(reading).c_str());
  }
  // Notify on every outcome: link health moved too, so the link sensors need the update.
  this->notify_device_update_(device_id);
}

bool IOHomeControlComponent::request_device_name(const std::string &device_id) {
  auto *dev = this->get_device(device_id);
  if (dev == nullptr || !this->initialized_)
    return false;

  IoFrame request;
  if (!create_get_name(request, this->node_id_, dev->node_id, dev->low_power))
    return false;
  return this->execute_request_and_update_(device_id, request, false, 0);
}

bool IOHomeControlComponent::set_light_position(const std::string &device_id, uint8_t position) {
  auto *dev = this->get_device(device_id);
  if (dev == nullptr || !this->initialized_)
    return false;

  if (!detail::known_device_matches_entity_class(*dev, DeviceCapabilityClass::LIGHT)) {
    detail::log_rejected_operation(device_id, *dev, "light command", "light entity");
    return false;
  }

  // Light entities reuse the controller's existing execute path — the same position encoding
  // covers use, confirmed on real dimmable hardware (see the somfy_izymo_dimmer_* captures).
  return this->set_device_position(device_id, position);
}

bool IOHomeControlComponent::set_fan_position(const std::string &device_id, uint8_t position) {
  auto *dev = this->get_device(device_id);
  if (dev == nullptr || !this->initialized_)
    return false;

  if (!detail::known_device_matches_entity_class(*dev, DeviceCapabilityClass::HEATER_LEVEL)) {
    detail::log_rejected_operation(device_id, *dev, "fan command", "exterior heating (fan entity)");
    return false;
  }

  // An exterior heater takes the same position execute as a dimmable light; the heat level is the
  // IO position (0 = full heat), see platform_fan.cpp.
  return this->set_device_position(device_id, position);
}

bool IOHomeControlComponent::set_switch_state(const std::string &device_id, bool on) {
  auto *dev = this->get_device(device_id);
  if (dev == nullptr || !this->initialized_)
    return false;

  if (!detail::known_device_matches_entity_class(*dev, DeviceCapabilityClass::SWITCH)) {
    detail::log_rejected_operation(device_id, *dev, "switch command", "switch entity");
    return false;
  }

  // Switches share the same transport-level representation as binary lights.
  return this->set_device_position(device_id, on ? BINARY_ENTITY_ON_POSITION : BINARY_ENTITY_OFF_POSITION);
}

bool IOHomeControlComponent::set_lock_state(const std::string &device_id, bool locked) {
  auto *dev = this->get_device(device_id);
  if (dev == nullptr || !this->initialized_)
    return false;

  if (!detail::known_device_matches_entity_class(*dev, DeviceCapabilityClass::LOCK)) {
    detail::log_rejected_operation(device_id, *dev, "lock command", "lock entity");
    return false;
  }

  // Lock entities currently reuse the protocol's proven binary execute encoding:
  // unlock maps to 0 and lock maps to 100.
  return this->set_device_position(device_id, locked ? BINARY_ENTITY_OFF_POSITION : BINARY_ENTITY_ON_POSITION);
}

void IOHomeControlComponent::queue_set_device_position(const std::string &device_id, uint8_t position) {
  if (queue_guard_rejects(this, device_id, QUEUE_GUARD_COVER)) {
    // control() applies the optimistic prediction before calling this method, so a queue-time
    // guard rejection is a command that will not happen and must withdraw it, exactly as
    // run_execute_operation_() does for a dispatch-time failure. No-op when nothing was predicted.
    this->registry_.rollback_optimistic(device_id);
    return;
  }

  // Pre-scan for a pending SET_TILT so we can log its value if coalescing happens.
  uint8_t pending_tilt = 0;
  for (const auto &op : this->op_queue_) {
    if (op.type == PendingOperationType::SET_TILT && op.device_id == device_id) {
      pending_tilt = op.position;  // SET_TILT stores tilt in op.position
      break;
    }
  }
  if (this->op_queue_.enqueue_set_position(device_id, position)) {
    ESP_LOGI(detail::TAG,
             "Coalesced SET_POSITION (pos=%u) + pending SET_TILT (tilt=%u) → SET_POSITION_AND_TILT for "
             "device %s",
             position, pending_tilt, device_id.c_str());
  }
}

bool IOHomeControlComponent::queue_device_command(const std::string &device_id, CoverCommand cmd) {
  const IoDevice *dev = this->get_device(device_id);
  // Every false return below is a command that will not happen; control() (STOP →
  // apply_optimistic_stop) already predicted, so withdraw it — see queue_set_device_position().
  const bool is_stop = cmd == CoverCommand::STOP;
  if (!this->initialized_ || dev == nullptr) {
    this->registry_.rollback_optimistic(device_id, /*failed_stop=*/is_stop);
    return false;
  }
  // Same COVER guard as QUEUE_GUARD_COVER, kept inline here: this method returns bool and its
  // rejection noun is the specific command name rather than a fixed "queued cover command".
  if (!detail::known_device_matches_entity_class(*dev, DeviceCapabilityClass::COVER)) {
    detail::log_rejected_operation(device_id, *dev, cover_command_name(cmd), "cover entity");
    this->registry_.rollback_optimistic(device_id, /*failed_stop=*/is_stop);
    return false;
  }
  this->op_queue_.enqueue_device_command(device_id, cmd);
  return true;
}

void IOHomeControlComponent::queue_set_device_tilt(const std::string &device_id, uint8_t tilt_percent) {
  if (queue_guard_rejects(this, device_id, QUEUE_GUARD_TILT)) {
    // Withdraw the entity's optimistic prediction — see queue_set_device_position().
    this->registry_.rollback_optimistic(device_id);
    return;
  }

  // Pre-scan for a pending SET_POSITION so we can log its value if coalescing happens.
  uint8_t pending_pos = 0;
  for (const auto &op : this->op_queue_) {
    if (op.type == PendingOperationType::SET_POSITION && op.device_id == device_id) {
      pending_pos = op.position;
      break;
    }
  }
  if (this->op_queue_.enqueue_set_tilt(device_id, tilt_percent)) {
    ESP_LOGI(detail::TAG,
             "Coalesced pending SET_POSITION (pos=%u) + SET_TILT (tilt=%u) → "
             "SET_POSITION_AND_TILT for device %s",
             pending_pos, tilt_percent, device_id.c_str());
  }
}

void IOHomeControlComponent::queue_set_device_position_and_tilt(const std::string &device_id, uint8_t position,
                                                                uint8_t tilt_percent) {
  if (queue_guard_rejects(this, device_id, QUEUE_GUARD_POSITION_AND_TILT)) {
    // Withdraw the entity's optimistic prediction — see queue_set_device_position().
    this->registry_.rollback_optimistic(device_id);
    return;
  }
  this->op_queue_.enqueue_set_position_and_tilt(device_id, position, tilt_percent);
}

void IOHomeControlComponent::queue_request_device_status(const std::string &device_id) {
  if (queue_guard_rejects(this, device_id, QUEUE_GUARD_STATUS))
    return;
  // Keep at most one pending status poll per device. Without this, an overdue next_update can add
  // the same poll on every main-loop iteration until the first queued request is finally processed.
  this->op_queue_.enqueue_request_status(device_id);
}

void IOHomeControlComponent::queue_request_device_limitation(const std::string &device_id) {
  if (this->get_device(device_id) == nullptr)
    return;
  this->op_queue_.enqueue_request_limitation(device_id);
}

void IOHomeControlComponent::queue_request_device_sun_level(const std::string &device_id) {
  if (this->get_device(device_id) == nullptr)
    return;
  this->op_queue_.enqueue_request_sun_level(device_id);
}

void IOHomeControlComponent::queue_request_device_battery_level(const std::string &device_id) {
  if (this->get_device(device_id) == nullptr)
    return;
  this->op_queue_.enqueue_request_battery_level(device_id);
}

void IOHomeControlComponent::queue_request_device_name(const std::string &device_id) {
  if (this->get_device(device_id) == nullptr)
    return;
  this->op_queue_.enqueue_request_name(device_id);
}

/// Queue a discovery-and-pair request with elevated priority.
///
/// Flushes any pending status/name poll operations (which would consume time
/// during the device's limited pairing window) and pushes discovery to the
/// front of the queue. Duplicate requests are suppressed.
void IOHomeControlComponent::queue_discover_and_pair() {
  if (this->last_pairing_end_ms_ != 0 && millis() - this->last_pairing_end_ms_ < PAIRING_STALE_PRESS_MS) {
    ESP_LOGW(detail::TAG, "Discover & Pair pressed while the previous attempt was running: ignored. Put the device "
                          "back into pairing mode and press again.");
    return;
  }
  this->op_queue_.enqueue_discover_and_pair();
}

std::string IOHomeControlComponent::describe_last_commander(const IoDevice &dev) const {
  return detail::describe_last_commander(dev, this->node_id_);
}

void IOHomeControlComponent::trigger_scan_paired_devices() {
  if (this->busy_) {
    // Same event/log path a rejected API call gets (ManagementActions::resolve_device_() and
    // friends) -- a busy press must not go silent, since it's the one press outcome the API
    // action can never produce (an automation-triggered press is the only way to reach this
    // guard at all, see the doc comment on the declaration).
    ManagementActionResult result;
    result.action = "scan_paired_devices";
    result.message = "a radio exchange was already in progress; press the button again once it finishes";
    this->management_actions_.publish_result(result);
    return;
  }
  this->management_actions_.api_scan_paired_devices();
}

void IOHomeControlComponent::queue_set_light_position(const std::string &device_id, uint8_t position) {
  if (queue_guard_rejects(this, device_id, QUEUE_GUARD_LIGHT))
    return;
  this->op_queue_.enqueue_set_light_position(device_id, position);
}

void IOHomeControlComponent::queue_set_fan_position(const std::string &device_id, uint8_t position) {
  if (queue_guard_rejects(this, device_id, QUEUE_GUARD_FAN))
    return;
  this->op_queue_.enqueue_set_fan_position(device_id, position);
}

void IOHomeControlComponent::queue_set_light_state(const std::string &device_id, bool on) {
  this->queue_set_light_position(device_id, on ? BINARY_ENTITY_ON_POSITION : BINARY_ENTITY_OFF_POSITION);
}

void IOHomeControlComponent::queue_set_lock_state(const std::string &device_id, bool locked) {
  if (queue_guard_rejects(this, device_id, QUEUE_GUARD_LOCK))
    return;
  this->op_queue_.enqueue_set_lock_state(device_id, locked);
}

void IOHomeControlComponent::queue_set_switch_state(const std::string &device_id, bool on) {
  if (queue_guard_rejects(this, device_id, QUEUE_GUARD_SWITCH))
    return;
  this->op_queue_.enqueue_set_switch_state(device_id, on);
}

// === 1W transmit ===

void IOHomeControlComponent::execute_oneway_command_(const std::string &controller_id, CoverCommand cmd) {
  this->execute_oneway_([&] { this->oneway_transmitter_.send_command(controller_id, cmd); });
}

void IOHomeControlComponent::execute_oneway_position_(const std::string &controller_id, uint8_t position) {
  this->execute_oneway_([&] { this->oneway_transmitter_.send_position(controller_id, position); });
}

void IOHomeControlComponent::execute_oneway_enroll_(const std::string &controller_id) {
  this->execute_oneway_([&] { this->oneway_transmitter_.send_enrollment(controller_id); });
}

void IOHomeControlComponent::execute_oneway_unenroll_(const std::string &controller_id) {
  this->execute_oneway_([&] { this->oneway_transmitter_.send_unenrollment(controller_id); });
}

void IOHomeControlComponent::process_pending_operation_() {
  if (this->busy_ || this->op_queue_.empty())
    return;

  // Pop before dispatch so any handler that re-queues follow-up work sees the queue in its
  // post-consumption state and cannot accidentally execute the same operation twice.
  auto opt = this->op_queue_.pop();
  if (!opt.has_value())
    return;
  const PendingOperation &operation = *opt;

  switch (operation.type) {
    case PendingOperationType::SET_POSITION:
      this->set_device_position(operation.device_id, operation.position);
      break;
    case PendingOperationType::SET_TILT:
      this->set_device_tilt(operation.device_id, operation.position);
      break;
    case PendingOperationType::SET_POSITION_AND_TILT:
      this->set_device_position_and_tilt(operation.device_id, operation.position, operation.tilt);
      break;
    case PendingOperationType::DEVICE_COMMAND:
      this->execute_device_command_(operation.device_id, operation.command);
      break;
    case PendingOperationType::SET_LIGHT_STATE:
      // operation.position already carries the target IO position (0-100) regardless of whether
      // it was enqueued via queue_set_light_state() (binary extremes) or
      // queue_set_light_position() (dimmable) — see enqueue_set_light_state()'s thin-wrapper doc.
      this->set_light_position(operation.device_id, operation.position);
      break;
    case PendingOperationType::SET_FAN_POSITION:
      this->set_fan_position(operation.device_id, operation.position);
      break;
    case PendingOperationType::SET_LOCK_STATE:
      this->set_lock_state(operation.device_id, operation.position == BINARY_ENTITY_OFF_POSITION);
      break;
    case PendingOperationType::SET_SWITCH_STATE:
      this->set_switch_state(operation.device_id, operation.position == BINARY_ENTITY_ON_POSITION);
      break;
    case PendingOperationType::ONEWAY_COMMAND:
      // device_id carries the controller-identity handle for 1W ops — see PendingOperation.
      this->execute_oneway_command_(operation.device_id, operation.command);
      break;
    case PendingOperationType::ONEWAY_POSITION:
      this->execute_oneway_position_(operation.device_id, operation.position);
      break;
    case PendingOperationType::ONEWAY_ENROLL:
      this->execute_oneway_enroll_(operation.device_id);
      break;
    case PendingOperationType::ONEWAY_UNENROLL:
      this->execute_oneway_unenroll_(operation.device_id);
      break;
    case PendingOperationType::REQUEST_STATUS:
      this->request_device_status(operation.device_id);
      break;
    case PendingOperationType::REQUEST_NAME:
      this->request_device_name(operation.device_id);
      break;
    case PendingOperationType::REQUEST_LIMITATION:
      this->request_device_limitation(operation.device_id);
      break;
    case PendingOperationType::REQUEST_SUN_LEVEL:
      this->request_device_sun_level(operation.device_id);
      break;
    case PendingOperationType::REQUEST_BATTERY_LEVEL:
      this->request_device_battery_level(operation.device_id);
      break;
    case PendingOperationType::DISCOVER_AND_PAIR:
      this->discover_and_pair();
      break;
  }
  // Every operation blocks on the radio, so a press that started during it was not heard. While a
  // burst of linked-remote activity is open, this moves its catch-up read out (ADR 0080).
  this->poll_policy_.on_exchange_ended(millis(), this->tuning_.remote_activity_catchup_poll_ms);
}

}  // namespace home_io_control
}  // namespace esphome
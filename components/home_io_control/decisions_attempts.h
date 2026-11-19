#pragma once

/// @file decisions_attempts.h
/// @brief Pure planning of how an exchange spends its transmit attempts: attempt plans and requests, the poll backoff
/// ladder and the low-power wake belief.
/// @ingroup hioc_hub
///
/// Inline, stateless helpers, testable without radio hardware or timing.

#include "proto_constants.h"
#include "proto_device_model.h"
#include "proto_frame.h"
#include "proto_timing.h"
#include "tuning_config.h"

#include <algorithm>
#include <cstdint>

namespace esphome {
namespace home_io_control {
namespace decisions {

/// True for a CMD_EXECUTE that has the same effect sent twice as sent once. Every EXECUTE this hub
/// sends names an absolute target (a position, open/close, STOP, a tilt angle, a light level)
/// except the stored-position selector POS_FAVORITE, used by favourite, vent, partial and pedestrian
/// (every NAMED_POSITIONS entry): on a Somfy motor "My" while moving means stop, so a
/// second copy can undo what the first one started.
/// @param request Outbound request frame.
[[nodiscard]] inline bool is_repeatable_execute(const IoFrame &request) {
  return request.cmd == CMD_EXECUTE && request.data_len > EXECUTE_MAIN_BYTE_OFFSET &&
         request.data[EXECUTE_MAIN_BYTE_OFFSET] != POS_FAVORITE;
}

/// Whether an authenticated-but-unanswered request may be sent again.
///
/// Every request except CMD_EXECUTE and CMD_SUBSCRIBE_EVENT is idempotent (status polls, name
/// reads, management actions, config writes) and keeps its full retry budget when the device
/// authenticates but never closes the exchange.
///
/// CMD_EXECUTE moves something, and a missing closing reply means two different things depending
/// on the device. Some devices never close an EXECUTE exchange within the response window and
/// report through a later status update instead; for them silence is normal and a re-send only
/// repeats a command already being carried out. For a device that normally does close it, silence
/// can mean our challenge answer never arrived and the command was not carried out at all (a STOP
/// that left an awning moving). So an EXECUTE is sent again only to a device known to confirm,
/// only when repeating it is harmless (is_repeatable_execute()), and at most
/// UNCONFIRMED_EXECUTE_MAX_RESENDS times per exchange.
///
/// CMD_SUBSCRIBE_EVENT stores a subscription in the device, and the device answers it with the
/// event's current value (is_subscription_answer()). A try the device authenticated has stored the
/// subscription, but its answer may still have been lost, so it is sent once more
/// (UNCONFIRMED_SUBSCRIBE_MAX_RESENDS), never more: the device has accepted repeated subscriptions
/// (issue #156), and a reading is what the request is for.
///
/// @param request                  Outbound request frame.
/// @param target_confirms_execute  The target has closed an EXECUTE exchange with a reply before
///                                 (TargetEvidence::confirms_execute).
/// @param unconfirmed_tries        Tries of this exchange that ended accepted without a closing
///                                 reply, including the one just ended (1-based).
/// @return true when the exchange should spend another try.
[[nodiscard]] inline bool retry_after_unconfirmed_accept_is_safe(const IoFrame &request, bool target_confirms_execute,
                                                                 uint8_t unconfirmed_tries) {
  if (request.cmd == CMD_SUBSCRIBE_EVENT)
    return unconfirmed_tries <= UNCONFIRMED_SUBSCRIBE_MAX_RESENDS;
  if (request.cmd != CMD_EXECUTE)
    return true;
  return target_confirms_execute && is_repeatable_execute(request) &&
         unconfirmed_tries <= UNCONFIRMED_EXECUTE_MAX_RESENDS;
}

/// @brief True when a scheduler-owned status poll sits in the middle band of the backoff ladder.
///
/// See SCHEDULED_POLL_RETRY_GRACE_FIRST_FAILURE (proto_timing.h) for why the band is where a poll
/// is worth more than one attempt: the slot that lands just after a manoeuvre ends, rather than the
/// start or the tail of the ladder.
///
/// The two counters are mutually exclusive by construction (StatusPollPolicy::on_exchange_failed()
/// zeroes one while incrementing the other), so an auth-shaped streak reads status_poll_failures as
/// 0 and would otherwise fall into the band's own "fresh window" case. It is rejected first,
/// deliberately, so the predicate stays correct even if that exclusivity is ever relaxed.
///
/// @param status_poll_failures Consecutive silent failures already recorded for this device.
/// @param auth_poll_failures   Consecutive challenge-seen failures already recorded.
[[nodiscard]] inline bool scheduled_poll_in_retry_band(uint8_t status_poll_failures, uint8_t auth_poll_failures) {
  return auth_poll_failures == 0 && status_poll_failures >= SCHEDULED_POLL_RETRY_GRACE_FIRST_FAILURE &&
         status_poll_failures <= SCHEDULED_POLL_RETRY_GRACE_LAST_FAILURE;
}

/// @brief Transmit-attempt budget for a scheduler-owned status poll of a heater, by ladder position.
///
/// Heaters keep this count-based budget; a cover's polls take their plan from
/// scheduled_poll_read_kind() instead.
///
/// @param status_poll_failures Consecutive silent failures already recorded for this device.
/// @param auth_poll_failures   Consecutive challenge-seen failures already recorded.
/// @return EXCHANGE_RETRY_COUNT inside the band, SCHEDULED_POLL_MAX_TRIES everywhere else.
inline uint8_t scheduled_poll_max_tries(uint8_t status_poll_failures, uint8_t auth_poll_failures) {
  return scheduled_poll_in_retry_band(status_poll_failures, auth_poll_failures) ? EXCHANGE_RETRY_COUNT
                                                                                : SCHEDULED_POLL_MAX_TRIES;
}

// == Attempt plan ==

/// @brief What a request is for, which decides how its attempts are spent.
enum class ReadKind : uint8_t {
  PROGRESS,  ///< A look at a device that is probably still busy (a mid-travel poll): one attempt, the
             ///< backoff ladder is the retry.
  SETTLING,  ///< A read that decides where a device ended up: as many attempts as fit the budget.
  COMMAND,   ///< A request that is harmless to repeat (is_repeat_safe_command()): to a mains device
             ///< as many attempts as fit the budget, to a low-power one today's few with a gap.
};

/// @brief How one exchange spends its attempts. Built by the plan builders below from a caller's request and run
/// by ExchangeEngine::send_and_receive().
struct AttemptPlan {
  uint8_t max_attempts{EXCHANGE_RETRY_COUNT};  ///< Most transmissions of the request, `[1, EXCHANGE_MAX_ATTEMPTS]`.
  uint16_t gap_ms{EXCHANGE_RETRY_DELAY_MS};    ///< Wait before each attempt after the first; 0 for none.
  uint16_t window_ms{RESPONSE_START_WAIT_MS};  ///< How long the last attempt listens for the first reply.
  /// How long every attempt before the last listens; 0 means the same as @ref window_ms. A mains
  /// target answers within tens of milliseconds or not at all, so the early attempts need not wait
  /// the whole window: the last one does, to still catch a device that answers late.
  uint16_t early_window_ms{0};
  /// True when the first attempt is a short lead to a low-power receiver in an unknown state
  /// (WakeBelief::UNKNOWN): the short start preamble, a brief window of its own
  /// (LOW_POWER_SHORT_LEAD_WINDOW_MS), and the wake-up attempts after it. A moving receiver hears
  /// the short one and a resting one the wake-up. It counts in @ref max_attempts.
  bool short_lead{false};
  /// True when the caller asked for the read to treat the receiver's state as unknown
  /// (AttemptRequest::with_state_unknown()). The exchange engine resolves the wake belief with it on
  /// sending too, so the plan and the tries agree (wake_belief()).
  bool state_unknown{false};
  /// True when the attempt count, not `exchange_total_budget_ms`, bounds the exchange: a wake-up
  /// exchange to a low-power target keeps all its attempts (the three wake-ups, plus the short lead
  /// where there is one) whatever a busy channel or jitter added to the earlier ones (ADR 0095).
  /// Set by the plan builders; the engine's guard asks obeys_time_budget().
  bool bounded_by_attempts{false};

  /// @param attempt  1-based number of the attempt about to listen.
  /// @param attempts Attempts the exchange makes in all (the last one listens @ref window_ms).
  /// @return How long that attempt listens for the first reply.
  [[nodiscard]] uint16_t window_for_attempt(uint8_t attempt, uint8_t attempts) const {
    if (short_lead && attempt == 1)
      return LOW_POWER_SHORT_LEAD_WINDOW_MS;
    return (attempt < attempts && early_window_ms != 0) ? early_window_ms : window_ms;
  }
};

/// @brief The attempt budget rule: an attempt starts only if the whole attempt, up to the end of the
/// last wait it can make, ends inside @p budget_ms.
///
/// Both the engine's guard (ExchangeEngine::try_fits_budget_()) and the plan that counts attempts
/// (attempts_that_fit()) use it, so a plan never promises an attempt the guard would refuse for a
/// target that stays silent. The attempt's overhead (EXCHANGE_ATTEMPT_OVERHEAD_MS) is counted here
/// and nowhere else.
///
/// @param elapsed_ms   Time since the exchange began.
/// @param gap_ms       Wait before the attempt is sent.
/// @param tx_ms        The attempt's transmit time as the radio estimates it.
/// @param window_ms    How long the attempt then listens for the first reply.
/// @param auth_tail_ms The final-reply window a challenged attempt adds, 0 until the target has challenged.
/// @param budget_ms    `exchange_total_budget_ms`.
[[nodiscard]] inline bool attempt_fits(uint32_t elapsed_ms, uint32_t gap_ms, uint32_t tx_ms, uint32_t window_ms,
                                       uint32_t auth_tail_ms, uint32_t budget_ms) {
  return elapsed_ms + gap_ms + tx_ms + EXCHANGE_ATTEMPT_OVERHEAD_MS + window_ms + auth_tail_ms < budget_ms;
}

/// @brief Whether the engine's guard applies `exchange_total_budget_ms` to the attempts of @p plan.
/// Every plan obeys it except one bounded by its attempt count (AttemptPlan::bounded_by_attempts, ADR 0095).
/// @param plan The plan the exchange runs.
[[nodiscard]] inline bool obeys_time_budget(const AttemptPlan &plan) { return !plan.bounded_by_attempts; }

/// @brief How many attempts to a silent target fit the budget when none waits a gap, capped at @p cap.
///
/// The first `n - 1` attempts listen @p early_window_ms and the last listens @p last_window_ms, and
/// the last must still pass attempt_fits(), so n is the largest with
/// `(n - 1) * (tx + overhead + early) + tx + overhead + last < budget`. At least one attempt: the
/// first always runs. A zero budget is no limit the plan can honour (the engine's guard still
/// applies), so it yields @p cap.
///
/// @param budget_ms        `exchange_total_budget_ms`.
/// @param attempt_tx_ms    One attempt's transmit time as the radio estimates it.
/// @param early_window_ms  Listen time of every attempt but the last.
/// @param last_window_ms   Listen time of the last attempt.
/// @param cap              Most attempts the caller allows.
[[nodiscard]] inline uint8_t attempts_that_fit(uint32_t budget_ms, uint32_t attempt_tx_ms, uint32_t early_window_ms,
                                               uint32_t last_window_ms, uint8_t cap) {
  const uint32_t early_ms = attempt_tx_ms + EXCHANGE_ATTEMPT_OVERHEAD_MS + early_window_ms;
  const uint32_t last_ms = attempt_tx_ms + EXCHANGE_ATTEMPT_OVERHEAD_MS + last_window_ms;
  if (budget_ms == 0 || early_ms == 0)
    return cap;
  if (last_ms >= budget_ms)
    return 1;
  const uint32_t fitting = 1 + ((budget_ms - 1 - last_ms) / early_ms);
  return static_cast<uint8_t>(std::max<uint32_t>(1, std::min<uint32_t>(fitting, cap)));
}

/// @brief The plan for a caller that names only a number of attempts: that many (at least one, at
/// most EXCHANGE_RETRY_COUNT), the default gap, and @p window_ms.
/// @param max_tries Attempts the caller asks for.
/// @param window_ms First-reply window of every attempt.
[[nodiscard]] inline AttemptPlan attempt_plan_for_tries(uint8_t max_tries, uint16_t window_ms) {
  AttemptPlan plan;
  plan.max_attempts = std::max<uint8_t>(1, std::min<uint8_t>(max_tries, EXCHANGE_RETRY_COUNT));
  plan.window_ms = window_ms;
  return plan;
}

/// @brief The attempt plan for a request, from what it is for and who it asks.
///
/// A settling read, and a repeat-safe command to a mains device, repeat without a gap: every attempt
/// listens from the end of its own transmission, so a silent attempt is already followed by the next
/// one as soon as its window ends. The attempts before the last listen only
/// `exchange_early_response_wait_ms` (a mains device answers within tens of milliseconds or not at
/// all) and the last listens the whole window, so how many fit is attempts_that_fit(), capped at
/// EXCHANGE_MAX_ATTEMPTS. A settling read of a low-power device makes LOW_POWER_SETTLING_ATTEMPTS
/// attempts of LOW_POWER_RESPONSE_WAIT_MS each, whatever the budget: such a plan is
/// bounded by its attempt count and runs past the budget when they do not fit
/// (AttemptPlan::bounded_by_attempts, ADR 0095, amending ADR 0073). For every
/// other target the plan is the rule the engine's guard applies at run time, so a plan never
/// promises an attempt the guard would refuse when the target stays silent. A challenged attempt
/// costs more (the final reply window), so the guard can still stop such a plan early.
///
/// PROGRESS and a COMMAND to a low-power device keep today's shapes: one attempt, and
/// EXCHANGE_RETRY_COUNT attempts with the default gap, both with the tuned start window; the
/// command's attempts are bounded by their count too (ADR 0095).
///
/// @param kind           What the request is for.
/// @param low_power      The target is a low-power receiver (YAML `low_power`, CTRL1 low-power bit).
/// @param tuning         Reply windows and exchange budget.
/// @param attempt_tx_ms  One attempt's transmit time as the radio estimates it (long wake-up
///                       preamble for a low-power target); sizes a gapless mains plan's attempt count.
/// @param last_window_ms Listen time of the last attempt of a mains target, or of every attempt of a
///                       gapped plan; 0 for the tuned start window.
/// @param with_short_lead Set to lead a settling read of a low-power target with one short attempt
///                       (the receiver's state is unknown, see WakeBelief::UNKNOWN). The lead is added to the wake-up
///                       attempts, which are the same as without it, and the plan counts it in
///                       AttemptPlan::max_attempts. No lead for any other read.
[[nodiscard]] inline AttemptPlan attempt_plan(ReadKind kind, bool low_power, const TuningConfig &tuning,
                                              uint32_t attempt_tx_ms, uint16_t last_window_ms = 0,
                                              bool with_short_lead = false) {
  AttemptPlan plan =
      attempt_plan_for_tries(kind == ReadKind::PROGRESS ? SCHEDULED_POLL_MAX_TRIES : EXCHANGE_RETRY_COUNT,
                             last_window_ms != 0 ? last_window_ms : tuning.exchange_start_response_wait_ms);
  if (kind == ReadKind::PROGRESS)
    return plan;
  if (kind == ReadKind::COMMAND && low_power) {
    plan.bounded_by_attempts = true;  // all of them are kept (ADR 0095)
    return plan;
  }
  plan.gap_ms = 0;
  if (low_power) {
    plan.window_ms = LOW_POWER_RESPONSE_WAIT_MS;
    plan.max_attempts = LOW_POWER_SETTLING_ATTEMPTS;
    plan.bounded_by_attempts = true;  // the wake-up attempts all run (ADR 0095)
    if (with_short_lead) {
      plan.short_lead = true;
      plan.max_attempts++;
    }
    return plan;
  }
  plan.early_window_ms = std::min(tuning.exchange_early_response_wait_ms, plan.window_ms);
  plan.max_attempts = attempts_that_fit(tuning.exchange_total_budget_ms, attempt_tx_ms, plan.early_window_ms,
                                        plan.window_ms, EXCHANGE_MAX_ATTEMPTS);
  return plan;
}

/// @brief Whether a request may be sent again to a device that said nothing, without a second action.
///
/// A device that hears a request acts on it and answers; silence therefore means it did not hear,
/// or its reply was lost. For a request whose copies have one effect (an absolute position, open,
/// close, STOP, a read) repeating it is harmless either way. CMD_EXECUTE with POS_FAVORITE is not
/// (is_repeatable_execute()): on a Somfy motor it stops a moving cover, so a second copy can undo
/// the first. Every request the hub sends that is not a CMD_EXECUTE is idempotent.
///
/// @param request Outbound request frame.
[[nodiscard]] inline bool is_repeat_safe_command(const IoFrame &request) {
  return request.cmd != CMD_EXECUTE || is_repeatable_execute(request);
}

/// @brief What a cover's status poll is for, from where it sits and what it settles.
///
/// A read that decides where the cover ended up is a SETTLING read: the poll after an accepted STOP,
/// the one confirming a cover that stopped short of its target (@p settles_a_position), and the
/// polls in the ladder's retry band (scheduled_poll_in_retry_band()). Every other scheduled poll is
/// a look at a device that is probably still busy or unreachable (PROGRESS): the ladder is its
/// retry.
///
/// @param status_poll_failures Consecutive silent failures already recorded for this device.
/// @param auth_poll_failures   Consecutive challenge-seen failures already recorded.
/// @param settles_a_position   True for the poll after an accepted STOP or one confirming a stop off
///                             target (StatusPollPolicy::take_settling_read()).
[[nodiscard]] inline ReadKind scheduled_poll_read_kind(uint8_t status_poll_failures, uint8_t auth_poll_failures,
                                                       bool settles_a_position) {
  return (settles_a_position || scheduled_poll_in_retry_band(status_poll_failures, auth_poll_failures))
             ? ReadKind::SETTLING
             : ReadKind::PROGRESS;
}

/// @brief What a caller asks of one exchange's attempts: a plain cap on tries (today's shape), or a
/// read kind whose plan the engine builds for the request (send_and_receive() builds the
/// same plan itself).
///
/// Converts implicitly from a number of tries, so callers that only name a count are unchanged.
struct AttemptRequest {
  uint8_t max_tries{EXCHANGE_RETRY_COUNT};  ///< Cap on attempts; ignored once @ref kind is set.
  bool planned{false};                      ///< True: build the plan for @ref kind instead of using a count.
  ReadKind kind{ReadKind::COMMAND};         ///< What the read is for; only read when @ref planned.
  /// True for the read at a moving cover's predicted end of travel (@ref with_wake_preamble_first()).
  bool wake_preamble_first{false};
  /// True for a read whose evidence about the receiver may be stale (@ref with_state_unknown()).
  bool state_unknown{false};

  AttemptRequest() = default;
  /// @param tries Cap on attempts (the count form of ExchangeOptions).
  AttemptRequest(uint8_t tries)
      : max_tries(tries) {}  // NOLINT(google-explicit-constructor): count callers stay as they are
  /// @param read_kind What the read is for.
  [[nodiscard]] static AttemptRequest of_kind(ReadKind read_kind) {
    AttemptRequest request;
    request.planned = true;
    request.kind = read_kind;
    return request;
  }
  /// @brief Mark the read as aimed at the moment the receiver has just come to rest: to a low-power
  /// target every attempt sends the wake-up preamble, which a resting receiver hears, whatever the
  /// wake belief says (ADR 0077). Without effect for a target that is not low-power.
  /// \todo Untested on hardware: needs a low-power cover that reports a remaining time.
  [[nodiscard]] AttemptRequest with_wake_preamble_first() const {
    AttemptRequest request = *this;
    request.wake_preamble_first = true;
    return request;
  }
  /// @brief Mark a status read as made without trustworthy news of the receiver: activity the hub
  /// may have missed can have set it moving since its last report, so a recent "stopped" report
  /// does not prove it rests. The wake belief is then UNKNOWN unless fresh moving evidence says
  /// AWAKE (wake_belief()), and a settling read leads with one short attempt before the wake-up
  /// attempts. Only a planned read (@ref of_kind) carries it to the engine; without effect for a
  /// target that is not low-power, for a command, and wherever the belief is not applied: a fixed
  /// preamble (@ref with_wake_preamble_first() wins) or `low_power_wake_belief` switched off.
  /// \todo Untested on hardware, like WakeBelief::UNKNOWN itself.
  [[nodiscard]] AttemptRequest with_state_unknown() const {
    AttemptRequest request = *this;
    request.state_unknown = true;
    return request;
  }
  /// True for a look at a device that is probably still busy: one attempt, the ladder is the retry
  /// (ReadKind::PROGRESS, or a count of SCHEDULED_POLL_MAX_TRIES).
  [[nodiscard]] bool is_progress_look() const {
    return planned ? kind == ReadKind::PROGRESS : max_tries == SCHEDULED_POLL_MAX_TRIES;
  }
};

// == Low-power wake belief ==

/// @brief Whether a low-power receiver is believed to be travelling right now, judged from what this
/// hub has seen of it. Orders the tries of a directed exchange: a moving receiver hears the short
/// start preamble and ignores the long wake-up one, a resting one needs the long one.
///
/// Having merely heard from the device recently is deliberately not a reason to lead short: a
/// resting VELUX SSL solar roller shutter ignored the short preamble on every try in field logs,
/// including tries sent 34 ms after it had answered (ADR 0040, amendment).
enum class WakeBelief : uint8_t {
  ASLEEP,  ///< Not known to be moving — lead with the wake-up preamble.
  AWAKE,   ///< Moving, or about to be stopped — the short preamble is the one it hears.
  /// A status read to a receiver the hub has no recent news of (no moving evidence, and either no status
  /// report within LOW_POWER_MAX_TRAVEL_MS — boot, a status refresh — or a read its caller marked with
  /// AttemptRequest::with_state_unknown(), such as a catch-up read): it may be travelling,
  /// when it ignores the wake-up preamble, or at rest, when it hears only that one. Every try of a
  /// single-try read keeps the wake-up preamble; a settling read leads with one short attempt of its
  /// own (AttemptPlan::short_lead) and then sends the wake-up attempts.
  /// \todo Experimental: rests on a moving VELUX screen ignoring the wake-up preamble (issues #114
  ///       and #209); no bench run has met a read to a receiver in this state yet.
  UNKNOWN,
};

/// @brief What the hub knows about the device an exchange is addressed to, as the exchange engine
/// sees it. The engine has no device registry of its own; the hub hands this over through
/// ExchangeEngine::set_target_evidence_provider(), and every per-target decision the engine makes
/// reads from it: wake_belief() and retry_after_unconfirmed_accept_is_safe(). Timestamps are `millis()` values, 0 =
/// never.
struct TargetEvidence {
  uint32_t last_moving_evidence_ms;  ///< Last sign the receiver is travelling (see note_moving_evidence(),
                                     ///< clear_moving_evidence()).
  uint32_t last_seen_ms;             ///< Last frame received from the receiver, any command. Only logged
                                     ///< (`age_ms=`); it does not feed the belief.
  bool confirms_execute;             ///< The device has closed a CMD_EXECUTE exchange with a reply before.
  uint32_t moving_until_ms{0};       ///< When the remaining time of its last trusted moving report has run
                                     ///< out (`millis()`), 0 = unknown (see moving_until_holds()).
  uint32_t last_status_ms{0};        ///< Last status report from the receiver (`millis()`), 0 = never. A recent
                                     ///< one is news of its state, so a status read is not UNKNOWN then.
};

/// Build the exchange engine's view of a device record.
/// @param dev Device record to read.
[[nodiscard]] inline TargetEvidence target_evidence(const IoDevice &dev) {
  return {dev.last_moving_evidence_ms, dev.last_seen_ms, dev.confirms_execute, dev.moving_until_ms, dev.last_status};
}

/// True while the remaining time the receiver last reported has not run out: by its own account it
/// is still travelling, so a silent exchange is not evidence that it has come to rest. False when no
/// remaining time is known (0). Wrap-safe.
/// \todo Untested on hardware: needs a low-power cover that reports a remaining time (ADR 0077).
/// @param evidence Stamps for the target device.
/// @param now      Current millis().
[[nodiscard]] inline bool moving_until_holds(const TargetEvidence &evidence, uint32_t now) {
  return evidence.moving_until_ms != 0 && static_cast<int32_t>(evidence.moving_until_ms - now) > 0;
}

/// True for a CMD_EXECUTE whose main byte is POS_STOP. A STOP is only ever sent to a receiver that
/// is (believed) moving, so it is treated as awake whatever the stamps say.
/// @param request Outbound request frame.
[[nodiscard]] inline bool is_stop_request(const IoFrame &request) {
  return request.cmd == CMD_EXECUTE && request.data_len > EXECUTE_MAIN_BYTE_OFFSET &&
         request.data[EXECUTE_MAIN_BYTE_OFFSET] == POS_STOP;
}

/// Judge whether a low-power receiver is travelling. `AWAKE` for a STOP request or while moving
/// evidence is younger than LOW_POWER_MAX_TRAVEL_MS. For a status read with neither moving evidence nor
/// a status report within that time the answer is `UNKNOWN`; otherwise `ASLEEP`. A zero stamp means
/// never and is never recent. Ages use unsigned subtraction, so `millis()` wrap-around is safe.
/// @param evidence Stamps for the target device.
/// @param now      Current millis().
/// @param is_stop  True when the request being sent is a STOP (see is_stop_request()).
/// @param is_status_read True when the request is a status read (CMD_PRIVATE): only a read can be
///                 UNKNOWN, a command keeps the rule it had.
/// @param state_unknown True when the caller marked the read as made without trustworthy news
///                 (AttemptRequest::with_state_unknown()): a recent status report then does not make
///                 the belief ASLEEP. Only a status read takes it; fresh moving evidence still wins.
[[nodiscard]] inline WakeBelief wake_belief(const TargetEvidence &evidence, uint32_t now, bool is_stop,
                                            bool is_status_read = false, bool state_unknown = false) {
  const uint32_t moving = evidence.last_moving_evidence_ms;
  if (is_stop || (moving != 0 && (now - moving) < LOW_POWER_MAX_TRAVEL_MS))
    return WakeBelief::AWAKE;
  const uint32_t reported = evidence.last_status_ms;
  if (is_status_read && (state_unknown || reported == 0 || (now - reported) >= LOW_POWER_MAX_TRAVEL_MS))
    return WakeBelief::UNKNOWN;
  return WakeBelief::ASLEEP;
}

/// Start-preamble length for one try of a directed exchange to a low-power receiver. The plans
/// (short = `short_preamble`, wake = `wake_preamble`): AWAKE short/wake/wake, ASLEEP and UNKNOWN wake on
/// every try (the short lead of an UNKNOWN settling read is an attempt of its own, AttemptPlan::short_lead). A moving
/// receiver answers the first short try; when it does not, it has most likely come to rest, where only the wake-up
/// preamble reaches it, so every later try uses that one. A single-try exchange (most scheduler-owned status polls)
/// sends only try 1's preamble; its backoff ladder, and the evidence a silent AWAKE exchange spends
/// (ExchangeEngine::set_wake_evidence_spent_handler()), cover a wrong belief there.
/// @param belief         See wake_belief().
/// @param try_index      1-based try number, clamped to [1, EXCHANGE_RETRY_COUNT].
/// @param short_preamble Preamble for a receiver known to be moving (`start_preamble_ms`, in bytes).
/// @param wake_preamble  Wake-up preamble for a resting receiver (`low_power_wake_ms`, in bytes).
[[nodiscard]] inline uint16_t low_power_try_preamble(WakeBelief belief, uint8_t try_index, uint16_t short_preamble,
                                                     uint16_t wake_preamble) {
  const uint8_t try_1based = std::max<uint8_t>(1, std::min<uint8_t>(try_index, EXCHANGE_RETRY_COUNT));
  if (belief == WakeBelief::AWAKE && try_1based == 1)
    return short_preamble;
  return wake_preamble;
}

/// Lowercase name of a belief for log lines.
[[nodiscard]] inline const char *wake_belief_name(WakeBelief belief) {
  switch (belief) {
    case WakeBelief::AWAKE:
      return "awake";
    case WakeBelief::UNKNOWN:
      return "unknown";
    case WakeBelief::ASLEEP:
    default:
      return "asleep";
  }
}

}  // namespace decisions
}  // namespace home_io_control
}  // namespace esphome

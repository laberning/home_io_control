#include "decisions_attempts.h"

#include "test_helpers.h"

using namespace esphome::home_io_control;
using namespace test;

// ============================================================================
// Attempt plans, retry safety, poll read kinds, target evidence and the low-power wake belief.
// ============================================================================
// Pure helpers in decisions_attempts.h, tested without radio or timing.

// ============================================================================
// Scheduled-poll retry budget of a heater — scheduled_poll_max_tries()
// ============================================================================

TEST(Decisions, ScheduledPollTriesSettleSlotStaysSingleTry) {
  EXPECT_EQ(decisions::scheduled_poll_max_tries(/*status_poll_failures=*/0, /*auth_poll_failures=*/0),
            SCHEDULED_POLL_MAX_TRIES)
      << "the settle poll fires mid-manoeuvre; a STOP the user presses then must not queue behind it";
}

TEST(Decisions, ScheduledPollTriesGraceBandGetsTheFullBudget) {
  EXPECT_EQ(decisions::scheduled_poll_max_tries(1, 0), EXCHANGE_RETRY_COUNT);
  EXPECT_EQ(decisions::scheduled_poll_max_tries(2, 0), EXCHANGE_RETRY_COUNT);
  EXPECT_EQ(decisions::scheduled_poll_max_tries(3, 0), EXCHANGE_RETRY_COUNT)
      << "these slots are the first real chances to catch a duty-cycled device once its manoeuvre "
         "has ended";
}

TEST(Decisions, ScheduledPollTriesTailDropsBackToSingleTry) {
  EXPECT_EQ(decisions::scheduled_poll_max_tries(4, 0), SCHEDULED_POLL_MAX_TRIES);
  EXPECT_EQ(decisions::scheduled_poll_max_tries(UINT8_MAX, 0), SCHEDULED_POLL_MAX_TRIES)
      << "several consecutive misses means unreachable, not asleep — retries buy nothing there";
}

TEST(Decisions, ScheduledPollTriesAuthStreakNeverGetsTheBand) {
  // Regression test: a naive `status_poll_failures < N` gate passes every test above but fails this
  // one, because on_exchange_failed() zeroes status_poll_failures whenever it counts an auth-shaped
  // failure instead — so an awake, challenge-answering device would score full retries by mistake.
  EXPECT_EQ(decisions::scheduled_poll_max_tries(0, 1), SCHEDULED_POLL_MAX_TRIES);
  EXPECT_EQ(decisions::scheduled_poll_max_tries(0, 2), SCHEDULED_POLL_MAX_TRIES);
  EXPECT_EQ(decisions::scheduled_poll_max_tries(0, UINT8_MAX), SCHEDULED_POLL_MAX_TRIES)
      << "a device answering with a 0x3C challenge is already awake; retries cannot buy a wake-up "
         "and an auth try is the most expensive shape the engine runs";
}

TEST(Decisions, HeaterPollBandStaysThreeTries) {
  // Heaters keep the count-based budget while a cover's polls take a read kind: the band is the
  // same one, so a heater's band poll still gets EXCHANGE_RETRY_COUNT whatever the settling plan does.
  EXPECT_EQ(decisions::scheduled_poll_max_tries(1, 0), 3u);
  EXPECT_EQ(decisions::scheduled_poll_max_tries(3, 0), 3u);
  EXPECT_EQ(decisions::scheduled_poll_max_tries(0, 0), 1u);
  EXPECT_EQ(decisions::scheduled_poll_max_tries(4, 0), 1u);
}

// ============================================================================
// A cover's poll kind — scheduled_poll_read_kind()
// ============================================================================

TEST(Decisions, CoverPollAfterAStopIsSettlingWhateverTheCounters) {
  EXPECT_EQ(decisions::scheduled_poll_read_kind(0, 0, /*settles_a_position=*/true), decisions::ReadKind::SETTLING)
      << "nothing is moving after an accepted STOP, and the user's reversal waits on this poll";
  EXPECT_EQ(decisions::scheduled_poll_read_kind(0, 1, true), decisions::ReadKind::SETTLING)
      << "the mark wins over a stale auth streak; the streak is only history of earlier polls";
  EXPECT_EQ(decisions::scheduled_poll_read_kind(5, 0, true), decisions::ReadKind::SETTLING);
}

TEST(Decisions, CoverPollProgressReadsStaySingleTries) {
  EXPECT_EQ(decisions::scheduled_poll_read_kind(0, 0, false), decisions::ReadKind::PROGRESS)
      << "the first poll after a command fires mid-manoeuvre; a STOP pressed then must not queue behind a burst";
  EXPECT_EQ(decisions::scheduled_poll_read_kind(4, 0, false), decisions::ReadKind::PROGRESS)
      << "past the band the device is unreachable, not asleep";
  EXPECT_EQ(decisions::scheduled_poll_read_kind(0, 1, false), decisions::ReadKind::PROGRESS)
      << "a challenge-answering device is awake; extra attempts buy no wake-up";
  EXPECT_EQ(decisions::scheduled_poll_read_kind(2, 1, false), decisions::ReadKind::PROGRESS)
      << "auth is checked before the status streak";
}

TEST(Decisions, CoverPollLadderBandIsSettling) {
  for (uint8_t failures = SCHEDULED_POLL_RETRY_GRACE_FIRST_FAILURE; failures <= SCHEDULED_POLL_RETRY_GRACE_LAST_FAILURE;
       failures++)
    EXPECT_EQ(decisions::scheduled_poll_read_kind(failures, 0, false), decisions::ReadKind::SETTLING)
        << "failures=" << static_cast<int>(failures);
}

TEST(Decisions, AttemptRequestFromACountKeepsTheCountAndPlansNothing) {
  const decisions::AttemptRequest counted = EXCHANGE_RETRY_COUNT;
  EXPECT_EQ(counted.max_tries, EXCHANGE_RETRY_COUNT);
  EXPECT_FALSE(counted.planned);
  EXPECT_FALSE(counted.is_progress_look());
  const decisions::AttemptRequest single = SCHEDULED_POLL_MAX_TRIES;
  EXPECT_TRUE(single.is_progress_look());
}

TEST(Decisions, AttemptRequestOfKindIsAProgressLookOnlyForProgress) {
  EXPECT_TRUE(decisions::AttemptRequest::of_kind(decisions::ReadKind::PROGRESS).is_progress_look());
  EXPECT_FALSE(decisions::AttemptRequest::of_kind(decisions::ReadKind::SETTLING).is_progress_look());
}

TEST(Decisions, ScheduledPollTriesAuthWinsOverAStatusStreak) {
  // Unreachable while on_exchange_failed() keeps the two counters mutually exclusive; pinned anyway
  // so the predicate's precedence (auth checked first) stays correct if that exclusivity ever
  // relaxes.
  EXPECT_EQ(decisions::scheduled_poll_max_tries(2, 1), SCHEDULED_POLL_MAX_TRIES);
}

// ============================================================================
// Low-power wake belief — is_stop_request(), wake_belief(), low_power_try_preamble()
// ============================================================================

namespace {

constexpr uint32_t WAKE_NOW = 1'000'000;  // Far from 0 so "now - stamp" never underflows by accident.

decisions::TargetEvidence evidence(uint32_t moving_ago_ms, uint32_t seen_ago_ms) {
  // An age of 0 in these tests means "never", i.e. a zero stamp.
  return {moving_ago_ms == 0 ? 0 : WAKE_NOW - moving_ago_ms, seen_ago_ms == 0 ? 0 : WAKE_NOW - seen_ago_ms, false};
}

}  // namespace

TEST(Decisions, TargetEvidenceCarriesTheDeviceStamps) {
  // The engine never sees an IoDevice: this builder is its only view of one, so every field it
  // reads must come from the record, field by field.
  IoDevice dev;
  dev.last_moving_evidence_ms = 1234;
  dev.last_seen_ms = 5678;
  dev.confirms_execute = true;
  dev.moving_until_ms = 9012;
  const decisions::TargetEvidence ev = decisions::target_evidence(dev);
  EXPECT_EQ(ev.last_moving_evidence_ms, 1234u);
  EXPECT_EQ(ev.last_seen_ms, 5678u);
  EXPECT_TRUE(ev.confirms_execute);
  EXPECT_EQ(ev.moving_until_ms, 9012u);
}

TEST(Decisions, MovingUntilHoldsOnlyWhileTheReportedRemainingTimeHasNotRunOut) {
  decisions::TargetEvidence ev{};
  EXPECT_FALSE(decisions::moving_until_holds(ev, WAKE_NOW)) << "0 = no remaining time known";
  ev.moving_until_ms = WAKE_NOW + 1;
  EXPECT_TRUE(decisions::moving_until_holds(ev, WAKE_NOW));
  EXPECT_FALSE(decisions::moving_until_holds(ev, WAKE_NOW + 1)) << "run out at the instant itself";
  EXPECT_FALSE(decisions::moving_until_holds(ev, WAKE_NOW + 5000));
}

TEST(Decisions, MovingUntilSurvivesMillisWrap) {
  decisions::TargetEvidence ev{};
  ev.moving_until_ms = 500u;  // an end 1 s after the wrap, judged from 0.5 s before it
  EXPECT_TRUE(decisions::moving_until_holds(ev, 0xFFFFFFFFu - 499u));
  EXPECT_FALSE(decisions::moving_until_holds(ev, 600u));
}

// ============================================================================
// Unconfirmed accept retry — is_repeatable_execute(), retry_after_unconfirmed_accept_is_safe()
// ============================================================================

namespace {

IoFrame execute_frame(void (*build)(IoFrame &)) {
  IoFrame f{};
  build(f);
  return f;
}

void build_position(IoFrame &f) { create_execute_position(f, test::OWN_ID, test::DST_ID, false, 40); }
void build_stop(IoFrame &f) { create_execute_command(f, test::OWN_ID, test::DST_ID, false, CoverCommand::STOP); }
void build_favorite(IoFrame &f) {
  create_execute_command(f, test::OWN_ID, test::DST_ID, false, CoverCommand::FAVORITE);
}
void build_vent(IoFrame &f) { create_execute_command(f, test::OWN_ID, test::DST_ID, false, CoverCommand::VENT); }
void build_partial(IoFrame &f) { create_execute_command(f, test::OWN_ID, test::DST_ID, false, CoverCommand::PARTIAL); }
void build_pedestrian(IoFrame &f) {
  create_execute_command(f, test::OWN_ID, test::DST_ID, false, CoverCommand::PEDESTRIAN);
}
void build_tilt(IoFrame &f) { create_execute_tilt(f, test::OWN_ID, test::DST_ID, false, 30); }
void build_status_poll(IoFrame &f) { create_get_status(f, test::OWN_ID, test::DST_ID, false); }

}  // namespace

TEST(Decisions, RepeatableExecuteExcludesOnlyTheStoredPositionSelector) {
  EXPECT_TRUE(decisions::is_repeatable_execute(execute_frame(build_position)));
  EXPECT_TRUE(decisions::is_repeatable_execute(execute_frame(build_stop)));
  EXPECT_TRUE(decisions::is_repeatable_execute(execute_frame(build_tilt)));
  EXPECT_FALSE(decisions::is_repeatable_execute(execute_frame(build_favorite)))
      << "a second \"My\" can stop the move the first one started";
  EXPECT_FALSE(decisions::is_repeatable_execute(execute_frame(build_vent))) << "vent uses the same selector";
  EXPECT_FALSE(decisions::is_repeatable_execute(execute_frame(build_partial)))
      << "partial uses the same selector, so a second copy is not sent";
  EXPECT_FALSE(decisions::is_repeatable_execute(execute_frame(build_pedestrian))) << "pedestrian likewise";
  EXPECT_FALSE(decisions::is_repeatable_execute(execute_frame(build_status_poll))) << "not an EXECUTE at all";
}

TEST(Decisions, UnconfirmedNonExecuteAlwaysKeepsItsRetryBudget) {
  const IoFrame poll = execute_frame(build_status_poll);
  EXPECT_TRUE(decisions::retry_after_unconfirmed_accept_is_safe(poll, false, 1));
  EXPECT_TRUE(decisions::retry_after_unconfirmed_accept_is_safe(poll, false, 3));
}

TEST(Decisions, UnconfirmedSubscribeEventIsResentOnce) {
  IoFrame subscribe{};
  ASSERT_TRUE(create_subscribe_event(subscribe, OWN_ID, DST_ID, EVENT_LOCAL_SENSOR, false));
  EXPECT_TRUE(decisions::retry_after_unconfirmed_accept_is_safe(subscribe, false, 1))
      << "the answer carries the reading, so a lost answer is worth one more copy";
  EXPECT_FALSE(
      decisions::retry_after_unconfirmed_accept_is_safe(subscribe, false, UNCONFIRMED_SUBSCRIBE_MAX_RESENDS + 1))
      << "a second silent try is not a one-off";
}

TEST(Decisions, UnconfirmedExecuteIsResentOnceOnlyToADeviceThatConfirms) {
  const IoFrame stop = execute_frame(build_stop);
  EXPECT_FALSE(decisions::retry_after_unconfirmed_accept_is_safe(stop, false, 1))
      << "a device never seen to confirm may simply never do so: silence is normal there";
  EXPECT_TRUE(decisions::retry_after_unconfirmed_accept_is_safe(stop, true, 1))
      << "silence from a device that normally confirms is an anomaly worth one re-send";
  EXPECT_FALSE(decisions::retry_after_unconfirmed_accept_is_safe(stop, true, UNCONFIRMED_EXECUTE_MAX_RESENDS + 1))
      << "a second silent try is not a one-off; a third copy only lengthens the blocked loop";
}

TEST(Decisions, UnconfirmedFavoriteIsNeverResent) {
  EXPECT_FALSE(decisions::retry_after_unconfirmed_accept_is_safe(execute_frame(build_favorite), true, 1));
  EXPECT_FALSE(decisions::retry_after_unconfirmed_accept_is_safe(execute_frame(build_vent), true, 1));
}

TEST(Decisions, IsStopRequestTrueOnlyForAStopExecute) {
  IoFrame stop{};
  ASSERT_TRUE(create_execute_command(stop, OWN_ID, DST_ID, true, CoverCommand::STOP, false));
  EXPECT_TRUE(decisions::is_stop_request(stop));

  IoFrame position{};
  ASSERT_TRUE(create_execute_position(position, OWN_ID, DST_ID, true, 40));
  EXPECT_FALSE(decisions::is_stop_request(position));

  IoFrame favorite{};
  ASSERT_TRUE(create_execute_command(favorite, OWN_ID, DST_ID, true, CoverCommand::FAVORITE, false));
  EXPECT_FALSE(decisions::is_stop_request(favorite)) << "only the STOP main byte counts";

  IoFrame poll{};
  ASSERT_TRUE(create_get_status(poll, OWN_ID, DST_ID, true));
  EXPECT_FALSE(decisions::is_stop_request(poll));

  IoFrame truncated = stop;
  truncated.data_len = EXECUTE_MAIN_BYTE_OFFSET;  // main byte not part of the payload any more
  EXPECT_FALSE(decisions::is_stop_request(truncated)) << "a short payload must not be read past its length";
}

TEST(Decisions, WakeBeliefIsAsleepWithoutEvidence) {
  EXPECT_EQ(decisions::wake_belief(evidence(0, 0), WAKE_NOW, false), decisions::WakeBelief::ASLEEP);
}

TEST(Decisions, WakeBeliefStopAloneIsAwake) {
  EXPECT_EQ(decisions::wake_belief(evidence(0, 0), WAKE_NOW, true), decisions::WakeBelief::AWAKE)
      << "a STOP is only sent to a receiver that is moving, whatever the stamps say";
}

TEST(Decisions, WakeBeliefMovingEvidenceIsAwakeUntilMaxTravel) {
  EXPECT_EQ(decisions::wake_belief(evidence(LOW_POWER_MAX_TRAVEL_MS - 1, 0), WAKE_NOW, false),
            decisions::WakeBelief::AWAKE);
  EXPECT_NE(decisions::wake_belief(evidence(LOW_POWER_MAX_TRAVEL_MS, 0), WAKE_NOW, false), decisions::WakeBelief::AWAKE)
      << "moving evidence expires at exactly LOW_POWER_MAX_TRAVEL_MS";
}

TEST(Decisions, WakeBeliefExpiredMovingEvidenceIsAsleep) {
  EXPECT_EQ(decisions::wake_belief(evidence(LOW_POWER_MAX_TRAVEL_MS, 0), WAKE_NOW, false),
            decisions::WakeBelief::ASLEEP)
      << "no frame from the device since the move: it has long since finished and gone back to sleep";
}

TEST(Decisions, WakeBeliefRecentFrameAloneIsAsleep) {
  // Field logs (VELUX SSL, ADR 0040 amendment): a resting receiver ignored the short preamble even
  // 34 ms after it had answered, so having heard from it is no reason to lead short.
  EXPECT_EQ(decisions::wake_belief(evidence(0, 34), WAKE_NOW, false), decisions::WakeBelief::ASLEEP);
  EXPECT_EQ(decisions::wake_belief(evidence(0, 5000), WAKE_NOW, false), decisions::WakeBelief::ASLEEP);
}

TEST(Decisions, WakeBeliefStaleMovingEvidenceWithARecentFrameIsAsleep) {
  // The move ended long ago (evidence past MAX_TRAVEL) but the device spoke 5 s ago.
  EXPECT_EQ(decisions::wake_belief(evidence(LOW_POWER_MAX_TRAVEL_MS + 1, 5000), WAKE_NOW, false),
            decisions::WakeBelief::ASLEEP);
}

TEST(Decisions, WakeBeliefAgeArithmeticSurvivesMillisWrap) {
  // Stamp taken 1 s before millis() wraps; now is 1 s after the wrap: an age of 2 s.
  const decisions::TargetEvidence wrapped{0xFFFFFFFFu - 999u, 0, false};
  EXPECT_EQ(decisions::wake_belief(wrapped, 1000u, false), decisions::WakeBelief::AWAKE);
}

TEST(Decisions, WakeBeliefStampEqualToNowIsFresh) {
  // Age 0 is a legitimately fresh stamp (taken this very millisecond), unlike a zero *stamp*.
  EXPECT_EQ(decisions::wake_belief({WAKE_NOW, 0, false}, WAKE_NOW, false), decisions::WakeBelief::AWAKE);
}

TEST(Decisions, LowPowerTryPreambleFollowsThePlanTable) {
  constexpr uint16_t SHORT = 32;
  constexpr uint16_t WAKE = LONG_PREAMBLE;
  struct Row {
    decisions::WakeBelief belief;
    uint16_t plan[EXCHANGE_RETRY_COUNT];
  };
  const Row rows[] = {
      {decisions::WakeBelief::AWAKE, {SHORT, WAKE, WAKE}},
      {decisions::WakeBelief::ASLEEP, {WAKE, WAKE, WAKE}},
  };
  for (const Row &row : rows) {
    for (uint8_t try_index = 1; try_index <= EXCHANGE_RETRY_COUNT; try_index++) {
      EXPECT_EQ(decisions::low_power_try_preamble(row.belief, try_index, SHORT, WAKE), row.plan[try_index - 1])
          << decisions::wake_belief_name(row.belief) << " try " << static_cast<int>(try_index);
    }
  }
}

TEST(Decisions, LowPowerTryPreambleClampsTheTryIndex) {
  constexpr uint16_t SHORT = 32;
  EXPECT_EQ(decisions::low_power_try_preamble(decisions::WakeBelief::AWAKE, 0, SHORT, LONG_PREAMBLE), SHORT)
      << "try 0 is treated as try 1";
  EXPECT_EQ(decisions::low_power_try_preamble(decisions::WakeBelief::AWAKE, 4, SHORT, LONG_PREAMBLE), LONG_PREAMBLE)
      << "try 4 is treated as the last try (wake-up for AWAKE)";
}

TEST(Decisions, LowPowerTryPreambleUsesTheTunedPreambles) {
  EXPECT_EQ(decisions::low_power_try_preamble(decisions::WakeBelief::AWAKE, 1, 48, 2048), 48)
      << "the short preamble is the caller's tuned value, not a fixed constant";
  EXPECT_EQ(decisions::low_power_try_preamble(decisions::WakeBelief::AWAKE, 2, 48, 2048), 2048)
      << "the wake-up preamble is the caller's tuned value, not LONG_PREAMBLE";
  EXPECT_EQ(decisions::low_power_try_preamble(decisions::WakeBelief::ASLEEP, 1, 48, 2048), 2048);
}

TEST(Decisions, WakeBeliefNamesAreDistinct) {
  EXPECT_STREQ(decisions::wake_belief_name(decisions::WakeBelief::ASLEEP), "asleep");
  EXPECT_STREQ(decisions::wake_belief_name(decisions::WakeBelief::AWAKE), "awake");
}

// ========================================================================================
// Wake belief of a receiver the hub has no news of (UNKNOWN), and the short lead
// ========================================================================================

TEST(Decisions, StatusReadWithNoNewsOfTheReceiverIsUnknown) {
  EXPECT_EQ(decisions::wake_belief(evidence(0, 0), WAKE_NOW, false, /*is_status_read=*/true),
            decisions::WakeBelief::UNKNOWN);
  EXPECT_EQ(decisions::wake_belief(evidence(0, 0), WAKE_NOW, false, /*is_status_read=*/false),
            decisions::WakeBelief::ASLEEP)
      << "a command keeps the rule it had";
}

TEST(Decisions, MovingEvidenceOrAStopStillMakesAReadAwake) {
  EXPECT_EQ(decisions::wake_belief(evidence(1000, 0), WAKE_NOW, false, true), decisions::WakeBelief::AWAKE);
  EXPECT_EQ(decisions::wake_belief(evidence(0, 0), WAKE_NOW, true, true), decisions::WakeBelief::AWAKE);
}

TEST(Decisions, ARecentStatusReportIsNewsSoTheReadIsAsleepNotUnknown) {
  decisions::TargetEvidence reported = evidence(0, 0);
  reported.last_status_ms = WAKE_NOW - 1000;
  EXPECT_EQ(decisions::wake_belief(reported, WAKE_NOW, false, true), decisions::WakeBelief::ASLEEP);
  reported.last_status_ms = WAKE_NOW - (LOW_POWER_MAX_TRAVEL_MS - 1);
  EXPECT_EQ(decisions::wake_belief(reported, WAKE_NOW, false, true), decisions::WakeBelief::ASLEEP);
}

TEST(Decisions, AStatusReportOlderThanTheLongestTravelIsNoNews) {
  decisions::TargetEvidence reported = evidence(0, 0);
  reported.last_status_ms = WAKE_NOW - LOW_POWER_MAX_TRAVEL_MS;
  EXPECT_EQ(decisions::wake_belief(reported, WAKE_NOW, false, true), decisions::WakeBelief::UNKNOWN)
      << "expires at exactly LOW_POWER_MAX_TRAVEL_MS";
}

TEST(Decisions, AReadMarkedStateUnknownIsUnknownDespiteARecentStatusReport) {
  // A catch-up read: activity the hub may have missed can have set the receiver moving after it
  // reported "stopped", so that report is no proof it rests.
  decisions::TargetEvidence reported = evidence(0, 0);
  reported.last_status_ms = WAKE_NOW - 1000;
  EXPECT_EQ(decisions::wake_belief(reported, WAKE_NOW, false, true, /*state_unknown=*/true),
            decisions::WakeBelief::UNKNOWN);
  EXPECT_EQ(decisions::wake_belief(reported, WAKE_NOW, false, true, /*state_unknown=*/false),
            decisions::WakeBelief::ASLEEP)
      << "unmarked, the same report keeps the read ASLEEP";
}

TEST(Decisions, StateUnknownNeverOverridesMovingEvidenceAStopOrACommand) {
  EXPECT_EQ(decisions::wake_belief(evidence(1000, 0), WAKE_NOW, false, true, true), decisions::WakeBelief::AWAKE)
      << "fresh moving evidence already leads short";
  EXPECT_EQ(decisions::wake_belief(evidence(0, 0), WAKE_NOW, true, true, true), decisions::WakeBelief::AWAKE);
  EXPECT_EQ(decisions::wake_belief(evidence(0, 0), WAKE_NOW, false, /*is_status_read=*/false, true),
            decisions::WakeBelief::ASLEEP)
      << "only a status read takes the mark";
}

TEST(Decisions, WithStateUnknownKeepsTheReadKindAndTheOtherMarks) {
  const auto request =
      decisions::AttemptRequest::of_kind(decisions::ReadKind::SETTLING).with_wake_preamble_first().with_state_unknown();
  EXPECT_TRUE(request.planned);
  EXPECT_EQ(request.kind, decisions::ReadKind::SETTLING);
  EXPECT_TRUE(request.wake_preamble_first);
  EXPECT_TRUE(request.state_unknown);
  EXPECT_FALSE(decisions::AttemptRequest::of_kind(decisions::ReadKind::SETTLING).state_unknown);
}

TEST(Decisions, TheAgeOfAStatusReportSurvivesAMillisWrap) {
  decisions::TargetEvidence reported = evidence(0, 0);
  reported.last_status_ms = 0xFFFFFFFFu - 999u;  // one second before the wrap; now is one second after it
  EXPECT_EQ(decisions::wake_belief(reported, 1000u, false, true), decisions::WakeBelief::ASLEEP);
}

TEST(Decisions, UnknownSendsTheWakeUpPreambleOnEveryTryOfAnExchangeWithoutALead) {
  // The lead of an unknown state is an attempt of the plan, not a preamble rule: a single try, or a
  // count of tries, keeps the preamble a resting receiver hears.
  for (uint8_t try_index = 1; try_index <= EXCHANGE_RETRY_COUNT; try_index++)
    EXPECT_EQ(decisions::low_power_try_preamble(decisions::WakeBelief::UNKNOWN, try_index, 32, LONG_PREAMBLE),
              LONG_PREAMBLE);
  EXPECT_STREQ(decisions::wake_belief_name(decisions::WakeBelief::UNKNOWN), "unknown");
}

TEST(Decisions, TargetEvidenceCarriesTheLastStatusReport) {
  IoDevice dev{};
  dev.last_status = 4242;
  EXPECT_EQ(decisions::target_evidence(dev).last_status_ms, 4242u);
}

// ============================================================================
// Attempt plan — attempt_plan()
// ============================================================================

namespace {
constexpr uint32_t MAINS_TX_MS = 12;       // a status poll behind the default start preamble
constexpr uint32_t LOW_POWER_TX_MS = 240;  // the same poll behind the 1024-byte wake-up preamble
}  // namespace

TEST(AttemptPlan, SettlingReadOfAMainsTargetListensShortOnEveryAttemptButTheLast) {
  const TuningConfig tuning;  // 200 ms early window, 400 ms start window, 2500 ms budget
  const auto plan = decisions::attempt_plan(decisions::ReadKind::SETTLING, false, tuning, MAINS_TX_MS);

  EXPECT_EQ(plan.gap_ms, 0) << "silent attempts follow each other";
  EXPECT_EQ(plan.window_ms, tuning.exchange_start_response_wait_ms) << "the last attempt listens the whole window";
  EXPECT_EQ(plan.early_window_ms, tuning.exchange_early_response_wait_ms);
  EXPECT_EQ(plan.max_attempts, EXCHANGE_MAX_ATTEMPTS)
      << "8 x (12 + 10 + 200) + (12 + 10 + 400) = 2198 ms: nine fit with room to spare";
  EXPECT_EQ(plan.window_for_attempt(1, plan.max_attempts), 200);
  EXPECT_EQ(plan.window_for_attempt(8, plan.max_attempts), 200);
  EXPECT_EQ(plan.window_for_attempt(9, plan.max_attempts), 400);
}

TEST(AttemptPlan, EarlyWindowIsTheLargestValueThatKeepsNineAttemptsAtTheDefaultBudget) {
  // Pins the default's evidence (EXCHANGE_EARLY_RESPONSE_WAIT_MS): 200 ms still fits nine attempts,
  // 250 ms does not.
  TuningConfig tuning;
  tuning.exchange_early_response_wait_ms = 250;
  EXPECT_EQ(decisions::attempt_plan(decisions::ReadKind::COMMAND, false, tuning, MAINS_TX_MS).max_attempts, 8);
  tuning.exchange_early_response_wait_ms = EXCHANGE_EARLY_RESPONSE_WAIT_MS;
  EXPECT_EQ(decisions::attempt_plan(decisions::ReadKind::COMMAND, false, tuning, MAINS_TX_MS).max_attempts, 9);
}

TEST(AttemptPlan, EarlyWindowAboveTheStartWindowIsCappedToIt) {
  TuningConfig tuning;
  tuning.exchange_early_response_wait_ms = 1000;
  const auto plan = decisions::attempt_plan(decisions::ReadKind::SETTLING, false, tuning, MAINS_TX_MS);
  EXPECT_EQ(plan.early_window_ms, tuning.exchange_start_response_wait_ms);
  EXPECT_EQ(plan.max_attempts, 5) << "5 x (12 + 10 + 400) = 2110 ms; a sixth would end at 2532 ms";
}

TEST(AttemptPlan, AttemptsThatFitCountsTheOverheadAndTheLongLastWindow) {
  // Equal windows reduce to (budget - 1) / attempt: 2499 / 422 = 5, the five attempts 422 ms apart
  // that a silent target drew on the bench boards.
  EXPECT_EQ(decisions::attempts_that_fit(2500, 12, 400, 400, EXCHANGE_MAX_ATTEMPTS), 5);
  EXPECT_EQ(decisions::attempts_that_fit(1000, 12, 200, 400, EXCHANGE_MAX_ATTEMPTS), 3);
  EXPECT_EQ(decisions::attempts_that_fit(2500, 12, 200, 400, 4), 4) << "the cap wins over the budget";
  EXPECT_EQ(decisions::attempts_that_fit(300, 12, 200, 400, EXCHANGE_MAX_ATTEMPTS), 1) << "the first always runs";
  EXPECT_EQ(decisions::attempts_that_fit(0, 12, 200, 400, 7), 7) << "no budget is no limit the plan can honour";
}

TEST(AttemptPlan, AttemptFitsIsStrictlyBelowTheBudgetAndCountsEveryPart) {
  constexpr uint32_t part_total = 100 + 50 + 12 + EXCHANGE_ATTEMPT_OVERHEAD_MS + 400 + 520;
  EXPECT_TRUE(decisions::attempt_fits(100, 50, 12, 400, 520, part_total + 1));
  EXPECT_FALSE(decisions::attempt_fits(100, 50, 12, 400, 520, part_total)) << "ending at the budget is over it";
}

TEST(AttemptPlan, EarlyAttemptsOfAPlanWithoutAnEarlyWindowListenTheWholeWindow) {
  decisions::AttemptPlan plan;
  plan.window_ms = 400;
  EXPECT_EQ(plan.window_for_attempt(1, 3), 400);
  EXPECT_EQ(plan.window_for_attempt(3, 3), 400);
}

TEST(AttemptPlan, SettlingReadOfALowPowerTargetListensTheLowPowerWindowThreeTimes) {
  const TuningConfig tuning;
  const auto plan = decisions::attempt_plan(decisions::ReadKind::SETTLING, true, tuning, LOW_POWER_TX_MS);

  EXPECT_EQ(plan.gap_ms, 0);
  EXPECT_EQ(plan.window_ms, LOW_POWER_RESPONSE_WAIT_MS) << "not the tuned start window";
  EXPECT_EQ(plan.max_attempts, LOW_POWER_SETTLING_ATTEMPTS);
  EXPECT_LT(LOW_POWER_SETTLING_ATTEMPTS * (LOW_POWER_TX_MS + LOW_POWER_RESPONSE_WAIT_MS),
            static_cast<uint32_t>(tuning.exchange_total_budget_ms))
      << "three wake-up attempts fit the default budget without gaps";
}

TEST(AttemptPlan, SettlingReadIsCappedWhateverTheBudget) {
  TuningConfig tuning;
  tuning.exchange_total_budget_ms = 12000;  // the highest the YAML accepts
  EXPECT_EQ(decisions::attempt_plan(decisions::ReadKind::SETTLING, false, tuning, MAINS_TX_MS).max_attempts,
            EXCHANGE_MAX_ATTEMPTS);
  EXPECT_EQ(decisions::attempt_plan(decisions::ReadKind::SETTLING, true, tuning, LOW_POWER_TX_MS).max_attempts,
            LOW_POWER_SETTLING_ATTEMPTS);
}

TEST(AttemptPlan, ABudgetBelowOneAttemptStillAllowsOne) {
  TuningConfig tuning;
  tuning.exchange_total_budget_ms = 300;
  EXPECT_EQ(decisions::attempt_plan(decisions::ReadKind::SETTLING, false, tuning, MAINS_TX_MS).max_attempts, 1);
  tuning.exchange_total_budget_ms = 0;
  EXPECT_EQ(decisions::attempt_plan(decisions::ReadKind::SETTLING, true, tuning, LOW_POWER_TX_MS).max_attempts,
            LOW_POWER_SETTLING_ATTEMPTS)
      << "a zero budget is no limit the plan can honour; the engine's guard still applies";
}

TEST(AttemptPlan, LongerWindowFitsFewerAttempts) {
  TuningConfig tuning;
  tuning.exchange_start_response_wait_ms = 1000;
  // 200 ms early, 1000 ms last: 1 + (2499 - 1022) / 222 = 7.
  EXPECT_EQ(decisions::attempt_plan(decisions::ReadKind::SETTLING, false, tuning, MAINS_TX_MS).max_attempts, 7);
}

TEST(AttemptPlan, ProgressReadIsASingleAttempt) {
  const TuningConfig tuning;
  for (const bool low_power : {false, true}) {
    const auto plan = decisions::attempt_plan(decisions::ReadKind::PROGRESS, low_power, tuning, MAINS_TX_MS);
    EXPECT_EQ(plan.max_attempts, SCHEDULED_POLL_MAX_TRIES);
    EXPECT_EQ(plan.gap_ms, EXCHANGE_RETRY_DELAY_MS);
    EXPECT_EQ(plan.window_ms, tuning.exchange_start_response_wait_ms);
  }
}

TEST(AttemptPlan, CommandToAMainsTargetTakesTheGaplessPlanWithTheEarlyWindow) {
  const TuningConfig tuning;
  const auto plan = decisions::attempt_plan(decisions::ReadKind::COMMAND, false, tuning, MAINS_TX_MS);
  EXPECT_EQ(plan.max_attempts, EXCHANGE_MAX_ATTEMPTS);
  EXPECT_EQ(plan.gap_ms, 0);
  EXPECT_EQ(plan.early_window_ms, tuning.exchange_early_response_wait_ms);
  EXPECT_EQ(plan.window_ms, tuning.exchange_start_response_wait_ms);
}

TEST(AttemptPlan, CommandToALowPowerTargetKeepsTodaysThreeAttemptsWithTheRetryGap) {
  const TuningConfig tuning;
  const auto plan = decisions::attempt_plan(decisions::ReadKind::COMMAND, true, tuning, LOW_POWER_TX_MS);
  EXPECT_EQ(plan.max_attempts, EXCHANGE_RETRY_COUNT);
  EXPECT_EQ(plan.gap_ms, EXCHANGE_RETRY_DELAY_MS);
  EXPECT_EQ(plan.window_ms, tuning.exchange_start_response_wait_ms);
  EXPECT_EQ(plan.early_window_ms, 0) << "a low-power target keeps one window for every attempt";
}

namespace {

/// Attempts the engine's guard starts for a silent target: the first always, each later one only
/// while the plan obeys the time budget and decisions::attempt_fits() passes against it.
uint8_t attempts_the_guard_starts(const decisions::AttemptPlan &plan, uint32_t budget_ms, uint32_t tx_ms) {
  uint32_t elapsed_ms = 0;
  uint8_t started = 0;
  for (uint8_t attempt = 1; attempt <= plan.max_attempts; attempt++) {
    const uint32_t gap_ms = attempt > 1 ? plan.gap_ms : 0;
    const uint32_t window_ms = plan.window_for_attempt(attempt, plan.max_attempts);
    if (attempt > 1 && decisions::obeys_time_budget(plan) &&
        !decisions::attempt_fits(elapsed_ms, gap_ms, tx_ms, window_ms, 0, budget_ms))
      break;
    started++;
    elapsed_ms += gap_ms + tx_ms + EXCHANGE_ATTEMPT_OVERHEAD_MS + window_ms;
  }
  return started;
}

// Transmit times the radios estimate for a start frame behind the wake-up preamble, at 38.4 kbit/s plus the
// frame: 8 bits per preamble byte on the software-PHY chips, 10 on the SX1276.
constexpr uint32_t WAKE_TX_MS_BY_PREAMBLE[][2] = {{1024, 218}, {2048, 432}, {4096, 859},
                                                  {1024, 272}, {2048, 539}, {4096, 1072}};

}  // namespace

TEST(AttemptPlan, ALowPowerSettlingReadKeepsThreeAttemptsAtEveryWakeUpLength) {
  const TuningConfig tuning;
  for (const auto &row : WAKE_TX_MS_BY_PREAMBLE) {
    SCOPED_TRACE(::testing::Message() << "wake-up preamble " << row[0] << " B, " << row[1] << " ms on air");
    const auto plan = decisions::attempt_plan(decisions::ReadKind::SETTLING, true, tuning, row[1]);
    EXPECT_EQ(plan.max_attempts, LOW_POWER_SETTLING_ATTEMPTS);
    EXPECT_TRUE(plan.bounded_by_attempts);
    EXPECT_FALSE(decisions::obeys_time_budget(plan));
    EXPECT_EQ(attempts_the_guard_starts(plan, tuning.exchange_total_budget_ms, row[1]), LOW_POWER_SETTLING_ATTEMPTS)
        << "the guard starts every attempt";
  }
}

TEST(AttemptPlan, ALowPowerCommandKeepsThreeAttemptsAtEveryWakeUpLength) {
  const TuningConfig tuning;
  for (const auto &row : WAKE_TX_MS_BY_PREAMBLE) {
    SCOPED_TRACE(::testing::Message() << "wake-up preamble " << row[0] << " B, " << row[1] << " ms on air");
    const auto plan = decisions::attempt_plan(decisions::ReadKind::COMMAND, true, tuning, row[1]);
    EXPECT_EQ(plan.max_attempts, EXCHANGE_RETRY_COUNT);
    EXPECT_TRUE(plan.bounded_by_attempts);
    EXPECT_EQ(attempts_the_guard_starts(plan, tuning.exchange_total_budget_ms, row[1]), EXCHANGE_RETRY_COUNT);
  }
}

TEST(AttemptPlan, ALowPowerPlanKeepsAllItsAttemptsAtAUserBudgetOfOneSecond) {
  TuningConfig tuning;
  tuning.exchange_total_budget_ms = 1000;
  for (const auto kind : {decisions::ReadKind::SETTLING, decisions::ReadKind::COMMAND}) {
    const auto plan = decisions::attempt_plan(kind, true, tuning, 432);
    EXPECT_EQ(attempts_the_guard_starts(plan, tuning.exchange_total_budget_ms, 432), plan.max_attempts)
        << "kind=" << static_cast<int>(kind);
  }
}

TEST(AttemptPlan, MainsAndProgressPlansObeyTheTimeBudget) {
  const TuningConfig tuning;
  for (const auto kind : {decisions::ReadKind::PROGRESS, decisions::ReadKind::SETTLING, decisions::ReadKind::COMMAND})
    EXPECT_TRUE(decisions::obeys_time_budget(decisions::attempt_plan(kind, false, tuning, 432)));
  EXPECT_TRUE(decisions::obeys_time_budget(decisions::attempt_plan(decisions::ReadKind::PROGRESS, true, tuning, 432)))
      << "a mid-travel look is one attempt";
  EXPECT_TRUE(decisions::obeys_time_budget(decisions::attempt_plan_for_tries(EXCHANGE_RETRY_COUNT, 400)));
}

TEST(AttemptPlan, ALowPowerPlanNeverHasMoreAttemptsThanTheThreeWakeUpsAndTheLead) {
  // The count is the only bound of such a plan, so it is bounded independently of the budget.
  for (const uint32_t budget_ms : {0u, 1u, 1000u, 2500u, 12000u, 65535u}) {
    TuningConfig tuning;
    tuning.exchange_total_budget_ms = budget_ms;
    for (const auto kind : {decisions::ReadKind::SETTLING, decisions::ReadKind::COMMAND}) {
      for (const bool lead : {false, true}) {
        const auto plan = decisions::attempt_plan(kind, true, tuning, 432, 0, lead);
        EXPECT_LE(plan.max_attempts, LOW_POWER_SETTLING_ATTEMPTS + 1u) << "budget=" << budget_ms;
        EXPECT_LE(plan.max_attempts, EXCHANGE_MAX_ATTEMPTS);
      }
    }
  }
}

TEST(AttemptPlan, ExplicitLastWindowReplacesTheTunedStartWindow) {
  const TuningConfig tuning;
  const auto mains = decisions::attempt_plan(decisions::ReadKind::COMMAND, false, tuning, MAINS_TX_MS, 500);
  EXPECT_EQ(mains.window_ms, 500);
  EXPECT_EQ(mains.early_window_ms, tuning.exchange_early_response_wait_ms);
  const auto low_power = decisions::attempt_plan(decisions::ReadKind::COMMAND, true, tuning, LOW_POWER_TX_MS, 500);
  EXPECT_EQ(low_power.window_ms, 500);
  EXPECT_EQ(low_power.max_attempts, EXCHANGE_RETRY_COUNT);
}

// === Which requests may be repeated ===

TEST(AttemptPlan, RepeatSafeCommandsAreEverythingButAStoredPositionExecute) {
  IoFrame request{};
  create_execute_position(request, test::OWN_ID, test::DST_ID, false, 40);
  EXPECT_TRUE(decisions::is_repeat_safe_command(request)) << "an absolute position";
  create_execute_command(request, test::OWN_ID, test::DST_ID, false, CoverCommand::STOP);
  EXPECT_TRUE(decisions::is_repeat_safe_command(request)) << "stop";
  create_execute_command(request, test::OWN_ID, test::DST_ID, false, CoverCommand::FORCE_OPEN);
  EXPECT_TRUE(decisions::is_repeat_safe_command(request)) << "open";
  create_execute_command(request, test::OWN_ID, test::DST_ID, false, CoverCommand::VENT);
  EXPECT_FALSE(decisions::is_repeat_safe_command(request)) << "a stored position, like 'My'";
  create_execute_command(request, test::OWN_ID, test::DST_ID, false, CoverCommand::FAVORITE);
  EXPECT_FALSE(decisions::is_repeat_safe_command(request))
      << "'My' stops a moving cover: a second copy undoes the first";
  create_get_status(request, test::OWN_ID, test::DST_ID, false);
  EXPECT_TRUE(decisions::is_repeat_safe_command(request)) << "a read";
}

TEST(AttemptPlan, PlanForTriesClampsToOneAndTheDefaultCount) {
  EXPECT_EQ(decisions::attempt_plan_for_tries(0, 400).max_attempts, 1);
  EXPECT_EQ(decisions::attempt_plan_for_tries(2, 400).max_attempts, 2);
  EXPECT_EQ(decisions::attempt_plan_for_tries(9, 400).max_attempts, EXCHANGE_RETRY_COUNT);
  EXPECT_EQ(decisions::attempt_plan_for_tries(2, 450).window_ms, 450);
}
// ========================================================================================
// Short lead of a settling read to a low-power receiver in an unknown state (ADR 0079)
// ========================================================================================

namespace {
constexpr uint32_t LEAD_WAKE_TX_MS =
    218;  // a status read behind the 1024-byte wake-up preamble, as the radios estimate it
}  // namespace

TEST(Decisions, ASettlingReadToAnUnknownLowPowerTargetLeadsWithOneShortAttempt) {
  TuningConfig tuning;
  const auto plan = decisions::attempt_plan(decisions::ReadKind::SETTLING, true, tuning, LEAD_WAKE_TX_MS,
                                            /*last_window_ms=*/0, /*with_short_lead=*/true);
  EXPECT_TRUE(plan.short_lead);
  EXPECT_EQ(plan.max_attempts, LOW_POWER_SETTLING_ATTEMPTS + 1u);
  EXPECT_EQ(plan.window_for_attempt(1, plan.max_attempts), LOW_POWER_SHORT_LEAD_WINDOW_MS);
  EXPECT_EQ(plan.window_for_attempt(2, plan.max_attempts), LOW_POWER_RESPONSE_WAIT_MS);
  EXPECT_EQ(plan.window_for_attempt(plan.max_attempts, plan.max_attempts), LOW_POWER_RESPONSE_WAIT_MS);
}

TEST(Decisions, ALeadPlanKeepsItsLeadAndEveryWakeUpAttemptWhateverTheBudget) {
  for (const uint32_t budget_ms : {600u, 1000u, 2400u, 2500u, 12000u}) {
    TuningConfig tuning;
    tuning.exchange_total_budget_ms = budget_ms;
    const auto without = decisions::attempt_plan(decisions::ReadKind::SETTLING, true, tuning, LEAD_WAKE_TX_MS);
    const auto with = decisions::attempt_plan(decisions::ReadKind::SETTLING, true, tuning, LEAD_WAKE_TX_MS, 0, true);

    EXPECT_TRUE(with.short_lead);
    EXPECT_EQ(with.max_attempts, without.max_attempts + 1u) << "no wake-up attempt is dropped; budget=" << budget_ms;
    EXPECT_FALSE(decisions::obeys_time_budget(with));
    EXPECT_EQ(attempts_the_guard_starts(with, budget_ms, LEAD_WAKE_TX_MS), with.max_attempts);
  }
}

TEST(Decisions, NoLeadForAnotherKindOfRead) {
  TuningConfig tuning;
  tuning.exchange_total_budget_ms = EXCHANGE_TOTAL_BUDGET_MS;
  EXPECT_FALSE(
      decisions::attempt_plan(decisions::ReadKind::PROGRESS, true, tuning, LEAD_WAKE_TX_MS, 0, true).short_lead);
  EXPECT_FALSE(
      decisions::attempt_plan(decisions::ReadKind::COMMAND, true, tuning, LEAD_WAKE_TX_MS, 0, true).short_lead);
  EXPECT_FALSE(
      decisions::attempt_plan(decisions::ReadKind::SETTLING, false, tuning, LEAD_WAKE_TX_MS, 0, true).short_lead);
}

TEST(AttemptPlan, NoReadKindPowerClassTuningOrTryCountGivesAPlanOutsideTheAttemptBounds) {
  // The engine runs the plan's attempt count as it is (no clamp of its own), so the plan builders
  // own the range [1, EXCHANGE_MAX_ATTEMPTS].
  const auto in_bounds = [](const decisions::AttemptPlan &plan) {
    return plan.max_attempts >= 1 && plan.max_attempts <= EXCHANGE_MAX_ATTEMPTS;
  };
  for (unsigned tries = 0; tries <= UINT8_MAX; tries++)
    ASSERT_TRUE(in_bounds(decisions::attempt_plan_for_tries(static_cast<uint8_t>(tries), 400))) << "tries=" << tries;

  const decisions::ReadKind kinds[] = {decisions::ReadKind::PROGRESS, decisions::ReadKind::SETTLING,
                                       decisions::ReadKind::COMMAND};
  const bool leads[] = {false, true};
  for (const auto kind : kinds) {
    for (const bool low_power : {false, true}) {
      for (const uint16_t budget_ms : {0, 1, 100, 600, 1000, 2400, 2500, 5000, 12000, 65535}) {
        for (const uint16_t early_ms : {0, 1, 200, 400, 65535}) {
          for (const uint16_t start_ms : {0, 1, 200, 400, 1000, 65535}) {
            for (const uint32_t tx_ms : {0, 1, 12, 218, 5000}) {
              for (const uint16_t last_ms : {0, 400}) {
                for (const auto &lead : leads) {
                  TuningConfig tuning;
                  tuning.exchange_total_budget_ms = budget_ms;
                  tuning.exchange_early_response_wait_ms = early_ms;
                  tuning.exchange_start_response_wait_ms = start_ms;
                  const auto plan = decisions::attempt_plan(kind, low_power, tuning, tx_ms, last_ms, lead);
                  ASSERT_TRUE(in_bounds(plan))
                      << "kind=" << static_cast<int>(kind) << " low_power=" << low_power << " budget=" << budget_ms
                      << " early=" << early_ms << " start=" << start_ms << " tx=" << tx_ms << " last=" << last_ms
                      << " lead=" << lead << " attempts=" << static_cast<int>(plan.max_attempts);
                }
              }
            }
          }
        }
      }
    }
  }
}

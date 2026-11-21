/// @file exchange_attempt_plan_test.cpp
/// @brief Tests for the attempt plans of the exchange engine: the window-aware budget guard, the short lead of a
/// settling read and the one options value an exchange is planned from.

#include "hub_exchange.h"
#include "hub_core.h"
#include "proto_frame.h"
#include "proto_commands.h"
#include "proto_crypto.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"

#include <cstring>
#include <vector>

#include "exchange_fixture.h"

using namespace esphome::home_io_control;
using namespace exchange_test;

// ============================================================================
// Attempt plans and the window-aware budget guard. A try starts only if its gap, transmission,
// first-reply window and, once the target has challenged, the final-reply window all end inside
// exchange_total_budget_ms. Every case runs on a ManualClock with modelled air time.
// ============================================================================

namespace {

/// An exchange engine on a ManualClock with modelled air time and a configurable tuning.
struct PlanRig {
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
  RadioDriver *radio_ptr{&radio};
  TuningConfig tuning{};
  ExchangeEngine engine{&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning};

  PlanRig() { radio.set_model_tx_airtime(true); }

  /// One attempt's transmit time for a status poll to a mains or low-power target, as the engine
  /// estimates it.
  uint32_t attempt_tx_ms(bool low_power) const {
    IoFrame request{};
    create_get_status(request, test::OWN_ID, test::DST_ID, low_power);
    uint8_t raw[64];
    const uint8_t len = serialize(request, raw, sizeof(raw));
    const uint16_t preamble =
        low_power ? mock_preamble_bytes(tuning.low_power_wake_ms) : mock_preamble_bytes(tuning.start_preamble_ms);
    return (io868_tx_air_time_us(preamble, len, SOFT_PHY_PREAMBLE_BITS_PER_BYTE) + 999) / 1000;
  }

  /// Run a status poll to a target that never answers, with @p options.
  /// @return The wall-clock time the exchange blocked.
  uint32_t run_silent_poll(const ExchangeOptions &options, bool low_power) {
    IoFrame request{};
    create_get_status(request, test::OWN_ID, test::DST_ID, low_power);
    IoFrame response{};
    const uint32_t start = esphome::test_clock::peek_ms();
    EXPECT_EQ(engine.send_and_receive(request, response, FREQ_CH2, options), ExchangeOutcome::FAILED);
    return esphome::test_clock::peek_ms() - start;
  }
};

/// An upper bound on the time @p attempts attempts to a silent target take, worked out from the attempts alone
/// (transmission, overhead, listen window and gap), independent of the guard and the budget it applies.
uint32_t silent_attempts_bound_ms(const PlanRig &rig, uint32_t attempts, uint32_t gap_ms, uint32_t window_ms,
                                  bool low_power) {
  return attempts * (rig.attempt_tx_ms(low_power) + EXCHANGE_ATTEMPT_OVERHEAD_MS + window_ms) +
         (attempts - 1U) * gap_ms;
}

}  // namespace

TEST(Exchange, SettlingReadToASilentMainsTargetMakesTheFittingNumberOfAttemptsWithoutAGap) {
  PlanRig rig;
  IoFrame status{};
  create_get_status(status, test::OWN_ID, test::DST_ID, false);
  const auto plan = expected_plan(rig.engine, rig.tuning, decisions::ReadKind::SETTLING, status);
  ASSERT_GT(plan.max_attempts, EXCHANGE_RETRY_COUNT) << "sanity: the plan is more than today's three";

  const uint32_t blocked_ms = rig.run_silent_poll(read_of(decisions::ReadKind::SETTLING), false);

  EXPECT_EQ(rig.radio.get_send_count(), plan.max_attempts);
  EXPECT_LT(blocked_ms, rig.tuning.exchange_total_budget_ms);
  const auto &t = rig.radio.send_times_ms();
  for (size_t i = 0; i + 1 < t.size(); i++)
    EXPECT_LE(t[i + 1] - t[i], rig.attempt_tx_ms(false) + rig.radio.wait_timeouts()[i] + CHANNEL_CHECK_TIME_MS)
        << "no gap: the next attempt follows the window and its channel check (the estimate rounds the air time up)";
}

TEST(Exchange, SettlingReadIsCappedAtTheMostAttemptsWhateverTheBudget) {
  PlanRig rig;
  rig.tuning.exchange_total_budget_ms = 12000;

  rig.run_silent_poll(read_of(decisions::ReadKind::SETTLING), false);

  EXPECT_EQ(rig.radio.get_send_count(), EXCHANGE_MAX_ATTEMPTS);
}

TEST(Exchange, SettlingReadToASilentLowPowerTargetListensTheLowPowerWindowOnEveryAttempt) {
  PlanRig rig;
  IoFrame status{};
  create_get_status(status, test::OWN_ID, test::DST_ID, true);
  const auto plan = expected_plan(rig.engine, rig.tuning, decisions::ReadKind::SETTLING, status);
  const uint32_t blocked_ms = rig.run_silent_poll(read_of(decisions::ReadKind::SETTLING), true);

  EXPECT_EQ(rig.radio.get_send_count(), LOW_POWER_SETTLING_ATTEMPTS);
  ASSERT_EQ(rig.radio.wait_timeouts().size(), static_cast<size_t>(LOW_POWER_SETTLING_ATTEMPTS));
  for (const auto wait : rig.radio.wait_timeouts())
    EXPECT_EQ(wait, LOW_POWER_RESPONSE_WAIT_MS);
  EXPECT_LT(blocked_ms, silent_attempts_bound_ms(rig, plan.max_attempts, plan.gap_ms, plan.window_ms, true));
}

TEST(Exchange, ASettlingReadToASilentLowPowerTargetRunsThreeWakeUpAttemptsPastTheBudgetAtTheDefaultWakeUp) {
  // ADR 0095, ADR 0097: at the 515 ms default one wake-up attempt and its window take a little over
  // a second, so three of them run past the 2500 ms budget, and the exchange ends after the third.
  PlanRig rig;
  ASSERT_EQ(mock_preamble_bytes(rig.tuning.low_power_wake_ms), MOCK_WAKE_PREAMBLE);
  const uint32_t blocked_ms = rig.run_silent_poll(read_of(decisions::ReadKind::SETTLING), true);

  EXPECT_EQ(rig.radio.get_send_count(), LOW_POWER_SETTLING_ATTEMPTS);
  EXPECT_GT(blocked_ms, rig.tuning.exchange_total_budget_ms) << "the exchange does run past the budget";
  EXPECT_LT(blocked_ms, 3400u) << "about 3.2 s, the three attempts and nothing more";
}

TEST(Exchange, ACommandToASilentLowPowerTargetKeepsThreeAttemptsAtEveryWakeUpLength) {
  for (const uint16_t wake_preamble : {1024, 2048, 4096}) {
    PlanRig rig;
    rig.tuning.low_power_wake_ms = mock_preamble_ms(wake_preamble);
    rig.run_silent_poll(read_of(decisions::ReadKind::COMMAND), true);
    EXPECT_EQ(rig.radio.get_send_count(), EXCHANGE_RETRY_COUNT) << "wake-up " << wake_preamble << " B";
  }
}

TEST(Exchange, AMainsTargetOrAMidTravelLookKeepsTheBudgetAtTheDefaultWakeUp) {
  PlanRig rig;
  const uint32_t blocked_ms = rig.run_silent_poll(read_of(decisions::ReadKind::SETTLING), false);
  EXPECT_LT(blocked_ms, rig.tuning.exchange_total_budget_ms);
}

TEST(Exchange, NoRequestLetsASilentTargetBlockPastTheBudget) {
  // Windows and budgets from short to long, for a settling read (gapless) and for the count form
  // (the default retry gap): the guard keeps the blocked time under the budget when the target
  // never answers. A low-power target's wake-up exchange is bounded by its attempt count instead
  // (ADR 0095): it makes all its attempts and takes no longer than they do.
  for (const bool counted : {false, true}) {
    for (const bool low_power : {false, true}) {
      for (const uint16_t window_ms : {200, 400, 550, 1000}) {
        for (const uint16_t budget_ms : {1000, 2500, 5000, 12000}) {
          PlanRig rig;
          rig.tuning.exchange_start_response_wait_ms = window_ms;
          rig.tuning.exchange_total_budget_ms = budget_ms;
          const ExchangeOptions options =
              counted ? ExchangeOptions{EXCHANGE_RETRY_COUNT} : read_of(decisions::ReadKind::SETTLING);
          IoFrame probe{};
          create_get_status(probe, test::OWN_ID, test::DST_ID, low_power);
          const uint32_t blocked_ms = rig.run_silent_poll(options, low_power);
          if (low_power) {
            const uint32_t attempts = counted ? EXCHANGE_RETRY_COUNT : LOW_POWER_SETTLING_ATTEMPTS;
            const uint32_t gap_ms = counted ? EXCHANGE_RETRY_DELAY_MS : 0;
            const uint32_t listen_ms = counted ? window_ms : LOW_POWER_RESPONSE_WAIT_MS;
            EXPECT_EQ(rig.radio.get_send_count(), static_cast<int>(attempts));
            EXPECT_LT(blocked_ms, silent_attempts_bound_ms(rig, attempts, gap_ms, listen_ms, true))
                << "counted=" << counted << " window=" << window_ms << " budget=" << budget_ms;
          } else if (window_ms < budget_ms) {
            // The first attempt always runs, so a window longer than the budget is the one exception.
            EXPECT_LT(blocked_ms, budget_ms)
                << "counted=" << counted << " window=" << window_ms << " budget=" << budget_ms;
          }
        }
      }
    }
  }
}

TEST(Exchange, ChallengedLowPowerPollKeepsItsThirdTryBecauseItsAttemptCountBoundsIt) {
  // Each try is challenged and its final reply never comes: a try costs its wake-up transmission
  // plus the 500 ms final window, so after two the budget rule would refuse a third. A low-power
  // target's wake-up exchange is bounded by its attempt count instead (ADR 0095): all three run.
  PlanRig rig;
  queue_challenges(rig.radio, EXCHANGE_RETRY_COUNT);
  IoFrame request{};
  create_get_status(request, test::OWN_ID, test::DST_ID, /*low_power=*/true);
  IoFrame response{};

  EXPECT_EQ(rig.engine.send_and_receive(request, response, FREQ_CH2), ExchangeOutcome::SUCCESS_UNCONFIRMED);

  EXPECT_EQ(rig.radio.get_send_count(), 2 * EXCHANGE_RETRY_COUNT) << "three tries, each a request and a 0x3D";
  EXPECT_STRNE(rig.engine.get_debug().stage, "retry_budget_exhausted");
}

TEST(Exchange, ChallengedMainsPollStillMakesItsThirdTry) {
  PlanRig rig;
  queue_challenges(rig.radio, EXCHANGE_RETRY_COUNT);
  IoFrame request{};
  create_get_status(request, test::OWN_ID, test::DST_ID, /*low_power=*/false);
  IoFrame response{};

  EXPECT_EQ(rig.engine.send_and_receive(request, response, FREQ_CH2), ExchangeOutcome::SUCCESS_UNCONFIRMED);

  EXPECT_EQ(rig.radio.get_send_count(), 2 * EXCHANGE_RETRY_COUNT);
}

TEST(Exchange, CommandToASilentMainsTargetMakesNineAttemptsListeningShortOnAllButTheLast) {
  PlanRig rig;
  IoFrame request{};
  create_execute_position(request, test::OWN_ID, test::DST_ID, /*low_power=*/false, 40);
  IoFrame response{};
  const uint32_t start = esphome::test_clock::peek_ms();

  EXPECT_EQ(rig.engine.send_and_receive(request, response, FREQ_CH2, read_of(decisions::ReadKind::COMMAND)),
            ExchangeOutcome::FAILED);

  EXPECT_EQ(rig.radio.get_send_count(), EXCHANGE_MAX_ATTEMPTS) << "more than today's three for a silent mains target";
  const auto &waits = rig.radio.wait_timeouts();
  ASSERT_EQ(waits.size(), static_cast<size_t>(EXCHANGE_MAX_ATTEMPTS));
  for (size_t i = 0; i + 1 < waits.size(); i++)
    EXPECT_EQ(waits[i], rig.tuning.exchange_early_response_wait_ms) << "attempt " << i + 1;
  EXPECT_EQ(waits.back(), rig.tuning.exchange_start_response_wait_ms) << "the last attempt listens the whole window";
  EXPECT_LT(esphome::test_clock::peek_ms() - start, rig.tuning.exchange_total_budget_ms);
}

TEST(Exchange, CommandPlanNeverBlocksPastTheBudgetForAnyEarlyAndStartWindow) {
  for (const uint16_t early_ms : {150, 200, 400, 1000}) {
    for (const uint16_t start_ms : {200, 400, 1000}) {
      for (const uint16_t budget_ms : {500, 1000, 2500, 12000}) {
        PlanRig rig;
        rig.tuning.exchange_early_response_wait_ms = early_ms;
        rig.tuning.exchange_start_response_wait_ms = start_ms;
        rig.tuning.exchange_total_budget_ms = budget_ms;
        IoFrame status{};
        create_get_status(status, test::OWN_ID, test::DST_ID, false);
        const auto plan = expected_plan(rig.engine, rig.tuning, decisions::ReadKind::COMMAND, status);
        const uint32_t blocked_ms = rig.run_silent_poll(read_of(decisions::ReadKind::COMMAND), false);
        const std::string where = "early=" + std::to_string(early_ms) + " start=" + std::to_string(start_ms) +
                                  " budget=" + std::to_string(budget_ms);
        EXPECT_EQ(rig.radio.get_send_count(), plan.max_attempts) << where << ": the plan is what the guard admits";
        // The first attempt always runs, so a start window longer than the budget is the one exception.
        if (start_ms < budget_ms) {
          EXPECT_LT(blocked_ms, budget_ms) << where;
        }
      }
    }
  }
}

TEST(Exchange, ChallengedWriteIsNeverCopiedMoreThanTodaysThreeTimesWhateverThePlan) {
  // The device challenged every copy, so it heard the write; a larger plan must not become up to
  // nine copies of a register write it already holds.
  PlanRig rig;
  queue_challenges(rig.radio, EXCHANGE_MAX_ATTEMPTS);
  const uint8_t payload[] = {0x01, 0x02, 0x03};
  IoFrame request{};
  ASSERT_TRUE(create_write_private(request, test::OWN_ID, test::DST_ID, /*low_power=*/false, payload,
                                   static_cast<uint8_t>(sizeof(payload))));
  ASSERT_EQ(expected_plan(rig.engine, rig.tuning, decisions::ReadKind::COMMAND, request).max_attempts,
            EXCHANGE_MAX_ATTEMPTS)
      << "sanity: the command's plan is the largest, gapless";
  IoFrame response{};

  EXPECT_EQ(rig.engine.send_and_receive(request, response, FREQ_CH2, read_of(decisions::ReadKind::COMMAND)),
            ExchangeOutcome::SUCCESS_UNCONFIRMED);

  EXPECT_EQ(rig.radio.get_send_count(), 2 * EXCHANGE_RETRY_COUNT) << "three requests, each with its 0x3D";
}

TEST(Exchange, ChallengedWriteAfterSilentAttemptsIsNotCopiedAgain) {
  // Three silent attempts spend today's three copies; the fourth is the first the device challenges,
  // and the exchange ends there instead of sending up to five more.
  ResendRig rig;
  rig.radio.queue_rx_hold_until_sent(4);  // nothing is heard until the fourth request is out
  rig.queue_challenge();
  const uint8_t payload[] = {0x01, 0x02, 0x03};
  IoFrame request{};
  ASSERT_TRUE(create_write_private(request, test::OWN_ID, test::DST_ID, /*low_power=*/false, payload,
                                   static_cast<uint8_t>(sizeof(payload))));
  // The command's plan has short early listens, which leave the budget room for further copies.
  ASSERT_EQ(expected_plan(rig.engine, rig.tuning, decisions::ReadKind::COMMAND, request).max_attempts,
            EXCHANGE_MAX_ATTEMPTS);
  IoFrame response{};

  EXPECT_EQ(rig.engine.send_and_receive(request, response, FREQ_CH2, read_of(decisions::ReadKind::COMMAND)),
            ExchangeOutcome::SUCCESS_UNCONFIRMED);

  EXPECT_EQ(rig.radio.get_send_count(), 5) << "four requests and the 0x3D answering the fourth's challenge";
}

TEST(Exchange, ExecuteChallengedWithoutAClosingReplyIsResentOnceWhateverThePlan) {
  ResendRig rig;
  rig.queue_challenge();
  rig.radio.queue_rx_hold_until_sent(3);
  rig.queue_challenge();
  ASSERT_EQ(expected_plan(rig.engine, rig.tuning, decisions::ReadKind::COMMAND, stop_request()).max_attempts,
            EXCHANGE_MAX_ATTEMPTS);
  IoFrame response{};

  EXPECT_EQ(rig.engine.send_and_receive(stop_request(), response, FREQ_CH2, read_of(decisions::ReadKind::COMMAND)),
            ExchangeOutcome::SUCCESS_UNCONFIRMED);

  EXPECT_EQ(rig.radio.get_send_count(), 4) << "request, 0x3D, one re-send, 0x3D; no third copy";
  const auto &waits = rig.radio.wait_timeouts();
  ASSERT_GE(waits.size(), 3u);
  EXPECT_EQ(waits[0], rig.tuning.exchange_early_response_wait_ms) << "the first attempt is an early one";
}

TEST(Exchange, FavouriteExecuteWhoseReplyIsLostAfterItWasChallengedIsNeverSentAgain) {
  // 'My' stops a moving cover: a copy after a challenged one could undo what the first started.
  ResendRig rig;
  rig.queue_challenge();  // the device challenges, we answer, its closing reply never comes
  IoFrame request{};
  create_execute_command(request, test::OWN_ID, test::DST_ID, false, CoverCommand::FAVORITE);

  EXPECT_EQ(rig.send(request), ExchangeOutcome::SUCCESS_UNCONFIRMED);

  EXPECT_EQ(rig.radio.get_send_count(), 2) << "one request and its 0x3D";
}

TEST(Exchange, FavouriteExecuteToASilentTargetKeepsTodaysThreeAttempts) {
  // Not repeat-safe, so the hub gives it no plan: the count-based exchange stays at three.
  PlanRig rig;
  IoFrame request{};
  create_execute_command(request, test::OWN_ID, test::DST_ID, false, CoverCommand::FAVORITE);
  IoFrame response{};

  EXPECT_EQ(rig.engine.send_and_receive(request, response, FREQ_CH2), ExchangeOutcome::FAILED);

  EXPECT_EQ(rig.radio.get_send_count(), EXCHANGE_RETRY_COUNT);
}

TEST(Exchange, ResendKeepsItsLongGapWhenThePlanHasNoGap) {
  ResendRig rig;
  rig.queue_challenge();
  rig.radio.queue_rx_hold_until_sent(3);
  rig.queue_challenge();
  rig.radio.queue_rx_hold_until_sent(4);
  rig.queue_final_reply();
  IoFrame response{};

  ASSERT_EQ(rig.engine.send_and_receive(stop_request(), response, FREQ_CH2, read_of(decisions::ReadKind::COMMAND)),
            ExchangeOutcome::SUCCESS_WITH_RESPONSE);

  const auto &t = rig.radio.send_times_ms();
  ASSERT_EQ(t.size(), 4u);
  EXPECT_EQ(t[2] - t[1],
            rig.tuning.exchange_response_wait_ms + UNCONFIRMED_EXECUTE_RESEND_DELAY_MS + CHANNEL_CHECK_TIME_MS)
      << "the re-send waits 750 ms in total whether or not the plan has a gap of its own, then its channel check";
}

TEST(Exchange, ResendToALowPowerTargetIsSentBehindTheWakeUpPreamble) {
  // A low-power EXECUTE accepted without a closing reply is still re-sent after the 750 ms gap; its
  // exchange is bounded by its attempt count, not the budget (ADR 0095).
  ResendRig rig;
  rig.radio.set_model_tx_airtime(true);
  rig.queue_challenge();
  rig.radio.queue_rx_hold_until_sent(3);
  rig.queue_challenge();
  IoFrame request{};
  create_execute_command(request, test::OWN_ID, test::DST_ID, /*low_power=*/true, CoverCommand::STOP);

  EXPECT_EQ(rig.send(request), ExchangeOutcome::SUCCESS_UNCONFIRMED);
  EXPECT_EQ(rig.radio.get_send_count(), 4) << "request, 0x3D, the re-send, 0x3D";
}

TEST(Exchange, ResendThatFitsTheBudgetIsSentOnceItsGapIsWaited) {
  // The re-send's own check (gap, transmission, window) admits it; the guard ahead of the re-sent try
  // must not then refuse it for a final-reply window the re-send's check leaves out, after the long
  // gap has already been waited. A mains target, since a low-power one is bounded by its attempt count
  // and skips the guard (ADR 0095). The budget sits between what the re-send needs and what it would
  // need with the final-reply window counted ahead of it.
  ResendRig rig;
  rig.radio.set_model_tx_airtime(true);
  rig.queue_challenge();
  rig.radio.queue_rx_hold_until_sent(3);
  rig.queue_challenge();
  IoFrame request = stop_request();
  // Try 1 ends with its 500 ms final wait at about 500 ms; the re-send then needs the 750 ms gap, its
  // transmission, the overhead and the 400 ms start window (about 1680 ms in all), and the final-reply
  // window would add about 520 ms more.
  rig.tuning.exchange_total_budget_ms = 2000;
  const uint32_t start = esphome::test_clock::peek_ms();

  EXPECT_EQ(rig.send(request), ExchangeOutcome::SUCCESS_UNCONFIRMED);

  EXPECT_EQ(rig.radio.get_send_count(), 4) << "request, 0x3D, the re-send, 0x3D";
  EXPECT_STRNE(rig.engine.get_debug().stage, "retry_budget_exhausted");
  EXPECT_LT(esphome::test_clock::peek_ms() - start, rig.tuning.exchange_total_budget_ms);
}

TEST(Exchange, UnconfirmedExecuteOnItsLastAllowedTryIsNotWaitedForAResend) {
  // The challenged try is the last one the exchange allows, so no re-send can follow: the long
  // re-send gap must not be waited and no re-send announced. The budget has room for a re-send,
  // so only the attempt count stops it.
  ResendRig rig;
  rig.queue_challenge();
  IoFrame response{};
  const uint32_t start = esphome::test_clock::peek_ms();

  EXPECT_EQ(rig.engine.send_and_receive(stop_request(), response, FREQ_CH2, /*max_tries=*/1),
            ExchangeOutcome::SUCCESS_UNCONFIRMED);

  EXPECT_EQ(rig.radio.get_send_count(), 2) << "request, 0x3D; no re-send on a one-try exchange";
  EXPECT_LT(esphome::test_clock::peek_ms() - start,
            rig.tuning.exchange_start_response_wait_ms + rig.tuning.exchange_response_wait_ms)
      << "the exchange ends with the final wait, not after the re-send gap";
}

namespace {

/// Make every channel read busy for every clear-channel check, and let a request go out anyway once
/// its access time is spent (lbt_force_when_busy), so each start frame waits out its whole access
/// time before it is sent.
void make_channels_always_busy_but_forced(PlanRig &rig) {
  rig.radio.set_rssi_default(0);
  rig.tuning.lbt_force_when_busy = true;
}

}  // namespace

TEST(Exchange, ALowPowerWakeUpExchangeKeepsAllItsAttemptsWhenABusyChannelDelaysEachOne) {
  // Size the budget so the three attempts fit it with 1 ms to spare when nothing delays them. Each
  // attempt then waits the low-power access time (600 ms) for a free channel, so the budget alone
  // would drop the second and third attempts. The attempt count bounds the exchange instead (ADR 0095).
  for (const auto kind : {decisions::ReadKind::SETTLING, decisions::ReadKind::COMMAND}) {
    PlanRig rig;
    make_channels_always_busy_but_forced(rig);
    const uint32_t gap_ms = kind == decisions::ReadKind::COMMAND ? EXCHANGE_RETRY_DELAY_MS : 0;
    const uint32_t window_ms =
        kind == decisions::ReadKind::COMMAND ? rig.tuning.exchange_start_response_wait_ms : LOW_POWER_RESPONSE_WAIT_MS;
    rig.tuning.exchange_total_budget_ms =
        silent_attempts_bound_ms(rig, LOW_POWER_SETTLING_ATTEMPTS, gap_ms, window_ms, true) + 1;

    const uint32_t blocked_ms = rig.run_silent_poll(read_of(kind), true);

    EXPECT_EQ(rig.radio.get_send_count(), static_cast<int>(LOW_POWER_SETTLING_ATTEMPTS))
        << "kind=" << static_cast<int>(kind);
    EXPECT_GT(blocked_ms, rig.tuning.exchange_total_budget_ms) << "the delays pushed the exchange past the budget";
    EXPECT_LT(blocked_ms,
              silent_attempts_bound_ms(rig, LOW_POWER_SETTLING_ATTEMPTS, gap_ms, window_ms, true) +
                  LOW_POWER_SETTLING_ATTEMPTS * (LOW_POWER_CHANNEL_ACCESS_BUDGET_MS + 3 * CHANNEL_CHECK_READINGS))
        << "no longer than the attempts and their channel access time";
  }
}

TEST(Exchange, ALowPowerExchangeRunsNoMoreThanItsAttemptsWhateverTheBudget) {
  // The count bound is also a ceiling: a huge budget adds no attempt to the planned ones (the count
  // form is capped at EXCHANGE_RETRY_COUNT, a settling read at the three wake-ups).
  PlanRig rig;
  rig.tuning.exchange_total_budget_ms = 12000;

  rig.run_silent_poll(ExchangeOptions{EXCHANGE_RETRY_COUNT}, true);
  EXPECT_EQ(rig.radio.get_send_count(), static_cast<int>(EXCHANGE_RETRY_COUNT));

  rig.radio.clear();
  rig.run_silent_poll(read_of(decisions::ReadKind::SETTLING), true);
  EXPECT_EQ(rig.radio.get_send_count(), static_cast<int>(LOW_POWER_SETTLING_ATTEMPTS));
}

TEST(Exchange, AUserBudgetOfOneSecondStillGivesALowPowerTargetThreeAttempts) {
  for (const bool counted : {false, true}) {
    PlanRig rig;
    rig.tuning.exchange_total_budget_ms = 1000;
    rig.run_silent_poll(counted ? ExchangeOptions{EXCHANGE_RETRY_COUNT} : read_of(decisions::ReadKind::SETTLING), true);
    EXPECT_EQ(rig.radio.get_send_count(), static_cast<int>(LOW_POWER_SETTLING_ATTEMPTS)) << "counted=" << counted;
  }
}

TEST(Exchange, AMainsTargetStillStopsAtTheBudgetOnABusyChannel) {
  PlanRig rig;
  make_channels_always_busy_but_forced(rig);
  rig.tuning.exchange_total_budget_ms = 1000;

  const uint32_t blocked_ms = rig.run_silent_poll(ExchangeOptions{EXCHANGE_RETRY_COUNT}, false);

  EXPECT_LT(rig.radio.get_send_count(), static_cast<int>(EXCHANGE_RETRY_COUNT)) << "the budget drops a later attempt";
  EXPECT_LT(blocked_ms, rig.tuning.exchange_total_budget_ms);
}

// ============================================================================
// A settling read of a low-power receiver in an unknown state leads with one short attempt
// (decisions::WakeBelief::UNKNOWN, ADR 0079). The exchange runs on a ManualClock with modelled air
// time, to a MockRadio that is silent unless a test queues a reply for a given send.
// ============================================================================

namespace {

/// A PlanRig whose target-evidence provider is scripted: all-zero stamps are a receiver the hub has
/// no news of.
struct ShortLeadRig : PlanRig {
  decisions::TargetEvidence evidence{};
  int evidence_spent{0};

  ShortLeadRig() {
    engine.set_target_evidence_provider([this](const uint8_t *, decisions::TargetEvidence &out) {
      out = evidence;
      return true;
    });
    engine.set_wake_evidence_spent_handler([this](const uint8_t *) { evidence_spent++; });
  }

  /// A status read of the low-power target.
  static IoFrame status_read() {
    IoFrame request{};
    create_get_status(request, test::OWN_ID, test::DST_ID, /*low_power=*/true);
    return request;
  }

  /// The target's answer to a status read.
  static RadioRxPacket status_reply() {
    const IoFrame reply = build_status_response(test::DST_ID, test::OWN_ID);
    uint8_t raw[64];
    RadioRxPacket pkt{};
    pkt.len = serialize(reply, raw, sizeof(raw));
    memcpy(pkt.data, raw, pkt.len);
    return pkt;
  }

  /// The transmit time the engine plans for the short lead of status_read().
  uint32_t short_tx_ms() const {
    IoFrame request = status_read();
    uint8_t raw[64];
    const uint8_t len = serialize(request, raw, sizeof(raw));
    return (io868_tx_air_time_us(mock_preamble_bytes(tuning.start_preamble_ms), len, SOFT_PHY_PREAMBLE_BITS_PER_BYTE) +
            999) /
           1000;
  }

  std::vector<uint16_t> preambles() const {
    std::vector<uint16_t> result;
    for (const auto &config : radio.get_tx_configs())
      result.push_back(config.preamble_len);
    return result;
  }

  /// The plan the request path builds for a settling read of status_read() that leads short.
  decisions::AttemptPlan leading_plan() const {
    return expected_plan(engine, tuning, decisions::ReadKind::SETTLING, status_read(), /*with_short_lead=*/true);
  }

  /// Run a settling read of status_read() against the rig's scripted radio.
  ExchangeOutcome settling_read(bool state_unknown = false, uint16_t preamble_override = 0) {
    IoFrame response{};
    decisions::AttemptRequest attempts = decisions::AttemptRequest::of_kind(decisions::ReadKind::SETTLING);
    attempts.state_unknown = state_unknown;
    return engine.send_and_receive(status_read(), response, FREQ_CH2,
                                   ExchangeOptions{attempts}.with_preamble(preamble_override));
  }
};

}  // namespace

TEST(ShortLead, AnUnknownLowPowerSettlingReadLeadsWithOneShortAttemptThenTheWakeUpAttempts) {
  ShortLeadRig rig;
  const auto plan = rig.leading_plan();
  ASSERT_TRUE(plan.short_lead);
  EXPECT_EQ(plan.max_attempts, LOW_POWER_SETTLING_ATTEMPTS + 1u) << "the three wake-up attempts are kept";
  EXPECT_TRUE(plan.bounded_by_attempts) << "three wake-ups behind the 2048-byte default outlast the budget";

  EXPECT_EQ(rig.settling_read(), ExchangeOutcome::FAILED);

  EXPECT_EQ(rig.preambles(), (std::vector<uint16_t>{mock_preamble_bytes(rig.tuning.start_preamble_ms),
                                                    MOCK_WAKE_PREAMBLE, MOCK_WAKE_PREAMBLE, MOCK_WAKE_PREAMBLE}));
  const auto &waits = rig.radio.wait_timeouts();
  ASSERT_EQ(waits.size(), plan.max_attempts);
  EXPECT_EQ(waits[0], plan.window_for_attempt(1, plan.max_attempts));
  EXPECT_EQ(waits[0], LOW_POWER_SHORT_LEAD_WINDOW_MS);
  EXPECT_EQ(waits[1], LOW_POWER_RESPONSE_WAIT_MS);
}

TEST(ShortLead, AReadMarkedStateUnknownLeadsShortDespiteARecentStoppedReport) {
  ShortLeadRig rig;
  // The target reported a second ago (a "stopped" report clears the moving evidence): unmarked, the
  // read believes it asleep and sends only wake-up attempts.
  rig.evidence.last_status_ms = esphome::test_clock::peek_ms() - 1000;

  EXPECT_EQ(rig.settling_read(), ExchangeOutcome::FAILED);
  EXPECT_EQ(rig.preambles(), (std::vector<uint16_t>{MOCK_WAKE_PREAMBLE, MOCK_WAKE_PREAMBLE, MOCK_WAKE_PREAMBLE}))
      << "unmarked, the read believes the target asleep and leads with no short attempt";

  rig.radio.clear();
  EXPECT_EQ(rig.settling_read(/*state_unknown=*/true), ExchangeOutcome::FAILED);
  EXPECT_EQ(rig.preambles(), (std::vector<uint16_t>{mock_preamble_bytes(rig.tuning.start_preamble_ms),
                                                    MOCK_WAKE_PREAMBLE, MOCK_WAKE_PREAMBLE, MOCK_WAKE_PREAMBLE}));
  EXPECT_EQ(rig.engine.get_debug().wake_belief, decisions::WakeBelief::UNKNOWN)
      << "the exchange resolves the belief the mark asks for";
}

TEST(ShortLead, FourAttemptsToASilentTargetRunPastTheDefaultBudgetAndNoFurther) {
  ShortLeadRig rig;
  const auto plan = rig.leading_plan();

  const uint32_t start = esphome::test_clock::peek_ms();
  EXPECT_EQ(rig.settling_read(), ExchangeOutcome::FAILED);
  const uint32_t blocked_ms = esphome::test_clock::peek_ms() - start;

  EXPECT_EQ(rig.radio.get_send_count(), static_cast<int>(plan.max_attempts))
      << "the guard admits every attempt the plan promised, the wake-up attempts included";
  EXPECT_EQ(rig.preambles(), (std::vector<uint16_t>{mock_preamble_bytes(rig.tuning.start_preamble_ms),
                                                    MOCK_WAKE_PREAMBLE, MOCK_WAKE_PREAMBLE, MOCK_WAKE_PREAMBLE}));
  const auto &waits = rig.radio.wait_timeouts();
  ASSERT_EQ(waits.size(), 4u);
  EXPECT_EQ(waits[0], LOW_POWER_SHORT_LEAD_WINDOW_MS) << "the lead listens briefly";
  EXPECT_EQ(waits[1], LOW_POWER_RESPONSE_WAIT_MS);
  EXPECT_GT(blocked_ms, rig.tuning.exchange_total_budget_ms) << "the wake-up attempts run past the budget";
  EXPECT_LT(blocked_ms,
            rig.short_tx_ms() + EXCHANGE_ATTEMPT_OVERHEAD_MS + LOW_POWER_SHORT_LEAD_WINDOW_MS +
                silent_attempts_bound_ms(rig, LOW_POWER_SETTLING_ATTEMPTS, 0, LOW_POWER_RESPONSE_WAIT_MS, true))
      << "the lead and the three wake-up attempts take no longer than they need";
}

TEST(ShortLead, AUserBudgetOfOneSecondStillRunsTheLeadAndEveryWakeUpAttempt) {
  // The budget admits none of the attempts after the first; the attempt count alone bounds the exchange
  // (ADR 0095).
  ShortLeadRig rig;
  rig.tuning.exchange_total_budget_ms = 1000;
  const auto plan = rig.leading_plan();
  ASSERT_EQ(plan.max_attempts, LOW_POWER_SETTLING_ATTEMPTS + 1u);

  const uint32_t start = esphome::test_clock::peek_ms();
  EXPECT_EQ(rig.settling_read(), ExchangeOutcome::FAILED);
  const uint32_t blocked_ms = esphome::test_clock::peek_ms() - start;

  EXPECT_EQ(rig.radio.get_send_count(), static_cast<int>(plan.max_attempts))
      << "no wake-up attempt is dropped for the lead";
  EXPECT_GT(blocked_ms, rig.tuning.exchange_total_budget_ms) << "the attempts run past the budget";
}

TEST(ShortLead, ACoverAnsweringOnlyTheShortPreambleIsReachedOnTheLead) {
  // A moving VELUX screen answers the short start frame and ignores the wake-up one.
  ShortLeadRig rig;
  rig.radio.queue_rx(ShortLeadRig::status_reply());

  EXPECT_EQ(rig.settling_read(), ExchangeOutcome::SUCCESS_WITH_RESPONSE);

  EXPECT_EQ(rig.radio.get_send_count(), 1);
  EXPECT_EQ(rig.preambles(), (std::vector<uint16_t>{mock_preamble_bytes(rig.tuning.start_preamble_ms)}));
}

TEST(ShortLead, ACoverAnsweringOnlyTheWakeUpPreambleIsReachedOnTheSecondAttempt) {
  // A resting receiver ignores the short start frame and hears the wake-up one.
  ShortLeadRig rig;
  rig.radio.queue_rx_hold_until_sent(2);
  rig.radio.queue_rx(ShortLeadRig::status_reply());

  EXPECT_EQ(rig.settling_read(), ExchangeOutcome::SUCCESS_WITH_RESPONSE);

  EXPECT_EQ(rig.radio.get_send_count(), 2);
  EXPECT_EQ(rig.preambles(),
            (std::vector<uint16_t>{mock_preamble_bytes(rig.tuning.start_preamble_ms), MOCK_WAKE_PREAMBLE}));
}

TEST(ShortLead, ASilentLeadSpendsNoEvidence) {
  // Only an exchange planned on AWAKE spends the moving evidence when it goes unanswered; the lead of an
  // unknown state has none to spend.
  ShortLeadRig rig;

  EXPECT_EQ(rig.settling_read(), ExchangeOutcome::FAILED);

  EXPECT_EQ(rig.evidence_spent, 0);
}

TEST(ShortLead, ASingleTryReadKeepsTheWakeUpPreamble) {
  // The lead never replaces the only try: a progress read goes to a receiver whose state is unknown
  // with the preamble that reaches a resting one.
  ShortLeadRig rig;
  IoFrame response{};

  EXPECT_EQ(rig.engine.send_and_receive(ShortLeadRig::status_read(), response, FREQ_CH2,
                                        read_of(decisions::ReadKind::PROGRESS)),
            ExchangeOutcome::FAILED);

  EXPECT_EQ(rig.preambles(), (std::vector<uint16_t>{MOCK_WAKE_PREAMBLE}));
  EXPECT_EQ(rig.engine.get_debug().wake_belief, decisions::WakeBelief::UNKNOWN);
}

TEST(ShortLead, ACountedStatusReadKeepsTheWakeUpPreambleOnEveryTry) {
  ShortLeadRig rig;
  IoFrame response{};

  EXPECT_EQ(rig.engine.send_and_receive(ShortLeadRig::status_read(), response, FREQ_CH2, EXCHANGE_RETRY_COUNT),
            ExchangeOutcome::FAILED);

  EXPECT_EQ(rig.preambles(), (std::vector<uint16_t>{MOCK_WAKE_PREAMBLE, MOCK_WAKE_PREAMBLE, MOCK_WAKE_PREAMBLE}))
      << "a caller that names only a number of tries gets no lead";
}

TEST(ShortLead, NewsOfTheReceiversStateLeavesNoLead) {
  ShortLeadRig rig;

  rig.evidence.last_status_ms = esphome::millis() - 1000;  // a status report a second ago: at rest, known
  EXPECT_EQ(rig.settling_read(), ExchangeOutcome::FAILED);
  EXPECT_EQ(rig.preambles(), (std::vector<uint16_t>{MOCK_WAKE_PREAMBLE, MOCK_WAKE_PREAMBLE, MOCK_WAKE_PREAMBLE}));

  rig.radio.clear();
  rig.evidence.last_status_ms = 0;
  rig.evidence.last_moving_evidence_ms = esphome::millis() - 1000;  // believed moving: the belief itself leads short
  EXPECT_EQ(rig.settling_read(), ExchangeOutcome::FAILED);
  EXPECT_EQ(rig.radio.get_send_count(), LOW_POWER_SETTLING_ATTEMPTS)
      << "the wake-up attempts only, none added as a lead";
  EXPECT_EQ(rig.radio.wait_timeouts().front(), LOW_POWER_RESPONSE_WAIT_MS);
}

TEST(ShortLead, AFixedPreambleOrACommandHasNoLead) {
  ShortLeadRig rig;

  EXPECT_EQ(rig.settling_read(/*state_unknown=*/false, MOCK_WAKE_PREAMBLE), ExchangeOutcome::FAILED);
  EXPECT_EQ(rig.preambles(), (std::vector<uint16_t>{MOCK_WAKE_PREAMBLE, MOCK_WAKE_PREAMBLE, MOCK_WAKE_PREAMBLE}))
      << "the read at a cover's predicted end fixes the wake-up preamble";

  rig.radio.clear();
  IoFrame move{};
  create_execute_position(move, test::OWN_ID, test::DST_ID, /*low_power=*/true, 40);
  IoFrame response{};
  EXPECT_EQ(rig.engine.send_and_receive(move, response, FREQ_CH2, read_of(decisions::ReadKind::COMMAND)),
            ExchangeOutcome::FAILED);
  EXPECT_EQ(rig.radio.get_send_count(), EXCHANGE_RETRY_COUNT);
  for (const auto &config : rig.radio.get_tx_configs())
    EXPECT_EQ(config.preamble_len, MOCK_WAKE_PREAMBLE);
}

TEST(ShortLead, ANonLowPowerTargetHasNoLead) {
  ShortLeadRig rig;
  IoFrame request{};
  create_get_status(request, test::OWN_ID, test::DST_ID, /*low_power=*/false);
  IoFrame response{};

  EXPECT_EQ(rig.engine.send_and_receive(request, response, FREQ_CH2, read_of(decisions::ReadKind::SETTLING)),
            ExchangeOutcome::FAILED);

  for (const auto &config : rig.radio.get_tx_configs())
    EXPECT_EQ(config.preamble_len, mock_preamble_bytes(rig.tuning.start_preamble_ms));
}

TEST(ShortLead, SwitchedOffWithTheWakeBeliefTuning) {
  ShortLeadRig rig;
  rig.tuning.low_power_wake_belief = false;

  EXPECT_EQ(rig.settling_read(), ExchangeOutcome::FAILED);

  EXPECT_EQ(rig.preambles(), (std::vector<uint16_t>{MOCK_WAKE_PREAMBLE, MOCK_WAKE_PREAMBLE, MOCK_WAKE_PREAMBLE}))
      << "with the belief off every try keeps the wake-up preamble";
}

// ============================================================================
// One options value; each exchange is planned once
// ============================================================================

TEST(ExchangeOptions, SettlingReadOfALowPowerTargetLooksTheTargetUpOnce) {
  WakeBeliefRig rig;
  IoFrame request{};
  create_get_status(request, test::OWN_ID, test::DST_ID, /*low_power=*/true);
  IoFrame response{};

  rig.engine.send_and_receive(request, response, FREQ_CH2,
                              decisions::AttemptRequest::of_kind(decisions::ReadKind::SETTLING));

  EXPECT_EQ(rig.provider_calls, 1) << "the preamble plan and the attempt plan share one lookup";
}

TEST(ExchangeOptions, EveryWayOfNamingTheAttemptsLooksTheTargetUpOnce) {
  const IoFrame request = low_power_position_request();
  IoFrame response{};
  {
    WakeBeliefRig rig;
    rig.engine.send_and_receive(request, response, FREQ_CH2, ExchangeOptions{2});
    EXPECT_EQ(rig.provider_calls, 1) << "a count";
  }
  {
    WakeBeliefRig rig;
    rig.engine.send_and_receive(request, response, FREQ_CH2,
                                decisions::AttemptRequest::of_kind(decisions::ReadKind::COMMAND));
    EXPECT_EQ(rig.provider_calls, 1) << "a command read kind";
  }
  {
    WakeBeliefRig rig;
    rig.engine.send_and_receive(request, response, FREQ_CH2);
    EXPECT_EQ(rig.provider_calls, 1) << "the default options";
  }
}

TEST(ExchangeOptions, OptionsKeepEachFieldWhenAnotherIsSet) {
  const uint8_t key[AES_KEY_SIZE] = {};
  const ExchangeOptions options = ExchangeOptions{3}.with_preamble(40).with_key(key);
  EXPECT_EQ(options.attempts.max_tries, 3);
  EXPECT_EQ(options.preamble_override, 40);
  EXPECT_EQ(options.key, key);
}

TEST(ExchangeOptions, OnlyAnExecuteLooksTheTargetUpWhenItsTryIsUnconfirmed) {
  {
    ResendRig rig;
    rig.queue_challenge();  // challenged, never closed
    EXPECT_EQ(rig.send(stop_request()), ExchangeOutcome::SUCCESS_UNCONFIRMED);
    EXPECT_EQ(rig.lookups, 1) << "an EXECUTE asks whether the device confirms";
  }
  {
    ResendRig rig;
    rig.queue_challenge();
    IoFrame request{};
    ASSERT_TRUE(create_subscribe_event(request, test::OWN_ID, test::DST_ID, EVENT_LOCAL_SENSOR, false));
    EXPECT_EQ(rig.send(request), ExchangeOutcome::SUCCESS_UNCONFIRMED);
    EXPECT_EQ(rig.lookups, 0) << "a subscription's answer does not depend on the target";
  }
  {
    ResendRig rig;
    rig.queue_challenge();
    IoFrame request{};
    create_get_status(request, test::OWN_ID, test::DST_ID, /*low_power=*/false);
    EXPECT_EQ(rig.send(request), ExchangeOutcome::SUCCESS_UNCONFIRMED);
    EXPECT_EQ(rig.lookups, 0);
  }
}

TEST(ExchangeOptions, UnconfirmedStatusReadKeepsItsPlanGapAndIsNotGivenTheLongResendGap) {
  ResendRig rig;
  rig.queue_challenge();
  IoFrame request{};
  create_get_status(request, test::OWN_ID, test::DST_ID, /*low_power=*/false);

  ASSERT_EQ(rig.send(request), ExchangeOutcome::SUCCESS_UNCONFIRMED);

  const auto &t = rig.radio.send_times_ms();
  ASSERT_GE(t.size(), 3u);
  EXPECT_EQ(t[2] - t[1], rig.tuning.exchange_response_wait_ms + EXCHANGE_RETRY_DELAY_MS + CHANNEL_CHECK_TIME_MS);
}

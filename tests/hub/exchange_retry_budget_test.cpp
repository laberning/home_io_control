/// @file exchange_retry_budget_test.cpp
/// @brief Tests for the retry budget of the exchange engine: the tries a window leaves, run_attempts(), the budget a
/// transmission uses, the failure kind a report carries, and the max_tries cap of a scheduler-owned poll.

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
// Retry budget: EXCHANGE_RETRY_COUNT is a maximum, not a promise. Three tries at a window sized
// for the slowest device on the network blocks the ESPHome loop past its own warning threshold
// (ADR 0013), so a try only starts if the exchange still has budget. See EXCHANGE_TOTAL_BUDGET_MS.
// ============================================================================

TEST(Exchange, RetriesStopOnceTheTotalBudgetIsSpent) {
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;

  TuningConfig tuning;
  // A try starts only if it ends inside the budget: elapsed + retry delay (250) + the try's air time
  // (a mains START frame's short preamble, ~16 ms) + its overhead and window (250). Try 2's check runs
  // at ~276 ms (try 1's end) and ends at ~802 < 1000; try 3's runs at ~802 and would end at ~1328 --
  // exactly two tries, in real milliseconds rather than relative to the window.
  tuning.exchange_start_response_wait_ms = 250;
  tuning.exchange_total_budget_ms = 1000;
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  IoFrame request{};
  create_execute_position(request, test::OWN_ID, test::DST_ID, /*low_power=*/false, 100);
  IoFrame response{};
  engine.send_and_receive(request, response, FREQ_CH2);  // nothing queued -> every try fails

  EXPECT_EQ(radio.get_send_count(), 2) << "a third try must not start once the budget is spent";
  EXPECT_STREQ(engine.get_debug().stage, "retry_budget_exhausted");
}

TEST(Exchange, ALowPowerTargetKeepsItsThreeTriesWhateverTheBudget) {
  // The same exchange to a low-power target: the budget would stop it after two, the attempt count
  // alone bounds it, so the third runs (ADR 0095).
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;

  TuningConfig tuning;
  tuning.exchange_start_response_wait_ms = 250;
  tuning.exchange_total_budget_ms = 1000;
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  IoFrame request{};
  create_execute_position(request, test::OWN_ID, test::DST_ID, /*low_power=*/true, 100);
  IoFrame response{};
  engine.send_and_receive(request, response, FREQ_CH2);

  EXPECT_EQ(radio.get_send_count(), EXCHANGE_RETRY_COUNT);
}

TEST(Exchange, GenerousBudgetStillAllowsEveryRetry) {
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;

  TuningConfig tuning;
  tuning.exchange_start_response_wait_ms = 200;
  tuning.exchange_total_budget_ms = 60000;  // far more than three tries plus their retry delays can spend
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  IoFrame request{};
  create_execute_position(request, test::OWN_ID, test::DST_ID, true, 100);
  IoFrame response{};
  engine.send_and_receive(request, response, FREQ_CH2);

  EXPECT_EQ(radio.get_send_count(), EXCHANGE_RETRY_COUNT) << "with budget to spare the retry count is unchanged";
}

// --- run_attempts(): the loop pairing's retries run through ------------------------------------

TEST(ExchangeRunAttempts, RunsEveryTryWithTheGapBeforeEachFurtherOne) {
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning;
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  std::vector<uint32_t> starts;
  std::vector<uint8_t> numbers;
  const uint32_t begin_ms = esphome::test_clock::peek_ms();
  const AttemptRunResult result =
      engine.run_attempts(ExchangeEngine::fixed_attempts(3, 250, 500), [&](uint8_t number, uint16_t window_ms) {
        starts.push_back(esphome::test_clock::peek_ms() - begin_ms);
        numbers.push_back(number);
        EXPECT_EQ(window_ms, 500);
        return AttemptVerdict::AGAIN;
      });

  EXPECT_EQ(result, AttemptRunResult::TRIES_USED);
  EXPECT_EQ(numbers, (std::vector<uint8_t>{1, 2, 3}));
  EXPECT_EQ(starts, (std::vector<uint32_t>{0, 250, 500})) << "no gap before the first try, one before each other";
  EXPECT_EQ(engine.counters().retransmits, 2u) << "tries beyond the first count as retransmits";
}

TEST(ExchangeRunAttempts, DoneAndStopEndTheRunAtOnce) {
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning;
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  uint8_t calls = 0;
  EXPECT_EQ(engine.run_attempts(ExchangeEngine::fixed_attempts(3, 250),
                                [&](uint8_t number, uint16_t) {
                                  calls++;
                                  return number == 2 ? AttemptVerdict::DONE : AttemptVerdict::AGAIN;
                                }),
            AttemptRunResult::DONE);
  EXPECT_EQ(calls, 2);

  calls = 0;
  EXPECT_EQ(engine.run_attempts(ExchangeEngine::fixed_attempts(3, 250),
                                [&](uint8_t, uint16_t) {
                                  calls++;
                                  return AttemptVerdict::STOP;
                                }),
            AttemptRunResult::STOPPED);
  EXPECT_EQ(calls, 1);
}

TEST(ExchangeRunAttempts, ACountOfZeroRunsNoTry) {
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning;
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  bool called = false;
  EXPECT_EQ(engine.run_attempts(ExchangeEngine::fixed_attempts(0, 250),
                                [&](uint8_t, uint16_t) {
                                  called = true;
                                  return AttemptVerdict::DONE;
                                }),
            AttemptRunResult::TRIES_USED);
  EXPECT_FALSE(called);
}

TEST(ExchangeRunAttempts, ARunWithoutABudgetIgnoresTheExchangeBudget) {
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning;
  tuning.exchange_total_budget_ms = 100;  // far less than the gaps alone
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  uint8_t calls = 0;
  EXPECT_EQ(engine.run_attempts(ExchangeEngine::fixed_attempts(3, 250),
                                [&](uint8_t, uint16_t) {
                                  calls++;
                                  return AttemptVerdict::AGAIN;
                                }),
            AttemptRunResult::TRIES_USED);
  EXPECT_EQ(calls, 3);
}

TEST(ExchangeRunAttempts, ABudgetedRunStopsStartingTriesThatCannotFinish) {
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning;
  tuning.exchange_total_budget_ms = 1000;
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  // Try 1 spends its 200 ms window. Try 2 starts: 200 + 250 (gap) + 100 (air) + overhead + 200
  // ends before 1000, and the run is then at 650 ms. Try 3 would end at 650 + 250 + 100 + overhead
  // + 200 > 1000, so it is refused.
  ExchangeEngine::AttemptRun run = ExchangeEngine::fixed_attempts(9, 250, 200);
  run.budgeted_tx_ms = 100;
  uint8_t calls = 0;
  const AttemptRunResult result = engine.run_attempts(run, [&](uint8_t, uint16_t window_ms) {
    calls++;
    esphome::delay(window_ms);  // the try listens its whole window
    return AttemptVerdict::AGAIN;
  });

  EXPECT_EQ(result, AttemptRunResult::BUDGET_EXHAUSTED);
  EXPECT_EQ(calls, 2) << "the third try would end past the budget, so it is not started";
}

TEST(ExchangeRunAttempts, ABudgetedRunStillStartsItsFirstTryWhateverItCosts) {
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning;
  tuning.exchange_total_budget_ms = 1;
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  ExchangeEngine::AttemptRun run = ExchangeEngine::fixed_attempts(3, 250, 400);
  run.budgeted_tx_ms = 100;
  uint8_t calls = 0;
  EXPECT_EQ(engine.run_attempts(run,
                                [&](uint8_t, uint16_t) {
                                  calls++;
                                  return AttemptVerdict::AGAIN;
                                }),
            AttemptRunResult::BUDGET_EXHAUSTED);
  EXPECT_EQ(calls, 1);
}

// --- A try's transmission counts against the budget -------------------------------------------
// A try starts only if its transmission ends inside the budget (ExchangeEngine::try_fits_budget_()),
// with the transmit time taken from the driver's tx_air_time_us(). Every case models air time on a
// ManualClock, so the clock the budget is checked against includes it.

namespace {

/// A radio whose wake-up is a 500 ms transmission for a LONG start frame, e.g. one that wakes
/// receivers with a train of frames instead of a long preamble.
class SlowWakeRadio : public MockRadio {
 public:
  static constexpr uint32_t LONG_WAKE_US = 500000;
  uint32_t tx_air_time_us(uint8_t len, const RadioTxConfig &cfg) const override {
    return cfg.wake == TxWake::LONG ? LONG_WAKE_US : MockRadio::tx_air_time_us(len, cfg);
  }
};

/// Run a status poll that is challenged and never closed on every try, with @p budget_ms.
/// @return The radio's send count.
int run_challenged_status_poll(uint16_t budget_ms, std::vector<uint32_t> *send_times = nullptr,
                               const char **stage = nullptr) {
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
  radio.set_model_tx_airtime(true);
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning;
  tuning.exchange_total_budget_ms = budget_ms;
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);
  queue_challenges(radio, EXCHANGE_RETRY_COUNT);

  IoFrame request{};
  create_get_status(request, test::OWN_ID, test::DST_ID, /*low_power=*/false);
  IoFrame response{};
  EXPECT_EQ(engine.send_and_receive(request, response, FREQ_CH2), ExchangeOutcome::SUCCESS_UNCONFIRMED);
  if (send_times != nullptr)
    *send_times = radio.send_times_ms();
  if (stage != nullptr)
    *stage = engine.get_debug().stage;
  return radio.get_send_count();
}

}  // namespace

TEST(Exchange, ThreeUnansweredSlowWakeTriesAllRunPastTheBudget) {
  esphome::test_clock::ManualClock clock;
  SlowWakeRadio radio;
  radio.set_model_tx_airtime(true);
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning;  // defaults: 400 ms start window, 250 ms retry gap, 2500 ms budget
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  IoFrame request{};
  create_execute_position(request, test::OWN_ID, test::DST_ID, /*low_power=*/true, 100);
  IoFrame response{};
  const uint32_t start = esphome::test_clock::peek_ms();
  EXPECT_EQ(engine.send_and_receive(request, response, FREQ_CH2), ExchangeOutcome::FAILED);
  const uint32_t blocked_ms = esphome::test_clock::peek_ms() - start;

  // Three tries of a 500 ms wake-up, a 400 ms window and the 250 ms gap end at 3 x 910 + 2 x 250 =
  // 3230 ms, past the 2500 ms budget: a low-power target keeps its three tries, bounded by their count
  // (ADR 0095). The exchange runs no longer than they need.
  EXPECT_EQ(radio.get_send_count(), 3);
  EXPECT_STREQ(engine.get_debug().stage, "wait_first_timeout");
  EXPECT_GT(blocked_ms, tuning.exchange_total_budget_ms);
  EXPECT_LT(blocked_ms, 3230u);
  for (const auto &cfg : radio.get_tx_configs())
    EXPECT_EQ(cfg.wake, TxWake::LONG);
}

TEST(Exchange, SettlingReadRunsThePlanTheGuardAgreesWith) {
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
  radio.set_model_tx_airtime(true);
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning;
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);
  IoFrame request{};
  create_get_status(request, test::OWN_ID, test::DST_ID, /*low_power=*/false);

  const decisions::AttemptPlan plan = expected_plan(engine, tuning, decisions::ReadKind::SETTLING, request);

  EXPECT_EQ(plan.gap_ms, 0u);
  // At the defaults one attempt is the short start frame on air (~10 ms) plus its overhead and the
  // 200 ms early window, and the last one listens the whole 400 ms: nine fit below the 2500 ms
  // budget (ADR 0075).
  EXPECT_EQ(plan.max_attempts, EXCHANGE_MAX_ATTEMPTS) << "a mains settling read makes nine attempts at the defaults";
  // A silent target gets exactly the planned attempts: the plan equals what the guard admits.
  IoFrame response{};
  const uint32_t start = esphome::test_clock::peek_ms();
  EXPECT_EQ(engine.send_and_receive(request, response, FREQ_CH2, read_of(decisions::ReadKind::SETTLING)),
            ExchangeOutcome::FAILED);
  EXPECT_EQ(radio.get_send_count(), plan.max_attempts);
  EXPECT_LT(esphome::test_clock::peek_ms() - start, tuning.exchange_total_budget_ms);
}

TEST(Exchange, SettlingReadAgreesWithTheGuardWhenTheBudgetLimitsTheAttempts) {
  // At a 1000 ms budget the plan is 3 (1 + (999 - 422) / 222) and a silent target gets exactly those.
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
  radio.set_model_tx_airtime(true);
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning;
  tuning.exchange_total_budget_ms = 1000;
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);
  IoFrame request{};
  create_get_status(request, test::OWN_ID, test::DST_ID, /*low_power=*/false);

  const decisions::AttemptPlan plan = expected_plan(engine, tuning, decisions::ReadKind::SETTLING, request);
  IoFrame response{};
  EXPECT_EQ(engine.send_and_receive(request, response, FREQ_CH2,
                                    decisions::AttemptRequest::of_kind(decisions::ReadKind::SETTLING)),
            ExchangeOutcome::FAILED);

  EXPECT_EQ(plan.max_attempts, 3u);
  EXPECT_EQ(radio.get_send_count(), plan.max_attempts) << "the plan promises what the guard admits, no more, no fewer";
}

TEST(Exchange, SettlingReadOfASilentLowPowerTargetUsesTheLowPowerWindowAndThreeAttempts) {
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
  radio.set_model_tx_airtime(true);
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning;
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);
  IoFrame request{};
  create_get_status(request, test::OWN_ID, test::DST_ID, /*low_power=*/true);

  IoFrame response{};
  EXPECT_EQ(engine.send_and_receive(request, response, FREQ_CH2,
                                    decisions::AttemptRequest::of_kind(decisions::ReadKind::SETTLING)),
            ExchangeOutcome::FAILED);

  EXPECT_EQ(radio.get_send_count(), LOW_POWER_SETTLING_ATTEMPTS)
      << "wake-up preamble plus 550 ms window: three fit the default budget, the cap";
  ASSERT_EQ(radio.wait_timeouts().size(), static_cast<size_t>(LOW_POWER_SETTLING_ATTEMPTS));
  for (const auto wait : radio.wait_timeouts())
    EXPECT_EQ(wait, LOW_POWER_RESPONSE_WAIT_MS);
}

TEST(Exchange, ProgressLookToASilentTargetIsASingleAttempt) {
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning;
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);
  IoFrame request{};
  create_get_status(request, test::OWN_ID, test::DST_ID, /*low_power=*/false);

  IoFrame response{};
  EXPECT_EQ(engine.send_and_receive(request, response, FREQ_CH2,
                                    decisions::AttemptRequest::of_kind(decisions::ReadKind::PROGRESS)),
            ExchangeOutcome::FAILED);

  EXPECT_EQ(radio.get_send_count(), SCHEDULED_POLL_MAX_TRIES);
}

TEST(Exchange, ATryThatWouldEndAtTheBudgetIsNotStarted) {
  // Measure the third try's check time with budget to spare: its request goes out one retry gap
  // after the check.
  std::vector<uint32_t> t;
  ASSERT_EQ(run_challenged_status_poll(60000, &t), 2 * EXCHANGE_RETRY_COUNT);
  ASSERT_EQ(t.size(), 6u);
  const uint32_t third_check_ms = t[4] - EXCHANGE_RETRY_DELAY_MS - t[0];
  IoFrame request{};
  create_get_status(request, test::OWN_ID, test::DST_ID, false);
  uint8_t raw[64];
  const uint8_t len = serialize(request, raw, sizeof(raw));
  const uint32_t tx_ms = (io868_tx_air_time_us(mock_preamble_bytes(TuningConfig{}.start_preamble_ms), len,
                                               SOFT_PHY_PREAMBLE_BITS_PER_BYTE) +
                          999) /
                         1000;
  ASSERT_GT(tx_ms, 0u);

  // The elapsed time alone is well inside the budget, but the third try would end exactly at it:
  // its transmission and overhead, its first-reply window and, the target having challenged, its
  // final-reply window with the 0x3D. That try does not start.
  // The tail is what a challenged try adds: the final-reply window, the 0x3D's air time (the request
  // stands in for its size) and the overhead.
  const TuningConfig defaults;
  MockRadio probe;
  const uint32_t answer_tx_ms =
      (io868_tx_air_time_us(probe.response_preamble(), len, SOFT_PHY_PREAMBLE_BITS_PER_BYTE) + 999) / 1000;
  const uint32_t tail_ms = defaults.exchange_response_wait_ms + answer_tx_ms + EXCHANGE_ATTEMPT_OVERHEAD_MS;
  const auto edge =
      static_cast<uint16_t>(third_check_ms + EXCHANGE_RETRY_DELAY_MS + tx_ms + EXCHANGE_ATTEMPT_OVERHEAD_MS +
                            decisions::start_reply_window_ms(defaults, IO_868_BAND) + tail_ms);
  const char *stage = nullptr;
  EXPECT_EQ(run_challenged_status_poll(edge, nullptr, &stage), 4);
  EXPECT_STREQ(stage, "retry_budget_exhausted");
  // One millisecond more, and it ends inside the budget.
  EXPECT_EQ(run_challenged_status_poll(static_cast<uint16_t>(edge + 1)), 6);
}

TEST(Exchange, RetryCadenceMatchesTheRecordedWaitSliceForEachTryPlusTheFixedRetryDelay) {
  // Computed from the recorded slices rather than a hardcoded expected gap, so a future change to
  // RESPONSE_START_WAIT_MS or EXCHANGE_RETRY_DELAY_MS can't silently break this without the
  // assertion catching it. Note the per-slice half of that gap is partly self-referential --
  // MockRadio advances the manual clock by exactly wait_timeouts[i] on an empty slice, so that
  // arithmetic isn't independently verified here. What this test does add: exactly one wait per
  // try (not several smaller slices), and exactly one EXCHANGE_RETRY_DELAY_MS gap on top, with no
  // extra time unaccounted for.
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning;  // defaults: EXCHANGE_RETRY_COUNT tries, a total budget generous enough for all of them
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  IoFrame request{};
  create_execute_position(request, test::OWN_ID, test::DST_ID, true, 100);
  IoFrame response{};
  engine.send_and_receive(request, response, FREQ_CH2);  // nothing queued -> every try fails

  ASSERT_EQ(radio.get_send_count(), EXCHANGE_RETRY_COUNT)
      << "sanity: every try must have run for this to be meaningful";
  const auto &send_times = radio.send_times_ms();
  const auto &wait_timeouts = radio.wait_timeouts();
  ASSERT_EQ(send_times.size(), static_cast<size_t>(EXCHANGE_RETRY_COUNT));
  ASSERT_EQ(wait_timeouts.size(), static_cast<size_t>(EXCHANGE_RETRY_COUNT))
      << "HOLD_REQUEST_CHANNEL (wait_for_first_response_) issues exactly one wait per try";

  for (size_t i = 0; i + 1 < send_times.size(); i++) {
    EXPECT_EQ(send_times[i + 1] - send_times[i], wait_timeouts[i] + EXCHANGE_RETRY_DELAY_MS + CHANNEL_CHECK_TIME_MS)
        << "try " << i
        << "'s gap to the next send must be its own wait slice plus the fixed retry delay and the "
           "next request's channel check";
  }
}

TEST(Exchange, ExecuteUsesUserDefaultAceiPriority) {
  // A real 2W hub (Velux KIG300, 2026-08-14 capture) sends ACEI 0x63 -- level 3, user_default.
  // Claiming a higher priority than the reference controller is what this hub used to do.
  IoFrame f{};
  ASSERT_TRUE(create_execute_position(f, test::OWN_ID, test::DST_ID, true, 100));
  EXPECT_EQ(f.data[1], 0x63);
  EXPECT_EQ((f.data[1] & ACEI_LEVEL_MASK) >> ACEI_LEVEL_SHIFT, ACEI_LEVEL_USER_DEFAULT);
}

// ============================================================================
// A failure report has to say which kind of failure it was. Every wait_for_packet() clears the
// radio's capture before listening, so recording "the latest" meant the report always described
// the final timed-out wait — making cap_valid=0 tautological and hiding whether the radio heard
// nothing or heard something this layer discarded.
// ============================================================================

TEST(Exchange, FailureReportKeepsTheInformativeCaptureNotTheLastEmptyOne) {
  MockRadio radio;
  radio.set_emulate_capture_lifecycle(true);
  RadioDriver *radio_ptr = &radio;

  TuningConfig tuning;
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  // One frame arrives and is correctly ignored (wrong endpoints), then nothing for the rest of the
  // exchange. The radio demonstrably heard something.
  const uint8_t other_device[3] = {0x55, 0x66, 0x77};
  IoFrame unrelated = build_status_response(other_device, test::OWN_ID);
  uint8_t raw[64];
  uint8_t raw_len = serialize(unrelated, raw, sizeof(raw));
  RadioRxPacket pkt{};
  pkt.len = raw_len;
  memcpy(pkt.data, raw, raw_len);
  pkt.freq_hz = FREQ_CH2;
  radio.queue_rx(pkt);

  IoFrame request{};
  create_execute_position(request, test::OWN_ID, test::DST_ID, true, 100);
  IoFrame response{};
  ASSERT_EQ(engine.send_and_receive(request, response, FREQ_CH2), ExchangeOutcome::FAILED);

  EXPECT_TRUE(engine.get_debug().capture_valid)
      << "the exchange received a frame, so its report must not claim the radio heard nothing";
  EXPECT_EQ(engine.get_debug().capture_freq_hz, FREQ_CH2) << "and it should describe the frame actually heard";
}

// The radio only clears its capture when it begins a listen, so without an explicit reset a
// fully-silent exchange would inherit the *previous* exchange's capture and claim "we heard a
// frame". This is the exact misread that sent the issue #95 analysis down a wrong path.
TEST(Exchange, FailureReportDoesNotInheritThePreviousExchangesCapture) {
  MockRadio radio;
  radio.set_emulate_capture_lifecycle(true);
  RadioDriver *radio_ptr = &radio;

  TuningConfig tuning;
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  // Exchange 1: a direct status reply arrives and succeeds, leaving a populated radio capture.
  IoFrame reply = build_status_response(test::DST_ID, test::OWN_ID);
  uint8_t raw[64];
  RadioRxPacket pkt{};
  pkt.len = serialize(reply, raw, sizeof(raw));
  memcpy(pkt.data, raw, pkt.len);
  pkt.freq_hz = FREQ_CH2;
  radio.queue_rx(pkt);

  IoFrame request{};
  create_get_status(request, test::OWN_ID, test::DST_ID, /*low_power=*/false);
  IoFrame response{};
  ASSERT_EQ(engine.send_and_receive(request, response, FREQ_CH2), ExchangeOutcome::SUCCESS_WITH_RESPONSE);
  ASSERT_TRUE(engine.get_debug().capture_valid) << "sanity: exchange 1 heard a frame";

  // Exchange 2: nothing on air at all. Its report must describe *this* exchange. This runs the
  // realistic listen path (default budget, no queued TX failures) and only checks FAILED + capture
  // fields on purpose — asserting a try count here would be fragile against the host millis() stub
  // and the total budget (see CappedExchangeDoesProportionallyLessBlockingListening for that).
  IoFrame request2{};
  create_get_status(request2, test::OWN_ID, test::DST_ID, /*low_power=*/false);
  IoFrame response2{};
  ASSERT_EQ(engine.send_and_receive(request2, response2, FREQ_CH2), ExchangeOutcome::FAILED);

  EXPECT_FALSE(engine.get_debug().capture_valid)
      << "a fully-silent exchange must not report the previous exchange's radio capture";
  EXPECT_EQ(engine.get_debug().capture_frame_len, 0);
  EXPECT_EQ(engine.get_debug().capture_rssi_dbm, 0);
}

// ============================================================================
// max_tries: a scheduler-owned status poll caps its transmit attempts at SCHEDULED_POLL_MAX_TRIES
// so a dead device does not block loop() for the full EXCHANGE_RETRY_COUNT product. Everything
// else keeps the default.
// ============================================================================

TEST(Exchange, DebugLineRendersEveryFieldADiagnosisNeeds) {
  // The failure line and the accepted-without-reply line share this renderer, so one test covers
  // both. The fields are the whole point of the lines: a field report has to be able to tell
  // "the radio saw nothing" from "it received something unusable" without the reporter re-running.
  ExchangeEngine::DebugInfo d;
  d.stage = "success_auth_unconfirmed";
  d.tries = 2;
  d.max_tries = 3;
  d.request_cmd = CMD_EXECUTE;
  d.saw_challenge = true;
  d.capture_valid = true;
  d.capture_rx_done = false;
  d.capture_crc_error = true;
  d.capture_freq_hz = FREQ_CH2;
  d.capture_irq_status = 0x000C;
  d.capture_packet_status = 0x20;
  d.capture_reported_len = 34;
  d.capture_frame_len = 23;
  d.capture_rssi_dbm = -47;
  d.final_waits = 3;
  d.final_rx_ignored = 2;
  d.final_rx_failed = 1;
  d.final_rx_irq = 0x0084;

  char buf[EXCHANGE_DEBUG_LINE_SIZE];
  const int written = render_exchange_debug(buf, sizeof(buf), "DA88B6", d);

  ASSERT_GT(written, 0);
  EXPECT_LT(static_cast<size_t>(written), sizeof(buf))
      << "EXCHANGE_DEBUG_LINE_SIZE must hold the whole line: a truncated diagnostic loses exactly "
         "the trailing capture fields it exists to carry";
  const std::string line(buf);
  EXPECT_NE(line.find("device=DA88B6"), std::string::npos);
  EXPECT_NE(line.find("stage=success_auth_unconfirmed"), std::string::npos);
  EXPECT_NE(line.find("tries=2 max_tries=3"), std::string::npos);
  EXPECT_NE(line.find("saw_challenge=1"), std::string::npos);
  EXPECT_NE(line.find("cap_valid=1 cap_rx_done=0 cap_crc_err=1"), std::string::npos)
      << "the three capture flags are what separates a lost challenge answer from a lost reply";
  EXPECT_NE(line.find("cap_irq=0x000C"), std::string::npos);
  EXPECT_NE(line.find("cap_reported_len=34 cap_frame_len=23"), std::string::npos);
  EXPECT_NE(line.find("cap_rssi=-47"), std::string::npos);
  EXPECT_NE(line.find("final_waits=3 final_rx_ignored=2 final_rx_failed=1 final_rx_irq=0x0084"), std::string::npos)
      << "the final_rx_* fields are what separates a reply lost on this side from one never sent";
}

TEST(Exchange, DebugLinePrintsNaForAnUnknownRssi) {
  ExchangeEngine::DebugInfo d;
  d.stage = "failed";
  d.capture_valid = true;
  d.capture_rssi_dbm = RSSI_UNKNOWN_DBM;

  char buf[EXCHANGE_DEBUG_LINE_SIZE];
  ASSERT_GT(render_exchange_debug(buf, sizeof(buf), "DA88B6", d), 0);

  EXPECT_NE(std::string(buf).find("cap_rssi=n/a "), std::string::npos) << buf;
}

// --- Final-wait reception counters -------------------------------------------
// The cap_* fields keep the first informative reception, which in an authenticated exchange is the
// device's challenge, and every re-arm inside the final wait clears the capture. So what the final
// wait heard is counted per event instead. A ManualClock makes a silent wait expire at its deadline
// the way a real radio's does; under the legacy +1 ms clock every empty-queue return would look
// like an early failed reception.

namespace {
/// Queue a valid 0x3C challenge from the device to the hub.
void queue_device_challenge(MockRadio &radio) {
  const uint8_t chal_data[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
  IoFrame challenge = build_challenge(test::DST_ID, test::OWN_ID, chal_data);
  uint8_t raw[64];
  RadioRxPacket pkt{};
  pkt.len = serialize(challenge, raw, sizeof(raw));
  memcpy(pkt.data, raw, pkt.len);
  radio.queue_rx(pkt);
}
}  // namespace

TEST(Exchange, FinalWaitCountsFailedAndIgnoredReceptions) {
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
  radio.set_emulate_capture_lifecycle(true);  // real drivers clear and refill the capture per wait
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning;
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  IoFrame request{};
  create_execute_position(request, test::OWN_ID, test::DST_ID, false, 100);

  queue_device_challenge(radio);
  // During the final wait: one reception that fails to decode, one frame from another device.
  radio.queue_rx_failed_reception(0x0084);
  const uint8_t other_device[3] = {0x11, 0x22, 0x33};
  IoFrame unrelated = build_status_response(other_device, test::OWN_ID);
  uint8_t raw[64];
  RadioRxPacket unrelated_pkt{};
  unrelated_pkt.len = serialize(unrelated, raw, sizeof(raw));
  memcpy(unrelated_pkt.data, raw, unrelated_pkt.len);
  radio.queue_rx(unrelated_pkt);

  IoFrame response{};
  ASSERT_EQ(engine.send_and_receive(request, response, FREQ_CH2), ExchangeOutcome::SUCCESS_UNCONFIRMED);

  const auto &d = engine.get_debug();
  EXPECT_EQ(d.final_waits, 1u);
  EXPECT_EQ(d.final_rx_failed, 1u) << "the failed reception must be counted before the re-arm clears it";
  EXPECT_EQ(d.final_rx_irq, 0x0084u);
  EXPECT_EQ(d.final_rx_ignored, 1u) << "the other device's frame arrived during the wait and was not the reply";
  EXPECT_EQ(d.capture_frame_len, 15u) << "cap_* still describes the challenge, which is why final_rx_* exists";
}

TEST(Exchange, SilentFinalWaitCountsNothing) {
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning;
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  IoFrame request{};
  create_execute_position(request, test::OWN_ID, test::DST_ID, false, 100);
  queue_device_challenge(radio);  // then nothing: the device never closes the exchange

  IoFrame response{};
  ASSERT_EQ(engine.send_and_receive(request, response, FREQ_CH2), ExchangeOutcome::SUCCESS_UNCONFIRMED);

  const auto &d = engine.get_debug();
  EXPECT_EQ(d.final_waits, 1u);
  EXPECT_EQ(d.final_rx_failed, 0u) << "a wait that simply expires is silence, not a failed reception";
  EXPECT_EQ(d.final_rx_ignored, 0u);
}

TEST(Exchange, FirstResponseWaitDoesNotFeedTheFinalCounters) {
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning;
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  IoFrame request{};
  create_get_status(request, test::OWN_ID, test::DST_ID, /*low_power=*/false);
  radio.queue_rx_failed_reception(0x0084);  // lands in the first-response wait; nothing answers

  IoFrame response{};
  engine.send_and_receive(request, response, FREQ_CH2, /*max_tries=*/1);

  const auto &d = engine.get_debug();
  EXPECT_EQ(d.final_waits, 0u) << "no challenge, so no final wait ever ran";
  EXPECT_EQ(d.final_rx_failed, 0u);
}

TEST(Exchange, UnconfirmedAcceptKeepsTheFinalWaitCaptureForTheLogLine) {
  // An EXECUTE that draws a challenge and then silence ends as SUCCESS_UNCONFIRMED, which prints
  // the accepted-without-reply line rather than a failure. That line is only worth printing if the
  // snapshot behind it still describes the final wait, so assert the snapshot, which is what the
  // host log macros cannot show.
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  IoFrame request{};
  create_execute_position(request, comp.node_id_, test::DST_ID, false, 100);

  uint8_t chal_data[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
  IoFrame challenge = build_challenge(test::DST_ID, comp.node_id_, chal_data);
  uint8_t raw_chal[64];
  const uint8_t len_chal = serialize(challenge, raw_chal, sizeof(raw_chal));
  RadioRxPacket chal_pkt{};
  chal_pkt.len = len_chal;
  memcpy(chal_pkt.data, raw_chal, len_chal);
  radio.queue_rx(chal_pkt);
  // Nothing queued for the final wait.

  IoFrame response{};
  ASSERT_EQ(comp.send_and_receive_(request, response, FREQ_CH2), ExchangeOutcome::SUCCESS_UNCONFIRMED);

  const auto &d = comp.exchange_engine_.get_debug();
  EXPECT_STREQ(d.stage, "success_auth_unconfirmed");
  EXPECT_TRUE(d.saw_challenge) << "the challenge is the reason this counts as accepted at all";
  EXPECT_EQ(d.request_cmd, CMD_EXECUTE);

  char buf[EXCHANGE_DEBUG_LINE_SIZE];
  const int written = render_exchange_debug(buf, sizeof(buf), "DEADBE", d);
  EXPECT_LT(static_cast<size_t>(written), sizeof(buf));
  EXPECT_NE(std::string(buf).find("stage=success_auth_unconfirmed"), std::string::npos);
}

TEST(Exchange, MaxTriesOfOneTransmitsExactlyOnce) {
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning;
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  for (int i = 0; i < EXCHANGE_RETRY_COUNT; ++i)
    radio.queue_tx_result(false);

  IoFrame request{};
  create_get_status(request, test::OWN_ID, test::DST_ID, /*low_power=*/false);
  IoFrame response{};
  engine.send_and_receive(request, response, FREQ_CH2, /*max_tries=*/1);

  EXPECT_EQ(radio.get_send_count(), 1) << "max_tries=1 must transmit once, not EXCHANGE_RETRY_COUNT times";
  EXPECT_EQ(engine.get_debug().tries, 1u);
  EXPECT_EQ(engine.get_debug().max_tries, 1u) << "the budgeted cap is surfaced for the failure log";
}

TEST(Exchange, MaxTriesDefaultsToTheFullRetryCount) {
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning;
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  for (int i = 0; i < EXCHANGE_RETRY_COUNT; ++i)
    radio.queue_tx_result(false);

  IoFrame request{};
  create_get_status(request, test::OWN_ID, test::DST_ID, /*low_power=*/false);
  IoFrame response{};
  engine.send_and_receive(request, response, FREQ_CH2);  // no max_tries argument

  EXPECT_EQ(radio.get_send_count(), EXCHANGE_RETRY_COUNT) << "the default is still the full retry count";
  EXPECT_EQ(engine.get_debug().max_tries, EXCHANGE_RETRY_COUNT);
}

TEST(Exchange, MaxTriesIsClampedToTheValidRange) {
  {  // 0 must not mean "transmit nothing".
    MockRadio radio;
    RadioDriver *radio_ptr = &radio;
    TuningConfig tuning;
    ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);
    for (int i = 0; i < EXCHANGE_RETRY_COUNT; ++i)
      radio.queue_tx_result(false);
    IoFrame request{};
    create_get_status(request, test::OWN_ID, test::DST_ID, /*low_power=*/false);
    IoFrame response{};
    engine.send_and_receive(request, response, FREQ_CH2, /*max_tries=*/0);
    EXPECT_EQ(radio.get_send_count(), 1) << "max_tries=0 clamps up to one transmit";
  }
  {  // A too-large value must not exceed the budgeted ceiling.
    MockRadio radio;
    RadioDriver *radio_ptr = &radio;
    TuningConfig tuning;
    ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);
    for (int i = 0; i < EXCHANGE_RETRY_COUNT; ++i)
      radio.queue_tx_result(false);
    IoFrame request{};
    create_get_status(request, test::OWN_ID, test::DST_ID, /*low_power=*/false);
    IoFrame response{};
    engine.send_and_receive(request, response, FREQ_CH2, /*max_tries=*/99);
    EXPECT_EQ(radio.get_send_count(), EXCHANGE_RETRY_COUNT) << "max_tries clamps down to EXCHANGE_RETRY_COUNT";
  }
}

// The tests above pin the outer loop bound via TX failures, which never reach a listen. This one
// pins what the change is actually for: a capped exchange spends proportionally less time in the
// blocking first-response wait. Realistic path (TX succeeds, nothing replies) with the budget
// lifted and the window tiny so the host millis() stub can't trip the budget on its own. The
// first-response wait slices internally, so the count per try is an implementation detail — assert
// the ratio, which is exact because each try re-runs the identical listen.
TEST(Exchange, CappedExchangeDoesProportionallyLessBlockingListening) {
  TuningConfig tuning;
  tuning.exchange_start_response_wait_ms = 50;
  tuning.exchange_total_budget_ms = 60000;

  auto listen_slices_for = [&](uint8_t max_tries) {
    MockRadio radio;
    RadioDriver *radio_ptr = &radio;
    ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);
    IoFrame request{};
    create_get_status(request, test::OWN_ID, test::DST_ID, /*low_power=*/false);
    IoFrame response{};
    engine.send_and_receive(request, response, FREQ_CH2, max_tries);
    return std::make_pair(radio.get_send_count(), radio.wait_timeouts().size());
  };

  const auto one = listen_slices_for(1);
  const auto full = listen_slices_for(EXCHANGE_RETRY_COUNT);

  EXPECT_EQ(one.first, 1) << "max_tries=1 transmits once";
  EXPECT_EQ(full.first, EXCHANGE_RETRY_COUNT);
  EXPECT_GE(one.second, 1u) << "a single try still performs a blocking listen (not just a TX)";
  EXPECT_EQ(full.second, one.second * EXCHANGE_RETRY_COUNT)
      << "the default blocks in the first-response wait EXCHANGE_RETRY_COUNT times as long";
}

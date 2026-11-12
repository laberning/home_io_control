# ADR 0073: A read plans its attempts, and an attempt must fit the budget with its windows

<!-- doxygen-label: adr0073 -->

**Status:** Accepted · **Recorded:** 2026-11

Amended by [ADR 0074](0074-settling-reads-take-the-attempt-plan-and-progress-reads-stay-single-tries.md)
(which reads take the plan: settling reads do, progress reads stay single tries) and by
[ADR 0075](0075-commands-and-the-key-pull-take-the-attempt-plan-and-early-attempts-listen-short.md)
(commands and the key pull take it; early attempts to a mains device listen short; every attempt costs
a measured overhead).

Amends [ADR 0013](0013-blocking-exchange-on-the-esphome-loop.md) (the budget arithmetic: what an
attempt costs), [ADR 0043](0043-an-unconfirmed-movement-command-is-resent-once-to-a-device-that-normally-confirms.md)
(the re-send gap is the total wait, not an addition to the retry gap),
[ADR 0044](0044-chip-neutral-transmit-intent-wake-level-idle-scanning-and-air-time.md) (the budget
rule counts an attempt's windows, not only its transmission) and the wording of
[ADR 0040](0040-low-power-start-preamble-follows-the-wake-belief.md) and
[ADR 0066](0066-a-cover-is-read-at-rest-by-an-opt-in-status-refresh.md) where they say "three tries".

## Context

An exchange made at most three attempts, 250 ms apart, each listening 400 ms, inside a 2500 ms
budget (ADR 0013). The check that stopped a further attempt counted the time spent, the gap and the
attempt's transmission, but not the window the attempt then listens or the final-reply window a
challenging device adds. With three attempts the shortfall stayed hidden; with more it would not: a
budget that admits an attempt it cannot finish blocks the loop past ESPHome's 2550 ms warning, and
`exchange_total_budget_ms` goes up to 12000, which admits 25 attempts.

A read that decides where a cover ended up is worth more attempts than three, and they need not wait
250 ms between each: an attempt that hears nothing has already listened its whole window from the end
of its own transmission, so the next one can follow at once. Six silent mains attempts fit the budget
only that way (6 x 412 ms = 2472 ms); with the gap only three do.

A low-power receiver listens for a wake-up preamble and answers late. The corpus has 93 low-power
requests this hub transmitted, in 83 captures: the median answer came 248 ms after the end of the
request, the 95th percentile 354 ms, and the four slowest 647 to 4375 ms. The 400 ms window sits just
above the 95th percentile.

## Decision

- **An attempt starts only if all of it fits.** Time spent, the gap before the attempt, its
  transmission, the window it then listens, and, once the target has challenged in this exchange, the
  final-reply window must stay below `exchange_total_budget_ms`. The unconfirmed-command re-send
  counts its gap, transmission and window, not a final-reply window: it is the last copy. The
  guard ahead of the re-sent attempt leaves that window out too, so a re-send whose check passed is
  sent. A try that is the last the exchange allows plans no re-send and waits no re-send gap.
- **A read has a plan** (`decisions::attempt_plan()`): the number of attempts, the gap before each
  further one, and the first-reply window of every attempt, from what the read is for and whether
  the target is low-power. A settling read (one that decides state at rest) has no gap and as many
  attempts as fit the budget, at most `EXCHANGE_MAX_ATTEMPTS` (9) whatever the budget. A low-power
  settling read has three, each listening `LOW_POWER_RESPONSE_WAIT_MS` (550 ms: about 1.5 times the
  95th percentile, and below every slower sample). Reads of a device that is probably still
  busy keep one attempt, and everything else keeps three attempts with the 250 ms gap and the tuned
  windows.
- **The engine takes a plan.** `ExchangeEngine::send_and_receive()` accepts an attempt plan; the
  `max_tries` form builds today's plan from the number, clamped to `EXCHANGE_RETRY_COUNT`, so a
  caller that does not ask for a plan behaves as before, except for the guard.
- **The re-send keeps its 750 ms.** The ADR 0043 gap is the total wait before the re-send; the
  engine sleeps what the plan's own gap does not already cover.
- **Callers move to plans one at a time.** The places that assume three attempts stay at three until
  they take a plan: commands, one-off reads, heater registers, the pairing loops and the
  scheduler-owned polls. The ADR 0040 settle poll after a `stop` and the ADR 0066 status refresh
  keep their three tries until then; once they take the settling plan, "three tries" there reads
  "the settling plan's attempts". A pairing loop keeps its own count: its tries are separate
  exchanges and not a budgeted read.

## Consequences

- A challenged exchange to a low-power device no longer starts a third try: two challenged tries
  (a wake-up transmission and a 500 ms final wait each) leave no room for a third inside 2500 ms.
  A mains device that challenges still makes three. Raise `exchange_total_budget_ms` for a low-power
  device that challenges on every try and needs the third.
- Nothing else changes for a caller that has not asked for a plan. A plan with no gap makes a failing
  settling read block the loop for nearly the whole budget; the budget bounds it, and a STOP waits
  behind it for as long as it runs.
- The budget is the only bound on a plan besides the cap, so lowering it lowers the attempt count of
  every plan; the plan never promises an attempt the guard would refuse for a silent target.
- The 550 ms low-power window rests on 93 samples that are mostly not battery sensors; the four
  slower ones are pairing and key-exchange replies. 89 of the 93 arrive within 400 ms and within
  550 ms alike, so the longer window buys margin for a late reply, not a sample the corpus shows. A bench with a real low-power device may move it.

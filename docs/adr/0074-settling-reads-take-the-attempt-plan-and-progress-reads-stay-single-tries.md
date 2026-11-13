# ADR 0074: Settling reads take the attempt plan, and progress reads stay single tries

<!-- doxygen-label: adr0074 -->

**Status:** Accepted · **Recorded:** 2026-11

Amended by [ADR 0075](0075-commands-and-the-key-pull-take-the-attempt-plan-and-early-attempts-listen-short.md)
(a mains settling read makes up to nine attempts, early ones listening a shorter window) and by
[ADR 0076](0076-a-moving-cover-is-read-once-at-its-predicted-end-of-travel.md) (the read at the predicted end of a
move is a settling read; a stopped report never schedules from its count), and by [ADR 0079](0079-a-cover-is-followed-by-its-execution-state.md)
(a settling status read to a low-power cover in an unknown state leads with one short attempt before its three).

Amends [ADR 0073](0073-a-read-plans-its-attempts-and-an-attempt-must-fit-the-budget-with-its-windows.md)
(which reads take the plan), [ADR 0066](0066-a-cover-is-read-at-rest-by-an-opt-in-status-refresh.md)
(the refresh takes the settling plan), the "three tries" of
[ADR 0040](0040-low-power-start-preamble-follows-the-wake-belief.md) (the poll after a `stop`) and the
worst-case blocking arithmetic of [ADR 0013](0013-blocking-exchange-on-the-esphome-loop.md).

## Context

ADR 0073 gave the engine an attempt plan and a guard that keeps every attempt inside the exchange
budget, but left every caller at today's attempt counts. A status read that decides where a cover
ended up is the read worth repeating: after a `stop` nothing moves, the entity shows the last
reported position until the read is answered, and a lost frame costs the whole backoff slot
(5 s or more) before the next look. A read during a move is the opposite: the device is busy and
its silence is expected, so a repeat changes nothing and only fills the air.

## Decision

- **Which reads settle.** A status read is a settling read, and takes `attempt_plan()` with
  `ReadKind::SETTLING`, when it is
  - the poll after an accepted `stop`,
  - the poll that confirms a cover that reported stopped short of its target (the hub marks it when
    it sees that report, as it marks the poll after a `stop`),
  - a scheduled poll in the middle band of the backoff ladder (the three slots after the first
    miss), or
  - a read with no ladder behind it: a one-off poll and the opt-in status refresh.
- **Progress reads stay single tries.** Every other scheduled poll of a cover (the first slot after
  a command, the slots past the band, a challenge-answering device) keeps one attempt and the
  backoff ladder as its retry. This is a deliberate airtime and responsiveness choice: the device
  is probably busy, and a `stop` pressed during a move must not wait behind a burst of reads.
- **Heaters keep their counts.** Heater and water heater polls, rain, sun and battery polls, and
  commands keep the count-based budget (`scheduled_poll_max_tries()`, `EXCHANGE_RETRY_COUNT`).
  Heating-level heaters share the cover status path but branch before the plan.
- **Callers ask the engine for the plan.** `ExchangeEngine::plan_read()` builds the plan for a
  request, with the transmit time the engine itself would plan for it (the radio's estimate for the
  request's start preamble), so the plan and the budget guard agree. The hub passes a read kind
  where it used to pass a count.
- **The post-`stop` poll, the refresh and the one-off poll no longer say "three tries".** They make
  as many attempts as the plan gives: up to six at the defaults for a mains-powered cover
  (a 32-byte start preamble and a 400 ms window), three for a low-power one.
  On the bench boards (SX1276, SX1262 and LR1121) a read to an address no device answers made five
  attempts 422 ms apart and gave up after about 2.15 s: the guard counts the time that has really
  passed, and each attempt's overhead on the radio leaves no room for a sixth.

## Consequences

- **Worst case on the loop.** A settling read whose every attempt is lost blocks the loop for nearly
  the whole budget (about 2.5 s at the default), where three attempts with gaps blocked it for about
  1.6 s. A `stop` queued during it runs as soon as the read gives up; the operation queue runs a
  user command ahead of a queued poll, so at most one settling read stands in front of it. The loop
  stays below ESPHome's 2550 ms warning because the guard bounds every attempt (ADR 0073).
- **Airtime.** A settling read of a silent mains cover sends up to six requests instead of three.
  The refresh pays this once per interval for a cover that does not answer. The status read of a
  cover that does answer is unchanged: it returns at the first reply.
- **Low-power targets.** A settling read to a low-power device makes three attempts, each
  listening 550 ms after its wake-up preamble, with no gap, instead of three attempts listening
  400 ms with a 250 ms gap. A target that challenges has fewer: two challenged attempts leave no room
  for a third inside 2500 ms, as ADR 0073 states for every challenged low-power exchange. Raise
  `exchange_total_budget_ms` for a low-power device that challenges every poll and needs the third.
- **Stale marks.** The mark that makes the next poll a settling read is dropped by a new command,
  by clearing the tracking window and by the poll that takes it, so a later poll never inherits
  one.
- **ADR 0066 and 0040 wording.** Where they say the refresh or the poll after a `stop` has three
  tries, it reads: the settling plan's attempts.

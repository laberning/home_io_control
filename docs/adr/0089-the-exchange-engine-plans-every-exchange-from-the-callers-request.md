# ADR 0089: The exchange engine plans every exchange from the caller's request

<!-- doxygen-label: adr0089 -->

**Status:** Accepted, amended by [ADR 0095](0095-a-resting-battery-device-gets-a-2048-byte-wake-up-and-three-attempts.md) · **Recorded:** 2026-11

Amends [ADR 0074](0074-settling-reads-take-the-attempt-plan-and-progress-reads-stay-single-tries.md) (the point
"Callers ask the engine for the plan").
Amended by [ADR 0095](0095-a-resting-battery-device-gets-a-2048-byte-wake-up-and-three-attempts.md) (point 3: the short lead no longer has a budget allowance; a low-power wake-up
plan is bounded by its attempt count instead).

## Context

ADR 0074 has callers get the attempt plan from `ExchangeEngine::plan_read()` and hand it to
`send_and_receive()`. Since then `ExchangeOptions` is the one value `send_and_receive()` takes, and the
hub passes a read kind; the engine plans from it with the transmit time it estimates for the request
(`plan_for_()`). The ready plan and `plan_read()` survived only as a test seam: the exchange tests used
them to pin exact cases, such as an attempt count above the limit, a plan with no budget allowance or a
hand-set gap, which no request can produce. A test that hands the engine a plan the engine would never
build does not run the path production runs.

## Decision

1. **One way in.** `send_and_receive()` plans every exchange itself, once, from `ExchangeOptions::attempts`
   (a try count or a read kind) and the tuning. `ExchangeOptions` has no plan field and no constructor from a
   plan, and `ExchangeEngine` has no `plan_read()`. A loop that runs its own attempts (the key pull) asks
   `plan_attempts()` for its plan, under the same budget rule.
2. **The plan builders own the attempt range.** `decisions::attempt_plan_for_tries()` and
   `decisions::attempt_plan()` give an attempt count in `[1, EXCHANGE_MAX_ATTEMPTS]` for every read kind,
   power class, tuning and try count; a sweep test in the decisions suite checks it. The engine runs the count
   as it is and has no clamp of its own.
3. **Rules the request path cannot reach are tested where they live.** The budget allowance of the short lead
   is tested on `decisions::attempt_plan()` and `decisions::attempt_fits()`; exchange tests reach their edge
   cases through the read kind, the target's power class and the tuning (windows, early window, budget).

## Consequences

- Every exchange test runs the path the hub uses, so a change to the planning is seen by all of them.
- The engine's guard no longer has a defence against a plan with too many attempts; a new plan builder must
  keep the range, and the sweep test fails if it does not.
- A test cannot construct an exchange with a gapless plan of a count the builders do not give; the edge it
  pinned (the guard honours the allowance) is covered on the pure functions instead.

## Considered options

- **Keep the ready plan as a documented test seam.** No test work, but about thirty tests run a path
  production never takes, and the seam invites a caller to bypass the planning.
- **Keep the engine's clamp as defence.** A branch no request reaches and no test can reach.

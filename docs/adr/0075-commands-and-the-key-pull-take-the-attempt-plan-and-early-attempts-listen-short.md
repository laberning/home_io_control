# ADR 0075: Commands and the key pull take the attempt plan, and early attempts to a mains device listen short

<!-- doxygen-label: adr0075 -->

**Status:** Accepted · **Recorded:** 2026-11

Amends [ADR 0073](0073-a-read-plans-its-attempts-and-an-attempt-must-fit-the-budget-with-its-windows.md)
(commands take the plan; every attempt costs a measured overhead; the early window),
[ADR 0074](0074-settling-reads-take-the-attempt-plan-and-progress-reads-stay-single-tries.md) (the
settling plan's attempt count and windows) and the scope of
[ADR 0043](0043-an-unconfirmed-movement-command-is-resent-once-to-a-device-that-normally-confirms.md)
(what a challenged request that never closes may be followed by).

## Context

A command to a device that missed its frame made three attempts, 250 ms apart, each listening 400 ms.
A mains-powered device answers within tens of milliseconds or not at all, so most of that listening
is waiting for a reply that is not coming, and three attempts leave a command exposed to two lost
frames in a row. A settling read of a silent mains device made five attempts on the bench boards
(SX1276, SX1262 and LR1121) where the plan said six: each attempt cost about 10 ms more than its
transmit estimate plus its window (channel check, hop and SPI around every transmit), so attempts came
422 ms apart where the plan assumed 412 ms.

The answer window of a mains device is measured, not guessed. Across the corpus, 47 of the 52 start
requests this hub sent to a device that is not declared low-power drew their first reply within
98 ms (median 66 ms); the other five are a stalled key extraction and replies that belong to a later
request. On the bench, an Izymo dimmer answered 36 to 42 ms after the start of the hub's transmission
on SX1262 and LR1121 boards, in 400 exchanges none of which needed a retry; its carrier came up
within about 0.1 ms of the end of the hub's frame. A Sunea awning answered its challenged status
reads 33 to 49 ms after the start of the hub's transmission on SX1276, SX1262 and LR1121 boards.

A command that a device already heard must not be copied further than before: an authenticated
EXECUTE is challenged before the device acts, so silence means it did not act, but a request the
device challenged and then never closed may well have been carried out.

## Decision

- **Repeat-safe commands take a plan.** A command whose copies have one effect
  (`decisions::is_repeat_safe_command()`: an absolute position, open, close, stop, a read) is sent
  with the command plan. To a mains device that is as many attempts without a gap as fit
  `exchange_total_budget_ms`, at most `EXCHANGE_MAX_ATTEMPTS` (9). To a low-power device it stays
  three attempts with the retry gap. A stored-position selector (favourite, vent, partial,
  pedestrian) is not repeat-safe, because a second copy can stop what the first started, and keeps
  the three attempts of the count-based exchange.
- **Early attempts listen short.** In a plan without a gap to a mains device (the command plan and the
  settling plan), every attempt but the last listens `exchange_early_response_wait_ms` (default 200 ms,
  at least 150 ms in YAML), and the last listens the whole `exchange_start_response_wait_ms`, so a
  device that answers late still gets one full listen. 200 ms is twice the slowest corpus reply of
  the class and about four times the bench maximum, and the largest round value at which nine
  attempts fit the default budget. A low-power target keeps one window for every attempt. A value
  above the start window is capped to it.
- **An attempt costs its overhead.** The budget rule and the plan both add
  `EXCHANGE_ATTEMPT_OVERHEAD_MS` (10 ms) to every attempt, through one function
  (`decisions::attempt_fits()`), so the plan promises exactly the attempts the guard admits: five
  attempts of 422 ms for the old 400 ms window, nine at the new defaults. The final-reply window a
  challenged attempt adds now includes the 0x3D's own air time and its overhead, which closes the
  case where a challenge at the very end of a window ended a few milliseconds past the budget.
- **A challenged request that never closes is not copied further.** A CMD_EXECUTE keeps ADR 0043's
  single re-send, and only to a device that normally confirms. Every other request is followed by
  at most the copies that bring the exchange to three attempts in all, whatever its plan: a larger
  plan never becomes up to nine copies of a write the device holds.
- **The key pull takes the same plan.** The 0x38 key request is repeated by the command plan of the
  device's power class, through the same budget rule, with the key-transfer reply window as the last
  attempt's listen and the early window before it.
- **What stays at three attempts:** the discovery and pairing challenge loops, the key-held check,
  heater register reads and writes and every other caller of the count-based exchange, which keep
  their own counts and gaps.

## Consequences

- **A command to a mains device that missed a frame gets up to nine attempts about 220 ms apart
  instead of three about 650 ms apart**, and still blocks the loop for less than the budget (about
  2.2 s when every attempt is lost, where three attempts blocked about 1.7 s). A silent command costs
  up to nine short frames of airtime instead of three; the cost of a command that is answered is
  unchanged.
- **Settling reads of a mains device make nine attempts, not five**, with the same blocking bound.
  A `stop` queued behind a failing settling read waits for the budget at most.
- **A device that answers between the early window and the full window can be sent a second copy
  before its first reply lands.** The device answers each copy it hears, and a repeat-safe request has
  one effect, so this costs airtime, not a second action; the late reply still ends the exchange.
  Raise `exchange_early_response_wait_ms` for a mains device that answers later than 200 ms, with
  fewer attempts as the price.
- **Challenged copies of a write are bounded by what was sent before.** Silent attempts count toward
  the three, so a write the device challenged on its fourth attempt is not copied again.
- **Not decided here:** a new radio channel for each attempt and treating a busy channel as an
  attempt, and a per-device window from the turnaround class a device reports at discovery.

## Considered options

- **A shorter window on every attempt.** Rejected: a device that answers later than the early
  window would then never be heard; the full window on the last attempt keeps it reachable.
- **A window per device from the turnaround class it reports at discovery.** Deferred: a device
  declared in YAML has none, so one tunable window with a floor covers every mains device today.

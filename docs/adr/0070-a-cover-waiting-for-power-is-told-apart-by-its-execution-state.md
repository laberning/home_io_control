# ADR 0070: A cover waiting for power is told apart by its execution state

<!-- doxygen-label: adr0070 -->

**Status:** Accepted · **Recorded:** 2026-11

## Context

A cover that reports "stopped" away from its target is taken as at rest once a second report
agrees (`decisions::classify_stopped_report()`), and the follow-up polls end. That is right for a
cover that froze, met an obstacle, was limited by a sensor or was stopped by its user.

A power supply that runs one motor at a time produces the same two reports for another reason.
Two VELUX SML roller shutters on one KUX 110 run in turn: the second accepts its command, stays in
place, and starts when the first has finished. The hub shows it at rest and stops reading it, and
the move that follows is unseen until the next command or status refresh.

The ordinary status read does not separate the two cases. The main-parameter status read a KLF 200
gateway sends (`create_status_mp_fp_read()`) does: its reply begins `2D` on every cover read at
rest, `2B` on a queued SML shutter and `2C` once that shutter's motor runs. Read as
`(code << 3) | state` with the node states of the KLF 200 API specification v3.18, that is state 5
(done), state 3 (waiting for power) and state 4 (executing).

## Options considered

- **Keep polling every cover that stops away from its target.** Rejected: a frozen or obstructed
  cover would show movement for the whole follow-up window, which ADR 0061 bounds on purpose.
- **Read the state from the ordinary status reply.** Rejected for now: every such reply in the
  corpus begins `04` or `05`, which fits a state in the low bits as well as a single stopped flag,
  and none was captured from a waiting cover.
- **Read the execution state on every stop away from the target, for every cover.** Rejected: one
  more transmission for every install, for a state most installs cannot have, and a decode that
  rests on two measured replies from one product.
- **Read the execution state once two reports agree, for a cover that opted in** (chosen).

## Decision

- A cover that declares `follow_waiting_for_power: true` is not taken as at rest on two agreeing
  reports inside a follow-up window. The hub sends the main-parameter status read first and acts
  on its first byte (`decisions::power_wait_step()`).
- Waiting for power keeps the cover handled as moving toward its target, shows
  `NODE_WAITING_FOR_POWER` on its Active Issue sensor, and repeats the read every
  `POWER_WAIT_POLL_INTERVAL_MS` (10 s) through the follow-up poll schedule.
- Executing hands the cover back to the ordinary follow-up polls. Every other state, a first byte
  whose code is not the measured one, and an unanswered first read conclude as before.
- The follow-up window is the safety bound: a cover still waiting when it runs out is shown at
  rest. A command, a STOP or another controller's activity ends a check or a wait, and the polls
  they start decide afresh.
- The reply is used inside its own exchange and never reaches the status decoder (ADR 0024). The
  hub takes only the state from it; position and target stay with the ordinary status read.

## Consequences

- **A cover without the key behaves as before**, and so does an opted-in cover that an untracked
  read finds resting away from its target.
- **A move that ends on target costs nothing more.** A stop away from the target costs one
  unauthenticated read; a wait costs one every 10 seconds, about 60 at most.
- **A waiting cover shows opening or closing while it stands still.** That is the pending
  direction, and the Active Issue sensor says why.
- **The decode rests on replies from one product.** Done, waiting for power and executing were
  measured on a VELUX SML; the byte for an error comes from the specification's state numbers, not
  from a capture. A device that reports another state once it moves ends its wait with the
  ordinary read that follows, or with the window; a wait cannot outlast the window.
- **A STOP is not known to withdraw a queued command.** If the shutter still reports waiting after
  a STOP, the cover shows waiting again once the polls after the STOP have agreed.

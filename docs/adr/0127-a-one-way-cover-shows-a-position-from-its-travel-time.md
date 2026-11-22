# ADR 0127: A one-way cover shows a position from its travel time

<!-- doxygen-label: adr0127 -->

**Status:** Accepted, amends [ADR 0117](0117-a-one-way-controller-can-show-an-assumed-state-cover.md) · **Recorded:** 2026-11

Stays within [ADR 0018](0018-yaml-is-the-source-of-truth-hub-persists-nothing.md) and
[ADR 0025](0025-persist-monotonic-counters-as-the-only-exception-to-0018.md): the hub persists its counters and
nothing else.

## Context

ADR 0117 gives a one-way identity of a cover kind an assumed-state cover with no travel time: open, closed, or open
and idle after a stop. A one-way receiver never reports where it is, so the cover cannot show a position, and users
who want one build ESPHome's `time_based` cover next to it (issue #74). The hub already knows when each of its own
commands went out and when it heard a followed remote's press; with the time a cover takes to open and to close, it
can work out the position the same way the `time_based` cover does, without a second entity and without sending the
remote's command twice.

## Decision

- **Two optional keys** on an identity with `cover:`: `open_duration:` and `close_duration:`, both or neither, each
  above zero. Without them the cover behaves exactly as ADR 0117 describes.
- **An estimate, not a reading.** With the durations the cover reports a position and shows opening or closing while
  the estimate moves. The estimate is a pure function of the last estimate, the last intent and the time, written
  without cover terms so that other entities can use it; time differences are unsigned, so a `millis()` wrap does not
  disturb it.
- **What moves it.** The same two inputs as ADR 0117: the identity's own commands that reached the radio, and the
  presses of the remotes listed under `follows:`. Open and close start a run towards the end at the matching rate. A
  stop freezes the estimate where it is. A run that lasts its whole duration ends at the end stop, whatever the
  estimate was before, so a full run corrects every earlier error. A position frame moves the estimate to that
  percentage at the travel rate and stops it there, on the assumption that the cover does the same; no cover on
  record confirms that it obeys a position frame. A favourite or ventilation press, or an unknown value, changes
  nothing, because the hub does not know where that position is.
- **Publishing.** About once a second while the estimate moves, and at every change of direction, stop or end of
  run, from the entity's loop; no task of its own.
- **Not persisted.** After a boot the cover shows fully open and idle, as in ADR 0117; the first full run corrects it.
- **The slider is not part of this record:** the cover's `control()` still sends open, close and stop, and a
  position the user asks for goes to its nearer end.

## Considered options

- **Leave it to the `time_based` cover.** It works, but needs a second entity, a template or an automation to follow
  the remote, and sends the remote's command again. Kept in the docs as the alternative.
- **Measure the travel times with a calibration run.** Rejected: nothing tells the hub when a one-way cover reaches
  its end; the user times it once and types the values.
- **Persist the estimate.** Rejected: ADR 0025 allows counters only, and a stored position goes stale with every
  press the hub did not hear while it was off.

## Consequences

- A one-way cover with travel times shows a moving position in Home Assistant that follows the wall remote.
- The position is wrong after a press the hub missed, a command the device ignored, a favourite press, a hand stop
  or a boot, until the next full run to an end; the docs say so.

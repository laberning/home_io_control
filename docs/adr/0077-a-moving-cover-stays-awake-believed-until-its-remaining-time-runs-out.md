# ADR 0077: A moving cover stays awake-believed until its remaining time runs out

<!-- doxygen-label: adr0077 -->

**Status:** Accepted · **Recorded:** 2026-11

Amends [ADR 0040](0040-low-power-start-preamble-follows-the-wake-belief.md) (when a silent exchange
spends the moving evidence, and the preamble of the predicted-end read) and builds on
[ADR 0076](0076-a-moving-cover-is-read-once-at-its-predicted-end-of-travel.md).

## Context

A battery or solar receiver that is travelling hears the short start preamble and ignores the long
wake-up one; a resting one needs the long one (ADR 0040). The hub keeps a per-device belief: moving
evidence younger than two minutes leads the first try with the short preamble, and an exchange that
draws no frame while the belief is "moving" spends the evidence, so the next exchange leads with the
wake-up preamble.

That rule reads one lost frame as "it has come to rest". A VELUX SSL solar shutter froze after one
missed short-preamble poll in a field log (PR #148): the hub switched to the wake-up preamble,
which the still-moving shutter ignored, and the position stayed put until the shutter stopped. A
moving cover that counts reports how long it still has to travel, so the hub can tell a lost frame
from a stopped cover.

## Decision

- **The belief learns the end of travel.** A trusted status reply that says moving and carries a
  remaining time R records `moving_until` = the time of the report + R + 1 s (the margin of
  ADR 0076). Every sign of travel without a count, every report that says stopped and every cleared
  evidence forgets it. A reply to a command carries no usable count and sets none.
- **Silence before that time does not spend the evidence.** An exchange to a target believed awake
  that draws no frame spends the moving evidence only when `moving_until` is unknown or has been
  reached. Before it, the next poll leads with the short preamble again. A silent `stop` still
  spends nothing, as before.
- **The predicted-end read leads with the wake-up preamble.** The read of ADR 0076 is aimed at the
  moment the cover has come to rest by its own count, where only the wake-up preamble reaches it. To
  a low-power target it sends that preamble on every attempt, whatever the belief says. A target
  that is not low-power is unaffected, and every other read keeps the short-first rule of ADR 0040.

## Consequences

- **A missed frame mid-move costs one poll.** The position keeps updating on the next progress read
  instead of freezing until the cover stops.
- **A cover that reports no count is unchanged.** Its belief behaves as in ADR 0040.
- **A cover that stops early.** Its next report says stopped and clears the belief, as before. If it
  stops and never answers again before its count has run out, the hub leads short for the polls up to
  that time, which costs a try each and no more.
- **Silence at the predicted end spends nothing.** The override takes the belief out of that exchange,
  and a cover still travelling past its count ignores the wake-up preamble. The next read leads with
  the short preamble and sends the wake-up one on its later attempts, so it reaches the cover either
  way; if it draws no frame either, it spends the evidence, because the count has run out.
- **Hardware status.** Experimental. No low-power cover that reports a remaining time is on the
  bench, and the corpus holds no capture of one; the behaviour is covered by host tests only. It is
  confirmed by a log from a VELUX solar shutter or similar.

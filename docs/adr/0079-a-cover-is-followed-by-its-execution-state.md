# ADR 0079: A cover is followed by its execution state

<!-- doxygen-label: adr0079 -->

**Status:** Accepted · **Recorded:** 2026-11

Amends [ADR 0061](0061-a-moving-status-report-opens-the-follow-up-window.md) (the window's length),
[ADR 0076](0076-a-moving-cover-is-read-once-at-its-predicted-end-of-travel.md) (what follows a frozen
count), [ADR 0040](0040-low-power-start-preamble-follows-the-wake-belief.md) and
[ADR 0074](0074-settling-reads-take-the-attempt-plan-and-progress-reads-stay-single-tries.md) (the
attempts of a settling status read to a low-power cover whose state is unknown), and, for tunings
near the limit, the worst-case blocking time of
[ADR 0013](0013-blocking-exchange-on-the-esphome-loop.md).

## Context

Byte 0 of a status reply (`0x04`) and a status update (`0x71`) packs a code and a state as
`(code << 3) | state`, the same layout as the error reply. The hub read only bit 0 of it, "stopped".
The VELUX KLF 200 API v3.18 (Table 105) defines the state: 0 non-executing (the device could not
run the command), 1 error while executing, 2 not used, 3 waiting for power, 4 executing, 5 done. Every
ordinary status reply in the corpus carries 4 or 5, where both readings agree. They disagree on 3 and
0. A VELUX SML shutter that waits behind a second shutter on the same KUX 110 power supply reports 3
(issue #230, corpus `velux_sml_probe_execution_state_waiting_then_executing_sx1262`): bit 0 reads it
as stopped, so the hub stops following it and Home Assistant misses the move that follows.

Two bounds on the follow-up were wider than the evidence needs. The window ran for 600 s; the slowest
travel in our evidence is a VELUX roof window at silent speed, about 65 s (issue #205), the longest
count in the corpus is 35 s, and a cover queued behind another on a shared supply makes two such
travels, about 130 s. A frozen count (three reports whose remaining time did not fall, ADR 0076)
dropped back to progress reads every 3 s for the rest of the window, which reads a stuck cover up to
about 200 times.

A read with no fresh evidence (a status refresh, the read after boot, a catch-up read) goes to a
low-power cover behind the wake-up preamble. A moving VELUX screen ignores that preamble (issue #209,
0 of 12; issue #114, about 1 in 19), so the read draws nothing, no window opens and the move is lost.
A resting one needs it.

## Decision

1. **Byte 0 is the execution state.** `execution_state_of()` decodes it with the error reply's state
   mask. States 2, 3 and 4 read as moving; 0, 1 and 5 as not moving. The undefined 6 and 7 fall back
   to bit 0 with a warning. The raw byte stays in the log.
2. **A waiting cover is followed.** State 3 (and 2) keeps the follow-up window and the
   predicted-end read as a moving report does, and the cover is shown moving toward its target;
   Active Issue shows `NODE_WAITING_FOR_POWER` while it lasts. States 0 and 1 settle the cover like
   a stopped report (one off its target is confirmed by one more read, ADR 0074), and Active Issue
   shows them until the next state 2 to 5. Following state 3 in an ordinary status reply is
   experimental until such a reply is captured; the #230 captures show it in the main-parameter
   read only.
3. **The window lasts 240 s.** It is still fixed when it opens (ADR 0061 point 2): at the hub's own
   or an overheard command, or at the first moving report when none of those opened it. 240 s
   covers the queued slow case with almost twice its length as margin. A cover still reporting
   moving when it ends is shown at rest.
4. **A frozen count backs off.** From the third report whose count did not fall, the next reads
   wait 10 s, 15 s, then 20 s each, instead of 3 s. The slowest falling count we have seen drops by
   3 s between two 3 s polls (issue #230), so a slow but live count is never taken as frozen. A
   count that falls again returns to the predicted-end read and resets the steps.
5. **An unknown low-power cover is tried short first.** A settling status read with more than one
   attempt, to a low-power cover with no moving evidence and no status report within the longest
   travel, leads with one short-preamble attempt with a 100 ms window (a moving VELUX answers a
   short start frame within 41 to 47 ms, issue #114), followed by the three wake-up attempts it had.
   The read after a `stop` to such a cover is one of them. A single-try read keeps the wake-up
   preamble, and a silent short attempt spends no evidence. Commands are unchanged.
6. **The short attempt may run over the budget, and only by its own cost.** At the default tuning the
   four attempts fit `EXCHANGE_TOTAL_BUDGET_MS` (2500 ms): 122 ms for the short attempt (12 ms
   transmission, 10 ms overhead, 100 ms window) plus three times 778 ms is 2456 ms. When a tuning
   makes them not fit, the plan and the engine's guard both grant this read the short attempt's cost
   on top, from one value, so no wake-up attempt is dropped.

## Consequences

- A cover queued behind another is shown opening or closing while it waits, and its move is
  followed to the end.
- A cover that keeps reporting moving costs at most 240 s of reads per window, and a stuck count
  costs about a dozen reads instead of up to about 200.
- The retry ladder's 300 s rungs after a failed read lie past the window's end, so a window now ends
  before they are reached.
- A settling status read to a low-power cover in an unknown state costs about 122 ms more air and
  blocking time. With a tuning that already sits near the budget, the worst case grows by that cost
  (about 2.62 s at a 2500 ms budget), above the 2550 ms at which ESPHome warns about a slow
  operation (ADR 0013); the default stays inside it.
- Remaining-time and state reasons beyond the six states (why a device could not run) are not
  decoded; their carrier is not known.

## Considered options

- **An opt-in extra main-parameter read to follow a waiting cover (PR #231).** Rejected: the state
  is already in every ordinary status reply, so no extra frame and no setting are needed.
- **Keep the 600 s window and the 3 s cadence after a frozen count.** Rejected: neither bound rests
  on a travel we have seen, and the cadence reads a stuck cover for ten minutes.
- **Short, wake-up, wake-up for the unknown read (three attempts).** Rejected: a resting cover loses
  one of its three wake-up tries on every refresh, boot and catch-up read, while a wake-up try at
  rest is answered only about half the time.

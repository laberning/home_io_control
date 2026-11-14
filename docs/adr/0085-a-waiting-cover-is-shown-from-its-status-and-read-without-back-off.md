# ADR 0085: A waiting cover is shown from its status and read without back-off

<!-- doxygen-label: adr0085 -->

**Status:** Accepted · **Recorded:** 2026-11

Amends [ADR 0079](0079-a-cover-is-followed-by-its-execution-state.md) (point 2: where a waiting state
is shown, and its experimental note; point 4: the frozen-count back-off). It also corrects the Context of
[ADR 0065](0065-a-covers-travel-speed-is-a-three-way-choice.md): the VELUX SML capture
`velux_sml_exchange_execute_ack_waiting_for_power_sx1262` answers the 8-byte EXECUTE form with result
code 0, so not every device in the corpus calls its speed parameter unknown.

## Context

ADR 0079 follows a cover by the execution state in byte 0 of its status answers. States 2 and 3 say
the cover waits (for another motor on the same power supply, for example) and read as moving, and
Active Issue shows `NODE_WAITING_FOR_POWER` while they last. Following state 3 in an ordinary status
reply was experimental, because only the main-parameter read had been captured with it.

A VELUX SML shutter behind a KUX 110 (issue #230) shows three things. It answers every command with
state 3 in the acknowledgement, even when nothing else is moving, and its first status read 3 s later
reports state 4; so Active Issue showed a waiting state for about 3 s on every move. While it really
waits, its remaining time stays at 5 s without counting down, so the frozen-count back-off of
ADR 0079 point 4 stretched the reads to 10, 15 and 20 s, and a 6 s move fell between two reads. And
an ordinary status read while it waits answers state 3 several times in a row, then state 5 at rest:
the case ADR 0079 marked experimental.

## Decision

1. **A waiting state in the acknowledgement of the hub's own command counts as moving but sets no
   Active Issue.** The cover is followed as for any moving answer. Only a status read or a status
   report with state 2 or 3 shows `NODE_WAITING_FOR_POWER`, so a cover that really waits still shows
   it at the first read after the command.
2. **No frozen-count back-off while the state is 2 or 3.** A count that does not fall while the cover
   waits is expected; the reads keep the cadence the remaining time gives. The follow-up window of
   ADR 0079 point 3 (240 s) still ends a cover that never settles, which bounds a stuck wait to about
   80 reads. A frozen count in state 4 backs off as before.
3. **State 3 in an ordinary status reply is confirmed** (issue #230); the experimental note of ADR 0079
   point 2 ends.

## Consequences

- A command to a VELUX SML no longer flashes a waiting issue for about 3 s.
- A queued shutter is read every few seconds while it waits, so its move is seen as it happens.
- A cover that waits for the whole window costs more reads than before; the window bounds them.

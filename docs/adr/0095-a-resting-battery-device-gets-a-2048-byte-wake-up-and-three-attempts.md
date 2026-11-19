# ADR 0095: A resting battery device gets a 2048-byte wake-up and three attempts

<!-- doxygen-label: adr0095 -->

**Status:** Accepted, amends [ADR 0040](0040-low-power-start-preamble-follows-the-wake-belief.md), [ADR 0013](0013-blocking-exchange-on-the-esphome-loop.md), [ADR 0073](0073-a-read-plans-its-attempts-and-an-attempt-must-fit-the-budget-with-its-windows.md), [ADR 0079](0079-a-cover-is-followed-by-its-execution-state.md), [ADR 0080](0080-linked-remote-activity-is-followed-by-an-opt-in-catch-up-read.md) and [ADR 0089](0089-the-exchange-engine-plans-every-exchange-from-the-callers-request.md); amended by [ADR 0097](0097-every-preamble-is-set-as-a-duration-the-same-on-every-radio.md) (the wake-up is 600 ms) · **Recorded:** 2026-11

## Context

A battery-powered receiver at rest hears a directed start frame only behind a long wake-up preamble
([ADR 0040](0040-low-power-start-preamble-follows-the-wake-belief.md)). The default,
`low_power_wake_preamble: 1024` (213 ms on air), is answered on about half the tries or fewer by a resting
VELUX SSL solar roller shutter or screen:

- [issue #114](https://github.com/laberning/home_io_control/issues/114) (PR #148): 8 of 17 tries at rest;
- [issue #209](https://github.com/laberning/home_io_control/issues/209): 5 of 18 and 1 of 6 tries at rest, and
  on a later run 12 of 25.

Longer wake-ups did better for the reporters who tried them:

- issue #114 (PR #148 thread): 1536 bytes made four open and close cycles reliable, and with 2048 bytes the
  shutters "have always woken up and responded to commands, regardless of how long they had been sleeping";
- [issue #151](https://github.com/laberning/home_io_control/issues/151): 12 of 16 tries answered at rest at
  2048 bytes, and every exchange completed.

No reporter compared both lengths on the same device on the same day, so the numbers show a direction, not
an exact rate.

An exchange has a time budget, `exchange_total_budget_ms` (2500 ms), so it does not block the ESPHome loop
past ESPHome's 2550 ms warning ([ADR 0013](0013-blocking-exchange-on-the-esphome-loop.md)); an attempt
starts only if it fits with its windows
([ADR 0073](0073-a-read-plans-its-attempts-and-an-attempt-must-fit-the-budget-with-its-windows.md)). At
2048 bytes a low-power attempt with its 550 ms window takes about 1 s, so only two fit: a reporter who set
2048 bytes got two attempts instead of three without being told.

The catch-up read after linked-remote activity
([ADR 0080](0080-linked-remote-activity-is-followed-by-an-opt-in-catch-up-read.md)) is one exchange with no
retry. In issue #209, 2 of 16 catch-up reads to resting screens went unanswered on every try, about what a
rate near one half per try predicts.

## Decision

- **The default wake-up is 2048 bytes** (`low_power_wake_preamble`: 427 ms on air on the SX1262 and LR1121, 533 ms on the SX1276, whose packet coder sends each preamble byte as a 10-bit cell; measured on our bench). The range of the key
  stays 1024 to 4096. The one-way burst is not part of this record and keeps its own wake-up.
- **A low-power target keeps its three wake-up attempts.** An exchange that opens with a wake-up to a
  low-power target is bounded by its three attempts, not by `exchange_total_budget_ms`: all three run,
  whatever a busy channel or timing jitter added to the earlier ones (about 3.3 s at the default for a
  target that stays silent, up to about 4.6 s for one that challenges each attempt late and never sends
  its final reply). ESPHome's warning that an operation took long is accepted in that case, until the
  exchange no longer blocks the loop. Every other plan keeps the budget rule of ADR 0073. The short lead
  of a read to a cover in an unknown state counts as one of these attempts, so it needs no budget
  allowance of its own (it replaces point 6 of
  [ADR 0079](0079-a-cover-is-followed-by-its-execution-state.md)), and no plan carries such an allowance
  any more (the example in point 3 of
  [ADR 0089](0089-the-exchange-engine-plans-every-exchange-from-the-callers-request.md)).
- **An unanswered catch-up read is retried once**, after the same delay the catch-up itself waits. A second
  silence ends it.

## Consequences

- A resting battery device is reached more often, for commands, settling reads and catch-up reads.
- An exchange to a battery device that does not answer blocks the loop for up to about 3.3 s instead of 2.5 s
  (up to about 4.6 s when it challenges every attempt and never closes the exchange), and ESPHome may log
  a warning about it. Users with only mains devices see no change.
- Each wake-up uses twice the air time. Every wake-up to a resting device costs about 12 % of the hour the
  EU duty cycle allows on the request channel; the hub does not enforce that limit today.
- The low-power discovery with the pairing frames after it
  ([ADR 0078](0078-discovery-follows-the-start-preamble.md)) and the low-power pass of the roll-call take the
  same setting, so their wake-ups are 2048 bytes too; a roll-call of both power classes puts
  about 1.3 s on air instead of 0.7 s.
- A user who prefers the old behaviour sets `low_power_wake_preamble: 1024`.

## Considered options

- **Keep 1024 bytes.** Rejected: the reporters' numbers favour a longer wake-up.
- **A longer default, up to the 4096-byte ceiling.** Not chosen: no report has tried more than 2048 bytes,
  and every step doubles the air time and the time the hub is deaf again.
- **Keep the budget and accept two attempts at 2048 bytes.** Rejected: a third try is what turns a rate near
  three quarters per try into an exchange that almost always completes.
- **Make the exchange non-blocking first.** Left for later: it removes the warning but is a redesign of the
  exchange's wait loop.

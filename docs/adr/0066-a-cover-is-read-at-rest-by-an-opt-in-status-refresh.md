# ADR 0066: A cover is read at rest by an opt-in status refresh

<!-- doxygen-label: adr0066 -->

**Status:** Accepted · **Recorded:** 2026-11

## Context

The hub learns a cover's state from its own commands and their follow-up polls (ADR 0061), from
status updates (`0x71`) the device sends by itself, and from another controller's exchanges it
overhears, after which it reads the device. `status_poll_interval` only sets the cadence of the
follow-up polls; nothing reads a cover at rest.

So a change the hub hears nothing of stays unseen until the next command. The radio is half duplex
and listens on one channel at a time, so another controller's whole exchange with the device can
fall into a moment the hub is transmitting or listening elsewhere. A VELUX roof window opened from
a KLR 200 pad showed this: the hub logged no frame of the exchange, and the cover kept showing
closed while the window stood open, until the next command. A rain sensor that closes a window is
the same case without any second controller.

ADR 0049 rejected a timer that polls every window, and ADR 0050 made the rain read opt-in for the
same reason: a default cost on every install, for a state most installs do not have.

## Options considered

- **Poll every cover always.** Rejected for the reason in ADR 0049 and ADR 0050: transmit time on
  a shared band for every install.
- **Keep the follow-up window open at a slow cadence after a move.** Rejected: the window is
  bounded on purpose (ADR 0061), and a change that comes long after the last move would still be
  missed.
- **A Home Assistant automation that requests a status.** Rejected for the reason in ADR 0050: the
  schedule would live outside the hub, which knows when a device last reported.
- **A schedule class of its own.** Rejected for the reason in ADR 0064: `PeriodicPollPolicy`
  already is the periodic schedule; the refresh only needs it to restart on a report.
- **Read a cover once it has reported nothing for an opt-in per-cover interval** (chosen).

## Decision

- A cover that declares `status_refresh_interval` (minimum one minute, no default) is read with the
  ordinary status read once it has reported no status for that interval. A cover without the key
  behaves as before. The key creates no entity.
- Every status the device reports to the hub moves its next refresh one interval out: the reply to
  any of the hub's status reads (a follow-up poll, a read after another controller's command, a
  refresh) and a `0x71` status update. The reply to the hub's own execute does not count, because
  its position bytes can echo the state from before the command; the follow-up polls after it do.
- The schedule is one more `PeriodicPollPolicy` instance next to the sensor polls' (ADR 0064),
  with their random offset of up to 10 % of the interval, so covers that reported together do not
  stay in step. A report calls its new `restart_interval()`. The first refresh is due 15 to 45
  seconds after boot like a sensor poll's, unless the cover's boot status read is answered first
  and moves it out.
- A due refresh queues the ordinary status read. It is a background operation: commands and 1W
  activity go first, and pairing flushes it. When it is popped, the next refresh is scheduled one
  interval later, so a read that gets no answer, or that pairing drops, is not retried on every
  loop. The refresh uses neither the sensor polls' miss retry nor their miss counter: there is no
  reading to turn unknown, and the status read has tries of its own.

## Consequences

- **Transmit time is the user's choice and is not enforced**, as for the rain poll. A refresh costs
  about what a rain poll costs at the same interval; the documentation points to that table and
  advises an hour or more on a `low_power` device, whose battery each read draws on.
- **A change shows up within the interval** (plus the offset), not at once. Covers that are
  commanded or read more often than the interval are never refreshed on top of that.
- **A read that gets no answer uses the full retry budget** of an ordinary status read (three
  tries), not the rain poll's two, because the refresh reuses that read unchanged. It counts in the
  device's Exchange Failures, and a reply that reads as moving opens the follow-up window like any
  other status report (ADR 0061).
- **Covers only.** Other platforms keep their behaviour: a heater already has its own cadence, and
  the other kinds have no field report of a missed change yet.

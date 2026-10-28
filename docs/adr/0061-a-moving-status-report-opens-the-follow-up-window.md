# ADR 0061: A moving status report opens the follow-up window

<!-- doxygen-label: adr0061 -->

**Status:** Accepted · **Recorded:** 2026-10

## Context

After a move the hub polls the device until it reports stopped, inside a bounded window of 10
minutes. That window opened only when the hub sent a command or overheard another controller's. A
status report that read as moving outside a window, such as the read shortly after start-up, a
manual read or a status update the device sent, was not followed: the cover kept showing opening or
closing until the next command, with nothing polling it.
[PR #158](https://github.com/laberning/home_io_control/pull/158) found this on a bioclimatic
pergola. Its main fix, a stopped report that comes to rest short of its target, is decided
separately (a second agreeing report confirms it). Its second part asked for the case here: a status
read as moving with no poll planned, such as the poll after boot or a status update after a move by
another controller, should start the follow-up polling.

A second gap sat next to it. A reply that arrived after the window had expired cleared the window
without recording that it ran out, so the cover was not shown at rest and stayed moving.

## Options considered

- **PR #158's variant**: open a window for a moving report unless an expired one is still on
  record, and clear that expired one. Its core, a moving report opening the window, is the decision
  below. Clearing the expired window without recording that it ran out leaves the second gap open:
  the cover is not shown at rest.
- **A "spent" flag** that blocks reopening a window until a stopped report is seen. Rejected: it adds
  state per device, and a cover moved again by a remote the hub did not hear would not be followed,
  because the flag would still be set.
- **Open a window for a stopped report that is off target.** Rejected: a stopped answer is final,
  and the confirmation rule for a stopped report off target already decides it without a window.
- **Sweep expired windows when a poll is popped.** Rejected: a popped poll waits in the operation
  queue with no next poll time set, so the sweep could close its window before the reply, and the
  reply would then open a new one. That is the hub's own follow-up reopening a window, the one way
  windows could chain.

## Decision

1. **The report opens the window.** `StatusPollPolicy::on_moving_report()` decides, for a status
   reply or a status update that reads as moving, between four outcomes: follow an open window,
   open one, end an expired one, or do nothing.
2. **An open window is followed and its deadline stays.** A later report never extends it.
3. **A report that finds the window expired ends it.** The window is closed and reported ended, so
   the cover is shown at rest at its last reported position, and no further poll is scheduled. The
   hub's own follow-up poll therefore never reopens a window.
4. **A fresh read may open a new one.** Any moving report arriving with no window, from a read the
   hub did not schedule as a follow-up or from a frame the device sent, starts a window of the
   usual length.
5. **Only kinds that report motion through status may open a window.** The gate is
   `known_device_reports_motion_through_status()`, the one the at-rest fallback uses. It restricts
   opening only: a window that is already open is followed for every kind, as before.
6. **Stopped reports open nothing.**

## Consequences

- A cover found moving by a read is followed until it reports stopped or the window runs out.
- A device that keeps answering "moving" costs at most one window of polls per read that is not the
  hub's own follow-up, and is shown at rest when the window ends.
- A window with no next poll time can keep an expired deadline until the next report. That report
  then ends it instead of opening a new one, so that one read shows the cover at rest; the next
  moving read opens a window. This errs toward fewer polls, never endless ones.
- A device that keeps sending moving status updates after a window ended opens a new window for
  each. Each update is authenticated and sent by the device, and a window costs at most one poll per
  settle delay, so this is accepted until it is seen in the field.

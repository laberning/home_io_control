# ADR 0080: Linked-remote activity is followed by an opt-in catch-up read

<!-- doxygen-label: adr0080 -->

**Status:** Accepted · **Recorded:** 2026-11

Amends [ADR 0079](0079-a-cover-is-followed-by-its-execution-state.md) (point 5: which status reads lead a
low-power cover with the short attempt).

## Context

A press on a remote in a device's `linked_remotes` schedules a status poll of the devices it
controls, `REMOTE_ACTIVITY_STATUS_POLL_DELAY_MS` (2 s) after the press, and the follow-up window
then polls every few seconds while the device moves (ADR 0061). `ONEWAY_QUIET_PERIOD_MS` holds those
polls back while a remote is still transmitting.

That hold protects only a burst the hub has already started to hear. The radio is half duplex, so
a press that begins while the hub transmits, for example during a 1024-byte wake-up preamble, is
never heard. People press a remote several times, or press two remotes one after another, within
seconds, which is exactly when the follow-up polls run. Issue #209 logged it: CLOSE pressed on one
VELUX solar screen's remote during the polls after a press on the other's; the screen closed, the
hub heard nothing, and it kept showing the screen open. Nothing reads it again until the next heard
activity.

`status_refresh_interval` (ADR 0066) corrects such a state within its interval, but on a solar
`low_power` screen each read wakes the motor, so it is meant to be an hour or more there.

A read sent to catch such a press meets a screen that may be at rest or may be moving. A resting
VELUX solar screen hears only the wake-up preamble, and a moving one ignored it in issue #209 (no
answer to 12 reads at 1024 bytes while it moved, and one answer in a later run). A screen polled
during the burst has just reported "stopped", so by ADR 0079 point 5 the read would believe it at
rest and send only wake-up attempts.

## Considered options

- **Only `status_refresh_interval`.** Rejected as the only answer for `low_power` covers: it reads
  every interval whether or not anyone touched a remote, and corrects the state an interval late.
- **Read only the devices of the remotes the hub heard.** Rejected: the missed press can be on a
  remote the hub never heard at all (issue #209 shows the device re-read only because of an
  unexplained frame of its remote).
- **Start each device's follow-up window with its catch-up read**, so an unanswered read is retried
  on the backoff ladder. Rejected: an answer that says moving already opens the follow-up window
  (ADR 0079), so the window is needed only when the read goes unanswered, and the ladder then polls
  every linked device for minutes after every burst. The read's own attempts are its retry, as for
  any one-off read (ADR 0074).
- **Read every linked device once after a burst of remote activity, with the receiver's state
  treated as unknown** (chosen), plus a configurable delay before the first poll after a press.

## Decision

- `tuning: remote_activity_poll_delay_ms` (default 2000, today's constant) is the delay between a
  linked-remote press and the first status poll of its devices. A STOP still polls at once. Polls
  armed by another two-way controller's overheard traffic keep the 2 s constant.
- `tuning: remote_activity_catchup_poll_ms` (default 0 = off) opens a burst at a linked-remote
  press. Every exchange that ends while the burst is open sets the catch-up due time to this many
  milliseconds after it. When it is due, the burst closes and every device with `linked_remotes`
  (by id or by `class:`) gets one ordinary status read. The burst closes before the reads are
  queued, so the catch-up's own exchanges do not reopen it. A new press during the catch-up opens a
  new burst.
- **A catch-up read treats the receiver's state as unknown** (amends ADR 0079 point 5). A press the
  hub missed may have set the device moving after its last report, so a recent status report does
  not make the read believe it at rest: to a `low_power` cover the read leads with the short attempt
  a moving receiver hears, then sends the wake-up attempts a resting one hears. Fresh moving
  evidence or a STOP still decides as before. The mark sits in `StatusPollPolicy` and is consumed
  by the device's next status read; heard activity or a report drops it.
- A catch-up read opens no follow-up window of its own. An answer that says moving opens one
  (ADR 0079), and the cover is then followed to rest.
- The schedule is pure logic in `StatusPollPolicy` with injected timestamps; `loop()` asks it for a
  due catch-up.

## Consequences

- **A missed press is corrected within the catch-up delay** after the burst's last exchange. A
  short delay, a few seconds, catches a cover while it still moves and then follows it, so Home
  Assistant shows the move as it happens; a delay longer than the travel time reads it once at rest.
- **Transmit cost is one read per linked device per burst** (on a `low_power` cover one short
  attempt and up to three wake-up attempts), nothing while no remote is touched. An unanswered
  read is not repeated until the next burst or the status refresh.
- **Defaults keep today's behaviour.** A press missed outside any heard burst is still not caught;
  that remains `status_refresh_interval`'s case.
- The short lead to a moving `low_power` receiver rests on VELUX screens answering short start
  frames while moving (issues #114 and #209); a catch-up read on hardware has not run yet.

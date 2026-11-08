# ADR 0069: Linked-remote activity is followed by an opt-in catch-up read

<!-- doxygen-label: adr0069 -->

**Status:** Accepted · **Recorded:** 2026-11

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

## Options considered

- **Only `status_refresh_interval`.** Rejected as the only answer for `low_power` covers: it reads
  every interval whether or not anyone touched a remote, and corrects the state an interval late.
- **Read only the devices of the remotes the hub heard.** Rejected: the missed press can be on a
  remote the hub never heard at all (issue #209 shows the device re-read only because of an
  unexplained frame of its remote).
- **Read every linked device once after a burst of remote activity** (chosen), plus a configurable
  delay before the first poll after a press.

## Decision

- `tuning: remote_activity_poll_delay_ms` (default 2000, today's constant) is the delay between a
  linked-remote press and the first status poll of its devices. A STOP still polls at once. Polls
  armed by another two-way controller's overheard traffic keep the 2 s constant.
- `tuning: remote_activity_catchup_poll_ms` (default 0 = off) opens a burst at a linked-remote
  press. Every exchange that ends while the burst is open sets the catch-up due time to this many
  milliseconds after it. When it is due, the burst closes and every device with `linked_remotes`
  (by id or by `class:`) gets one status read, queued like the follow-up polls.
- Each catch-up read starts the device's follow-up window (ADR 0061), so a read that gets no answer
  is retried on the background backoff ladder within that window. A screen that a missed press set
  moving ignores reads until it rests (issue #209 test), so a single untracked read could fail and
  leave the state stale again.
- The burst closes before the reads are queued, so the catch-up's own exchanges do not reopen it.
  A new press during the catch-up opens a new burst.
- The schedule is pure logic in `StatusPollPolicy` with injected timestamps; `loop()` asks it for a
  due catch-up.

## Consequences

- **A missed press is corrected within the catch-up delay** after the burst's last exchange, plus
  the device's travel time if the read finds it moving. Choosing a delay longer than the travel
  time (about 35 s on a VELUX solar screen) avoids the retries.
- **Transmit cost is one read per linked device per burst**, nothing while no remote is touched.
- **Defaults keep today's behaviour.** A press missed outside any heard burst is still not caught;
  that remains `status_refresh_interval`'s case.

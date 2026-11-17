# ADR 0096: The hub's requests take a free channel and are never forced onto a busy one

<!-- doxygen-label: adr0096 -->

**Status:** Accepted, amends [ADR 0045](0045-listen-before-talk-is-part-of-sending.md) · **Recorded:** 2026-11

## Context

Before a frame that opens a session, the hub checks whether the channel is free: up to
`lbt_max_retries` (5) checks, 5 ms apart, at `lbt_rssi_threshold_dbm` (-90 dBm), then one send
without a check ([ADR 0045](0045-listen-before-talk-is-part-of-sending.md)). Every two-way request
goes out on channel 2 (868.95 MHz).

In [issue #209](https://github.com/laberning/home_io_control/issues/209) the five checks read busy
within about 35 ms, the hub's wake-up poll went out anyway, and it covered a one-way remote's press:
the screen did not move (2 of 3 overlaps). The remote kept channel 2 busy for at least 0.45 s before
its first frame, so no number of 5 ms checks would have let it finish. In run 2 of the same report a
poll lost to such a collision was taken as "the screen is at rest".

io-homecontrol receivers scan all three channels (Somfy's patent EP 2 401 729 B1 describes the scan),
and controllers use more than channel 2: a VELUX KLF 200 polls its devices on all three channels,
868.25, 868.95 and 869.85 MHz (corpus `velux_klf200_statuspoll_roundrobin_868_sx1262`, issue #98; issue #87). A one-way
remote, and a request to a battery-powered receiver, use channel 2.

Answers inside a session stay unchecked: a delay of one 5 ms back-off already loses the exchange on
some receivers ([issue #178](https://github.com/laberning/home_io_control/issues/178), ADR 0045's
amendment).

The hub's received level is now read correctly on every chip; on the SX1276 its idle floor is about
-100 dBm, so -90 dBm leaves about 10 dB of margin on all boards.

## Decision

- **A request from the exchange engine never goes out onto a channel measured busy** unless the
  user opts in. A check takes up to five readings about 1 ms apart; one reading at or above
  `lbt_rssi_threshold_dbm` makes the channel busy.
- **An ordinary request takes a different channel on every attempt** (never the channel of the
  previous attempt, chosen at random), and on a busy result another free one. A request with the
  low-power flag stays on channel 2.
- **Between checks the hub waits a random, growing back-off** (a few to about 40 ms) while it keeps
  receiving, so a remote's press heard meanwhile is handled. The checks of one attempt end after
  about 100 ms, or about 600 ms for a low-power request, which outlasts a remote's wake-up on
  channel 2. These values are this project's design choice, sized from issue #209.
- **When the time is up the attempt ends as "channel busy"**: nothing is sent, the attempt counts
  in its plan, and it is no evidence about the receiver's state. The plan's next attempt follows.
- **The hub listens for the answer on the channel it sent on.**
- **`lbt_force_when_busy`** (default off) restores a send when every channel stays busy: the hub
  then measures all three and sends on the quietest; a low-power request is sent on channel 2.
  `lbt_max_retries` no longer has an effect and is deprecated.
- **Unchanged:** answers inside a session (no check, at once), one-way bursts (channel 2), and,
  for now, discovery, pairing, the key pull and broadcasts (channel 2, five checks 5 ms apart, then
  a send). Their count of checks is now fixed, since `lbt_max_retries` has no effect.

## Consequences

- A remote's press is no longer covered by the hub's own poll; the hub moves to another channel or
  waits until the remote is done.
- Requests spread over three channels, and channels 1 and 3 have larger duty-cycle allowances than
  channel 2.
- An exchange to a battery device can block the loop for up to about 5 s when channel 2 stays
  nearly busy, until exchanges no longer block the loop.
- A command can now fail with "channel busy" where it used to be sent into a busy channel.

## Considered options

- **Keep sending after five checks.** Rejected: issue #209.
- **Wait longer on channel 2 only.** Rejected for ordinary requests: changing channel avoids the
  remote at once, and receivers scan all three.
- **Keep `lbt_max_retries` as the count of checks.** Rejected: the time limit ends the checks, and
  a forced send is a different choice, made explicit by its own key.

# ADR 0128: A one-way light shows an estimated level, and a slider value is a timed stop or a position frame

<!-- doxygen-label: adr0128 -->

**Status:** Accepted · **Recorded:** 2026-11

A sibling of [ADR 0117](0117-a-one-way-controller-can-show-an-assumed-state-cover.md) for lights; uses the estimate
of [ADR 0127](0127-a-one-way-cover-shows-a-position-from-its-travel-time.md) and decides the slider for both.

## Context

A one-way identity of a light kind gives Home Assistant buttons only. A dimmer does not jump to its level when told
on or off: it fades, so a level shown at once would be wrong for seconds. One-way has no "set level" that every
device is known to obey; it has open (on), close (off) and stop, which every enrolled one-way receiver on record
obeys, and a position frame that is unconfirmed on every cover.

A bench run on a Somfy Izymo dimmer (mains), with each step read back over two-way, measured: the dimmer fades
linearly, about 4.7 s from off to full and the same back; a one-way stop during the fade freezes the level, four of
four within 1 % of the level a linear fade predicts for the moment the stop went out; and a one-way position frame
is obeyed, with the level reached at once (already there and at rest at the first read, under a second later).

## Decision

- **An option of the one-way identity.** An entry of a light kind, decided from the kind's capabilities, may ask for a
  light entity (`light:` with a name), binary, or with a brightness slider under `dimmable: true`. It is an
  assumed-state companion like ADR 0117's cover and takes the same inputs: the identity's own commands that reached
  the radio and the presses of the remotes under `follows:`. `light:` and `cover:` exclude each other. Nothing is
  persisted.
- **An estimated level.** With `on_duration:` and `off_duration:` the shown brightness follows the estimate of ADR
  0127, with on as open and off as close: it moves over the fade time, ends at the commanded end and freezes on a
  stop. A position frame sets the light's estimate to its level at once, as the Izymo does; a cover's estimate keeps
  travelling (ADR 0127). Home Assistant transitions stay off (length 0): the fade shown is the device's own, and
  a transition asked for anyway sends only its end value.
- **A slider value is one of two ways**, chosen by `set_position:` on the identity, valid only with the durations, the
  same for a light and a cover:
  - `timed` (the default): the hub sends on or open towards the value, or off or close, and one stop when the
    estimate reaches the value. The stop is scheduled from the moment the command's report says it reached the radio,
    so the delay starts when the fade or the motor does. A value at an end sends the plain command and no stop, so the
    end stop re-anchors the estimate.
  - `frame`: one position frame carrying the value, mirrored for an inverted kind as `oneway_set_position` does, and
    no stop.
- **No stale stop.** A new value, a stop, or a followed remote's press cancels a pending stop; a new value is planned
  again from the current estimate, and a reversal sends the opposite command.
- **One rule.** The choice of command and the stop delay are one pure function of estimate, requested value,
  durations and way, shared by the light and the cover.

## Considered options

- **The position frame as the default.** Rejected: no cover on record confirms it, while the timed way needs only on,
  off and stop. It stays the opt-in for a device known to obey it.
- **Only a binary light.** Rejected: the timed stop works on the Izymo, so a slider costs one scheduled stop.
- **Fade in ESPHome.** Rejected: the device fades on its own; an ESPHome transition would send many commands.

## Consequences

- A one-way dimmer appears as one light with a slider and a level that follows the wall remote.
- A timed stop goes out as a burst, so the level or position lands slightly after the computed moment; a stop the
  device misses runs it to the end. The estimate carries both errors until the next run to an end.
- A device that ignores a position frame does nothing with `set_position: frame`; the docs state per device what the
  bench showed.

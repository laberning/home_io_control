# ADR 0119: Somfy one-way bursts lead with one wake-up copy, and no preamble is set in bytes

<!-- doxygen-label: adr0119 -->

**Status:** Accepted, supersedes the Somfy and unknown-manufacturer rows of
[ADR 0041](0041-unset-oneway-power-class-comes-from-the-manufacturer-profile.md) and amends
[ADR 0038](0038-oneway-bursts-follow-the-identity-power-class.md) and
[ADR 0115](0115-a-low-power-one-way-wake-up-lasts-the-hubs-wake-up-duration.md) · **Recorded:** 2026-11

## Context

A one-way identity without `low_power:` takes its burst shape from its manufacturer's profile (ADR 0041). For VELUX
that is four short copies. For Somfy and every manufacturer without a profile it was the long shape: the wake-up
preamble of 1024 programmed bytes in front of all four copies, the low-power flag clear on each. ADR 0041 kept it
only until a run on Somfy hardware showed whether a shorter shape works; its reason, missed short start frames on a
Somfy awning, predates the 10 ms start preamble (ADR 0042, ADR 0097).

The long shape keeps the transmitter on for about 0.88 s per command (1.1 s on an SX1276 board) and holds the radio
for about 1.0 s (1.2 s), on a channel where the EU rules allow 3.6 s of transmission an hour, and it pairs a long
wake-up with a clear low-power flag, which ADR 0029 removed for two-way. Its
1024 bytes are also the last preamble set in bytes, so it lasts 213 ms on an SX1262 or LR1121 board and 267 ms on an
SX1276 board.

A bench run sent 50 one-way commands of each shape to an enrolled Somfy Izymo dimmer (mains) from an SX1276, two
SX1262 and an LR1121 board, alternating on and off, each checked by a two-way read of the device's state and of the
controller that last commanded it. Every shape was obeyed 50 of 50 times on every board: the long shape, one 515 ms
wake-up copy followed by three short copies (the `low_power: true` shape), and four short copies. The hub's own
bursts of each shape, with the 1024-byte wake-up of the time, are in the corpus as captured on the air
(`tests/corpus/captures/oneway/unenrolled_window_opener_oneway_stop_*_burst_*.yaml`). A Somfy Smoove remote, also captured on the air,
sends every frame four times back to back with a 13.2 ms preamble and no wake-up copy and no low-power flag at all
(`tests/corpus/captures/oneway/somfy_smoove_oneway_short_press_on_air.yaml`): a mains Somfy receiver needs no wake-up,
and the one kept in the default is for a battery receiver that cannot be tested here.

## Decision

- **Somfy and unknown manufacturers default to the `low_power: true` shape:** one wake-up copy of
  `low_power_wake_ms` with the low-power flag, then three copies at the start preamble. VELUX keeps four short copies.
  An explicit `low_power:` still decides.
- **The long-on-every-copy shape is removed.** No device on record needs it. The boot log names the shape an identity
  gets and where it came from, so a user whose default changed sees it.
- **No preamble is set in bytes.** The one-way copies are durations converted per radio like every other preamble.
  Whether a preamble is a wake-up is decided from its duration: a chip-neutral threshold of 300 ms in the protocol
  timing header, compared after the driver's own conversion where a transmission is stamped. 300 ms is the floor of
  `low_power_wake_ms` and lies above the ceiling of every other preamble setting except the discovery preamble, whose
  ceiling is the same 300 ms; a discovery preamble set to its maximum counts as a wake-up.
- **No new key** pins the old shape. ADR 0041's sentence that a VELUX user "can still force `LEGACY_LONG` back" never
  matched the schema and no longer applies.

## Consequences

- A Somfy or unknown-manufacturer command keeps the transmitter on for about 0.58 s instead of about 0.88 s (1.1 s on
  an SX1276) and holds the radio for about 0.72 s instead of about 1.0 s (1.2 s). About six such commands fit in the
  3.6 s an hour of the one-way channel, where the long shape allowed four (three on an SX1276).
- The flag and the long preamble agree on every one-way copy, as on two-way frames.
- A one-way command lasts the same on every board.
- A battery-powered Somfy device was not on the bench; the default still leads with the wake-up such a device needs,
  and `low_power_wake_ms` stays the remedy if one misses commands.
- A configuration with `low_power:` unset for Somfy changes on the air after an update; nothing else changes.

# ADR 0087: The SX1262 receives at 117.3 kHz by default

<!-- doxygen-label: adr0087 -->

**Status:** Accepted · **Recorded:** 2026-11

## Context

The SX1262 receives IO-Homecontrol's GFSK signal (38.4 kbps, 19.2 kHz deviation) through a channel
filter set by `sx1262_rx_bandwidth`, and it has no automatic frequency correction in this mode. Semtech
sizes such a filter as bitrate + 2 × deviation + carrier offset, 76.8 kHz before any offset; real io
transmitters occupy about 77 kHz (99 % occupied bandwidth measured in issue #65). The default was
58.6 kHz, below that figure, chosen in 2026-08 after a sweep in which a Somfy RS100 solar shutter
answered more reliably at 58.6 than at 117.3 kHz. That sweep changed several other things at the same
time and recorded no per-value counts.

Since then the field and the bench point the other way:

- A Somfy White LED Receiver io needed 156.2 kHz (issues #119, #143: 4 of 11 commands confirmed at
  117.3, 9 of 11 at 156.2).
- A bioclimatic pergola failed 15 of 60 polls at 58.6 and 1 of 60 at 156.2 (PR #157).
- A hand-wired SX1262 module with a plain crystal heard nothing at 58.6 and worked at once at 156.2
  (issue #177).
- On our bench, two SX1262 boards receiving a peer that transmits about 22 kHz below nominal lost
  3.5 to 5.5 % of its frames to single-bit decode errors at 58.6 kHz, about 0.5 % at 78.2 kHz and none
  at 117.3 kHz (50 bursts of 4 frames per value). From an on-frequency peer every value decoded every
  frame.

The one report that favoured 58.6, on a single VELUX shutter (issue #87), found no setting helped and 58.6
the best of 39.0, 58.6 and 156.2; it did not try 117.3. The LR1121, which uses the same Semtech filter grid, has received at 117.3 kHz
by default since its bring-up without a report of trouble.

## Decision

1. **The SX1262 default is 117.3 kHz**, the same as the LR1121's. It clears the 76.8 kHz figure with
   about 20 kHz of margin for a transmitter that sits off nominal.
2. **The setting stays.** A device whose transmitter sits further off (the White LED Receiver io) still
   needs `sx1262_rx_bandwidth: 156.2`, and a noisy install may still try narrower values.
3. The SX1276 default (41.7 kHz single-sideband, 83.4 kHz in total, with automatic frequency
   correction) is unchanged.

## Consequences

- An SX1262 hub without the setting receives more of an off-frequency device's frames; an
  on-frequency device behaves as before.
- A wider filter lets in more noise. On our bench at 117.3 kHz one or two noise bursts a minute
  reached the decoder and were dropped because they did not parse as a frame, where 58.6 kHz showed
  none; commands and status reads were answered as before. An install that worked better at 58.6 kHz sets
  `sx1262_rx_bandwidth: 58.6`.
- Both soft-PHY chips now share one default, so a configuration moved from an SX1262 board to an
  LR1121 board keeps its receiver width.

## Considered options

- **78.2 kHz.** Just above the sizing figure and nearly as good on the bench, but it leaves no margin
  for a carrier offset, which is the case the field reports are about.
- **156.2 kHz.** What the worst field cases needed, but twice the signal's width; no report shows the
  ordinary devices need it.
- **Keep 58.6 kHz.** Rests on one unquantified observation and is below the sizing figure.

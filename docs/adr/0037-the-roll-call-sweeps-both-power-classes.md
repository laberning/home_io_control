# ADR 0037: The roll-call sweeps both power classes
<!-- doxygen-label: adr0037 -->

**Status:** Accepted · **Recorded:** 2026-09

## Context

`scan_paired_devices()`'s roll-call has always had one frame shape: `CTRL1 = 0x00`, broadcast on
each of the three channels in turn. ADR 0029 chose that shape deliberately — a real always-alive
VELUX installation only started answering directed commands once the 1024-byte start preamble was
dropped to the short, runtime-tunable `normal_start_preamble`, and the roll-call inherited the same
fix.

A field report captured a real VELUX KLR300's own roll-call traffic. Its `0x2A` carries
`CTRL1 = 0x30` (`CTRL1_LOW_POWER | CTRL1_ACK`) and a long wake-up preamble, and the two low-power
roller shutters on that installation answered it within 183 ms and 798 ms of the request. The same
field report also states that our own `CTRL1 = 0x00` roll-call, sent to the same installation,
drew nothing back from those shutters. This is consistent with the long-preamble wake-up call
being necessary for a duty-cycled receiver to hear a roll-call at all, but it is one installation's
data, gathered by an observer listening on a single channel, not a controlled bisection. Mixed
installations — mains-powered windows alongside solar or battery shutters — are the VELUX norm, not
an edge case, so a roll-call that only reaches one power class is incomplete by design for a large
share of real installs.

## Options considered

- **Low-power-only roll-call** (always `CTRL1 = 0x30` and the long preamble). Rejected: this
  re-breaks the always-alive VELUX installation ADR 0029 already fixed — the exact regression that
  ADR closed.
- **A tuning switch only, defaulting to today's behaviour.** Rejected as the sole mechanism: a user
  cannot know their installation's power-class mix before scanning, and discovering it is the
  scan's job. Kept as an override, not the primary path.
- **Raise `normal_start_preamble` globally** to a value long enough for both classes. Rejected: the
  same knob also lengthens every directed always-alive start frame, and no evidence bounds the
  always-alive tolerance ceiling.
- **A single compromise preamble length for both classes.** Rejected for now, kept as a future
  optimisation: no evidence exists for either class's actual threshold, and finding one needs its
  own bisection.
- **Only run the low-power pass when the registry already has a `low_power: true` device.**
  Rejected: the roll-call exists to find devices that are not in the registry yet.
- **Interleave the two frame shapes per channel** instead of running each pass to completion.
  Rejected: no evidence this improves the found rate, and it breaks the always-alive pass's
  byte-identical continuity with the roll-call this project shipped before.
- **Run the always-alive pass first, low-power second.** Rejected: a low-power reply arriving after
  the last low-power window would fall off the end of the scan with nothing left to catch it,
  where running the low-power pass first lets a late reply still land inside the trailing
  always-alive windows instead.

## Decision

`scan_paired_devices()` runs two passes, low-power first, six attempts total:

| Pass | TX channels | CTRL1 | Start preamble | Listen policy |
|---|---|---|---|---|
| Low-power | CH2, CH1, CH3 | `CTRL1_LOW_POWER \| CTRL1_ACK` (`0x30`) | `LONG_PREAMBLE`, via `request_preamble_for_()`'s existing rule | `ROTATE_ALL_CHANNELS` |
| Always-alive | CH2, CH1, CH3 | `0x00` | `normal_start_preamble` | `ROTATE_SKIPPING_REQUEST` |

- The low-power pass's `CTRL1 = 0x30` matches the captured KLR300 header exactly, rather than
  inventing an untested variant. `request_preamble_for_()` is unchanged: setting `CTRL1_LOW_POWER`
  on the request still selects `LONG_PREAMBLE` by the same rule ADR 0029 established for directed
  frames, so the flag and the preamble can never disagree. For a directed frame the per-device
  `low_power` YAML property sets the flag; for the roll-call, the pass being sent sets it.
- The low-power pass listens on all three channels, including the one it transmitted on — the
  captured low-power replies were observed landing there. `collect_broadcast_responses()` gained a
  `ListenPolicy` parameter for this; every existing caller keeps the previous default
  (`ROTATE_SKIPPING_REQUEST`) unchanged.
- The always-alive pass is unchanged in every respect — frame shape, preamble, channel order,
  listen policy, window — from the roll-call this project shipped before this change.
- Responders are deduplicated by node ID across all six attempts, so a device answering both
  passes is still reported once, and a low-power reply caught only by a later always-alive window
  is still reported with its own self-described power class.
- `pairing_discovery_ack_capable` has no effect on either pass; both frame shapes are fixed. That
  tunable belongs to Discover & Pair, a different scenario (never-enrolled devices) from the
  roll-call's already-enrolled targets.
- A `scan_power_classes` tuning select (`both` default, `always_alive`, `low_power`) narrows the
  sweep to one pass — for bisecting an installation in the field, or for restoring the shorter
  single-pass scan on an install with no solar/battery devices. It lives in the existing opt-in
  `tuning:` block; no new action argument or hub YAML key.
- A known device whose registered `low_power` YAML property disagrees with its self-reported
  power-save class gets an advisory `hint:` line in the report. This never changes the registry or
  the device's runtime `low_power` value — ADR 0029's explicit-override model stands.

## Consequences

- **Loop blocking roughly doubles**, from ~6.1 s to ~12.7 s (six 2000 ms-default windows plus
  three ~213 ms long-preamble transmits). ESPHome's "operation took a long time" warning already
  fired on every invocation before this change (its ratchet saturates at 2550 ms), so this adds no
  new warning class.
- **Airtime per scan rises** from roughly 0.03 s to roughly 0.7 s, dominated by the three long
  preambles. Still a manually triggered action, not a concern unless scans are automated in a tight
  loop.
- **A reply arriving after the sixth attempt is still lost** to the same passive-path fallback as
  before (`hub_status.cpp`'s `unhandled_cmd` catch-all) — unchanged from the single-pass scan.
- **Whether `CTRL1_ACK` on a broadcast is safe for Somfy hardware is unmeasured.** `CTRL1_ACK` on a
  *directed* frame has silenced a Somfy awning in the past; whether the same holds for a broadcast
  roll-call has not been tested. If it does misbehave, the low-power pass's CTRL1 drops to `0x20`
  (LOW_POWER without ACK) — a one-constant change, needing its own VELUX low-power retest.
- **The low-power pass's positive path is field-verified only where testers have low-power
  devices.** The always-alive pass is unchanged and carries forward its existing validation.
- **1W transmissions are out of scope.** ADR 0038 governs 1W burst power-class behaviour
  separately; nothing here changes it.
- **A per-pass, CH2-only low-power optimisation is deferred.** The per-reply DEBUG log this change
  adds (source, pass, attempt, TX/RX channel, latency, RSSI, self-reported power class) is what a
  future decision to narrow the low-power pass to fewer channels would need; it does not exist yet.
- **`ROTATE_ALL_CHANNELS` had no production caller before this change.** It is unit-tested but had
  no real-world traffic through it; the low-power pass is its first production use.

See also [ADR 0029](0029-start-preamble-is-a-property-of-the-target.md) (the per-device preamble
rule the low-power pass reuses unchanged) and
[ADR 0028](0028-channel-policy-is-a-property-of-the-frame-not-of-the-chip.md) (the channel-policy
model this extends to a second broadcast frame shape).

# ADR 0078: Discovery follows the start preamble

<!-- doxygen-label: adr0078 -->

**Status:** Accepted · **Recorded:** 2026-11

Amends [ADR 0029](0029-start-preamble-is-a-property-of-the-target.md) (the pairing paragraph of its
consequences, which named a 1024-byte discovery default) and builds on
[ADR 0042](0042-start-preamble-default-comes-from-the-radio-driver.md).

## Context

Discover & Pair sent its discovery broadcast (`0x28`, and `0x2E` when configured) behind a
1024-byte wake-up burst (about 213 ms), with `pairing_discovery_preamble` as the way down. The
pairing frames that follow a discovery never use a longer preamble than the discovery itself
(ADR 0029), so the long default made every pairing start frame long as well.

Several devices do not answer that. A VELUX SSL solar roller shutter answers discovery only behind
a short preamble and ignores the directed pairing frames behind the long one (issues #112 and #114;
corpus `velux_ssl_pairing_directed_long_preamble_ignored`). A VELUX SML on a KUX 110 failed to pair
at the defaults (issue #17) and paired two-way at 32 bytes (issue #230). Users of both had to add a
tuning entry before pairing worked.

Nothing in the public record shows a controller sending a plain discovery behind a wake-up burst.
Somfy's patent EP 2 401 729 B1 describes a session start as 52 preamble bytes and two sync bytes,
about 14 ms at 38.4 kb/s, and SDR measurements of devices' own discovery replies show the same
length (issue #65). The long form belongs to a first frame that carries the low-power flag, which
addresses a receiver that sleeps between short listening windows.

## Decision

1. **Discovery has no length of its own.** Unless the user sets `pairing_discovery_preamble`, the
   discovery broadcast goes out behind the radio's normal start preamble (`normal_start_preamble`,
   whose default comes from the driver per ADR 0042: 32 bytes on SX1276, 48 bytes on SX1262 and
   LR1121). One value covers every first frame without the low-power flag, so a later change of
   that length moves discovery with it.
2. **The low-power flag takes the wake-up preamble.** With `pairing_discovery_low_power: true` and
   no `pairing_discovery_preamble`, discovery goes out behind `low_power_wake_preamble`, as every
   other first frame with that flag does (ADR 0029).
3. **A set value wins.** `pairing_discovery_preamble` (8 to 1024 bytes, YAML or the Home Assistant
   number) overrides both, 1024 included, so the wake-up burst stays one line away for a device
   that needs it. A value written through Home Assistant stays set until the next restart.
4. **The pairing cap is unchanged.** The directed pairing frames still never exceed the discovery
   length (`PairingEngine::pairing_start_preamble_()`), so at the new default they go out at the
   normal start preamble too.
5. **One accessor decides.** `effective_discovery_preamble()` in `tuning_config.h` resolves the
   three cases at send time; nothing else reads the raw setting. The "Tuning overrides active" log
   line names `pairing_discovery_preamble` only when it was set, never because a driver's start
   preamble differs from the compiled default.

## Consequences

- VELUX SSL and SML shutters pair with no tuning entry. The documented recipe for them drops the
  preamble line; existing configs with `pairing_discovery_preamble: 32` keep working unchanged.
- A device in learning mode that only listens in short duty-cycled windows would miss a short
  discovery. None is known; such a device would need `pairing_discovery_preamble: 1024` or
  `pairing_discovery_low_power: true`.
- At 32 bytes on SX1276 (about 6.7 ms on air) the first frame is shorter than the 14 ms of the
  patent, and a receiver that scans its channels may miss a first attempt. Discovery's three
  attempts cover that today; the length of the normal start preamble itself is a separate question
  that this decision leaves to `normal_start_preamble`.
- `PAIRING_DISCOVERY_PREAMBLE` is gone from `proto_timing.h`; the tunable's ceiling stays at
  `LONG_PREAMBLE`, the only value that reaches the wake-up level (ADR 0044).

## Considered options

- **A fixed short discovery constant (about 65 bytes, the patent's 14 ms).** Rejected: a second
  "normal first frame" length next to `normal_start_preamble` would let the two drift; one value
  for both keeps discovery and directed start frames in step.
- **Short first, long fallback on the third attempt.** Rejected: no public evidence of a device
  that needs a long plain discovery, and a 1024-byte attempt costs about 213 ms of air time on
  every pairing that finds nothing.

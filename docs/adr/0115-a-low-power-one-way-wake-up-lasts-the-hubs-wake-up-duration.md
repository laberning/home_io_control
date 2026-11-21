# ADR 0115: A low-power one-way wake-up lasts the hub's wake-up duration

<!-- doxygen-label: adr0115 -->

**Status:** Accepted, amends [ADR 0097](0097-every-preamble-is-set-as-a-duration-the-same-on-every-radio.md) and [ADR 0038](0038-oneway-bursts-follow-the-identity-power-class.md) · **Recorded:** 2026-11

Amends ADR 0097 (every preamble is a duration, except the one-way wake-up) and ADR 0038 (one-way bursts follow the
identity's power class), and corrects passages of ADRs 0026, 0027, 0031, 0032 and 0086 that no longer match the
code.

## Context

ADR 0097 made every preamble a duration that means the same on every radio, but left one-way bursts with their own
wake-up of 1024 programmed bytes. A byte count is a different duration per radio: about 213 ms on the SX1262 and
LR1121 and about 267 ms on the SX1276, as the SDR captures
`tests/corpus/captures/oneway/unenrolled_window_opener_oneway_stop_low_power_burst_{sx1276,sx1262,lr1121}.yaml` show.
The two-way wake-up for a resting battery device is 600 ms (`low_power_wake_ms`, ADR 0095 and ADR 0097), and a
tester's solar blind sometimes ignored a one-way command until a 2048-byte wake-up was tried on a modified build
(issue #74). A `low_power: true` identity exists for exactly those devices.

The wake label a radio uses (`TxWake::LONG`) is decided by comparing programmed bytes with 1024. The setting's floor
of 300 ms is at least 1024 programmed bytes on every radio, so a duration taken from the setting is always labelled a
wake-up.

## Decision

- **The wake-up copy of a `low_power: true` identity lasts `low_power_wake_ms`** (default 600 ms), the hub's one
  setting for waking a resting device, on every radio. Each driver turns the duration into its own byte count; a
  change of the setting applies to the next command. There is no separate one-way setting.
- **The long-on-every-copy shape keeps 1024 bytes per copy** (the Somfy default and the unknown-manufacturer default,
  ADR 0041) until the bench run that decides the Somfy default; its air time per radio does not move here.
- **Short copies** follow `start_preamble_ms` as before.
- **The transmit log gives each copy's preamble in milliseconds**, not in bytes, so a report reads the same on every
  board.

## Consequences

- A `low_power: true` command holds the radio for about 0.8 s (one 600 ms copy, three short ones and the gaps) on
  every board, instead of about 0.42 s (SX1262, LR1121) or 0.47 s (SX1276); it stays below the 1.0 to 1.2 s of the
  long-on-every-copy shape. A VELUX enrollment, six bursts, takes about 5 s instead of about 2.5 s.
- A user who needs a longer or shorter wake-up sets `low_power_wake_ms`, which also changes the two-way wake-up.
- `LONG_PREAMBLE` (1024 bytes) remains as the legacy shape's wake-up and as the threshold of the wake label.

## Corrections to earlier records

These records are not edited; this record states what holds instead.

- **ADR 0097**, "One-way bursts keep their own wake-up preamble (1024 bytes)": true only for the long-on-every-copy
  shape; a `low_power: true` wake-up follows `low_power_wake_ms` (above).
- **ADR 0038**, the TX log shown as "preamble 1024/32/32/32": the log gives milliseconds, the `low_power: true`
  wake-up is a duration as above, and the short copies follow the start preamble, about 10 ms (48 or 38 programmed
  bytes depending on the radio). The tuning value it names, `normal_start_preamble`, is `start_preamble_ms` (ADR 0097).
  Its open question whether the default should stay the long shape for every manufacturer was settled by ADR 0041.
- **ADR 0032**, "no automated sync check" for the profiled-vendor set: `tests/sync/oneway_wire_profile_sync_test.cpp`
  checks it.
- **ADR 0026**, "one short `0x39` burst, immediately followed by one short `0x30` burst": the bursts follow the
  identity's shape like any other one-way burst (ADR 0038), and the Somfy gesture addresses `00 00 3F` (ADR 0032's
  amendment).
- **ADR 0027**, rejecting two one-way entities with the same identity and class "deferred until entities exist":
  identities are checked at build time for shared and colliding addresses; no one-way entity of that kind exists.
- **ADR 0031**, the TX log reading `STOP -> vertical_exterior_awning`: only the "Last 1W Command" sensor reads so;
  the transmit log line reads `1W tx: from <address> to <class or all> intent STOP (4x, preamble … ms)`, with the
  preamble of each copy in milliseconds.
- **ADR 0086**, "a wake-up preamble (about 218 ms on air)": about 213 ms on the SX1262 and LR1121 and about 267 ms on
  the SX1276 for the long shape, `low_power_wake_ms` for a `low_power: true` wake-up.

# ADR 0042: The directed start preamble's default comes from the radio driver

<!-- doxygen-label: adr0042 -->

**Status:** Accepted · **Recorded:** 2026-09

## Context

[ADR 0029](0029-start-preamble-is-a-property-of-the-target.md) made the directed start preamble a
property of the target's power class and set `normal_start_preamble` to 32 bytes for an
always-alive target. That number is the preamble the io-homecontrol radio notes describe (256
bits), and the ADR was candid that the margin around it was unmeasured: *"32 bytes is a defensible
middle, not a measured number."*

It has now been measured, and it is not enough on two of the three supported chips.

A bench comparison drove one mains-powered Somfy Izymo dimmer from four boards in turn — same
device, same position, same programmed 32-byte preamble, 30 on/off cycles each — and counted how
often a directed start frame was answered on the first try:

| Board | Chip | PHY | First-try |
|---|---|---|---|
| Heltec V2 | SX1276 | register, hardware IoHomeOn | **60/60** |
| LilyGo T3-S3 | LR1121 | shared software PHY | 57/65 |
| Heltec V4.3 | SX1262 + FEM | shared software PHY | 56/65 |
| Heltec V3 | SX1262 | shared software PHY | 54/65 |

A sweep of `normal_start_preamble` on the V3 then found a step rather than a slope: **78.9% at 32
bytes, and 60/60 at 48, 64, 128 and 256.**

Three explanations are ruled out by the data. The **chip** is not the variable: LR1121 and SX1262
are statistically indistinguishable from each other and both differ from the SX1276. The **link
budget** is not the variable: V4.3 and V3 run the same chip and the same driver with 5 dB between
their front ends, and score the same. The **device** is not the variable: it answers a programmed
32-byte preamble from the SX1276 every time.

What the two failing boards share is `SoftPhyDriverBase` and `radio_soft_phy`. The SX1276 does not.

**Why the soft PHY's emitted preamble is worse than its programmed length is not known.** Four
candidate causes were examined and eliminated: the software UART encoding (it covers only the
payload; the chip generates the preamble), the byte-to-bit conversion in
`build_gfsk_packet_params()` (correct, and byte-equivalent to the SX1276's registers), GFSK pulse
shaping (both are Gaussian BT=1.0), and PA ramp time (the SX1261/2 datasheet states the ramp
completes before preamble transmission begins, so it is startup latency, not airtime). A documented
difference does remain — the SX126x transmits a `0x55` preamble byte and cannot be configured
otherwise, while the SX1276 is set to the `0xAA` polarity the protocol notes describe — but the two
are the same alternating sequence offset by one bit, so its significance is unproven.

## Options considered

- **Raise `NORMAL_START_PREAMBLE` globally to 48.** Rejected: the SX1276 does not need it, and
  every extra byte is airtime on every start frame, against a 0.1% duty-cycle budget on this band.
- **Add a per-driver offset to the configured value.** Rejected: it breaks what the knob means. A
  user setting 64 would get 80 on air, the number in the UI would stop matching the number in the
  log, and values far above the threshold would be inflated for nothing.
- **Clamp with a per-driver floor**, `max(configured, driver_minimum)`. Rejected: ADR 0029 shipped
  this value as a live Home Assistant number specifically so a reporter could bisect it downward in
  the field, and a floor blocks the diagnostic that found this problem.
- **Make it a per-device YAML key.** Rejected: the deficit is a property of the hub's transmitter,
  not of the device. Every device on an SX1262 would need the same value, so this would ask users
  to configure a hub-side defect once per device.
- **Driver-supplied default, explicit override always honoured** (chosen).

## Decision

`RadioDriver::default_start_preamble()` returns the preamble a directed start frame gets when the
user has not set `normal_start_preamble:` in YAML.

- The base returns `NORMAL_START_PREAMBLE` (32), the protocol's documented value. `RadioSX1276`
  does not override it, so a V2 keeps exactly the behaviour ADR 0029 shipped.
- **`SoftPhyDriverBase` overrides it** with `SOFT_PHY_START_PREAMBLE` (48). The override sits on the
  shared base rather than on each concrete driver because that is the level the effect follows;
  giving SX1262 and LR1121 separate values would invent a difference the measurement does not show.
- `TuningConfig` gains `normal_start_preamble_from_yaml`, emitted by codegen when the key is
  present. The value alone cannot distinguish "the user chose 32" from "nobody chose anything",
  and only the second case should take the driver's default.
- `IOHomeControlComponent::resolve_start_preamble_default_()` applies it once during `setup()`,
  after the radio is constructed and before the first transmit.
- **`dump_config()` reports the resolved value and whether it came from YAML or the driver**, not
  `setup()`. A log client that attaches after boot receives only the config dump, which is how most
  reporters capture logs — a setup-time line would be invisible in exactly the case it is needed.
- An explicit `normal_start_preamble:` always wins, **including a value below the default**, so
  field bisection still works.

## Consequences

- **SX1262 and LR1121 hubs change behaviour**, from 32 bytes to 48 on every directed start frame,
  every roll-call always-alive pass (ADR 0029/0037) and every "short" try of the low-power wake
  ladder. The last two are untested at 48 and expected to improve for the same reason the directed
  frame does.
- **SX1276 hubs are unchanged.**
- **Airtime rises by 3.3 ms per start frame on the soft-PHY chips**, which matters against the
  0.1% duty-cycle limit. It is partly self-funding: at 32 bytes roughly one exchange in five needed
  a second start frame, and those retries are airtime too.
- **48 is the first value measured to work, not a value with known margin.** The threshold is
  bounded only to `(32, 48]`, on one device, one link and one chip — the sweep ran on the SX1262,
  which was the *worst* of the three soft-PHY boards at 32 bytes, so the better-performing LR1121
  is expected to clear it too. A device needing more than 48 would still fail, and the remedy is
  the existing knob.
- **The cause remains unexplained**, so this compensates for a defect rather than fixing one. If
  the shortfall is later identified and corrected, this default should be revisited rather than
  left as permanent padding.

See also [ADR 0029](0029-start-preamble-is-a-property-of-the-target.md) (the preamble model this
extends) and [ADR 0041](0041-unset-oneway-power-class-comes-from-the-manufacturer-profile.md),
which resolves an unset 1W key from a manufacturer profile by the same unset-versus-explicit rule.

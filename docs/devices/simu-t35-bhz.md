# SIMU T3.5 E BHz DC solar shutter motor
<!-- doxygen-label: dev_simu_t35_bhz -->

A solar tubular motor for roller shutters, sold by SIMU as the **T3.5 E BHz DC** (Autosun 2 BHz;
one report writes it "BHe"). It speaks io-homecontrol at 868 MHz, so the hub can pair it as a
two-way device and read its position back. Six of these motors are confirmed on a Heltec V3
(SX1262).

## Check the motor first

SIMU also sells a **T3.5 EHz DC**, without the B. That motor uses SIMU-Hz at 433 MHz and is not
io-homecontrol, so this project cannot control it. Look for "E BHz" on the motor label or its box
before you spend an evening on pairing.

## What works

| Capability | Status |
|---|---|
| Discover & Pair | ✅ Confirmed on six motors |
| Open, close | ✅ Confirmed |
| Live position while moving | ✅ Confirmed |
| Device name | ✅ Confirmed — the motor reports `T3.5EBHZ DC` |
| RSSI | ✅ Confirmed |
| Being driven by its BHz wall transmitter | ✅ Confirmed — the hub hears the transmitter's PROG press |

## Pairing a SIMU shutter motor

The motor is normally already tied to a **BHz wall transmitter** (a 1W remote). It has no button of
its own, so you open its pairing window from that transmitter, then run Discover & Pair.

### Route 1: one PROG hold

1. Power down any other shutters that share the transmitter, so only one motor reacts.
2. Hold **PROG** on the BHz transmitter and **let go at the first jog** of the motor.
3. Press **Discover & Pair** in Home Assistant straight away.

This is the route that paired the first motor. Holding PROG past the first jog can remove the
transmitter from the motor instead. Give each attempt one PROG press.

### Route 2: three jogs

One user paired five more motors, each on the first attempt, after a longer gesture on the same
transmitter:

1. Hold **UP + DOWN** until the motor does its first back-and-forth jog, then release.
2. Straight away, hold **UP + DOWN + FAVORITE (My)** until the second jog, then release.
3. Straight away, hold **PROG** on the back of the transmitter until the third jog, then release.
4. Press **Discover & Pair** in Home Assistant immediately.

Try route 1 first. The same user paired a motor with route 1 alone, so the first two steps of
route 2 may not be needed, and nobody has compared the two on the same motor yet. If route 1 finds
nothing after a few attempts, route 2 is the next thing to try.

## Working YAML

```yaml
cover:
  - platform: home_io_control
    name: "Office shutter"
    io_device_id: "0D1E2E"
    io_device_type: "roller_shutter"
    low_power: true
```

`low_power: true` is what the pairing log recommended for these motors. The ID above is an example;
Discover & Pair prints the real one together with a ready-to-paste snippet.

## Known quirks

- **Check the model before troubleshooting.** A motor that never answers discovery, with a manual
  that mentions "radio standby" or SIMU-Hz, is most likely the 433 MHz EHz model.
- **The gesture does not carry over to Somfy motors.** One Somfy awning owner tried step 1 of route
  2 and the motor did not react. Somfy motors have their own route; see
  [Supported devices](../supported-devices.md).

## Evidence

Hardware reports only; there are no corpus captures for this motor yet. The first motor is
documented in issue #138, the five others in issue #149, all on a Heltec V3 with the SX1262 driver.

## See also

- [Pairing](../pairing.md)
- [Supported devices](../supported-devices.md)

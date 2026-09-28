# Somfy White LED Receiver io
<!-- doxygen-label: dev_somfy_white_led_receiver -->

An IO-Homecontrol dimmer for 12/24 V white LED strips with four independent outputs (Somfy
reference 1822611). It turns up in pergola kits alongside a Somfy motor, with a Situo io remote
that drives both. The hub logs it under the name `LightVar_Wh_io`.

Two things about this receiver are different from other lights, and both catch people out: each
output pairs as a separate device, and an SX1262 board needs a wider receive filter to hear it.

## What works

| Capability | Status |
|---|---|
| On / off | ✅ Confirmed |
| Brightness (intermediate levels) | ✅ Confirmed |
| Status readback after a command | ✅ Confirmed |
| Discover & Pair, one output at a time | ✅ Confirmed on an SX1262 board with `sx1262_rx_bandwidth: 156.2` |
| The Situo remote keeps working alongside the hub | ✅ Confirmed |

## Each output is its own device

The receiver has four outputs, and each one answers pairing under its own node ID. A command to one
node ID switches only that output. Home Assistant therefore needs one `light:` entry per output you
want to control.

This matters because every output behaves like a working light over the air, including an output
with no LED strip connected. If you pair the wrong output, the hub sends each command, the receiver
confirms it, the entity's state updates, and the lamp stays dark. When that happens, pair the output
the lamp is actually wired to, as described below.

## Setting the receive filter first

On an SX1262 board (Heltec V3, V4), set this before you pair:

```yaml
home_io_control:
  tuning:
    sx1262_rx_bandwidth: 156.2
```

At the default of `58.6` kHz the hub misses most of this receiver's replies. Commands then fail
with `wait_first_timeout`, and pairing can fail with no response at all. At `156.2` two separate
installations went from mostly missed replies to reliable ones. The setting applies to every device
on the hub. The Somfy motors paired alongside this receiver kept working at `156.2`. See
[`sx1262_rx_bandwidth`](../configuration/tuning.md#sx1262_rx_bandwidth) for what the value does.

On an LR1121 board the default filter is already `117.3` kHz. This receiver hasn't been tested on
an LR1121 or an SX1276 board. If its replies go missing there, widen the filter the same way
(`lr1121_rx_bandwidth: 156.2`, or `sx1276_rx_bandwidth: 83.3`).

## Onboarding route that worked

**Discover & Pair, once per output, started from the Situo channel that controls that output:**

1. On the Situo, select the channel that switches the LED strip you want to pair.
2. Hold its PROG button for about 2 seconds, until the LED strip flashes briefly. The flash shows
   that output is in programming mode.
3. Press **Discover & Pair** in Home Assistant within a few seconds.
4. The hub finds a `LightVar_Wh_io` device and logs a YAML snippet with its node ID. Add it as a
   `light:` entry (see below).
5. Switch it on from Home Assistant and check that the right strip lights up.

Repeat for each output you use, selecting that output's Situo channel each time. An output that is
already paired doesn't answer pairing again, so each run finds a new one.

The Situo stays registered with the receiver through all of this, so it keeps controlling the
lights as before.

## Pairing a pergola kit

A pergola kit has a motor and this receiver on the same Situo, and often on the same power circuit.
Pair them separately, each from its own Situo channel. Pair the motor first, following the motor's
own route (for a Sunea, see [Somfy Sunea IO](somfy-sunea-io.md)), then pair each LED output as
above.

**A `LightVar_Wh_io` that turns up while you pair the motor is not necessarily your lamp's output.**
A spare output can answer the motor's pairing run, and it looks like a working light even though
nothing is connected to it. Test every light entity against the real strip. If one does nothing,
pair the lamp's output from its Situo channel, then remove the spare entry from your YAML.

## Working YAML

```yaml
home_io_control:
  tuning:
    sx1262_rx_bandwidth: 156.2

light:
  - platform: home_io_control
    name: "Pergola Light"
    io_device_id: "0626CD"
    io_device_type: "light"
    io_subtype: 0
    dimmable: true
```

`dimmable: true` gives you brightness control; without it the entity is on/off only. Add a second
`light:` entry with its own `io_device_id` for each further output you pair.

## Known quirks

- **The reply to a command doesn't prove the lamp switched.** The state the receiver reports
  straight after an on or off can still be the previous one, and it looks the same on an output
  with nothing connected. The status poll that follows is the value to trust, and the strip itself
  is the only proof you have the right output.
- **The closing reply is sometimes lost when the receiver switches a lit lamp.** It shows up as a
  rising [Unconfirmed Exchanges](../diagnostic-entities.md#link-health) count. Check the lamp when
  that happens: an unconfirmed command may or may not have been carried out, and the entity shows
  the real state again after the next status poll.

## Evidence

- [Issue #143](https://github.com/laberning/home_io_control/issues/143): a pergola kit on a Heltec
  V3 / SX1262. An output with nothing connected turned up while the motor (a Sunea SCR 40 io) was
  being paired, and it confirmed every command without switching the lamp. Pairing from the Situo's
  light channel found the lamp's own output, which then did on, off, brightness and status. It
  also established `156.2` as the working filter.
- [Issue #119](https://github.com/laberning/home_io_control/issues/119): a second installation on an
  SX1262, where the receive-filter sweep was done and the lost closing replies were measured.
- [Issue #121](https://github.com/laberning/home_io_control/issues/121): a Pergola io kit whose
  receiver never answered pairing at the default filter or at `117.3`.

## See also

- [Lights, locks and switches](../configuration/light-lock-switch.md)
- [Pairing](../pairing.md)
- [Supported devices](../supported-devices.md)

# Thermor Interface Cozytouch for pilot-wire heaters
<!-- doxygen-label: dev_thermor_pilot_wire_interface -->

A small mains-powered io-homecontrol module (Thermor reference 450 251) that sits in front of an
electric heater with a pilot wire and passes the Cozytouch's orders on to it. The hub sets the
heater's level through it: comfort, the two reduced comfort levels, eco, frost protection and off.
One interface is reported on a LilyGO T3 V1.6.1 (SX1276) in
[issue #228](https://github.com/laberning/home_io_control/issues/228); see
[Heating and climate](../configuration/climate.md) for the options.

Two things set this interface apart from the other heaters: it cannot be paired on its own, and it
reports no level back.

## What works

| Capability | Status |
|---|---|
| Discover & Pair on the interface itself | ❌ Does not work: a key push draws a challenge and no key confirmation, and a key pull is refused with code `0x08` |
| Control with the key of its Cozytouch installation | ✅ Confirmed |
| Comfort, Comfort -1, Comfort -2, Eco, frost protection and Off | ✅ Confirmed: the heater's display follows each one |
| Boost | ❌ Not offered: the interface ignores a boost |
| Reading the level back | ❌ The interface's status carries no heating level, and most status reads go unanswered |

What the display of the reported heater showed for each pilot-wire order:

| Order | Heater display |
|---|---|
| Comfort | `PROG 20.0°`, "Montée en température" (heating up) |
| Comfort -1 | `PROG CONF-1 19.0°` |
| Comfort -2 | `PROG CONF-2 18.0°` |
| Eco | `PROG ECO 15.0°` |
| Frost protection | `H.Gel 7.0°`, "Ordre prioritaire" |
| Off | "Délestage / Arrêt prioritaire", the load-shedding display |

## Modes in Home Assistant

The card offers `off` and `heat`, with one preset for each pilot-wire order and no boost.
[Level heaters and pilot-wire interfaces](../heating.md#level-heaters-and-pilot-wire-interfaces) has
the table of which choice sends which order.

## Getting the interface's key

The interface's own pairing does not work with the hub, so the hub controls it with the key it
already holds: the key of the Cozytouch installation it belongs to. A radiator paired with the same
Cozytouch, such as a Thermor EMOTION 3 H, hands over that key.

1. Pair one radiator of the same Cozytouch installation with **Discover & Pair**, as described in
   [Pairing a radiator](atlantic-agilia-io.md#pairing-a-radiator). The hub's log prints the key.
   Pairing only reads the radiator's key, so the radiator and the interface keep working with the
   Cozytouch.
2. Put the key into `secrets.yaml` and use it as the `system_key:` of the interface's entry.
3. Find the interface's address. Set `logger: level: DEBUG`, then change the heater's level in the
   Cozytouch app. The hub logs each frame it hears on an `io_capture` line, and the Cozytouch's
   frames to the interface carry its address as `dst=`.

Leave the interface's own pairing mode alone. The 4-second press that starts it also gives the
interface a new address, and the reporter had to pair it with the Cozytouch again afterwards.

## Working YAML

```yaml
climate:
  - platform: home_io_control
    name: "Bedroom heater"
    io_device_id: "91105B"
    io_device_type: "electrical_heater"
    io_subtype: 0
    io_model: pilot_wire_interface
    low_power: true
    system_key: !secret cozytouch_key
```

`io_model: pilot_wire_interface` removes the `boost` preset, which the interface ignores.
Keep `low_power: true`, although the interface is mains-powered: every confirmed command was sent
with it, and in an earlier run without it the interface answered only on the fifth try.

## Known quirks

- **The card shows what Home Assistant last sent.** The interface reports no level, so a change made
  at the heater or in the Cozytouch app does not show; see
  [What the card shows comes from the heater](../heating.md#what-the-card-shows-comes-from-the-heater).
- **Every command ends unconfirmed.** The interface never sends the closing reply of an exchange, so
  the hub logs each command as accepted without a closing reply, and the Unconfirmed Exchanges
  counter rises. The heater's level changes each time all the same.
- **A command is sometimes not heard.** Now and then the interface does not answer at all and the card
  keeps the previous state. Send the command again; a command can take two or three sends.
- **Comfort -1 and Comfort -2 can take a few minutes** to show on the heater's display.

## Evidence

All from [issue #228](https://github.com/laberning/home_io_control/issues/228), on a LilyGO T3
V1.6.1 with the SX1276 driver:

- [A Cozytouch bridge setting Eco and then Comfort on the interface, overheard by the hub](https://github.com/laberning/home_io_control/blob/main/tests/corpus/captures/exchange/thermor_pilot_wire_interface_exchange_heating_level_execute.yaml)
- The reporter's display readings for each of the six orders, in the issue thread.

## See also

- [Heaters and water heaters in Home Assistant](../heating.md)
- [Heating and climate](../configuration/climate.md)
- [Thermor EMOTION 3 H and the other heaters](../supported-devices.md#heating-and-hot-water)
- [Supported devices](../supported-devices.md)

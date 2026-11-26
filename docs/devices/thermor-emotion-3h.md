# Thermor EMOTION 3 H electric radiator
<!-- doxygen-label: dev_thermor_emotion_3h -->

An electric radiator of the Atlantic group. It reports type `0x34`, subtype 1, and is set up like
the [Atlantic AGILIA IO](atlantic-agilia-io.md), with no `io_model:`. Two radiators are confirmed on
a LilyGO T3 V1.6.1 (SX1276) in issue #228.

## What works

| Capability | Status |
|---|---|
| Discover & Pair from the radiator's menu | ✅ Confirmed on two radiators |
| Target temperature in `heat` | ✅ Confirmed on the display |
| Occupancy (`occupancy:` binary sensor) | ✅ Confirmed on both radiators |
| Energy total (`energy:` sensor) | ⚠️ Experimental — both radiators answer; nobody has compared the value with the display |
| A target set while the program runs (`auto`, PROG) | ❌ Refused: the write is acknowledged and the register reads back 0 |

## Onboarding route that worked

**Discover & Pair, from the radiator's own menu.** Open "Paramètres" → "Connectivité" →
"Connexion" on the radiator, then press Discover & Pair. The hub pulls the radiator's key, as on
the AGILIA IO; see
[Devices that hand over their own key](../pairing.md#devices-that-hand-over-their-own-key).

Both radiators hand over the same key, the one of their Cozytouch installation. That key also
drives a [Thermor Interface Cozytouch](thermor-pilot-wire-interface.md) of the same installation.

## Working YAML

```yaml
climate:
  - platform: home_io_control
    name: "Living Room Radiator"
    io_device_id: "…"
    io_device_type: "electrical_heater"
    io_subtype: 1
    system_key: !secret radiator_key
    occupancy:                # optional
      name: "Living Room Occupancy"
```

The options are on [Heating and climate](../configuration/climate.md); what the card offers is on
[Radiators with a setpoint](../heating.md#radiators-with-a-setpoint).

## Known quirks

- **Set a target in `heat`.** While the radiator runs its program, in `auto` and in PROG, it
  refuses a target: the write is acknowledged and the register reads back 0 (issue #228).
- **Frost protection is 7.0 °C.** A target of 8.0 °C is answered with 7.0 °C, the value the
  radiator keeps. The Cozytouch cannot set 8.0 °C either.
- **A detection lasts only a short time.** A short visit between two polls of the `occupancy:`
  sensor can be missed.
- **The energy total can go back a little after a restart.** One radiator read 9 kWh less after it
  restarted. You see a small negative step in the energy dashboard and a warning in the Home
  Assistant log.

## Evidence

Issue #228, two radiators on a LilyGO T3 V1.6.1 (SX1276).

## See also

- [Atlantic AGILIA IO](atlantic-agilia-io.md) — the radiator this one is set up like
- [Heaters and water heaters in Home Assistant](../heating.md)

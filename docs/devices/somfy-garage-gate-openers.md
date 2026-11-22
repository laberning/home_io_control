# Somfy garage and gate openers
<!-- doxygen-label: dev_somfy_garage_gate_openers -->

Three openers are reported: a MOTO 1000 io garage drive (sold by Wisniowski), a Dexxo Smart io 800
garage door opener and an Elixo 3S io swing gate opener. All three open, close and stop. None takes
a percent position, so each shows in Home Assistant as a cover without a slider.

## What works

| Device | Capability | Status |
|---|---|---|
| MOTO 1000 io | Discover & Pair, first try | 📣 Reported |
| MOTO 1000 io | Open, close, stop, status, also from rest | 📣 Reported |
| MOTO 1000 io | Partial Position button | 📣 Reported — from closed or open, not from a door stopped mid-travel |
| MOTO 1000 io | Percent positions and the favorite position | ❌ Refused by the drive |
| MOTO 1000 io | Linked remotes | 📣 Reported |
| Dexxo Smart io 800 | Open, close, stop | 📣 Reported |
| Dexxo Smart io 800 | Built-in light on and off | 📣 Reported |
| Elixo 3S io | Open, close, stop | 📣 Reported |
| Elixo 3S io | Built-in light output on and off | 📣 Reported |
| Elixo 3S io | Pedestrian Position button | Not reported yet |
| Dexxo and Elixo | KeyGo io buttons as `linked_remotes` | 📣 Reported |

## Onboarding routes that worked

- **MOTO 1000 io:** [Discover & Pair](../pairing.md), on a Heltec V3 (SX1262), issue #163.
- **Dexxo Smart io 800 and Elixo 3S io:** [key extraction](../key-extraction.md) from a TaHoma
  Switch, which keeps working alongside, on a LilyGO T3 V2.1 (SX1276), issue #244.

## Working YAML

Declare `io_subtype: 58` so the cover shows open, close and stop and no slider; see
[Covers that only open and close](../configuration/cover.md#covers-that-only-open-and-close).

```yaml
cover:
  - platform: home_io_control
    name: "Garage Door"
    io_device_id: "…"
    io_device_type: "garage_opener"   # "gate_opener" for the Elixo
    io_subtype: 58
    low_power: true                   # MOTO 1000 io
    linked_remotes:
      - "…"                           # one entry per KeyGo io button

light:
  - platform: home_io_control
    name: "Garage Light"              # the built-in light of a Dexxo or Elixo
    io_device_id: "…"
    io_subtype: 58
```

Each KeyGo io button transmits with its own sender address, so a four-button remote is up to four
`linked_remotes` entries.

## Known quirks

- **The MOTO 1000 io refuses the regular position frame.** It answers that frame and a 50 %
  position with `PARAMETER_UNKNOWN`, so garage openers get the shorter frame and no Favorite
  Position button.
- **The Partial Position button moves the door to the position stored in the drive.** It is
  disabled by default; see
  [Partial and pedestrian position buttons](../configuration/cover.md#partial-and-pedestrian-position-buttons).
  After a partial move the drive keeps reporting "moving" for about half a minute.
- **Keep `response_preamble_ms` at its default (3.5) or lower on the MOTO 1000 io.** The drive
  ignores a command whose authentication answer arrives late, as it did behind 6.7 ms; see
  [A tuning plan](../troubleshooting.md#a-tuning-plan). The reporter's confirmed runs used 1.7 ms
  (8 bytes on the SX1262). 3.5 ms is not yet tested on this drive, so if commands stop working, set
  [`response_preamble_ms`](../configuration/tuning.md#response_preamble_ms) to `1.7`.

## Evidence

- MOTO 1000 io: issue #163, with corpus captures
  `wisniowski_moto1000_exchange_execute_parameter_unknown` and
  `wisniowski_moto1000_exchange_execute_partial_position_refused`.
- Dexxo Smart io 800, Elixo 3S io and the KeyGo io remote: issue #244.

## See also

- [Covers](../configuration/cover.md)
- [Linked remotes](../configuration/remotes.md#linked-remotes)

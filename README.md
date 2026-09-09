# Home IO Control
[![CI](https://github.com/laberning/home_io_control/actions/workflows/ci.yml/badge.svg)](https://github.com/laberning/home_io_control/actions/workflows/ci.yml)
[![CodeQL](https://github.com/laberning/home_io_control/actions/workflows/codeql.yml/badge.svg)](https://github.com/laberning/home_io_control/actions/workflows/codeql.yml)
[![Fuzz](https://github.com/laberning/home_io_control/actions/workflows/fuzz.yml/badge.svg)](https://github.com/laberning/home_io_control/actions/workflows/fuzz.yml)
[![Docs](https://github.com/laberning/home_io_control/actions/workflows/doxygen-deploy.yml/badge.svg)](https://github.com/laberning/home_io_control/actions/workflows/doxygen-deploy.yml)
[![GitHub](https://img.shields.io/github/license/laberning/home_io_control)](https://github.com/laberning/home_io_control/blob/main/LICENSE)
![GitHub Repo stars](https://img.shields.io/github/stars/laberning/home_io_control)

Home IO Control is an [ESPHome](https://esphome.io/) component that speaks IO-Homecontrol.
Flash an ESP32 with a LoRa radio and it talks to your devices directly — locally, over the air,
with no vendor gateway and no cloud. Covers, lights and buttons appear in Home Assistant as native
entities, with real position feedback from two-way devices.

## What you need

An ESP32 board with an SX1276, SX1262 or LR1121 radio at 868 MHz. [Hardware](docs/hardware.md) lists
some available boards and shows the pins each one needs.

If your device speaks IO-Homecontrol, it has a good chance of working. The
[device matrix](docs/supported-devices.md#device-matrix) lists what has been tried, and what to
do when yours is not there.

## Features

- **Cover control** — open, close, stop, and set position (0–100%) for shutters, blinds, awnings,
  window and garage openers, curtain tracks, and related position-based devices.
- **Real position feedback** from the device, over the 2W protocol.
- **Tilt control** for venetian-style blinds, exposed automatically for tilt-capable device types.
- **Light control**, binary or dimmable.
- **Favorite / My and ventilation buttons**, generated automatically for the device types that
  support them.
- **Device discovery and pairing** from a Home Assistant button, with a machine-readable
  "Last Pairing Result" diagnostic and a plain-language advisor when an attempt fails.
- **Key extraction from an existing installation** — emulate an unpaired device so your current
  hub hands over its `node_id`/`system_key`. No sniffing hardware, no device reset.
- **1W remote and sensor events** — overheard remote presses and wind/rain sensor triggers fire
  Home Assistant events for sender IDs you opt in.
- **Sending 1W commands**, so the hub can drive devices that only listen for a remote.
- **LR1121 radio-firmware flashing**, opt-in, including a bootloader rewrite behind an arming
  switch.
- **Three radio chips supported** — SX1276, SX1262 and LR1121, all validated on real devices.
- **Experimental support for locks, switches and climate/heating devices.** None of these has
  been exercised against real hardware yet; if you own one, try it and report back.

## Installation

Add to your ESPHome YAML configuration:

```yaml
external_components:
  - source: github://laberning/home_io_control
```

Both the `esp-idf` and `arduino` frameworks are supported, though testing and development mostly
happen on `esp-idf`.

## Where to go next

| If you want to… | Go to |
|---|---|
| Get it working for the first time | [Getting started](docs/getting-started.md) · [Hardware](docs/hardware.md) · [Supported devices](docs/supported-devices.md) |
| Get a device onto the hub | [Pairing](docs/pairing.md) · [Key extraction](docs/key-extraction.md) |
| Look up a YAML option | [Configuration reference](docs/configuration/index.md) |
| Work out why something is broken | [Troubleshooting](docs/troubleshooting.md) · [FAQ](docs/faq.md) · [Radio tuning](docs/configuration/tuning.md) |
| Understand the code, or contribute | [Architecture overview](docs/architecture_overview.md) · [Contributing](docs/contributing.md) |

The full page tree is at [docs/](docs/index.md).


## Acknowledgments

This would be a much smaller project without the people who contributed to it — thank you. ❤️

- **Everyone who has tested, reported and sent me logs.** Most of the devices that work today are
  ones I have never owned. Someone put a radio next to theirs, tried what I suggested, and told me
  what came back. Many fixes only exist because someone stuck with a support thread round after
  round. If you would like to join in,
  [report your device](docs/contributing.md#reporting-unsupported-devices): one that *doesn't*
  work is as useful as one that does.
- [io-rts-esp32](https://github.com/nicolas5000/io-rts-esp32) — an ESP32
  implementation of IO-Homecontrol and Somfy RTS.
- [iown-homecontrol](https://github.com/Velocet/iown-homecontrol) — an open
  documentation effort covering the IO-Homecontrol protocol.
- [iown-homecontrol-esp32sx1276](https://github.com/cridp/iown-homecontrol-esp32sx1276) —
  an IO-Homecontrol implementation for the ESP32 and SX1276.
- [ESPHome](https://esphome.io/) — the framework this component is built on, and what makes
  the devices show up in Home Assistant as native entities.

## Disclaimer & License

> [!WARNING]
> **This project is built for interoperability and right-to-repair: freeing devices, hubs, and networks you already own from lock-in, provided "as is", without warranty of any kind. Only use it on devices, hubs, and networks you own or are authorized to modify — doing so with someone else's is unauthorized access, which is illegal in most jurisdictions.**

This project is licensed under the [MIT License](https://github.com/laberning/home_io_control/blob/main/LICENSE).

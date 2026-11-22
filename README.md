# Home IO Control
[![CI](https://github.com/laberning/home_io_control/actions/workflows/ci.yml/badge.svg)](https://github.com/laberning/home_io_control/actions/workflows/ci.yml)
[![CodeQL](https://github.com/laberning/home_io_control/actions/workflows/codeql.yml/badge.svg)](https://github.com/laberning/home_io_control/actions/workflows/codeql.yml)
[![Fuzz](https://github.com/laberning/home_io_control/actions/workflows/fuzz.yml/badge.svg)](https://github.com/laberning/home_io_control/actions/workflows/fuzz.yml)
[![Docs](https://github.com/laberning/home_io_control/actions/workflows/doxygen-deploy.yml/badge.svg)](https://github.com/laberning/home_io_control/actions/workflows/doxygen-deploy.yml)
[![GitHub](https://img.shields.io/github/license/laberning/home_io_control)](https://github.com/laberning/home_io_control/blob/main/LICENSE)
![GitHub Repo stars](https://img.shields.io/github/stars/laberning/home_io_control)

Home IO Control is an [ESPHome](https://esphome.io/) component that speaks IO-Homecontrol, the
868 MHz radio protocol of Somfy, VELUX, SIMU, Atlantic, Thermor, Sauter and other manufacturers.
Flash an ESP32 with a LoRa radio and it talks to your devices directly — locally, over the air,
with no vendor gateway and no cloud. Shutters, awnings, windows, lights, heaters and water heaters
appear in Home Assistant as native entities, with the state the device itself reports.

## What you need

An ESP32 board with an SX1276, SX1262 or LR1121 radio at 868 MHz. [Hardware](docs/hardware.md) lists
some available boards and shows the pins each one needs.

If your device speaks IO-Homecontrol, it has a good chance of working. The
[device matrix](docs/supported-devices.md#device-matrix) lists what has been tried, and what to
do when yours is not there.

## Features

What it controls:

- **Covers** — shutters, blinds, awnings, pergolas, curtain tracks, and window, garage and gate
  openers. Open, close, stop and a position from 0 to 100 %, with the real position reported by the
  device, tilt on venetian-style blinds, and buttons for the favorite and ventilation positions
  where the device has them. See [Covers](docs/configuration/cover.md).
- **Lights and switches** — on/off, or dimmable. See
  [Lights, locks and switches](docs/configuration/light-lock-switch.md).
- **Heaters and water heaters** — electric radiators, towel dryers, pilot-wire interfaces and
  water heaters of the Atlantic group: target temperature, modes, boost, and readings such as room
  temperature and energy. See
  [Heaters and water heaters in Home Assistant](docs/heating.md).
- **Locks, exterior heating and thermostats** are implemented and waiting for a first report from
  real hardware. If you own one, try it and
  [report back](docs/contributing.md#reporting-unsupported-devices).

How a device gets onto the hub:

- **Discover & Pair** from a Home Assistant button, with a plain-language advisor when an attempt
  fails. See [Pairing](docs/pairing.md).
- **Key extraction** from a hub you already own, so every device stays paired as it is and the
  vendor hub keeps working alongside. See [Key extraction](docs/key-extraction.md).
- **One-way (1W) control** for devices that only listen for a remote: the hub enrolls as one more
  remote. See [Sending 1W commands](docs/configuration/oneway-transmit.md).

And around that:

- **Remote and sensor events** — presses of your wall remotes and wind or rain sensor triggers keep
  entity state fresh and can fire Home Assistant automations. See
  [Linked remotes and sender events](docs/configuration/remotes.md).
- **Three radio chips** — SX1276, SX1262 and LR1121, all validated on real devices.

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

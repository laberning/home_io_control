# Documentation
<!-- doxygen-label: docs_index -->

Everything about Home IO Control, from a first flash to the protocol internals.

## How it works

IO-Homecontrol is the 868 MHz radio protocol behind the motorised shutters, blinds, awnings and
window openers sold by Somfy, VELUX and other manufacturers. Home IO Control turns an ESP32 with a
LoRa radio into a controller for it: your devices appear in Home Assistant as native entities, with
no vendor hub in between.

It speaks the two-way (2W) protocol, so devices report their real position back rather than being
commanded blindly, and it can also transmit one-way (1W) commands for devices that only listen for
a remote.

Four terms come up on every page:

- **Node ID** — every participant on the radio has a 3-byte address, written as 6 hex characters
  such as `C0FFEE`. You choose one for the hub; each device has one from the factory.
- **System key** — the 16-byte secret every paired device shares with its hub. Every command that
  changes a device's state is authenticated with it.
- **Pairing** — how a device that no hub has claimed learns your key: put it into pairing mode and
  press Discover & Pair.
- **Key extraction** — how you obtain the key from a hub you already own, so every device that hub
  controls keeps working without being reset.

Your configuration is one `home_io_control:` block per board, holding the radio pins and that
identity, plus one entity per device.

## Getting it working

From a board in hand to a moving blind, and the two ways of getting a device onto the hub.

<!-- doxygen-subpages -->
- [Getting started](getting-started.md)
- [Hardware](hardware.md)
- [Supported devices](supported-devices.md)
- [Pairing](pairing.md)
- [Key extraction](key-extraction.md)
<!-- /doxygen-subpages -->

## Reference

Every YAML key, what the companion sensors report, and the small things that make the result nicer
to live with.

<!-- doxygen-subpages -->
- [Configuration reference](configuration/index.md)
- [Diagnostic entities](diagnostic-entities.md)
- [Tips and tricks](tips-and-tricks.md)
<!-- /doxygen-subpages -->

## When something is wrong

Indexed by what you see rather than by subsystem.

<!-- doxygen-subpages -->
- [Troubleshooting](troubleshooting.md)
- [FAQ](faq.md)
<!-- /doxygen-subpages -->

## Advanced

Tools you will not need for a normal installation: sending undecoded opcodes to learn what a device
answers, and flashing the LR1121's own radio firmware.

<!-- doxygen-subpages -->
- [Diagnostic probes](diagnostic-probes.md)
- [LR1121 firmware update](lr1121-firmware.md)
<!-- /doxygen-subpages -->

## For contributors

How the component is put together, how to report a device, and how to build the code.

<!-- doxygen-subpages -->
- [Architecture overview](architecture_overview.md)
- [Contributing](contributing.md)
- [Development setup](development-setup.md)
- [Documentation map](home_io_control.md)
<!-- /doxygen-subpages -->

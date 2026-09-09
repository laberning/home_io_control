# Documentation map
<!-- doxygen-label: guide_setup -->

One line per page, so you can find the right one by what you are trying to do. The
[documentation index](index.md) has the same pages as a tree.

| Topic | Page |
|---|---|
| Setting up for the first time, board to moving blind | [Getting started](getting-started.md) |
| Which board to buy, pinouts, radio pin requirements | [Hardware](hardware.md) |
| Which devices work, and the named type/manufacturer tables | [Supported devices](supported-devices.md) |
| Every YAML key, and the options entities share | [Configuration reference](configuration/index.md) |
| `cover:` — shutters, blinds, awnings, openers, curtains | [Covers](configuration/cover.md) |
| `light:`, `lock:`, `switch:` | [Lights, locks and switches](configuration/light-lock-switch.md) |
| `climate:` — heating, experimental | [Heating and climate](configuration/climate.md) |
| `rename_device`, `identify_device`, `force_open_device` | [Home Assistant actions](configuration/actions.md) |
| `linked_remotes:`, `exposed_senders:` | [Linked remotes and sender events](configuration/remotes.md) |
| `oneway_controllers:` — transmitting 1W commands | [Sending 1W commands](configuration/oneway-transmit.md) |
| Discover & Pair, Scan Paired Devices, pairing diagnostics | [Pairing](pairing.md) |
| Recovering a key from an existing hub | [Key extraction](key-extraction.md) |
| Active Issue, Link Health, Last Command | [Diagnostic entities](diagnostic-entities.md) |
| Flashing the LR1121's radio firmware | [LR1121 firmware update](lr1121-firmware.md) |
| Radio timing and discovery parameters | [Radio tuning](configuration/tuning.md) |
| Sending undecoded opcodes to learn what a device answers | [Diagnostic probes](diagnostic-probes.md) |
| When something is broken | [Troubleshooting](troubleshooting.md) · [FAQ](faq.md) |
| Grouping entities, composing a 1W slider | [Tips and tricks](tips-and-tricks.md) |
| How the component is built | [Architecture overview](architecture_overview.md) |
| Reporting a device, sending a pull request | [Contributing](contributing.md) · [Development setup](development-setup.md) |

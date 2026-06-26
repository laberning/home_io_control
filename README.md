# Home IO Control
[![GitHub](https://img.shields.io/github/license/laberning/home_io_control)](https://github.com/laberning/home_io_control/blob/main/LICENSE)
![GitHub Repo stars](https://img.shields.io/github/stars/laberning/home_io_control)

An [ESPHome](https://esphome.io/) external component for controlling IO-Homecontrol 2W devices (two-way, with device feedback). Control shutters, blinds, awnings, openers, curtains, and other IO-Homecontrol devices directly from ESPHome and Home Assistant using an ESP32 board with an SX1276 or SX1262 radio module.

> [!NOTE]
> **Experimental Project — Use With Caution**
> This project is in early development. You may encounter edge cases where certain features do not yet function as expected.

Contributions are welcome. If you have hardware that is not listed here, an unsupported device, or validated pin mappings, see the Get Involved section below.

## Features

- **Cover control**: Open, close, stop, and set position (0–100%) for shutters, blinds, awnings, window openers, garage openers, gate openers, rolling doors, curtain tracks, and related position-based devices
- **Favorite or My position action for covers**: Covers with a declared position-capable `io_device_type` automatically get a companion Home Assistant button named `<Cover Name> Favorite Position`
- **Stored device-name diagnostics**: Covers, lights, locks, and switches auto-generate a diagnostic text sensor named `<Entity Name> Device Name`, disabled by default to reduce clutter and populated from the actuator's internally stored name via a protocol read on boot
- **On-demand device rename action**: The hub exposes a native ESPHome API action named `esphome.<node_name>_rename_device` so Home Assistant can rename a paired actuator without adding persistent helper entities
- **Tilt support for venetian-style blinds**: Tilt-capable device types expose slat-angle control automatically in Home Assistant when the paired device reports tilt support
- **Experimental binary light support**: On/off-only light entities for IO-Homecontrol light devices
- **Experimental lock support**: Lock/unlock entities for IO-Homecontrol lock devices
- **Experimental binary switch support**: On/off-only switch entities for IO-Homecontrol on/off switch devices
- **Position feedback**: Real-time position updates from devices (2-Way protocol)
- **Device discovery & pairing**: Pair new devices directly from Home Assistant via a button entity (confirmed on SX1276; currently not working on SX1262)
- **Home Assistant integration**: Cover devices appear as native cover entities with full position support, and tilt-capable blinds also expose slat-angle control
- **SX1276 & SX1262 support**: Works with both radio chips — SX1276 uses hardware IoHomeOn mode, SX1262 uses software CRC

## Hardware Requirements

You need an **ESP32 board with an SX1276 or SX1262 radio module** operating at 868 MHz.

The table below lists board mappings that are known to be plausible for this component. `Confirmed` means they were tested in this repo. `Untested` means the GPIO mapping was taken from vendor documentation and still needs real IO-homecontrol validation here.

| Board | Radio | Status | `spi:` pins | `home_io_control:` pins | Notes |
|-------|-------|--------|-------------|-------------------------|-------|
| **Heltec WiFi LoRa32 v2** | SX1276 | ✅ Confirmed to work | `clk_pin: 5`, `mosi_pin: 27`, `miso_pin: 19` | `cs_pin: 18`, `rst_pin: 14`, `dio0_pin: 26` | Matches [heltec-wifi-lora-32-v2.yaml](https://github.com/laberning/home_io_control/blob/main/config/heltec-wifi-lora-32-v2.yaml), the SX1276 cover example with OLED status display |
| **Heltec WiFi LoRa32 V3 / V3.2** | SX1262 | ✅ Confirmed to work | `clk_pin: 9`, `mosi_pin: 10`, `miso_pin: 11` | `cs_pin: 8`, `rst_pin: 12`, `dio1_pin: 14`, `busy_pin: 13` | Use `radio_type: sx1262` and `tcxo_voltage: 1_8V`; matches [heltec-wifi-lora-32-v3.yaml](https://github.com/laberning/home_io_control/blob/main/config/heltec-wifi-lora-32-v3.yaml), the SX1262 cover example with OLED status display; pairing does not yet work on SX1262 |
| LilyGO T3-S3 SX1262 | SX1262 | Untested | `clk_pin: 5`, `mosi_pin: 6`, `miso_pin: 3` | `cs_pin: 7`, `rst_pin: 8`, `dio1_pin: 33`, `busy_pin: 34` | should have the same mapping on v1.2 and v1.3; start with `radio_type: sx1262` |
| LilyGO T3-S3 SX1276 | SX1276 | Untested | `clk_pin: 5`, `mosi_pin: 6`, `miso_pin: 3` | `cs_pin: 7`, `rst_pin: 8`, `dio0_pin: 9` | |
| LilyGO LoRa32 V1.3 SX1276 | SX1276 | Untested | `clk_pin: 5`, `mosi_pin: 27`, `miso_pin: 19` | `cs_pin: 18`, `rst_pin: 14`, `dio0_pin: 26` | |
| LilyGO T-Beam 1W SX1262 | SX1262 | Untested | `clk_pin: 13`, `mosi_pin: 11`, `miso_pin: 12` | `cs_pin: 15`, `rst_pin: 3`, `dio1_pin: 1`, `busy_pin: 38` | vendor docs suggest that `fem_en_pin: 40` and `fem_pa_pin: 21` might be needed |
| Any other ESP32 + SX1276/SX1262 | Either | Untested | Board-specific | Board-specific | Use the chip pinout and set the appropriate `sx1276` or `sx1262` `radio_type` |

### Confirmed Board Notes

- Heltec LoRa32 v2 is the confirmed SX1276 reference platform used during development. Pairing (discover & pair) is confirmed working on this board.
- Heltec WiFi LoRa32 V3.2 is the confirmed SX1262 platform for authenticated 2W exchanges. Pairing does not yet work on SX1262 (under investigation).
- The Heltec V4 family is closely related to V3 electrically, so it should also work, but this has not been validated yet.

## Installation

Add to your ESPHome YAML configuration:

```yaml
external_components:
  - source: github://laberning/home_io_control
```

Or for local development:

```yaml
external_components:
  - source:
      type: local
      path: components
```

## Configuration

The full configuration reference lives in [docs/home_io_control.md](docs/home_io_control.md). That page contains all component parameters, platform-specific options and the pairing workflow.

`io_device_type` accepts both named values such as `awning` and raw numeric values such as `0x11`. Pairing logs will use the named form when the schema exposes one, otherwise they will print the raw numeric type and ask you to report it upstream.

Both `esp-idf` and `arduino` framework are supported, but testing and development mostly happens on `esp-idf`.

### Simple Example

```yaml
esphome:
  name: io-homecontrol
  friendly_name: Home IO Control

esp32:
  variant: esp32

logger:
  level: DEBUG

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password

api:
  encryption:
    key: !secret api_key

ota:
  - platform: esphome
    password: !secret ota_password

external_components:
  - source: github://laberning/home_io_control

# Set the pinout for your device - this example uses Heltec WiFi LoRa32 v2.
spi:
  clk_pin: 5
  mosi_pin: 27
  miso_pin: 19

home_io_control:
  cs_pin: 18
  rst_pin: 14
  dio0_pin: 26
  # If this device was previously paired with another hub, enter that hub's 
  # Node ID and System Key below to allow the devices to reconnect automatically. 
  # Otherwise, generate new values according to the requirements below:
  # Node ID: Must be exactly 6 hexadecimal characters.
  node_id: "C0FFEE"
  # System Key: Must be exactly 32 hexadecimal characters.
  system_key: "00112233445566778899AABBCCDDEEFF"

cover:
  - platform: home_io_control
    device_class: awning
    name: "Awning"
    # If the device ID is unknown, use the "Discover & Pair" button to discover it.
    io_device_id: "FEEB1E"
    io_device_type: "awning"
    io_subtype: 0
    # Optional explicit override. If omitted, inversion follows the learned device type.
    invert_position: true
    # Optional bounded follow-up polling while movement is expected.
    status_poll_interval: 500ms

button:
  - platform: home_io_control
    name: "Discover & Pair"
```

With `io_device_type: "awning"` declared, the cover above also generates a separate Home Assistant button named `Awning Favorite Position`. Pressing it sends the protocol's built-in favorite or My-position command. The same cover also generates a diagnostic text sensor named `Awning Device Name`, disabled by default, which requests and displays the actuator's stored device name after boot when enabled. There is currently no separate sensor for reading back the stored favorite value because the protocol support for that has not been identified.

When `api:` is enabled, the hub also exposes a node-scoped Home Assistant action named `esphome.<node_name>_rename_device` with two string fields: `device_id` and `new_name`. `device_id` must be the 6-character IO-homecontrol device ID, and `new_name` must fit within the protocol's Latin-1 write limit of 15 visible characters after trimming ASCII whitespace. The action emits an `esphome.home_io_control_action_result` event with fields including `action`, `device_id`, `success`, `verified`, `message`, `requested_name`, `applied_name`, and optional `result_code` metadata when the device explicitly rejects the rename.

## Home Assistant Actions

Home IO Control currently exposes one hub-level Home Assistant action through ESPHome's native API: `esphome.<node_name>_rename_device`.

`<node_name>` comes from `esphome.name`, not `friendly_name`. Home Assistant normalizes that node name to snake case. For example, the sample V2 config uses `name: hioc-heltec-v2`, so the action becomes `esphome.hioc_heltec_v2_rename_device`.

Home IO Control enables the required native API feature flags internally, so the YAML only needs a normal `api:` block:

```yaml
api:
  encryption:
    key: !secret api_key
```

In Home Assistant Developer Tools -> Actions, use the plain action block directly:

```yaml
action: esphome.hioc_heltec_v2_rename_device
data:
  device_id: "FEEB1E"
  new_name: "Patio Awning"
```

Use the same `device_id` value that you configured as `io_device_id` in your Home IO Control `cover:`, `light:`, `lock:`, or `switch:` entry. If the device was paired through discovery first, the same 6-character ID also appears in the pairing log snippet that Home IO Control prints for the generated YAML. This is the protocol-level actuator ID, not the Home Assistant entity ID.

For automations and scripts, wrap the same block in a normal `action:` step:

```yaml
alias: Rename Patio Awning
sequence:
  - action: esphome.hioc_heltec_v2_rename_device
    data:
      device_id: "FEEB1E"
      new_name: "Patio Awning"
```

Each rename attempt emits the Home Assistant event `esphome.home_io_control_action_result`, so an automation can react to `success`, `verified`, `message`, `requested_name`, `applied_name`, and optional `result_code` fields.

For all other examples, platform-specific options, and pairing instructions, use [docs/home_io_control.md](docs/home_io_control.md).

If a device does not emit unsolicited status updates on its own, set `status_poll_interval` on the affected `cover:`, `light:`, `lock:`, or `switch:` entry. Without that option, the hub still keeps the legacy single follow-up settle poll after a local command or overheard remote activity. With the option set, it continues polling only while the device still appears to be changing, and it stops automatically once the device reports a stable state or the bounded polling window expires. The minimum supported interval is 500ms.

Explicit device refusals show up as decoded warn-level ESPHome logs. For example, a command blocked by weather can log `LIMITATION_BY_RAIN` or `LIMITATION_BY_WIND` instead of looking like a silent no-op.

Device-name support now has two surfaces: the generated diagnostic text sensor for low-noise readback, and the hub-level `esphome.<node_name>_rename_device` action for on-demand writes. Rename requests are authenticated, validated as UTF-8 text that can be represented in Latin-1, sent as fixed-size protocol payloads, and then verified through the same cached readback path used by the diagnostic sensor. If a device ignores the verification readback or reports a different final value, the rename action still reports the acknowledgement but marks the result as unverified.

## Development

### Setup and Prerequisites

The build system uses Docker for firmware compilation and host tools for testing/linting. After setup, run `make check` to verify the full toolchain.

#### Ubuntu / Debian
```bash
sudo apt-get update && sudo apt-get install -y clang-format clang-tidy yamllint libgtest-dev
# Optional: for API documentation generation
sudo apt-get install graphviz python3-pygments
```
#### macOS(Homebrew):
```bash
brew install clang-format clang-tidy yamllint googletest

# Optional: for API documentation generation
brew install graphviz python3-pygments
```

#### Windows (WSL2)

Use the Ubuntu command above inside a WSL2 distribution.

### Testing

```bash
# Run host-based Google Test unit tests (no ESP32 needed)
make unit-test

# Compile all platform configurations (firmware test)
make firmware-test

# Run all tests (unit + firmware compilation)
make test

# Full QA: lint + tests
make check
```

### Firmware Build & Flash

```bash
# Compile the firmware (SX1276 / Heltec V2)
make compile

# Compile the SX1262 validation config (Heltec V3)
make compile-v3

# Compile and flash via USB
make upload          # SX1276
make upload-v3       # SX1262

# Monitor serial output
make logs            # SX1276
make logs-v3         # SX1262

# Clean build artifacts
make clean           # SX1276
make clean-v3        # SX1262

# Format all C++ source files
make format

# Generate API documentation (requires doxygen + graphviz)
make doxygen

# Start ESPHome dashboard on port 6052
make dashboard
```

## Get Involved

The most useful contributions for this project are still hardware validation and real-world device reports. If you have an IO-homecontrol device or ESP32 LoRa board that is not yet covered here, your testing results are valuable even if the outcome is "does not work yet".

### Hardware and Device Testing

- Test unvalidated boards, radios, or device families and report whether pairing, commands, and status feedback worked.
- Confirm pin mappings for boards that are still marked as untested, or suggest corrected mappings when vendor documentation is incomplete or wrong.
- Report negative results too. A failed setup is still useful when it includes the board model, radio chip, wiring or pinout, YAML config, and logs.
- If a device only partly works, include what does work and what does not. For example: pairing succeeds but status never updates, or open and close work but stop does not.

### Reporting Unsupported Devices

If pairing discovers a device type that is not yet supported, or the generated YAML snippet is incomplete, please open a GitHub issue and include enough data to reproduce the problem.

Use this checklist when collecting logs:

1. Enable debug and frame logging in your ESPHome config:

```yaml
esphome:
  platformio_options:
    build_flags:
      - -DIOHOME_FRAME_LOG

logger:
  level: DEBUG
```

2. Trigger the action that shows the problem.
   For new devices, put the device into pairing mode, press the Discover & Pair button, and capture the log from the button press until the pairing flow finishes.
3. Include the board model, radio chip, and full pin mapping you used.
4. Include the device model or product name if you know it, and mention whether it was previously paired with another hub.
5. Include any raw values reported by the logs, especially `io_device_id`, `io_device_type`, and `io_subtype`, even if they appear as numeric values such as `0x11`.
6. Include the relevant YAML snippet you used for `spi:`, `home_io_control:`, and the affected entity if one already exists.

Open issues here: [GitHub Issues](https://github.com/laberning/home_io_control/issues).

### Pull Requests

Pull requests for fixes, tests, documentation, and targeted improvements are welcome.

For larger features, new platform support, or broader architectural changes, please open an issue first to check whether the work aligns with the current direction of the project. That helps avoid spending time on changes that are unlikely to be merged and makes it easier to agree on scope before implementation.

## Acknowledgments

This project is only possible thanks to the effort and shared knowledge from these projects and their maintainers ❤️

- **[nicolas5000/io-rts-esp32](https://github.com/nicolas5000/io-rts-esp32)** — The reference IO-Homecontrol 2W implementation that this project is based on. Provides the working protocol implementation, radio register configuration, and device communication logic.
- **[Velocet/iown-homecontrol](https://github.com/Velocet/iown-homecontrol)** — Comprehensive IO-Homecontrol protocol documentation and reverse engineering.
- **[cridp/iown-homecontrol-esp32sx1276](https://github.com/cridp/iown-homecontrol-esp32sx1276)** — A detailed SX1276 IO-Homecontrol implementation that was used for validating this project's protocol work.
- **[ESPHome](https://esphome.io/)** — The ESPHome framework.

## Disclaimer & License

> [!WARNING]
> **This tool is designed for educational and testing purposes, provided "as is", without warranty of any kind. It is forbidden in most countries to interact with IO-Homecontrol devices that are not yours.**

This project is licensed under the [MIT License](https://github.com/laberning/home_io_control/blob/main/LICENSE).

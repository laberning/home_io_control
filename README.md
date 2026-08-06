# Home IO Control
[![CI](https://github.com/laberning/home_io_control/actions/workflows/ci.yml/badge.svg)](https://github.com/laberning/home_io_control/actions/workflows/ci.yml)
[![CodeQL](https://github.com/laberning/home_io_control/actions/workflows/codeql.yml/badge.svg)](https://github.com/laberning/home_io_control/actions/workflows/codeql.yml)
[![Fuzz](https://github.com/laberning/home_io_control/actions/workflows/fuzz.yml/badge.svg)](https://github.com/laberning/home_io_control/actions/workflows/fuzz.yml)
[![Docs](https://github.com/laberning/home_io_control/actions/workflows/doxygen-deploy.yml/badge.svg)](https://github.com/laberning/home_io_control/actions/workflows/doxygen-deploy.yml)
[![GitHub](https://img.shields.io/github/license/laberning/home_io_control)](https://github.com/laberning/home_io_control/blob/main/LICENSE)
![GitHub Repo stars](https://img.shields.io/github/stars/laberning/home_io_control)

An [ESPHome](https://esphome.io/) external component for controlling IO-Homecontrol 2W devices (two-way, with device feedback). Control shutters, blinds, awnings, openers, curtains, and other IO-Homecontrol devices directly from ESPHome and Home Assistant using an ESP32 board with an SX1276, SX1262, or LR1121 radio module.

> [!NOTE]
> **Experimental Project — Use With Caution**
> This project is in early development. You may encounter edge cases where certain features do not yet function as expected.

Contributions are welcome. If you have hardware that is not listed here, an unsupported device, or validated pin mappings, see the Get Involved section below.

## Features

- **Cover control**: Open, close, stop, and set position (0–100%) for shutters, blinds, awnings, window openers, garage openers, gate openers, rolling doors, curtain tracks, and related position-based devices
- **Favorite or My position action for covers**: Covers with a declared position-capable `io_device_type` automatically get a companion Home Assistant button named `<Cover Name> Favorite Position`
- **Ventilation position action for window covers**: Covers with `io_device_type` set to `window_opener` or `ventilation_point` automatically get a companion button named `<Cover Name> Ventilation Position` that moves the actuator to its predefined ventilation opening
- **Stored device-name diagnostics**: Covers, lights, locks, and switches auto-generate a diagnostic text sensor named `<Entity Name> Device Name`, disabled by default to reduce clutter and populated from the actuator's internally stored name via a protocol read on boot
- **Hub-level device actions**: Native ESPHome API actions to rename a paired actuator, trigger its physical identify jog/flash, or request a fully-open move at elevated priority intended to override wind/rain soft locks (experimental) — all without adding persistent helper entities; see [docs/home_io_control.md](docs/home_io_control.md#home-assistant-actions)
- **Tilt support for venetian-style blinds**: Tilt-capable device types (venetian blind, external venetian blind, blind, louvre blind) expose slat-angle control automatically in Home Assistant when `io_device_type` is declared in YAML
- **Light support (binary or dimmable)**: On/off light entities for IO-Homecontrol light devices by default; opt into `dimmable: true` for brightness control on devices that support intermediate positions (hardware-confirmed on a Somfy Izymo dimmer)
- **Experimental lock support**: Lock/unlock entities for IO-Homecontrol lock devices
- **Experimental binary switch support**: On/off-only switch entities for IO-Homecontrol on/off switch devices
- **Position feedback**: Real-time position updates from devices (2-Way protocol)
- **Device discovery & pairing**: Pair new devices directly from Home Assistant via a button entity
- **Key extraction from an existing installation (hardware-confirmed protocol; not yet confirmed against a third-party hub)**: A hub-level switch that emulates an unpaired device so your *existing* IO-Homecontrol hub can pair to it and hand over its `node_id`/`system_key` — no separate sniffing hardware or device reset required; see [Key Extraction](docs/home_io_control.md#key-extraction-accept-foreign-pairing)
- **Pairing diagnostics**: Every pairing attempt is recorded to a machine-readable "Last Pairing Result" diagnostic text sensor, plus an actionable log-level advisor that turns overheard radio traffic (1W pairing traffic, channel congestion, a foreign controller pairing the same device, dead RF) into a plain-language diagnosis
- **1W remote button events**: Overheard 1W transmissions (remotes *or* wind/rain sensors — same broadcast mechanism) fire an `esphome.home_io_control_sender_event` event for opted-in sender IDs (`exposed_senders`), so you can trigger Home Assistant automations directly from a physical remote press or sensor trigger
- **Optimistic cover state**: Linked-1W-remote presses and HA-issued open/close/stop/set-position commands show the requested movement direction immediately, before the confirming poll or device response lands (`optimistic_state`, default on, per-cover opt-out); remotes can also be linked to a whole device class (`class:awning`) instead of enumerating node IDs
- **Home Assistant integration**: Cover devices appear as native cover entities with full position support, and tilt-capable blinds also expose slat-angle control
- **LR1121 radio-firmware flashing (opt-in, hardware-confirmed)**: Flash a Semtech-published transceiver firmware image onto the LR1121 directly from a Home Assistant button, no vendor tooling required — see [LR1121 Firmware Update](docs/home_io_control.md#lr1121-firmware-update)
- **SX1276, SX1262 & LR1121 support**: Works across three radio chips — SX1276 uses hardware IoHomeOn mode; SX1262 and LR1121 share a software PHY (UART framing + CRC in software), since neither has an IoHomeOn-equivalent hardware mode

## Hardware Requirements

You need an **ESP32 board with an SX1276, SX1262, or LR1121 radio module** operating at 868 MHz.

The table below lists board mappings that are known to be plausible for this component. `Confirmed` means they were tested in this repo. `Untested` means the GPIO mapping was taken from vendor documentation and still needs real IO-homecontrol validation here. `Driver implemented, untested` means chip-driver code exists and compiles for the target but has never run against real silicon — treat every timing/register value as a starting point, not a validated default, until it clears hardware bring-up.

| Board | Radio | Status | `spi:` pins | `home_io_control:` pins | Notes |
|-------|-------|--------|-------------|-------------------------|-------|
| **Heltec WiFi LoRa32 v2** | SX1276 | ✅ Confirmed to work | `clk_pin: 5`, `mosi_pin: 27`, `miso_pin: 19` | `cs_pin: 18`, `rst_pin: 14`, `dio0_pin: 26` | Use `radio_type: sx1276`; matches [heltec-wifi-lora-32-v2.yaml](https://github.com/laberning/home_io_control/blob/main/config/heltec-wifi-lora-32-v2.yaml), the SX1276 cover example with OLED status display |
| **Heltec WiFi LoRa32 V3 / V3.2** | SX1262 | ✅ Confirmed to work | `clk_pin: 9`, `mosi_pin: 10`, `miso_pin: 11` | `cs_pin: 8`, `rst_pin: 12`, `dio1_pin: 14`, `busy_pin: 13` | Use `radio_type: sx1262` and `tcxo_voltage: 1_8V`; matches [heltec-wifi-lora-32-v3.yaml](https://github.com/laberning/home_io_control/blob/main/config/heltec-wifi-lora-32-v3.yaml), the SX1262 cover example with OLED status display |
| LilyGO T3-S3 SX1262 | SX1262 | Untested | `clk_pin: 5`, `mosi_pin: 6`, `miso_pin: 3` | `cs_pin: 7`, `rst_pin: 8`, `dio1_pin: 33`, `busy_pin: 34` | should have the same mapping on v1.2 and v1.3; start with `radio_type: sx1262` |
| LilyGO T3-S3 SX1276 | SX1276 | ✅ Confirmed to work | `clk_pin: 5`, `mosi_pin: 6`, `miso_pin: 3` | `cs_pin: 7`, `rst_pin: 8`, `dio0_pin: 9` | Use `radio_type: sx1276` |
| **LilyGO T3-S3 LR1121** | LR1121 | ✅ Confirmed to work | `clk_pin: 5`, `mosi_pin: 6`, `miso_pin: 3` | `cs_pin: 7`, `rst_pin: 8`, `dio1_pin: 36`, `busy_pin: 34` | **Not the same board as the two rows above** — same T3-S3 silkscreen/form factor, but a different radio chip and a different IRQ pin (GPIO36, vs. dio0/dio1 on 9/33 for the SX1276/SX1262 variants). Use `radio_type: lr1121` and `tcxo_voltage: 3_0V`; `dio1_pin` carries the LR1121's DIO9 interrupt line. Matches [t3s3-lr1121.yaml](https://github.com/laberning/home_io_control/blob/main/config/t3s3-lr1121.yaml) |
| LilyGO LoRa32 V1.3 SX1276 | SX1276 | Untested | `clk_pin: 5`, `mosi_pin: 27`, `miso_pin: 19` | `cs_pin: 18`, `rst_pin: 14`, `dio0_pin: 26` | Use `radio_type: sx1276` |
| LilyGO T-Beam 1W SX1262 | SX1262 | Untested | `clk_pin: 13`, `mosi_pin: 11`, `miso_pin: 12` | `cs_pin: 15`, `rst_pin: 3`, `dio1_pin: 1`, `busy_pin: 38` | Use `radio_type: sx1262`; vendor docs suggest that `fem_en_pin: 40` and `fem_pa_pin: 21` might be needed |
| Any other ESP32 + SX1276/SX1262/LR1121 | Any | Untested | Board-specific | Board-specific | Use the chip pinout and set the appropriate `sx1276`, `sx1262`, or `lr1121` `radio_type` |

GPIO5 (`clk_pin` on the classic-ESP32 boards above) and GPIO3 (`miso_pin` on the ESP32-S3 T3-S3 boards) are hardware strapping pins. ESPHome refuses to reuse a strapping pin as a plain GPIO unless `ignore_strapping_warning: true` is set on that pin's expanded schema, e.g. `clk_pin: {number: 5, ignore_strapping_warning: true}` — see [heltec-wifi-lora-32-v2.yaml](https://github.com/laberning/home_io_control/blob/main/config/heltec-wifi-lora-32-v2.yaml) or [t3s3-lr1121.yaml](https://github.com/laberning/home_io_control/blob/main/config/t3s3-lr1121.yaml) for the pattern.

### Confirmed Board Notes

- Heltec LoRa32 v2 is the confirmed SX1276 reference platform used during development. Pairing (discover & pair) is confirmed working on this board.
- Heltec WiFi LoRa32 V3.2 is the confirmed SX1262 platform for authenticated 2W exchanges and pairing.
- The Heltec V4 family is closely related to V3 electrically, so it should also work, but this has not been validated yet.
- LilyGO T3-S3 LR1121 is confirmed for authenticated 2W control exchanges (open/close/stop, with real position/state feedback decoded from the device's responses) against a real Somfy Sunea IO awning motor. Pairing/discovery has not been separately validated on this board yet — the tested device was already paired via another hub.
- The LR1121's own radio firmware (separate from this ESPHome component) is field-upgradable and versioned by Semtech. On boot, the LR1121 diagnostic log block always shows the chip-reported firmware version and, when it's older than the newest version known to this codebase, an extra line pointing at the update instructions. That "known latest" version is baked into the component at build time, not queried live, so it only reflects what was true when this repo was last updated. This project **can** flash a Semtech-published transceiver firmware image directly — see [LR1121 Firmware Update](docs/home_io_control.md#lr1121-firmware-update) — up to whatever your chip's own bootloader allows; on the common `0x2100` bootloader that ceiling is firmware `1.3`, since reaching `1.4` needs a bootloader update (`0x2100` → `0x2101`) that is deliberately out of scope, consistent with this project generally reporting versions and leaving vendor-level firmware operations to vendor tooling: firmware images and changelog at [Lora-net/radio_firmware_images](https://github.com/Lora-net/radio_firmware_images/tree/master/lr1121/transceiver), and the update procedure/tool at [Lora-net/SWTL001](https://github.com/Lora-net/SWTL001).

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
  clk_pin:
    number: 5
    # GPIO5 is a strapping pin on classic ESP32; required to reuse it as a plain SPI clock pin.
    ignore_strapping_warning: true
  mosi_pin: 27
  miso_pin: 19

home_io_control:
  cs_pin: 18
  rst_pin: 14
  dio0_pin: 26
  radio_type: sx1276
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

For window-type devices (`io_device_type: "window_opener"` or `"ventilation_point"`), a second button named `<Cover Name> Ventilation Position` is generated in addition to the favorite button. That button moves the actuator to its predefined ventilation opening without fully opening the window.

The hub also exposes three node-scoped Home Assistant actions — `rename_device`, `identify_device`, and `force_open_device` — for advanced, one-off operations that stay out of the entity UI. See ["Home Assistant Actions" in docs/home_io_control.md](docs/home_io_control.md#home-assistant-actions) for what each one does, its fields, and how to trigger it.

If a device does not emit unsolicited status updates on its own, set `status_poll_interval` on the affected `cover:`, `light:`, `lock:`, or `switch:` entry. Without that option, the hub polls after commands and overheard remote activity at the device-reported settle hint (fallback 3 s) until the device reports a stable state or the bounded 10-minute window expires. With the option set, it uses min(device hint, configured interval) as the follow-up cadence — the device can shorten your configured interval but never stretch it. A STOP command always confirms the resting position within ~1 s, regardless of the configured interval. Either way the hub stops polling automatically once the device settles. The minimum supported interval is 500ms.

Explicit device refusals show up as decoded warn-level ESPHome logs. For example, a command blocked by weather can log `LIMITATION_BY_RAIN` or `LIMITATION_BY_WIND` instead of looking like a silent no-op.

Device-name support has two surfaces: the generated diagnostic text sensor for low-noise readback, and the `rename_device` action (see above) for on-demand writes, which verifies the write via a readback of its own.

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
brew install clang-format llvm yamllint googletest

# Optional: for API documentation generation
brew install graphviz pygments
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

# Validate the golden-frame corpus (schema, CRC, crypto — see tests/corpus/README.md)
make corpus-validate

# Regenerate the corpus's generated C++ fixture header (also runs automatically before unit-test)
make corpus-gen

# Clean stale build caches for config/tests/*.yaml (fixes confusing linker errors after
# adding a new .cpp under components/home_io_control/ — see AGENTS.md for details)
make clean-test-cache
```

### Firmware Build & Flash

```bash
# Compile the firmware (SX1276 / Heltec V2)
make compile

# Compile the SX1262 validation config (Heltec V3)
make compile-v3

# Compile the LR1121 config (LilyGO T3-S3 LR1121 variant)
make compile-t3

# Compile and flash via USB
make upload          # SX1276
make upload-v3       # SX1262
make upload-t3       # LR1121

# Compile, flash, and stream logs in one shot (alias for upload-*, since
# `esphome run` already does all three)
make run              # SX1276
make run-v3           # SX1262
make run-t3           # LR1121

# Monitor serial output
make logs            # SX1276
make logs-v3         # SX1262
make logs-t3         # LR1121

# Clean build artifacts
make clean           # SX1276
make clean-v3        # SX1262
make clean-t3        # LR1121

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
  build_flags:
    - -DIOHOME_FRAME_LOG

logger:
  level: DEBUG
```

2. Trigger the action that shows the problem.
   For new devices, put the device into pairing mode, press the Discover & Pair button, and capture the log from the button press until the pairing flow finishes.

   A normal `-DIOHOME_FRAME_LOG` build never exposes your real system key: `CMD_KEY_TRANSFER`
   (0x32) payloads are always masked in frame logs (`[N bytes masked]`), regardless of this flag —
   that's enforced unconditionally by `redaction.h`, not something you can accidentally disable by
   turning on frame logging. It is still good practice to avoid pasting pairing logs (commands
   `0x31`/`0x32`/`0x33`) into a public issue when you don't need to — share the specific command
   you're actually debugging instead where possible. If you want to help turn your log into a
   permanent regression fixture instead of a one-off report, see `tests/corpus/README.md` —
   command/status logs can be contributed directly, and pairing logs go through
   `ingest.py --rekey` first (that tooling is there too). If you need to capture the *raw*,
   unmasked `0x32` bytes for a re-keyed corpus contribution (the masking above means a normal
   build genuinely cannot produce them), see `IOHOME_UNSAFE_LOG_KEY_MATERIAL` in
   `log_frame.h`' — an opt-in-only build flag, never use it for a bug-report log.
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

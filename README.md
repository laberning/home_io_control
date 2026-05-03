# Home IO Control ESPHome Component

An [ESPHome](https://esphome.io/) external component that implements the **IO-Homecontrol 2W** (two-way, with device feedback) protocol. Control shutters, blinds, awnings, openers, curtains, and other IO-Homecontrol devices directly from ESPHome and Home Assistant using an ESP32 board with an SX1276 or SX1262 radio module.

> [!IMPORTANT]
> **Experimental Project — Use With Caution**
> This project is in early development. You may encounter edge cases where certain features do not yet function as expected.
>  
> I can’t verify pairing at the moment due to some pairing issues with my IO-Homecontrol devices.
> However, if you reuse the same `node_id` and `system_key` from a previously paired hub, you can bypass pairing entirely - and that path is known to function correctly.
>  
> **Get Involved**  
> Support is greatly appreciated - whether you're helping with testing devices that I don't own, give feedback or submit pull requests.

## Features

- **Cover control**: Open, close, stop, and set position (0–100%) for shutters, blinds, awnings, window openers, garage openers, gate openers, rolling doors, curtain tracks, and related position-based devices
- **Experimental binary light support**: On/off-only light entities for IO-Homecontrol light devices
- **Experimental binary switch support**: On/off-only switch entities for IO-Homecontrol on/off switch devices
- **Position feedback**: Real-time position updates from devices (2-Way protocol)
- **Device discovery & pairing**: Pair new devices directly from Home Assistant via a button entity
- **Home Assistant integration**: Cover devices appear as native cover entities with full position support
- **SX1276 & SX1262 support**: Works with both radio chips — SX1276 uses hardware IoHomeOn mode, SX1262 uses software CRC
- **Shared protocol layer**: The same high-level exchange logic runs on both radios, with chip-specific workarounds kept local to the radio boundary
- **Auto-detection**: Automatically detects which radio chip is connected, or configure explicitly

## Hardware Requirements

You need an **ESP32 board with an SX1276 or SX1262 radio module** operating at 868 MHz.

The table below lists board mappings that are known to be plausible for this component. `Confirmed` means they were tested in this repo. `Untested` means the GPIO mapping was taken from vendor documentation and still needs real IO-homecontrol validation here.

| Board | Radio | Status | `spi:` pins | `home_io_control:` pins | Notes |
|-------|-------|--------|-------------|-------------------------|-------|
| **Heltec WiFi LoRa32 v2** | SX1276 | ✅ Confirmed to work | `clk_pin: 5`, `mosi_pin: 27`, `miso_pin: 19` | `cs_pin: 18`, `rst_pin: 14`, `dio0_pin: 26` | Matches [config/heltec-wifi-lora-32-v2.yaml](config/heltec-wifi-lora-32-v2.yaml), the SX1276 cover example with OLED status display |
| **Heltec WiFi LoRa32 V3 / V3.2** | SX1262 | ✅ Confirmed to work | `clk_pin: 9`, `mosi_pin: 10`, `miso_pin: 11` | `cs_pin: 8`, `rst_pin: 12`, `dio1_pin: 14`, `busy_pin: 13` | Use `radio_type: sx1262` and `tcxo_voltage: 1_8V`; matches [config/heltec-wifi-lora-32-v3.yaml](config/heltec-wifi-lora-32-v3.yaml), the SX1262 cover example with OLED status display |
| LilyGO T3-S3 SX1262 | SX1262 | Untested | `clk_pin: 5`, `mosi_pin: 6`, `miso_pin: 3` | `cs_pin: 7`, `rst_pin: 8`, `dio1_pin: 33`, `busy_pin: 34` | should have the same mapping on v1.2 and v1.3; start with `radio_type: sx1262` |
| LilyGO T3-S3 SX1276 | SX1276 | Untested | `clk_pin: 5`, `mosi_pin: 6`, `miso_pin: 3` | `cs_pin: 7`, `rst_pin: 8`, `dio0_pin: 9` | |
| LilyGO LoRa32 V1.3 SX1276 | SX1276 | Untested | `clk_pin: 5`, `mosi_pin: 27`, `miso_pin: 19` | `cs_pin: 18`, `rst_pin: 14`, `dio0_pin: 26` | |
| LilyGO T-Beam 1W SX1262 | SX1262 | Untested | `clk_pin: 13`, `mosi_pin: 11`, `miso_pin: 12` | `cs_pin: 15`, `rst_pin: 3`, `dio1_pin: 1`, `busy_pin: 38` | vendor docs suggest that `fem_en_pin: 40` and `fem_pa_pin: 21` might be needed |
| Any other ESP32 + SX1276/SX1262 | Either | Untested | Board-specific | Board-specific | Use the chip pinout and set the appropriate `sx1276` or `sx1262` `radio_type` |

### Confirmed Board Notes

- Heltec LoRa32 v2 is the confirmed SX1276 reference platform used during development.
- Heltec WiFi LoRa32 V3.2 is the confirmed SX1262 platform used for the current authenticated 2W validation work.
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

button:
  - platform: home_io_control
    name: "Discover & Pair"
```

For all other examples, platform-specific options, and pairing instructions, use [docs/home_io_control.md](docs/home_io_control.md).

## Troubleshooting

### "SX1276 not found" on boot
The ESP32 can't communicate with the radio chip. Check your SPI pin configuration (CLK, MOSI, MISO, CS) and ensure they match your board's wiring.

### Commands sent but device doesn't respond
- The device may not be paired with your system key. Try the discovery & pairing flow.
- If the device was previously paired with a Tahoma/connectivity kit, you need to use the same system key, or factory-reset the device and pair fresh.

### "No discovery response" when pairing
- Make sure the device is in pairing mode (PROG button pressed) **before** you press Discover & Pair.
- The pairing window is short — press the HA button within a few seconds of pressing PROG.
- Ensure you're within radio range of the device.

### Position shows as "Unknown"
The device hasn't reported its position yet. Try sending an Open or Close command — the device will report its position in the response.

## Implementation Notes

The current code keeps the high-level protocol flow shared between both radios and applies the minimum number of radio-specific differences needed to make SX1262 reliable.

### SX1276 vs SX1262

- **SX1276** is the reference implementation. It uses the chip's IoHomeOn support and remains the baseline for known-good on-air behavior.
- **SX1262** uses software CRC and software UART recovery on RX. That path was validated against the SX1276 reference captures.

### SX1262 Design Decisions

- The SX1262 RX path still treats short reported packet lengths defensively and now restores the full raw probe window in that case. That larger recovery read materially improved post-auth reply reliability without changing the controller exchange flow.
- The SX1262 TX path keeps a longer preamble only for `0x3D` challenge responses. That workaround improved authenticated 2W reliability substantially, and it is intentionally scoped to SX1262 so the working SX1276 waveform stays unchanged.
- Exchange waits ignore unrelated packets instead of aborting the transaction. In practice this made 2W exchanges much more reliable because other on-air traffic can appear while waiting for a matching challenge or final response.

### Logging

- Normal operation keeps informational logs for setup, command send attempts, authentication challenges, retries/timeouts, discovery/pairing, and decoded device state updates.
- Deep packet/frame dumps stay behind debug logging or the optional `IOHOME_FRAME_LOG` build flag.
- Each failed exchange still records a compact failure summary with the last radio-capture metadata so future debugging can start from useful evidence without keeping the earlier bring-up noise.

## Development

### Setup and Prerequisites

The build system uses Docker for firmware compilation and host tools for testing/linting. After setup, run `make check` to verify the full toolchain.

#### Ubuntu / Debian
```bash
sudo apt-get update && sudo apt-get install -y clang-format clang-tidy yamllint libgtest-dev
```

#### macOS(Homebrew):
```bash
brew install clang-format clang-tidy yamllint googletest
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

# Start ESPHome dashboard on port 6052
make dashboard
```

## Acknowledgments

This project is only possible thanks to the effort and shared knowledge from these projects and their maintainers ❤️

- **[nicolas5000/io-rts-esp32](https://github.com/nicolas5000/io-rts-esp32)** — The reference IO-Homecontrol 2W implementation that this project is based on. Provides the working protocol implementation, radio register configuration, and device communication logic.
- **[Velocet/iown-homecontrol](https://github.com/Velocet/iown-homecontrol)** — Comprehensive IO-Homecontrol protocol documentation and reverse engineering.
- **[cridp/iown-homecontrol-esp32sx1276](https://github.com/cridp/iown-homecontrol-esp32sx1276)** — A detailed SX1276 IO-Homecontrol implementation that was used for validating this project's protocol work.
- **[ESPHome](https://esphome.io/)** — The ESPHome framework.

## Disclaimer & Regulatory Warning

> [!WARNING]
> **This tool is designed for educational and testing purposes, provided "as is", without warranty of any kind. It is forbidden in most countries to interact with IO-Homecontrol devices that are not yours.**

## License

This project is licensed under the [MIT License](LICENSE).

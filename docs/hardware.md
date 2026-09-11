# Hardware
<!-- doxygen-label: hardware -->

You need an **ESP32 board with an SX1276, SX1262, or LR1121 radio module** operating at 868 MHz.
This page covers which board to buy, what each one needs in YAML, and the pins the component
expects.

## Recommended board

**Heltec WiFi LoRa 32 (V3)** — sold as V3 or V3.2, either works.

Reasons why it is the easiest first build:

- It is the board this project is developed and tested on most, so support is well tested.
- It is widely stocked and inexpensive.
- The on-board OLED gives you a status display without extra wiring, see the shipped example
  config on how to drive it.
- You can get a nice case for it, which makes the whole device look very clean.

Its full pin block, with the two settings the SX1262 needs beyond the pins:

<!-- board-pinout: heltec-v3 -->
```yaml
spi:
  clk_pin: 9
  mosi_pin: 10
  miso_pin: 11

home_io_control:
  cs_pin: 8
  rst_pin: 12
  dio1_pin: 14
  busy_pin: 13
  radio_type: sx1262
  tcxo_voltage: 1_8V
```

`clk_pin`, `mosi_pin` and `miso_pin` belong to ESPHome's own `spi:` bus, not to
`home_io_control:` — the hub block takes only the pins that go to the radio chip itself. None of
the V3's pins is a strapping pin, so this board needs no `ignore_strapping_warning:` anywhere; the
boards that do are called out under [Board notes](#board-notes).

If you already own a different board from the table below, use it — nothing here is exclusive to
the V3.

## All supported boards

The table lists board mappings that are known to be plausible for this component. `Confirmed`
means they were tested in this repo. `Untested` means the GPIO mapping was taken from vendor
documentation and still needs real IO-Homecontrol validation here. `Driver implemented, untested`
means chip-driver code exists and compiles for the target but has never run against real silicon —
treat every timing/register value as a starting point, not a validated default, until it clears
hardware bring-up.

| Board | Radio | Status | `spi:` pins | `home_io_control:` pins | Notes |
|-------|-------|--------|-------------|-------------------------|-------|
| Heltec WiFi LoRa32 v2 | SX1276 | ✅ Confirmed to work | `clk_pin: 5`, `mosi_pin: 27`, `miso_pin: 19` | `cs_pin: 18`, `rst_pin: 14`, `dio0_pin: 26` | matches [heltec-wifi-lora-32-v2.yaml](https://github.com/laberning/home_io_control/blob/main/config/heltec-wifi-lora-32-v2.yaml), the SX1276 cover example with OLED status display |
| Heltec WiFi LoRa32 V3 / V3.2 | SX1262 | ✅ Confirmed to work | `clk_pin: 9`, `mosi_pin: 10`, `miso_pin: 11` | `cs_pin: 8`, `rst_pin: 12`, `dio1_pin: 14`, `busy_pin: 13` | Use `tcxo_voltage: 1_8V`; matches [heltec-wifi-lora-32-v3.yaml](https://github.com/laberning/home_io_control/blob/main/config/heltec-wifi-lora-32-v3.yaml), the SX1262 cover example with OLED status display |
| LilyGO T3-S3 SX1262 | SX1262 | Untested | `clk_pin: 5`, `mosi_pin: 6`, `miso_pin: 3` | `cs_pin: 7`, `rst_pin: 8`, `dio1_pin: 33`, `busy_pin: 34` | should have the same mapping on v1.2 and v1.3 |
| LilyGO T3-S3 SX1276 | SX1276 | ✅ Confirmed to work | `clk_pin: 5`, `mosi_pin: 6`, `miso_pin: 3` | `cs_pin: 7`, `rst_pin: 8`, `dio0_pin: 9` | |
| LilyGO T3-S3 LR1121 | LR1121 | ✅ Confirmed to work | `clk_pin: 5`, `mosi_pin: 6`, `miso_pin: 3` | `cs_pin: 7`, `rst_pin: 8`, `dio1_pin: 36`, `busy_pin: 34` | same T3-S3 silkscreen/form factor, but a different radio chip. Use `tcxo_voltage: 3_0V`; `dio1_pin` carries the LR1121's DIO9 interrupt line. Matches [t3s3-lr1121.yaml](https://github.com/laberning/home_io_control/blob/main/config/t3s3-lr1121.yaml) |
| LilyGO LoRa32 V1.3 SX1276 | SX1276 | Untested | `clk_pin: 5`, `mosi_pin: 27`, `miso_pin: 19` | `cs_pin: 18`, `rst_pin: 14`, `dio0_pin: 26` | |
| LilyGO T-Beam 1W SX1262 | SX1262 | Untested | `clk_pin: 13`, `mosi_pin: 11`, `miso_pin: 12` | `cs_pin: 15`, `rst_pin: 3`, `dio1_pin: 1`, `busy_pin: 38` | Use `fem: xy16p35`, `vfem_pin: 40` ("LDO EN"), `fem_pa_pin: 21` ("LoRa Ctrl") — per LilyGO's own T-Beam 1W SX1262 docs and schematic; see [Front-end module (FEM) support](#front-end-module-fem-support) below. TCXO control voltage not yet confirmed; do not build from this row until a board package ships |
| Heltec WiFi LoRa32 V4.2 | SX1262 | Untested | `clk_pin: 9`, `mosi_pin: 10`, `miso_pin: 11` | `cs_pin: 8`, `rst_pin: 12`, `dio1_pin: 14`, `busy_pin: 13` | Use `tcxo_voltage: 1_8V`, `fem: gc1109`; see [Front-end module (FEM) support](#front-end-module-fem-support) below; matches [heltec-wifi-lora-32-v4-2.yaml](https://github.com/laberning/home_io_control/blob/main/config/heltec-wifi-lora-32-v4-2.yaml) |
| Heltec WiFi LoRa32 V4.3 | SX1262 | Untested | `clk_pin: 9`, `mosi_pin: 10`, `miso_pin: 11` | `cs_pin: 8`, `rst_pin: 12`, `dio1_pin: 14`, `busy_pin: 13` | Use `tcxo_voltage: 1_8V`, `fem: kct8103l`; also covers the `heltec_v4_r8` variant; see [Front-end module (FEM) support](#front-end-module-fem-support) below; matches [heltec-wifi-lora-32-v4-3.yaml](https://github.com/laberning/home_io_control/blob/main/config/heltec-wifi-lora-32-v4-3.yaml) |
| Any other ESP32 + SX1276/SX1262/LR1121 | Any | Untested | Board-specific | Board-specific | Use the chip pinout and set the appropriate `sx1276`, `sx1262`, or `lr1121` `radio_type` |

GPIO5 (`clk_pin` on the classic-ESP32 boards above) and GPIO3 (`miso_pin` on the ESP32-S3 T3-S3
boards) are hardware strapping pins. ESPHome refuses to reuse a strapping pin as a plain GPIO
unless `ignore_strapping_warning: true` is set on that pin's expanded schema, e.g.
`clk_pin: {number: 5, ignore_strapping_warning: true}` — see
[heltec-wifi-lora-32-v2.yaml](https://github.com/laberning/home_io_control/blob/main/config/heltec-wifi-lora-32-v2.yaml)
or [t3s3-lr1121.yaml](https://github.com/laberning/home_io_control/blob/main/config/t3s3-lr1121.yaml)
for the pattern.

The `config/*.yaml` files linked in the table are **not standalone**: each pulls its board's `spi:`
bus and radio pins from a package under
[config/boards/](https://github.com/laberning/home_io_control/tree/main/config/boards). To reuse
one, copy the matching `config/boards/*.yaml` alongside it or inline the pins from the table above
— see [Working configs in this repo](configuration/index.md#working-configs-in-this-repo).

## Front-end module (FEM) support

Some boards add an RF front-end module (FEM) ahead of the SX1262 — a PA plus a low-noise
amplifier the earlier V2/V3 boards don't have. `fem:` selects which front-end part's *control
behaviour* the driver applies; it never supplies a pin number, so every pin your profile needs
must be set explicitly on the board, the same as every other radio pin in this schema:

- **Heltec WiFi LoRa 32 V4.2** uses a Geo-chip **GC1109**. Set `fem: gc1109`, and set
  `vfem_pin`, `fem_en_pin`, and `fem_pa_pin`.
- **Heltec WiFi LoRa 32 V4.3** (and the `heltec_v4_r8` variant) uses a Kangxi **KCT8103L**. Set
  `fem: kct8103l`, and set `vfem_pin`, `fem_en_pin`, and `fem_pa_pin`.
- **LilyGO T-Beam 1W SX1262** (868 MHz variant) uses the **XY16P35** RF module. Set
  `fem: xy16p35`, and set `vfem_pin` and `fem_pa_pin` — this part has no secondary
  chip-enable pin, so `fem_en_pin` does not apply. The profile is implemented and available today;
  its reference board package (`config/boards/t-beam-1w.yaml`) is pending a vendor answer on the
  module's TCXO control voltage.

See
[the ADR](adr/0035-fem-support-is-a-behaviour-profile-boards-always-supply-pins.md) for the full
design, including why no single static pin level works for any of these parts in both directions.

To tell the two Heltec V4 revisions apart, check what version is written on the board.

**Radiated power:** each FEM's PA turns `tx_power` into much more antenna-port power than a bare
SX1262 would produce — roughly +11 dB (GC1109) / +13 dB (KCT8103L) at low drive, uncertain by
several dB without a real measurement; roughly +14 dB (XY16P35), from LilyGO's own measured
figures, a sparser but more directly-measured evidence base than the other two.
[heltec-v4-2.yaml](https://github.com/laberning/home_io_control/blob/main/config/boards/heltec-v4-2.yaml)
and [heltec-v4-3.yaml](https://github.com/laberning/home_io_control/blob/main/config/boards/heltec-v4-3.yaml)
ship `tx_power: 1`, not the SX1262 default of `17` — raising it without a spectrum analyser risks
exceeding your region's 868 MHz SRD limit. The driver logs its own estimate (and uncertainty) at
boot, and the schema warns once `tx_power` would put the estimate over +14 dBm — above `3` on a
GC1109, above `1` on a KCT8103L, above `0` on an XY16P35.

<!-- board-pinout: heltec-v4-2 -->
```yaml
spi:
  clk_pin: 9
  mosi_pin: 10
  miso_pin: 11

home_io_control:
  cs_pin: 8
  rst_pin: 12
  dio1_pin: 14
  busy_pin: 13
  radio_type: sx1262
  tcxo_voltage: 1_8V
  fem: gc1109
  vfem_pin: 7
  fem_en_pin: 2
  fem_pa_pin:
    number: 46
    ignore_strapping_warning: true
  tx_power: 1
```

<!-- board-pinout: heltec-v4-3 -->
```yaml
spi:
  clk_pin: 9
  mosi_pin: 10
  miso_pin: 11

home_io_control:
  cs_pin: 8
  rst_pin: 12
  dio1_pin: 14
  busy_pin: 13
  radio_type: sx1262
  tcxo_voltage: 1_8V
  fem: kct8103l
  vfem_pin: 7
  fem_en_pin: 2
  fem_pa_pin: 5
  tx_power: 1
```

Neither Heltec V4 revision has been validated against real IO-Homecontrol hardware yet — see
[Board notes](#board-notes).

## Radio pin requirements

Which pins the `home_io_control:` block needs depends on the radio chip, not on the board:

| Radio | Required hub pins | Optional hub pins | Typical extra setting |
| --- | --- | --- | --- |
| SX1276 | `cs_pin`, `rst_pin`, `dio0_pin` | `dio4_pin` | `radio_type: sx1276`, `pa_pin: BOOST` |
| SX1262 | `cs_pin`, `rst_pin`, `dio1_pin`, `busy_pin` | `fem_en_pin`, `vfem_pin`, `fem_pa_pin`, `fem` | `radio_type: sx1262`, `tcxo_voltage: 1_8V` |
| LR1121 | `cs_pin`, `rst_pin`, `dio1_pin`, `busy_pin` | — | `radio_type: lr1121`, `tcxo_voltage: 3_0V` |

All three chips are validated on real devices, and `radio_type` must always be set: there is no
chip auto-detection, so a mis-wired or ambiguous chip can never be driven with the wrong SPI
command set.

## Board notes

- **Heltec LoRa32 v2** (SX1276), **Heltec WiFi LoRa32 V3/V3.2** (SX1262) and **LilyGO T3-S3 LR1121**
  (LR1121) are the boards this project is developed on.
- **Heltec V4** (V4.2/V4.3) has not been validated against real IO-Homecontrol hardware yet —
  board packages exist (`fem: gc1109` / `fem: kct8103l`, see
  [Front-end module (FEM) support](#front-end-module-fem-support) above) and the FEM switching
  logic is code-complete, but no one has run one. Prefer the V3 until a field report lands.
- **V2 / V3 clones** (non-Heltec boards with the same silkscreen and pin map) are common and
  generally work if the SPI and radio pins match the corresponding row above — treat them as
  `Untested` until confirmed.

## See also

- [Getting started](getting-started.md) — flashing the board you just picked
- [Configuration reference](configuration/index.md) — every `home_io_control:` key, including the
  pin names above
- [LR1121 firmware update](lr1121-firmware.md) — updating the radio firmware on an LR1121 board

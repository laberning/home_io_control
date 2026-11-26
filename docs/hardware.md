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
the V3's pins is a strapping pin, so this board needs no `ignore_strapping_warning:` anywhere.

If you already own a different board from the table below, use it — nothing here is exclusive to
the V3.

**The Heltec WiFi LoRa 32 V4.3 is the other board to consider.** It is validated here against real
IO-Homecontrol hardware, and its front-end module gives it noticeably better receive sensitivity
than the V3 — a measured 25 dB of extra gain, which is what you want if the device you are
controlling is far away or behind a wall. Start from
[heltec-wifi-lora-32-v4-3.yaml](https://github.com/laberning/home_io_control/blob/main/config/heltec-wifi-lora-32-v4-3.yaml),
which brings in the board package for you.

The V3 stays the recommendation for a first build for one reason: that same front-end module also
amplifies transmission, so `tx_power` on a V4.3 means something different from `tx_power`
everywhere else. Read [Front-end module (FEM) support](#front-end-module-fem-support) before you
choose it.

## All supported boards

Three tables, by how much is known about each board. Every row gives the pins for ESPHome's `spi:`
bus and for the `home_io_control:` block.

### Boards tested in this project

These boards have run against real IO-Homecontrol devices here. The Heltec V2, V3, V4.3 and the
LilyGO T3-S3 LR1121 are the boards the project is developed on.

| Board | Radio | `spi:` pins | `home_io_control:` pins | Settings and example config |
|-------|-------|-------------|-------------------------|-----------------------------|
| Heltec WiFi LoRa32 v2 | SX1276 | `clk_pin: 5`, `mosi_pin: 27`, `miso_pin: 19` | `cs_pin: 18`, `rst_pin: 14`, `dio0_pin: 26` | [heltec-wifi-lora-32-v2.yaml](https://github.com/laberning/home_io_control/blob/main/config/heltec-wifi-lora-32-v2.yaml), a cover example with OLED status display |
| Heltec WiFi LoRa32 V3 / V3.2 | SX1262 | `clk_pin: 9`, `mosi_pin: 10`, `miso_pin: 11` | `cs_pin: 8`, `rst_pin: 12`, `dio1_pin: 14`, `busy_pin: 13` | `tcxo_voltage: 1_8V`. [heltec-wifi-lora-32-v3.yaml](https://github.com/laberning/home_io_control/blob/main/config/heltec-wifi-lora-32-v3.yaml), a cover example with OLED status display |
| Heltec Wireless Stick Lite V3.1 | SX1262 | `clk_pin: 9`, `mosi_pin: 10`, `miso_pin: 11` | `cs_pin: 8`, `rst_pin: 12`, `dio1_pin: 14`, `busy_pin: 13` | `tcxo_voltage: 1_8V`. Same pins as the V3 |
| Heltec WiFi LoRa32 V4.3 | SX1262 | `clk_pin: 9`, `mosi_pin: 10`, `miso_pin: 11` | `cs_pin: 8`, `rst_pin: 12`, `dio1_pin: 14`, `busy_pin: 13` | `tcxo_voltage: 1_8V`, `fem: kct8103l` and the FEM pins; also covers the `heltec_v4_r8` variant. [heltec-wifi-lora-32-v4-3.yaml](https://github.com/laberning/home_io_control/blob/main/config/heltec-wifi-lora-32-v4-3.yaml) |
| LilyGO T3-S3 SX1276 | SX1276 | `clk_pin: 5`, `mosi_pin: 6`, `miso_pin: 3` | `cs_pin: 7`, `rst_pin: 8`, `dio0_pin: 9` | |
| LilyGO T3-S3 LR1121 | LR1121 | `clk_pin: 5`, `mosi_pin: 6`, `miso_pin: 3` | `cs_pin: 7`, `rst_pin: 8`, `dio1_pin: 36`, `busy_pin: 34` | `tcxo_voltage: 3_0V`; `dio1_pin` carries the LR1121's DIO9 interrupt line. The same silkscreen and form factor as the other T3-S3 boards, with a different radio chip. [t3s3-lr1121.yaml](https://github.com/laberning/home_io_control/blob/main/config/t3s3-lr1121.yaml) |

### Boards reported by users

Someone running the board says it works. It has not been tested here.

| Board | Radio | `spi:` pins | `home_io_control:` pins | Settings | Report |
|-------|-------|-------------|-------------------------|----------|--------|
| Heltec WiFi LoRa32 V4.2 | SX1262 | `clk_pin: 9`, `mosi_pin: 10`, `miso_pin: 11` | `cs_pin: 8`, `rst_pin: 12`, `dio1_pin: 14`, `busy_pin: 13` | `tcxo_voltage: 1_8V`, `fem: gc1109` and the FEM pins. [heltec-wifi-lora-32-v4-2.yaml](https://github.com/laberning/home_io_control/blob/main/config/heltec-wifi-lora-32-v4-2.yaml) | 1W only: a VELUX FSC solar pleated blind (issue #74) |
| Heemol LoRa V3 (a Heltec V3 clone, ESP32-S3) | SX1262 | `clk_pin: 9`, `mosi_pin: 10`, `miso_pin: 11` | `cs_pin: 8`, `rst_pin: 12`, `dio1_pin: 14`, `busy_pin: 13` | `tcxo_voltage: 1_8V`. Same pins as the Heltec V3 | Two Sauter Alutu towel dryers, two-way (issue #262) |
| Heltec Wireless Paper V1.2 | SX1262 | Not reported | `cs_pin: 8`, `rst_pin: 12`, `dio1_pin: 14`, `busy_pin: 13` | `tcxo_voltage: 1_8V` | 1W only: two VELUX roof windows (issue #249) |
| LilyGO T-ETH-Elite (ESP32-S3) + LoRa shield SX1276 | SX1276 | `clk_pin: 10`, `mosi_pin: 11`, `miso_pin: 9` | `cs_pin: 40`, `rst_pin: 46`, `dio0_pin: 8` | See the notes below | Key extraction from a VELUX KLF 200 (issue #195) |
| LilyGO LoRa32 V1.3 SX1276 | SX1276 | `clk_pin: 5`, `mosi_pin: 27`, `miso_pin: 19` | `cs_pin: 18`, `rst_pin: 14`, `dio0_pin: 26` | | |
| LilyGO T3 V1.6.1 (TTGO LoRa32 V2.1) SX1276 | SX1276 | `clk_pin: 5`, `mosi_pin: 27`, `miso_pin: 19` | `cs_pin: 18`, `rst_pin: 23`, `dio0_pin: 26` | Same pins as the Heltec V2 except `rst_pin: 23` | Three users: VELUX SSL shutters, VELUX roof windows (issue #225), Thermor radiators (issue #228) |
| LilyGO T-Beam v1.2 SX1276 | SX1276 | `clk_pin: 5`, `mosi_pin: 27`, `miso_pin: 19` | `cs_pin: 18`, `rst_pin: 23`, `dio0_pin: 26` | Same pins as the T3 V1.6.1. See the notes below | Five mains Somfy RS100 IO roller shutters, two-way ([pull request #258](https://github.com/laberning/home_io_control/pull/258)) |
| ESP32 DevKit + XL1262-P01 module (LLCC68, hand-wired) | LLCC68, as `sx1262` | `clk_pin: 18`, `mosi_pin: 23`, `miso_pin: 19` (the reporter's wiring) | `cs_pin: 26`, `rst_pin: 25`, `dio1_pin: 32`, `busy_pin: 27` | `tcxo_voltage: none`, `sx1262_rx_bandwidth: 156.2`; see the notes below | 1W only: a VELUX roller shutter (issue #177) |
| ESP32-S3-Zero + RFM95 module (hand-wired) | SX1276 | `clk_pin: 6`, `mosi_pin: 7`, `miso_pin: 8` (the reporter's wiring) | `cs_pin: 9`, `rst_pin: 10`, `dio0_pin: 11` | Default `pa_pin: BOOST`, run at 10 dBm | Five VELUX roof windows polled, one moved ([issue #242](https://github.com/laberning/home_io_control/issues/242)) |
| Custom ESP32-S3 board + DX-LR30-900M22SP module | SX1262 | `clk_pin: 12`, `mosi_pin: 11`, `miso_pin: 13` (the reporter's board) | `cs_pin: 10`, `rst_pin: 15`, `dio1_pin: 14`, `busy_pin: 9` | `tcxo_voltage: none` | A Somfy Izymo on/off receiver, following a linked Somfy io remote ([issue #57](https://github.com/laberning/home_io_control/issues/57)) |

"1W only" means the reporter drives the device one-way; two-way use of that board is not reported
yet. Notes on single boards:

- **Heltec WiFi LoRa32 V4.2:** its front-end module is a different part from the V4.3's, wired the
  other way round, so the V4.3 result does not carry over. Prefer the V3 or the V4.3 for a two-way
  setup until a report lands.
- **Heltec Wireless Paper V1.2:** an ESP32-S3 with an e-paper display. The display driver is the
  reporter's own and not part of this project.
- **LilyGO T-ETH-Elite:** an Ethernet board, with a W5500 on its own SPI bus that ESPHome's
  `ethernet:` block configures. The SD card slot shares the radio's SPI bus with chip select
  GPIO12: drive it high with a `gpio` output so the card stays off the bus. GPIO46 is a strapping
  pin, so `rst_pin` needs `ignore_strapping_warning: true`.
- **LilyGO T3 V1.6.1:** its OLED sits on I2C SDA 21 / SCL 22 with no reset pin according to
  LilyGO's pinout, so the Heltec example's display block (SDA 4 / SCL 15 / reset 16) leaves it dark
  until you change the pins.
- **LilyGO T-Beam v1.2:** the on-board power chip needs no configuration. The radio comes up
  with no `axp` block and no rail setting. The OLED header is I2C SDA 21 / SCL 22, as on the
  T3 V1.6.1.
- **XL1262-P01 module:** the LLCC68 is a reduced SX1262 that keeps the GFSK modem this project
  uses, so `radio_type: sx1262` drives it. The module has a plain crystal: set
  `tcxo_voltage: none` and widen the receive filter to 156.2 kHz with
  [`sx1262_rx_bandwidth`](configuration/tuning.md#sx1262_rx_bandwidth), without which it heard no
  remote. Keep DIO1 off GPIO12, a strapping pin. A hand-wired DIO1 needs a sound joint: with it
  broken, frames still go out but each copy runs into the TX timeout, see
  [The log shows a TX timeout](troubleshooting.md#the-log-shows-a-tx-timeout). The reporter
  recovers the key of a VELUX KLI 310 remote, enrolls a VELUX roller shutter and drives it over 1W.

### Untested pin mappings

The GPIO mapping comes from vendor documentation and still needs a run against a real
IO-Homecontrol device.

| Board | Radio | `spi:` pins | `home_io_control:` pins | Notes |
|-------|-------|-------------|-------------------------|-------|
| LilyGO T3-S3 SX1262 | SX1262 | `clk_pin: 5`, `mosi_pin: 6`, `miso_pin: 3` | `cs_pin: 7`, `rst_pin: 8`, `dio1_pin: 33`, `busy_pin: 34` | Should have the same mapping on v1.2 and v1.3 |
| LilyGO T-Beam 1W SX1262 | SX1262 | `clk_pin: 13`, `mosi_pin: 11`, `miso_pin: 12` | `cs_pin: 15`, `rst_pin: 3`, `dio1_pin: 1`, `busy_pin: 38` | `fem: xy16p35`, `vfem_pin: 40` ("LDO EN"), `fem_pa_pin: 21` ("LoRa Ctrl"), per LilyGO's own T-Beam 1W SX1262 docs and schematic. The TCXO control voltage is not yet confirmed: do not build from this row until a board package ships |
| Any other ESP32 + SX1276/SX1262/LR1121 | Any | Board-specific | Board-specific | Use the chip pinout and set the matching `radio_type` |

**V2 / V3 clones** (non-Heltec boards with the same silkscreen and pin map) are common and
generally work if the SPI and radio pins match the corresponding row above. Treat them as untested
until confirmed.

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

**Check the version printed on the board before you pick a config.** The two V4 revisions differ
only there, and a V4.3 also looks much like a V3. A wrong pick gives no error: a V4.3.1 flashed with
the V3 pin block boots normally, but its front-end module is never switched, so it sends and
listens through an unpowered amplifier. Replies go missing and listen-before-talk reads the channel
as busy (issue #196). On a V4.3, use
[heltec-v4-3.yaml](https://github.com/laberning/home_io_control/blob/main/config/boards/heltec-v4-3.yaml)
as it is.

**Radiated power: keep `tx_power: 1` on a V4 board.** Each FEM's PA turns `tx_power` into much more
antenna-port power than a bare SX1262 would produce — roughly +11 dB (GC1109) / +13 dB (KCT8103L)
at low drive, uncertain by several dB without a real measurement; roughly +14 dB (XY16P35), from
LilyGO's own measured figures, a sparser but more directly-measured evidence base than the other
two.
[heltec-v4-2.yaml](https://github.com/laberning/home_io_control/blob/main/config/boards/heltec-v4-2.yaml)
and [heltec-v4-3.yaml](https://github.com/laberning/home_io_control/blob/main/config/boards/heltec-v4-3.yaml)
ship `tx_power: 1`, not the SX1262 default of `17`. Raising it without a spectrum analyser risks
exceeding your region's 868 MHz SRD limit, whatever your radio range. The driver logs its own
estimate (and uncertainty) at boot and repeats it in the config dump that `esphome logs` requests
on connect, so you can read it without catching the boot itself. The schema warns once `tx_power`
would put the estimate over +14 dBm — above `3` on a GC1109, above `1` on a KCT8103L, above `0` on
an XY16P35.

**Received signal strength:** each FEM's low-noise amplifier raises every RSSI reading the radio
takes. The driver subtracts the amplifier's gain, so RSSI, the [Link Health](diagnostic-entities.md#link-health)
sensor and the listen-before-talk check all read at the antenna, on the same scale as a board
without a FEM. The gain is 25 dB for a KCT8103L (measured against a bare SX1262 board, about
±4 dB), 15 dB for a GC1109 (quoted by Heltec, not measured here) and 0 dB for an XY16P35, whose
gain is not published — on that board RSSI still includes the amplifier's gain, and idle-channel
readings can look busier than they are. The config dump lists the gain and where it comes from.

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

## See also

- [Getting started](getting-started.md) — flashing the board you just picked
- [Configuration reference](configuration/index.md) — every `home_io_control:` key, including the
  pin names above
- [LR1121 firmware update](lr1121-firmware.md) — updating the radio firmware on an LR1121 board

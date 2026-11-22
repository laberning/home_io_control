# Somfy pergola and patio-roof motors
<!-- doxygen-label: dev_somfy_pergola -->

The motors of a Somfy-driven pergola: the roof or screen motor, the louvers of a bioclimatic
pergola, and the side screens. The kit's LED lighting has its own page,
[Somfy White LED Receiver io](somfy-white-led-receiver.md).

## What works

| Device | Type | Status |
|---|---|---|
| Pergola io motor | `horizontal_awning` | ✅ Confirmed |
| Bioclimatic pergola louvers (TaHoma: "Pergola louver IO") | `bioclimatic_pergola` (`0x1D`) | ✅ Confirmed — position, open, close, stop |
| Sunea SCR 40 io screen motor | not reported | ✅ Confirmed — open, close, position |
| IO patio-roof motor (logs as a "Somfy IO motor", behaves as a horizontal awning) | `horizontal_awning`, 1W | ✅ Confirmed for enrollment (the roof moved on Enroll); commands afterwards not reported |

## Onboarding routes that worked

- **Pergola io motor:** [Discover & Pair](../pairing.md) after a single PROG hold on its Situo, on a
  Heltec V3 (SX1262), issue #121. For the rest of the kit see
  [Pairing a pergola kit](somfy-white-led-receiver.md#pairing-a-pergola-kit).
- **Bioclimatic pergola louvers:** [key extraction](../key-extraction.md), on a Heltec V3 (SX1262),
  pull request #157.
- **Sunea SCR 40 io:** Discover & Pair, in a pergola kit alongside a White LED Receiver io, on a
  Heltec V3 (SX1262), issue #143.
- **IO patio-roof motor:** two-way Discover & Pair got no answer. 1W enrollment worked: hold PROG on
  the existing Situo 1 until the roof jogs, then press Enroll. The entry uses
  `manufacturer: somfy`, `execute_broadcast: all` and `enrollment: true`; see
  [Sending 1W commands](../configuration/oneway-transmit.md#enrolling-this-hub-as-a-controller).
  SX1276 board, issue #147.

## Working YAML

```yaml
cover:
  - platform: home_io_control
    name: "Pergola Louvers"
    io_device_id: "…"
    io_device_type: "bioclimatic_pergola"
    io_subtype: 0

home_io_control:
  # ...
  tuning:
    sx1262_rx_bandwidth: 156.2   # SX1262 boards only
```

## Known quirks

- **The louvers need the wider receive filter on an SX1262.** At 58.6 kHz about one reply in four
  is missed; set [`sx1262_rx_bandwidth`](../configuration/tuning.md#sx1262_rx_bandwidth) to `156.2`.
  The Sunea SCR 40 io keeps working at that setting.
- **The louvers stop about 1 % off target**, and report no position after a STOP.
- **The patio-roof motor's enrollment gesture is a broadcast.** The full Somfy enrollment gesture
  goes to `00003F`.

## Evidence

- Bioclimatic pergola louvers: pull request #157, with corpus captures
  `somfy_pergola_louver_probe_get_info2_sx1262` and
  `somfy_pergola_louver_exchange_open_close_position_stop_sx1262`.
- Pergola io motor: issue #121. Sunea SCR 40 io: issue #143. IO patio-roof motor: issue #147.

## See also

- [Somfy White LED Receiver io](somfy-white-led-receiver.md) — the kit's LED lighting
- [Covers](../configuration/cover.md)

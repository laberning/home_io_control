# Atlantic Calypso Connecté water heater
<!-- doxygen-label: dev_atlantic_calypso -->

A heat-pump water heater with an electric backup element, sold by Atlantic. It speaks io-homecontrol
two-way, so the hub reads its water temperature, target, mode, what is heating and its energy, and
sets the target and the mode. Starting a boost and away mode from Home Assistant are experimental.
One 200 L unit is reported on an SX1276 board ([issue #198](https://github.com/laberning/home_io_control/issues/198)); see
[Water heaters](../configuration/water-heater.md) for the options.

## What works

| Capability | Status |
|---|---|
| Discover & Pair | ✅ Reported; the heater accepts the key push |
| Water temperature | ✅ Read in every state |
| Target temperature | ✅ Read: 54 °C and 62 °C match the display |
| Mode read-back (automatic / manual ECO on / manual ECO off) | ✅ Read; follows the display |
| Heat pump and electric element running | ✅ Read idle, with the heat pump running, and during a boost |
| Boost duration register | ✅ Reads 1 during a boost started on the display and 0 after it |
| Boost and away state | ⚠️ Experimental: read from the heater's operating-mode word, which reads "both off" at rest; not yet seen during a boost |
| Starting and ending a boost from Home Assistant | ⚠️ Experimental: sets the operating mode with a duration of 1; not yet confirmed on the heater |
| Energy since installation | ✅ Read; matches the display's "since start" total |
| Setting the target from Home Assistant | ✅ Works for the heater's own temperature steps; see [Choosing a target](#choosing-a-calypso-target) |
| Changing the mode from Home Assistant | ✅ The display follows all three modes |
| Holiday absence | ✅ Read: the heater reports no target and the heat pump stops; the card shows no target |
| Holiday days left | ✅ Read: register `0x05` counts the days still to run (`FF` for a holiday with no end date); logged at DEBUG ([issue #198](https://github.com/laberning/home_io_control/issues/198)) |
| Starting and ending the holiday mode from Home Assistant | ⚠️ Experimental: the card's away mode; runs until turned off; not yet confirmed on the heater |
| Switching the heater off | ❌ Not offered: no off state has been seen |

## Modes in Home Assistant

| Heater display | Home Assistant mode |
|---|---|
| AUTO | `heat_pump` |
| Manual, ECO on | `eco` |
| Manual, ECO off | `performance` |

The heater's manual mode heats to the target you set. In AUTO the heater chooses its own target
between 50 and 62 °C and its display shows none, so Home Assistant shows no target either. A target
you set while the heater is in AUTO is not sent; the log says so:

```text
Device F9CF0F: target ignored in automatic mode, the heater chooses its own
```

Switch to `eco` or `performance` first. The hub reads the target again right after a mode change,
so the card shows the heater's manual target within a second or two.

## Choosing a Calypso target

The heater accepts only its own fixed targets, one per number of showers on its display. Set one of
them; the heater does not take a value between two steps. The steps known for the 200 L unit:

| Showers | Target |
|---|---|
| 3 | 50 °C |
| 4 | 54 °C |
| 5 | 62 °C |

The heater's manual lists every step for each tank size. The card offers 20 to 62 °C in 1 °C steps,
so pick a listed value there.

## Starting a boost or away mode

> [!NOTE]
> Boost and away from Home Assistant are experimental: the heater has not yet been seen to follow them.

The `boost:` option creates a switch. Turn it on to start a boost and off to end it. The card's away
mode starts and ends the heater's holiday mode, which runs until you turn it off. Both are set through
the heater's operating-mode setting; turning either off ends both. The switch and the card show what
the heater reports, read back right after each change and at every poll, so they stay off if the heater
does not take the setting. The switch writes nothing when the board restarts, so a reboot does not end
a running boost. Details: [Boost and away](../configuration/water-heater.md#boost-and-away).

## Working YAML

```yaml
water_heater:
  - platform: home_io_control
    name: "Hot water"
    io_device_id: "F9CF0F"
    io_device_type: domestic_hot_water
    system_key: !secret calypso_key   # only when the heater holds a key of its own
    # Optional companions:
    energy:
      name: "Hot water energy"
    heat_pump_running:
      name: "Hot water heat pump"
    electric_backup_running:
      name: "Hot water electric backup"
    boost:                            # a switch (experimental)
      name: "Hot water boost"
```

`io_device_type: domestic_hot_water` is required on this platform. Discover & Pair and Scan Paired
Devices print a `water_heater:` block with it. The build warns that the kind is experimental: one
heater of one brand has been read so far.

## What each value is

- **Current temperature** is the water temperature at the tank's probe, in tenths of a degree.
- **Target temperature** is what the heater reports in its status reply, between 20 and 62 °C. During
  a holiday absence the heater reports a main parameter of 0, the bottom of that range, which is no
  target: the card shows none, and a target set from Home Assistant then is not sent.
- **Energy** is the energy the heater has used since it was installed, in kWh, counted up by the
  heater. It fits the Home Assistant energy dashboard.
- **Heat pump running** is on while the compressor runs, **electric backup running** while the electric
  element heats (seen during boost only).
- **Boost** and **away** are on while the heater's operating-mode word says so. Its boost and away
  duration registers are read and logged, but they do not decide the state.

The hub polls the heater every five minutes by default: one status read and six register reads,
each answered in about 10 ms. At the first poll it also reads the heater's brand letter and logs it
once:

```text
Device F9CF0F reports hot water brand 'A' (0x41)
```

## What to report

If you own this heater, these answers help:

- The full list of targets and shower counts for your tank size, from the heater's manual.
- Whether turning the boost switch on starts a boost on the display, and turning it off ends it. The
  log lines "write acknowledged with value" show what the heater did with the duration writes.
- Whether the card's away mode starts the holiday mode on the display, and what the display shows.
- Whether the boost and away state follows a boost or holiday started on the display.
- Which boost durations the display offers.
- What the heater does with a target between two of its steps.
- What the entities show with the heater switched off.
- What the entities show while a holiday absence runs.

## Evidence

Corpus captures from issue #198, on an SX1276 board:

- [Register reads in the display's states](https://github.com/laberning/home_io_control/blob/main/tests/corpus/captures/exchange/atlantic_calypso_exchange_register_read.yaml)
- [The status reads that carry the target](https://github.com/laberning/home_io_control/blob/main/tests/corpus/captures/probe/atlantic_calypso_probe_status_mp_fp.yaml)
- [The status reads that carry the operating-mode capabilities and word](https://github.com/laberning/home_io_control/blob/main/tests/corpus/captures/probe/atlantic_calypso_probe_status_fp_high.yaml)

The design is recorded in
[ADR 0068](../adr/0068-a-domestic-hot-water-device-is-a-water-heater.md) and
[ADR 0081](../adr/0081-a-water-heaters-boost-and-away-travel-in-the-operating-mode-word.md).

## See also

- [Water heaters](../configuration/water-heater.md)
- [Supported devices](../supported-devices.md)

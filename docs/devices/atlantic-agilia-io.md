# Atlantic AGILIA IO electric radiator
<!-- doxygen-label: dev_atlantic_agilia_io -->

An electric radiator with a built-in display, sold by Atlantic mainly in France. It speaks
io-homecontrol two-way, so the hub sets its temperature and mode and reads back the room temperature
and whether it is heating. Three radiators are confirmed on a Heltec V3 (SX1262) with the `climate:`
platform; see [Heating and climate](../configuration/climate.md) for every option.

## What works

| Capability | Status |
|---|---|
| Discover & Pair | ✅ Confirmed on three radiators; the hub pulls each radiator's own key |
| Target temperature | ✅ Confirmed — the display follows, and the hub reads the value back |
| Off, Heat, Auto and the `Program` preset | ✅ Confirmed — the display follows, and the hub reads the mode back |
| Room temperature | ✅ Confirmed |
| Heating / idle | ✅ Confirmed; follows a change from Home Assistant at the poll 45 seconds later, other changes at the regular poll (every 5 minutes by default) |
| Maximum setpoint | ✅ Confirmed — the radiator reports 28.0 °C |
| `Window` sensor | ✅ Confirmed — turns on when a window near the radiator is opened |
| `Open Window Detection` switch | ⚠️ Reads back the radiator's setting; switching detection off from Home Assistant is not tested yet |

## Modes in Home Assistant

The radiator has three operating modes, described in its
[installation manual](https://www.atlanticheat.co.uk/assets/uploads/general/Agilia_IO_Digital_Installation_Manual_no-lines.pdf).
Home Assistant has no separate mode for the two automatic modes, so both appear as `auto`, with a
preset that tells them apart:

| Radiator display | Home Assistant |
|---|---|
| BASIC — holds the set temperature, smart functions off | `heat` |
| AUTO — learns when the room is used and adjusts the temperature | `auto` with the `Auto` preset |
| PROG — follows the radiator's weekly program | `auto` with the `Program` preset |
| Off | `off` |

Choose `Program` or `Auto` to switch between the radiator's automatic modes; `heat` or `off` leaves
them.
The weekly program itself is edited on the radiator.

## How a radiator appears on the radio

Each radiator answers as two io-homecontrol nodes:

- The **pairing node** answers Discover & Pair. It presents itself as a remote controller (node type
  1023), refuses a key from the hub, and hands over its own key instead.
- The **heater** is an electrical heater with an adjustable setpoint (node type 52, subtype 1), and
  it is the node your `climate:` entry controls. On a radiator as delivered it sits at the next
  address up (pairing node + 1).

Each radiator has a key of its own. The hub handles both points for you: Discover & Pair pulls the
key, reads the heater at the next address under that key, and prints a block for the heater with
that key in it. If the heater does not answer that read, the hub looks for it with a roll-call
instead, which takes about 20 seconds.

## Resetting a radiator's connections

The radiator's Expert menu (hold both arrow keys for more than 3 seconds) can delete all its
connections. That gives the radiator a new key and new addresses for both nodes, and the new heater
address is unrelated to the new pairing address. Your existing `climate:` entry for it stops working: pair
the radiator again and replace the entry with the new block. The hub finds the heater with its
roll-call, so this pairing takes about 20 seconds.

## Pairing a radiator

Pair one radiator at a time. The menus are in French.

1. On the radiator, press the round button to open the menu.
2. Scroll down to **Paramètres** and press the round button.
3. Choose **Connectivité** and press the round button.
4. Choose **Connexion** and press the round button. The radiator shows an hourglass while it waits.
5. In Home Assistant, press **Discover & Pair** on the hub.

The pairing takes a few seconds, or about 15 seconds when the hub has to fall back to a roll-call to
find the heater. The radiator then reports that the device was added ("Appareil ajouté avec
succès"). The hub's log prints a
report between `DEVICE KEY RECEIVED -- DO NOT SHARE IT` and `END OF DEVICE KEY REPORT`, with a
`climate:` block for the heater that includes the radiator's `system_key:`.

1. Put the key into `secrets.yaml`, not into your main YAML.
2. Paste the block under `climate:`, replace the key with `!secret ...`, and give it a name.
3. Flash the hub.

Until you flash, the hub controls the heater with the pulled key; after a reboot it needs the
`system_key:` in your YAML.

### When no block is printed

If neither the direct read nor the roll-call reaches the heater, the log says `outcome=key_received`
and prints the key without a block for the heater. Run Discover & Pair again with the radiator in
**Connexion** mode, or put the key into a `climate:` entry's `system_key:`, flash, and press **Scan
Paired Devices**. The heater answers only some roll-call requests, so repeat the scan if it finds
nothing.

A radiator that already holds the hub's own key (for example, one you paired before by setting its
key as the hub's `system_key:`) gets a block without `system_key:`, because its heater uses the hub's
key. If the hub cannot reach the heater, the attempt ends as `hub_key_returned`: press **Scan Paired
Devices**, and again if nothing answers.

Pairing the same radiator again before you flash prints its block again. Press Discover & Pair once
per attempt: a press made while an attempt runs is ignored, with a log line saying so.

## Working YAML

```yaml
climate:
  - platform: home_io_control
    name: "Bedroom radiator"
    io_device_id: "986111"
    io_device_type: "electrical_heater"
    io_subtype: 1
    system_key: !secret bedroom_radiator_key
```

`io_device_id` is the heater's address as the printed block shows it. Give
every radiator its own entry with its own `system_key:`.

## Known quirks

- **Heating / idle trails the radiator.** The hub reads the regulator when it polls: 45 seconds after
  a change from Home Assistant, and every `status_poll_interval` otherwise. A change made at the
  radiator, or a failed poll, can leave the card behind until the next poll.
- **A radiator far from the hub misses some replies.** A radiator received at about -60 dBm lost
  several replies to the hub's challenge answer, so its Unconfirmed Exchanges counter rose and a
  setpoint change sometimes had to be repeated. A radiator at about -40 dBm had no such failures.
- **The pairing can lose the radiator's proof of its key.** The hub challenges up to three times. If the
  log still says `not proven: no 0x3D received`, press Discover & Pair again. The radiator hands over
  the same key every time.
- **The device name is generic.** The heater reports its name as `I2G_Actuator`.

## Evidence

Six corpus captures from issue #165, on a Heltec V3 with the SX1262 driver:

- [A full pairing with the key pull and the roll-call that finds the heater](https://github.com/laberning/home_io_control/blob/main/tests/corpus/captures/pairing/atlantic_agilia_pairing_key_pull_heater_rollcall.yaml)
- [A pairing of a radiator that already holds the hub's key, where the roll-call stays silent](https://github.com/laberning/home_io_control/blob/main/tests/corpus/captures/pairing/atlantic_agilia_pairing_key_pull_own_key_heater_silent.yaml)
- [A Scan Paired Devices where the heater answers once, a second after the request](https://github.com/laberning/home_io_control/blob/main/tests/corpus/captures/discovery/atlantic_agilia_discovery_heater_rollcall_late_reply.yaml)
- [The climate entity's read-back of seven registers](https://github.com/laberning/home_io_control/blob/main/tests/corpus/captures/exchange/atlantic_agilia_exchange_status_read.yaml)
- [A setpoint change and Off, each read back](https://github.com/laberning/home_io_control/blob/main/tests/corpus/captures/exchange/atlantic_agilia_exchange_setpoint_and_off_read_back.yaml)
- [Heat, Auto and Program, each read back](https://github.com/laberning/home_io_control/blob/main/tests/corpus/captures/exchange/atlantic_agilia_exchange_mode_read_back.yaml)

An earlier capture shows the radiator refusing a pushed key:
[atlantic_agilia_pairing_key_transfer_command_unknown](https://github.com/laberning/home_io_control/blob/main/tests/corpus/captures/pairing/atlantic_agilia_pairing_key_transfer_command_unknown.yaml).

## See also

- [Heating and climate](../configuration/climate.md)
- [Pairing](../pairing.md)
- [Supported devices](../supported-devices.md)

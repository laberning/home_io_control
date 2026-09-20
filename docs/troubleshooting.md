# Troubleshooting
<!-- doxygen-label: troubleshooting -->

Indexed by what you see, not by which subsystem is responsible.

## Start here

| What you see | Go to |
|---|---|
| Discover & Pair never finds the device | [The device is never found](#the-device-is-never-found) |
| Pairing succeeded, but the device will not move | [Pairing succeeds but nothing moves](#pairing-succeeds-but-nothing-moves) |
| The cover shows no position, or a stale one | [Position is unknown or state is stale](#position-is-unknown-or-state-is-stale) |
| `stop` does nothing while the device is moving | [Commands are ignored mid-motion](#commands-are-ignored-mid-motion) |
| A 1W button press does nothing | [1W commands do nothing](#1w-commands-do-nothing) |
| The log says `home_io_control is marked FAILED`, `Radio setup failed:` or `Radio failed after setup:` | [The radio does not start](#the-radio-does-not-start) |
| The boot log shows `XOSC_START_ERR` on an SX1262 board, or `HF_XOSC_START_ERR` on an LR1121 board | [`XOSC_START_ERR` at boot](#xosc_start_err-at-boot) |
| The hub resets, or works differently on another USB port or cable | [Random resets or an unreliable link on USB power](#random-resets-or-an-unreliable-link-on-usb-power) |
| Pairing works some of the time, or the link is unreliable | [A tuning plan](#a-tuning-plan) |

## The device is never found

Discover & Pair reports `no_response`, and the "Last Pairing Result" sensor shows `heard=0` or only
rejected frames.

**The most common cause is that the device already belongs to a hub.** A device holding a hub's key
has nothing left to answer a discovery with, so no amount of radio tuning will reach it. This has
been the real cause in every field report so far where a device "never responds" — GitHub issues
#27, #98 and #103.

What to try, in order:

1. **Check the advisor codes** in the "Last Pairing Result" sensor, and the WARN lines in the log.
   `1w_traffic` means a remote's PROG gesture was heard and no device answered. It says nothing
   about the device's state on its own; the advisor table lists what to check. `rf_silent` means
   nothing was heard during the attempt. It points at the antenna or the pins only if the rest of
   the log shows no IO-Homecontrol traffic at all. See
   [Pairing](pairing.md#diagnosing-a-failed-pairing-attempt).
2. **Check the gesture, and that only one device is listening.** Put the device into pairing mode
   first, then press Discover & Pair straight away. For a device without a reachable button of its
   own, hold PROG on a remote already registered to it and let go at the first jog. If that remote
   also drives other devices, power them down first.
3. **Don't treat a reset as a pairing gesture.** A Double Power Cut or factory reset only returns
   the device to its first-time setup. If it has a local remote, register that again as the manual
   describes, then repeat step 2. The exception is a VELUX SSL solar roller shutter, which pairs
   right after its motor-side reset; see
   [VELUX INTEGRA](devices/velux-integra.md#ssl-solar-roller-shutter).
4. **If a two-way hub already controls it** — a Somfy TaHoma, Connexoon or Connectivity Kit, a
   VELUX KLF200, KLR200 or KIG300 — use [key extraction](key-extraction.md). It is the route that
   works for a claimed device. A 1W-only wall switch or remote does not qualify; see
   [Key extraction](key-extraction.md) for why.
5. **Only then, tune discovery.** [Radio tuning](configuration/tuning.md) covers the parameters,
   and [A tuning plan](#a-tuning-plan) below is the order to change them in.

## Pairing succeeds but nothing moves

Pairing reported `paired`, the entity exists, and commands do nothing.

- **Check `<Name> Active Issue`.** It is enabled by default and is the fastest read. A refusal such
  as `LIMITATION_BY_WIND`, `LIMITATION_BY_RAIN` or `THERMAL_PROTECTION` means the device received
  the command and declined it — that is a device-side lockout, not a bug here.
- **Check `low_power:`.** If this is a solar or battery actuator and `low_power: true` is not set,
  the hub addresses it with the wrong preamble and the device may never wake to hear the command.
  Pairing and Scan Paired Devices pre-fill the correct value in the snippet they print; if the
  device is already registered, a scan's `hint:` line calls out a mismatch between its YAML and its
  self-reported power class.
- **Check the device type.** A command aimed at the wrong capability class is rejected. Compare
  `io_device_type` against what the pairing log reported.
- **Enable `<Name> RSSI` and `<Name> Exchange Failures`.** A poor RSSI with a climbing failure
  count is a link problem; a good RSSI with climbing failures is not.

## Position is unknown or state is stale

- **Before the first move, position is genuinely unknown.** The protocol has no "report your
  position" command that works from cold, so the cover shows no percentage until the device reports
  one. This is expected on a fresh boot, not a fault.
- **If state only updates when you command the device**, the device is not sending unsolicited
  updates. Set `status_poll_interval` on the entity — see [Covers](configuration/cover.md).
- **If a wall remote moves the device and Home Assistant does not notice**, the hub is not
  listening for that remote. Add it to `linked_remotes:` — see
  [Linked remotes](configuration/remotes.md#linked-remotes).
- [Diagnostic entities](diagnostic-entities.md) explains what each companion sensor reports.

## Commands are ignored mid-motion

`open` and `close` work, but `stop` does nothing while the device is travelling.

Some devices answer nothing the hub sends while their motor runs. A stop sent mid-travel never
arrives, and status polls sent during the move go unanswered too, so the "Exchange Failures" count
rises during every movement and the position updates only once the device has stopped. VELUX solar
products behave this way: the MSU awning screen and the SSL roller shutter. Open, close and set
position from rest work.

To stop such a device mid-travel, use its own 1W remote. The hub can send the same kind of stop
from a 1W identity once that identity is enrolled in the device; see
[Sending 1W commands](configuration/oneway-transmit.md).

## 1W commands do nothing

With no reply frames, this ladder is the diagnostic. Work down it in order.

**Nothing appears in "Last 1W Command" after a press.** The command never reached the transmitter.
Check that the button you pressed belongs to the identity you think it does (`<identity_id>_<command>`),
and look for `no controller identity` in the log.

**The sensor says `not sent (no sequence reserved)`.** The identity resolved but its counter could
not be written to flash, so nothing was built — the hub refuses to transmit a sequence it has not
durably reserved, because reusing one is unrecoverable. This is a storage failure, not a radio one.

**The sensor updates but the device does not move.** In order of likelihood:

1. **The device does not know this controller.** A 1W device obeys only source addresses it has
   been *taught* — see [Enrolling this hub as a controller](configuration/oneway-transmit.md#enrolling-this-hub-as-a-controller). This is established on real
   hardware, not inferred: devices holding the exact key the hub signed with, addressed correctly,
   did not react until the hub's own address was actually enrolled into them. **Enroll this
   identity first**, following the two-sided gesture exactly (durations matter), before working
   through the rest of this list.
2. **Key mismatch.** The identity's `system_key` no longer matches what this address enrolled with
   — most often because `system_key` was changed after enrollment, or an identity meant to reuse an
   existing network's key (see
   [Recovering a 1W controller key](configuration/oneway-transmit.md#recovering-a-1w-controller-key))
   was never actually re-enrolled
   with that key. Re-run enrollment for this identity after any `system_key` change.
3. **Wrong device class.** `io_device_type` selects the broadcast address. A shutter will not act
   on a command addressed to the awning class. Compare against the class you see in overheard 1W
   traffic from the real remote.
4. **Desynced counter.** The device remembers the highest sequence it accepted from you and rejects
   anything at or below it. If your counter fell behind — a replaced board, a restored backup — every
   command is silently dropped. **Remedy:** raise `initial_sequence:` above the value in the sensor
   and reflash. Move it in *small* steps: devices accept a forward jump only within a window
   (~1000), so overshooting fails exactly like undershooting.


## A tuning plan

If a device that should be in pairing mode does not pair with the defaults, work through the
following in order, changing one thing at a time and checking the logs after each step. This
is a general starting point, not a guarantee — different devices need different combinations.

1. **Baseline & identify the target.** Enable `ui_controls: true` and `DEBUG` logging, then press
   *Discover & Pair*. Confirm the hub transmits (`io_capture … stage=tx_frame`) and watch whether
   any discovery response (`0x29`) comes back. While the device is in pairing mode, note whether
   the *target device itself* transmits (by its own `src=` address) and to which `dst=` address —
   that address is your best clue for the following steps.

2. **Check whether the device is already claimed by another hub.** This is the most common reason
   a device stays silent even after a correct PROG gesture and a factory reset (Double Power
   Cut) — a device that already holds a hub's key has nothing left to respond to a discovery with.
   If you have (or can borrow) that hub — a Somfy TaHoma, Connexoon or Connectivity Kit, a VELUX
   KLF200 or KLR200 — use [key extraction](key-extraction.md) against it instead of tuning
   discovery parameters against the device.

3. **If the device is genuinely factory-fresh** (never claimed by any hub) and still doesn't
   respond, the combined preset sends both broadcasts, in case the device expects to see `0x2E`
   alongside `0x28`. A broadcast `0x2E` on its own has never drawn a response from any device in
   the corpus — see
   [`pairing_discovery_commands`](configuration/tuning.md#pairing_discovery_commands):
   ```yaml
   pairing_discovery_commands: ["0x28", "0x2E"]
   ```

4. **Match the address the device is active on.** If the device announces itself on a particular
   address (commonly `0x00003F`), force discovery to that exact address regardless of command,
   using an explicit destination:
   ```yaml
   pairing_discovery_commands: ["0x28"]
   pairing_discovery_destination: "0x00003F"
   ```
   …and try the reverse pairing of command and address too (`0x2E` to `0x00003B`). This decouples
   the *command* from the *address*, since a device may only answer on the specific address it is
   listening on — which is not always the command's conventional one.

5. **If the device is confirmed genuinely unpaired and still doesn't answer** any of the above,
   try shortening the discovery broadcast's preamble — see
   [`pairing_discovery_preamble`](configuration/tuning.md#pairing_discovery_preamble). A VELUX SSL
   solar roller shutter answers only at the short setting:
   ```yaml
   pairing_discovery_preamble: 32   # then 8
   ```

6. **As a last resort, try the ACK-capable flag.** Real VELUX hubs send their discovery broadcast
   with `CTRL1_ACK` set. It is off by default — read
   [`pairing_discovery_ack_capable`](configuration/tuning.md#pairing_discovery_ack_capable) before
   enabling it, and turn it back off if it doesn't help:
   ```yaml
   pairing_discovery_ack_capable: true
   ```

7. **If discovery is intermittent** (responses appear sometimes), widen the overall wait and
   initial dwell — but leave the hop slice alone or shorten it, not the other way around. Don't
   raise `sx1262_discovery_hop_slice_ms` here: the short default already reflects the measured
   optimum (see [the hop-slice parameters](configuration/tuning.md#sx1276_discovery_hop_slice_ms--sx1262_discovery_hop_slice_ms)),
   so widening it back toward a long dwell makes things worse,
   not better:
   ```yaml
   pairing_discovery_wait_ms: 3000
   pairing_discovery_initial_dwell_ms: 500
   ```

8. **If discovery succeeds but key exchange fails** (`saw_challenge=0`, or the exchange stops
   after discovery), give the receiver more margin around the turnaround (SX1262 shown; on
   LR1121 boards use the `lr1121_*` equivalents instead). Narrowing the bandwidth rejects more
   noise — see [`sx1262_rx_bandwidth`](configuration/tuning.md#sx1262_rx_bandwidth):
   ```yaml
   sx1262_post_tx_settle_us: 750    # then 1000
   sx1262_rx_bandwidth: 46.9        # then 39.0 — narrower, not wider
   sx1262_response_preamble: 12     # then 16
   ```

9. **If the logs show LBT delaying transmissions** on a quiet channel, relax LBT — but read
   [Safety and compliance](configuration/tuning.md#safety-and-compliance) before you leave a
   relaxed value in a permanent config:
   ```yaml
   lbt_max_retries: 1
   lbt_rssi_threshold_dbm: -80
   ```

After each step, record the tuning snapshot line and the outcome. When a combination works,
paste that snapshot into your permanent `tuning:` block. If a combination makes an
otherwise-unsupported device work, open an issue with it so the defaults can improve.


## The radio does not start

No device responds, and the `IO-Homecontrol:` block of the log either ends with
`home_io_control is marked FAILED: ...` or carries a `Radio setup failed:` or
`Radio failed after setup:` line. All three are part of the config dump, so you see them even when
you connected to the log after the boot.

- **`Invalid node_id or system_key configuration`**: `node_id` must be 6 hex digits and `system_key`
  32. Fix the two keys in your `home_io_control:` block.
- **A `Radio setup failed:` line** names the cause. Use the table below.
- **A `Radio failed after setup:` line** means the radio started, then stopped responding
  (typically a BUSY timeout). The component is not marked failed in that case, but nothing the
  radio does works any more. Use the same table, and reset the board.

| Reason in the log | What to check |
|---|---|
| `SX1276 not found (version register is not 0x12)` | The SPI pins and the chip select in your board package, and that `radio_type` matches the chip on the board. See [Hardware](hardware.md). |
| `chip does not identify as an LR1121` | The same checks for an LR1121 board; a board with a different chip under the same silkscreen fails here. |
| `BUSY pin stayed high` | `busy_pin` and the SPI wiring. On an SX1262 or LR1121 board, also `tcxo_voltage`: a TCXO that never starts can hold BUSY high. See [`XOSC_START_ERR` at boot](#xosc_start_err-at-boot). |
| `SX1276 never reached the requested operating mode` or `SX1276 image calibration never completed` | The chip answers on SPI but does not finish start-up. Check the power supply, the reset pin and the SPI wiring. |
| `no driver for radio_type '...'` | A pin the chip needs is missing from the `home_io_control:` block: `dio0_pin` for an SX1276, `busy_pin` and `dio1_pin` for an SX1262 or LR1121. |


## `XOSC_START_ERR` at boot

The log shows `SX1262 device errors after init: ... (XOSC_START_ERR)`,
`LR1121 device errors after init: ... (HF_XOSC_START_ERR)` or
`SX1262 TCXO started after N attempts` shortly after boot. If you connected to the log after the
boot, read the same facts from the config dump instead: the `SX1262 Diagnostic` block lists
`TCXO voltage`, `TCXO startup: N attempts` and `Init device errors (cleared after init)`. An LR1121 board lists
`HF_XOSC_START_ERR` in the same `Init device errors` line of its `LR1121 Diagnostic` block.

The radio's TCXO is not coming up on the control voltage it is being given.

- **Raise `tcxo_voltage` one step** from whatever your board config uses (e.g. `1_8V` → `2_2V`).
  On the Heltec V3 the value lives in the board package, so set `tcxo_voltage:` explicitly in
  your own `home_io_control:` block to override it. See [Hardware](hardware.md).
- `SX1262 TCXO started after N attempts` (no error) means the retry ladder recovered it — the
  radio is working, but the first startup window was marginal; raising `tcxo_voltage` one step
  removes the retry.

## Random resets or an unreliable link on USB power

Exchanges fail for no clear reason, the hub reboots in the middle of a command, or the same setup
behaves differently after you move it to another USB port or cable. Rule out the power supply
before you tune the radio: a board fed from a computer's USB port is a common cause, and it
imitates a radio fault.

Two separate things can go wrong there.

**The voltage sags while the board transmits.** Wi-Fi bursts draw a couple of hundred milliamps on
their own, and the radio's power amplifier adds roughly another hundred for as long as a frame is
on air. A long or thin cable, a front-panel port, or an unpowered hub cannot deliver those bursts
without the supply voltage dropping. The ESP32 then either resets, printing
`Brownout detector was triggered` as it comes back, or transmits below the power you configured —
which costs you range at the moment you need it.

**A computer's 5 V rail is electrically noisy.** That noise follows the cable onto the board and
raises the level the receiver hears as background, so weak replies that would otherwise decode no
longer do. It looks like a receive problem: a device answers on a clean supply and appears silent
on a PC cable, which is easy to mistake for a device or tuning fault.

What to do:

1. **Power the board from a mains USB adapter** rated 1 A or more, using a short, thick cable. Keep
   the computer's port for flashing.
2. **Read the log over the network** while you test — the ESPHome dashboard's log view, or
   `esphome logs` against the device's host name — so the board can stay on the adapter. A problem
   that disappears once the board is off the computer is a power problem, and no tuning change
   will fix it.
3. **Look for `Brownout detector was triggered`** in the log after a reset, and for a board that
   reboots at the moment it first transmits. Both name the supply directly.
4. **Lower `tx_power` by a few steps as a test.** A link that gets *more* reliable at lower
   transmit power is telling you the supply cannot sustain the transmit burst. Put it back once
   the supply is fixed. The key is described in [Configuration](configuration/index.md).
5. **On an SX1262 board, check whether [`XOSC_START_ERR`](#xosc_start_err-at-boot) also appears.**
   A marginal supply makes the TCXO's startup window marginal too, so the two often arrive
   together.

A powered USB hub, or a second supply for the board while a computer keeps only the data lines,
also works. What matters is that the transmit bursts come from something that can deliver them,
and that the board's 5 V does not come straight from a PC.

## See also

- [Radio tuning](configuration/tuning.md) — every tuning parameter in detail
- [Diagnostic probes](diagnostic-probes.md) — asking a device what it supports
- [Contributing](contributing.md) — how to report a device that still does not work

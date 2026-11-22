# Troubleshooting
<!-- doxygen-label: troubleshooting -->

Indexed by what you see, not by which subsystem is responsible.

## Start here

| What you see | Go to |
|---|---|
| Discover & Pair never finds the device | [The device is never found](#the-device-is-never-found) |
| Scan Paired Devices does not list a device you expect | [A device is missing from the scan](#a-device-is-missing-from-the-scan) |
| Pairing succeeded, but the device will not move | [Pairing succeeds but nothing moves](#pairing-succeeds-but-nothing-moves) |
| The cover shows no position, or a stale one | [Position is unknown or state is stale](#position-is-unknown-or-state-is-stale) |
| Commands from Home Assistant arrive late, or several at once | [Commands arrive late or in bursts](#commands-arrive-late-or-in-bursts) |
| `stop` does nothing while the device is moving | [Commands are ignored mid-motion](#commands-are-ignored-mid-motion) |
| A 1W button press does nothing | [1W commands do nothing](#1w-commands-do-nothing) |
| The log says `home_io_control is marked FAILED`, `Radio setup failed:`, `Radio failed after setup:` or `Radio stopped responding:`, or an Active Issue sensor shows `RADIO_STOPPED_RESPONDING` | [The radio does not start](#the-radio-does-not-start) |
| The boot log shows `XOSC_START_ERR` on an SX1262 board, or `HF_XOSC_START_ERR` on an LR1121 board | [`XOSC_START_ERR` at boot](#xosc_start_err-at-boot) |
| The hub resets, or works differently on another USB port or cable | [Random resets or an unreliable link on USB power](#random-resets-or-an-unreliable-link-on-usb-power) |
| Pairing works some of the time, or the link is unreliable | [A tuning plan](#a-tuning-plan) |

## The device is never found

Discover & Pair reports `no_response`, and the "Last Pairing Result" sensor shows `heard=0` or only
rejected frames.

Two causes account for most reports, and neither is fixed by radio tuning:

- **The device already holds a key.** A device paired to a two-way hub has nothing left to answer a
  discovery with (issues #98, #103). The key can also be this hub's own: an attempt that ended in
  `key_exchange_failed` may still have left the key on the device, which then stops answering
  discovery (issue #161). Run [Scan Paired Devices](scan-paired-devices.md) before you
  retry or reset anything.
- **The gesture missed.** The device was not in pairing mode when the hub asked, or a different
  device was. For some products the right gesture is not the obvious one: a SIMU motor paired only
  through PROG on the remote that already drives it (issue #138), and each output of a Somfy White
  LED Receiver io pairs separately from its own remote channel (issue #143).

What to try, in order:

1. **Check the advisor codes** in the "Last Pairing Result" sensor, and the WARN lines in the log.
   `1w_traffic` means a remote's PROG gesture was heard and no device answered. It says nothing
   about the device's state on its own; the advisor codes list what to check. `rf_silent` means
   nothing was heard during the attempt. It points at the antenna or the pins only if the rest of
   the log shows no IO-Homecontrol traffic at all. See
   [Pairing](pairing.md#diagnosing-a-failed-pairing-attempt).
2. **Check the gesture, and that only one device is listening.** Put the device into pairing mode
   first, then press Discover & Pair straight away. For a device without a reachable button of its
   own, hold PROG on a remote already registered to it and let go at the first jog. If that remote
   also drives other devices, power them down first. Give each attempt one PROG press — pressing it
   again while the device is still in pairing mode can close the window the first press opened.
3. **Don't treat a reset as a pairing gesture.** A Double Power Cut or factory reset only returns
   the device to its first-time setup. If it has a local remote, register that again as the manual
   describes, then repeat step 2. The exception is a VELUX SSL solar roller shutter, which pairs
   right after its motor-side reset; see
   [VELUX INTEGRA](devices/velux-integra.md#ssl-solar-roller-shutter).
4. **If a two-way hub already controls it** — a Somfy TaHoma, Connexoon or Connectivity Kit, a
   VELUX KLF 200, KLR 100, KLR 200, KLR 300 or KIG 300 — use [key extraction](key-extraction.md). It is the route that
   brings a whole claimed installation over at once. A 1W-only wall switch or remote does not
   qualify; see [Key extraction](key-extraction.md) for why. To take one claimed device over
   without involving the hub that holds it, write its entity by hand as above and pair it by
   address instead: [Pairing a device by address](pairing.md#pairing-a-device-by-address).
5. **Only then, tune discovery.** [Radio tuning](configuration/tuning.md) covers the parameters,
   and [A tuning plan](#a-tuning-plan) below is the order to change them in.

## A device is missing from the scan

Scan Paired Devices lists some devices of an installation and not the one you expect.

**Press the button again first.** Paired devices duty-cycle across the three radio channels on
their own schedule, so one scan can miss a device. A repeat press has no side effects.

**If the device never turns up, write its entity by hand.** All you need is its `io_device_id`.
Turn on frame logging:

```yaml
esphome:
  build_flags:
    - -DIOHOME_FRAME_LOG

logger:
  level: DEBUG
```

Reflash, then operate the device from its existing hub or remote while watching the log. Each
logged frame ends in its raw bytes: pairs three to five are the destination address and pairs six
to eight are the sender, so in `4B 20 30 E1 F2 C0 FF EE 03` the device is `30E1F2` and the
controller talking to it is `C0FFEE`. Your device is whichever address is not your own hub's. Then
pick `io_device_type` from [Named device types](device-types.md#named-device-types) to match
what the device actually is.

## Pairing succeeds but nothing moves

Pairing reported `paired`, the entity exists, and commands do nothing.

- **Check `<Name> Active Issue`.** It is enabled by default and is the fastest read. A refusal such
  as `LIMITATION_BY_WIND`, `LIMITATION_BY_RAIN` or `THERMAL_PROTECTION` means the device received
  the command and declined it — that is a device-side lockout, not a bug here. `BAD_MAC` is
  different: the device rejected the hub's answer to its challenge, so it does not hold the key the
  hub used. Check that the hub's `system_key`, or the entity's own `system_key` if the device has
  its own key (as each Atlantic radiator does, issue #165), is the key this device was paired with.
  `RADIO_STOPPED_RESPONDING` is not about the device: the hub's own radio stopped, see
  [The radio does not start](#the-radio-does-not-start).
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
- **If state only updates when you command the device**, the change came from something the hub
  did not hear. The hub reads a cover after its own commands, after a linked remote's press and
  after another controller's traffic to it; on a cover `status_poll_interval` only paces the polls
  during a movement. Link the remotes that drive the device (next point). For a change with no
  traffic the hub hears, such as a move from a VELUX pad (issue #207) or a window its rain sensor
  closed, set `status_refresh_interval` on the cover so the hub reads it at rest — see
  [Refreshing the status at rest](configuration/cover.md#refreshing-the-status-at-rest).
- **If a wall remote moves the device and Home Assistant does not notice**, the hub is not
  listening for that remote. Add it to `linked_remotes:` — see
  [Linked remotes](configuration/remotes.md#linked-remotes).
- **If a linked remote's press is sometimes not noticed**, it probably arrived while the hub was
  polling after an earlier press — see
  [When a press is missed](configuration/remotes.md#when-a-press-is-missed).
- [Diagnostic entities](diagnostic-entities.md) explains what each companion sensor reports.

## Commands arrive late or in bursts

A command from Home Assistant reaches the hub late, or several identical commands arrive within a
few milliseconds: the log shows the same line, such as `optimistic stop`, several times in a row.
The usual cause is the ESP32's Wi-Fi power saving holding back network traffic, not the radio. Turn
it off in your YAML:

```yaml
wifi:
  power_save_mode: none
```

With that, every command on a reporter's Heltec V4.3 went on air within about 50 ms of arriving
([issue #196](https://github.com/laberning/home_io_control/issues/196)).

## Commands are ignored mid-motion

`open` and `close` work, but `stop` does nothing while the device is travelling.

A `low_power:` device that is travelling is awake: it answers only the short start preamble
and ignores the long wake-up preamble that reaches it at rest. The hub leads with the short
preamble while it believes the device is awake
([`low_power_wake_belief`](configuration/tuning.md#low_power_wake_belief),
[ADR 0040](adr/0040-low-power-start-preamble-follows-the-wake-belief.md)).

- Leave `low_power_wake_belief` on; the boot log states it (`Low-power wake belief: on`). With it
  off, every `stop` goes out with the wake-up preamble and none lands mid-travel.
- A mid-travel `stop` does not land on every attempt, so press stop again if the device keeps
  moving. This is how a VELUX SSL solar roller shutter stops mid-travel.
- The VELUX MSU solar awning screen has no confirmed mid-travel stop yet; stop it with its own
  remote (see [VELUX INTEGRA](devices/velux-integra.md)).

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
2. **Key mismatch.** The identity's `system_key` differs from the one this address enrolled with
   — most often because `system_key` was changed after enrollment, or an identity meant to reuse an
   existing network's key (see
   [Recovering a 1W controller key](configuration/oneway-transmit.md#recovering-a-1w-controller-key))
   was never actually re-enrolled
   with that key. Re-run enrollment for this identity after any `system_key` change.
3. **Wrong device class.** `io_device_type` selects the broadcast address. A shutter will not act
   on a command addressed to the awning class. Compare against the class you see in overheard 1W
   traffic from the real remote. With `execute_broadcast: all` the command carries no class, so
   this step cannot be the cause; go to step 7.
4. **Desynced counter.** The device remembers the highest sequence it accepted from you and rejects
   anything at or below it. If your counter fell behind — a replaced board, a restored backup — every
   command is silently dropped. **Remedy:** call the
   [`oneway_set_sequence`](configuration/actions.md#oneway_set_sequence) action with a `sequence`
   above the last number the device obeyed, then press the identity's button; no reflash is needed.
   That number is in the sensor, or, after a board replacement, in the sensor's history in Home
   Assistant. The action refuses a number that is not above the current one unless you pass
   `allow_lower: "true"`; do that only to undo a step that went too far, because a device that is
   in step ignores a lower number until the counter passes its old one again.
   `initial_sequence:` is the YAML form of the same step and needs a reflash; at boot a value at or
   below the counter the hub already holds, or 32768 or more above it, changes nothing. A bench run on a
   Somfy Izymo showed that it obeys any higher number, compared as a
   plain number (jumps up to 32000 tested), so a large forward step is fine there; other receivers
   are not measured, so prefer small steps. The same run showed that the Izymo ignores every step
   across the 16-bit wrap (65535 to 0), so an identity whose counter wraps is lost to that device:
   give it a new address (a new `node_id:`, or a new `id:` when the address is derived) and enroll
   it again ([ADR 0120](adr/0120-a-one-way-counter-that-wraps-is-lost-to-a-somfy-receiver.md)); the
   action cannot bring a wrapped identity back.
   Pressing Enroll again for the same identity is not a remedy: a bench run on a Somfy Izymo showed
   that it takes an already-enrolled identity off
   ([details](configuration/oneway-transmit.md#enrolling-this-hub-as-a-controller)).
5. **Wrong burst shape.** A solar or battery receiver sleeps between listening windows and needs
   the long wake-up copy: set `low_power: true`. Unset `low_power:` sends four short copies for every manufacturer
   ([ADR 0122](adr/0122-an-unset-one-way-low-power-sends-four-short-copies.md)), which a mains-powered receiver obeys and a sleeping one misses. If a battery or solar device stopped obeying after an update and its identity leaves `low_power:` unset, set `low_power: true`: before this default an unset key gave every manufacturer but VELUX the wake-up copy. An awake VELUX receiver ignores a frame that
   arrives behind the long wake-up, so a mains-powered one keeps `low_power:` unset or `false`. The TX line in the log lists the preamble of each copy in milliseconds, for
   example `preamble 515/10/10/10 ms` for the wake-up on the first copy only, and the boot log names
   the shape the identity uses. The wake-up length of a `low_power: true` identity is
   [`low_power_wake_ms`](configuration/tuning.md#low_power_wake_ms). How long a burst takes
   per shape is in [Sending 1W commands](configuration/oneway-transmit.md#what-one-command-puts-on-the-air).
6. **Wrong manufacturer profile.** `manufacturer:` selects the priority byte of every open, close
   and stop frame: `0x43` for Somfy, `0x61` for VELUX. Press your real remote with DEBUG logging
   on: its `rx 1W remote ... EXECUTE(0x00)` line ends in `priority=user_high` for `0x43` and
   `priority=user_default` for `0x61`. Compare that with the `acei` the boot log prints for the
   identity. If they differ, fix `manufacturer:`, or set `execute_acei:` for a vendor this
   component has no profile for.
7. **Class address or all-devices address.** A handheld remote sends open, close and stop to the
   all-devices address. If your real remote's `EXECUTE(0x00)` lines show `dst-class=all`, set
   `execute_broadcast: all` on the identity; with the default `typed` the hub sends to the class of
   `io_device_type` instead.
8. **Open and Close swapped.** Some kinds move the other way round on the wire, for example a
   horizontal awning. The boot log shows `inverted=yes (from kind)`, `inverted=yes (configured)` or
   `inverted=no` for each identity. Set `invert_position: true` or `false` to match what the
   device does.


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
   change the discovery broadcast's preamble — see
   [`pairing_discovery_preamble_ms`](configuration/tuning.md#pairing_discovery_preamble_ms). Discovery
   goes out behind the short start preamble by default, which VELUX shutters answer. A device that
   may be a duty-cycled receiver can need the long wake-up burst instead:
   ```yaml
   pairing_discovery_preamble_ms: 300   # or go shorter: 2
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
   LR1121 boards use the `lr1121_*` equivalents of the radio-specific keys instead). If the device's replies are missed, widen
   the receive filter; only a noisy install gains from narrowing it. See
   [`sx1262_rx_bandwidth`](configuration/tuning.md#sx1262_rx_bandwidth):
   ```yaml
   sx1262_post_tx_settle_us: 750    # then 1000
   sx1262_rx_bandwidth: 156.2       # then 187.2; 78.2 or 58.6 only to reject noise
   response_preamble_ms: 5          # then 7
   ```

   A longer response preamble also delays the hub's answer to the device's challenge. Some
   devices reject a late answer: a Somfy MOTO 1000 io garage drive ignored all six test commands
   behind a response preamble of about 6.7 ms (32 bytes on its SX1262 board) ([issue #163](https://github.com/laberning/home_io_control/issues/163)).
   If commands stop working after you raise it, set it back.

9. **If the logs show `channel busy` on a quiet channel** (`Try N ended: channel busy`), the hub is
   reading noise as traffic. It learns the limit from your board's own quiet level, and warns when
   a channel idles close to the highest limit it uses. Fix the limit a little above the level the
   debug log names, but read
   [Safety and compliance](configuration/tuning.md#safety-and-compliance) before you leave a
   relaxed value in a permanent config:
   ```yaml
   lbt_rssi_threshold_dbm: -80
   ```
   The hub waits for a free channel and never sends onto a busy one; how long it waits and what
   `lbt_force_when_busy` changes is described under
   [Channel access](configuration/tuning.md#lbt_rssi_threshold_dbm--lbt_force_when_busy).

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
  (typically a BUSY timeout). A short Wi-Fi stall or roam scan does not cause this, because the
  radio wait is judged on a fresh reading once the stall is over. A line like this therefore points
  at the chip, its wiring or its power. The component is not marked failed in that case, but
  nothing the radio does works any more. When it happens the log also shows a one-time
  `Radio stopped responding: <reason>; reboot the board` error, and the component sets ESPHome's
  warning status, which makes a configured `status_led` blink. In Home Assistant, every device's
  Active Issue sensor shows `RADIO_STOPPED_RESPONDING` until the board is rebooted. Use the same
  table, and reset the board.

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

## The log shows a TX timeout

A command takes a second or more, a STOP arrives late, and the log shows
`TX timeout after N ms (air time N us) — DIO/IRQ pin never fired`. The radio did not report the
end of a transmission within the frame's own air time plus a quarter of a second, so the hub gave
that copy up. The frame may still have gone out: a cover can move even though every copy logs a
timeout.

On an SX1262 or LR1121 board the next line, `TX timeout chip state: ... (mode=...), tx_done=...`,
tells you where to look:

- **`mode=STDBY_XOSC` and `tx_done=1`**: the transmission finished, but its interrupt never reached
  the ESP32. Check the `dio1_pin` wiring and solder joint (`DIO1=` shows the pin's level), and that
  the pin is not a strapping pin such as GPIO12.
- **`mode=STDBY_XOSC` and `tx_done=0`**: the radio never started the transmission. Post the log in
  an issue.
- **`mode=STDBY_RC`**: the chip reset during the transmission and lost its settings. The hub
  configures it again and keeps working; the next line says
  `reset during the transmission and lost its configuration`. A reset under the transmit current
  points at the power supply, see the next section.
- **`mode=TX`**: the chip is still transmitting long after the frame should have ended. Post the
  log in an issue.
- **A named device error** such as `PLL_LOCK_ERR` or `PA_RAMP_ERR`: the chip could not lock its
  frequency or ramp its amplifier. Check `tcxo_voltage` and the supply.

On an SX1276 board the line itself carries the chip state:
`TX timeout after N ms — DIO0 never fired (RegOpMode 0x.., RegIrqFlags1 0x.., RegIrqFlags2 0x..)`.

- **`RegOpMode 0x09`**: the radio reset during the transmission and lost its settings. The hub
  configures it again and keeps working, as on the other chips; a reset under the transmit current
  points at the power supply, see the next section.
- **`RegIrqFlags2` has bit `0x08` (PacketSent) set**: the transmission finished, but DIO0 never
  reached the ESP32. Check the `dio0_pin` wiring.
- **`RegIrqFlags2` has bit `0x40` (FifoEmpty) clear**: part of the frame never left the radio.
  `RegIrqFlags1` shows whether the radio was ready to send: bit `0x20` (TxReady) and bit `0x10`
  (PllLock) should both be set. If either is clear, the radio could not start the transmission,
  which points at the supply. Post the log in an issue either way.

Whatever the radio, the hub goes back to receiving after a timeout, so one timeout costs that copy
and nothing more.

Lowering `tx_power` by a few steps is a quick test: if the timeouts stop, the supply cannot deliver
the transmit burst.

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

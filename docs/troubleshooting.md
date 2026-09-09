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
   `1w_traffic` means the motor is not in 2W learning mode. `rf_silent` means nothing at all was
   heard, which points at the antenna or the pins rather than at the device. See
   [Pairing](pairing.md#diagnosing-a-failed-pairing-attempt).
2. **Check your timing.** Press the device's PROG button first, then Discover & Pair within two or
   three seconds. The device's pairing window is short.
3. **Try a Double Power Cut** to force the motor into 2W learning mode, then retry.
4. **If you own the hub that has it** — a Somfy TaHoma, Connexoon or Connectivity Kit, a VELUX
   KLF200, KLR200 or KIG300 — use [key extraction](key-extraction.md). It is the route that works
   for a claimed device.
5. **Only then, tune discovery.** [Radio tuning](configuration/tuning.md) covers the parameters,
   and [A tuning plan](#a-tuning-plan) below is the order to change them in.

## Pairing succeeds but nothing moves

Pairing reported `paired`, the entity exists, and commands do nothing.

- **Check `<Name> Active Issue`.** It is enabled by default and is the fastest read. A refusal such
  as `LIMITATION_BY_WIND`, `LIMITATION_BY_RAIN` or `THERMAL_PROTECTION` means the device received
  the command and declined it — that is a device-side lockout, not a bug here.
- **Check `low_power:`.** If this is a solar or battery actuator and `low_power: true` is not set,
  the hub addresses it with the wrong preamble and the device may never wake to hear the command.
  This was the cause in issue #87. Pairing and Scan Paired Devices pre-fill the correct value in
  the snippet they print.
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

Some devices will not complete a 2W handshake while they are moving. The hub has nothing to talk
to, so the stop is never delivered. This is confirmed on a VELUX MSU solar awning screen in issue
#95, where open and close both work and only `stop` fails.

There is no complete fix from the hub side for such a device. If the device also listens for a 1W
remote, [sending a 1W STOP](configuration/oneway-transmit.md) is the route that reaches it,
provided this hub is enrolled as a controller first.

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
   [`pairing_discovery_preamble`](configuration/tuning.md#pairing_discovery_preamble).
   Unconfirmed as a fix for any specific device; worth reporting back either way:
   ```yaml
   pairing_discovery_preamble: 32   # then 8
   ```

6. **If discovery is intermittent** (responses appear sometimes), widen the overall wait and
   initial dwell — but leave the hop slice alone or shorten it, not the other way around. Don't
   raise `sx1262_discovery_hop_slice_ms` here: the short default already reflects the measured
   optimum (see [the hop-slice parameters](configuration/tuning.md#sx1276_discovery_hop_slice_ms--sx1262_discovery_hop_slice_ms)),
   so widening it back toward a long dwell makes things worse,
   not better:
   ```yaml
   pairing_discovery_wait_ms: 3000
   pairing_discovery_initial_dwell_ms: 500
   ```

7. **If discovery succeeds but key exchange fails** (`saw_challenge=0`, or the exchange stops
   after discovery), give the receiver more margin around the turnaround (SX1262 shown; on
   LR1121 boards use the `lr1121_*` equivalents instead). Narrowing the bandwidth rejects more
   noise — see [`sx1262_rx_bandwidth`](configuration/tuning.md#sx1262_rx_bandwidth):
   ```yaml
   sx1262_post_tx_settle_us: 750    # then 1000
   sx1262_rx_bandwidth: 46.9        # then 39.0 — narrower, not wider
   sx1262_response_preamble: 12     # then 16
   ```

8. **If the logs show LBT delaying transmissions** on a quiet channel, relax LBT — but read
   [Safety and compliance](configuration/tuning.md#safety-and-compliance) before you leave a
   relaxed value in a permanent config:
   ```yaml
   lbt_max_retries: 1
   lbt_rssi_threshold_dbm: -80
   ```

After each step, record the tuning snapshot line and the outcome. When a combination works,
paste that snapshot into your permanent `tuning:` block. If a combination makes an
otherwise-unsupported device work, open an issue with it so the defaults can improve.


## See also

- [Radio tuning](configuration/tuning.md) — every tuning parameter in detail
- [Diagnostic probes](diagnostic-probes.md) — asking a device what it supports
- [Contributing](contributing.md) — how to report a device that still does not work

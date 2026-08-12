# Radio Diagnostics Tuning

This project is still experimental. IO-Homecontrol covers a wide range of motors and
actuators, and every model can behave slightly differently during pairing — different
discovery commands, timings, or radio settings. It is not possible to test against every
device that exists in the field, so getting pairing to work reliably on a *specific* device
sometimes needs a bit of per-device fine-tuning.

The `tuning:` block exists for exactly that. It exposes the pairing and radio parameters that
are normally fixed, so you can experiment on a stubborn device and find a working combination
— then copy that combination back into your configuration.

> [!NOTE]
> **Use these carefully.** They change real RF and protocol behaviour. Aggressive or
> mismatched values can cause failed or partial pairing, leave a device in an unexpected
> state, or produce transmissions the device rejects. Change one thing at a time, watch the
> logs, and revert anything that does not help. When you are done, keep only the values that
> made a difference in your configuration.

## Enabling the tuning UI

Add a `tuning:` block under `home_io_control:` and set `ui_controls: true`:

```yaml
home_io_control:
  node_id: "C0FFEE"
  system_key: "..."

  tuning:
    ui_controls: true
```

`ui_controls: true` exposes every parameter as a Home Assistant `number` or `select` entity,
so you can **change values live from the UI without reflashing** for each experiment. The
entities appear under the device's **Configuration** section, grouped into `Radio …` and
`Pairing …` names.

> [!NOTE]
> **Tuned values are not persisted.** Every value resets to its default on reboot. Once you
> find a combination that works, copy it from the log snapshot into your YAML `tuning:` block
> so it survives a restart. You can also set any parameter directly in YAML (without
> `ui_controls`) once you know what you want.

## How to experiment

1. **Turn on verbose logging.** Set the logger to `DEBUG` so you can see the pairing exchange
   and the raw packet capture:

   ```yaml
   logger:
     level: DEBUG
     # Or keep the global level higher and raise only these tags:
     # logs:
     #   home_io_control: DEBUG
     #   home_io_control.exchange: DEBUG
     #   io_capture: DEBUG          # per-frame TX/RX packet capture
   ```

2. **Change one parameter at a time.** With several changes at once you cannot tell which one
   mattered. Adjust a single value, trigger *Discover & Pair*, and read the result.

3. **Watch the logs for impact.** Useful signals:
   - `io_capture … stage=tx_frame …` — a frame the hub transmitted.
   - a discovery response (`0x29`) — the device heard a discovery command.
   - `saw_challenge=1` in the exchange summary — the device answered the key-exchange start.
   - the `Exchange failed … stage=…` line — where in the flow it stopped.
   - the `src=` and `dst=` of *incoming* frames — a device in pairing mode announces itself and
     reveals the **address it is active on** (often `0x00003F`); that address is a strong hint
     for where to aim discovery. Be careful to confirm the `src=` is the device you are pairing:
     remotes and other devices share the air and may look like the target. Disconnecting other
     controllers during the test removes that noise.

4. **Identify the target's own address first.** Before tuning, operate the device (or put it in
   pairing mode) and watch which `src=` address *it* transmits from and which `dst=` it targets.
   If the device you want never transmits at all, no discovery command will reach it — the issue
   is the device's pairing mode, not a tuning value.

5. **Expect a brief block during each attempt.** A pairing attempt runs to completion in one go,
   so a `took a long time for an operation (… ms)` warning is normal and harmless. Its duration
   grows with `pairing_discovery_wait_ms` × the number of commands × the fixed per-command
   retries, so keep `pairing_discovery_wait_ms` modest when trying several commands at once.

6. **Save your logs.** Keep a copy of the logs for each experiment (for example
   `esphome logs … > pairing-attempt-01.log`). Together with the one-line tuning snapshot the
   component prints, this lets you compare attempts later and reconstruct exactly which change
   produced which result.

At boot and at the start of every pairing attempt, the component logs the active overrides:

```
[home_io_control] Tuning overrides active: pairing_discovery_commands=[0x28,0x2E] sx1262_post_tx_settle_us=750
```

or, when nothing is overridden:

```
[home_io_control] Tuning: defaults active
```

Each UI change is also logged in YAML-compatible form, ready to paste back:

```
[home_io_control] Tuning updated via HA: sx1262_post_tx_settle_us=750
```

## Parameter reference

Parameters fall into two groups: the **radio / physical layer** (how the chip transmits and
receives) and the **pairing protocol** (which frames are sent and how the hub waits for
answers). The *Radio* column shows which chip a parameter affects — a chip-specific parameter
(*SX1262*, *SX1276*, or *LR1121*) is ignored on any other chip's boards; *both* applies
everywhere.

Each parameter below has a short **What we've seen** note. These summarise concrete behaviour
observed while getting pairing to work — treat them as starting hints, not guarantees, because
your device may differ.

> [!NOTE]
> **Background — channels & hopping.** The protocol uses three 868 MHz channels
> (≈868.25 / 868.95 / 869.85 MHz). A device frequently *answers on a different channel* than
> the one the request went out on, so the hub hops across all three while waiting and lingers
> on any channel where it detects an incoming preamble. Several parameters below tune that
> wait — but if a device is never heard at all, the cause is usually the command/address, not
> the timing.

### Quick reference

| Parameter | Radio | Default | Range / options | What it does |
|---|---|---|---|---|
| `sx1262_rx_bandwidth` | SX1262 | `117.3` | `58.6` / `78.2` / `117.3` / `156.2` / `187.2` (kHz) | Receiver bandwidth; wider tolerates post-TX frequency offset. |
| `sx1262_response_preamble` | SX1262 | `8` | 8–256 B | Preamble length on reply frames, for the peer to lock on. |
| `sx1262_post_tx_settle_us` | SX1262 | `500` | 0–2000 µs | Settling delay after TX before switching back to RX. |
| `sx1276_rx_bandwidth` | SX1276 | `41.7` | `20.8` / `41.7` / `62.5` / `83.3` / `125.0` (kHz) | Receiver bandwidth; wider tolerates LO offset, narrower rejects more noise. |
| `sx1276_response_preamble` | SX1276 | `12` | 8–256 B | Preamble length on reply frames, for the peer to lock on. |
| `sx1276_discovery_hop_slice_ms` | SX1276 | `5` | 5–200 ms | Per-channel dwell while hopping during discovery. |
| `sx1262_discovery_hop_slice_ms` | SX1262 | `200` | 50–500 ms | Per-channel dwell while hopping during discovery. |
| `lr1121_rx_bandwidth` | LR1121 | `117.3` | `39.0` / `46.9` / `58.6` / `78.2` / `117.3` / `156.2` / `187.2` (kHz) | Receiver bandwidth; wider tolerates post-TX frequency offset. |
| `lr1121_response_preamble` | LR1121 | `8` | 8–256 B | Preamble length on reply frames, for the peer to lock on. |
| `lr1121_post_tx_settle_us` | LR1121 | `500` | 0–2000 µs | Settling delay after TX before switching back to RX. |
| `lr1121_discovery_hop_slice_ms` | LR1121 | `200` | 50–500 ms | Per-channel dwell while hopping during discovery. |
| `lbt_max_retries` | both | `5` | 0–10 | Listen-before-talk carrier-sense attempts before TX. |
| `lbt_rssi_threshold_dbm` | both | `-90` | -95 to -70 dBm | RSSI below which the channel counts as free. |
| `pairing_discovery_commands` | both | `["0x28"]` | ordered list of `0x28` / `0x2E` | Which discovery command(s) to send, and in what order. |
| `pairing_discovery_destination` | both | `auto` | `auto` / `0x00003B` / `0x00003F` | Address the discovery frames are sent to. |
| `pairing_discovery_payload` | both | `none` | `none` / `0x00` | Optional payload byte (used by the alternate command). |
| `pairing_discovery_low_power` | both | `false` | `true` / `false` | Sets the LOW_POWER flag in discovery frames. |
| `pairing_discovery_wait_ms` | both | `2000` | 500–5000 ms | How long to wait for a response after each discovery TX. Also the per-attempt listen window for each of the three roll-call attempts the `scan_paired_devices` action makes. |
| `pairing_discovery_initial_dwell_ms` | both | `300` | 0–500 ms | Settle delay before the first discovery TX. |
| `pairing_key_exchange_retries` | both | `3` | 1–5 | Retries for the authenticated key-exchange phase. |

`ui_controls` itself is a feature toggle (default `false`) that exposes these as entities; it is
not a tunable.

### Radio / physical layer — in detail

#### `sx1262_rx_bandwidth`

GFSK receiver bandwidth on the SX1262. Change it when discovery or key-exchange replies fail to
decode cleanly on an SX1262 board.

*Observations:* the SX1262's local oscillator needs frequency headroom to recover after a
TX→RX turnaround. Experiments here found that at `58.6` kHz the demodulator corrupted roughly
half the bytes of a post-transmit frame (pairing succeeded only about one attempt in five);
widening to `117.3` kHz removed the problem entirely. Going wider still (`156.2`/`187.2`) can
help when a device's transmitter drifts more than the controller's own radio, at the cost of
more noise. `58.6` is close to the narrow default used on the older SX1276 chip (≈41.7 kHz), but
is marginal on the SX1262's tight turnaround.

#### `sx1262_response_preamble`

How long a preamble the SX1262 puts in front of its *reply* frames (the key-transfer and
authentication responses), giving the peer time to lock on after the hub transmits. Raise it
when key exchange stalls right after discovery on an SX1262 board, especially with a device that
has never been paired before.

*Observations:* the SX1262's modulation is marginal for short preambles on the fast post-TX
turnaround. Already-paired devices lock onto an on-air preamble of 8 bytes reliably. Brand-new
devices often fail to decode on-air preambles of 1/4/8 bytes and need something substantially
longer — exactly how much, up to the protocol's full 1024-byte long preamble, has not yet been
independently validated on this chip, so treat any specific number above 8 as untested rather
than known-good.

#### `sx1262_post_tx_settle_us`

How long the SX1262 waits after transmitting before switching back to receive. Increase it when
fast replies (the challenge or key frames) arrive corrupted.

*Observations:* experiments here established ~`500` µs as the minimum that keeps the
demodulator from mangling the first bytes of a quick reply after the transmit→standby→receive
transition. It works hand-in-hand with bandwidth — a narrower bandwidth generally wants a
longer settle.

#### `sx1276_rx_bandwidth`

GFSK receiver bandwidth on the SX1276, written to both the RX and AFC bandwidth registers.
Change it when discovery or key-exchange replies fail to decode cleanly on an SX1276 board.

*Observations:* the default `41.7` kHz is the long-standing fixed value — tighter than the
~77 kHz Carson-rule figure, chosen to maximise sensitivity by rejecting out-of-band noise, and
validated against real devices. Unlike the SX1262, the SX1276 has a fast TX→RX turnaround and
has worked reliably at this narrow default across the devices tested here, so this knob is
exposed for marginal-range or drifting installs rather than because a change was needed. Widen
it (`62.5`/`83.3`/`125.0`) when a device's transmitter drifts more than the controller's radio,
at the cost of admitting more noise; narrow to `20.8` for maximum noise rejection on a clean
signal.

#### `sx1276_response_preamble`

How long a preamble the SX1276 puts in front of its *reply* frames (the key-transfer and
authentication responses), giving the peer time to lock on after the hub transmits. Raise it
if key exchange stalls right after discovery on an SX1276 board.

*Observations:* the SX1276 originally reused the protocol's 8-byte short preamble here. A
slightly longer `12` bytes was found on hardware to improve the peer's lock-on with no
measurable timing cost, so that is now the default — actually longer than the SX1262's `8`
(see the SX1262 section above: brand-new devices needed noticeably more than that on the SX1262,
so don't read the SX1276's smaller number as evidence 12 is generous; it has simply never needed
raising in testing here). Lengthen it further for a stubborn or marginal-range device.

#### `sx1276_discovery_hop_slice_ms` / `sx1262_discovery_hop_slice_ms`

How long the receiver dwells on each channel while hopping during the discovery wait. Change
these when a device is clearly present but its discovery response is never caught.

*Observations:* because a device often answers on a different channel than the request was
sent on, hopping during the wait is essential (this was the single biggest discovery fix).
Known implementations dwell only a few milliseconds per channel, extending the dwell when a
preamble is detected — the SX1276 uses that fast cycle. The SX1262 needs a much longer
per-channel dwell (~200 ms) because its shorter-preamble responses are harder to catch mid-hop.

#### `lr1121_rx_bandwidth` / `lr1121_response_preamble` / `lr1121_post_tx_settle_us` / `lr1121_discovery_hop_slice_ms`

The LR1121 equivalents of the four SX1262 knobs above — same meaning, same defaults, same
register-level reasoning (the LR1121's GFSK bandwidth encoding is register-identical to the
SX1262's, and it needs the same standby→retune→RX hop cycle, no fast hop).

*Observations:* the defaults were seeded from SX1262's validated values and are now confirmed
working on real LR1121 hardware — authenticated open/close/stop exchanges complete reliably
against a real awning at the stock `117.3` kHz / `8` B / `500` µs / `200` ms settings. Two of
`lr1121_rx_bandwidth`'s enum values (39.0/46.9 kHz) turned out to have the wrong register
encoding when borrowed directly from SX1262 and were corrected — the full, corrected option set
is `39.0` / `46.9` / `58.6` / `78.2` / `117.3` / `156.2` / `187.2` kHz. The main variable that
still matters in practice is RF link quality (RSSI) rather than these timing/bandwidth knobs —
weak signal shows up as intermittent frame loss on either leg of an exchange, which the
existing per-command retry already absorbs; there was no need to touch these defaults to get a
working exchange.

#### `lbt_max_retries` / `lbt_rssi_threshold_dbm`

Listen-Before-Talk: before transmitting, the hub checks the channel is quieter than the
threshold, retrying up to *max_retries* times, then transmits anyway. Loosen them when
transmissions are delayed on a channel that only *looks* busy.

*Observations:* a device's own pairing-mode beacons made the channel read as busy (around
-77 to -83 dBm), which tripped every LBT retry before each discovery transmit and slowed
pairing noticeably. Raising the threshold (less sensitive) or lowering the retry count lets the
hub transmit through that.

> [!WARNING]
> **Regulatory note:** the `-90` dBm / ≥5 ms defaults follow the 868 MHz band's listen-before-talk
> rules. Loosening them is acceptable for a brief experiment but should not be left in a
> production configuration.

### Pairing protocol — in detail

#### `pairing_discovery_commands`

Which discovery command(s) the hub broadcasts, and in what order; it sends them in sequence and
stops at the first device response.

- `0x28` — standard 2W broadcast discovery, sent to `0x00003B`.
- `0x2E` — *alternate* discovery, sent to `0x00003F` — the address on which devices in a
  1W-triggered pairing mode (and their beacons) listen. Conventionally paired with payload
  `0x00` and `low_power` on.

`0x2A` (SPE roll-call) is **not** an option here, on purpose. It looks superficially similar —
authenticated with the configured `system_key`, broadcast to `0x00003B` — but only devices that
*already* hold your key answer it. A device sitting in learning mode, waiting to be paired, holds
no key yet and stays silent. So adding it to this list cannot help a pairing attempt succeed; it
would only add replies from devices you have already paired. It is a "who is still out there"
roll-call over your existing devices — a different question entirely from "who wants to pair".

If a "who is still out there" roll-call over your *already-paired* devices is what you actually
want, that is exactly what the `scan_paired_devices` hub action does — see
`docs/home_io_control.md`'s "Home Assistant Actions" section.

*Observations:* for the devices tested here, plain `0x28` to `0x00003B` is what works — the
alternate `0x2E` drew no response from them (but they already answer `0x28`). Full-featured
controllers are known to broadcast both commands throughout pairing, which is exactly why the
combined preset exists: a device that ignores `0x28` may only be reachable via `0x2E`. In the
Home Assistant UI the selector offers the combination (`0x28,0x2E`) as a preset.

#### `pairing_discovery_destination`

The address the discovery frames are sent to. With `auto` each command uses its conventional
address (above). An explicit `0x00003B` / `0x00003F` forces *every* configured command to that
address — useful for deliberately sending `0x28` to the alternate address, or vice-versa.

#### `pairing_discovery_payload` / `pairing_discovery_low_power`

An optional single `0x00` payload byte, and the `LOW_POWER` frame flag.

*Observations:* the alternate discovery path is conventionally sent *with* the `0x00` payload
and `LOW_POWER` set, and some devices filter on them; they have no effect on the plain `0x28`
path, so only enable them alongside `0x2E`.

#### `pairing_discovery_wait_ms` / `pairing_discovery_initial_dwell_ms`

How long to wait for a response after each discovery transmit, and an initial settle before the
first transmit. `pairing_discovery_wait_ms` also sets the per-attempt listen window for the
`scan_paired_devices` Home Assistant action (see `docs/home_io_control.md`'s "Home Assistant
Actions" section); since that action makes three attempts, one per channel, its total runtime is
roughly 3x this value.

*Observations:* the `300` ms initial dwell mirrors the conventional wait after a start frame.
Lengthening the wait only helps when responses are *intermittent* — merely extending the dwell
without hopping across channels did **not** help in testing; the fix was the hopping itself.

#### `pairing_key_exchange_retries`

How many times to retry the authenticated key exchange, each attempt using a fresh challenge.

*Observations:* the key-transfer step is the most fragile part of the flow on SX1262 — a
device may acknowledge the challenge yet never confirm the key. That is usually a
preamble/turnaround problem, so if extra retries alone don't help, address
`sx1262_response_preamble` and `sx1262_post_tx_settle_us` first. Retries mainly guard against
occasional transient decode failures.

### Reading pairing results without the tuning UI

Every `home_io_control` config with a `button:` entity gets a companion **"Last Pairing
Result"** diagnostic text sensor that publishes a frozen, machine-readable summary after each
pairing attempt (leading `v1;` is a version tag — any later format change bumps it):

```
v1; outcome=<paired|no_response|invalid_response|key_exchange_failed|config_failed>; phase=<...>; node=<XXXXXX|->; type=<...|->; attempts=<n>; lbt=<n>; dur_ms=<n>; heard=<n>; advice=<codes|none>
```

`lbt` (LBT retries consumed) and `advice` (see below) are the two fields most useful while
tuning: a high `lbt` count with a `channel_busy` advice code means the channel — not the
tuning parameters above — is the bottleneck. See `docs/home_io_control.md`'s "Diagnosing a
failed pairing attempt" section for the full field reference and the pairing-window traffic
advisor's advice codes (`1w_traffic`, `channel_busy`, `foreign_controller`, `rf_silent`).

## A suggested tuning plan

If a device that should be in pairing mode does not pair with the defaults, work through the
following in order, changing one thing at a time and checking the logs after each step. This
is a general starting point, not a guarantee — different devices need different combinations.

1. **Baseline & identify the target.** Enable `ui_controls: true` and `DEBUG` logging, then press
   *Discover & Pair*. Confirm the hub transmits (`io_capture … stage=tx_frame`) and watch whether
   any discovery response (`0x29`) comes back. While the device is in pairing mode, note whether
   the *target device itself* transmits (by its own `src=` address) and to which `dst=` address —
   that address is your best clue for the following steps.

2. **Try the alternate discovery command.** Devices that ignore `0x28` may respond to `0x2E`
   on `0x00003F`:
   ```yaml
   tuning:
     ui_controls: true
     pairing_discovery_commands: ["0x2E"]
     pairing_discovery_payload: "0x00"
     pairing_discovery_low_power: true
   ```

3. **Send both broadcasts.** Cover devices that only answer one of them:
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

5. **If discovery is intermittent** (responses appear sometimes), widen the timing:
   ```yaml
   pairing_discovery_wait_ms: 3000
   pairing_discovery_initial_dwell_ms: 500
   sx1262_discovery_hop_slice_ms: 250   # SX1262 boards only; use lr1121_discovery_hop_slice_ms on LR1121
   ```

6. **If discovery succeeds but key exchange fails** (`saw_challenge=0`, or the exchange stops
   after discovery), give the receiver more margin around the turnaround (SX1262 shown; on
   LR1121 boards use the `lr1121_*` equivalents instead):
   ```yaml
   sx1262_post_tx_settle_us: 750    # then 1000
   sx1262_rx_bandwidth: 156.2
   sx1262_response_preamble: 12     # then 16
   ```

7. **If the logs show LBT delaying transmissions** on a quiet channel, relax LBT — but see the
   compliance note below:
   ```yaml
   lbt_max_retries: 1
   lbt_rssi_threshold_dbm: -80
   ```

After each step, record the tuning snapshot line and the outcome. When a combination works,
paste that snapshot into your permanent `tuning:` block. If you find a combination that makes
a previously-unsupported device work, it is worth sharing via an issue on the projects Github-Page
so the defaults can improve.

## Safety and compliance

`lbt_max_retries` and `lbt_rssi_threshold_dbm` exist for diagnostics only. Setting the
threshold too high (e.g. `-45 dBm`) or the retry count too low can force transmissions on a
busy channel and may violate local regulations for the 868 MHz SRD band. Do not leave
aggressive LBT values in a production configuration.

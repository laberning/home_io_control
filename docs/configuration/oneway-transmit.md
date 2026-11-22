# Sending 1W commands
<!-- doxygen-label: cfg_oneway -->

The hub can act as a **1W controller** — the kind of thing a wall remote is — and drive devices by
transmitting. This is off unless you configure it, and it signs with a key you already hold: the
same authorisation as any 2W command this component sends.

> [!WARNING]
> A device only obeys a controller it has been taught. Real-hardware testing established that 1W
> actuators keep a **table of registered controllers**. A frame that is correctly built, correctly
> addressed and signed with a key the device accepts is still ignored if this hub's source address
> is not in that table. Enrollment (below) is what registers it: do that first, for every identity,
> before expecting any command to move a device.

## What 1W is, and what it is not

Everything about this feature follows from two properties of the protocol:

- **A 1W command addresses a device *class*, not a device.** There is no unicast form. A command
  sent as a `roller_shutter` identity reaches every roller shutter in range, and every one of them
  that has enrolled this identity acts on it. To control two shutters separately, give each its own
  identity and enroll each identity in only one of them.
- **Nothing replies.** No acknowledgement, no status, no error. A command a device ignored is
  indistinguishable on the radio from one it obeyed.

The second one shapes the whole feature. There is no failure you can be notified about, so the
"Last 1W Command" sensor and the [troubleshooting ladder](../troubleshooting.md#1w-commands-do-nothing) are the diagnostic tools.

## Controller identities

Because nothing on the wire names a device, what distinguishes one 1W control surface from another
is the *controller* doing the transmitting. That triple — source address, network key, device class
— is a **controller identity**, and it takes the place node addressing has for 2W
([ADR 0027](../adr/0027-controller-identities-replace-node-addressing-for-1w.md)).

**Minimal example** — the fewest fields that generate a working set of buttons:

```yaml
home_io_control:
  # ... radio pins, node_id, system_key ...
  oneway_controllers:
    - id: velux_windows
      io_device_type: window_opener
      commands: [open, close, stop]
```

**Full example** — every optional key at once, so you can see the whole shape in one place:

```yaml
home_io_control:
  oneway_controllers:
    - id: velux_windows
      io_device_type: window_opener                    # see the named device types table
      commands: [open, close, stop, favorite]
      node_id: A11CE0                                   # optional -- overrides the derived address
      system_key: FEDCBA98765432100123456789ABCDEF      # optional -- reuse a recovered network's key
      initial_sequence: 4000                            # optional -- seed the rolling counter
      manufacturer: somfy                               # required only because enrollment: true, below
      enrollment: true                                  # generates the "Enroll 1W Controller" button
      drives: [FEEB1E]                                  # optional -- devices that follow this identity's commands
      cover: "Roof Window"                              # optional -- a cover entity with an assumed state
      follows: [9D6085]                                 # optional -- remotes whose presses move that cover
      open_duration: 30s                                # optional, with close_duration -- the cover shows a position
      close_duration: 28s
      set_position: timed                               # optional -- how a slider value is sent: timed or frame
    - id: hall_lamp
      io_device_type: light
      commands: [open, close, stop]
      light: "Hall Lamp"                                # optional -- a light entity with an assumed level
      dimmable: true                                    # optional -- a brightness slider
      on_duration: 4.7s                                 # optional, with off_duration -- the light shows its fade
      off_duration: 4.7s
      set_position: timed                               # optional -- how a slider value is sent: timed or frame
```

| Key | Required | Meaning |
|---|---|---|
| `id` | yes | Handle the generated entities are named and ID'd from. |
| `io_device_type` | yes | The device class this identity commands — see the [named device types table](../device-types.md#named-device-types) for the full list. |
| `node_id` | no | Source address to transmit as. **Derived from your hub's `node_id` and this `id` when omitted**, deterministically at *compile* time — a derived address takes part in the same collision checks as an explicit one (a clash fails the build), and is printed at boot marked `(derived)`, since nothing in your YAML shows it otherwise. Asking you to invent a 3-byte radio address instead would be an unanswerable question: nothing tells you which addresses are safe, and colliding with a real remote in range silently desyncs both transmitters' counters. |
| `system_key` | no | Network key for this identity. **Defaults to the hub's own** — every new identity works fine with the default. Only set this to a recovered key when reusing an existing, already-registered identity (see "Recovering a 1W controller key" below). |
| `initial_sequence` | no | Seeds the rolling counter, from 0 to 65535 (default 0). It only ever moves the counter forward: raise it and reflash when a device has stopped accepting commands because its own count ran ahead. To fix that without a reflash, call [`oneway_set_sequence`](actions.md#oneway_set_sequence). At boot the hub uses it when nothing is stored yet, or when it is above the stored counter by less than 32768 (half the 16-bit range); otherwise the stored counter stands. A value at or below the stored counter changes nothing, and a counter that has wrapped past 65535 is not sent back to a value 32768 or more above it ([ADR 0114](../adr/0114-a-one-way-counter-resumes-from-its-persisted-value-across-the-wrap.md)). A bench run on a Somfy Izymo showed that it obeys any higher number (tested up to a jump of 32000) and ignores every step across the 16-bit wrap (65535 to 0), so an identity whose counter wraps is lost to that device: give it a new address (a new `node_id`, or a new `id` when the address is derived) and enroll it again ([ADR 0120](../adr/0120-a-one-way-counter-that-wraps-is-lost-to-a-somfy-receiver.md)). Other receivers are not measured, so prefer small steps. See [1W commands do nothing](../troubleshooting.md#1w-commands-do-nothing). |
| `commands` | no | Which buttons to generate: `open`, `close`, `stop`, `vent`, `favorite`. `open`, `close` and `stop` match captured presses of a real Somfy remote. `vent` is not backed by any capture. `favorite` is extrapolated from the 2W encoding and contradicted by this project's own capture of a real My-button press, which encodes it differently; on a real dimmer it changes the brightness to an unverified target. Treat `vent` and `favorite` as untested. |
| `manufacturer` | conditional | The manufacturer ID byte an enrollment frame carries on air — a named value such as `somfy`, or a raw integer; see the [named manufacturers table](../device-types.md#named-manufacturers) for the full list. **Required whenever `enrollment: true` is set** — the build fails otherwise, rather than silently broadcasting `0`. Also selects the **1W wire profile** (see "Matching your remote's vendor" below): `velux` sends `CMD_EXECUTE` frames with a different priority byte than `somfy`. Find the value in a key recovery report for this network (see "Recovering a 1W controller key" below), or in the device's own documentation. |
| `execute_broadcast` | no | `typed` (default) addresses your `io_device_type` class; `all` addresses the all-devices broadcast, which is what a handheld cover remote of either vendor sends for open/close/stop. Set `all` to mimic a real remote. With `all` the frame carries no class at all, so `io_device_type` does not decide which devices are reached; it still decides the default of `invert_position` and the name in the "Last 1W Command" sensor. |
| `execute_acei` | no | Raw override of the priority byte (payload[1]) in a 1W `CMD_EXECUTE` frame, e.g. `0x61`. Must be `1`–`0xFF` — `0x00` is rejected, since `0` is the sentinel for "not overridden". Normally left unset — it is derived from `manufacturer`. Use it only for a vendor this project has no profile for. |
| `enrollment` | no | Build flag (default `false`) for this identity's **"Enroll 1W Controller"** button — see "Enrolling this hub as a controller" below. |
| `enrollment_classes` | no | Which device classes a **VELUX** enrollment `0x30` sweep targets, as a list of one to three `io_device_type` names, each at most once and none `unknown`. Unset → the profile default `[roller_shutter, vertical_exterior_awning, dual_roller_shutter]`, the exterior-shading set a KLI 310/313 uses. **Interior blinds need it set**: a KLI 312 uses `[vertical_interior_blind, interior_venetian_blind]`. To find the classes for your product, see "Finding your enrollment classes" below. **Ignored by the Somfy gesture**, which sends both bursts to the all-devices address `00003F` (issue #147), and ignored without `enrollment: true`; the build warns in both cases. See "Enrolling this hub as a controller" below. |
| `low_power` | no | The radio preamble every burst of this identity uses ([ADR 0038](../adr/0038-oneway-bursts-follow-the-identity-power-class.md)). Unset and `false` send four short copies at the live `start_preamble_ms` tuning value, whatever `manufacturer:` says ([ADR 0122](../adr/0122-an-unset-one-way-low-power-sends-four-short-copies.md)). That is what a real Somfy remote sends and what a mains-powered receiver needs. **A battery or solar receiver needs `low_power: true`**: it sends one wake-up copy first, which lasts [`low_power_wake_ms`](tuning.md#low_power_wake_ms) (default 515 ms, the same on every radio; [ADR 0115](../adr/0115-a-low-power-one-way-wake-up-lasts-the-hubs-wake-up-duration.md), [ADR 0118](../adr/0118-the-wake-up-preamble-defaults-to-515-ms.md)), then three short repeats. Without it a sleeping receiver misses the command. Your boot log prints which shape the identity uses. Applies to every transmit from this identity: commands, positions, both enrollment gestures, and un-enrollment. The section "What one command puts on the air" below gives the time each shape takes. |
| `invert_position` | no | Whether open, close and every position are mirrored on the wire (`0` ↔ `100`). Leave it unset and the device kind decides: a kind whose wire polarity is inverted, such as `horizontal_awning`, is mirrored, exactly as a 2W cover of that kind is, so the generated Open button opens the awning. Set `false` if your receiver turns out to follow the other polarity, or `true` to mirror a kind that is not inverted by default. Your boot log prints `inverted=yes (from kind)`, `inverted=yes (configured)` or `inverted=no`. With `execute_broadcast: all` the mirrored frame reaches every receiver of the broadcast, so the build warns when an inverted kind is combined with it and no explicit `invert_position`. STOP, favorite and vent are never mirrored. |
| `drives` | no | The devices, by `io_device_id`, that follow this identity's commands: a list of covers or lights declared in the same configuration. After each command the hub transmits as this identity, every listed device shows the commanded state at once and is read a moment later. See [Following a device the identity drives](#following-a-device-the-identity-drives). |
| `cover` | no | The name of a cover entity for this identity, for a cover kind only (a shutter, awning, blind, window, door or gate; the build fails for a light or any other kind). It sends the identity's open, close and stop and shows the state the hub assumes ([ADR 0117](../adr/0117-a-one-way-controller-can-show-an-assumed-state-cover.md)). Its ID is `<identity_id>_cover`. See [A cover for the identity](#a-cover-for-the-identity). |
| `follows` | no | The source addresses of the real remotes whose presses the cover or the light follows, a list of six hexadecimal characters each. Needs `cover` or `light`. A remote that is not listed never moves the state. |
| `light` | no | The name of a light entity for this identity, for a light kind only (the build fails for any other kind, and for an identity that also has `cover`). It sends the identity's open (on) and close (off) and shows the level the hub assumes ([ADR 0128](../adr/0128-a-one-way-light-shows-an-estimated-level-and-a-slider-value-is-a-timed-stop-or-a-position-frame.md)). Its ID is `<identity_id>_light`. See [A light for the identity](#a-light-for-the-identity). |
| `dimmable` | no | Gives the light a brightness slider, as `dimmable` does on the two-way light. Only with `light`. |
| `on_duration`, `off_duration` | no | How long the light takes to fade from off to full and from full to off, for example `4.7s`. Set both or neither, each above zero, and only with `dimmable: true`. With them the light shows an estimated level while it fades, and a slider value reaches the device. |
| `set_position` | no | How a slider value reaches the device: `timed` (default) or `frame`. Only with `open_duration` and `close_duration` on a cover, or with `on_duration` and `off_duration` on a light. See [A slider value on the cover](#a-slider-value-on-the-cover) and [A light for the identity](#a-light-for-the-identity). |
| `open_duration`, `close_duration` | no | How long the cover takes for a full run from closed to open and from open to closed, for example `30s`. Set both or neither, each above zero, and only with `cover`. With them the cover shows an estimated position and shows opening or closing while it moves ([ADR 0127](../adr/0127-a-one-way-cover-shows-a-position-from-its-travel-time.md)). See [A position from the travel time](#a-position-from-the-travel-time). Time a full run once with a stopwatch and type it in. |

## What the build checks

A build stops with an error when an identity shares its source address with another identity, the
hub, a linked remote or a device's `io_device_id`, when an `id` repeats, or when `enrollment: true`
has no `manufacturer:`, or when the removed `enrollment_with_mac` key is still set (delete the line; the
enrollment frame always goes out without a MAC trailer, like a real remote, [ADR 0124](../adr/0124-enrollment-with-mac-is-removed.md)). It also stops when
`drives:` names an `io_device_id` no entry declares, names a device that is not a cover or a light,
or lists a device twice. An identity's own address under `linked_remotes:` is rejected too; list the
device under `drives:` instead.
It continues with a warning, which names the identity, when a configuration is legal but likely
wrong:

| Warning | What it means |
|---|---|
| `manufacturer` has no 1W wire profile | An identity that transmits uses a vendor other than Somfy or VELUX, so it sends the Somfy priority byte `0x43`. Set `execute_acei:` if that is wrong for the device. |
| VELUX interior blind with `enrollment: true` and no `enrollment_classes:` | The default sweep covers exterior shading and probably misses the blind; see "Finding your enrollment classes" below. |
| Inverted device kind with `execute_broadcast: all` and no `invert_position:` | Open and Close are mirrored for every receiver of the broadcast, including devices of other kinds. Set `invert_position` explicitly. |
| `enrollment_classes:` without `manufacturer: velux` and `enrollment: true` | Only the VELUX enrollment gesture reads the list, so it is ignored here. |

## Matching your remote's vendor

A 1W `CMD_EXECUTE` frame (open/close/stop) differs between vendors in exactly one byte: the
priority/ACEI byte at payload position 1. **Somfy remotes send `0x43`; VELUX KLI-class remotes
send `0x61`.** The hub picks this from `manufacturer:` — `somfy` (or omitted) gives `0x43`; `velux`
gives `0x61`. Any other vendor falls back to `0x43`, since this project has only
verified the two; if that vendor was set explicitly (not defaulted) on an identity that actually
transmits — one with `commands:` or `enrollment: true` — the build also logs a warning.

To mimic a handheld remote, also set `execute_broadcast: all` — real remotes broadcast open/close/
stop to every device, not to one class. The boot log prints the resolved `acei` and `broadcast`
for each identity; log a press of your real remote at DEBUG level and compare its
`rx 1W remote ... EXECUTE(0x00)` line: `priority=user_high` is `0x43`, `priority=user_default` is
`0x61`, and `dst-class=all` is the all-devices address.

The VELUX profile is verified against KLI-class remotes for exterior shading (awnings, screens,
roller shutters such as the SML) and interior blinds (KLI 312). A VELUX window-opener or KLR-class remote
may differ — use `execute_acei:` if so.

There is no 1W `force_open` button, because no 1W frame is known to carry that meaning: the byte
sometimes labelled "force open", `0x64`, moves a real device to an ordinary 50% position when sent
as a command. The hub's force-open (`force_open_device`) is a separate, 2W, per-device action — see
[Home Assistant actions](actions.md).

## What one command puts on the air

Every command goes out as one burst. Most of the burst is fixed; two settings change its shape.

| Fixed for every identity | Value |
|---|---|
| Copies per command | 4, all with the same sequence and signature: a device counts one sequence as one command |
| Gap between copies | No deliberate wait: the next copy starts 6 to 13 ms after the previous one ends (about 7 ms on the SX1276, about 10 ms on the SX1262 and LR1121), the time of the channel check and the transmit setup, measured on the air. A real remote sends its copies 4 to 7 ms apart (`tests/corpus/captures/oneway/somfy_smoove_oneway_short_press_on_air.yaml`) |
| Channel | 868.95 MHz (channel 2) |
| Wake-up preamble, where a copy has one | `low_power: true`: `low_power_wake_ms`, 515 ms by default on every radio ([ADR 0115](../adr/0115-a-low-power-one-way-wake-up-lasts-the-hubs-wake-up-duration.md), [ADR 0118](../adr/0118-the-wake-up-preamble-defaults-to-515-ms.md)) |
| Channel check | Before every copy the radio listens for up to 25 ms and sends anyway after that |

Nothing in your YAML changes these. The things you can set are the shape, with `low_power:`
([ADR 0038](../adr/0038-oneway-bursts-follow-the-identity-power-class.md)), the length of the
short preamble, with [`start_preamble_ms`](tuning.md#start_preamble_ms) (default 10 ms), and the
length of the `low_power: true` wake-up copy, with [`low_power_wake_ms`](tuning.md#low_power_wake_ms)
(default 515 ms). A change of either setting applies to the next command. While a
burst runs the hub sends and receives nothing else, so one command holds the radio for:

| Shape | You get it with | SX1262, LR1121 | SX1276 |
|---|---|---|---|
| Short preamble on all four copies | `low_power:` unset or `false`, with any `manufacturer:` | about 0.1 s | about 0.1 s |
| Wake-up on the first copy, short on the other three | `low_power: true` | about 0.6 s | about 0.6 s |

On the one-way channel the EU rules allow 3.6 s of transmission an hour. The transmitter is on for
about 0.07 s of a command in the first shape and about 0.58 s in the second (the short gaps between
copies do not count), so about fifty commands of the first shape fit in an hour, or about six of the second. A Somfy Izymo dimmer obeyed 50 of 50 commands in
each shape, copies back to back, on an SX1276, two SX1262 and an LR1121 board ([ADR 0121](../adr/0121-one-way-copies-go-out-back-to-back.md)), so a mains-powered Somfy receiver does not
need the wake-up copy. A battery or solar one does: set `low_power: true` on it
([ADR 0122](../adr/0122-an-unset-one-way-low-power-sends-four-short-copies.md)).

The copies, channel and preamble lengths in the first table come from the air: each shape, sent by
an SX1276, an SX1262 and an LR1121 board and captured with an SDR on all three channels, is in
`tests/corpus/captures/oneway/unenrolled_window_opener_oneway_stop_{low_power,short}_burst_{sx1276,sx1262,lr1121}.yaml`.
The hub that sent those captures waited 40 ms between copies and used a 1024-byte wake-up (about
213 ms, 267 ms on the SX1276). The gaps and the 515 ms wake-up of the current hub were measured with
the same SDR set-up on all four boards; they are not in the corpus. Only the first copy of a
`low_power: true` burst carries the low-power flag. An enrollment sends two bursts (Somfy) or six
(VELUX), so it holds the radio for about that many times longer.

## Enrolling this hub as a controller

**This is the step that makes every command above actually move something.** A 1W device ignores
any frame from a source it has not been taught, no matter how correctly it is built or signed —
see the warning at the top of this section. Enrollment is what teaches it.

Add `manufacturer:` and `enrollment: true` to an identity — see the full example above. `enrollment:
true` is the build flag: its presence is the whole gate, and adding or removing the line and
reflashing is the feature's entire lifecycle. Setting it creates one more entity for that identity:
**"\<Identity\> Enroll 1W Controller"**, `entity_category: config`.

**Enrolling is additive, not destructive.** A device's controller table holds more than one entry,
each with its own key — enrolling this hub alongside an already-registered remote leaves that
remote working exactly as before; the device answers commands from either.

**The gesture is two-sided, and only one half is a button press.** What the receiver-side half looks
like depends on your device's manufacturer — Somfy and VELUX differ, see below.

| Half | Who does it | What it is |
|---|---|---|
| Receiver enters association mode | **you, physically** | a physical gesture on the receiver, confirmed by its own indicator — for **Somfy**, a **2 second** hold on the actuator's PROG button; **VELUX** uses a different gesture, see below |
| Controller offers its credential | **the hub** | one **short** press of the "Enroll 1W Controller" entity |

For Somfy, get the order and the durations right: 2 seconds on the receiver, *then* one press on the
hub — not the other way around, and not a long hold on the hub's entity (there is nothing to hold; a
press is a press). Getting the two halves' timing backwards is the most common failure mode here,
not a protocol problem.

The press sends two bursts back to back — `0x39` (remove; carries only this identity's own
address, so it can never touch a different controller's entry), immediately followed by `0x30`
(the credential itself), 4 copies each. With the default
(`low_power` unset) every copy is short, so a burst takes about 0.1 s and the whole press about 0.2 s;
with `low_power: true` the first copy of each burst carries the wake-up preamble, a burst takes about 0.6 s
and the whole press about 1.2 s.
Press Enroll soon after the receiver confirms its association mode, not at the end of its window.
This is the documented 1W pairing handshake (a real Somfy Smoove remote does exactly this,
corpus-captured), not two independent actions. For Somfy, both bursts go to the all-devices address
`00003F`, like a real Somfy remote, so the `io_device_type` you configured does not decide whether
the receiver is reached; it addresses only the identity's commands. There is no "learn window" on
the hub's side beyond that, because the device owns its own timeout and there is nothing further to
wait for. **Only one device should be in association mode at a time**: the frames reach every
device in range that is currently listening for them, so a second actuator in learn mode nearby
would be taught too.

The `0x30` half is the 29-byte frame real remotes send, with no MAC trailer
([ADR 0124](../adr/0124-enrollment-with-mac-is-removed.md)).

**Pressing Enroll for an identity the receiver already knows removes it.** A bench run on a Somfy
Izymo showed that one PROG window takes one change: the first remove (`0x39`) or add (`0x30`) from a
controller the device already knows takes that controller off, and the window is spent. The Somfy
gesture sends the remove first, so on an identity that is already enrolled the press un-enrolls it
instead of enrolling it again. An identity the device does not know is paired normally: the add
pairs it, and the device then follows the sequence numbers from that add. A remove from an
unknown controller changes nothing. Enrolling again is therefore no cure for a desynced counter on
this device. Give the identity a new address (a new `node_id:`, or a new `id:` when the address is
derived) and enroll that fresh, as the `initial_sequence` guidance above says for a counter that
has wrapped. The run also left one identity that had been removed twice unpairable in three later
windows, for a reason that is not known. How VELUX receivers handle an identity they already know is
not confirmed.

### VELUX (`manufacturer: velux`) uses a different gesture

A real VELUX KLI remote does not enroll the way a Somfy Smoove does, so with `manufacturer: velux`
the Enroll button emits the KLI gesture instead
([ADR 0032](../adr/0032-oneway-velux-enrollment-gesture.md)):

1. `0x39` clear to the all-devices address (not the identity's typed class).
2. `0x30` add-controller **swept across the identity's enrollment classes** under one sequence. The
   actuator presumably filters by its own class, so only the matching frame registers the hub. The default is the
   exterior-shading set `roller_shutter`, `vertical_exterior_awning`, `dual_roller_shutter`. Interior blinds use other
   classes: set `enrollment_classes:`, see "Finding your enrollment classes" below.
3. A STOP then a DOWN command to the all-devices address — the KLI manual's "then press STOP then
   DOWN" registration-completion step.

The receiver side of this gesture is **the Gear button ("open for registration"), pressed for
about 1 second, on an already-registered VELUX control** (e.g. the KLI that already drives the
product). A KLI also has a **Pair** button ("register"); that one belongs to the *new* control, and
the hub's Enroll button takes its place. Don't press Pair on the existing control after Gear:
Gear followed by Pair deletes all products from that wall switch.
The product you are enrolling then runs a ready sequence, which on a shutter can take half a minute:
it may first travel to about 10% closed, jog back and forth several times, and then return to where
it started. **Wait until that sequence has finished, then press Enroll on the hub right away** — how
long the product stays open for registration after that is not measured. If that control drives
several products, every product that jogs is open for registration at the same time, so assume each
of them will register the hub too — confirmed for real KLI remotes, unconfirmed for the hub, but the
gesture is additive, the same as Somfy's. An identity registered in two products moves both, even
though each product only obeys its own registered controllers. **For separately controllable covers,
use one identity per product and make sure only that product runs its ready sequence** — press Gear on
a control that drives only that product.

Set `io_device_type:` to whatever the device actually is (it drives the control-frame destination).
The enroll gesture ignores `io_device_type` and uses the enrollment classes. At the short preamble —
which an unset `low_power:` gives you — the Enroll button blocks for about 0.6 seconds;
with `low_power: true` for about 3.6 seconds (six bursts of about 0.6 s).

```yaml
oneway_controllers:
  # Exterior shading behind a KLI 310/313, mains-powered (e.g. an SML roller shutter)
  - id: velux_shutter
    io_device_type: roller_shutter
    manufacturer: velux
    execute_broadcast: all
    enrollment: true
    commands: [open, close, stop]
  # Interior blind behind a KLI 312, solar-powered
  # (low_power: true adds the wake-up copy a duty-cycled receiver needs)
  - id: velux_blind
    io_device_type: interior_venetian_blind
    manufacturer: velux
    execute_broadcast: all
    enrollment: true
    enrollment_classes: [vertical_interior_blind, interior_venetian_blind]
    low_power: true
    commands: [open, close, stop]
```

#### Finding your enrollment classes

The existing remote tells you. Enable DEBUG logging, press Gear on it, and look for its `0x2E`
lines:

```
rx 1W remote 084374 dst-class=vertical_interior_blind: DISCOVER_ALT_REQ(0x2E) ...
rx 1W remote 084374 dst-class=interior_venetian_blind: DISCOVER_ALT_REQ(0x2E) ...
```

Set `enrollment_classes:` to the classes those lines target (here `[vertical_interior_blind, interior_venetian_blind]`). If
they are `roller_shutter`, `vertical_exterior_awning` and `dual_roller_shutter`, the default already fits and you can leave
the key unset. The build warns when a VELUX `vertical_interior_blind` or `interior_venetian_blind` identity enrolls with
the default classes, because that combination is unlikely to fit.

> [!WARNING]
> Pressing Enroll can physically move a cover. Step 3 is a real STOP then a real DOWN broadcast,
> but a 1W receiver only acts on frames from a controller already in its own table — the hub
> transmits under this identity's own address, never a remote's — so only a product that holds this
> identity as a registered controller reacts, including one that has registered it a moment earlier
> through the sweep in step 2. That closing movement is the success signal: if nothing moves,
> nothing registered, unless the cover is already fully closed, in which case the DOWN has nothing
> visible to do. The ready sequence returns the product to where it started, so start from a partly
> open position, or check with Open afterwards. This is the KLI registration gesture itself.

> **VELUX 1W enrollment is confirmed** on a mains-powered SML roller shutter (SX1276, `low_power: false`,
> default classes) and on KLI 312 interior blinds (SX1262, `low_power: true`,
> `enrollment_classes: [vertical_interior_blind, interior_venetian_blind]`), both in issue #74. If it doesn't take, check the
> enrollment classes first, then that Gear made the product run its ready sequence and you pressed
> Enroll once it had finished. A solar or battery VELUX receiver wants `low_power: true`; a
> mains-powered one needs nothing, since an unset `low_power:` already sends the short preamble.
> The STOP+DOWN frames are reconstructed rather than byte-matched against a VELUX capture.

**A hub cannot enroll into a device nobody has walked up to.** The receiver-side physical gesture is
the real safety interlock here, stronger than any software confirmation could be — it is why this
feature has no separate arming switch the way the (irreversible) LR1121 bootloader rewrite does.

### Un-enrolling without re-enrolling

The Enroll button follows its `0x39` with a `0x30`, which pairs the identity on a receiver that
does not know it. To send the remove alone, call the [`oneway_remove_controller`](actions.md#oneway_remove_controller)
action:

```yaml
action: esphome.<device_name>_oneway_remove_controller
data:
  controller_id: velux_windows
```

This sends `0x39` alone, nothing else. It carries only this identity's own source address, so it
cannot remove a different remote's registration, and neither can the Enroll button's `0x39`.

> [!WARNING]
> Put the receiver in its programming mode first. Press the receiver's PROG button, then call
> `oneway_remove_controller` inside its association window. On a Somfy Izymo dimmer the light
> blinks during the remove and then ignores the controller (corpus capture
> `somfy_izymo_dimmer_enrollment_remove_controller_in_prog_window_sx1276`); sent with no window
> open, the action changes nothing and the hub keeps controlling the dimmer. The Izymo is the only
> receiver this is confirmed on; other receivers are untested.

**When you are done enrolling**, remove `enrollment: true` from the identity and reflash. A build
that can put a device into someone else's controller table should not be the build that runs
permanently — the same reasoning as removing `recover_oneway_key: true` after a key recovery.


## Recovering a 1W controller key

> [!WARNING]
> This feature is receive-only, but it recovers a real secret. It never transmits anything. It
> listens for a frame a 1W remote broadcasts during its key-copy gesture and decrypts it. The
> decryption needs no secret of its own (see the security note below), so treat the recovered key
> exactly as you would treat your `system_key`. Recover a key only from a remote or network you own
> or are authorized to access — see the project [Disclaimer](../../README.md#disclaimer--license).

One-way (1W) installations — a handheld remote driving a shutter or awning directly, with no hub —
have their own network key. This feature recovers it by overhearing a single `0x30` "add
controller" broadcast, which is what a 1W remote sends while its **remote-to-remote key-copy mode**
is active in order to hand its network key to a new remote.

**For controlling a device, you almost never need this.** A device accepts whichever key arrives
during its own association-mode window — enrolling this hub as a controller (see "Enrolling this
hub as a controller" above) works as well with a freshly generated key as with a recovered one, so
there is nothing to gain from recovering a key only to enroll with it. Recover a key only when you
specifically need to **become** an existing, already-registered identity rather than add a new one:

- **Replacing a remote that is gone for good** (lost, broken, retired), so the hub can take over
  its exact identity instead of the device learning a new one.
- **Understanding or documenting an existing network** — a diagnostic use, not a control one.

**Never reuse a recovered key under the original remote's address while that remote is still in
use.** A device tracks one rolling-sequence high-water mark per source address, fed by whichever
transmitter used it most recently. Two independent transmitters sharing one address — the original
remote and this hub — cannot coordinate that counter: the hub's own persisted value only reflects
what *it* has sent, so its next frame is very likely to land at or below what the device already
accepted from the real remote and be silently rejected as a replay. Reuse an identity's address
only once its original transmitter will never transmit again.

```yaml
home_io_control:
  # ... rst_pin / node_id / system_key / etc. as usual ...
  recover_oneway_key: true
```

Configuration variable:

- `recover_oneway_key` (Optional, boolean, default `false`): When `true`, dynamically creates the
  **"Recover 1W Controller Key"** switch entity, bound directly to this hub.

Like `accept_foreign_pairing`, this lives directly under `home_io_control:`, the generated switch
always boots off (`restore_mode: ALWAYS_OFF`) so a reboot can never leave it armed, and its name is
fixed (not configurable). The two features are independent — arming one never arms the other.

### Workflow

1. Flash the firmware with `recover_oneway_key: true` set in your `home_io_control:` block.
2. Turn the **"Recover 1W Controller Key"** switch on in Home Assistant. The hub arms for
   **10 minutes** and logs that it is listening. Nothing is transmitted.
3. Trigger the **key-copy gesture on your existing 1W remote** — the remote-to-remote copy mode
   described in its manual, the one you would use to teach a second remote the same network.
   Do this near the hub.
4. Watch the ESPHome logs. On success you will see a clearly-delimited block containing the
   recovered key and a ready-to-paste `oneway_controllers:` entry. The switch turns itself off
   immediately — one adoption per arm.
5. Paste the block directly into your `home_io_control:` block — the recovered key is already
   inline (`system_key: "..."`), same as the 2W Key Extraction report — and reflash.
6. If nothing happens within 10 minutes, the switch turns itself off and says so. Re-arm and try
   again closer to the device.

### Reading the result

The report tells you the **MAC status**, which is your on-the-spot evidence that the recovered key
is correct:

- **`MAC VERIFIED`** — the frame carried an authenticator and it checked out *under the key that
  was recovered a moment ago*. This is the strongest confirmation available without commanding a device.
- **`MAC FAILED`** — the authenticator did not check out. The key is probably wrong (a marginal
  reception is the usual cause). Re-arm and repeat the gesture closer to the hub.
- **`MAC not present`** — the frame carried no authenticator to check. Not an error; real hardware
  frequently omits it. The key may still be correct — enroll with it (see
  "Enrolling this hub as a controller" above) and confirm by
  commanding the device.

Two fields in the emitted block deserve a note:

- **`node_id` is deliberately absent.** The hub transmits under its *own* address, derived from
  your hub's `node_id`. It must never impersonate the existing remote: reusing that address would
  hijack the remote's rolling sequence counter and break it.
- **`io_device_type` is prefilled from what was overheard**, if this hub happened to see other 1W
  traffic from the same remote while armed. It is a well-founded guess, not authoritative — verify
  it. If nothing was observed, the line is commented out with a pointer to the DEBUG log line that
  reveals it.

### Security note

Anyone within radio range of a key-copy gesture can recover the network key this way. The wrapped
key in that broadcast is protected only by a **publicly-known transfer key**, using an
initialisation vector derived from the sender's own address — which is in the same frame's header,
in plaintext. There is no secret involved in the unwrap.

That is a property of io-homecontrol, not something this project introduces; the same framing
applies as to [key extraction](../key-extraction.md). The practical advice
is the same as for any secret: perform the key copy **once**, indoors, and treat the recovered key
as the credential it is. Raw `0x30` payloads are masked in this component's own frame logs for
that reason — the recovered key is printed in exactly one deliberate place, the adoption report.


## Generated buttons

Each name in `commands:` generates a button. Their **IDs follow `<identity_id>_<command>`** —
`velux_windows` + `open` → `velux_windows_open` — and that rule is a documented contract, because
you cannot compose against IDs you cannot predict. Entity names derive from the same pair
("Velux Windows Open").

These are created from the `oneway_controllers:` block rather than declared as device-bound
`button:` entries, deliberately. A platform entry would have to infer what the button *is* from
which keys are present, so a device-bound entry that merely omitted its `io_device_id` could be
misread as the security-sensitive 1W kind instead of failing validation. Creating these from the
hub block makes that class of mistake structurally impossible.

## Following a device the identity drives

A device that obeys one of your identities does not know that the hub sent the command, and the hub
does not hear its own transmission. Without a link, the cover keeps showing its old state until the
next status read. List the device under `drives:` and the hub treats its own command like a press of
a [linked remote](remotes.md#linked-remotes) it has just heard
([ADR 0116](../adr/0116-a-one-way-identity-names-the-devices-it-drives.md)):

- After an open, close, position or stop, each listed device shows the commanded state at once.
- A status read of each device follows: right away after a stop, otherwise after
  [`remote_activity_poll_delay_ms`](tuning.md#remote_activity_poll_delay_ms). The read is the only confirmation. Nothing replies to
  a 1W command, so a device that ignored it, for example while a rain or wind sensor holds its
  priority lock, shows the commanded state only until the read corrects it.
- A command that did not reach the radio, and the enrollment frames, change nothing.
- The state follows what went out on the wire, so an inverted device kind and `invert_position`
  need no extra setting.
- The hub fires no [sender event](remotes.md#sender-events-remote-buttons-and-sensors) for its own commands.

One identity can drive several devices, and `drives:` accepts covers and lights. A command addresses
the identity's `io_device_type`, so a driven device of another type is read afterwards but does not
show the commanded state at once; with `execute_broadcast: all` every driven device does.

**Example: a cover that follows its one-way identity.** When you command a motor through a one-way
identity it is enrolled on (its buttons, or an automation that presses them), list the cover under
`drives:`, so the cover shows each of those commands at once and reads the motor afterwards. Here
the identity's STOP button stops the awning, and the cover follows:

```yaml
home_io_control:
  oneway_controllers:
    - id: awning_remote
      io_device_type: vertical_exterior_awning
      manufacturer: somfy
      enrollment: true
      commands: [stop]
      drives: [FEEB1E]

cover:
  - platform: home_io_control
    name: "Awning"
    io_device_id: "FEEB1E"
    io_device_type: vertical_exterior_awning
```

```yaml
action: button.press
target:
  entity_id: button.awning_remote_stop
```

The cover shows stopped at once and reads the motor straight after the burst. The cover's own stop
is unchanged: this does not reroute `cover.stop` over 1W.

## A cover for the identity

An identity of a cover kind can show up in Home Assistant as one cover: one card with the usual open,
close and stop arrows, usable in scenes and voice assistants. Name it with `cover:`
([ADR 0117](../adr/0117-a-one-way-controller-can-show-an-assumed-state-cover.md)):

```yaml
home_io_control:
  oneway_controllers:
    - id: patio_awning
      io_device_type: vertical_exterior_awning
      manufacturer: somfy
      commands: [open, close, stop]
      cover: "Patio Awning"
      follows: [9D6085]   # the wall remote of this awning; see below
```

The entity sends the same one-way commands as the identity's buttons, one sequence each, and keeps the
buttons you list under `commands:`. A one-way device never reports a position, so the cover shows one
only when you give it the travel times; see [A position from the travel time](#a-position-from-the-travel-time).
The rest of this section describes the cover without them.

**What the cover shows.** The hub has no reply to read, so the cover shows what it heard of, and Home
Assistant marks it as an assumed state (both arrows stay usable whatever it shows):

- **Open or close** from the cover or the identity's own buttons shows open or closed as soon as the
  burst has gone out. The hub does not model the travel, so there is no opening or closing phase.
- **Stop** shows the cover as open, because Home Assistant has no "stopped half-way" state for a
  cover without a position; it is anything but fully closed.
- **A command that did not reach the radio** changes nothing.
- **A position** sent with [`oneway_set_position`](actions.md#oneway_set_position) shows its nearer
  end. A favorite, a ventilation position and a position of exactly 50 % change nothing.
- **A remote listed under `follows:`** moves the state the same way when the hub hears it, and the hub
  sends nothing for it, so the device sees one command, not two. Each press counts once however often
  the remote repeats it. A remote that is not listed never moves the state, so a neighbour's remote
  of the same kind cannot. Find a remote's address with
  [sender events](remotes.md#sender-events-remote-buttons-and-sensors).
- **An inverted kind** such as a horizontal awning, or an identity with `invert_position`, needs
  nothing more: "open" means open for you, for commands and for the remote's presses alike.

**The state is an assumption, and it is wrong when:**

- the hub did not hear a press of the real remote (it was out of range, or the hub was busy with a
  radio exchange);
- the device ignored a command: it is out of range, its counter has desynced, or a rain or wind sensor
  holds a priority lock on it, in which case the device ignores a one-way command without any sign;
- the motor was stopped or moved by hand, or by a remote you did not list.

The hub cannot tell any of these apart from a command the device obeyed. Pressing the cover again
re-sends the command and puts the shown state back in line with what you last asked for.

**After a reboot** the hub does not remember the state: it persists nothing but the counters
([ADR 0018](../adr/0018-yaml-is-the-source-of-truth-hub-persists-nothing.md)). ESPHome's cover shows
open until the first command or press arrives, whatever the awning really does.

### A position from the travel time

Add `open_duration:` and `close_duration:` to the identity and the cover shows where it is: the hub
works the position out from the time a full run takes and from the commands and presses it knows of
([ADR 0127](../adr/0127-a-one-way-cover-shows-a-position-from-its-travel-time.md)). Home Assistant shows
the percentage, and "opening" or "closing" while the cover moves. The position updates about once a
second and again when the cover starts, stops, turns around or arrives. It follows the wall remote of a
`follows:` entry the same way.

```yaml
home_io_control:
  oneway_controllers:
    - id: patio_awning
      io_device_type: vertical_exterior_awning
      manufacturer: somfy
      commands: [open, close, stop]
      cover: "Patio Awning"
      follows: [9D6085]
      open_duration: 30s    # full run from closed to open
      close_duration: 28s   # full run from open to closed
```

What moves the position:

- **Open and close** start a run towards that end at the matching speed. A run that lasts the whole
  duration ends at the end, whatever the position was before.
- **Stop**, sent by the hub or heard from a followed remote, freezes the position where it is, and the
  cover shows it idle.
- **A position** sent with [`oneway_set_position`](actions.md#oneway_set_position) or heard from a
  followed remote makes the position travel to that percentage and stop there. The hub assumes the
  device does the same, which no device on record has confirmed.
- **A favorite or ventilation press** changes nothing: the hub does not know where that position is.
- **A command that did not reach the radio** changes nothing.

**The position is an estimate, and it is wrong when** the hub missed a press of the real remote, the
device ignored a command, a favorite or ventilation press moved the cover, the motor was stopped or
moved by hand, or the hub rebooted. The hub cannot tell any of these from a correct run. A full run to
an end corrects the position: send open or close and let it finish. After a reboot the cover shows fully
open and idle; the position is not stored.

### A slider value on the cover

With travel times the cover's slider moves it to a percentage, usable in scenes and automations
([ADR 0128](../adr/0128-a-one-way-light-shows-an-estimated-level-and-a-slider-value-is-a-timed-stop-or-a-position-frame.md)).
A value reaches the device in one of two ways, chosen with `set_position:`:

- **`timed`** (default) sends open or close towards the value and, when the estimate reaches it, one stop. It needs
  only what every one-way cover obeys: open, close and stop. The hub starts counting when the command has reached the
  radio, so the stop lands when the cover is where you asked. A value of 0 or 100 % sends the plain close or open and
  no stop, so the end stop of the motor corrects the estimate. A new value, a stop and the press of a followed remote
  cancel a stop that is still waiting, so a stale stop is never sent, and a new value is worked out from the position
  reached so far; a value the cover has to go back for sends the opposite command. A button or action of the same
  identity queued just before a value does not cost it its stop: the hub tells the report of the value's own command
  from the others. One queued after the value is the last command and wins: it cancels the value's stop. If a run
  that is already going passes the value before the command reaches the radio, the hub sends a stop or the way back
  instead. The stop goes out as a burst of four copies, and it waits behind any command still in the queue, so the cover stops a little
  past the value; the shown position freezes when the burst is out and includes that lag. **A stop the motor misses
  runs the cover to its end**, while Home Assistant shows it stopped at the value: the hub cannot tell. The shown
  position stays wrong until the next run to an end.
- **`frame`** sends one position frame with the percentage, and no stop. A device that ignores a position frame does
  nothing with `set_position: frame`, so try `timed` first on a cover nobody has tried it on. The cover shows the position travelling to the value at the travel rate, like any position it hears,
  and a value of 0 or 100 % is the same frame as open or close.

```yaml
home_io_control:
  oneway_controllers:
    - id: patio_awning
      io_device_type: vertical_exterior_awning
      commands: [open, close, stop]
      cover: "Patio Awning"
      open_duration: 30s
      close_duration: 28s
      set_position: timed   # or frame
```

A bench run on a Somfy Sunea io awning, with each result read back over two-way, moved it to 50 % both ways. With
`timed` (35.5 s open, 38 s close) the stop went on the air 19.1 s after the close, and the awning rested at 50 and 51 %
in three runs. With `frame` one position frame moved it to 50 %. See [Somfy Sunea IO](../devices/somfy-sunea-io.md).

`set_position:` without `open_duration:` and `close_duration:` is a configuration error: without travel times there
is nothing to work a stop out from. Without them the slider has no values between the ends and a value goes out as
open or close, whichever end it is nearer, and 50 % opens. The `time_based` recipe in
[Composing a slider from 1W buttons](../tips-and-tricks.md#composing-a-slider-from-1w-buttons) is still the better
choice when one cover has to send more than one identity's own open, close and stop, for example several identities
moved together, or a script of yours around each command: its actions are any ESPHome actions.

## A light for the identity

An identity of a light kind can show up in Home Assistant as one light: on and off, and with
`dimmable: true` a brightness slider, usable in scenes and voice assistants. Name it with `light:`
([ADR 0128](../adr/0128-a-one-way-light-shows-an-estimated-level-and-a-slider-value-is-a-timed-stop-or-a-position-frame.md)):

```yaml
home_io_control:
  oneway_controllers:
    - id: hall_lamp
      io_device_type: light
      manufacturer: somfy
      commands: [open, close, stop]
      light: "Hall Lamp"
      dimmable: true
      follows: [9D6085]   # the wall remote of this lamp
      on_duration: 4.7s   # full fade from off to full
      off_duration: 4.7s  # full fade from full to off
```

The light sends the same one-way commands as the identity's buttons, one sequence each: on is the identity's
open and off is its close. It shows what the hub heard of, like [the cover](#a-cover-for-the-identity), and
follows the presses of the remotes under `follows:` the same way. Without `dimmable:` it is an on and off light;
with `dimmable:` and no durations a slider value goes out as on or off, whichever end it is nearer.

**A dimmer fades, so the level is an estimate.** Home Assistant's transition stays off (length 0): the device
fades by itself, and the light shows that fade. A transition you ask for anyway, for example in a scene, sends only
its end value. With `on_duration:` and `off_duration:` the hub works the level out from the time a full fade takes,
as the cover does from its travel time ([A position from the travel time](#a-position-from-the-travel-time)): on
starts a fade towards full, off a fade towards off, and a stop, sent by the hub or heard from a followed remote,
freezes the level where it is. The level updates about once a second while it fades. Time a full fade once and type it in.

**A slider value** reaches the device in one of two ways, chosen with `set_position:`:

- **`timed`** (default) sends on or off towards the value and, when the estimate reaches it, one stop. The hub
  starts counting when the command has reached the radio, so the stop lands when the fade is where you asked. A value
  at an end sends the plain on or off and no stop; the end stop of the device then corrects the estimate. A new
  value, a stop and the press of a followed remote cancel a stop that is still waiting, so a stale stop is never
  sent, and a new value is worked out from the level reached so far. A button or action of the same identity queued
  just before a value does not cost it its stop: the hub tells the report of the value's own command from the
  others. One queued after the value is the last command and wins: it cancels the value's stop. A button pressed in
  the same moment as a value can cost the value itself: when the hub sends the button's command before the light has
  handed the value on, the level that command reports replaces the value, and nothing goes out for it, so send the
  value a moment after the button. If a fade
  that is already running passes the value before the command reaches the radio, the hub sends a stop or the way back
  instead. The stop goes out as a burst, so the level lands slightly past the value, and a stop the device misses runs the fade to its end.
- **`frame`** sends one position frame with the level, and no stop. The device is expected to go to the level at
  once, and the light shows it at once. Full and off fade like on and off.

A bench run on a Somfy Izymo dimmer, with each step read back over two-way, measured a linear fade of about
4.7 s from off to full and the same back; a one-way stop during a fade froze the level, four of four within 1 % of
the level a linear fade predicts for the moment the stop went out; and a one-way position frame was obeyed, with
the level reached at once. The light entity was then run on that dimmer, each result read back over two-way: in each
way, on, off and the levels 25, 50 and 75 % ten times each, 50 of 50 as asked. With `frame` the dimmer reached
each level exactly; with `timed` it landed 2 points brighter every time (73, 48 and 23 on the device's scale for 25,
50 and 75 %), the lag of the stop burst ([Somfy Izymo IO dimmer](../devices/somfy-izymo-dimmer.md)). A device
that ignores a position frame does nothing with `set_position: frame`: try the `timed` way there.

**The level is an assumption, and it is wrong when** the hub missed a press of the real remote, the device
ignored a command, the lamp was dimmed by hand or by a remote you did not list, or the hub rebooted. The hub cannot
tell any of these from a correct fade. A full on or off corrects the level: send it and let it finish. After a reboot
the light shows off; nothing is stored.

## Continuous control

A cover with travel times has a slider ([A slider value on the cover](#a-slider-value-on-the-cover)), and a
dimmable light has one ([A light for the identity](#a-light-for-the-identity)). Without travel times a one-way cover
entity has no position. To get a slider there, compose one from the generated buttons:
[Composing a slider from 1W buttons](../tips-and-tricks.md#composing-a-slider-from-1w-buttons) shows how.

## Numeric positions

For anything other than the generated buttons there is an action
([ADR 0006](../adr/0006-management-actions-as-native-api-actions.md)):

```yaml
action: esphome.<device_name>_oneway_set_position
data:
  controller_id: velux_windows
  position: "40"
```

`position` runs 0 (fully open) to 100 (fully closed) and is passed as a string, like every argument
on this component's action surface; [`oneway_set_position`](actions.md#oneway_set_position) lists
the fields and the result event. It is the position you mean: for an identity of an inverted kind
(see `invert_position`) the hub mirrors it on the wire, and the "Last 1W Command" sensor still
names what you asked for.

The result event reports only that the command was **queued**. Nothing downstream can ever upgrade
that to "the device moved".

## The "Last 1W Command" sensor

Every identity gets one, and it is the only feedback this feature can produce:

```
CLOSE -> window_opener seq 4013
```

It reports what the hub **transmitted** — never that a device acted, because that is not knowable.
It also shows the **sequence** used, which is the number you need when a command is ignored.

The entity is named after the identity: an identity with `id: awning_remote` produces
`awning_remote_last_1w_command`.

## When a 1W command does nothing

1W has no reply frames, so there is nothing to read back when a command is ignored.
[1W commands do nothing](../troubleshooting.md#1w-commands-do-nothing) has the diagnostic ladder
for it.

## What is stored on the device

The rolling sequence counter, and nothing else. It is the one exception to this component's
otherwise absolute rule that YAML is the only source of truth, because the counter is neither
configuration nor re-learnable from the air — a 1W device never transmits, so nothing reports the
high-water mark your counter has to stay ahead of.
[ADR 0025](../adr/0025-persist-monotonic-counters-as-the-only-exception-to-0018.md) records the
exception and its cost; the practical consequence is that **replacing your board loses the counters**, and the identities
on the new board need a higher number once: call [`oneway_set_sequence`](actions.md#oneway_set_sequence) with a value above the last one the "Last 1W Command" sensor showed for the old board (its history in Home Assistant keeps it), or raise `initial_sequence:` and reflash.

## See also

- [Linked remotes and sender events](remotes.md) — the receiving side: overhearing a remote rather
  than transmitting as one
- [Composing a slider from 1W buttons](../tips-and-tricks.md#composing-a-slider-from-1w-buttons) —
  turning the generated buttons into a position control
- [1W commands do nothing](../troubleshooting.md#1w-commands-do-nothing) — when nothing moves and
  there is no reply to tell you why

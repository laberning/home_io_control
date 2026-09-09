# Sending 1W commands
<!-- doxygen-label: cfg_oneway -->

The hub can act as a **1W controller** — the kind of thing a wall remote is — and drive devices by
transmitting. This is off unless you configure it, and it signs with a key you already hold: the
same authorisation as any 2W command this component sends.

> **⚠️ Read this before configuring: a device only obeys a controller it has been taught.**
> Real-hardware testing established that 1W actuators keep a **table of registered controllers**.
> A frame that is correctly built, correctly addressed and signed with a key the device accepts is
> still ignored if this hub's source address is not in that table. **Enrollment (below) is what
> registers it** — do that first, for every identity, before expecting any command to move a device.

## What 1W is, and what it is not

Everything about this feature follows from two properties of the protocol:

- **A 1W command addresses a device *class*, not a device.** There is no unicast form. A command
  sent as a `roller_shutter` identity reaches every roller shutter in range that holds the signing
  key. That is what 1W *is*, not a limitation to design around.
- **Nothing replies.** No acknowledgement, no status, no error. A command a device ignored is
  indistinguishable on the radio from one it obeyed.

The second one shapes the whole feature. There is no failure you can be notified about, so the
"Last 1W Command" sensor and the section below are the diagnostic tools.

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
      enrollment_with_mac: false                        # optional -- see the table below
```

| Key | Required | Meaning |
|---|---|---|
| `id` | yes | Handle the generated entities are named and ID'd from. |
| `io_device_type` | yes | The device class this identity commands — see the [named device types table](../supported-devices.md#named-device-types) for the full list. |
| `node_id` | no | Source address to transmit as. **Derived from your hub's `node_id` and this `id` when omitted**, deterministically at *compile* time — a derived address takes part in the same collision checks as an explicit one (a clash fails the build), and is printed at boot marked `(derived)`, since nothing in your YAML shows it otherwise. Asking you to invent a 3-byte radio address instead would be an unanswerable question: nothing tells you which addresses are safe, and colliding with a real remote in range silently desyncs both transmitters' counters. |
| `system_key` | no | Network key for this identity. **Defaults to the hub's own** — every new identity works fine with the default. Only set this to a recovered key when reusing an existing, already-registered identity (see [Recovering a 1W controller key](#recovering-a-1w-controller-key)). |
| `initial_sequence` | no | Seeds the rolling counter. The day-one remedy for a desynced device — see troubleshooting. |
| `commands` | no | Which buttons to generate: `open`, `close`, `stop`, `vent`, `favorite`. `open`, `close` and `stop` match captured presses of a real Somfy remote. `vent` is not backed by any capture. `favorite` is extrapolated from the 2W encoding and contradicted by this project's own capture of a real My-button press, which encodes it differently; on a real dimmer it changes the brightness to an unverified target. Treat `vent` and `favorite` as untested. |
| `manufacturer` | conditional | The manufacturer ID byte an enrollment frame carries on air — a named value such as `somfy`, or a raw integer; see the [named manufacturers table](../supported-devices.md#named-manufacturers) for the full list. **Required whenever `enrollment: true` is set** — the build fails otherwise, rather than silently broadcasting `0`. Also selects the **1W wire profile** (see "Matching your remote's vendor" below): `velux` sends `CMD_EXECUTE` frames with a different priority byte than `somfy`. Find the value in a [key recovery](#recovering-a-1w-controller-key) report for this network, or in the device's own documentation. |
| `execute_broadcast` | no | `typed` (default) addresses your `io_device_type` class; `all` addresses the all-devices broadcast, which is what a handheld cover remote of either vendor sends for open/close/stop. Set `all` to mimic a real remote. |
| `execute_acei` | no | Raw override of the priority byte (payload[1]) in a 1W `CMD_EXECUTE` frame, e.g. `0x61`. Must be `1`–`0xFF` — `0x00` is rejected, since `0` is the sentinel for "not overridden". Normally left unset — it is derived from `manufacturer`. Use it only for a vendor this project has no profile for. |
| `enrollment` | no | Build flag (default `false`) for this identity's **"Enroll 1W Controller"** button — see "Enrolling this hub as a controller" below. |
| `enrollment_with_mac` | no | Whether the `0x30` half of the enroll button's press carries a trailing MAC (default `false`, meaning **no MAC at all** — there is no in-band form for this frame, see below). Real hardware disagrees on this byte: most captures this project holds carry no MAC (the default), but a real Somfy Izymo has separately been shown to accept the MAC-bearing form too. Untested manufacturers may need either — try flipping this before assuming enrollment doesn't work at all. |
| `enrollment_classes` | no | Which device classes a **VELUX** enrollment `0x30` sweep targets, as a list of `io_device_type` names (max 3). Unset → the profile default `[roller_shutter, awning, dual_shutter]` — the exact set a real KLI PROG press sweeps. Set it (e.g. `[awning]`) to narrow the sweep once you know which class your actuator listens on. **Ignored by the Somfy gesture**, which always uses `io_device_type`. See "Enrolling this hub as a controller" below. |

## Matching your remote's vendor

A 1W `CMD_EXECUTE` frame (open/close/stop) differs between vendors in exactly one byte: the
priority/ACEI byte at payload position 1. **Somfy remotes send `0x43`; VELUX KLI-class remotes
send `0x61`.** The hub picks this from `manufacturer:` — `somfy` (or omitted) gives `0x43`; `velux`
gives `0x61`. Any other vendor falls back to `0x43`, since this project has only
verified the two; if that vendor was set explicitly (not defaulted) on an identity that actually
transmits — one with `commands:` or `enrollment: true` — the build also logs a warning.

To mimic a handheld remote, also set `execute_broadcast: all` — real remotes broadcast open/close/
stop to every device, not to one class. The boot log prints the resolved `acei` and `broadcast`
for each identity; frame-log a press of your real remote and compare `cmd 0x00 payload[1]` and the
destination address to confirm.

The VELUX profile is verified against KLI-class exterior-shading remotes only (awnings, screens,
roller shutters). A VELUX window-opener or KLR-class remote may differ — use `execute_acei:` if so.

There is no 1W `force_open` button, because no 1W frame is known to carry that meaning: the byte
sometimes labelled "force open", `0x64`, moves a real device to an ordinary 50% position when sent
as a command. The hub's force-open (`force_open_device`) is a separate, 2W, per-device action — see
[Home Assistant actions](actions.md).

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

**The gesture is two-sided, and only one half is a button press.**

| Half | Who does it | What it is |
|---|---|---|
| Receiver enters association mode | **you, physically** | **2 second** hold on the actuator's PROG button, confirmed by its own indicator |
| Controller offers its credential | **the hub** | one **short** press of the "Enroll 1W Controller" entity |

Get the order and the durations right: 2 seconds on the receiver, *then* one press on the hub — not
the other way around, and not a long hold on the hub's entity (there is nothing to hold; a press is
a press). Getting the two halves' timing backwards is the most common failure mode here, not a
protocol problem.

The press sends two bursts back to back — `0x39` (self-directed; carries only this identity's own
address, so it can only ever clear its own prior entry, never a different controller's),
immediately followed by `0x30` (the credential itself) — each ~125 ms, ~4 copies, no gap beyond the
bursts' own airtime. This is the documented 1W pairing handshake (a real Somfy Smoove remote does
exactly this, corpus-captured), not two independent actions. There is no "learn window" on the
hub's side beyond that, because the device owns its own timeout and there is nothing further to
wait for. **Only one device should be in association mode at a time**: the frames reach every
device of that class in range that is currently listening, so a second actuator in learn mode
nearby would be taught too.

The `0x30` half's MAC trailer is controlled by `enrollment_with_mac:` (default `false`, no MAC at
all) — see the option table above if enrollment doesn't take with the default shape.

### VELUX (`manufacturer: velux`) uses a different gesture

A real VELUX KLI 310/311/312/313 PROG press does not enroll the way a Somfy Smoove does, so with
`manufacturer: velux` the Enroll button emits the KLI gesture instead
([ADR 0032](../adr/0032-oneway-velux-enrollment-gesture.md)):

1. `0x39` clear to the all-devices address (not the identity's typed class).
2. `0x30` add-controller **swept across `roller_shutter`, `awning`, `dual_shutter`** under one
   sequence — never `screen` / `blind` / `venetian_blind`, which no VELUX remote pairs on. The
   actuator filters by its own class; only the matching frame registers the hub. Narrow the sweep
   with `enrollment_classes:` once you know which class yours listens on.
3. A STOP then a DOWN command to the all-devices address — the KLI manual's "then press STOP then
   DOWN within 3 seconds" registration completion.

So for a VELUX exterior-shading device, set `io_device_type:` to whatever the device actually is
(it drives the control-frame destination), and leave `enrollment_classes:` unset unless you need
to narrow the sweep — the enroll gesture ignores `io_device_type` and uses the three-class list.
The Enroll button blocks for up to ~6 seconds while it sends all six bursts.

> **⚠️ Pressing Enroll physically moves your covers.** Step 3 is a real STOP then a real DOWN
> broadcast to *every* 1W-enrolled cover in range that holds the key — they will close. That is
> the KLI registration gesture, not a side effect to fix, but it is worth knowing before you press
> the button.

> **⚠️ VELUX 1W enrollment is unconfirmed.** No hub has been shown to 1W-enroll on any VELUX
> actuator (issue #74). The frame shapes are reconstructed from a real KLI 310 capture; the
> STOP+DOWN step in particular is not confirmed against a VELUX capture. Two likely blockers: the
> actuator's own 2-second PROG window must be open at the moment the `0x30` sweep transmits (hold
> PROG on the actuator, then press Enroll), and the sweep plus STOP plus DOWN may not fit the
> manual's own 3-second window at this radio cadence
> ([ADR 0032](../adr/0032-oneway-velux-enrollment-gesture.md)).

**A hub cannot enroll into a device nobody has walked up to.** The receiver's physical PROG hold is
the real safety interlock here, stronger than any software confirmation could be — it is why this
feature has no separate arming switch the way the (irreversible) LR1121 bootloader rewrite does.

**Un-enrolling without re-enrolling** is reached through its own explicitly-named action — the
Enroll button's `0x39` above always re-registers via the `0x30` that follows it, so this is the
path when you want the removal without the re-add:

```yaml
- action: esphome.<device_name>_oneway_remove_controller
  data:
    controller_id: velux_windows
```

This sends `0x39` alone, nothing else. It carries only this identity's own source address, so it
cannot remove a different remote's registration — the same property that makes the Enroll button's
`0x39` prelude safe to send automatically.

> **⚠️ Un-enrollment is unconfirmed on real hardware.** This action has not been shown to have any
> effect on real hardware — the hub keeps controlling the device afterwards regardless. The most
> likely explanation, by analogy with enrollment itself, is that a device only acts on `0x39`
> while its receiver is in the same **2 second PROG association mode** enrollment needs. Treat
> "un-enroll" as the documented design intent, not a confirmed rollback, until this is retested
> with that gesture.

**When you are done enrolling**, remove `enrollment: true` from the identity and reflash. A build
that can put a device into someone else's controller table should not be the build that runs
permanently — the same reasoning as removing `recover_oneway_key: true` after a key recovery.


## Recovering a 1W controller key

> **⚠️ Receive-only, but it recovers a real secret.** This feature never transmits anything. It
> listens for a frame a 1W remote broadcasts during its key-copy gesture and decrypts it. The
> decryption needs no secret of its own — see the security note below — so treat the recovered key
> exactly as you would treat your `system_key`. Recover a key only from a remote or network you own
> or are authorized to access — see the project [Disclaimer](../../README.md#disclaimer--license).

One-way (1W) installations — a handheld remote driving a shutter or awning directly, with no hub —
have their own network key. This feature recovers it by overhearing a single `0x30` "add
controller" broadcast, which is what a 1W remote sends while its **remote-to-remote key-copy mode**
is active in order to hand its network key to a new remote.

**For controlling a device, you almost never need this.** A device accepts whichever key arrives
during its own association-mode window — enrolling this hub as a controller (see
[Enrolling this hub as a controller](#enrolling-this-hub-as-a-controller) above) works just as
well with a freshly generated key as with a recovered one, so there
is nothing to gain from recovering a key just to enroll with it. Recover a key only when you
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
  was just recovered*. This is the strongest confirmation available without commanding a device.
- **`MAC FAILED`** — the authenticator did not check out. The key is probably wrong (a marginal
  reception is the usual cause). Re-arm and repeat the gesture closer to the hub.
- **`MAC not present`** — the frame carried no authenticator to check. Not an error; real hardware
  frequently omits it. The key may still be correct — enroll with it (see
  [Enrolling this hub as a controller](#enrolling-this-hub-as-a-controller)) and confirm by
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

These are created from the `oneway_controllers:` block rather than declared as
`button: - platform: home_io_control` entries, deliberately. A platform entry would have to infer
what the button *is* from which keys are present, so a device-bound entry that merely omitted its
`io_device_id` could be misread as the security-sensitive 1W kind instead of failing validation.
Creating these from the hub block makes that class of mistake structurally impossible.

## Continuous control

There is no 1W cover entity, so a slider has to be composed from the generated buttons —
[Composing a slider from 1W buttons](../tips-and-tricks.md#composing-a-slider-from-1w-buttons)
shows how.

## Numeric positions

For anything other than the generated buttons there is an action
([ADR 0006](../adr/0006-management-actions-as-native-api-actions.md)):

```yaml
- action: esphome.<device_name>_oneway_set_position
  data:
    controller_id: velux_windows
    position: "40"
```

`position` runs 0 (fully open) to 100 (fully closed) and is passed as a string, like every argument
on this component's action surface.

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
on the new board will need `initial_sequence:` raised once.

## See also

- [Linked remotes and sender events](remotes.md) — the receiving side: overhearing a remote rather
  than transmitting as one
- [Composing a slider from 1W buttons](../tips-and-tricks.md#composing-a-slider-from-1w-buttons) —
  turning the generated buttons into a position control
- [1W commands do nothing](../troubleshooting.md#1w-commands-do-nothing) — when nothing moves and
  there is no reply to tell you why

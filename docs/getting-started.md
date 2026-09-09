# Getting started
<!-- doxygen-label: getting_started -->

From a board in your hand to a blind moving in Home Assistant. Allow about an hour the first time,
most of which is flashing and waiting.

## Before you start

You need three things:

- **A supported board.** An ESP32 with an SX1276, SX1262 or LR1121 radio at 868 MHz. If you have
  not bought one yet see [Hardware](hardware.md) for some recommendations.
- **A working ESPHome setup**, with Home Assistant already adopting ESPHome devices. If ESPHome is
  new to you, follow [their getting-started guide](https://esphome.io/guides/getting_started_hassio.html)
  first and come back once you can flash a blank node.
- **An IO-Homecontrol device within radio range**, and — this is the part worth checking now —
  either the ability to put it into pairing mode, or an existing hub that already controls it.
  Which of those you have decides your route in step 3.

A short antenna and a device two rooms away is usually fine. The protocol is not fussy about
signal, and [Link Health](diagnostic-entities.md#link-health) will tell you later if it is.

## 1. Make up a node ID and a system key

Your hub needs two identifiers before it will boot. You invent both. There is nothing to look up
and nobody issues them to you.

- **`node_id`** — this hub's own address on the radio. Exactly **6** hex characters.
- **`system_key`** — the shared secret that every device on your installation is paired against.
  Exactly **32** hex characters.

**Hex** is short for hexadecimal, and it means the only characters allowed are the digits `0`–`9`
and the letters `A`–`F`. So `C0FFEE` is a valid node ID; `G0FFEE` is not, because `G` is not one of
them. Upper and lower case both work.

Two things worth knowing before you pick:

- **The system key is a credential**, not a serial number. Anyone who has it can drive your
  devices. Keep it out of public issue reports and put it in ESPHome's `secrets.yaml` rather than
  in the config you might paste somewhere.
- **Keep both stable.** Changing either one after you have paired devices means pairing all of them
  again.

## 2. Flash the component

This config gives you a working radio and the entities you need to go find your devices. There are
no `cover:` or `light:` entries yet, because you do not know your devices' addresses — those come
in step 3.

The pin block is for a **Heltec WiFi LoRa 32 (V3)**. For any other board, swap in its pins from
[Hardware](hardware.md#all-supported-boards).

<!-- board-pinout: heltec-v3 -->
```yaml
esphome:
  name: io-homecontrol

esp32:
  variant: esp32s3
  framework:
    type: esp-idf

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password

logger:
  level: DEBUG

api:

external_components:
  - source: github://laberning/home_io_control

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

  node_id: "A1B2C3"                 # yours, from step 1
  system_key: !secret io_system_key # yours, from step 1

  # Onboarding helpers. Each adds one Home Assistant entity that you need while setting
  # up and will not touch again afterwards -- see "Tidying up" at the end of this page.
  accept_foreign_pairing: true      # "Recover System Key" switch, for Route B
  scan_paired_devices_button: true  # "Scan Paired Devices" button, lists devices that
                                    # already trust your key

# The Discover & Pair button is not created automatically -- this block is what adds it.
button:
  - platform: home_io_control
    name: "Discover & Pair"
```

`logger:` at `DEBUG` matters here: every route below reads its result out of the log.

Flash it and watch that log. You are looking for the radio to come up — a boot block naming your
chip and its version. If it does not appear, the pins are wrong, and nothing later will work until
that block is clean.

## 3. Get your devices onto the hub

### First, 1W and 2W

IO-Homecontrol devices speak two dialects:

- **1W (one-way)** is what an ordinary wall remote uses. It transmits and nothing answers back, so
  the remote never learns whether the device actually moved.
- **2W (two-way)** is what a hub uses. Commands are authenticated with the system key, and the
  device replies with its real position and state.

This component speaks both, but 2W is the one that gives you position feedback in Home Assistant,
and it is what pairing sets up.

The two are independent of each other. A shutter can take orders from three wall remotes and still
never have met a hub. So the question that picks your onboarding route is: **Is the device already
paired to a hub?**

That matters because a device that has been paired holds that hub's system key, and a device
holding a key has nothing left to answer a fresh discovery with. Wall remotes do not claim a device
this way; a hub does.

### Route A — no hub has claimed it

For a device that is new, factory reset, or has only ever been driven by wall remotes.

1. Put the device into pairing mode, usually a 2 second press of the motor's own PROG button.
   On some devices the PROG button on a paired wall remote does the same. If the motor has no
   reachable button, a Double Power Cut normally forces it into learning mode; check its manual
   for the exact sequence.
2. Press **Discover & Pair** in Home Assistant within a few seconds of that.
3. Read the log. On success it prints a ready-to-paste block with the device's `io_device_id`,
   `io_device_type` and `io_subtype`.

**Expect this to take a few goes.** The device's pairing window is only a few seconds wide and the
timing between the PROG press and the button press is genuinely fiddly — the hub makes three
attempts per press, and it is still common to need two or three presses. Press PROG again and repeat
before you change any settings. Pair one device at a time.

[Pairing](pairing.md) has the full walkthrough and a decision tree.

### Route B — a hub already controls it

For a device controlled today by a Somfy TaHoma, Connexoon or Connectivity Kit, a VELUX KLF200,
KLR200 or KIG300, or similar.

**Discover & Pair cannot reach these devices at all**, however carefully you time it. You have two
ways forward.

#### B1 — take the key from your existing hub

Try this first: nothing gets reset, and every device stays paired exactly as it is.

1. Turn on the **Recover System Key** switch in Home Assistant. The hub arms for 10 minutes.
2. Put your existing hub into its own "add a device" mode — the same wizard you would use to add a
   new shutter to it. Your hub pairs to this component as though it were a new device.
3. The log prints your installation's real `node_id` and `system_key`.
4. Paste both into your YAML, replacing the values you made up in step 1, and reflash.
5. Press **Scan Paired Devices**. Every device that trusts that key answers with its own
   ready-to-paste block.

**If a device you expected is missing from the scan, press the button again.** Paired devices
duty-cycle across the three radio channels on their own schedule, so the hub sweeps all three on
every scan and one can still be missed. A repeat press has no side effects.

**If a device never turns up, you can still write its entity by hand.** All you need is its
`io_device_id`. Turn on frame logging:

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
pick `io_device_type` from [Named device types](supported-devices.md#named-device-types) to match
what the device actually is.

[Key extraction](key-extraction.md) covers this route in detail.

#### B2 — factory reset the device, then use Route A

This works, but it drops the device from your existing hub, and you repeat it for every device.
The reset is usually a Double Power Cut plus a device-specific sequence of button presses; check
the device's manual for the exact gesture.

## 4. Add your devices to the YAML

Either route leaves you with the same three values per device, printed in the log. Add one entity
per device, on the platform that matches what it is: `cover:` for shutters, blinds, awnings and
window openers, or `light:`, `lock:`, `switch:` and `climate:` for the rest.

This example is a cover — see [Configuration](configuration/index.md) for the other platforms:

```yaml
cover:
  - platform: home_io_control
    name: "Patio Awning"
    device_class: awning        # from the log
    io_device_id: "FEEB1E"      # from the log
    io_device_type: "awning"    # from the log
    io_subtype: 0               # from the log
```

Take `io_device_type` from the log rather than guessing — it is what the device reports itself to
be, and it may not be the class you expected. [Named device types](supported-devices.md#named-device-types)
lists every value the option accepts. If the log printed a raw numeric type such as `0x11`, keep
that exact value.

Declaring `io_device_type` is worth doing even though it is optional. It is what enables
type-specific behaviour: an `awning` gets a `Patio Awning Favorite Position` button, a
`window_opener` also gets a Ventilation Position button, and families such as horizontal awnings
get their direction inverted automatically. These entities are generated at compile time, so a type
learned later over the radio cannot add them.

A few additional options worth knowing about:

- **`low_power: true`** — set this on a solar or battery actuator. Leaving it off on such a device
  is a common reason a hub pairs successfully and then hears nothing back.
- **`invert_position: true`** — if open and closed come out backwards.
- **`silent: true`** — move at the manufacturer app's quieter, slower speed.

The rest are in [Covers](configuration/cover.md), and on each platform's own page.

Reflash with the updated YAML.

## 5. Check it works

Your devices appear in Home Assistant as native entities. Operate one — open a cover, toggle a
light, unlock a lock.

What success looks like: the entity responds immediately (that is the optimistic state), the device
itself moves or switches, and a second or two later the entity settles to what the device actually
reports back. That settle is the 2W half doing its job.

If nothing happens, the log is the place to look. [Troubleshooting](troubleshooting.md) is indexed
by what you are seeing rather than by subsystem, so start there and work from the symptom.

## Tidying up

The three onboarding entities from step 2 have done their job once your devices are in the YAML.
Drop `accept_foreign_pairing:` and `scan_paired_devices_button:`, and the `button:` block with
them, then reflash.

None of them is dangerous to leave in place; this only spares you three controls you will never
press in normal operation.

## Next steps

- **Group the entities** so a cover and its companion buttons appear as one device in Home
  Assistant, using ESPHome's `device_id:` — see [Tips and tricks](tips-and-tricks.md).
- **Wire up your existing remotes** so a physical press keeps the entity's state fresh, or
  fires a Home Assistant automation — see
  [Linked remotes](configuration/remotes.md#linked-remotes).
- **Read the full reference** at [Configuration](configuration/index.md) for every option.
- **If the radio is unreliable**, [Radio tuning](configuration/tuning.md) covers timing
  and discovery parameters.

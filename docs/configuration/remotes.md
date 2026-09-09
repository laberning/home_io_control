# Linked remotes and sender events
<!-- doxygen-label: cfg_remotes -->

Two ways to make the hub react to a physical 1W remote or sensor: **linked remotes** keep an
entity's state fresh when someone presses a wall switch, and **sender events** turn a press into a
Home Assistant trigger. Both are receive-only — see [Sending 1W commands](oneway-transmit.md) for
the transmit side.

## Linked remotes

Physical IO-Homecontrol remotes (wall switches, handheld remotes, wind sensors) use the 1W (one-way) protocol to send commands. Unlike 2W devices that address a specific device ID, 1W remotes broadcast to a type-class address (e.g., "all awning devices"). This means the controller cannot automatically detect which of your devices a particular remote controls — you need to configure the link explicitly.

### Why link a remote?

Without `linked_remotes`, when someone presses a wall remote, the device moves but the controller doesn't know about it until the next scheduled poll. With `linked_remotes` configured, the controller overhears the remote's radio traffic and immediately polls the device for its new position (with a configurable poll interval).

### Finding your remote's node ID

1. Set the logger to DEBUG level in your YAML:

```yaml
logger:
  level: DEBUG
```

2. Flash the firmware and open the serial log or ESPHome logs.

3. Press a button on the physical remote.

4. Look for a log line like:

```
[D][home_io_control] rx 1W remote 9D6085 targets all: EXECUTE(0x00) CLOSE originator=user_remote priority=user_high
```

5. The 6-character hex ID after `1W remote` is your remote's node ID — in this example, `9D6085`.

**Notes:**
- Each button press may produce multiple log lines (a CMD 0x20 beacon and a CMD 0x00 execute). The source ID is the same on all of them — use either one.
- Wind and rain sensors also show up with `originator=wind_sensor` or `originator=rain_sensor`. These can be linked the same way if you want the controller to react to sensor-triggered movements.
- If you have multiple remotes, press each one separately and note which ID appears for each.
- Devices nearby on the same frequency will also appear in the log. If you're unsure which ID belongs to your remote, press the button a few times and look for the ID that consistently appears at the same time.

### Configuring linked remotes

Add the remote's node ID to the `linked_remotes` list on the entity it controls:

```yaml
cover:
  - platform: home_io_control
    name: "Patio Awning"
    io_device_id: "30E1F2"
    io_device_type: "awning"
    linked_remotes:
      - "9D6085"
```

Multiple remotes can be linked to the same device, and the same remote can be linked to multiple devices if it controls more than one:

```yaml
cover:
  - platform: home_io_control
    name: "Patio Awning"
    io_device_id: "30E1F2"
    linked_remotes:
      - "9D6085"
      - "A0A9A1"

  - platform: home_io_control
    name: "Bedroom Shutter"
    io_device_id: "054E17"
    linked_remotes:
      - "9D6085"
```

### Linking by device class

Instead of (or alongside) individual remote node IDs, a `linked_remotes` entry can be `class:<device_type>`, matching how 1W remotes actually address a device — a typed broadcast such as "all awnings" rather than a single node. This links *every* typed broadcast targeting that class to the device, without enumerating each remote's node ID:

```yaml
cover:
  - platform: home_io_control
    name: "Patio Awning"
    io_device_id: "30E1F2"
    io_device_type: "awning"
    linked_remotes:
      - "class:awning"
```

`<device_type>` accepts the same named values as `io_device_type` — see the [named device types table](../supported-devices.md#named-device-types) for the full list (e.g. `awning`, `roller_shutter`, `venetian_blind`) — or a raw hex ID such as `class:0x14` for a type without a named alias yet. Bare node-ID entries, named `class:` entries, and raw-hex `class:` entries can all be mixed freely in the same list, and a device linked both ways (by ID and by class) is only updated once per press — the class form is purely a convenience for "any remote that presses this device's type," it does not change how bare node IDs behave.

### Optimistic state

By default (`optimistic_state: true`), a linked remote's press — like a Home Assistant command — shows the requested direction immediately, before the confirming poll arrives. The prediction never overwrites the device's last reported values, and the poll supersedes it with real data. Set `optimistic_state: false` to fall back to polling only.

Notes:
- **A failed HA command reverts the prediction.** If the command never reaches the device — a silent timeout, or an explicit refusal such as `LIMITATION_BY_WIND` — the entity drops the assumed movement and goes back to the last position the device actually reported, instead of animating indefinitely. A linked-remote press has no such failure to detect and is only ever settled by the follow-up poll.
- **Tilt-only commands are not optimistic.** Setting only a tilt value (no position) queues the command normally but does not show an optimistic state change, since tilt has no equivalent "direction" concept in the cover UI. A combined position+tilt command *does* apply the optimistic position target.
- **Class-linked devices are safety-filtered.** When a typed broadcast (e.g. "all awnings") also fans out to `class:`-linked devices, a device is only optimistically moved if its own declared `io_device_type` matches the broadcast's target type — it is never skipped for polling, only for the optimistic nudge. This prevents an "all awnings" press from optimistically moving a device you've linked to that class but declared as a different type.

### What the decoded log tells you

| Field | Meaning |
|-------|---------|
| `1W remote XXXXXX` | The remote's 6-character node ID |
| `targets all` / `targets awning` | The broadcast device-type class the remote addresses |
| `EXECUTE(0x00) CLOSE` | The command: OPEN, CLOSE, STOP, FAVORITE, VENT, or a numeric position |
| `originator=user_remote` | Who triggered the command (user, wind sensor, rain sensor, timer, etc.) |
| `priority=user_high` | The ACEI priority level of the command |
| `(linked → 30E1F2)` | Shown when the remote is already configured — confirms the link is active |


## Sender events (remote buttons and sensors)

Every decoded 1W transmission is DEBUG-logged (see "Linked remotes" above), regardless of configuration. Additionally, senders on the `exposed_senders` allowlist fire an `esphome.home_io_control_sender_event` event to Home Assistant, so you can trigger automations directly from a physical remote press — including a remote that doesn't control any device you own an entity for (e.g. a spare remote you want to repurpose as an HA trigger).

"Sender" is deliberately not "remote": handheld/wall remotes and wind/rain sensors use the exact same 1W broadcast mechanism and the same node-ID addressing — a wind sensor is just another device that broadcasts a command (e.g. "close due to wind") with `originator=wind_sensor` instead of `originator=user_remote`. From the radio's perspective they're indistinguishable except for that one payload byte, so `exposed_senders` (like `linked_remotes`) works identically for either: a wind or rain sensor's node ID can go in this list the same way a handheld remote's can.

### Why this is opt-in

1W frames are unencrypted broadcasts to a type-class address (e.g., "all awning devices") — there is no ownership marker on the radio protocol. Your controller can overhear a neighbor's remote (or sensor) as easily as your own if it's in range. Firing an event to Home Assistant for every overheard transmission could mean HA sees a neighbor's button presses, so `exposed_senders` defaults to an **empty list**: nothing fires an event until you explicitly add its node ID.

This is a separate mechanism from `linked_remotes` (which drives optimistic status polling for a device you own):

- A sender can be in `exposed_senders` without being linked to any device — useful for a remote or sensor you want purely as an HA trigger.
- A sender can be linked to a device without being in `exposed_senders` — linking still triggers a status poll, but the transmission does **not** reach Home Assistant as an event.
- A sender can be in both lists at once.

### Configuring exposed senders

Find the sender's node ID the same way as for `linked_remotes` (the same log line works for a wind/rain sensor's node ID too), then add it to the hub-level `exposed_senders` list:

<!-- board-pinout: heltec-v2 -->
```yaml
home_io_control:
  cs_pin: 18
  rst_pin: 14
  dio0_pin: 26
  radio_type: sx1276
  node_id: "C0FFEE"
  system_key: "00112233445566778899AABBCCDDEEFF"
  exposed_senders:
    - "9D6085"
```

### Event data

| Field | Meaning |
|-------|---------|
| `remote_id` | The sender's 6-character node ID |
| `target_class` | Address classification: `unicast`, `broadcast_all`, `broadcast_type`, `discovery`, or `unknown_broadcast` |
| `target_type` | The broadcast device-type class the sender addresses (e.g. `awning`, or `unknown` for an all-devices broadcast) |
| `cmd` | The command name and hex code, e.g. `execute(0x00)` |
| `intent` | The decoded command: `OPEN`, `CLOSE`, `STOP`, `FAVORITE`, `VENT`, or a numeric position |
| `originator` | Who triggered the command (`user_remote`, `wind_sensor`, `rain_sensor`, `timer`, etc.) |
| `acei_level` | The ACEI priority level of the command |
| `linked` | `"true"` if this sender is also in some entity's `linked_remotes` list, `"false"` otherwise |

### Verifying the event fires

Before wiring up an automation, confirm the event actually reaches Home Assistant:

1. In Home Assistant, go to **Developer Tools → Events**.
2. Under "Listen to events", type `esphome.home_io_control_sender_event` and click **Start listening**.
3. Press the physical remote button (or trigger the sensor). If everything is configured correctly, the event and its full data payload appear in that panel within a second or two.
4. Leave that panel open while iterating on `exposed_senders`: after each config change and reflash, press the remote again and watch the panel.

If nothing appears, check the ESPHome DEBUG log (`logger: level: DEBUG`) for one of these lines, logged right after the `rx 1W remote ...` decode line:

| Log line | Meaning |
|----------|---------|
| `Firing esphome.home_io_control_sender_event for sender XXXXXX` | The event was sent — if Developer Tools still shows nothing, check the Home Assistant API connection instead (`api:` block, encryption key, network). |
| `1W sender XXXXXX has intent but is not in exposed_senders, skipping ...` | The sender ID isn't on the allowlist (or doesn't match — check exact casing/value). |
| `1W sender XXXXXX has intent but the API is not connected, skipping ...` | Home Assistant hasn't got an active connection to this device yet. |
| *(no line at all, just the `rx 1W remote ...` decode)* | This particular frame carried no decodable command intent — see below. |

**Not every button press fires the event.** Only frames decoded as `CMD_EXECUTE` or `CMD_ACTIVATE_MODE` carry a command intent (`OPEN`/`CLOSE`/`STOP`/etc.); other 1W traffic from the same remote — for example a `WRITE_PRIVATE(0x20)` frame, which some remotes send as part of the same button press — is still DEBUG-logged but never fires the event, because there is nothing decodable to put in `intent`. If you only ever see `WRITE_PRIVATE` lines and never an `EXECUTE` line for a press that should have moved something, the `EXECUTE` frame itself was likely never received (radio timing/contention), not silently dropped by this component.

### Example automation

```yaml
automation:
  - alias: "Awning remote pressed"
    trigger:
      - platform: event
        event_type: esphome.home_io_control_sender_event
        event_data:
          remote_id: "9D6085"
    action:
      - action: notify.mobile_app
        data:
          message: "Awning remote: {{ trigger.event.data.intent }}"
```


## See also

- [Sending 1W commands](oneway-transmit.md) — the transmit side
- [Covers](cover.md) — `optimistic_state` on a per-cover basis
- [Named device types](../supported-devices.md#named-device-types) — what `class:` accepts

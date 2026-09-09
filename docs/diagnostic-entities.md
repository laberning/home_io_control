# Diagnostic entities
<!-- doxygen-label: diagnostic_entities -->

Every cover, light, lock, switch and climate entity comes with seven companion entities that report
on the device behind it. They need no YAML: each is generated from the parent entity and named
after it, so a cover called `Awning` gets `Awning Active Issue`, `Awning RSSI` and so on. If the
parent has a `device_id:`, they group under the same Home Assistant device.

Only Active Issue is enabled by default. The rest are opt-in so the entity list stays manageable;
enable one from the device page in Home Assistant when you need it.

| Companion | Kind | Enabled | What it reports |
|---|---|---|---|
| `<Name> Active Issue` | Text | ✅ Yes | Why a command was refused — a wind or rain lockout, for instance. Empty the rest of the time. |
| `<Name> Device Name` | Text | No | The name stored inside the actuator itself, read once at boot. |
| `<Name> RSSI` | Sensor (dBm) | No | Signal strength of the last frame from this device. |
| `<Name> Last Contact` | Sensor (s) | No | Seconds since the last frame from this device — an age that keeps counting up, not a timestamp. |
| `<Name> Exchange Failures` | Sensor | No | Running total of exchanges that got no valid reply. |
| `<Name> Last Commanded By` | Text | No | Which controller last commanded the device. |
| `<Name> Last Command Source` | Text | No | What kind of source that was. |

Two other diagnostic sensors belong to specific features and are documented with them: the
"Last Pairing Result" sensor that comes with the Discover & Pair button
([Pairing](pairing.md#diagnosing-a-failed-pairing-attempt)) and the "Last 1W Command" sensor each
1W controller identity gets
([Sending 1W commands](configuration/oneway-transmit.md#the-last-1w-command-sensor)).

## Device Name

The name stored inside the actuator, the one the vendor's own app shows. The hub reads it once at
boot; a device that does not support the request keeps an empty name, and a failed request never
affects control or status. To change the stored name, use the `rename_device` action
([Home Assistant actions](configuration/actions.md#rename_device)), which reads the name back
afterwards so this sensor stays correct.

## Active Issue

Enabled by default because it turns a silent "nothing happened" into an explanation: pressing
"open" on an awning during high wind surfaces `LIMITATION_BY_WIND` in Home Assistant, not only in
the log.

It is not a per-command result. It is set only while an issue is outstanding:

- It publishes the symbolic result name, such as `LIMITATION_BY_RAIN`, `LIMITATION_BY_WIND` or
  `THERMAL_PROTECTION`. The same name appears in the log at WARN level, decoded rather than
  collapsed into a generic failure:
  `Device ABC123: command 0x00 returned limitation result=0xEB LIMITATION_BY_WIND (parameter was limited by a wind sensor)`.
- It is empty until the first refusal, and empties again once the device answers a later status
  poll or command successfully. A limitation from an hour ago is worse than none, so it does not
  linger once the device is confirmed working.
- Both an unsolicited report (a device announcing a limitation on its own, after a wind gust) and a
  refusal in direct reply to a Home Assistant command are recorded.

A refused command also reverts the entity to the device's last reported state — see
[Position and state](configuration/cover.md#position-and-state).

## Link Health

Three `sensor:` entities for radio and exchange health. They are low-level radio diagnostics
rather than everyday values, which is why they are disabled by default.

- **RSSI** (dBm): a smoothed signal-strength reading (exponential moving average, 1/8 weight per
  sample), updated on every frame received from the device, whether a reply to the hub or
  unsolicited traffic. Unavailable until the first frame; a placeholder 0 dBm is never published.
- **Last Contact** (seconds): time since the last frame from the device. It is an age, not a
  timestamp: it resets to about 0 on every frame, including replies to the hub's own polls, and
  counts up while the device is quiet, republished once a minute so it keeps advancing in Home
  Assistant between frames. The hub has no wall-clock source, which is why this is not a
  `timestamp`-class sensor. Unavailable until the first frame.
- **Exchange Failures** (count): cumulative outbound exchanges to this device (position, tilt,
  status and name requests) that received no valid response. Zero is a real, always-published
  value here. A rising count on an otherwise-working device points at a marginal link — weak
  signal, interference, distance — worth checking against RSSI.

RSSI and Exchange Failures update only when the hub processes a frame or exchange for the device;
there are no background timers to refresh them. Last Contact's once-a-minute heartbeat is the one
exception.

## Last Command

Two `text_sensor:` entities that answer "who moved my shutter" in a house with several remotes
plus a hub, and help spot a forgotten paired remote still commanding a device.

- **Last Commanded By**: the node ID of whatever last commanded the device, read from bytes the
  device includes in every status reply.
- **Last Command Source**: that command's originator, rendered `name(0xXX)`, for example
  `user_remote(0x01)`.

| Situation | `Last Commanded By` | `Last Command Source` |
|---|---|---|
| no record decoded yet | *(empty)* | *(empty)* |
| foreign controller `3B74DC` | `3B74DC` | `user_remote(0x01)` |
| this hub | `C0FFEE (this hub)` | `user_remote(0x01)` |
| device names its own ID | `2FE2D2 (this device)` | `local_user(0x00)` |
| gate, undefined originator | `586E35 (this device)` | `unknown(0x0A)` |

They cost no extra radio traffic: the bytes arrive in every status reply the hub already receives.
Their limits:

- Last-writer-wins and inherently stale: the record refreshes only when something commands the
  device, so it can lag behind reality between commands.
- A foreign controller's node ID has no friendly name unless you recognise it yourself; there is no
  built-in ID-to-name mapping.
- The originator decode is field-validated on roller shutters only, where it is a clean
  remote-versus-motor-button split. Other device classes (a mains gate, so far) have shown
  originator bytes with no defined name, which is what `unknown(0xXX)` is for.
- The record is **not** read from the immediate reply to a Home Assistant command, whose payload
  layout depends on the request. It updates on the settle poll a second or two later.

## See also

- [Configuration reference](configuration/index.md) — the options every entity shares
- [Troubleshooting](troubleshooting.md) — which of these to look at for a given symptom
- [Diagnostic probes](diagnostic-probes.md) — when these sensors are not enough and you want to
  ask the device directly

# Covers
<!-- doxygen-label: cfg_cover -->

Use the cover platform for position-capable IO-Homecontrol devices such as shutters, awnings,
blinds, openers, curtains, and related families. This is the primary and most-used platform.

```yaml
cover:
  - platform: home_io_control
    id: patio_awning
    name: "Patio Awning"
    io_device_id: "FEEB1E"
    io_device_type: "vertical_exterior_awning"
    invert_position: true
    status_poll_interval: 2s
    linked_remotes:
      - "ABCDEF"
      - "class:vertical_exterior_awning"
    optimistic_state: true
```

## Configuration variables:

The keys every entity platform shares — `name`, `io_device_id`, `io_device_type`, `io_subtype`,
`status_poll_interval`, `low_power`, `linked_remotes`, `home_io_control_id` — are documented in
[the configuration reference](index.md). The options below are the cover's own.

A cover whose declared kind no capture or report has confirmed on a real device (a gate opener or swinging
shutter of subtype 58, a sliding window with a lock) logs one warning when the config is built. The config is
accepted and the cover is created; the warning asks you to
[report what happens](../contributing.md#reporting-unsupported-devices).

- `invert_position` (Optional): Explicitly override the open/close position mapping. When omitted, the controller uses the learned device profile and automatically inverts families such as horizontal awnings once their type is known.
- `optimistic_state` (Optional, default: `true`): Show the requested position/movement direction immediately in Home Assistant — for both HA-issued commands (open/close/set-position/stop) and commands from a linked 1W remote — instead of waiting for the confirming poll or device response. The confirming poll/response always still runs and is the source of truth; optimistic state is a UX bridge only. Set `false` for a device where you don't want HA to show assumed movement, for example one with an unreliable RF link where a stale optimistic state could be misleading.
- `silent` (Optional): Move at the manufacturer app's "silent operation" speed — slower and quieter. Only a kind that takes a travel-speed parameter accepts it: config validation rejects `silent:` on garage openers, on interior venetian blinds, on dual roller shutters and on any device of subtype 58, and its message says to remove the key. On an interior venetian blind the first functional parameter is the slat orientation and on a dual roller shutter it is the upper curtain, so neither takes a speed there (experimental, see [Interior venetian blinds and dual roller shutters](cover.md#interior-venetian-blinds-and-dual-roller-shutters)). It selects the protocol's slow travel profile on position moves and on the favorite ("My") command. STOP is excluded because stopping has no travel speed, and ventilation and tilt because nothing has been captured for them yet — they keep their existing payloads rather than being guessed at. Nothing on the wire reports a device's current profile, so, like `invert_position`, this is a declared preference rather than a readback. Declaring the key also generates a `<Cover Name> Silent Operation` switch so the profile can be changed at runtime; the YAML value is the boot state. Omit the key entirely and no switch is created.
- `rain_sensor_poll_interval` (Optional): Ask the device how far its own rain protection lets it open, once per interval, and report the answer as a binary sensor. The minimum is `1min` and there is no default: omit the key and nothing is sent and no entity is created. Declaring it generates a `<Cover Name> Rain sensor` binary sensor. Validated on VELUX windows with a wired rain sensor; a Somfy Sunea awning answers the request with a reply this project cannot read as an opening limit, so the sensor stays without state there. Read [Polling for rain](cover.md#polling-for-rain) before choosing an interval, because every poll uses transmit time that the hub shares with everything else it sends.
- `battery` (Optional, default: `false`): Report the battery level a battery or solar device includes in its status replies. Generates a `<Cover Name> Battery Low` binary sensor and a `<Cover Name> Battery` text sensor. It sends nothing extra over the radio. See [Battery level](cover.md#battery-level).
- All standard options from the ESPHome cover base schema also apply, including `id`, `device_class` (see [Home Assistant device class](cover.md#home-assistant-device-class)), `icon`, entity metadata, MQTT options, and cover automations such as `on_opening`, `on_closing`, and `on_idle`.

## Home Assistant device class

A cover that declares `io_device_type` and no `device_class:` gets the class of its kind, so Home
Assistant shows the matching icon and wording. Set `device_class:` yourself to override it, or
`device_class: ""` for none.

| `io_device_type` | Default `device_class` |
|---|---|
| `garage_opener` | `garage` |
| `gate_opener` | `gate` |
| `rolling_door_opener` | `door` |
| `window_opener`, `sliding_window` | `window` |
| `roller_shutter`, `dual_roller_shutter`, `swinging_shutter` | `shutter` |
| `interior_venetian_blind`, `exterior_venetian_blind`, `louvre_blind` | `blind` |
| `vertical_interior_blind` | `shade` |
| `curtain_track` | `curtain` |
| `horizontal_awning`, `vertical_exterior_awning` | `awning` |

Every other kind, including `bioclimatic_pergola`, has no default class.

## Companion entities

Beyond the diagnostic companions every platform generates, covers add up to eight more:

- **`<Cover Name> Favorite Position`** — a button, generated for any cover with a declared
  position-capable `io_device_type` except garage openers, whose motors refuse the favorite
  command (issue #163). It sends the protocol's favorite or My-position command. A motor with no
  favorite position refuses the command before authenticating, and nothing moves.
- **`<Cover Name> Ventilation Position`** — a second button, for covers with `io_device_type` set
  to `window_opener` or `ventilation_point`. It moves the actuator to a predefined partially-open
  position suitable for air exchange.
- **`<Cover Name> Partial Position`** (garage openers) and **`<Cover Name> Pedestrian Position`**
  (gate openers) — experimental buttons, disabled by default: enable them in Home Assistant before
  you use them. See [Partial and pedestrian position buttons](cover.md#partial-and-pedestrian-position-buttons).
- **`<Cover Name> Silent Operation`** — a switch, generated only when `silent:` is declared.
- **`<Cover Name> Rain sensor`** — a binary sensor with device class `moisture`, generated only
  when `rain_sensor_poll_interval:` is declared. See [Polling for rain](cover.md#polling-for-rain).
- **`<Cover Name> Battery Low`** and **`<Cover Name> Battery`** — a binary sensor and a text
  sensor, generated only with `battery: true`. See [Battery level](cover.md#battery-level).

These follow the cover's own `device_id:`, if one is set, so they group under the same Home
Assistant device as the cover itself.

Generating them is compile-time only. If `io_device_type` is omitted and learned later from radio
traffic, the controller can still operate the cover normally, but it cannot add new ESPHome
entities at runtime after boot, and it cannot drop the position slider of a device that only opens
and closes. The same applies to the diagnostic sensors. Declare `io_device_type` and `io_subtype`
as the paste-ready YAML from [Discover & Pair](../pairing.md#discover--pair) prints them.

## Covers that only open and close

Garage openers, gate openers and swinging shutters of subtype 58 take open, close and stop and
nothing in between: a 50 % position gets the refusal `PARAMETER_UNKNOWN` from the motor, and the
door does not move (issue #163, corpus
`wisniowski_moto1000_exchange_execute_partial_position_refused`). For these kinds the cover:

- shows open, close and stop and no position slider, and sends open and close in the short
  main-parameter frame;
- shows closed only when the device reports fully closed, and open for every other report. A door
  stopped half-way reports an unknown position, so it shows open;
- is an assumed-state cover, so Home Assistant keeps both the Open and the Close button enabled
  whatever state the cover shows;
- refuses a position between open and closed from a lambda or an action, without sending it, and
  logs why;
- has no Favorite Position button when it is a garage opener, and takes no `silent:` key.

Subtype 58 on a gate opener or a swinging shutter is inferred from the garage opener's behaviour
and the shared subtype definition, so it is experimental: the Favorite Position button stays and
is sent, and a motor without a favorite position answers `PARAMETER_UNKNOWN` and does not move.

## Partial and pedestrian position buttons

A garage opener (`io_device_type: garage_opener`, subtype 0 or 58) gets a `Partial Position`
button, and a gate opener (`gate_opener`, subtype 0 or 58) gets a `Pedestrian Position` button
(experimental). Both are created disabled: enable them under the entity's settings in Home Assistant.

A press sends that named position to the motor. A motor that has no such position refuses it with
`PARAMETER_UNKNOWN` before authenticating, and nothing moves, as for the favorite position in
issue #163. The hub sends these two on 2W only, never from a one-way controller, and, like the
favorite position, never repeats one that went unanswered.

## Polling for rain

A window with a wired rain sensor limits its own opening while it rains, and it only reports that
limit when a controller asks. With `rain_sensor_poll_interval` set, the hub asks on that interval:
it reads the window's minimum opening limit, and `<Cover Name> Rain sensor` is on while that limit
is 89 % or more.

```yaml
cover:
  - platform: home_io_control
    name: "Roof Window"
    io_device_id: "123ABC"
    io_device_type: window_opener
    rain_sensor_poll_interval: 15min
```

What to expect from the sensor:

- **No state until the first answer.** "Off" always means the window said so. The sensor goes back
  to no state after three polls in a row get no answer, so a window that is out of range does not
  keep showing an old reading. A device whose reply is not in the VELUX layout never gives a
  state either. Set `logger: level: DEBUG` to see what each poll received: the log shows the
  decoded limitation reply, or the raw frame when it does not fit.
- **A reading is as fresh as the interval.** A shower that starts right after a poll shows up at the
  next one.
- **It does not replace Active Issue.** Rain sensor reports the window's opening limit right now;
  Active Issue reports that the last move was ordered by the rain sensor or that a command was
  clamped — see [Diagnostic entities](../diagnostic-entities.md#active-issue).
- **A moving window is not asked.** The poll waits 30 seconds and tries again.
- **The interval varies by up to 10 %** from poll to poll, so several windows with the same
  interval do not transmit at the same moment.

### Choosing an interval

Use 15 minutes unless you have a reason not to. The request goes out on 868.95 MHz, which allows
3.6 seconds of transmission per hour (0.1 % duty cycle, EN 300 220), and the hub shares that
allowance between every command, poll and retry. The table shows what one poll costs in transmit
time per hour, for a request that is answered on the first try. A poll that gets no answer is tried
twice, so a window that is out of range uses up to double.

| Interval | Always-alive device | `low_power` device |
|---|---|---|
| 1 minute | 0.7 to 0.9 s | 13 s |
| 5 minutes | 0.13 to 0.17 s | 2.6 s |
| 15 minutes | 0.04 to 0.06 s | 0.9 s |
| 60 minutes | 0.01 s | 0.2 s |

The two figures for an always-alive device are the 32-byte start preamble of the SX1276 and the
48-byte one of the SX1262 and LR1121. Figures are approximate: they count one 12-byte request.

A `low_power` device is woken with the long preamble on every poll, which costs far more transmit
time and draws on the device's battery each time. Poll it every hour or more. At one minute it
would by itself use more than three times the whole hourly allowance.

## Battery level

Battery and solar motors include a coarse battery level in their status replies: full, normal or
low. With `battery: true`, the hub publishes it as two entities:

- **`<Cover Name> Battery Low`**: a binary sensor with device class `battery`, on while the level
  is low. This is the entity Home Assistant treats as a low-battery warning, so low-battery
  notification blueprints and battery dashboards pick it up without extra setup. Use it in
  automations.
- **`<Cover Name> Battery`**: a text sensor showing `full`, `normal` or `low`. It is disabled by
  default; enable it from the device page if you want to see all three levels.

```yaml
cover:
  - platform: home_io_control
    name: "Terrace Awning"
    io_device_id: "123ABC"
    io_device_type: vertical_exterior_awning
    low_power: true
    battery: true
```

What to expect:

- **It is a level, not a percentage.** There is no `%` value to put a threshold on. Act on
  `Battery Low` turning on instead.
- **It updates when the hub hears a status from the motor.** That happens at boot, a few seconds
  after every move, and whenever the motor reports a move on its own. Nothing is polled in the
  background, so a motor that stays still keeps the level it last reported.
- **Every status line in the log names the level**, for example
  `Device 123ABC: position=0% target=0% stopped battery=full`.
- **A mains-powered device reports no level.** Both entities then stay without a state, its status
  lines end in `battery=not_reported`, and the log explains this once with `reports no battery
  state`.
- **Only `full` has been seen on real hardware so far.** Solar Somfy motors report `full` in every
  capture we hold, in daylight. If yours ever shows `normal` or `low`, please add it to
  [issue #156](https://github.com/laberning/home_io_control/issues/156) along with the time and the
  weather.

## Tilt

When `io_device_type` is declared as a tilt-capable type — `interior_venetian_blind`,
`exterior_venetian_blind`, or `louvre_blind` — the cover entity automatically exposes a
tilt slider in Home Assistant.

Tilt-only commands keep the current position unchanged. When Home Assistant sends both a position
and a tilt value at once (via `cover.control` with both fields), they go to the device in a single
atomic command, so the cover reaches the desired position and slat angle without a race between two
sequential exchanges.

A vertical interior blind (`vertical_interior_blind`) has no slat parameter in the VELUX KLF 200 API
specification, Appendix 2, so it is a plain cover without a tilt slider (experimental).

## Interior venetian blinds and dual roller shutters

(Experimental.) The VELUX KLF 200 API specification, Appendix 2 uses the first functional parameter for
the slat orientation of an interior venetian blind and for the upper curtain of a dual roller shutter,
not for a travel speed. The hub therefore sends position and force-open commands to both kinds in the
short form that carries the main parameter only, and refuses `silent:` for them at config time. The
interior venetian blind keeps its tilt slider. No two-way device of either kind has confirmed this yet.

## Position and state

- **Position starts at fully open and becomes real after the first status reply.** The protocol
  has no "report your position" request that works from cold, so a cover reports `1.0` until the
  device says otherwise. Internally the hub uses `212.0` as its unknown-position marker, but that
  value never reaches the ESPHome `position` field, so a lambda can treat any value in `[0.0, 1.0]`
  as valid. The OLED example configs show `--` and `Unknown` while the position is unknown.
- **A failed command reverts the entity.** If a device is unreachable or refuses a command — a
  silent timeout or an explicit `LIMITATION_BY_WIND` alike — the entity returns to the last
  position the device reported and an idle state rather than staying mid-travel. A failed stop
  keeps showing the movement it tried to stop, toward the same target, rather than claiming the
  cover stopped, and a failed tilt withdraws its predicted slat angle. With `optimistic_state: false` there is no assumed movement
  to revert. The refusal itself shows up in the `Active Issue` sensor — see
  [Diagnostic entities](../diagnostic-entities.md).
- **A cover that stops a little off its target goes idle one status poll later.** Some motors
  report "stopped" a fraction of a percent short of where they were sent, for example a VELUX SSL
  at an end stop ([issue #151](https://github.com/laberning/home_io_control/issues/151)). The hub
  polls once more, and when the device reports the same place again, the cover shows idle at the
  position the device reports. The position is never rounded to the target.

A lambda that respects the position contract:

```yaml
lambda: |-
  const float position = id(patio_awning).position;
  if (position < 0.0f || position > 1.0f) {
    ESP_LOGI("example", "position is unknown");
  } else {
    ESP_LOGI("example", "position %.0f%%", position * 100.0f);
  }
```

## Notes

- The favorite command is one-way only: move to favorite. This component does not expose a sensor for reading the stored favorite position value, and it does not yet expose a save/delete favorite workflow, because no verified controller-side protocol command has been identified.
- `status_poll_interval` is movement-scoped, not a continuous background refresh. The hub polls only while a command or overheard remote activity suggests the device is still moving, and stops once the device reports a stable state or the bounded polling window expires. Many devices report a settle hint in their status replies, and the next poll comes after **min(device hint, configured interval)**, so the interval is a ceiling: a device can shorten it but never stretch it. Without `status_poll_interval`, the ceiling is 3 s: the next poll comes after the hint or 3 s, whichever is shorter. The hint and the chosen delay are visible at debug log level.
- After a STOP the settle poll is capped at about 1 s regardless of interval or hint, so Home Assistant gets the resting position quickly even if the device was mid-travel.
- While the cover is moving, the hub does not back off when a poll gets no answer: the next poll comes at the usual cadence, at most 5 s later. Only a third miss in a row is retried after about 5 s, and after longer delays if it keeps failing. A poll of a cover at rest backs off from the first miss. A motor can drop its reply at the moment it stops at the end of its travel, so the final position can show up a poll later than expected (issue #178).
- Unsolicited `0x71` device status updates are always applied to the entity state and extend settle polling while the device is still moving.

## See also

- [Diagnostic entities](../diagnostic-entities.md) — what Active Issue, Link Health and Last
  Command report
- [Linked remotes and sender events](remotes.md) — wiring a physical remote to this cover
- [ESPHome Cover Component](https://esphome.io/components/cover/)

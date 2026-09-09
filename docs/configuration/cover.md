# Covers
<!-- doxygen-label: cfg_cover -->

Use the cover platform for position-capable IO-Homecontrol devices such as shutters, awnings,
blinds, openers, curtains, and related families. This is the primary and most-used platform.

```yaml
cover:
  - platform: home_io_control
    id: patio_awning
    name: "Patio Awning"
    device_class: awning
    io_device_id: "FEEB1E"
    io_device_type: "awning"
    invert_position: true
    status_poll_interval: 2s
    linked_remotes:
      - "ABCDEF"
      - "class:awning"
    optimistic_state: true
```

## Configuration variables:

The keys every entity platform shares — `name`, `io_device_id`, `io_device_type`, `io_subtype`,
`status_poll_interval`, `low_power`, `linked_remotes`, `home_io_control_id` — are documented in
[the configuration reference](index.md). The options below are the cover's own.

- `invert_position` (Optional): Explicitly override the open/close position mapping. When omitted, the controller uses the learned device profile and automatically inverts families such as horizontal awnings once their type is known.
- `optimistic_state` (Optional, default: `true`): Show the requested position/movement direction immediately in Home Assistant — for both HA-issued commands (open/close/set-position/stop) and commands from a linked 1W remote — instead of waiting for the confirming poll or device response. The confirming poll/response always still runs and is the source of truth; optimistic state is a UX bridge only. Set `false` for a device where you don't want HA to show assumed movement, for example one with an unreliable RF link where a stale optimistic state could be misleading.
- `silent` (Optional): Move at the manufacturer app's "silent operation" speed — slower and quieter. It selects the protocol's slow travel profile on position moves and on the favorite ("My") command. STOP is excluded because stopping has no travel speed, and ventilation and tilt because nothing has been captured for them yet — they keep their existing payloads rather than being guessed at. Nothing on the wire reports a device's current profile, so, like `invert_position`, this is a declared preference rather than a readback. Declaring the key also generates a `<Cover Name> Silent Operation` switch so the profile can be changed at runtime; the YAML value is the boot state. Omit the key entirely and no switch is created.
- All standard options from the ESPHome cover base schema also apply, including `id`, `device_class`, `icon`, entity metadata, MQTT options, and cover automations such as `on_opening`, `on_closing`, and `on_idle`.

## Companion entities

Beyond the seven diagnostic companions every platform generates, covers add up to three more:

- **`<Cover Name> Favorite Position`** — a button, generated for any cover with a declared
  position-capable `io_device_type`. It sends the protocol's favorite or My-position command.
- **`<Cover Name> Ventilation Position`** — a second button, for covers with `io_device_type` set
  to `window_opener` or `ventilation_point`. It moves the actuator to a predefined partially-open
  position suitable for air exchange.
- **`<Cover Name> Silent Operation`** — a switch, generated only when `silent:` is declared.

These follow the cover's own `device_id:`, if one is set, so they group under the same Home
Assistant device as the cover itself.

Generating them is compile-time only. If `io_device_type` is omitted and learned later from radio
traffic, the controller can still operate the cover normally, but it cannot add new ESPHome
entities at runtime after boot. The same applies to the diagnostic sensors.

## Tilt

When `io_device_type` is declared as a tilt-capable type — `venetian_blind`,
`external_venetian_blind`, `blind`, or `louvre_blind` — the cover entity automatically exposes a
tilt slider in Home Assistant.

Tilt-only commands keep the current position unchanged. When Home Assistant sends both a position
and a tilt value at once (via `cover.control` with both fields), they go to the device in a single
atomic command, so the cover reaches the desired position and slat angle without a race between two
sequential exchanges.

## Position and state

- **Position starts at fully open and becomes real after the first status reply.** The protocol
  has no "report your position" request that works from cold, so a cover reports `1.0` until the
  device says otherwise. Internally the hub uses `212.0` as its unknown-position marker, but that
  value never reaches the ESPHome `position` field, so a lambda can treat any value in `[0.0, 1.0]`
  as valid. The OLED example configs show `--` and `Unknown` while the position is unknown.
- **A failed command reverts the entity.** If a device is unreachable or refuses a command — a
  silent timeout or an explicit `LIMITATION_BY_WIND` alike — the entity returns to the last
  position the device reported and an idle state rather than staying mid-travel. A failed stop
  falls back to the last observed movement rather than claiming it stopped, and a failed tilt
  withdraws its predicted slat angle. With `optimistic_state: false` there is no assumed movement
  to revert. The refusal itself shows up in the `Active Issue` sensor — see
  [Diagnostic entities](../diagnostic-entities.md).

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
- `status_poll_interval` is movement-scoped, not a continuous background refresh. The hub polls only while a command or overheard remote activity suggests the device is still moving, and stops once the device reports a stable state or the bounded polling window expires. Many devices report a settle hint in their status replies, and the next poll comes after **min(device hint, configured interval)**, so the interval is a ceiling: a device can shorten it but never stretch it. Without the key, the hint drives the cadence directly (3 s when there is none). The hint and the chosen delay are visible at debug log level.
- After a STOP the settle poll is capped at about 1 s regardless of interval or hint, so Home Assistant gets the resting position quickly even if the device was mid-travel.
- Unsolicited `0x71` device status updates are always applied to the entity state and extend settle polling while the device is still moving.

## See also

- [Diagnostic entities](../diagnostic-entities.md) — what Active Issue, Link Health and Last
  Command report
- [Linked remotes and sender events](remotes.md) — wiring a physical remote to this cover
- [ESPHome Cover Component](https://esphome.io/components/cover/)

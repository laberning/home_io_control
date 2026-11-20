# Tips and tricks
<!-- doxygen-label: tips -->

Things that are not required to get running, but make the result nicer to live with.

## Grouping entities into Home Assistant devices

The cover, light, switch and lock platforms accept ESPHome's own `device_id:` key. It
groups an entity, and every companion entity it generates, under its own Home Assistant device
instead of everything landing on the one physical ESPHome node. This is unrelated to
`io_device_id:`, the device's radio address: `device_id:` is purely a Home Assistant grouping.

```yaml
esphome:
  devices:
    - id: patio_awning_device
      name: "Patio Awning"
      relation: via

cover:
  - platform: home_io_control
    device_id: patio_awning_device
    id: patio_awning
    name: ""
    io_device_id: "FEEB1E"
    io_device_type: "vertical_exterior_awning"
```

- Declare each sub-device once under `esphome: devices:`, then reference its `id:` from the
  entity's `device_id:`.
- `relation: via` tells Home Assistant that the device is reached through this ESPHome node. ESPHome 2026.11
  and later require the key; on an older ESPHome, leave the `relation:` line out.
- Every companion entity the platform generates — the favorite, ventilation, partial and pedestrian buttons (the last two are disabled by default, the pedestrian one is experimental), the silent
  operation switch, the travel speed select, the rain sensor and the diagnostic sensors — inherits the parent entity's sub-device.
- **Give the entity an empty name.** Home Assistant displays an entity as
  `<device name> <entity name>`, so a device and an entity both called "Patio Awning" show up as
  "Patio Awning Patio Awning", and every companion gets that prefix too. `name: ""` is ESPHome's
  idiom for "this entity is the device" and avoids it. (`name: None` does the same but additionally
  requires `esphome: friendly_name:`.) An empty name needs an explicit `id:`, because there is
  nothing else to derive the companion entities' IDs from; config validation says so if you forget.
- Hub-level entities — the key-extraction and 1W key recovery switches, the LR1121 firmware
  controls, tuning numbers and selects, the Scan Paired Devices button, the Discover & Pair button
  and its "Last Pairing Result" sensor (see [Discover & Pair](pairing.md#discover--pair)), and each
  1W identity's command buttons — do not take `device_id:` and always appear on the hub's own
  ESPHome device.

## Composing a slider from 1W buttons

There is no 1W cover entity: 1W support gives you buttons and the `oneway_set_position` action.
ESPHome's `time_based` cover platform turns those buttons into a position slider, estimating the
position from travel time:

```yaml
cover:
  - platform: time_based
    name: "Roof Window"
    open_action:  {then: [button.press: velux_windows_open]}
    close_action: {then: [button.press: velux_windows_close]}
    stop_action:  {then: [button.press: velux_windows_stop]}
    open_duration: 30s
    close_duration: 28s
    has_built_in_endstop: true
```

The position is an estimate. A press on the physical remote moves the device without telling Home
Assistant, so the estimate drifts until the next full open or close re-synchronises it.

## Following the wall remote with a time-based cover

The cover above does not see a press on the physical remote. If the hub hears the remote, it can
tell Home Assistant, and an automation can pass the press to the cover so its estimate follows the
wall switch.

Add the remote's node ID to `exposed_senders`; [Sender events](configuration/remotes.md#sender-events-remote-buttons-and-sensors)
shows how to find the ID and what the event carries. The automation below follows one remote:

```yaml
home_io_control:
  exposed_senders: ["9D6085"]
```

Then create an automation that reacts to the sender event and calls the matching cover service.
Replace the remote ID and the cover entity:

```yaml
alias: "Roof window follows its wall switch"
mode: queued
triggers:
  - trigger: event
    event_type: esphome.home_io_control_sender_event
    event_data:
      remote_id: "9D6085"
conditions:
  - condition: template
    value_template: "{{ trigger.event.data.intent in ['OPEN', 'CLOSE', 'STOP'] }}"
actions:
  - action: >
      {{ {'OPEN': 'cover.open_cover',
          'CLOSE': 'cover.close_cover',
          'STOP': 'cover.stop_cover'}[trigger.event.data.intent] }}
    target:
      entity_id: cover.roof_window
```

For several windows, make one automation per remote.

**The command goes out a second time.** The cover's open, close and stop actions press the
generated 1W buttons, so the hub transmits the command the remote already sent. The device sees
the same command twice, once from the remote and once from the hub's identity, which must have
enrolled with it. The path also runs through Home Assistant, so the estimate starts a moment after
the real move and does not work while the API is disconnected. A user first described this approach in
[issue #74](https://github.com/laberning/home_io_control/issues/74).

## Quieting frame warnings in the log

The hub logs a line when it receives a frame it did not act on, for example a command it does not
handle or a rejected status update. These lines use their own log tag, `home_io_control.frame`, and
appear as warnings only when the frame is addressed to one of your devices or to the hub. Frames the
hub merely overheard, such as a heater broadcasting its menu state once a second, stay at debug level.

If a device still floods the log, raise the level of that tag only:

```yaml
logger:
  logs:
    home_io_control.frame: ERROR
```

This also hides the `auth_failed` lines, which you need when you diagnose pairing or a device that
ignores commands. Remove the line before you collect a log for an issue report.

## See also

- [Sending 1W commands](configuration/oneway-transmit.md) — the buttons a slider is composed from
- [Covers](configuration/cover.md) — `travel_speed:`, `invert_position:` and the rest

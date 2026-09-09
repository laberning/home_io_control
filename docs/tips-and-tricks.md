# Tips and tricks
<!-- doxygen-label: tips -->

Things that are not required to get running, but make the result nicer to live with.

## Grouping entities into Home Assistant devices

The cover, light, switch, lock and button platforms accept ESPHome's own `device_id:` key. It
groups an entity, and every companion entity it generates, under its own Home Assistant device
instead of everything landing on the one physical ESPHome node. This is unrelated to
`io_device_id:`, the device's radio address: `device_id:` is purely a Home Assistant grouping.

```yaml
esphome:
  devices:
    - id: patio_awning_device
      name: "Patio Awning"

cover:
  - platform: home_io_control
    device_id: patio_awning_device
    id: patio_awning
    name: ""
    io_device_id: "FEEB1E"
    io_device_type: "awning"
```

- Declare each sub-device once under `esphome: devices:`, then reference its `id:` from the
  entity's `device_id:`.
- Every companion entity the platform generates — the favorite and ventilation buttons, the silent
  operation switch and the diagnostic sensors — inherits the parent entity's sub-device.
- **Give the entity an empty name.** Home Assistant displays an entity as
  `<device name> <entity name>`, so a device and an entity both called "Patio Awning" show up as
  "Patio Awning Patio Awning", and every companion gets that prefix too. `name: ""` is ESPHome's
  idiom for "this entity is the device" and avoids it. (`name: None` does the same but additionally
  requires `esphome: friendly_name:`.) An empty name needs an explicit `id:`, because there is
  nothing else to derive the companion entities' IDs from; config validation says so if you forget.
- Hub-level entities — the key-extraction and 1W key recovery switches, the LR1121 firmware
  controls, tuning numbers and selects, the Scan Paired Devices button and each 1W identity's
  command buttons — do not take `device_id:` and always appear on the hub's own ESPHome device.
  The Discover & Pair button is device-bound like any other platform entity, and its companion
  sensor follows it — see [Discover & Pair](pairing.md#discover--pair).

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

## See also

- [Sending 1W commands](configuration/oneway-transmit.md) — the buttons a slider is composed from
- [Covers](configuration/cover.md) — `silent:`, `invert_position:` and the rest

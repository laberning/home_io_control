# Lights, locks and switches
<!-- doxygen-label: cfg_light_lock_switch -->

Three platforms that share their configuration with the cover and differ only in what they control.
The keys every entity platform shares — `name`, `io_device_id`, `io_device_type`, `io_subtype`,
`status_poll_interval`, `low_power`, `linked_remotes`, `home_io_control_id` — are documented in
[the configuration reference](index.md), along with the seven diagnostic companion entities each of
these platforms generates.

## Lights

Use the light platform for IO-Homecontrol light devices. It defaults to binary on/off; set
`dimmable: true` for brightness control on devices that support intermediate positions.

Light control is hardware-validated on a Somfy Izymo dimmer, in binary mode and for intermediate
brightness alike.

```yaml
light:
  - platform: home_io_control
    id: garden_light
    name: "Garden Light"
    io_device_id: "D15C05"

  - platform: home_io_control
    id: dimmable_light
    name: "Dimmable Light"
    io_device_id: "D15C06"
    dimmable: true
```

### Configuration variables:

- `io_device_type` (Optional): Use the named value `light` when known, or a raw integer such as `0x06` if you are working from a pairing log that reports a not-yet-exposed alias.
- `dimmable` (Optional, default `false`): Expose brightness control (`ColorMode::BRIGHTNESS`) instead of on/off only. The protocol has no machine-readable signal for whether a given light device actually supports intermediate positions, so this is an explicit opt-in — leave it unset for binary-only devices. Brightness is applied instantly (no client-side fade) by default, since ESPHome's default 1 s transition would otherwise send a stream of superseding radio commands over the device's round trip; override `default_transition_length` in YAML if you want a fade anyway.
- All standard options from the ESPHome light schema also apply.

### Notes

- Known non-light device families are rejected once the device type is known.

## Locks

Use the lock platform for IO-Homecontrol lock devices that should appear in Home Assistant as
native ESPHome locks.

> [!NOTE]
> This platform is experimental. It has not been exercised against a real IO-Homecontrol lock, so
> treat its behaviour as unproven and report what you find.

```yaml
lock:
  - platform: home_io_control
    id: front_door_lock
    name: "Front Door Lock"
    io_device_id: "D0A9C0"
    io_device_type: "lock"
    status_poll_interval: 2s
```

### Configuration variables:

- `io_device_type` (Optional): Use the named value `lock` when known, or a raw integer such as `0x09` if pairing reports a type without a named YAML alias yet.
- All standard options from the ESPHome lock schema also apply.

### Notes

- The implementation exposes lock and unlock behavior and maps it onto the protocol's shared 0/100 execute encoding.
- The platform does not advertise ESPHome's optional `open` or code-required lock features, because those command semantics are not validated here yet.
- Known non-lock device families are rejected once the device type is known.

## Switches

Use the switch platform for binary on/off IO-Homecontrol switch devices.

> [!NOTE]
> This platform is experimental and limited to binary on/off semantics. It has not been exercised
> against real hardware.

```yaml
switch:
  - platform: home_io_control
    id: irrigation_switch
    name: "Irrigation Switch"
    io_device_id: "112233"
```

### Configuration variables:

- `io_device_type` (Optional): Use the named value `on_off_switch` when known, or a raw integer such as `0x0F` if pairing reports a type without a named YAML alias yet.
- All standard options from the ESPHome switch schema also apply.

### Notes

- Known non-switch device families are rejected once the device type is known.

## See also

- [Diagnostic entities](../diagnostic-entities.md) — what the companion sensors report
- [ESPHome Light Component](https://esphome.io/components/light/) ·
  [Lock](https://esphome.io/components/lock/) ·
  [Switch](https://esphome.io/components/switch/)

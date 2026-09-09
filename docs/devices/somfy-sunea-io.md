# Somfy Sunea IO
<!-- doxygen-label: dev_somfy_sunea_io -->

A family of IO-Homecontrol tubular motors for awnings and roller shutters. The awning variant is
the project's reference actuator; the shutter variants come from field reports.

## What works

| Capability | Status |
|---|---|
| Open, close, stop | ✅ Confirmed |
| Set position (0–100%) | ✅ Confirmed |
| Position feedback | ✅ Confirmed |
| Favorite / My position | ✅ Confirmed |
| Force open | ✅ Confirmed to move the device |
| Read and write the stored device name | ✅ Confirmed |
| Identify (jog) | ✅ Confirmed |
| Discover & Pair | ✅ Confirmed |
| Driven by a linked 1W Smoove remote | ✅ Confirmed |

## Onboarding route that worked

**Discover & Pair** for the awning and for the 70/17 and 40/17 shutter variants.

The exception is the MAESTRIA+ in the same family, where Discover & Pair did not succeed after
prolonged effort and [key extraction](../key-extraction.md) did — reported in issue #103. If your
Sunea has ever been paired to a TaHoma or Connexoon, start there rather than with discovery.

## Working YAML

```yaml
cover:
  - platform: home_io_control
    name: "Patio Awning"
    device_class: awning
    io_device_id: "30E1F2"
    io_device_type: "awning"
    io_subtype: 0
    invert_position: true
    status_poll_interval: 2s
    linked_remotes:
      - "9D6085"     # the Smoove wall switch
```

For a roller-shutter variant use `io_device_type: "roller_shutter"` and drop `invert_position`.

## Known quirks

- **The command acknowledgement can report a stale target position.** The ack that comes back
  immediately after an open or close sometimes still carries the *previous* target. The follow-up
  status poll is the value to trust, and the component treats it that way.
- **Listen-before-talk contention is real on this device.** A busy channel delays the command
  rather than failing it. If `Exchange Failures` is climbing, see
  [Radio tuning](../configuration/tuning.md).
- **Solar variants need `low_power: true`.** The Sunea itself is mains-powered, but the RS100 and
  Oximo solar motors in the same catalogue are not — see [Somfy RS100](somfy-rs100.md).
- **A wrong system key produces an error reply, not silence.** Captured deliberately, and useful
  when diagnosing: silence means the device never heard you, an error reply means it did.

## Evidence

31 corpus captures on the maintainer's own hardware, node `30E1F2` ("Patio Awning"). They cover
every control command, position feedback, name get/set, identify, favorite, force-open, LBT
contention, a wrong-key error, and ten probe scenarios, on SX1276, SX1262 and LR1121.

The shutter variants rest on issue #65. A horizontal awning pair and an IO Vertical awning are
also captured.

## See also

- [Covers](../configuration/cover.md)
- [Key extraction](../key-extraction.md) — the route for a MAESTRIA+ or a previously-paired motor

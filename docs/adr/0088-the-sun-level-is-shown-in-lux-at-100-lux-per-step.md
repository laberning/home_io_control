# ADR 0088: The sun level is shown in lux at 100 lux per step

<!-- doxygen-label: adr0088 -->

**Status:** Accepted; amended by [ADR 0107](0107-a-covers-periodic-readings-and-battery-state-are-reading-rows-on-one-schedule.md) · **Recorded:** 2026-11

Amends [ADR 0064](0064-a-sensor-reading-is-drawn-by-a-re-sent-event-subscription.md) (point 1: the
`Sun level` sensor's unit and device class, and the consequence that the sun value has no unit).

## Context

ADR 0064 publishes the sun value of a Somfy RS100 Solar io raw, without a unit, because nothing
showed what one step means. The reporter of issue #156 has since posted four days of readings from
two screens and an outdoor Hue light sensor a few metres away, facing the same way and mounted
about a metre higher than the panels; another building shades the panels at times:

- On two dull November days the sun value peaked at 35 to 61; the Hue peaked at 540 to 665 lx.
- On two sunny days it peaked at 652 to 713; the Hue peaked at 6003 lx on both days.
- Earlier logs in the same issue show 600 to 750 in direct sun, 5 to 25 in shade and at dusk, and 0
  at night.

Whenever panel and Hue saw the same light, the Hue read about 8 to 12 times the sun value, on dull
and sunny days alike, so the value is a light level in proportion to illuminance. Taken against
common daylight figures, the Hue's ratio of about 10 lx per step would put direct sun on the panel
at 6 000 to 7 500 lx, far below the 50 000 to 100 000 lx usually given for direct sunlight. 100 lx
per step puts both ends in their usual ranges: 600 to 750 becomes 60 000 to 75 000 lx, and 35 to
61 becomes 3 500 to 6 100 lx, within overcast daylight (a few thousand to about 10 000 lx). The two
estimates disagree by a factor of ten; choosing 100 means taking the Hue to read about ten times
low, which only a lux meter held next to the panel can confirm or refute.

## Decision

1. **`<Cover Name> Sun level` reports lux:** unit `lx`, device class `illuminance`, value = sun
   value × 100. The name, the YAML key `sun_level_poll_interval` and the entity id stay.
2. **The factor is an estimate and labelled so.** It is a round number that puts the reporter's
   sun values into common daylight ranges, not a measured calibration; the docs and the code say so
   until a lux meter held next to the panel confirms it.

## Consequences

- The sensor shows a value a Home Assistant user can read and compare, and automations can use
  ordinary illuminance thresholds.
- The scale is not confirmed, and the posted Hue readings point to a factor ten lower. A confirmed
  reading changes only the constant.
- The unit changes on an existing entity, so Home Assistant starts a new statistics series for it
  once. Keeping the entity outweighs that for the few installations that run the sensor.

## Considered options

- **Keep the raw value.** No wrong scale, but a number without meaning for the user.
- **A new `Illuminance` entity next to the raw one.** No statistics break, but two entities for one
  value, and a raw one nobody needs once the lux value exists.
- **10 lx per step, from the Hue.** Matches the only meter the reporter had, but puts direct sun
  far below common daylight figures, see Context.

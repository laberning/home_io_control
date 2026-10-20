# ADR 0057: Temperature setpoints travel in the main parameter

<!-- doxygen-label: adr0057 -->

**Status:** Accepted · **Recorded:** 2026-10

## Context

The heating temperature interface (node type 14) and the generic heat pump (22, subtype 0) are driven
by the standard EXECUTE like a cover: no private registers, no heating levels. Their main parameter
is a temperature, or one of a few named setpoints (aliases) such as the device's comfort and economy
setpoints. A 16-bit main parameter spans 51200 steps (`0xC800`). The heat pump converts it over a fixed
range of -40 to +80 degrees Celsius. The thermostat converts it over two range parameters of its own,
and no way to read them over the air is known.

The two devices differ in what a command carries. The thermostat takes the active mode in functional
parameter 9 (`0` off, `1` auto, `2` manual). The heat pump takes its on/off state in functional
parameter 16 (`0x4000` on, `0x8000` off), and its setpoint takes none. The heating / cooling bits
(`4`, `8` of FP9) belong to a separate operating-mode command that is not built, and FP15 (the heat
pump's capabilities) and the other bits of FP16 report state: no command selects cooling.

## Decision

1. **A third control path.** `TEMPERATURE_EXECUTE` joins the register and heating-level paths in the
   heating profiles (ADR 0055). Each mode and preset entry names the main parameter and the functional
   parameter its command sends; the profile names the functional parameter of a setpoint. Neither
   device has a current temperature: none is invented. The Atlantic subtypes 22/1 and 22/2 do not
   inherit the 22/0 profile: a subtype the type table names on its own gets no fallback.

2. **Encoding.** `main_parameter = round((C - min) / (max - min) x 51200)`, clamped to the span, and the
   inverse for a status reply (a resolution of about 0.0023 °C over the default range). The heat pump
   uses the fixed range, -40.0 to +80.0 °C. The thermostat assumes the same range, and the YAML keys
   `min_temperature:` and `max_temperature:` (each within -50 to +100 °C, minimum below maximum)
   override it for a device that turns out to use another. The keys are accepted on a thermostat only.
   The card's own range is 5.0 to 30.0 °C.

3. **Thermostat commands**, as the device's own controller sends them:
   - setpoint: the temperature, FP9 `2`;
   - heat (manual): the current target temperature, FP9 `2`; with no target known nothing is sent;
   - auto: the comfort alias `0xD80F`, FP9 `1`;
   - off: `0xD400`, FP9 `0`;
   - presets comfort (`0xD80F`), eco (`0xD80E`) and secured (`0xD80A`): the alias alone, no functional
     parameter. Secured is a selectable custom preset.

4. **Heat pump commands:**
   - setpoint: the temperature, no functional parameter;
   - presets comfort (`0xD80F`), eco (`0xD812`) and the custom presets Setback (`0xD80E`) and Halted
     (`0xD813`): the alias with FP16 `0x4000`;
   - heat is the comfort alias with FP16 `0x4000`; off is `0xD400` with FP16 `0x8000`.
   There is no cooling and no auto mode; FP15 is not read.

5. **The mode is the last command.** Nothing in the status read returns the mode, so the card's mode is
   not observed (`mode_observed` is false in the profile). Until a mode is commanded, a device that
   reports a setpoint or an alias is shown as heating. The setpoint and preset are observed: a status
   reply that carries an alias selects the preset, and one that carries a temperature becomes the target
   and clears the preset.

6. **No motion, one follow-up poll.** Both devices are in `device_has_no_motion()`; a command leaves one
   read-back poll.

## Considered

- **Reading the thermostat's range from the device.** Not possible as far as is known; the default
  range plus a YAML override covers a device that differs.
- **Writing the comfort and eco temperatures.** Rejected: no wire form for an alias write is known, and
  guessing one on a device's stored setpoints is not reversible by the user.
- **Sending a full functional-parameter block a cover gets.** Rejected for the reason of ADR 0056: a
  guessed block costs a refusal.
- **OR-ing the heating bit into the thermostat's mode flags.** Rejected: the heating / cooling bits are
  part of a different command, not of the active mode.

## Consequences

- Both devices are experimental and untested: the main-parameter encoding, the range, the aliases and
  the functional parameters rest on understanding and no capture.
- The thermostat's range is an assumption; a thermostat that converts over another range shows a wrong
  target until the YAML names its range.
- The card's mode shows the last command, so a mode changed at the device shows only after the next
  command.
- A heat pump cannot be set to cooling or read for its capabilities from this component.

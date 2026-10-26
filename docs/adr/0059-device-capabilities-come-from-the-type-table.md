# ADR 0059: Device capabilities come from the type table

<!-- doxygen-label: adr0059 -->

**Status:** Accepted · **Recorded:** 2026-10

Amends [ADR 0052](0052-device-types-follow-the-io-node-type-and-subtype-table.md) (behaviour keys on
the row's capability word) and [ADR 0055](0055-a-climate-entitys-traits-come-from-the-devices-heating-profile.md)
point 6 (the window companions read a capability bit).

## Context

A garage opener of subtype 58 refused everything the cover offered it except open, close and stop: a
50 % position and the favorite alias both drew `PARAMETER_UNKNOWN` (issue #163, corpus
`wisniowski_moto1000_exchange_execute_parameter_unknown` and
`wisniowski_moto1000_exchange_execute_partial_position_refused`). The cover still showed a slider and
a Favorite Position button, because "what can this device do" was not asked of one place.

It was answered in many: a switch over the node type for tilt, another for the ventilation position,
a subtype-58 test for the frame shape, a second copy of that test in the fan platform, a Python list
of kinds with a favorite button and another of kinds with a ventilation button, a YAML key for
brightness that nothing checked against the kind, and a Python table of heaters with window sensors
that a sync test held equal to a C++ one. Each copy could be right alone and still disagree with the
others, and adding a capability meant finding all of them.

## Decision

1. **One capability word per row.** Each row of the node-type table carries a 16-bit word
   (`DeviceFeatures`) of independent bits: `MAIN_PARAMETER_ONLY`, `LEVELS` (intermediate values:
   percent position, brightness, heat level), `TILT`, the named positions `FAVORITE`, `VENT`,
   `PARTIAL` and `PEDESTRIAN`, `EXTENDED_POSITION_FRAME`, `WINDOW_COMPANIONS` and `INVERTED`. A row
   states its whole word and is never merged with its type's row. Platforms, the hub and the codegen
   ask that word and never branch on a node type or subtype number for a capability. A bit says
   whether a device can do something; how it does it (register layouts, modes, presets) stays in a
   per-family profile, as the heating profile does ([ADR 0055](0055-a-climate-entitys-traits-come-from-the-devices-heating-profile.md)).

2. **One resolved copy per device.** `resolve_device_features()` computes a device record's word from
   its (type, subtype) and the YAML keys that may override it (`invert_position:`, `dimmable:`); the
   registry runs it whenever a record is stored or learns its type. The table lookup is called by the
   resolver and by checks that run before a device exists.

3. **The subtype-58 rule is one function.** The VELUX KLF 200 API specification, Appendix 2, defines
   subtype 58 as "main parameter only" for a few node types. `main_parameter_only_variant()` applies it
   to every type and, from issue #163, also reads it as "no intermediate positions": it clears `LEVELS`,
   `TILT` and `EXTENDED_POSITION_FRAME` (each needs a functional parameter or an intermediate value)
   and sets `MAIN_PARAMETER_ONLY`. The generalisation to every type and the reading as "no
   intermediate positions" are this component's, not the specification's. The 58 rows and the lookup
   fallback both use it.

4. **Named positions are main-parameter values of the family.** The specification defines aliases such
   as the window opener's ventilation value per actuator profile, independent of the functional
   parameters (KLF 200 section 14.2; section 14.1 gives the stop value). Subtype 58 therefore keeps
   them. `NAMED_POSITIONS` is the one table that pairs a position's feature bit, its main-parameter
   value and the command that sends it; the frame builder, the value check
   (`device_accepts_main_parameter()`), the command names and the Python companion buttons all read it.
   `PARTIAL` (`0xD809`) and `PEDESTRIAN` (`0xD807`) have no public source and are experimental.

5. **`EXTENDED_POSITION_FRAME` decides the frame shape.** A position or force-open EXECUTE uses the
   8-byte form with functional parameter 1 set only when the row has the bit; otherwise it uses the
   6-byte main-parameter form. On covers, functional parameter 1 is the travel speed on most node types
   (KLF 200 Appendix 2), so the `silent:` key needs the bit too. The Appendix lists parameter 1 as the
   slat orientation on type 1 and the upper curtain on type 13, so those two lose the bit
   (experimental).

6. **A capability is removed only when a device has refused it.** The garage family loses `FAVORITE`
   because the garage opener of issue #163 refused the favorite alias (one device, applied to the
   whole family, so the subtype 0 row rests on an inference); the window, gate and
   rolling-door families keep it. An unsupported named position is refused by the motor before
   authentication and nothing moves, so keeping one costs a refused command, not a movement. Rows 7/58
   and 24/58 inherit their family's favorite.

7. **Undeclared kinds keep their entity's defaults.** A device whose type is not declared, or has no
   row, takes `undeclared_kind_features()` of the entity that registered it: a cover accepts a slider,
   the favorite and ventilation positions and the 8-byte frame, a light, switch or lock the 8-byte
   frame, an exterior-heating fan its heat levels in the 8-byte frame, and no class gets tilt. Once the radio teaches the real type, the row takes over.

8. **The Python mirror is literal and checked.** `hub_validators.py` carries `FEATURE_*` and
   `DEVICE_FEATURES` (one entry per row of a YAML-named type whose word is not zero, plus the explicit
   zero where its type's word would otherwise be inherited), and `tests/sync/device_features_sync_test.cpp`
   checks every bit and the whole word of every (type, subtype) against the C++ table, the pattern of
   [ADR 0015](0015-table-driven-tuning-registry-with-a-sync-gate.md). A generator was not worth a build
   step for a table that changes rarely.

9. **Keys a kind cannot use fail validation.** One helper, `require_feature()`, rejects a key when the
   declared kind lacks a required bit or has a forbidden one, with a message that names the key, the
   kind and what to remove. `silent:` requires `EXTENDED_POSITION_FRAME`; `dimmable: true` is refused
   where the row is `MAIN_PARAMETER_ONLY` (the 6/0 row has no `LEVELS` on purpose, because `dimmable:`
   decides that for lights). A config that declares no kind passes: nothing says what the device is.

10. **A cover without `LEVELS` is a discrete cover.** It offers open, close and stop and no slider. It
    shows closed only when the device reports fully closed (inversion respected) and open for every
    other report, so a door stopped half-way shows open, and it is an assumed-state cover so Home
    Assistant keeps both buttons enabled. A position between open and closed is refused at the hub
    before a frame is built, through the same value check that guards named positions.

11. **Unconfirmed kinds are marked, and the config says so.** A row that no capture or report confirms
    on a real device carries the `EXPERIMENTAL` identity flag (subtype rows only). Discovery and pairing
    logs print "(experimental)" from it, the docs table marks it, and a config that declares one logs
    a warning at build time that names the kind and links to the page for reporting a result. Python
    keeps its copy as `KIND_EXPERIMENTAL`, bit 15 of the same word, so the sync test covers it. Rows 1,
    10 and 13 are not flagged: warning every user of those common kinds for a change in one feature
    would be noise; their changed behaviour is marked experimental in the row's Doxygen and the docs.
    A second sync test checks the node-type table in the docs against the C++ table.

12. **Each cover kind has a default Home Assistant device class.** A cover that declares
    `io_device_type` and no `device_class:` gets its kind's class (garage, gate, door, window, shutter,
    blind, shade, curtain, awning). The table is Python-only, because no C++ reads a device class, and
    an explicit `device_class:` always wins, including the empty string.

## Considered

- **Generate the Python mirror from the C++ table.** Rejected: a generated file in the build and in
  review for a table that changes rarely. The sync test turns drift into a failing test instead.
- **Keep the per-kind lists and add a sync test for each.** Rejected: it keeps one copy per capability
  and per language, and the lists still disagree about what a subtype changes.
- **Remove the favorite from every kind where no device has been seen to accept it.** Rejected: a
  missing confirmation is not a refusal. A bit leaves a row only on a device's refusal.
- **Flag rows 1, 10 and 13 as experimental.** Rejected for the reason in point 11.
- **Round an intermediate position on a discrete cover to open or closed.** Rejected: the user asked
  for something the device cannot do, and a refusal that logs why is clearer than a different move.

## Consequences

What existing configurations see:

- **The garage Favorite Position button is gone** on garage openers of every subtype (5/0 and 5/58).
  A garage opener of subtype 0 keeps its percent slider; a 5/58 does not.
- **Percent positions are refused on 5/58**, and 7/58 and 24/58 are open, close and stop covers
  (experimental, inferred from the shared subtype definition and the 5/58 of issue #163). All three
  show open or closed, and assume the state so both buttons stay enabled.
- **Type 10 loses the tilt slider**, and 1/58, 10/58, 17/58 and 18/58 have no tilt, because the
  Appendix lists no slat parameter for type 10 and subtype 58 clears the bit.
- **Types 1 and 13 send the short position frame** (6 bytes) and refuse `silent:`.
- **A config with `silent:` on a kind without a speed parameter fails validation**: garage openers,
  types 1 and 13, and any subtype 58. The same holds for `dimmable: true` on a light of subtype 58.
  Each error names the key to remove.
- **A cover declared with the raw `io_device_type: 0x14`** (a ventilation point, not a cover) loses
  the ventilation button it was given.
- **Garage openers get a disabled Partial Position button and gate openers a disabled Pedestrian
  Position button** (experimental); enable them in Home Assistant.
- **Covers that declare a kind get its default `device_class`.** A user who wanted no class sets
  `device_class: ""`.
- **Declaring an experimental kind logs a warning** when the config is built. It does not stop the
  build.
- A light without `dimmable:` and a cover now refuse an intermediate or out-of-range value at the hub,
  where only the entity used to prevent it. An explicit `invert_position: false` survives a type
  learned from the radio.

What it costs:

- The assumed-state trait of a discrete cover follows how Home Assistant's frontend enables the Open
  and Close buttons, read from its source. It has not yet been exercised against a running instance.
- Traits are read when Home Assistant connects. A cover that declares no kind learns it from the
  radio, and its traits change only at the next reconnect; the generated YAML block always declares
  the kind, and the docs say to keep it.
- The subtype-58 reading as "no intermediate positions" rests on one device. A 58 device that does
  take positions needs a row of its own.
- The `PARTIAL` and `PEDESTRIAN` values rest on no public source. A motor without the position refuses
  it and nothing moves.

Extending it:

- **A capability** is one bit with its evidence, the rows that have it, its Python mirror line, an
  invariant in the table test when it constrains other bits, and a read from the device's resolved word.
- **A named position** is one `NAMED_POSITIONS` entry, one `CoverCommand` value and one companion
  button row in the Python codegen.
- **A device kind** is one row.
- **Behaviour that differs by manufacturer** needs a manufacturer column and a lookup argument; the
  lookup's signature leaves room for it.
- **A cover family with a different wire form** needs a per-family profile, as heaters have, once a
  capture or a reporter shows the form.

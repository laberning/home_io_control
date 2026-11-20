# ADR 0110: A device value's row says how it is written too

<!-- doxygen-label: adr0110 -->

**Status:** Accepted, amends [ADR 0068](0068-a-domestic-hot-water-device-is-a-water-heater.md), [ADR 0081](0081-a-water-heaters-boost-and-away-travel-in-the-operating-mode-word.md), [ADR 0101](0101-device-readings-are-one-named-table-with-sources-and-one-codec-vocabulary.md), [ADR 0105](0105-every-device-value-write-is-judged-by-one-rule-per-register.md) and [ADR 0106](0106-a-water-heaters-values-are-readings-of-the-same-table.md) · **Recorded:** 2026-11

Builds on [ADR 0067](0067-a-model-below-the-subtype-for-products-that-share-a-wire-type.md) (a model row carries its own
capabilities) and [ADR 0098](0098-device-capabilities-are-a-named-set.md) (capabilities are names).

## Context

[ADR 0101](0101-device-readings-are-one-named-table-with-sources-and-one-codec-vocabulary.md) put every value a device
reports into one table, with the source that says how the value is read. Writes stayed hand-built: the water heater's
target, boost and away are three functions of the heater controller, and the target range is one pair of constants that
every water heater shares ([ADR 0068](0068-a-domestic-hot-water-device-is-a-water-heater.md)).

Heaters that report the same node type as the Atlantic Calypso differ in all of it
([pull request #246](https://github.com/laberning/home_io_control/pull/246) and
[#248](https://github.com/laberning/home_io_control/pull/248), both tested on the contributors' heaters):

- One target is written as a main parameter over 20 to 70 °C instead of 20 to 62 °C.
- Another takes its target as a shower count (2, 3 or 4, for 50, 60 and 70 °C) written to a register, only in manual
  mode, and reads the temperature it heats to from another register.
- Boost and away are an operating-mode word that reads the other way round on one heater (`4900` at rest, `4A00` boost),
  and flag registers written and read directly on another.
- One answers no useful status read, so its poll must not send one.

Adding each as code in the controller would repeat the problem the readings table solved for reads. The same facts
(a range, a scale, the register a value lives in) would be stated once for reading and again for writing.

## Decision

1. **A row of the table can say how its value is written** (`DeviceReadingRow::write`, a `ValueWrite`), with the same
   form as the read side: a register write is encoded and judged by the register's row
   ([ADR 0105](0105-every-device-value-write-is-judged-by-one-rule-per-register.md)), a main-parameter write over the
   range of the row's read side. A range or a scale exists once, and a value outside it is refused, never clamped.
2. **A write side has four parts.**
   - The **target** is where the value goes: `RegisterWrite{register}`, `MainParameterWrite{}` (an EXECUTE with the
     value as the main parameter), or `FunctionalParameterWrite{fp, on_word, off_word}` (an EXECUTE that keeps the main
     parameter and sets one functional parameter to a fixed word for a flag value of 1 or 0).
   - A **guard** says when the write may be sent: a reading equals a value or has a bit set. A guard whose reading has
     been read and is false refuses the write with a warning and sends nothing; a guard whose reading has not been read
     yet lets the write try, so a heater that never reports the reading is not locked out.
   - A **before-write** is a fixed register write sent first. Its failure is logged and the target is sent anyway, as
     the boost's duration register always was ([ADR 0081](0081-a-water-heaters-boost-and-away-travel-in-the-operating-mode-word.md)).
   - A **rounding policy** (`EXACT` by default, `NEAREST`, `DOWN`, `UP`, with an optional step) says what a value
     between the steps of a stepped target becomes. The steps are the knots of a scaled register form, or the row's
     step for a main parameter; no step means any value. `EXACT` refuses a value between steps without sending, so a
     later change of the policy is one table word.
3. **After a write the hub reads the row's read side again**: a register read for a register source, a status read for a
   status source. A write side carries no read-back of its own.
4. **Several rows may play one part of a reading.** The device's capabilities select exactly one
   (`device_reading_row()`); no match or two matches is a table fault that a test catches, not a runtime guess.
5. **Capability groups.** A group names a base capability (`WATER_HEATER`) and mutually exclusive members
   (`HOT_WATER_TARGET`: `HOT_WATER_TARGET_20_62`; boost and away follow as groups of their own). Every kind that has the
   base, model rows included, carries exactly one member of each group, and the rows of a reading with several rows are
   gated by the members of one group, so selection by capability is complete and unambiguous. A model row combines one
   member per group. A kind that loses the base capability loses the members with it.
6. **The water heater's target is its own reading**, `HOT_WATER_TARGET`, whose row's status source carries the range
   (`StatusMainParameterSource::range`). The climate card keeps `STATUS_SETPOINT`. The target range is a product fact of
   the model, so it is a row, never a YAML key and never a profile table of its own. `device_temperature_range()`, the
   status decode, the target write and the card's traits all take it from the selected row.
7. **A functional parameter field is read as "field equals N"** (`FIELD_EQUALS`), replacing the field that read 1 only,
   so a heater that reports boost as 2 differs by one number in a row.
8. **A stepped register target** uses a scaled byte codec (`SCALED_BYTE`) that shares the knot logic of the scaled word
   codec, and the card's target step comes from the row (its `step`, or the spacing of evenly spaced knots, otherwise 1 °C).
9. **The poll derives its status read from the rows.** A water heater's poll reads the status only when one of its
   selected rows reads from a status reply; a device whose rows are all registers sends no status read. The functional
   parameter request bytes of the Calypso's status read stay constants. `NO_STATUS_READ` stays for a device that has no
   state to poll (the heat pump).
10. **Boost and away are rows too**: their read side is a field of the operating-mode word, their write side a
    `FunctionalParameterWrite` or a `RegisterWrite` of a flag register, guarded by the capability word the heater
    reports, with the duration register as the before-write. The Calypso's sequence (guard, duration, word, status
    read-back) is unchanged. Ending boost does not end away by any rule of the hub: the read-back shows what the heater
    did.

## Consequences

- A water heater model with another target range, another boost word, flag registers or a stepped target is table
  rows and a capability, with no controller or card code.
- The target range, the scale and the register of a value exist once. A heat pump's setpoint and a number such as a
  water heater's manual shower count are writes of a row, not new functions.
- The water heater's target and mode writes go through one function, `HeaterController::write_value()`, which selects
  the device's row, plans the write without sending (`plan_value_write()`, pure, tested without a radio), sends it and
  reads back. The planner is the write twin of the status decode.
- The card's range and step follow the row through `HubHeatingPort`. A water heater's card shows no target range until
  the hub knows the device.
- A new capability that selects a row is one `DeviceFeature` enumerator, the member in its group, the rows that carry
  it and its name in the codegen mirror. The group table test fails when a kind has none or two members of a group, or
  when a shared reading's row is gated by a capability outside its group.
- The Calypso's frames are unchanged, with one addition: the target write now ends with a status read of the heater,
  as the boost and the mode writes did, so the card shows the target the heater holds before the poll that follows.
- [ADR 0068](0068-a-domestic-hot-water-device-is-a-water-heater.md)'s fixed 20 to 62 °C scale is the Calypso's row, and
  [ADR 0081](0081-a-water-heaters-boost-and-away-travel-in-the-operating-mode-word.md)'s words and durations are the
  Calypso's rows of boost and away. [ADR 0106](0106-a-water-heaters-values-are-readings-of-the-same-table.md) rejected a
  second target reading while the range was fixed; the range now differs by model.

## Considered options

- **A water heater profile table per model, like the heater's `HeatingProfile`:** rejected. It is a second table of the
  same kind as the readings table, with the range in the profile and the decoding in the readings.
- **Fold water heaters into `HeatingProfile`:** rejected. That table is shaped for climate (HVAC modes, presets,
  programme periods); the water heater's modes follow [ADR 0090](0090-a-water-heaters-modes-are-chosen-by-what-heats.md).
- **One capability per product:** rejected, products do not compose; one member per concern does.
- **A default row with an override:** rejected, the precedence would be hidden; groups make exactly one row apply.
- **The range as a YAML key or on the model row:** rejected, it is a product fact, and the model row would turn into
  the profile table again.

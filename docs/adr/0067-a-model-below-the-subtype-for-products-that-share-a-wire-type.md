# ADR 0067: A model below the subtype for products that share a wire type

<!-- doxygen-label: adr0067 -->

**Status:** Accepted · **Recorded:** 2026-11

Amends [ADR 0059](0059-device-capabilities-come-from-the-type-table.md): the row a device's
capabilities come from is the (type, subtype, model) row, where a (type, subtype) has models.

## Context

[ADR 0059](0059-device-capabilities-come-from-the-type-table.md) makes the node-type table the one
place that says what a device can do: one row per (type, subtype), and every capability check reads
that row's word. The heating profile is keyed the same way
([ADR 0055](0055-a-climate-entitys-traits-come-from-the-devices-heating-profile.md)).

An Atlantic Adelis towel dryer breaks the assumption that the (type, subtype) names one product. Scan
Paired Devices lists both units of
[issue #202](https://github.com/laberning/home_io_control/issues/202) as an electrical heater with an
adjustable setpoint (`0x34/1`), the kind of the AGILIA radiator. Yet the towel dryer needs other
handling: its mode lives in another register (corpus `atlantic_adelis_probe_mode_reads`), boost and
drying are a register of their own, the regulator reads "none" while the element runs at full power,
and it has no window function, so the radiator's Window sensor and Open Window Detection switch do
nothing on it. Nothing the hub reads during discovery or pairing tells the two apart.

The window entities are created at codegen, and a climate entity's traits are fixed at setup, so the
choice has to be known before the device answers anything.

## Options considered

- **Invent a subtype number for the towel dryer.** Rejected: the subtype is what the device sends. A
  made-up value contradicts the type the hub learns from the radio and every declared-against-reported
  check, and could collide with a real subtype later.
- **Detect the towel dryer at runtime and switch profiles.** Rejected as the selector: the window
  entities and the traits are fixed before the first poll. A runtime read stays useful as a check.
- **A per-platform YAML flag (`heater_variant:` on `climate:` only).** Rejected: it would be a second
  place that decides what a device can do, outside the table and its Python mirror.
- **Model rows below a (type, subtype) in the same table, declared with `io_model:`** (chosen).

## Decision

- The node-type table gets **model rows** below a (type, subtype) row. A model row states its own
  whole capability word, exactly like a subtype row, and keeps the capability class, the platform
  and the flags of the row above it (a table test checks that). Its name is the YAML spelling. The
  first and only model is `towel_dryer` under 52/1, with no `WINDOW_COMPANIONS`.
- **`io_model:` declares it**, on any device-bound platform, and only together with
  `io_device_type:` and `io_subtype:`. The codegen accepts only a model the declared (type, subtype)
  offers. A model is never learned from the radio: there is nothing on the wire to learn it from.
- **Model 0 is the (type, subtype) row itself** (`DeviceModel::NONE`), so every existing device and
  config is unchanged.
- **Lookups take the model as an explicit argument, without a default**: `device_features()`,
  `find_device_type_info()` and `find_heating_profile()`, with an `IoDevice` overload of the last for
  callers that hold a device record. A defaulted argument would compile at every forgotten call site
  and silently drive a towel dryer as a radiator. A model the (type, subtype) does not offer falls back
  to the (type, subtype) row. Lookups of facts that belong to the (type, subtype) (its platform, its
  class, its control path) take no model; a test checks that a model profile keeps the control path of
  the profile above it.
- **The model is a resolver input**, like `dimmable:`: `resolve_device_features()` reads the model row,
  and a record stored again by pairing keeps the declared model.
- **The Python mirror** keys a model row as `"<type>/<subtype>/<model>"` in `DEVICE_FEATURES`, with
  the model values in `DEVICE_MODEL_OPTIONS`; the sync tests check both against the C++ table, and the
  docs node-type table lists the models after their subtype.
- **The hub checks the declaration and only warns.** At the first poll that the device answers, a
  heater whose (type, subtype) has model rows has its model name read and logged once, and the hub
  compares whether the towel-dryer mode register holds a value with the declared model. A mismatch is
  a warning that names the line to add or remove; nothing changes behaviour.

## Consequences

- A towel dryer gets its own heating profile (mode register, a preset register with its own table, a
  "none" preset that ends boost or drying, and the working rate as the action source), and the AGILIA
  radiator's profile and every existing config behave exactly as before.
- **The user must declare the model.** The block that Discover & Pair or Scan Paired Devices prints
  names the wire type only, and a towel dryer declared without the model runs as a radiator until the
  warning is acted on.
- The check reads two registers once per boot on every 52/1 heater, the AGILIA radiator included.
  Whether an AGILIA holds a value in the towel-dryer mode register has not been read yet, so the
  warning says "looks like".
- **Adding a model** is one `DeviceModel` value, one table row with its word, one
  `DEVICE_MODEL_OPTIONS` and one `DEVICE_FEATURES` line, a docs table entry, and the profile or
  behaviour that differs. The water heater's later work reuses the explicit model argument.

# Architecture Decision Records
<!-- doxygen-label: adr_index -->

Each file here records one architectural decision: the problem it solved, the
options weighed, what was chosen, and what that choice costs. They explain
*why* the code looks the way it does — [`docs/architecture_overview.md`](../architecture_overview.md)
describes *what* it looks like today.

## Records by theme

Numbers are stable identifiers, not a reading order. Grouped by theme:

### Structure

| # | Decision | In short |
|---|---|---|
| [0001](0001-layered-protocol-radio-hub-architecture.md) | Layered protocol / radio / hub architecture | Three layers, one composition root; the protocol never names a chip |
| [0004](0004-hub-collaborator-decomposition.md) | Hub split into collaborators | Ten single-purpose objects (eleven with the compile-gated LR1121 controller) instead of one large controller class |
| [0005](0005-pure-decision-logic-separated-from-io.md) | Pure decisions separated from I/O | Frame classification is side-effect-free and host-testable |

### Radio

| # | Decision | In short |
|---|---|---|
| [0002](0002-direct-spi-radio-drivers.md) | Direct SPI radio drivers | Own the chip drivers rather than reuse ESPHome's generic radio components |
| [0003](0003-shared-software-phy-base-for-sx1262-lr1121.md) | Shared software PHY + IRQ base | SX1262 and LR1121 share one framing codec and one RX/TX state machine |
| [0013](0013-blocking-exchange-on-the-esphome-loop.md) | Blocking radio work on the loop | No FreeRTOS tasks; the operation queue *is* the concurrency model |
| [0020](0020-flash-lr1121-transceiver-firmware-not-the-bootloader.md) | Flash LR1121 firmware, not the bootloader | *Superseded by 0021.* Stage the riskier operation for later; keep every failed flash recoverable now |
| [0021](0021-flash-the-lr1121-bootloader-behind-an-arming-switch.md) | Flash the bootloader, behind an arming switch | Reach the CVE-fixing `0x0104`; gate the one unrecoverable write behind visible armed state and a build-time recovery image |
| [0042](0042-start-preamble-default-comes-from-the-radio-driver.md) | The start preamble's default comes from the radio driver | Measured: the software PHY (SX1262/LR1121) needs 48 bytes where the SX1276's register PHY is answered every time at the protocol's 32; an explicit `normal_start_preamble:` still wins, including downward |
| [0028](0028-channel-policy-is-a-property-of-the-frame-not-of-the-chip.md) | Channel policy is a property of the frame | One shared listen primitive, three named policies picked by reply shape — not by chip |
| [0029](0029-start-preamble-is-a-property-of-the-target.md) | Start preamble is a property of the target's power class (amended by 0040) | `LONG_PREAMBLE` iff `CTRL1_LOW_POWER`, iff the per-device `low_power` YAML property (default false) — not iff the frame is a start frame (roll-call: ADR 0037) |
| [0035](0035-fem-support-is-a-behaviour-profile-boards-always-supply-pins.md) | FEM support is a behaviour profile; boards always supply pins | `fem:` selects front-end switching behaviour only; every board package spells out every pin its profile needs, no implicit defaulting |
| [0037](0037-the-roll-call-sweeps-both-power-classes.md) | The roll-call sweeps both power classes | A low-power pass (CTRL1=0x30, long preamble, all channels) then the unchanged always-alive pass; scan_power_classes narrows it |
| [0039](0039-pairing-sends-discover-confirm-and-tolerates-no-answer.md) | Pairing sends a discover-confirm (0x2C) and tolerates no answer | Universal `pairing_discover_confirm` mode (`skip`/`send`/`send_with_ack`); the step never fails the attempt; try 2 alone rotates channels (deviates from ADR 0028) |
| [0040](0040-low-power-start-preamble-follows-the-wake-belief.md) | A low-power device's start preamble follows its wake belief | Tries ordered by per-device evidence (short first only while believed travelling, then the wake-up preamble; a silent exchange spends the evidence; the post-`stop` poll gets three tries); amends 0029; `low_power_wake_belief` switch, `low_power_wake_preamble` length |
| [0044](0044-chip-neutral-transmit-intent-wake-level-idle-scanning-and-air-time.md) | Chip-neutral transmit intent | Every transmission carries its wake level; a driver may scan idle channels by itself; a try starts only if its transmission ends inside the exchange budget |
| [0045](0045-listen-before-talk-is-part-of-sending.md) | Listen-before-talk is part of sending | The radio checks the transmit channel inside `send_packet()`, retuning first when the idle receiver is elsewhere; the engine keeps the retry policy and, since the 2026-10 amendment, checks only frames with START set |

### Behavior

| # | Decision | In short |
|---|---|---|
| [0006](0006-management-actions-as-native-api-actions.md) | Admin ops as native API actions | Rarely-used operations get no permanent entity |
| [0016](0016-sender-events-are-opt-in-by-allowlist.md) | Sender events are opt-in | The radio has no ownership marker, so nothing fires until named |
| [0017](0017-device-poll-hint-shortens-but-never-stretches.md) | Poll hint shortens, never stretches | The configured interval is a ceiling, not a target |
| [0024](0024-diagnostic-probes-gated-and-isolated-from-the-status-decoder.md) | Diagnostic probes, gated and isolated (amended by 0060, 0062) | Undecoded opcodes only reach paired devices behind an opted-in config, and can never reach the status decoder |
| [0030](0030-predictions-are-kept-apart-from-observations.md) | Predictions are kept apart from observations | An optimistic guess lives in its own overlay, withdrawn on failure and superseded per-axis by real data — never written into a reported field |
| [0031](0031-oneway-vendor-wire-behaviour-is-driven-by-manufacturer.md) | 1W vendor wire behaviour is driven by `manufacturer:` | `manufacturer:` selects the 1W `CMD_EXECUTE` ACEI byte (`0x43` Somfy / `0x61` Velux); `execute_broadcast: typed\|all` is a separate handheld-vs-class-bound axis, not a vendor one |
| [0032](0032-oneway-velux-enrollment-gesture.md) | 1W enrollment follows the gesture the target's `manufacturer:` expects | `velux` → `0x39` broadcast, a `0x30` sweep across `{roller_shutter, vertical_exterior_awning, dual_roller_shutter}` under one sequence, then STOP+DOWN; `somfy` path unchanged; `enrollment_classes:` overrides the sweep list |
| [0041](0041-unset-oneway-power-class-comes-from-the-manufacturer-profile.md) | An unset 1W `low_power:` resolves from the manufacturer profile | `velux` → `ALWAYS_ALIVE` (an awake VELUX receiver rejects the 1024-byte preamble, and 1W has no ACK to reveal it); Somfy and unrecognised manufacturers stay `LEGACY_LONG`; an explicit key always wins |
| [0033](0033-heating-send-path-bypasses-the-cover-machinery.md) | Heating send path bypasses the cover machinery | 2W heating does a plain send-and-receive — no status decode, no poll backoff, no ADR 0030 overlay (heating has no observation stream); the write-only premise is superseded by 0054 |
| [0054](0054-heaters-are-read-back-over-the-private-register-protocol.md) | Heaters are read back over the private register protocol | One register table drives get, set and decode; a climate entity polls room temperature, setpoint, mode and regulator state (default 5 min) and shows only what the heater answered; a write is confirmed by a read; the window functions are `set_open_window_detection` and a `window_open` read; amended 2026-10: the setpoint heater offers no presence preset, its automatic modes are the presets `Auto` and `Program` |
| [0055](0055-a-climate-entitys-traits-come-from-the-devices-heating-profile.md) | A climate entity's traits come from the device's heating profile | One profile row per heater kind (control path, modes, presets, observed-state table, poll registers, features); `device_supports_climate_control()` means a profile exists, `device_uses_heating_registers()` gates every register frame; the plain heater is commanded with heating levels in the standard EXECUTE; window companions are declared at validation from the declared kind; amended 2026-10: every preset is observed, a profile carries no last-commanded marker |
| [0056](0056-exterior-heating-is-a-fan-entity.md) | Exterior heating is a fan entity | Type 21 gets the `HEATER_LEVEL` capability class and the `fan:` platform; heat `p` % is IO position `100 - p` like a dimmable light; no motion, one follow-up poll per command; no restored state |
| [0057](0057-temperature-setpoints-travel-in-the-main-parameter.md) | Temperature setpoints travel in the main parameter | A third control path for the heating temperature interface and the generic heat pump: temperature encoded over a -40..+80 °C default range (YAML override), comfort / eco selected by alias, FP9 mode flags only on the thermostat, mode shown as last command, no current temperature |
| [0043](0043-an-unconfirmed-movement-command-is-resent-once-to-a-device-that-normally-confirms.md) | An unconfirmed movement command is re-sent once to a device that normally confirms | Learned in RAM from the first EXECUTE the device closes with a reply; favourite/vent never re-sent; at most one re-send inside the exchange budget; no device list, nothing stored |
| [0049](0049-a-rain-limitation-is-derived-from-observed-status-never-predicted.md) | A rain limitation is derived from observed status, never predicted | A separate `limited_by_rain` flag from two evidence rules on ordinary status replies (rain-sensor originator; a clamped command while rain evidence is fresh); an explicit `CMD_ERROR_RESP` still wins; no extra polling |
| [0050](0050-rain-is-read-by-an-opt-in-limitation-poll.md) | Rain is read by an opt-in limitation poll | `rain_sensor_poll_interval` reads the minimum limit on a jittered interval and drives a Rain sensor binary sensor (89 % rule, unknown after three misses); independent of the derived flag; transmit time is the user's choice |
| [0051](0051-pairing-reads-the-key-refusal-and-checks-an-unconfirmed-key.md) | Pairing reads the key refusal and checks an unconfirmed key | `0xFE` byte decoded as `code << 3 \| state`; a refused key transfer ends the attempt (`key_transfer_refused`); an unconfirmed one gets an authenticated name read before it fails; `pairing_key_transfer: auto` pulls a remote controller's key, rolls call under it and registers the devices that hold it |
| [0058](0058-pairing-reads-the-heater-behind-an-atlantic-pairing-node-directly.md) | Pairing reads the heater behind an Atlantic pairing node directly | After a pull from an Atlantic remote-controller node, one register read of the next address finds the heater; the roll-call is the fallback; a hub-key pull with nothing found ends `hub_key_returned`; pairing-registered devices keep getting their block; the key proof is retried |

### Interfaces and naming

| # | Decision | In short |
|---|---|---|
| [0008](0008-io-device-id-config-key-naming.md) | `io_device_id`, not `device_id` | ESPHome reserves `device_id`; protocol keys take an `io_` prefix |
| [0009](0009-companion-entity-ids-declared-at-schema-time.md) | Companion IDs declared at validation | Late-created IDs are silently dropped at runtime |
| [0015](0015-table-driven-tuning-registry-with-a-sync-gate.md) | Table-driven tuning registry | Python and C++ stay hand-written, a build gate catches drift |
| [0018](0018-yaml-is-the-source-of-truth-hub-persists-nothing.md) | YAML is the only source of truth | The hub persists nothing; pairing ends in paste-and-reflash |
| [0019](0019-declare-what-the-protocol-cannot-report.md) | Declare, don't guess | Undetectable capabilities are YAML options, not inferences |
| [0025](0025-persist-monotonic-counters-as-the-only-exception-to-0018.md) | Counters are the one exception to 0018 | 1W's rolling sequence cannot be re-derived, so it is persisted — and nothing else is |
| [0027](0027-controller-identities-replace-node-addressing-for-1w.md) | Controller identities for 1W | 1W is class-addressed, so the sending identity replaces node addressing |
| [0036](0036-hub-entities-come-from-hub-block-flags-never-a-platform-entry.md) | Hub entities come from hub-block flags, never a platform entry | Discover & Pair joins its siblings behind `discover_and_pair_button:`; the old `button:` platform is deprecated, not deleted outright |
| [0052](0052-device-types-follow-the-io-node-type-and-subtype-table.md) | Device types follow the IO node-type and subtype table | A 16-bit `DeviceType` and one (type, subtype) table drive names, entity class, platform and behaviour flags; six YAML names change and the old ones fail with the replacement |
| [0053](0053-a-device-may-carry-its-own-system-key.md) | A device may carry its own system key | An optional `system_key:` on any device entity; the exchange engine picks the key per frame from the peer's node, falling back to the hub key; Scan Paired Devices sweeps once per distinct key |
| [0059](0059-device-capabilities-come-from-the-type-table.md) | Device capabilities come from the type table | One capability word per (type, subtype) row, resolved once per device; the subtype-58 rule is one function; named positions are one table; the Python mirror is sync-tested; keys a kind cannot use fail validation; unconfirmed kinds are marked experimental and warned about |
| [0060](0060-a-state-changing-diagnostic-probe.md) | A state-changing diagnostic probe (amends 0024) | `subscribe_event` is allowlisted to two events, not sweepable, never re-sent after the device accepted it; reports are recorded on a diagnostic sensor and never applied to device state |
| [0061](0061-a-moving-status-report-opens-the-follow-up-window.md) | A moving status report opens the follow-up window | A report that reads as moving opens the window when none is open; a report that finds it expired ends it instead of reopening it; only kinds that report motion through status may open one |
| [0062](0062-a-read-only-data-transport-probe.md) | A read-only data-transport probe (amends 0024) | `read_object` opens, reads and closes five allowlisted objects over `0x46`/`0x4A`; read direction only by construction, capped, raw output never applied |

### Security

| # | Decision | In short |
|---|---|---|
| [0007](0007-self-contained-aes-implementation.md) | Self-contained AES-128 | Avoid an mbedTLS linkage dependency for one fixed primitive |
| [0011](0011-key-material-redaction-in-logs.md) | Key material masked in all logs | Masking is unconditional, not tied to the frame-logging flag |
| [0012](0012-key-extraction-responder-for-owned-devices.md) | Key-extraction responder | Emulate a device so a user's own hub hands over its credentials |
| [0022](0022-unauthenticated-status-frames-are-never-applied.md) | Unauthenticated status frames are never applied | Drop the merge rather than pay a challenge or an attacker-triggerable poll |
| [0026](0026-1w-enrollment-is-required-and-safety-comes-from-a-physical-interlock.md) | 1W enrollment needs no arming switch | The receiver's own physical PROG hold is the interlock, and even a successful enrollment only adds a controller, never displaces one |

### Testing

| # | Decision | In short |
|---|---|---|
| [0010](0010-golden-frame-corpus-as-regression-source-of-truth.md) | Real captured frames as test truth | Committed YAML captures, generated fixtures, no hand-written byte guesses |
| [0014](0014-host-tests-against-stubbed-esphome-headers.md) | Host tests on stubbed ESPHome headers | Plain `g++`, no ESPHome or hardware — with a stated fidelity cost |
| [0023](0023-reference-material-as-a-corpus-origin.md) | `reference-material` corpus origin | Real devices this project doesn't own, sourced from published third-party material |

### Documentation

| # | Decision | In short |
|---|---|---|
| [0034](0034-doxygen-syntax-generated-at-staging-time.md) | Doxygen syntax generated, not committed | `.md` files stay pure GitHub Markdown; `scripts/stage-docs.py` injects `{#label}` / `\subpage` / link rewrites into the build copy |


## All records

Every record, in number order (the tables above group them by theme).

<!-- doxygen-subpages -->
- [ADR 0001: Layered protocol / radio-driver / hub architecture](0001-layered-protocol-radio-hub-architecture.md)
- [ADR 0002: Direct SPI radio drivers instead of ESPHome's built-in radio components](0002-direct-spi-radio-drivers.md)
- [ADR 0003: Shared software PHY and IRQ orchestration base for SX1262/LR1121](0003-shared-software-phy-base-for-sx1262-lr1121.md)
- [ADR 0004: Hub responsibilities split into single-purpose collaborators](0004-hub-collaborator-decomposition.md)
- [ADR 0005: Pure decision logic kept separate from I/O](0005-pure-decision-logic-separated-from-io.md)
- [ADR 0006: Hub-level admin operations as native API actions, not permanent entities](0006-management-actions-as-native-api-actions.md)
- [ADR 0007: Self-contained AES-128 implementation](0007-self-contained-aes-implementation.md)
- [ADR 0008: `io_device_id`, not `device_id` — and an `io_` prefix for protocol keys](0008-io-device-id-config-key-naming.md)
- [ADR 0009: Companion entity IDs are declared during schema validation](0009-companion-entity-ids-declared-at-schema-time.md)
- [ADR 0010: Real captured frames are the regression-test source of truth](0010-golden-frame-corpus-as-regression-source-of-truth.md)
- [ADR 0011: Key material is masked wherever frames are logged, unconditionally](0011-key-material-redaction-in-logs.md)
- [ADR 0012: Key-extraction responder for recovering credentials from an owned installation](0012-key-extraction-responder-for-owned-devices.md)
- [ADR 0013: Radio work is blocking, on the ESPHome loop, with no FreeRTOS tasks](0013-blocking-exchange-on-the-esphome-loop.md)
- [ADR 0014: Host unit tests build against stubbed ESPHome headers](0014-host-tests-against-stubbed-esphome-headers.md)
- [ADR 0015: Table-driven tuning registry, guarded by a cross-language sync gate](0015-table-driven-tuning-registry-with-a-sync-gate.md)
- [ADR 0016: Sender events are opt-in, by explicit allowlist](0016-sender-events-are-opt-in-by-allowlist.md)
- [ADR 0017: A device's settle hint may shorten the poll interval, never stretch it](0017-device-poll-hint-shortens-but-never-stretches.md)
- [ADR 0018: YAML is the source of truth; the hub persists no state of its own](0018-yaml-is-the-source-of-truth-hub-persists-nothing.md)
- [ADR 0019: What the protocol cannot report is declared, not guessed](0019-declare-what-the-protocol-cannot-report.md)
- [ADR 0020: Flash LR1121 transceiver firmware, not the bootloader](0020-flash-lr1121-transceiver-firmware-not-the-bootloader.md)
- [ADR 0021: Flash the LR1121 bootloader, behind an arming switch](0021-flash-the-lr1121-bootloader-behind-an-arming-switch.md)
- [ADR 0022: Unauthenticated status frames are never applied to device state](0022-unauthenticated-status-frames-are-never-applied.md)
- [ADR 0023: `reference-material` as a fourth corpus origin, for real devices this project doesn't own](0023-reference-material-as-a-corpus-origin.md)
- [ADR 0024: Diagnostic probes are gated, paired-devices-only, and isolated from the status decoder](0024-diagnostic-probes-gated-and-isolated-from-the-status-decoder.md)
- [ADR 0025: Monotonic counters are the one thing the hub persists](0025-persist-monotonic-counters-as-the-only-exception-to-0018.md)
- [ADR 0026: 1W enrollment is required, and its safety comes from a physical interlock](0026-1w-enrollment-is-required-and-safety-comes-from-a-physical-interlock.md)
- [ADR 0027: Controller identities replace node addressing for 1W](0027-controller-identities-replace-node-addressing-for-1w.md)
- [ADR 0028: Channel policy is a property of the frame, not of the chip](0028-channel-policy-is-a-property-of-the-frame-not-of-the-chip.md)
- [ADR 0029: The start preamble is a property of the target's power class, not of the frame's position](0029-start-preamble-is-a-property-of-the-target.md)
- [ADR 0030: A hub prediction is kept apart from a device observation](0030-predictions-are-kept-apart-from-observations.md)
- [ADR 0031: 1W vendor wire behaviour is driven by `manufacturer:`, and `execute_broadcast` is a remote-shape axis](0031-oneway-vendor-wire-behaviour-is-driven-by-manufacturer.md)
- [ADR 0032: 1W enrollment follows the gesture the target's `manufacturer:` expects](0032-oneway-velux-enrollment-gesture.md)
- [ADR 0033: The heating send path bypasses the cover status/optimistic machinery](0033-heating-send-path-bypasses-the-cover-machinery.md)
- [ADR 0034: Doxygen page syntax is generated at staging time, never committed](0034-doxygen-syntax-generated-at-staging-time.md)
- [ADR 0035: FEM support is a behaviour profile; boards always supply their own pins](0035-fem-support-is-a-behaviour-profile-boards-always-supply-pins.md)
- [ADR 0036: Hub-level entities come from `home_io_control:` flags, never a platform entry](0036-hub-entities-come-from-hub-block-flags-never-a-platform-entry.md)
- [ADR 0037: The roll-call sweeps both power classes](0037-the-roll-call-sweeps-both-power-classes.md)
- [ADR 0038: 1W bursts follow the identity's power class, not a hard-coded preamble](0038-oneway-bursts-follow-the-identity-power-class.md)
- [ADR 0039: Pairing sends a discover-confirm (0x2C) and tolerates no answer](0039-pairing-sends-discover-confirm-and-tolerates-no-answer.md)
- [ADR 0040: A low-power device's start preamble follows its wake belief](0040-low-power-start-preamble-follows-the-wake-belief.md)
- [ADR 0041: An unset 1W `low_power:` resolves from the manufacturer profile](0041-unset-oneway-power-class-comes-from-the-manufacturer-profile.md)
- [ADR 0042: The directed start preamble's default comes from the radio driver](0042-start-preamble-default-comes-from-the-radio-driver.md)
- [ADR 0043: An unconfirmed movement command is re-sent once to a device that normally confirms](0043-an-unconfirmed-movement-command-is-resent-once-to-a-device-that-normally-confirms.md)
- [ADR 0044: Chip-neutral transmit intent: wake level, idle scanning and air time](0044-chip-neutral-transmit-intent-wake-level-idle-scanning-and-air-time.md)
- [ADR 0045: Listen-before-talk is part of sending](0045-listen-before-talk-is-part-of-sending.md)
- [ADR 0049: A rain limitation is derived from observed status, never predicted](0049-a-rain-limitation-is-derived-from-observed-status-never-predicted.md)
- [ADR 0050: Rain is read by an opt-in limitation poll](0050-rain-is-read-by-an-opt-in-limitation-poll.md)
- [ADR 0051: Pairing reads the key refusal and checks an unconfirmed key](0051-pairing-reads-the-key-refusal-and-checks-an-unconfirmed-key.md)
- [ADR 0052: Device types follow the IO node-type and subtype table](0052-device-types-follow-the-io-node-type-and-subtype-table.md)
- [ADR 0053: A device may carry its own system key](0053-a-device-may-carry-its-own-system-key.md)
- [ADR 0054: Heaters are read back over the private register protocol](0054-heaters-are-read-back-over-the-private-register-protocol.md)
- [ADR 0055: A climate entity's traits come from the device's heating profile](0055-a-climate-entitys-traits-come-from-the-devices-heating-profile.md)
- [ADR 0056: Exterior heating is a fan entity](0056-exterior-heating-is-a-fan-entity.md)
- [ADR 0057: Temperature setpoints travel in the main parameter](0057-temperature-setpoints-travel-in-the-main-parameter.md)
- [ADR 0058: Pairing reads the heater behind an Atlantic pairing node directly](0058-pairing-reads-the-heater-behind-an-atlantic-pairing-node-directly.md)
- [ADR 0059: Device capabilities come from the type table](0059-device-capabilities-come-from-the-type-table.md)
- [ADR 0060: A state-changing diagnostic probe](0060-a-state-changing-diagnostic-probe.md)
- [ADR 0061: A moving status report opens the follow-up window](0061-a-moving-status-report-opens-the-follow-up-window.md)
- [ADR 0062: A read-only data-transport probe](0062-a-read-only-data-transport-probe.md)
<!-- /doxygen-subpages -->

## Writing a new one

Number sequentially from the highest existing number. Use the same section
headings (Status / Context / Options considered / Decision / Consequences),
and keep "Options considered" only when real alternatives were weighed — don't
invent losing options to fill the template.

State consequences honestly, including the ones that cost something. A
consequences section listing only benefits means the decision hasn't been
thought through, or the ADR is selling rather than recording.

Diagrams are welcome where structure or sequence is genuinely hard to hold in
your head. Skip them where prose is clearer — a decorative box-and-arrow adds
maintenance without adding understanding.

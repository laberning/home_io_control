# Architecture Overview

This page gives a contributor-oriented map of the Home IO Control component and links the generated API documentation back to the main architectural layers.

## Layer Map

- \ref hioc_protocol "Protocol Layer": frame layout, command builders, cryptographic helpers, and shared protocol utilities. The protocol model is split into cohesive headers (`proto_sizes.h`, `proto_timing.h`, `proto_constants.h`, `proto_device_model.h`, `proto_codecs.h`, `proto_commands.h`, `proto_crypto.h`) with `proto_frame.h` holding only the frame container; each header's includers name their real dependencies directly rather than relying on an umbrella.
- \ref hioc_radio "Radio Driver Layer": the `RadioDriver` abstraction and the SX1276 / SX1262 / LR1121 implementations. Chips without hardware IoHomeOn framing (SX1262 and LR1121) share the software PHY in `radio_soft_phy.h/.cpp` (UART bit-encoding for TX, UART-decode probe with CRC validation for RX) and the IRQ-driven RX/TX orchestration in `SoftPhyDriverBase` (`radio_soft_phy_driver_base.h/.cpp`), which `RadioSX1262`/`RadioLR1121` both derive from — SPI opcode encoding/transport and IRQ bit values/word width stay behind virtual primitives each chip implements. The LR1121 driver is hardware-validated: authenticated 2W exchanges (open/close/stop with real position/state feedback) confirmed against a real Somfy Sunea IO awning motor.
- \ref hioc_hub "Controller Layer": `IOHomeControlComponent` orchestrates setup and loop scheduling through seven collaborator objects — `ExchangeEngine` (authenticated exchanges), `PairingEngine` (discovery and pairing), `ManagementActions` (rename/identify/force-open device actions), `DeviceRegistry` (device table and callbacks), `OperationQueue` (pending-operation coalescing), `StatusPollPolicy` (per-device poll scheduling), and `PairingTelemetry` (per-attempt pairing event recorder, shared with both engines; its read-only companion `pairing_advisor.h` turns a completed attempt into actionable diagnostics). The hub itself is split by concern: `hub_core.cpp` (lifecycle and loop), `hub_operations.cpp` (queued operation dispatch), `hub_status.cpp` (passive receive-side handling), plus thin `hub_pairing.cpp` / `hub_management.cpp` wrappers around their engines. `hub_decisions.h` holds pure frame-classification helpers testable without radio or timing. The "Accept Foreign Pairing" key-extraction responder is `PairingEngine`'s device-role mirror: where `PairingEngine` drives the hub *pairing to* a device (blocking, one call per attempt), `pairing_responder.h/.cpp` is event-driven and stateless-per-call — its pure ARMED_IDLE→SENT_DISCOVER_RESP→SENT_CHALLENGE→EXTRACTED transitions are invoked from new branches inside `process_received_packet_()` (`hub_status.cpp`) while the hub keeps operating normally; the impure side (arming, throwaway node-ID generation, the auto-off timer, and the security log block) lives in `hub_key_extraction.cpp`. Hardware-confirmed end-to-end against this project's own hub; not yet confirmed against a third-party hub — see `docs/home_io_control.md`'s "Key Extraction" section.
- \ref hioc_tuning "Tuning Layer" (sub-group of the Controller Layer): the runtime `TuningConfig`, the table-driven `tuning_registry`, and the optional Home Assistant number/select entities.
- \ref hioc_entities "ESPHome Integration Layer": the runtime entities plus the Python schema/codegen modules that expose the component to ESPHome. Shared device-binding codegen lives in `platform_common.py`; the C++ counterparts are the mixins in `platform_entity_base.h` — `DeviceBoundEntity` for the main entity platforms and `DeviceBoundCompanion` for the auto-generated per-device companion diagnostic sensors (device name, active issue, RSSI, last contact, exchange failures).

## Design Decisions

The reasoning behind the architectural choices below — including options
considered and their trade-offs — is recorded as individual Architecture
Decision Records in [`docs/adr/`](https://github.com/laberning/home_io_control/tree/main/docs/adr).
Start there for *why* something is built the way it is, not just *what* it is.

## Layering Rules

These invariants keep the layers independent; changes should preserve them:

1. The protocol layer is radio-agnostic: no chip names, chip registers, or driver behavior in `proto_*` files. `proto_timing.h` holds only chip-neutral protocol timing.
2. The controller layer is chip-agnostic: hub and engine code interacts with the radio exclusively through `RadioDriver` virtuals (`response_preamble()`, `exchange_wait_slice_ms()`, `discovery_hop_slice_ms()`, `has_fast_tx_rx_turnaround()`, `apply_tuning()`, …). Chip-specific behavior belongs in a driver override, not in an `if (chip == …)` branch; `chip_name()` is for logging only.
3. Chip-specific constants live either in the driver header (`radio_sx1276.h` / `radio_sx1262.h` / `radio_lr1121.h`) or, when they are user-tunable defaults, next to their `TuningConfig` fields in `tuning_config.h`.
4. The composition root is `hub_core.cpp` `setup()`: it is the only place that names concrete driver classes, selecting one by the required `radio_type` YAML field.

## Request Flow

1. A YAML declaration is validated by the Python code-generation modules in \ref hioc_codegen "Python Code Generation".
2. ESPHome codegen wires those declarations to runtime C++ objects such as `IOHomeControlComponent`, `IOHomeCover`, `IOHomeLight`, `IOHomeLock`, `IOHomeSwitch`, `IOHomeDiscoverButton`, and the generated companions (`IOHomeDeviceNameTextSensor` per entity, `IOHomePairingResultTextSensor` alongside the pairing button, `IOHomeAcceptForeignPairingSwitch` when `home_io_control.accept_foreign_pairing: true` — a hub-level option, not a `switch:` platform entry, so it can never be confused with a device-bound `IOHomeSwitch`).
3. Runtime entities call into the hub through the high-level operation methods documented in \ref hioc_hub "Controller Layer".
4. The hub builds protocol frames using helpers from \ref hioc_protocol "Protocol Layer" and sends them through a concrete radio backend from \ref hioc_radio "Radio Driver Layer".
5. Replies, passive updates, and authenticated inbound messages are parsed back through the same protocol layer and merged into the shared device registry before the entity callbacks publish state to Home Assistant.

## Device-Name Flows

The device-name feature now has separate read and write paths that share the same cached UTF-8 registry field.

## Home Assistant Action Surface

Hub-level operations that should not become permanent entities are exposed through ESPHome native API actions: `rename_device`, `identify_device`, and `force_open_device`. All three share one data-driven descriptor class (`detail::ManagementServiceDescriptor` in `management_actions.cpp`), parametrized by action name, argument list, and callback — adding a fourth action is a registration call, not a new class.

1. The ESPHome node needs a normal `api:` block so the Home Assistant native API is available.
2. Home Assistant triggers an action with a normal automation or script step such as `action: esphome.<node_name>_rename_device`.
3. `IOHomeControlComponent` registers each descriptor with `api::APIServer` through its `ManagementActions` collaborator, which then emits `esphome.home_io_control_action_result` so the automation can inspect success and verification details.

Home IO Control enables the required native API compile-time flags internally, so user YAML only needs a normal `api:` block.

Example Home Assistant step:

```yaml
action: esphome.hioc_heltec_v2_rename_device
data:
  device_id: "FEEB1E"
  new_name: "Patio Awning"
```

Replace `hioc_heltec_v2` with the normalized `esphome.name` of the ESPHome node that owns the Home IO Control hub.

### Read Path

1. Each configured cover, light, lock, or switch generates a companion diagnostic text sensor.
2. That sensor schedules a boot-time `GET_NAME` request through the hub's normal queued-operation path.
3. `CMD_GET_NAME_RESP` is decoded from Latin-1 wire bytes into cached UTF-8 text in the shared `IoDevice` record.
4. The same device callback fan-out used by the primary entities updates the generated text sensor in Home Assistant.
5. Name-request failures are isolated from movement/status behavior; unsupported devices simply keep an empty cached name.

### Write Path

1. Home Assistant calls the node-scoped native API action `esphome.<node_name>_rename_device`.
2. `IOHomeControlComponent` validates the supplied IO-homecontrol device ID and normalizes the requested UTF-8 name into the protocol's fixed Latin-1 payload.
3. The hub sends an authenticated `CMD_SET_NAME` exchange through the same radio/exchange stack used by other management operations.
4. A successful acknowledgement triggers an immediate `GET_NAME` readback so the cached `IoDevice.name` field stays authoritative.
5. The hub publishes `esphome.home_io_control_action_result` with success, verification, and any decoded device refusal metadata so the Home Assistant automation can react without requiring always-visible helper entities.

## Main Source Anchors

- Hub entry point: `IOHomeControlComponent` in [hub_core.h](../components/home_io_control/hub_core.h)
- Authenticated exchange engine: [exchange_engine.h](../components/home_io_control/exchange_engine.h)
- Pairing engine: [pairing_engine.h](../components/home_io_control/pairing_engine.h)
- Key-extraction responder (device-role pairing mirror): [pairing_responder.h](../components/home_io_control/pairing_responder.h)
- Exchange/auth state types: [hub_exchange.h](../components/home_io_control/hub_exchange.h)
- Pairing state types: [hub_pairing.h](../components/home_io_control/hub_pairing.h)
- Pairing telemetry recorder: [pairing_telemetry.h](../components/home_io_control/pairing_telemetry.h)
- Pairing traffic advisor: [pairing_advisor.h](../components/home_io_control/pairing_advisor.h)
- Key-material redaction helpers: [redaction.h](../components/home_io_control/redaction.h)
- Pure frame-classification helpers: [hub_decisions.h](../components/home_io_control/hub_decisions.h)
- Device registry: [device_registry.h](../components/home_io_control/device_registry.h)
- Operation queue: [operation_queue.h](../components/home_io_control/operation_queue.h)
- Status poll policy: [status_poll_policy.h](../components/home_io_control/status_poll_policy.h)
- Management actions: [management_actions.h](../components/home_io_control/management_actions.h)
- Radio abstraction: [radio_interface.h](../components/home_io_control/radio_interface.h)
- SX1276 driver: [radio_sx1276.h](../components/home_io_control/radio_sx1276.h)
- SX1262 driver: [radio_sx1262.h](../components/home_io_control/radio_sx1262.h)
- LR1121 driver: [radio_lr1121.h](../components/home_io_control/radio_lr1121.h)
- Shared software PHY (UART framing for chips without hardware IoHomeOn): [radio_soft_phy.h](../components/home_io_control/radio_soft_phy.h)
- Shared software-PHY driver flow (IRQ-driven RX/TX orchestration base class for SX1262/LR1121): [radio_soft_phy_driver_base.h](../components/home_io_control/radio_soft_phy_driver_base.h)
- Protocol frame model: [proto_frame.h](../components/home_io_control/proto_frame.h)
- Runtime tuning config: [tuning_config.h](../components/home_io_control/tuning_config.h)
- Tuning parameter registry: [tuning_registry.h](../components/home_io_control/tuning_registry.h)
- Shared entity mixins: [platform_entity_base.h](../components/home_io_control/platform_entity_base.h)
- ESPHome hub schema: [__init__.py](../components/home_io_control/__init__.py)
- Shared platform codegen: [platform_common.py](../components/home_io_control/platform_common.py)

## Test Corpus

A versioned corpus of real captured IO-Homecontrol frames lives at `tests/corpus/captures/`,
one YAML file per scenario, format spec and contribution workflow in `tests/corpus/README.md`.
`scripts/corpus/build.py` renders the
YAML captures into a git-ignored C++ fixture header (`build/corpus/corpus_generated.h`) before
every host test build — the YAML is the single source of truth, so there is no generated file
to keep in sync or commit. `scripts/corpus/ingest.py` scaffolds new captures from pasted on-air
logs (and re-keys own-hardware captures with `--rekey` so no real system key is ever committed);
`scripts/corpus/validate.py` (`make corpus-validate`, part of `make lint`) enforces schema,
CRC/CTRL0 self-consistency, and cryptographic promises. Five host test suites replay the corpus
through the real protocol/crypto/codec/decision/exchange code
(`tests/corpus_frame_test.cpp`, `corpus_crypto_test.cpp`, `corpus_decode_test.cpp`,
`corpus_classification_test.cpp`, `corpus_exchange_replay_test.cpp`) — an issue-derived capture
becomes a permanent parser/decoder regression fixture the day it's ingested.

## Navigation Notes

- The generated Doxygen UI places groups under **Topics**. In this project, those topics represent the architecture layers above.
- The long-form user documentation still lives primarily in the generated README and YAML reference pages, while this page is meant as the bridge into the API reference.

## TODO and Issue Lists

Doxygen can generate dedicated maintenance lists when comments use structured commands such as `\todo` and `\bug` inside Doxygen comments.

Examples:

```cpp
/// \todo Validate this workaround on Heltec V4 hardware.
/// \bug SX1276 pairing remains unproven on this path.
```

This repo now has `GENERATE_TODOLIST` and `GENERATE_BUGLIST` enabled, but plain `// TODO:` comments are not enough to populate those pages. They need to be converted into Doxygen comments intentionally, otherwise the generated lists stay empty.
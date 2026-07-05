# Architecture Overview

This page gives a contributor-oriented map of the Home IO Control component and links the generated API documentation back to the main architectural layers.

## Layer Map

- \ref hioc_protocol "Protocol Layer": frame layout, command builders, cryptographic helpers, and shared protocol utilities.
- \ref hioc_radio "Radio Driver Layer": the `RadioDriver` abstraction and the SX1276 / SX1262 implementations.
- \ref hioc_hub "Controller Layer": `IOHomeControlComponent` orchestrates setup and loop scheduling through six collaborator objects — `ExchangeEngine` (authenticated exchanges), `PairingEngine` (discovery and pairing), `ManagementActions` (rename-device), `DeviceRegistry` (device table and callbacks), `OperationQueue` (pending-operation coalescing), and `StatusPollPolicy` (per-device poll scheduling).
- \ref hioc_entities "ESPHome Integration Layer": the runtime entities plus the Python schema/codegen modules that expose the component to ESPHome.

## Request Flow

1. A YAML declaration is validated by the Python code-generation modules in \ref hioc_codegen "Python Code Generation".
2. ESPHome codegen wires those declarations to runtime C++ objects such as `IOHomeControlComponent`, `IOHomeCover`, `IOHomeLight`, `IOHomeLock`, `IOHomeSwitch`, `IOHomeDiscoverButton`, and the generated `IOHomeDeviceNameTextSensor` companions.
3. Runtime entities call into the hub through the high-level operation methods documented in \ref hioc_hub "Controller Layer".
4. The hub builds protocol frames using helpers from \ref hioc_protocol "Protocol Layer" and sends them through a concrete radio backend from \ref hioc_radio "Radio Driver Layer".
5. Replies, passive updates, and authenticated inbound messages are parsed back through the same protocol layer and merged into the shared device registry before the entity callbacks publish state to Home Assistant.

## Device-Name Flows

The device-name feature now has separate read and write paths that share the same cached UTF-8 registry field.

## Home Assistant Action Surface

Hub-level operations that should not become permanent entities are exposed through ESPHome native API actions.

1. The ESPHome node needs a normal `api:` block so the Home Assistant native API is available.
2. Home Assistant triggers an action with a normal automation or script step such as `action: esphome.<node_name>_rename_device`.
3. `IOHomeControlComponent` registers the descriptor with `api::APIServer` through its `ManagementActions` collaborator, which then emits `esphome.home_io_control_action_result` so the automation can inspect success and verification details.

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
- Exchange/auth state types: [hub_exchange.h](../components/home_io_control/hub_exchange.h)
- Pairing state types: [hub_pairing.h](../components/home_io_control/hub_pairing.h)
- Device registry: [device_registry.h](../components/home_io_control/device_registry.h)
- Operation queue: [operation_queue.h](../components/home_io_control/operation_queue.h)
- Status poll policy: [status_poll_policy.h](../components/home_io_control/status_poll_policy.h)
- Management actions: [management_actions.h](../components/home_io_control/management_actions.h)
- Radio abstraction: [radio_interface.h](../components/home_io_control/radio_interface.h)
- Protocol frame model: [proto_frame.h](../components/home_io_control/proto_frame.h)
- ESPHome hub schema: [__init__.py](../components/home_io_control/__init__.py)

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
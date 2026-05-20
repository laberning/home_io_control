# Architecture Overview

This page gives a contributor-oriented map of the Home IO Control component and links the generated API documentation back to the main architectural layers.

## Layer Map

- \ref hioc_protocol "Protocol Layer": frame layout, command builders, cryptographic helpers, and shared protocol utilities.
- \ref hioc_radio "Radio Driver Layer": the `RadioDriver` abstraction and the SX1276 / SX1262 implementations.
- \ref hioc_hub "Controller Layer": setup, loop scheduling, authenticated exchanges, pairing, passive status handling, and queued operations.
- \ref hioc_entities "ESPHome Integration Layer": the runtime entities plus the Python schema/codegen modules that expose the component to ESPHome.

## Request Flow

1. A YAML declaration is validated by the Python code-generation modules in \ref hioc_codegen "Python Code Generation".
2. ESPHome codegen wires those declarations to runtime C++ objects such as `IOHomeControlComponent`, `IOHomeCover`, `IOHomeLight`, `IOHomeSwitch`, and `IOHomeDiscoverButton`.
3. Runtime entities call into the hub through the high-level operation methods documented in \ref hioc_hub "Controller Layer".
4. The hub builds protocol frames using helpers from \ref hioc_protocol "Protocol Layer" and sends them through a concrete radio backend from \ref hioc_radio "Radio Driver Layer".
5. Replies, passive updates, and authenticated inbound messages are parsed back through the same protocol layer and merged into the shared device registry before the entity callbacks publish state to Home Assistant.

## Main Source Anchors

- Hub entry point: `IOHomeControlComponent` in [hub_core.h](../components/home_io_control/hub_core.h)
- Exchange state model: [hub_exchange.h](../components/home_io_control/hub_exchange.h)
- Pairing state model: [hub_pairing.h](../components/home_io_control/hub_pairing.h)
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
# ADR 0084: Entities reach the hub only through ports

<!-- doxygen-label: adr0084 -->

**Status:** Accepted · **Recorded:** 2026-11

Amends [ADR 0004](0004-hub-collaborator-decomposition.md) (how the entity classes reach the hub).

## Context

The ESPHome entity classes (covers, lights, switches, locks, fans, climate, water heaters and their
companions, and the hub-level buttons and switches) held a pointer to the whole hub class and called
about fifty of its methods. Every entity file therefore included the hub's header and was on the
include-graph allowlist, a change to the hub's surface recompiled every entity, and platform tests
had to subclass the hub to stand in for it. The calls fall into three groups by caller: device-bound
entities and their companions, the heating entities, and the hub-level entities.

## Decision

1. **Three ports** in `hub_ports.h`, abstract interfaces described in contract terms:
   `HubDevicePort` for device-bound entities and companions (register a device, read its record, follow
   its updates, queue commands and status reads, set poll intervals, optimistic state),
   `HubHeatingPort` for the climate and water heater entities and their controls (register reads and
   writes, executing a heating write plan, the water heater's settings), and `HubControlPort` for the
   hub-level entities (pairing, scanning, the arming switches, one-way sending and reporting, device
   event and pairing result text, the LR1121 update).
2. **Entities hold port pointers, never the hub class.** No entity platform file (`platform_*`)
   includes the hub's header. The tuning number and select entities are outside this rule: they take
   the hub by constructor injection and keep its header out of their own with a forward declaration.
3. **The hub implements all three by inheritance**; the heating port forwards to `HeaterController`
   (ADR 0083). The generated code keeps calling `set_parent(hub)`; the entity takes from that call each
   port the hub provides, so users' configurations and the code generator are unchanged.
4. **Platform tests use port doubles**: a device-port double that owns a real device registry, and
   doubles of the heating and control ports where a test needs them.

## Consequences

- An entity can be built and tested without a hub object.
- A hub method an entity needs is a port method; anything else on the hub is out of the entities'
  reach.
- Each port call is a virtual call; the cost is a few pointers in the hub and an indirect call per
  entity action.
- Methods that the hub keeps virtual only so that a test can replace them, and the include-graph
  allowlist entries for entity files, are removed in a follow-up once the platform tests use the
  doubles.

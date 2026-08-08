# Architecture Decision Records

Each file here records one architectural decision: the problem it solved, the
options weighed, what was chosen, and what that choice costs. They explain
*why* the code looks the way it does — [`docs/architecture_overview.md`](../architecture_overview.md)
describes *what* it looks like today.

## Index

Numbers are stable identifiers, not a reading order. Grouped by theme:

### Structure

| # | Decision | In short |
|---|---|---|
| [0001](0001-layered-protocol-radio-hub-architecture.md) | Layered protocol / radio / hub architecture | Three layers, one composition root; the protocol never names a chip |
| [0004](0004-hub-collaborator-decomposition.md) | Hub split into collaborators | Seven single-purpose objects instead of one large controller class |
| [0005](0005-pure-decision-logic-separated-from-io.md) | Pure decisions separated from I/O | Frame classification is side-effect-free and host-testable |

### Radio

| # | Decision | In short |
|---|---|---|
| [0002](0002-direct-spi-radio-drivers.md) | Direct SPI radio drivers | Own the chip drivers rather than reuse ESPHome's generic radio components |
| [0003](0003-shared-software-phy-base-for-sx1262-lr1121.md) | Shared software PHY + IRQ base | SX1262 and LR1121 share one framing codec and one RX/TX state machine |
| [0013](0013-blocking-exchange-on-the-esphome-loop.md) | Blocking radio work on the loop | No FreeRTOS tasks; the operation queue *is* the concurrency model |
| [0020](0020-flash-lr1121-transceiver-firmware-not-the-bootloader.md) | Flash LR1121 firmware, not the bootloader | *Superseded by 0021.* Stage the riskier operation for later; keep every failed flash recoverable now |
| [0021](0021-flash-the-lr1121-bootloader-behind-an-arming-switch.md) | Flash the bootloader, behind an arming switch | Reach the CVE-fixing `0x0104`; gate the one unrecoverable write behind visible armed state and a build-time recovery image |

### Behavior

| # | Decision | In short |
|---|---|---|
| [0006](0006-management-actions-as-native-api-actions.md) | Admin ops as native API actions | Rarely-used operations get no permanent entity |
| [0016](0016-sender-events-are-opt-in-by-allowlist.md) | Sender events are opt-in | The radio has no ownership marker, so nothing fires until named |
| [0017](0017-device-poll-hint-shortens-but-never-stretches.md) | Poll hint shortens, never stretches | The configured interval is a ceiling, not a target |

### Interfaces and naming

| # | Decision | In short |
|---|---|---|
| [0008](0008-io-device-id-config-key-naming.md) | `io_device_id`, not `device_id` | ESPHome reserves `device_id`; protocol keys take an `io_` prefix |
| [0009](0009-companion-entity-ids-declared-at-schema-time.md) | Companion IDs declared at validation | Late-created IDs are silently dropped at runtime |
| [0015](0015-table-driven-tuning-registry-with-a-sync-gate.md) | Table-driven tuning registry | Python and C++ stay hand-written, a build gate catches drift |
| [0018](0018-yaml-is-the-source-of-truth-hub-persists-nothing.md) | YAML is the only source of truth | The hub persists nothing; pairing ends in paste-and-reflash |
| [0019](0019-declare-what-the-protocol-cannot-report.md) | Declare, don't guess | Undetectable capabilities are YAML options, not inferences |

### Security

| # | Decision | In short |
|---|---|---|
| [0007](0007-self-contained-aes-implementation.md) | Self-contained AES-128 | Avoid an mbedTLS linkage dependency for one fixed primitive |
| [0011](0011-key-material-redaction-in-logs.md) | Key material masked in all logs | Masking is unconditional, not tied to the frame-logging flag |
| [0012](0012-key-extraction-responder-for-owned-devices.md) | Key-extraction responder | Emulate a device so a user's own hub hands over its credentials |
| [0022](0022-unauthenticated-status-frames-are-never-applied.md) | Unauthenticated status frames are never applied | Drop the merge rather than pay a challenge or an attacker-triggerable poll |

### Testing

| # | Decision | In short |
|---|---|---|
| [0010](0010-golden-frame-corpus-as-regression-source-of-truth.md) | Real captured frames as test truth | Committed YAML captures, generated fixtures, no hand-written byte guesses |
| [0014](0014-host-tests-against-stubbed-esphome-headers.md) | Host tests on stubbed ESPHome headers | Plain `g++`, no ESPHome or hardware — with a stated fidelity cost |

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

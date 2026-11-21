# ADR 0124: `enrollment_with_mac` is removed

<!-- doxygen-label: adr0124 -->

**Status:** Accepted, supersedes [ADR 0113](0113-enrollment-with-mac-is-rejected-on-the-sx1276.md), amends [ADR 0032](0032-oneway-velux-enrollment-gesture.md) · **Recorded:** 2026-11

## Context

`enrollment_with_mac: true` made the hub's add-controller frame (`0x30`) carry a six-byte MAC trailer after its
declared length. ADR 0113 rejected it on the SX1276, whose packet engine cannot send the trailer. Since then:

- no on-air capture carries the trailer: a real Somfy Smoove's PROG press sends the 29-byte frame without it
  (`tests/corpus/captures/enrollment/somfy_smoove_enrollment_prog_press_on_air.yaml`);
- a bench run on a Somfy Izymo dimmer did not take the frame with the trailer from an SX1262 board;
- the only capture of the hub sending it was taken from the hub's own transmit log, not from the air.

## Decision

- The option, its YAML key, the builder's trailer parameter and the SX1276 rule are removed. A configuration that
  still sets `enrollment_with_mac` fails validation with a message that says the option was removed.
- The corpus capture taken from the hub's transmit log is deleted.

## Consequences

- Every enrollment sends the 29-byte add frame that real remotes send.
- The published reference vector with a trailer stays in the corpus as reference material. The frame parser still
  accepts the trailer shape (`IoFrame::has_mac`) so the vector parses, its MAC verifies under the recovered key and a
  controller that does send it can be adopted; the hub never builds it.

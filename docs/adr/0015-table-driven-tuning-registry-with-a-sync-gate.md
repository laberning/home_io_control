# ADR 0015: Table-driven tuning registry, guarded by a cross-language sync gate

**Status:** Accepted · **Recorded:** 2026-08

## Context

Radio and pairing parameters are tunable from YAML and, optionally, live from
Home Assistant `number`/`select` entities. Each therefore exists on both sides
of the codegen boundary: in the Python that validates YAML and builds the
entities, and in the C++ that holds the runtime value and pushes it to the
driver.

Two failure modes follow. Within C++, dispatching by name through hand-written
`if`/`else` chains means several parallel functions to keep aligned. Across the
language boundary, a parameter added on one side and forgotten on the other
produces no error — just an option that silently does nothing, which reading
either file alone will not reveal.

## Options considered

1. **Generate one side from the other** so it cannot drift. Eliminates the bug
   class outright, but adds a generated file to the build and to review, for a
   table that changes rarely.
2. **Keep both hand-written, and fail the build when they disagree.** Drift
   becomes a loud, immediate failure, with no generated artifact.

## Decision

Option 2, plus a table instead of dispatch chains.

Each parameter is one row in a C++ table pairing its wire name with getter and
setter function pointers over the runtime tuning struct, plus a flag marking
whether a change must be re-applied to the radio immediately. The hub's
update/lookup methods become table lookups.

A checker extracts the parameter names from both sides — Python via `ast`,
never by importing ESPHome; the C++ tables by regex — and fails with a diff if
the two sets differ. It runs in the lint target and has its own CI job.

Adding a parameter is exactly two edits: one table row, one Python entry.
Forgetting either fails the build.

## Consequences

- The silent-drift failure mode is gone: a half-added parameter cannot reach a
  release, and the error names the missing side.
- No generated file, so both sides stay directly readable — which matters
  because the C++ side carries the per-parameter documentation of what each
  value does to the radio.
- The gate checks **names, not semantics**. A parameter present on both sides
  with mismatched ranges, units, or defaults still passes. A known limit, not
  an oversight: names are what drift silently, while a wrong range surfaces the
  first time someone moves the slider.
- The checker parses rather than imports, so it runs anywhere Python does — no
  ESPHome, matching ADR 0014.

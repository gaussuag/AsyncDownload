# Phase 1: Telemetry Skeleton - Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in CONTEXT.md — this log preserves the alternatives considered.

**Date:** 2026-03-24
**Phase:** 1-Telemetry Skeleton
**Areas discussed:** File organization, Public API surface, Payload design

---

## File organization

| Option | Description | Selected |
|--------|-------------|----------|
| Include in existing types.hpp | Add to types.hpp for simplicity | |
| New telemetry directory in include/asyncdownload/telemetry/ | Follow existing pattern, clean separation | ✓ |

**Notes:** Auto mode — selected standard approach following existing project patterns.

## Public API surface

| Option | Description | Selected |
|--------|-------------|----------|
| Expose all components directly | Event, Sink, Collector all public | |
| Facade-only public (Session) | Only TelemetrySession public, internals hidden | ✓ |

**Notes:** Auto mode — selected facade pattern per architecture doc §5.4.

## Payload design

| Option | Description | Selected |
|--------|-------------|----------|
| std::variant for payloads | Type-safe, but uses dynamic allocation | |
| Fixed-size tagged union | Memory-bounded, cache-friendly | ✓ |

**Notes:** Auto mode — selected fixed-size tagged union per architecture doc constraints (memory-bounded, no dynamic allocation).

---

## Agent Discretion

- Exact file names within src/telemetry/ directory
- Internal class visibility (public vs private)
- Whether to use pimpl idiom for collector implementation
- CMake target structure

## Deferred Ideas

None — Phase 1 scope is clear from architecture document.


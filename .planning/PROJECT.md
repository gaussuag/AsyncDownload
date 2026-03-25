# AsyncDownload Telemetry Refactoring

## What This Is

Refactoring the AsyncDownload C++ library's performance metrics system into a decoupled Telemetry module. Current implementation couples download logic with telemetry state and calculations. Target state: clean separation where download code emits semantic events, Telemetry module handles all metrics aggregation and export.

## Core Value

Maintain 100% backward compatibility for `benchmark.py` and `profiler.py` while achieving clean architectural separation between download functionality and telemetry concerns.

## Requirements

### Validated

- ✓ Async HTTP download with libcurl — existing
- ✓ Session-based download management — existing
- ✓ Progress tracking via shared state — existing (to be replaced)
- ✓ Performance summary export via PerformanceSummary — existing (to be replaced)
- ✓ benchmark.py smoke test — existing
- ✓ profiler.py smoke test — existing
- ✓ TelemetryEvent enum and fixed-size payload skeleton — validated in Phase 1
- ✓ TelemetrySink queue wrapper over moodycamel::BlockingConcurrentQueue — validated in Phase 1
- ✓ TelemetryCollector lifecycle and consumer loop skeleton — validated in Phase 1
- ✓ TelemetrySession facade with steady_clock event emission — validated in Phase 1
- ✓ Telemetry skeleton unit coverage — validated in Phase 1
- ✓ TelemetryCollector metric aggregation logic (TTFB, speeds, peaks, counts, packet stats) — validated in Phase 2
- ✓ `ProgressSnapshot` watermark generation in `TelemetryCollector::current_snapshot()` — validated in Phase 2
- ✓ Telemetry collector regression coverage — validated in Phase 2
- ✓ `download_engine` and `persistence_thread` now emit telemetry events through `SessionState::telemetry_session_` — validated in Phase 3
- ✓ Telemetry emission regression coverage and runtime-metric immutability checks — validated in Phase 3
- ✓ `SessionState` now retains only coordination state while telemetry timing and summary fields live in `TelemetrySession` — validated in Phase 4
- ✓ Progress callbacks now merge telemetry-owned snapshots with coordination/range state — validated in Phase 4
- ✓ CLI summary and progress regression coverage passes with telemetry-owned snapshot/final-summary paths — validated in Phase 4
- ✓ Deprecated acceptance and diagnostic export paths removed — validated in Phase 5
- ✓ `benchmark.py` compatibility smoke passes against the Release CLI contract — validated in Phase 5
- ✓ `profiler.py` compatibility smoke passes against the Release CLI contract — validated in Phase 5

### Active

None.

## Current State

**v1.0 MVP shipped 2026-03-25.** All 5 phases complete. 24/24 requirements satisfied.

The telemetry refactoring is complete:
- Telemetry module provides event-driven telemetry with clean separation
- Download engine and persistence thread emit events via TelemetrySession
- SessionState retains only coordination state
- Progress and summary sourced from TelemetryCollector
- benchmark.py and profiler.py remain compatible

**Next milestone:** v1.1 (not yet planned)

### Out of Scope

- Automatic retry or connection recovery strategies — not changing download semantics
- Runtime telemetry query API for external consumers — internal boundary only
- Generic logging bus or message system — purpose-built for download telemetry
- Zero-intrusion instrumentation — thin event emission calls allowed
- Changes to benchmark.py or profiler.py internal logic — interfaces must remain compatible

## Context

AsyncDownload is a C++20 CMake project using vcpkg with libcurl. The existing implementation stores performance metrics (bytes downloaded, speeds, pause counts, memory samples) directly in SessionState and download_engine. This create:
- Large files with mixed responsibilities
- Difficulty adding new metrics without modifying core download code
- Risk of telemetry changes break download functionality

The existing benchmark.py and profiler.py scripts consume PerformanceSummary output. These must remain functional after refactoring.

## Constraints

- **C++20**: Must use modern C++ features
- **No RTTI/exceptions**: Project convention, no exceptions allowed
- **Backward compatibility**: benchmark.py and profiler.py must produce identical output
- **Thread safety**: Multi-producer queue, single consumer model
- **Memory bounded**: Event struct sizes must be controlled

## Key Decisions

| Decision | Rationale | Outcome |
|----------|-----------|---------|
| Use moodycamel::BlockingConcurrentQueue | Already in libs/, thread-safe, blocking pop | Validated in Phase 1 via TelemetrySink wrapper and queue tests |
| steady_clock timestamps | Monotonic, not affected by system clock adjustments | Validated in Phase 1 event helpers and session emission tests |
| 4-component architecture (Event/Sink/Collector/Session) | Clear separation of concerns, testable individually | Phase 1 established all four components as compilable skeletons |
| Collector owns metric aggregation | Keeps timing/packet/pause/resource calculations out of download and persistence code | Validated in Phase 2 by collector implementation and focused regression tests |
| Snapshot watermark is part of the public snapshot model | Snapshot consumers need a monotonic read marker without draining the queue | Validated in Phase 2 by `ProgressSnapshot::watermark_timestamp_ns` and collector tests |
| Producers emit into one SessionState-owned TelemetrySession | Keeps download and persistence telemetry on a shared event stream with one collector | Validated in Phase 3 by event-emission migration and regression tests |
| SessionState keep only coordination fields | Download and persistence still need shared counters, but telemetry timing and summaries should not live in shared runtime state | Validated in Phase 4 by removing deprecated fields from `SessionState` |
| Progress callbacks start from telemetry snapshot data | Keeps UI-facing rates, inflight bytes, and memory sourced from one collector while still merging scheduler state locally | Validated in Phase 4 by `invoke_progress()` refactor and integration tests |
| Keep benchmark/profiler interfaces unchanged | User investment in existing tooling | Validated in Phase 5 by unchanged benchmark/profiler smoke runs against the Release CLI |
| Delete acceptance.py | Redundant with benchmark+profiler paths | Validated in Phase 5 by deleting the script and removing the remaining diagnostic sidecar path |

## Evolution

This document evolves at phase transitions and milestone boundaries.

**After each phase transition** (via `/gsd-transition`):
1. Requirements invalidated? → Move to Out of Scope with reason
2. Requirements validated? → Move to Validated with phase reference
3. New requirements emerged? → Add to Active
4. Decisions to log? → Add to Key Decisions
5. "What This Is" still accurate? → Update if drifted

**After each milestone** (via `/gsd-complete-milestone`):
1. Full review of all sections
2. Core Value check — still the right priority?
3. Audit Out of Scope — reasons still valid?
4. Update Context with current state

---
*Last updated: 2026-03-25 after v1.0 milestone completion*

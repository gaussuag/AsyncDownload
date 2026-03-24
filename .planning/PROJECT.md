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

### Active

- [ ] Implement TelemetryCollector metrics aggregation logic
- [ ] Migrate download_engine to emit TelemetryEvents instead of updating metrics directly
- [ ] Migrate SessionState to remove telemetry state pollution
- [ ] Delete acceptance.py and diagnostic export path
- [ ] Verify benchmark.py output unchanged
- [ ] Verify profiler.py output unchanged
- [ ] Unit tests for TelemetryCollector
- [ ] Integration smoke tests for CLI progress and summary

## Current State

Phase 1 is complete and verified. The repository now contains a public telemetry skeleton under `include/asyncdownload/telemetry/` and `src/telemetry/`, plus dedicated tests in `tests/telemetry_skeleton_test.cpp`.

Next focus: Phase 2 will move metric computation into `TelemetryCollector` while preserving the new event/sink/session boundaries established in Phase 1.

### Out of Scope

- Automatic retry or connection recovery strategies — not changing download semantics
- Runtime telemetry query API for external consumers — internal boundary only
- Generic logging bus or message system — purpose-built for download telemetry
- Zero-intrusion instrumentation — thin event emission calls allowed
- Changes to benchmark.py or profiler.py internal logic — interfaces must remain compatible

## Context

AsyncDownload is a C++20 CMake project using vcpkg with libcurl. The existing implementation stores performance metrics (bytes downloaded, speeds, pause counts, memory samples) directly in SessionState and download_engine. This creates:
- Large files with mixed responsibilities
- Difficulty adding new metrics without modifying core download code
- Risk of telemetry changes breaking download functionality

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
| Keep benchmark/profiler interfaces unchanged | User investment in existing tooling | — Pending |
| Delete acceptance.py | Redundant with benchmark+profiler paths | — Pending |

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
*Last updated: 2026-03-24 after Phase 1 verification*

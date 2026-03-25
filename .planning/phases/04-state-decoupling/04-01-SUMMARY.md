---
phase: 04-state-decoupling
plan: 01
subsystem: state
tags: [telemetry, session-state, cleanup]
requires:
  - phase: 03-event-emission-migration
    provides: TelemetrySession-backed event emission from download and persistence paths
provides:
  - Removal of deprecated telemetry-only SessionState fields
  - Removal of download_engine inline speed and first-byte state calculation
  - Download runtime that starts telemetry sessions without duplicate local timing state
affects: [phase-04, telemetry, state, download]
tech-stack:
  added: []
  patterns: [session-state-coordination-only, telemetry-owned-timing]
key-files:
  created: []
  modified:
    - src/core/models.hpp
    - src/download/download_engine.cpp
key-decisions:
  - "Kept only coordination counters in SessionState and deleted telemetry-only timing, watermark, and summary state."
  - "Moved first-byte deduplication responsibility fully into TelemetryCollector instead of preserving a duplicate SessionState guard."
patterns-established:
  - "download_engine no longer maintains local progress-rate state; telemetry timing now comes only from TelemetrySession events."
requirements-completed: [MIGR-03, MIGR-04]
duration: session-batch
completed: 2026-03-25
---

# Phase 4 Plan 01 Summary

**`SessionState` is reduced to coordination state and `download_engine` no longer calculates telemetry locally**

## Accomplishments
- Removed deprecated telemetry-only timing, watermark, and runtime-summary fields from `SessionState`.
- Deleted `update_progress_rates()`, the old first-byte timestamp path, and the unused buffered-byte helper from `download_engine.cpp`.
- Kept download/persistence coordination counters intact while leaving telemetry timing and aggregation exclusively to `TelemetrySession`.
- Cleaned up stale local helpers that became dead code after the decoupling.

## Files Created/Modified
- `src/core/models.hpp` - Removed deprecated telemetry-only `SessionState` fields.
- `src/download/download_engine.cpp` - Removed inline progress-rate calculation and first-byte/task-start state writes.

## Verification
- `scripts\build.bat`
- `build\tests\Debug\AsyncDownload_tests.exe --gtest_filter=DownloadIntegrationTest.ReportsDetailedProgressSnapshot`
- `build\tests\Debug\AsyncDownload_tests.exe`

## Task Commits

No git commits were created in this workspace session.

## Issues Encountered

None during the code migration itself. The remaining work after this plan was verification-level cleanup to align telemetry tests with the asynchronous collector contract.

## Next Phase Readiness

With local telemetry state removed from `SessionState`, the remaining phase work can treat `TelemetrySession::current_snapshot()` as the single live source for progress values.

---
*Phase: 04-state-decoupling*
*Completed: 2026-03-25*

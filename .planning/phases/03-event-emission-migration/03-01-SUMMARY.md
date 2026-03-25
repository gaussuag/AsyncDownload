---
phase: 03-event-emission-migration
plan: 01
subsystem: download
tags: [telemetry, download-engine, event-emission]
requires:
  - phase: 02-collector-computation
    provides: Working TelemetrySession and TelemetryCollector aggregation
provides:
  - SessionState-owned TelemetrySession
  - download_engine telemetry event emission
  - Summary generation sourced from telemetry aggregation instead of runtime metric mutation
affects: [phase-03, telemetry, download]
tech-stack:
  added: []
  patterns: [event-producer-session-state, telemetry-summary-bridge]
key-files:
  created: []
  modified:
    - src/core/models.hpp
    - src/download/download_engine.cpp
key-decisions:
  - "Added a single TelemetrySession to SessionState so download and persistence code emit into the same collector."
  - "Switched build_performance_summary() to use TelemetrySession::final_summary() as the primary metric source."
patterns-established:
  - "download_engine now emits lifecycle, pause, first-byte, download-delta, and memory-sample events instead of mutating performance_metrics."
requirements-completed: [MIGR-01]
duration: session-batch
completed: 2026-03-24
---

# Phase 3 Plan 01 Summary

**`download_engine` now behaves as a telemetry event producer**

## Accomplishments
- Added `telemetry_session_` to `SessionState`.
- Replaced direct pause, packet, memory, and first-byte metric writes in `download_engine.cpp` with `TelemetrySession::record_*()` calls.
- Wired `task_started` / `task_completed` into the download lifecycle.
- Switched final performance summary construction to telemetry-derived values, while preserving the existing disk-speed I/O timing override for CLI compatibility.

## Files Created/Modified
- `src/core/models.hpp` - Added shared `TelemetrySession` ownership to `SessionState`.
- `src/download/download_engine.cpp` - Replaced direct metric mutation with event emission and lifecycle wiring.

## Verification
- `scripts\build.bat`
- `build\tests\Debug\AsyncDownload_tests.exe --gtest_filter=DownloadIntegrationTest.LoadsDownloadOptionsFromConfigFile`
- `build\tests\Debug\AsyncDownload_tests.exe`

## Task Commits

No git commits were created in this workspace session.

## Issues Encountered

The existing summary-file integration test depended on network and disk speeds not formatting to the same textual value, so the final summary bridge had to preserve the file-writer disk-speed path instead of using a single telemetry-derived display value for both.

## Next Phase Readiness

The network producer side is fully event-driven, and the remaining migration work can focus on cleanup and state decoupling rather than collector correctness.

---
*Phase: 03-event-emission-migration*
*Completed: 2026-03-24*

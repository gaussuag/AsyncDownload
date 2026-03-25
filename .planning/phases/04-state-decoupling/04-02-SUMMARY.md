---
phase: 04-state-decoupling
plan: 02
subsystem: integration
tags: [telemetry, progress, cli, tests]
requires:
  - phase: 04-state-decoupling
    provides: SessionState no longer owns telemetry-only progress state
provides:
  - Progress snapshots sourced from TelemetrySession current_snapshot
  - Telemetry regression tests decoupled from removed RuntimePerformanceMetrics state
  - Verification that config-summary and progress snapshot flows still pass
affects: [phase-04, telemetry, cli, testing]
tech-stack:
  added: []
  patterns: [telemetry-snapshot-merge, drain-before-assert]
key-files:
  created: []
  modified:
    - src/download/download_engine.cpp
    - tests/telemetry/telemetry_event_emission_test.cpp
key-decisions:
  - "Built progress callbacks by merging telemetry snapshot fields with coordination-only SessionState and range state."
  - "Kept asynchronous snapshot semantics in tests by draining through final_summary() before asserting post-completion snapshot values."
patterns-established:
  - "CLI-facing progress data now comes from TelemetrySession::current_snapshot() plus non-telemetry coordination state."
requirements-completed: [INTG-01, INTG-02]
duration: session-batch
completed: 2026-03-25
---

# Phase 4 Plan 02 Summary

**Progress export now reads from telemetry snapshots instead of hand-built runtime metrics**

## Accomplishments
- Refactored `invoke_progress()` to start from `session.telemetry_session_.current_snapshot()` and only merge in coordination/range/handle state.
- Removed the last progress-path memory sampling and byte-rate calculation from `download_engine.cpp`.
- Updated telemetry event tests to validate summary and snapshot values without referencing the removed `SessionState::performance_metrics`.
- Verified that both config-summary output and detailed progress snapshot integration coverage still pass.

## Files Created/Modified
- `src/download/download_engine.cpp` - Switched progress snapshot assembly onto telemetry-owned snapshot data.
- `tests/telemetry/telemetry_event_emission_test.cpp` - Reworked telemetry assertions around the asynchronous collector and removed deleted state references.

## Verification
- `scripts\build.bat`
- `build\tests\Debug\AsyncDownload_tests.exe --gtest_filter=*TelemetryEventEmission*`
- `build\tests\Debug\AsyncDownload_tests.exe --gtest_filter=DownloadIntegrationTest.LoadsDownloadOptionsFromConfigFile`
- `build\tests\Debug\AsyncDownload_tests.exe --gtest_filter=DownloadIntegrationTest.ReportsDetailedProgressSnapshot`
- `build\tests\Debug\AsyncDownload_tests.exe`

## Task Commits

No git commits were created in this workspace session.

## Issues Encountered

The first version of the updated telemetry test assumed `current_snapshot()` had already observed every queued event. Because the collector is intentionally asynchronous, the test had to drain the session through `final_summary()` before asserting the final snapshot values.

## Next Phase Readiness

Phase 4 leaves the telemetry refactor at a clean boundary: runtime paths now depend on telemetry-owned snapshots and summaries, so Phase 5 can focus on compatibility smoke tests and deleting deprecated tooling.

---
*Phase: 04-state-decoupling*
*Completed: 2026-03-25*

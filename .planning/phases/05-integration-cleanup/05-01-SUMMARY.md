---
phase: 05-integration-cleanup
plan: 01
subsystem: cleanup
tags: [telemetry, cleanup, cli, docs]
requires:
  - phase: 04-state-decoupling
    provides: Telemetry-owned progress and summary paths with deprecated runtime telemetry state removed
provides:
  - Removal of the deprecated acceptance entrypoint
  - Removal of standalone diagnostic export CLI path
  - Cleanup of tests and user-facing documentation that referenced deleted tooling
affects: [phase-05, cli, docs, testing, scripts]
tech-stack:
  added: []
  patterns: [single-performance-entrypoints, no-diagnostic-sidecar]
key-files:
  created: []
  modified:
    - src/main.cpp
    - tests/download/download_resume_integration_test.cpp
    - README.md
    - docs/performance/optimization_regression_guide_zh.md
    - docs/performance/performance_playbook_zh.md
    - docs/performance/performance_thread_initialization_zh.md
    - docs/performance/performance_optimization_history_zh.md
  deleted:
    - scripts/performance/acceptance.py
key-decisions:
  - "Removed `--diagnostic-file` instead of keeping a compatibility shim because Phase 5 explicitly retires the separate diagnostic export path."
  - "Updated current guidance to point directly at benchmark, profiler, and targeted gtest commands rather than preserving a redundant wrapper script."
patterns-established:
  - "Performance validation now flows through benchmark.py, profiler.py, and focused integration tests without a separate acceptance layer."
requirements-completed: [CLEN-01, CLEN-02]
duration: session-batch
completed: 2026-03-25
---

# Phase 5 Plan 01 Summary

**Deprecated diagnostic tooling is removed and the remaining validation entrypoints are documented directly**

## Accomplishments
- Deleted `scripts/performance/acceptance.py`.
- Removed CLI parsing, usage text, and file-writing logic for `--diagnostic-file` from `src/main.cpp`.
- Deleted the obsolete integration test that asserted creation of the diagnostic JSON artifact.
- Updated the README and current performance guides so they point to benchmark/profiler smoke commands and targeted gtests instead of the removed acceptance wrapper.
- Recorded the cleanup in the performance history document so the old path remains explainable without staying live in current guidance.

## Files Created/Modified
- `src/main.cpp` - Removed the standalone diagnostic export option and resource-diagnostics writer path.
- `tests/download/download_resume_integration_test.cpp` - Removed obsolete `WritesResourceDiagnosticsFile` coverage.
- `README.md` - Dropped `--diagnostic-file` and `acceptance.py` examples, added current smoke commands.
- `docs/performance/optimization_regression_guide_zh.md` - Replaced the acceptance-layer section with direct benchmark/profiler plus gtest validation guidance.
- `docs/performance/performance_playbook_zh.md` - Updated the current validation entrypoint description.
- `docs/performance/performance_thread_initialization_zh.md` - Updated the validation guidance to reflect direct smoke/test flows.
- `docs/performance/performance_optimization_history_zh.md` - Added a dated record of the diagnostic-path removal.
- `scripts/performance/acceptance.py` - Deleted.

## Verification
- `scripts\build.bat`
- `build\tests\Debug\AsyncDownload_tests.exe`
- `build\src\Release\AsyncDownload.exe`

## Task Commits

No git commits were created in this workspace session.

## Issues Encountered

The original cleanup plan assumed `acceptance.py` was already isolated. In practice, the deleted path still had live references in the CLI, README, tests, and current performance guides, so the cleanup scope had to be expanded to remove the entire deprecated diagnostic chain instead of deleting only one file.

## Next Phase Readiness

With the deprecated acceptance and diagnostic export paths removed, compatibility verification can now focus on the surviving contract: benchmark.py and profiler.py running unchanged against the Release CLI.

---
*Phase: 05-integration-cleanup*
*Completed: 2026-03-25*

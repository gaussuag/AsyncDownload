---
phase: 02-collector-computation
plan: 03
subsystem: telemetry
tags: [telemetry, collector, peaks]
requires:
  - phase: 02-collector-computation
    provides: Running byte totals
provides:
  - Peak memory tracking
  - Peak inflight-byte tracking
  - Running snapshot updates for memory and inflight bytes
affects: [phase-02, telemetry, collector]
tech-stack:
  added: []
  patterns: [peak-aggregation]
key-files:
  created: []
  modified:
    - include/asyncdownload/telemetry/telemetry_collector.hpp
    - src/telemetry/telemetry_collector.cpp
key-decisions:
  - "Tracked current inflight bytes as downloaded minus persisted totals."
  - "Stored peak values directly in collector state and reflected running values into snapshot_."
patterns-established:
  - "MemorySample updates current memory while preserving a separate high-water mark for summary export."
requirements-completed: [COLL-03, COLL-04]
duration: session-batch
completed: 2026-03-24
---

# Phase 2 Plan 03 Summary

**Peak resource metrics are now aggregated by the collector**

## Accomplishments
- Recorded the maximum observed memory sample.
- Recorded the maximum inflight byte watermark from cumulative download/persist deltas.
- Updated running snapshot fields so callers can inspect current memory and inflight state.

## Files Created/Modified
- `include/asyncdownload/telemetry/telemetry_collector.hpp` - Added resource peak state.
- `src/telemetry/telemetry_collector.cpp` - Implemented memory and inflight tracking.

## Verification
- `scripts\build.bat`
- `build\tests\Debug\AsyncDownload_tests.exe --gtest_filter=*TelemetryCollector*:*Collector*`

## Task Commits

No git commits were created in this workspace session.

## Issues Encountered

None.

## Next Phase Readiness

The collector can now export both running resource state and final peak metrics without relying on download-engine-owned counters.

---
*Phase: 02-collector-computation*
*Completed: 2026-03-24*

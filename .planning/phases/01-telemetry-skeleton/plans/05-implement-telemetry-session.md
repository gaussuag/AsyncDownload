# Plan 5: Implement TelemetrySession Facade

## Task Description

Implement TelemetrySession as the public facade for the telemetry system. Per architecture doc §5.4 and context D-05 through D-09.

## Context

Phase 1 of 5: Telemetry Skeleton. Requirement TELE-05.
TelemetrySession is the ONLY public interface that download_engine and persistence_thread will call.
Per D-06: record_*() methods for event emission
Per D-07: Query methods: current_snapshot() and final_summary()
Per D-09: Lifecycle tied to download task

## Implementation Notes

1. Create `src/telemetry/telemetry_session.cpp` and `include/asyncdownload/telemetry/telemetry_session.hpp`
2. TelemetrySession owns TelemetrySink (for enqueuing) and TelemetryCollector (for querying)
3. record_*() methods:
   - record_task_started()
   - record_first_byte_received()
   - record_download_delta(std::uint64_t bytes)
   - record_persist_delta(std::uint64_t bytes)
   - record_pause(TelemetryPauseReason reason, bool is_queue_full)
   - record_memory_sample(std::uint64_t memory_bytes)
   - record_task_completed()
4. Each record_*() method:
   - Captures steady_clock timestamp immediately
   - Constructs TelemetryEvent with appropriate payload
   - Enqueues to sink
5. current_snapshot() — returns RuntimeSnapshot with latest aggregated state (stub in Phase 1)
6. final_summary() — returns PerformanceSummary (stub in Phase 1)

## Files to Create

- `include/asyncdownload/telemetry/telemetry_session.hpp` (new)
- `src/telemetry/telemetry_session.cpp` (new)

## Verification

1. record_*() methods can be called from multiple threads safely
2. Events appear in queue after record_*() call
3. Session can be destroyed cleanly

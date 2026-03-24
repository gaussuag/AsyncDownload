# Plan 4: Implement TelemetryCollector Stub

## Task Description

Create TelemetryCollector class that consumes events from TelemetrySink. Phase 1 creates a stub that receives events but does not yet compute metrics. Full computation comes in Phase 2.

## Context

Phase 1 of 5: Telemetry Skeleton. Requirement TELE-04.
Per architecture doc §5.3: Collector consumes events, maintains aggregation state, generates snapshots and final summary.
Phase 1 scope: stub only — establish the consumer loop and interface.

## Implementation Notes

1. Create `src/telemetry/telemetry_collector.cpp` and `include/asyncdownload/telemetry/telemetry_collector.hpp`
2. TelemetryCollector owns TelemetrySink reference (or pointer) to consume events
3. Implement `start_consuming()` — starts consumer loop in background thread
4. Implement `stop_consuming()` — signals stop and joins thread
5. Implement `wait_until_drained()` — blocks until all events processed
6. Consumer loop: while not stopped, call `wait_pop()` on sink, process event (stub — no-op or log)
7. Phase 2 will add metric computation

## Files to Create

- `include/asyncdownload/telemetry/telemetry_collector.hpp` (new)
- `src/telemetry/telemetry_collector.cpp` (new)

## Verification

1. Collector starts and runs consumer loop without crashing
2. Collector can be stopped gracefully
3. Multiple events can be consumed in order
4. Thread-safe shutdown

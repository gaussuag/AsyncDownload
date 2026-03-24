# Plan 3: Implement TelemetrySink with moodycamel Queue

## Task Description

Implement TelemetrySink class that wraps moodycamel::BlockingConcurrentQueue for multi-producer safe event enqueueing. Per architecture doc §5.2 and context D-03.

## Context

Phase 1 of 5: Telemetry Skeleton. Requirement TELE-03.
Multi-producer queue: download threads, persistence threads, and other components can all enqueue events concurrently.
Event consumption happens in a separate consumer thread (TelemetryCollector).

## Implementation Notes

1. Create `src/telemetry/telemetry_sink.cpp` and `include/asyncdownload/telemetry/telemetry_sink.hpp`
2. TelemetrySink owns a `moodycamel::BlockingConcurrentQueue<TelemetryEvent>`
3. Provide `enqueue(event)` method for multi-producer safe enqueueing
4. Provide `enqueue_from_producer(token, event)` for explicit producer tokens
5. Internal queue capacity: use default or configure (reference libs/concurrentqueue for sizing guidance)
6. Sink does NOT consume events — that happens in TelemetryCollector
7. Sink does NOT calculate metrics — it only moves events

## Files to Create

- `include/asyncdownload/telemetry/telemetry_sink.hpp` (new)
- `src/telemetry/telemetry_sink.cpp` (new)

## Verification

1. Multiple threads can enqueue events concurrently without data races
2. Queue handles events in FIFO order
3. Blocking pop can be called from consumer side (TelemetryCollector will use this)
4. No memory leaks on destruction

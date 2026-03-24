# Plan 2: Implement TelemetryEvent Enum and Payload Structures

## Task Description

Define the TelemetryEventType enum with all 7 event types and design the fixed-size tagged union TelemetryPayload structure per architecture doc §5.1.

## Context

Phase 1 of 5: Telemetry Skeleton. Requirements TELE-01 and TELE-02.
Event types per D-11: TaskStarted, FirstByteReceived, DownloadDelta, PersistDelta, QueuePaused, MemorySample, TaskCompleted.

Per D-10: Fixed-size tagged union, no dynamic allocation.
Per D-12: timestamp_ns always captured at emission site using steady_clock.

## Implementation Notes

1. Create `include/asyncdownload/telemetry/telemetry_event.hpp`
2. Define `TelemetryEventType` enum class as uint16_t
3. Define payload structures for each event type:
   - TaskStarted: no payload (or empty)
   - FirstByteReceived: no payload
   - DownloadDelta: bytes_count (uint32_t or uint64_t)
   - PersistDelta: bytes_count (uint32_t or uint64_t)
   - QueuePaused: reason (enum or uint8_t), is_queue_full (bool)
   - MemorySample: memory_bytes (uint64_t)
   - TaskCompleted: status (uint8_t)
4. Define `TelemetryPayload` as a tagged union with maximum size constraint
5. Define `TelemetryEvent` struct with type, timestamp_ns, and payload

## Files to Create

- `include/asyncdownload/telemetry/telemetry_event.hpp` (new)

## Verification

1. All 7 event types are defined in enum
2. TelemetryPayload is fixed-size (verify with static_assert)
3. No dynamic allocation (no std::vector, no std::string in payload)
4. Timestamp type is uint64_t nanoseconds from steady_clock

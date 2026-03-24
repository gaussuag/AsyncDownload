# Plan 6: Add Unit Tests for Telemetry Skeleton

## Task Description

Create unit tests to verify the Telemetry skeleton components work correctly. Tests for Phase 1 focus on structural correctness rather than metric computation (that's Phase 2).

## Context

Phase 1 of 5: Telemetry Skeleton. Tests ensure TELE-01 through TELE-06 are satisfied.
Per REQUIREMENTS.md §11: Telemetry module must have independent unit test coverage.

## Implementation Notes

1. Create `tests/telemetry_skeleton_test.cpp` following existing test patterns (see `tests/main_test.cpp`)
2. Test TelemetryEvent:
   - All 7 event types can be constructed
   - timestamp_ns is set correctly
   - Payload sizes are fixed (static_assert)
3. Test TelemetrySink:
   - Single producer can enqueue events
   - Consumer can dequeue events in FIFO order
   - Multiple producers can enqueue concurrently
4. Test TelemetrySession:
   - record_*() methods enqueue correct event types
   - Timestamp is captured at emission (not at construction)
   - Session lifecycle (create, emit events, destroy) works
5. Build tests with gtest using the same CMake structure as existing tests

## Files to Create

- `tests/telemetry_skeleton_test.cpp` (new)

## Verification

1. `build/tests/Debug/AsyncDownload_tests.exe --gtest_filter=TelemetrySkeleton*` passes
2. All tests compile without warnings
3. No memory leaks detected

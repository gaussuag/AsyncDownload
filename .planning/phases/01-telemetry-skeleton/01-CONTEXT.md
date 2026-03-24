# Phase 1: Telemetry Skeleton - Context

**Gathered:** 2026-03-24
**Status:** Ready for planning

<domain>
## Phase Boundary

Build TelemetryEvent enum, fixed-size payload structures, TelemetrySink with moodycamel queue, empty TelemetryCollector, TelemetrySession facade with record_*() methods. Phase 1 creates the skeleton only — no metric computation yet. This phase establishes the architectural boundary between download logic and telemetry.

</domain>

<decisions>
## Implementation Decisions

### File organization
- **D-01:** Public headers in `include/asyncdownload/telemetry/` (following existing `include/asyncdownload/` pattern)
- **D-02:** Implementation in `src/telemetry/` directory
- **D-03:** Main public header: `include/asyncdownload/telemetry.hpp` (aggregating all components)
- **D-04:** Separate component headers: `telemetry_event.hpp`, `telemetry_sink.hpp`, `telemetry_collector.hpp`, `telemetry_session.hpp`

### Public API surface
- **D-05:** TelemetrySession as primary facade (per architecture doc §5.4)
- **D-06:** record_*() methods: `record_task_started()`, `record_first_byte_received()`, `record_download_delta(bytes)`, `record_persist_delta(bytes)`, `record_pause(reason)`, `record_memory_sample(bytes)`, `record_task_completed()`
- **D-07:** Query methods: `current_snapshot()` returns RuntimeSnapshot, `final_summary()` returns PerformanceSummary
- **D-08:** No telemetry state in SessionState — Phase 4 will remove it
- **D-09:** TelemetrySession owned by download session, lifecycle tied to download task

### Payload design
- **D-10:** Fixed-size tagged union for TelemetryPayload (per architecture doc §5.1)
- **D-11:** Event types: TaskStarted, FirstByteReceived, DownloadDelta, PersistDelta, QueuePaused, MemorySample, TaskCompleted
- **D-12:** timestamp_ns always captured at emission site using steady_clock (enforced, not optional)
- **D-13:** No dynamic allocation in event/payload structures

### Integration points
- **D-14:** TelemetrySession created in download engine, passed to components that need to emit events
- **D-15:** Existing ProgressCallback and PerformanceSummary interfaces preserved (backward compatibility)

### Agent discretion
- Exact file names within src/telemetry/ directory
- Internal class visibility (public vs private)
- Whether to use pimpl idiom for collector implementation
- CMake target structure

</decisions>

<canonical_refs>
## Canonical References

### Architecture specification
- `.planning/performance.md` §5-7 — Core architecture, event/payload design, timestamp rules, module boundaries

### Existing types and patterns
- `include/asyncdownload/types.hpp` — PerformanceSummary struct definition
- `include/asyncdownload/performance_metrics.hpp` — RuntimeMetrics vs SummaryMetrics patterns
- `libs/concurrentqueue/include/concurrentqueue/blockingconcurrentqueue.h` — moodycamel queue API

### Requirements
- `.planning/REQUIREMENTS.md` — TELE-01 through TELE-06 (Phase 1 requirements)

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable assets
- moodycamel::BlockingConcurrentQueue — already in libs/, multi-producer safe, blocking pop
- performance::RuntimePerformanceMetrics / SummaryPerformanceMetrics — existing metric patterns to understand
- load_value() template function — for atomic-to-plain copying

### Established patterns
- Namespace: `asyncdownload::performance` for metrics
- Header organization: `include/asyncdownload/` for public, `src/` for implementation
- ProgressCallback as function<std::void()> pattern for observers

### Integration points
- SessionState (src/core/models.hpp) — will hold TelemetrySession reference in Phase 4
- download_engine — will call TelemetrySession::record_*() methods
- persistence_thread — will call TelemetrySession::record_*() methods
- DownloadResult.performance — already has PerformanceSummary field

</code_context>

<deferred>
## Deferred Ideas

None — Phase 1 scope is clear from architecture document.

</deferred>

---

*Phase: 01-telemetry-skeleton*
*Context gathered: 2026-03-24*

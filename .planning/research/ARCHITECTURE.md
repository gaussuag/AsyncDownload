# Architecture Research: C++ Telemetry Systems for Async Download

**Domain:** C++ telemetry/diagnostics for async download library
**Project:** AsyncDownload Telemetry Refactoring
**Researched:** 2026-03-24
**Confidence:** HIGH

## Executive Summary

This document defines the target architecture for decoupling AsyncDownload's telemetry system from download logic. The refactoring transforms a tightly-coupled model (where `SessionState` and `download_engine` directly maintain performance metrics) into a clean event-driven architecture with four well-defined components. The architecture follows standard C++ telemetry patterns: semantic event emission, lock-free queue transport, single-threaded aggregation, and facade-based public API.

## Target Architecture

### System Overview

```
┌─────────────────────────────────────────────────────────────────────────┐
│                         Business Logic Layer                             │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐   │
│  │ HTTP Layer  │  │Persistence  │  │Range         │  │ Orchestrator│   │
│  │ (libcurl)   │  │ Thread      │  │Scheduler     │  │              │   │
│  └──────┬──────┘  └──────┬──────┘  └──────┬──────┘  └──────┬──────┘   │
│         │                │                │                │           │
│         └────────────────┼────────────────┼────────────────┘           │
│                          │                │                                │
│                          ▼                ▼                                │
│                 ┌────────────────────────┐                                │
│                 │   TelemetrySession     │  ← Public API facade          │
│                 │   (record_* methods)    │                                │
│                 └───────────┬────────────┘                                │
│                             │                                              │
├─────────────────────────────┼─────────────────────────────────────────────┤
│                             ▼                 Telemetry Module            │
│  ┌─────────────────────────────────────────────────────────────────────┐ │
│  │                      TelemetryEvent                                  │ │
│  │   { type, timestamp_ns, payload } — immutable, producer-owned       │ │
│  └─────────────────────────────┬───────────────────────────────────────┘ │
│                                │                                          │
│                                ▼                                          │
│  ┌─────────────────────────────────────────────────────────────────────┐ │
│  │                     TelemetrySink                                    │ │
│  │   moodycamel::BlockingConcurrentQueue — multi-producer, 1 consumer │ │
│  └─────────────────────────────┬───────────────────────────────────────┘ │
│                                │                                          │
│                                ▼                                          │
│  ┌─────────────────────────────────────────────────────────────────────┐ │
│  │                   TelemetryCollector                                │ │
│  │   Event consumer · State aggregation · Snapshot publishing         │ │
│  │   - Maintains all metrics state (ONLY permitted location)           │ │
│  │   - Produces RuntimeSnapshot (running state)                       │ │
│  │   - Produces PerformanceSummary (final result)                      │ │
│  └───────────────────────────────────────────────────────────────────────┘ │
└────────────────────────────────────────────────────────────────────────────┘
```

### Component Boundaries

| Component | Responsibility | Owner | Boundaries |
|-----------|---------------|-------|------------|
| **TelemetryEvent** | Immutable fact record with timestamp | Producer (business code) | Contains type enum + steady_clock timestamp + fixed-size payload. No allocation in hot path. |
| **TelemetrySink** | Thread-safe event queue | Telemetry module | Single instance per session. Encapsulates moodycamel queue. Business code never accesses queue directly. |
| **TelemetryCollector** | Event consumption + metric computation | Telemetry module | **Only location** where metrics state and calculation logic live. Produces snapshots and summaries. Single-threaded consumption. |
| **TelemetrySession** | Public API facade for business code | Telemetry module | Thin wrapper. Constructs events with timestamps, pushes to sink. Contains zero metrics state. |

### What Business Code Is Allowed to Do

- Emit semantic events at well-defined boundaries (download start, bytes received, bytes persisted, pause triggered, memory sampled, task completed)
- Read running `RuntimeSnapshot` for progress display
- **NOT** maintain any metrics variables, calculation state, or summary computation

### What Business Code Is Forbidden from Doing

- Defining `max_memory_bytes`, `packets_enqueued_total`, `time_to_first_byte_ms` or any metric field
- Saving performance sampling timestamps (`task_started_at`, `first_network_byte_at`)
- Incrementing pause/packet/inflight counters directly
- Calculating averages, peaks, or final summary values

## Data Flow

### Event Flow (Primary Path)

```
[Business Code]
    │
    │ record_task_started() / record_download_delta(bytes) / etc.
    ▼
[TelemetrySession]           ← Thin facade, zero state
    │
    │ Emplaces TelemetryEvent {type, timestamp_ns, payload}
    ▼
[TelemetrySink]              ← moodycamel::BlockingConcurrentQueue
    │ (multi-producer enqueue)
    │
    │ blocking_pop() — single collector thread
    ▼
[TelemetryCollector]        ← ALL metrics state lives here
    │
    ├─→ Updates internal aggregation state
    ├─→ Publishes RuntimeSnapshot (periodic)
    └─→ Accumulates toward PerformanceSummary
```

### Snapshot Flow (Query Path)

```
[ProgressCallback / External Caller]
    │
    │ current_snapshot()
    ▼
[TelemetrySession]
    │
    │ Delegates to collector's latest snapshot
    ▼
[TelemetryCollector]
    │
    │ Returns immutable RuntimeSnapshot with watermark_timestamp_ns
    ▼
[ProgressSnapshot]           ← Includes network_bytes_per_second,
    │                         disk_bytes_per_second, inflight_bytes, etc.
    ▼
[CLI / Python Scripts]      ← benchmark.py, profiler.py consume
                                PerformanceSummary (final, not snapshot)
```

### Final Summary Flow

```
[DownloadTask Complete]
    │
    │ drain_and_finalize()
    ▼
[TelemetryCollector]
    │
    ├─ Consumes all remaining events
    ├─ Computes final metrics (avg_network_speed, ttfb, peaks)
    └─ Returns stable PerformanceSummary
    ▼
[DownloadResult.performance]
    │
    ▼
[benchmark.py / profiler.py]  ← Must produce identical output
```

## Component Design

### TelemetryEvent

```cpp
enum class TelemetryEventType : uint16_t {
    TaskStarted,
    FirstByteReceived,
    DownloadDelta,    // payload: bytes_received
    PersistDelta,     // payload: bytes_written
    QueuePaused,      // payload: pause_reason
    MemorySample,     // payload: memory_bytes
    TaskCompleted,     // payload: final_status
};

struct TelemetryPayload {
    // Tagged union — only one field active based on event type
    std::uint64_t bytes{0};
    std::uint64_t timestamp_ns{0};
    std::uint32_t pause_reason{0};  // 0=queue_full, 1=memory, 2=gap
};

struct TelemetryEvent {
    TelemetryEventType type;
    std::uint64_t timestamp_ns;     // steady_clock, set by producer
    TelemetryPayload payload;
};
```

**Design rules:**
- Fixed size (no heap allocation in event creation)
- Timestamp set by producer at emit time, never rewritten
- Payload is a tagged union to avoid dynamic allocation

### TelemetrySink

```cpp
class TelemetrySink {
public:
    using Queue = moodycamel::BlockingConcurrentQueue<TelemetryEvent>;
    
    void emit(TelemetryEvent event) noexcept;  // multi-producer safe
    
    // Called by collector only
    TelemetryEvent wait_pop() noexcept;         // blocking
    bool try_pop(TelemetryEvent& event) noexcept;
    
private:
    Queue queue_;
};
```

**Design rules:**
- Single instance per `TelemetrySession`
- Queue is **never** exposed to business code
- `BlockingConcurrentQueue` provides multi-producer safety with blocking pop

### TelemetryCollector

```cpp
class TelemetryCollector {
public:
    // Single-threaded consumption from sink
    void consume_until(TelemetryEventType stop_type) noexcept;
    
    // Called from collector thread only
    RuntimeSnapshot current_snapshot() const noexcept;
    PerformanceSummary final_summary() noexcept;
    
private:
    // ALL metrics state — no other component may have this
    std::uint64_t total_bytes_downloaded_{0};
    std::uint64_t total_bytes_persisted_{0};
    std::uint64_t task_started_timestamp_ns_{0};
    std::uint64_t first_byte_timestamp_ns_{0};  // 0 = not yet received
    std::uint64_t packets_enqueued_total_{0};
    std::uint64_t max_packet_size_bytes_{0};
    std::uint64_t max_inflight_bytes_{0};
    std::uint64_t max_memory_bytes_{0};
    std::size_t total_pause_count_{0};
    std::size_t queue_full_pause_count_{0};
    // ... all metrics state
    
    RuntimeSnapshot latest_snapshot_;
    std::uint64_t watermark_timestamp_ns_{0};
};
```

**Design rules:**
- **Only component** that holds metrics state
- Single-threaded: events consumed and processed sequentially
- Snapshot publishing happens on collector thread, not business threads

### TelemetrySession

```cpp
class TelemetrySession {
public:
    explicit TelemetrySession(TelemetryCollector& collector,
                               TelemetrySink& sink) noexcept;
    
    // Thin wrappers — timestamp set here, event sent to sink
    void record_task_started() noexcept;
    void record_download_delta(std::uint64_t bytes) noexcept;
    void record_persist_delta(std::uint64_t bytes) noexcept;
    void record_pause(std::uint32_t reason) noexcept;
    void record_memory_sample(std::uint64_t bytes) noexcept;
    void record_task_completed() noexcept;
    
    // Queries
    RuntimeSnapshot current_snapshot() const noexcept;
    PerformanceSummary final_summary() noexcept;

private:
    TelemetryCollector& collector_;
    TelemetrySink& sink_;
};
```

**Design rules:**
- **Zero metrics state** — pure delegation
- Sets `steady_clock` timestamp on each event
- Business code only interacts with this facade

## Build Order Implications

The migration must follow a strict dependency order:

```
┌─────────────────────────────────────────────────────────────────────┐
│ PHASE 1: Telemetry Skeleton                                          │
│                                                                      │
│  1. Define TelemetryEvent enum + struct (include/asyncdownload/)    │
│  2. Implement TelemetrySink with queue scaffolding                  │
│  3. Implement TelemetryCollector (empty aggregation for now)        │
│  4. Implement TelemetrySession facade                                │
│                                                                      │
│  Dependencies: NONE (greenfield)                                     │
│  Output: Stub that compiles but does nothing meaningful             │
└─────────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────────┐
│ PHASE 2: Collector Computation                                       │
│                                                                      │
│  5. Implement ALL metrics computation in TelemetryCollector         │
│     - Average speed calculations                                     │
│     - TTFB calculation from TaskStarted + FirstByteReceived         │
│     - Peak tracking (max_memory, max_inflight)                      │
│     - Pause counting (total_pause_count, queue_full_pause_count)    │
│     - Packet statistics                                             │
│                                                                      │
│  Dependencies: Phase 1 completed                                     │
│  Verification: Unit tests for TelemetryCollector                    │
└─────────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────────┐
│ PHASE 3: Event Emission Migration                                    │
│                                                                      │
│  6. Migrate download_engine to use TelemetrySession                 │
│     - Replace direct metrics updates with record_* calls            │
│     - Network layer → record_download_delta()                        │
│     - Persistence layer → record_persist_delta()                     │
│     - Backpressure path → record_pause()                            │
│     - Memory accounting → record_memory_sample()                    │
│                                                                      │
│  Dependencies: Phase 2 (need collector to compute), Phase 1         │
│  Note: Business code still has access to metrics via SessionState   │
│        but should NOT update them after this phase                   │
└─────────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────────┐
│ PHASE 4: State Decoupling                                            │
│                                                                      │
│  7. Remove performance_metrics from SessionState                    │
│  8. Remove direct metrics fields (downloaded_bytes, paused_count)   │
│     from SessionState                                                │
│  9. Remove speed calculation variables from SessionState             │
│  10. Verify benchmark.py / profiler.py output unchanged             │
│                                                                      │
│  Dependencies: Phase 3 (all emission migrated)                      │
│  Risk: HIGH — this is where backward compatibility is verified      │
└─────────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────────┐
│ PHASE 5: Cleanup                                                      │
│                                                                      │
│  11. Delete acceptance.py and diagnostic export path                 │
│  12. Verify all tests pass                                           │
│  13. Final integration smoke tests                                   │
│                                                                      │
│  Dependencies: Phase 4                                                │
└─────────────────────────────────────────────────────────────────────┘
```

## Architectural Patterns

### Pattern 1: Event-Driven Telemetry

**What:** Business logic emits immutable facts; telemetry system interprets them.

**When to use:** When download semantics must remain unchanged while adding/modifying observability.

**Trade-offs:**
- Pros: Decoupled, testable, can add new metrics without touching business code
- Cons: Latency between event and metric availability; extra allocation/queue overhead

### Pattern 2: Single-Threaded Aggregation

**What:** A dedicated thread consumes all telemetry events sequentially.

**When to use:** When metrics must be consistent and race-free without locking overhead on business threads.

**Trade-offs:**
- Pros: No locks on business threads, simple state management, deterministic ordering
- Cons: Metric lag equal to queue drain latency; single point of delay if collector falls behind

### Pattern 3: Facade API Pattern

**What:** `TelemetrySession` provides a stable interface hiding internal queue, collector, and state.

**When to use:** When you need to isolate business code from telemetry implementation details.

**Trade-offs:**
- Pros: Business code remains simple; can swap queue implementation without touching callers
- Cons: Extra indirection layer

## Anti-Patterns to Avoid

### Anti-Pattern 1: Metrics State in Business Objects

**What:** Storing `max_memory_bytes`, `pause_count`, `avg_speed` directly in `SessionState` or download components.

**Why bad:** Telemetry changes require modifying core download logic; risk of breaking download if telemetry bugs are introduced.

**Instead:** Emit events; keep all state in `TelemetryCollector`.

### Anti-Pattern 2: Business Thread Metric Computation

**What:** Computing speeds, updating counters, or calculating summaries in network callbacks or persistence threads.

**Why bad:** Cross-thread state access requires locking; metric computation adds latency to hot paths.

**Instead:** Emit events; compute in collector thread.

### Anti-Pattern 3: Dynamic Allocation in Event Path

**What:** Using `std::vector` or `std::string` in event payload.

**Why bad:** Allocation in hot path (network receive callback) causes performance spikes and fragmentation.

**Instead:** Fixed-size tagged union payload.

## Scaling Considerations

| Scale | Architecture Adjustments |
|-------|-------------------------|
| Single file, short download | Single collector thread sufficient; no batching needed |
| Large file, many ranges | Queue may need tuning; collector throughput unlikely to be bottleneck |
| 100+ concurrent sessions | Each session gets own sink+collector; no shared state |

**First bottleneck at scale:** Queue capacity overflow if collector stalls. Mitigation: backpressure already exists in download flow.

**Second bottleneck:** Snapshot publication frequency. Mitigation: collector publishes on interval or event count threshold, not every event.

## Integration Points

### With libcurl (HTTP Layer)

- **Integration:** HTTP receive callbacks emit `DownloadDelta` events
- **Boundary:** `record_download_delta(bytes)` called per receive buffer
- **Note:** No direct queue access; goes through `TelemetrySession`

### With Persistence Thread

- **Integration:** Persist completion callbacks emit `PersistDelta` events
- **Boundary:** `record_persist_delta(bytes)` called after write confirmation

### With Range Scheduler

- **Integration:** Gap pause and memory pause paths emit `QueuePaused` events
- **Boundary:** `record_pause(reason)` with reason enum (queue_full=0, memory=1, gap=2)

### With Python Scripts (benchmark.py, profiler.py)

- **Integration:** `DownloadResult.performance` field is `PerformanceSummary`
- **Boundary:** `PerformanceSummary` must have identical schema to current output
- **Note:** These scripts consume **final summary only**, not running snapshots

## Sources

- Target architecture specification: `.planning/performance.md`
- Current metrics implementation: `include/asyncdownload/performance_metrics.hpp`
- Current session state: `src/core/models.hpp` (SessionState struct)
- C++ event-driven telemetry patterns: industry standard (observer pattern variant)
- moodycamel::BlockingConcurrentQueue: concurrent queue for multi-producer single-consumer

---

*Architecture research for: AsyncDownload C++ Telemetry Refactoring*
*Based on: `.planning/performance.md` (target architecture specification)*

# Feature Research

**Domain:** C++ Telemetry/Diagnostics System for Async Download Library
**Researched:** 2026-03-24
**Confidence:** MEDIUM

## Feature Landscape

### Table Stakes (Users Expect These)

Features users assume exist. Missing these = product feels incomplete.

| Feature | Why Expected | Complexity | Notes |
|---------|--------------|------------|-------|
| **Event emission on semantic boundaries** | Users expect instrumentation that doesn't interfere with download logic | MEDIUM | Must be thin, non-blocking calls; existing download code must emit events at TaskStarted, DownloadDelta, PersistDelta, QueuePaused, MemorySample, TaskCompleted |
| **Thread-safe event queue** | Multi-threaded download (network + persistence threads) requires safe multi-producer queue | LOW | moodycamel::BlockingConcurrentQueue already in libs/; use as-is |
| **Metrics aggregation from events** | Users expect correct calculation of speeds, peaks, and counts from events | HIGH | Collector must correctly compute avg_network_speed, avg_disk_speed, TTFB, pause counts, packet stats, memory peaks |
| **Runtime progress snapshot** | CLI and callbacks need current download state (bytes, speeds, progress %) | MEDIUM | Snapshot must be eventually consistent, not require locking on producers |
| **Final PerformanceSummary export** | benchmark.py and profiler.py consume this for regression testing | LOW | Fields must be byte-for-byte compatible with existing output |
| **Monotonic timestamps** | Time-based metrics must not break on system clock adjustment | LOW | Use steady_clock; already specified in architecture |

### Differentiators (Competitive Advantage)

Features that set the product apart. Not required, but valuable.

| Feature | Value Proposition | Complexity | Notes |
|---------|-------------------|------------|-------|
| **Decoupled architecture** | Clean separation means telemetry changes don't risk breaking download core | MEDIUM | 4-component architecture (Event/Sink/Collector/Session) enables independent testing and evolution |
| **External query API boundary** | Future consumers (not just CLI) can query runtime state without coupling | MEDIUM | Internal boundary is TelemetrySession::current_snapshot(); external API can be layered on top |
| **Pluggable sink backends** | Users could route telemetry to multiple destinations (file, network, custom) | MEDIUM | Sink interface allows swapping moodycamel queue for other implementations |
| **Latency breakdown (TTFB)** | Time-to-first-byte is critical for diagnosing slow connections | LOW | Already in spec; must be calculated from TaskStarted→FirstByteReceived events |
| **Memory pressure signals** | Knowing when backpressure causes pauses helps tune queue_capacity_packets and backpressure thresholds | LOW | QueuePaused events with reason tracking already in spec |
| **Packet size distribution** | avg/max packet sizes indicate whether block_size and scheduler_window_bytes are appropriate | LOW | PacketsEnqueued event tracks payload size; collector computes avg/max |

### Anti-Features (Commonly Requested, Often Problematic)

Features that seem good but create problems.

| Feature | Why Requested | Why Problematic | Alternative |
|---------|---------------|-----------------|-------------|
| **Real-time streaming of every event to external consumers** | "We want full fidelity data" | Would require synchronous I/O on hot path, blocking producers; queue would need backpressure management | Collector batches snapshots; external consumers poll current_snapshot() at acceptable frequency |
| **Dynamic event types (user-defined payloads)** | "Flexibility to add any metric" | Breaks compile-time payload size guarantees; memory becomes unbounded | Enum-fixed event types with fixed-size payloads; add new event types when new metric categories emerge |
| **Generic logging bus** | "Why not make it a general-purpose message system" | Scope creep; download semantics get lost in generic abstraction; maintenance burden increases | Purpose-built for download telemetry; event types map directly to download semantics |
| **Zero-intrusion instrumentation** | "Shouldn't have to modify download code at all" | Requires binary instrumentation or AOP; fragile, toolchain-dependent, hard to debug | Thin event emission calls at semantic boundaries are acceptable |
| **Automatic metric derivation (AI-suggested metrics)** | "Let the system discover what to track" | Over-engineering; download metrics are well-understood; adds complexity without value | Fixed metric set derived from known download performance concerns |

## Feature Dependencies

```
TelemetrySession (public API)
    └──requires──> TelemetrySink (event queue)
                       └──requires──> TelemetryCollector (aggregation logic)
                                          └──requires──> TelemetryEvent (event definitions)

download_engine emits events
    └──requires──> TelemetrySession.record_*() methods

benchmark.py / profiler.py consume PerformanceSummary
    └──requires──> TelemetryCollector.final_summary()
                       └──requires──> All event types processed

Runtime progress (CLI/callbacks)
    └──requires──> TelemetryCollector.current_snapshot()
```

### Dependency Notes

- **TelemetrySession requires TelemetrySink:** Session must enqueue events; sink must exist before session
- **Collector requires all event types:** Each event type contributes to specific metrics; removing an event breaks that metric
- **PerformanceSummary requires collector drain:** Final summary only available after TaskCompleted drains the queue

## MVP Definition

### Launch With (v1)

Minimum viable product — what's needed to validate the concept.

- [ ] **TelemetryEvent enum and payload structure** — Foundation of the entire system; no events = no telemetry
- [ ] **TelemetrySink with moodycamel::BlockingConcurrentQueue** — Thread-safe event transport; enables multi-producer safe emission
- [ ] **TelemetryCollector consuming events and computing metrics** — Core aggregation logic; must produce byte-for-byte identical PerformanceSummary
- [ ] **TelemetrySession as public API facade** — Clean boundary; download code only calls session.record_*()
- [ ] **Migration: download_engine emits events instead of updating metrics directly** — Decouples telemetry from download logic
- [ ] **Migration: SessionState stripped of telemetry state** — Validates clean separation; telemetry state should live only in collector
- [ ] **Verification: benchmark.py output unchanged** — Non-negotiable backward compatibility

### Add After Validation (v1.x)

Features to add once core is working.

- [ ] **Pluggable sink interface** — Enable alternative transport (e.g., async file writer) without changing collector
- [ ] **profiler.py compatibility verification** — Second compatibility gate; profiler has different output format
- [ ] **Memory pressure attribution** — Break down pause reasons beyond queue_full (e.g., gap_paused, memory_paused)

### Future Consideration (v2+)

Features to defer until product-market fit is established.

- [ ] **External runtime query API** — Would require API design and potential ABI stability commitments
- [ ] **Multi-session aggregation** — Aggregate telemetry across multiple download sessions for overview dashboards
- [ ] **Adaptive sampling** — Dynamically adjust sampling frequency based on download speed to reduce overhead

## Feature Prioritization Matrix

| Feature | User Value | Implementation Cost | Priority |
|---------|------------|---------------------|----------|
| Event emission at semantic boundaries | HIGH | MEDIUM | P1 |
| Thread-safe event queue (moodycamel) | HIGH | LOW | P1 |
| Metrics aggregation (collector) | HIGH | HIGH | P1 |
| PerformanceSummary export (benchmark-compatible) | HIGH | MEDIUM | P1 |
| Runtime progress snapshot | HIGH | MEDIUM | P1 |
| TelemetrySession public API | HIGH | LOW | P1 |
| SessionState telemetry state removal | MEDIUM | MEDIUM | P1 |
| Pluggable sink backends | MEDIUM | MEDIUM | P2 |
| Latency breakdown (TTFB) | MEDIUM | LOW | P1 (already in spec) |
| Memory pressure signals | MEDIUM | LOW | P2 |
| Packet size distribution | LOW | LOW | P2 |
| External runtime query API | LOW | HIGH | P3 |
| Multi-session aggregation | LOW | MEDIUM | P3 |
| Adaptive sampling | LOW | HIGH | P3 |

**Priority key:**
- P1: Must have for launch
- P2: Should have, add when possible
- P3: Nice to have, future consideration

## Competitor Feature Analysis

| Feature | libcurl (easy interface) | Boost.Asio | Our Approach |
|---------|--------------------------|------------|--------------|
| Easy performance callbacks | CURLOPT_XFERINFOFUNCTION for progress | None built-in | Events at semantic boundaries (not raw callbacks) |
| Connection reuse metrics | curl_easy_getinfo() after transfer | None built-in | Collector aggregates from events |
| Time-to-first-byte | Manual timing around curl_easy_perform() | N/A | TaskStarted→FirstByteReceived event timestamps |
| Memory tracking | None built-in | None built-in | MemorySample events from download_engine |
| Queue/congestion signals | None built-in | None built-in | QueuePaused events with reason tracking |
| Summary export | Manual construction | N/A | PerformanceSummary struct, JSON-serializable |

**Note:** Most C++ HTTP libraries provide raw progress callbacks only. The value add is in the Telemetry layer that transforms raw events into meaningful metrics.

## Sources

- Current codebase: `include/asyncdownload/performance_metrics.hpp`, `include/asyncdownload/types.hpp`
- Current telemetry consumers: `scripts/performance/benchmark.py`, `scripts/performance/profiler.py`, `scripts/performance/performance_common.py`
- Architecture design: `.planning/performance.md`
- Project requirements: `.planning/PROJECT.md`

---
*Feature research for: C++ Telemetry/Diagnostics for Async Download*
*Researched: 2026-03-24*

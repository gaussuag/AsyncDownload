# Project Research Summary

**Project:** AsyncDownload Telemetry Refactoring
**Domain:** C++20 Event-Driven Telemetry/Diagnostics System
**Researched:** 2026-03-24
**Confidence:** HIGH

## Executive Summary

This project refactors AsyncDownload's telemetry system from tight coupling with download logic into a clean event-driven architecture. The system transforms `SessionState` and `download_engine` (which directly maintain performance metrics) into four decoupled components: `TelemetryEvent` (immutable facts), `TelemetrySink` (moodycamel queue), `TelemetryCollector` (single-threaded aggregation), and `TelemetrySession` (public API facade). Experts build这类系统 using event emission at semantic boundaries, lock-free queues for transport, and dedicated aggregation threads to avoid locking overhead on business code.

The recommended approach is **custom lightweight implementation over OpenTelemetry**. OpenTelemetry C++ uses exceptions by default, requires heavy SDK setup, and is designed for distributed systems with external collectors—overkill for internal-only telemetry consumed by `benchmark.py` and `profiler.py`. The existing `libs/concurrentqueue` provides the thread-safe queue; we build minimal components on top.

**Key risk: Summary Calibre Drift.** The non-negotiable backward compatibility requirement (`benchmark.py` output must be byte-for-byte identical) means the TelemetryCollector must replicate exact calculation formulas from `build_performance_summary()`. Any "cleaner" implementation that produces mathematically equivalent but numerically different results is unacceptable. Mitigation: golden reference tests with deterministic event sequences.

## Key Findings

### Recommended Stack

Build a custom lightweight telemetry module. Do NOT use OpenTelemetry C++, Prometheus client, or spdlog—they're wrong paradigm, wrong error handling, or both. All required components already exist in the project.

**Core technologies:**
- **Custom TelemetryEvent + TelemetryCollector** — zero-dependency, fully controls exception/RTTI policy, fixed-size payloads (no allocation in hot path)
- **moodycamel::BlockingConcurrentQueue** — already in `libs/concurrentqueue/`, multi-producer safe, blocking pop, no allocation on enqueue
- **std::chrono::steady_clock** — monotonic timestamps required for TTFB calculation; system_clock adjustments break time-based metrics
- **C++20 std::atomic / std::memory_order** — lock-free metrics access, no mutex contention on producer threads
- **nlohmann/json** — only at final export boundary for benchmark.py/profiler.py compatibility; NOT in hot path

### Expected Features

**Must have (table stakes):**
- Event emission at semantic boundaries (TaskStarted, DownloadDelta, PersistDelta, QueuePaused, MemorySample, TaskCompleted) — thin non-blocking calls
- Thread-safe event queue (moodycamel) — multi-producer safe between network and persistence threads
- Metrics aggregation from events — correct computation of speeds, TTFB, peaks, counts
- Runtime progress snapshot for CLI/callbacks — eventually consistent, no locking on producers
- PerformanceSummary export — byte-for-byte compatible with existing benchmark.py output

**Should have (competitive differentiators):**
- Decoupled architecture — telemetry changes don't risk breaking download core
- Pluggable sink backends — route telemetry to multiple destinations
- Latency breakdown (TTFB) — critical for diagnosing slow connections
- Memory pressure signals — helps tune queue_capacity_packets thresholds

**Defer (v2+):**
- External runtime query API — requires API design and ABI stability commitments
- Multi-session aggregation — aggregate across multiple download sessions
- Adaptive sampling — dynamically adjust sampling frequency

### Architecture Approach

Four-component event-driven architecture with strict boundaries. Business code emits immutable TelemetryEvents through TelemetrySession facade; events travel via moodycamel queue to TelemetryCollector (single-threaded consumer); TelemetryCollector maintains ALL metrics state and produces RuntimeSnapshot (for progress) and PerformanceSummary (for final output). This pattern (observer variant) is standard for C++ telemetry—decoupled, testable, can add metrics without touching business code.

**Major components:**
1. **TelemetryEvent** — immutable fact record with steady_clock timestamp and fixed-size tagged union payload; no allocation in hot path
2. **TelemetrySink** — encapsulates moodycamel::BlockingConcurrentQueue; single instance per session; business code never accesses queue directly
3. **TelemetryCollector** — ONLY permitted location for metrics state; single-threaded consumption; produces snapshots and summary
4. **TelemetrySession** — thin public API facade; zero metrics state; sets timestamps and pushes to sink

### Critical Pitfalls

1. **Telemetry State Re-Infiltration** — After migration, developers add telemetry state back to SessionState for "convenience." Prevention: static assertion `sizeof(SessionState::performance_metrics) == 0` after migration; linter rules.
2. **Summary Calibre Drift** — benchmark.py output changes because collector implements equivalent logic differently. Prevention: document exact formulas before migration; golden reference tests with deterministic event sequences.
3. **Partial Event Emission Migration** — Some pause paths emit events while others still mutate SessionState directly. Prevention: audit ALL performance_metrics mutation sites before starting; migrate one metric family at a time.
4. **Timestamp Source Mixing** — Using system_clock breaks monotonicity; TTFB calculations produce negative or absurd values. Prevention: TelemetryEvent contains only `uint64_t timestamp_ns` derived from steady_clock; validate ordering in collector.
5. **Queue Memory Explosion** — Events back up if collector stalls; unbounded memory growth. Prevention: O(1) amortized processing per event; bounded queue with drop policy; MemorySample is droppable, TaskStarted/Completed are not.

## Implications for Roadmap

Based on research, the 5-phase structure from ARCHITECTURE.md is correct and should be preserved:

### Phase 1: Telemetry Skeleton
**Rationale:** Foundation must be built before any aggregation logic. Timestamp discipline (steady_clock only) must be enforced from day one.
**Delivers:** TelemetryEvent enum + struct, TelemetrySink with queue scaffolding, TelemetryCollector (empty), TelemetrySession facade
**Avoids:** Pitfall 4 (Timestamp Mixing) — enforce steady_clock in TelemetryEvent design
**Research Flags:** Standard pattern (Facade + Queue + Consumer) — skip deeper research

### Phase 2: Collector Computation
**Rationale:** All metrics computation lives here. Must use sub-aggregator composition to avoid god object. Capacity/backpressure design must be in place before Phase 3 stress.
**Delivers:** Full metrics computation (avg speeds, TTFB, peaks, pause counts, packet stats), sub-aggregators (PauseAgg, SpeedCalc, TTFBTracker, PacketAgg), capacity/backpressure design
**Uses:** steady_clock, atomics, moodycamel queue
**Avoids:** Pitfalls 5 (Queue Explosion), 6 (Collector God Object), 7 (Snapshot/Summary Divergence) — design for composability and shared aggregation path
**Research Flags:** Collector sub-aggregator interface design may need validation

### Phase 3: Event Emission Migration
**Rationale:** download_engine must emit events instead of updating metrics directly. Requires complete audit of all mutation sites before starting.
**Delivers:** download_engine emits record_*() calls; SessionState still has performance_metrics (not yet removed); unit tests for each metric family
**Avoids:** Pitfalls 1 (Re-Infiltration), 2 (Summary Calibre Drift), 3 (Partial Emission) — must have golden reference tests before declaring complete
**Research Flags:** Need complete audit of download_engine.cpp and persistence_thread.cpp mutation sites — verify all sites identified

### Phase 4: State Decoupling
**Rationale:** SessionState telemetry fields removed here. This is where backward compatibility is verified.
**Delivers:** SessionState stripped of performance_metrics; benchmark.py output unchanged; profiler.py output unchanged
**Avoids:** Pitfall 1 (Re-Infiltration) — static assertion verifies SessionState::performance_metrics is empty
**Research Flags:** Drain synchronization design — verify final_summary() blocks until queue empty

### Phase 5: Cleanup
**Rationale:** Final integration verification. Delete deprecated paths.
**Delivers:** acceptance.py removed, diagnostic export path removed, all tests pass, smoke tests pass
**Research Flags:** Standard pattern — skip deeper research

### Phase Ordering Rationale

- **Dependency order:** Each phase depends on the previous—skeleton before computation, computation before emission, emission before decoupling
- **Risk mitigation:** Phase 3 has highest risk (calibre drift); golden reference tests must exist before starting emission migration
- **Pitfall prevention:** Phase 1 enforces timestamp discipline; Phase 2 designs capacity; Phase 3 audits mutation sites; Phase 4 enforces boundaries

### Research Flags

**Phases needing deeper research during planning:**
- **Phase 2:** Sub-aggregator interface design — composition pattern needs validation with team
- **Phase 4:** Drain synchronization protocol — blocking drain vs timeout drain needs decision

**Phases with standard patterns (skip research-phase):**
- **Phase 1:** Facade pattern + queue scaffolding — well-documented, use existing patterns
- **Phase 3:** Event emission migration — standard find-and-replace pattern with audit
- **Phase 5:** Cleanup and smoke tests — routine

## Confidence Assessment

| Area | Confidence | Notes |
|------|------------|-------|
| Stack | HIGH | Verified against official OpenTelemetry docs; moodycamel already in project; steady_clock requirement from architecture doc |
| Features | MEDIUM | Based on current codebase analysis and competitor comparison; some features (external query API) not fully validated with users |
| Architecture | HIGH | Derived from .planning/performance.md which is a detailed design doc; standard C++ event-driven patterns |
| Pitfalls | HIGH | Based on existing codebase analysis (download_engine.cpp lines 138-1033); all 8 pitfalls have specific prevention strategies |

**Overall confidence:** HIGH

### Gaps to Address

- **Gap:** Golden reference test data — need deterministic event sequence that produces known benchmark.py output. Must create this before Phase 3 begins.
- **Gap:** Drain protocol decision — blocking drain vs drain_timeout_ms parameter. Needs architecture decision before Phase 4.
- **Gap:** SessionState::performance_metrics current size/type — verify exact struct to ensure static assertion will work after removal.

## Sources

### Primary (HIGH confidence)
- OpenTelemetry C++ SDK documentation — verified exception dependency, OTLP complexity
- OpenTelemetry C++ API — verified current version 1.11.0
- .planning/performance.md — target architecture specification with steady_clock, queue, component specs
- .planning/PROJECT.md — backward compatibility requirement, no exceptions policy

### Secondary (MEDIUM confidence)
- Current codebase: download_engine.cpp, persistence_thread.cpp, models.hpp — mutation site analysis
- benchmark.py, profiler.py, performance_common.py — output format verification
- Industry C++ event-driven telemetry patterns — standard patterns, not product-specific

### Tertiary (LOW confidence)
- Community consensus on OpenTelemetry vs custom — inferential, no direct citation

---
*Research completed: 2026-03-24*
*Ready for roadmap: yes*

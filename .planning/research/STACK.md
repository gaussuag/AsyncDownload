# Stack Research: C++ Telemetry Module for AsyncDownload

**Domain:** C++20 Event-Driven Telemetry/Diagnostics System
**Project:** AsyncDownload Telemetry Refactoring
**Researched:** 2026-03-24
**Confidence:** HIGH

## Recommended Stack

### Core Technologies

| Technology | Version | Purpose | Why Recommended |
|------------|---------|---------|----------------|
| **Custom Telemetry Module** | N/A (build from scratch) | Event emission, aggregation, and export | Lightweight, zero dependencies beyond moodycamel, fully controls exception/RTTI policy |
| **moodycamel::BlockingConcurrentQueue** | 1.3.0+ (in libs/) | Thread-safe event queue | Already in `libs/concurrentqueue/`, multi-producer safe, blocking pop, no allocation on enqueue |
| **std::chrono::steady_clock** | C++20 built-in | Monotonic timestamps | System clock adjustments can't corrupt TTFB calculations; required by architecture |
| **C++20 std::atomic / std::memory_order** | C++20 built-in | Lock-free metrics access | No mutex contention on producer threads; cache-friendly atomic operations |

### Supporting Libraries

| Library | Version | Purpose | When to Use |
|---------|---------|---------|-------------|
| **nlohmann/json** | 3.10.5+ (in vcpkg.json) | PerformanceSummary serialization | Only at final export boundary for benchmark.py/profiler.py compatibility; not in hot path |
| **GoogleTest** | 1.12.1+ (in libs/) | Unit testing telemetry collector | When writing TelemetryCollector aggregation tests |
| **BS_thread_pool** | (in libs/thread-pool/) | Background collector thread | Only if collector needs dedicated thread; may use existing pool |

### NOT Using: OpenTelemetry C++

| Library | Why Not | Use Instead |
|---------|---------|-------------|
| **opentelemetry-cpp** | Exception dependency by default; heavy SDK; over-engineered for internal-only boundary; vendor lock-in to OTel schema | Custom lightweight TelemetryEvent + TelemetryCollector per ARCHITECTURE.md |
| **prometheus-client-cpp** | Pull-model (Prometheus scrapes); different paradigm than our push-to-summary workflow | nlohmann/json for final export only |
| **spdlog** | Logging library, not telemetry; wrong abstraction level | N/A — structured events, not logs |
| **Boost.Telemetry / Beast** | Heavy dependencies; exception-based error handling | Custom implementation with error_code |

## Rationale

### Why Custom Lightweight Module (Not OpenTelemetry)

**Project constraints driving this decision:**

1. **No exceptions allowed** — OpenTelemetry C++ SDK uses exceptions for error handling by default. Our project prohibits exceptions (AGENTS.md: "No TODOs or FIXME markers, never throw exceptions"). OTel's no-exception path exists but is not well-documented and may have gaps.

2. **Memory-bounded event structs** — OTel's span/event model uses `std::map` and string handling for attributes. Our architecture requires fixed-size tagged union payloads (see ARCHITECTURE.md) to avoid allocation in the hot path.

3. **Internal boundary only** — OTel is designed for distributed systems where services export to external collectors. Our `benchmark.py`/`profiler.py` scripts consume `PerformanceSummary` directly. OTel's OTLP exporter would add unnecessary complexity for this use case.

4. **Specific output format requirement** — `PerformanceSummary` must produce byte-for-byte identical output to existing `build_performance_summary()`. OTel's metric export would require mapping to OTel schema, adding translation layer, and risking semantic drift.

**OpenTelemetry C++ is the right choice when:**
- Exporting to external observability platforms (Jaeger, Zipkin, Datadog)
- Distributed tracing across service boundaries
- Standard vendor-neutral telemetry protocol is required
- Auto-instrumentation for HTTP databases is needed

**For this project, it's overkill.**

### Why moodycamel::BlockingConcurrentQueue

Already in `libs/concurrentqueue/include/concurrentqueue/blockingconcurrentqueue.h`. Properties that match our requirements:

- **Multi-producer safe**: Network thread + persistence thread can enqueue concurrently without locking
- **Blocking pop**: Collector thread blocks when queue is empty — no busy-wait polling
- **No allocation on enqueue**: Uses ring buffer with fixed-capacity slots; events must be copied (trivial for fixed-size struct)
- **Bounded capacity**: Can set max capacity; blocks producers on full queue (backpressure)

### Why steady_clock for Timestamps

`std::chrono::steady_clock` is monotonic — it never goes backwards when system clock adjusts. Our TTFB calculation (`first_byte_timestamp_ns - task_started_timestamp_ns`) requires monotonic ordering. system_clock can jump, causing negative or absurdly large durations.

## Stack Patterns by Variant

**If the project had external telemetry consumers (OTLP → Jaeger/Prometheus):**
- Use opentelemetry-cpp with OTLP exporter
- Map TelemetryEvent → OTel Span
- Collector computes metrics; OTel exports

**If the project needed runtime query API for external processes:**
- Add HTTP server (librestbed or civetweb) exposing /metrics endpoint
- Collector serves Prometheus-compatible format
- But this is out of scope per PROJECT.md

**If the project required distributed tracing across service boundaries:**
- Use opentelemetry-cpp with W3C TraceContext propagation
- Each download session becomes a trace with spans per range
- But libcurl handles HTTP only; no service mesh

## Version Compatibility

| Package | Compatible With | Notes |
|---------|-----------------|-------|
| moodycamel::BlockingConcurrentQueue | C++11+, all compilers | Header-only, no version concerns |
| nlohmann/json | C++11+, GCC 5+, Clang 3.9+, MSVC 2017+ | Header-only; vcpkg has 3.10.5 |
| GoogleTest | C++11+, GCC 4.8.5+, Clang 3.8+, MSVC 2015+ | Pre-built in libs/ |
| BS_thread_pool | C++11+ | Header-only; in libs/thread-pool/ |

**No known compatibility conflicts** — all libraries are header-only or pre-built in the project.

## Installation

No additional installation needed. All required libraries are already in the project:

```bash
# Already present:
libs/concurrentqueue/include/concurrentqueue/blockingconcurrentqueue.h
libs/thread-pool/include/thread-pool/BS_thread_pool.hpp
libs/googletest/ (for testing)

# Already in vcpkg.json:
vcpkg install nlohmann-json  # For PerformanceSummary JSON export
```

## Architecture Component Mapping

Per ARCHITECTURE.md, the stack maps to four components:

```
┌─────────────────────────────────────────────┐
│  TelemetryEvent (include/telemetry/)        │
│  - Fixed-size struct, no allocation          │
│  - steady_clock timestamp                   │
│  - Tagged union payload                     │
├─────────────────────────────────────────────┤
│  TelemetrySink (moodycamel queue)           │
│  - libs/concurrentqueue/ BlockingConcurrent │
│  - Multi-producer, single-consumer          │
├─────────────────────────────────────────────┤
│  TelemetryCollector (src/telemetry/)        │
│  - All metrics state (atomics only)         │
│  - Sub-aggregators: PauseAgg, SpeedCalc... │
├─────────────────────────────────────────────┤
│  TelemetrySession (public API facade)       │
│  - Zero state; pure delegation              │
│  - record_*() methods for business code     │
└─────────────────────────────────────────────┘
```

## Sources

- **OpenTelemetry C++ SDK**: https://opentelemetry.io/docs/languages/cpp/ — Verified: exception-based error handling, OTLP exporters, complex setup (HIGH confidence)
- **OpenTelemetry C++ API**: https://opentelemetry.io/docs/languages/cpp/api/ — Verified: 1.11.0 is current version (HIGH confidence)
- **moodycamel::BlockingConcurrentQueue**: Already in `libs/concurrentqueue/include/concurrentqueue/blockingconcurrentqueue.h` — No external verification needed (LOW confidence for version, HIGH for correctness)
- **Project architecture**: `.planning/research/ARCHITECTURE.md` — Internal design document with steady_clock, queue, and component specifications (HIGH confidence)
- **Project constraints**: `.planning/PROJECT.md` — No exceptions, no RTTI, backward compatibility with benchmark.py/profiler.py (HIGH confidence)

---

*Stack research for: AsyncDownload C++ Telemetry Refactoring*
*Researched: 2026-03-24*

# Roadmap: AsyncDownload Telemetry Refactoring

## Overview

Refactoring AsyncDownload's telemetry system from tight coupling with download logic into a clean event-driven architecture. Journey: Build Telemetry skeleton (event/sink/collector/session) → Implement all metric computations in collector → Migrate download_engine to emit events → Remove telemetry state from SessionState → Clean up deprecated code and verify compatibility.

## Phases

- [x] **Phase 1: Telemetry Skeleton** - Build event enum, payload structure, sink infrastructure, and session facade
- [x] **Phase 2: Collector Computation** - Implement all metric aggregations in TelemetryCollector
- [x] **Phase 3: Event Emission Migration** - Migrate download_engine and persistence_thread to emit TelemetryEvents
- [x] **Phase 4: State Decoupling** - Remove telemetry fields from SessionState, verify CLI progress and final summary
- [ ] **Phase 5: Integration & Cleanup** - Verify backward compatibility, delete acceptance.py, run smoke tests

## Phase Details

### Phase 1: Telemetry Skeleton
**Goal**: TelemetryEvent enum, fixed-size payload structures, TelemetrySink with moodycamel queue, empty TelemetryCollector, TelemetrySession facade with record_*() methods
**Depends on**: Nothing (first phase)
**Requirements**: TELE-01, TELE-02, TELE-03, TELE-04, TELE-05, TELE-06
**Success Criteria** (what must be TRUE):
  1. TelemetryEvent enum contains all event types (TaskStarted, FirstByteReceived, DownloadDelta, PersistDelta, QueuePaused, MemorySample, TaskCompleted)
  2. TelemetryPayload is fixed-size tagged union with no dynamic allocation
  3. TelemetrySink uses moodycamel::BlockingConcurrentQueue with multi-producer safe enqueue
  4. TelemetryCollector consumes events from sink without crashing
  5. TelemetrySession provides record_*() facade that pushes events to sink
  6. All timestamps use steady_clock (uint64_t nanoseconds)
**Plans**: 6 executed (`01-01-SUMMARY.md` through `01-06-SUMMARY.md`)

### Phase 2: Collector Computation
**Goal**: Full metrics computation in TelemetryCollector (TTFB, speeds, peaks, counts, packet stats)
**Depends on**: Phase 1
**Requirements**: COLL-01, COLL-02, COLL-03, COLL-04, COLL-05, COLL-06, COLL-07
**Success Criteria** (what must be TRUE):
  1. time_to_first_byte_ms correctly calculated from TaskStarted to FirstByteReceived timestamps
  2. avg_network_speed and avg_disk_speed computed from DownloadDelta and PersistDelta events
  3. max_memory_bytes tracked from MemorySample events
  4. max_inflight_bytes tracked across DownloadDelta events
  5. total_pause_count and queue_full_pause_count derived from QueuePaused events
  6. packets_enqueued_total, avg_packet_size_bytes, max_packet_size_bytes computed from DownloadDelta
  7. current_snapshot() returns RuntimeSnapshot with watermark_timestamp_ns
**Plans**: 6 executed (`02-01-SUMMARY.md` through `02-06-SUMMARY.md`)
- `02-01-PLAN.md` — TTFB and packet statistics
- `02-02-PLAN.md` — EMA speed calculation
- `02-03-PLAN.md` — Peak tracking (memory, inflight)
- `02-04-PLAN.md` — Pause count tracking
- `02-05-PLAN.md` — Snapshot and final summary
- `02-06-PLAN.md` — Unit tests

### Phase 3: Event Emission Migration
**Goal**: download_engine and persistence_thread emit TelemetryEvents instead of updating metrics directly
**Depends on**: Phase 2
**Requirements**: MIGR-01, MIGR-02
**Success Criteria** (what must be TRUE):
  1. download_engine emits TelemetryEvents via TelemetrySession record_*() methods
  2. persistence_thread emits PersistDelta and QueuePaused events via TelemetrySession
  3. Existing unit tests pass with event emission enabled
  4. SessionState performance_metrics remains unmodified during downloads (verified by tests)
**Plans**: 3 executed (`03-01-SUMMARY.md` through `03-03-SUMMARY.md`)
- `03-01-PLAN.md` — download_engine event emission
- `03-02-PLAN.md` — persistence_thread event emission
- `03-03-PLAN.md` — Integration and verification tests

### Phase 4: State Decoupling
**Goal**: SessionState telemetry fields removed; benchmark.py and profiler.py output unchanged
**Depends on**: Phase 3
**Requirements**: MIGR-03, MIGR-04, INTG-01, INTG-02
**Success Criteria** (what must be TRUE):
  1. SessionState no longer contains telemetry-only fields such as first-byte timing, progress-rate caches, watermark episode state, or `performance_metrics`
  2. download_engine no longer calculates speeds, TTFB, or summary fields directly
  3. CLI progress display reads from TelemetrySession::current_snapshot()
  4. PerformanceSummary final export via TelemetrySession::final_summary() produces identical output
**Plans**: 2 executed (`04-01-SUMMARY.md` through `04-02-SUMMARY.md`)
- `04-01-PLAN.md` — Remove deprecated SessionState fields and update_progress_rates
- `04-02-PLAN.md` — Refactor invoke_progress to use TelemetrySession::current_snapshot()

### Phase 5: Integration & Cleanup
**Goal**: acceptance.py deleted, benchmark.py and profiler.py smoke tests pass
**Depends on**: Phase 4
**Requirements**: INTG-03, INTG-04, CLEN-01, CLEN-02
**Success Criteria** (what must be TRUE):
  1. acceptance.py and all diagnostic export paths deleted
  2. Deprecated metric calculation code removed from SessionState and download_engine
  3. benchmark.py smoke test passes with identical output
  4. profiler.py smoke test passes with identical output
**Plans**: 2 planned
- `05-01-PLAN.md` — Cleanup (CLEN-01, CLEN-02)
- `05-02-PLAN.md` — Smoke tests (INTG-03, INTG-04)

## Progress

| Phase | Plans Complete | Status | Completed |
|-------|----------------|--------|-----------|
| 1. Telemetry Skeleton | 6/6 | Complete | 2026-03-24 |
| 2. Collector Computation | 6/6 | Complete | 2026-03-24 |
| 3. Event Emission Migration | 3/3 | Complete | 2026-03-24 |
| 4. State Decoupling | 2/2 | Complete | 2026-03-25 |
| 5. Integration & Cleanup | 0/2 | Not started | - |

---
*Roadmap created: 2026-03-24*

# Phase 2: Collector Computation - Context

**Gathered:** 2026-03-24
**Status:** Ready for planning

<domain>
## Phase Boundary

Implement all metric aggregations in TelemetryCollector: TTFB, speeds, peaks, counts, packet stats. The collector consumes TelemetryEvents from the sink and computes running and final metrics. Phase 2 establishes the aggregation logic — event emission migration comes later (Phase 3).

</domain>

<decisions>
## Implementation Decisions

### TTFB (time_to_first_byte_ms)
- **D-01:** TTFB stored only in `summary_` (PerformanceSummary), not in `snapshot_` (RuntimeSnapshot)
- **D-02:** TTFB computed when FirstByteReceived event arrives: `first_byte_ns - task_started_ns`
- **D-03:** TTFB available only via `final_summary()`, not via `current_snapshot()`

### Speed calculation (avg_network_speed, avg_disk_speed)
- **D-04:** Exponential Moving Average (EMA) algorithm for speed smoothing
- **D-05:** EMA smoothing factor: α = 0.8 (old_speed weighted 0.8, current_speed weighted 0.2)
- **D-06:** Network speed computed from DownloadDelta events, disk speed from PersistDelta events
- **D-07:** EMA state stored in member variables (not atomic — accessed only from consumer thread)
- **D-08:** `current_snapshot()` returns EMA-computed speeds, `final_summary()` returns total average

### Packet statistics
- **D-09:** `packets_enqueued_total` — incremental counter on each DownloadDelta
- **D-10:** `avg_packet_size_bytes` — incrementally computed: `total_bytes / count`
- **D-11:** `max_packet_size_bytes` — track maximum across all DownloadDelta events

### Snapshot consistency
- **D-12:** `current_snapshot()` uses copy-on-read: atomically copy `snapshot_` on each call
- **D-13:** `watermark_timestamp_ns` = `steady_clock::now()` at the moment `current_snapshot()` is called (snapshot ID semantics, time flows one direction)
- **D-14:** `current_snapshot()` returns immediately without waiting for queue to drain

### State storage strategy
- **D-15:** Full intermediate state storage: all peaks, totals, timestamps retained for debugging
- **D-16:** Member variables for: `task_started_ns`, `first_byte_received_ns`, `first_byte_received_set`, `total_download_bytes`, `total_persist_bytes`, `last_download_timestamp_ns`, `last_persist_timestamp_ns`, `last_network_speed`, `last_disk_speed`
- **D-17:** Thread safety: use `std::atomic` for counters and flags, consumer thread is single-threaded so no mutex needed for aggregation logic

### Final summary computation
- **D-18:** Pre-compute summary on TaskCompleted event, store in `summary_`
- **D-19:** `final_summary()` recalculates from stored state (double-check on TaskCompleted pre-computation)
- **D-20:** All metrics included in final summary: max_memory_bytes, max_inflight_bytes, total_pause_count, queue_full_pause_count, packets_enqueued_total, max_packet_size_bytes, time_to_first_byte_ms, average_network_bytes_per_second, average_disk_bytes_per_second, average_packet_size_bytes

### Error handling
- **D-21:** Duplicate TaskStarted events: ignore subsequent ones, keep first timestamp
- **D-22:** Events arriving after TaskCompleted: silently ignored
- **D-23:** Out-of-order events (e.g., DownloadDelta before TaskStarted): silently ignored

### Agent discretion
- Exact member variable naming within TelemetryCollector
- Internal helper methods for aggregation computation
- How to flush/reset state between tasks

</decisions>

<canonical_refs>
## Canonical References

### Architecture
- `.planning/performance.md` §5-7 — Core architecture, event/payload design, timestamp rules, module boundaries

### Requirements
- `.planning/REQUIREMENTS.md` — COLL-01 through COLL-07 (Phase 2 requirements)
- `.planning/ROADMAP.md` — Phase 2 success criteria

### Phase 1 context
- `.planning/phases/01-telemetry-skeleton/01-CONTEXT.md` — Established file layout, public API, payload design

### Existing types
- `include/asyncdownload/performance_metrics.hpp` — SummaryPerformanceMetrics, SummaryDirectPerformanceMetrics, DerivedPerformanceMetrics structure
- `include/asyncdownload/types.hpp` — ProgressSnapshot, PerformanceSummary types

</canonical_refs>

<code_context>
## Existing Code Insights

### TelemetryCollector skeleton
- `src/telemetry/telemetry_collector.cpp` — consume_loop() and handle_event() exist as stubs
- `handle_event()` currently only tracks last_event_type and processed_event_count
- `snapshot_` and `summary_` members already declared in header

### Aggregation patterns from performance_metrics.hpp
- `SummaryDirectPerformanceMetrics`: max_memory_bytes, max_inflight_bytes, total_pause_count, queue_full_pause_count, packets_enqueued_total, max_packet_size_bytes
- `DerivedPerformanceMetrics`: average_network_bytes_per_second, average_disk_bytes_per_second, time_to_first_byte_ms, average_packet_size_bytes
- `load_value()` template for atomic-to-plain copying

### Integration points
- TelemetryCollector owns `TelemetrySink&` reference
- Consumer loop runs on dedicated worker thread
- TelemetrySession wraps sink + collector

</code_context>

<deferred>
## Deferred Ideas

None — discussion stayed within Phase 2 scope.

</deferred>

---

*Phase: 02-collector-computation*
*Context gathered: 2026-03-24*

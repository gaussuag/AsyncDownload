# Requirements: AsyncDownload Telemetry Refactoring

**Defined:** 2026-03-24
**Core Value:** Maintain 100% backward compatibility for benchmark.py and profiler.py while achieving clean architectural separation between download functionality and telemetry concerns.

## v1 Requirements

### Telemetry Core

- [x] **TELE-01**: TelemetryEvent enum with all event types (TaskStarted, FirstByteReceived, DownloadDelta, PersistDelta, QueuePaused, MemorySample, TaskCompleted)
- [x] **TELE-02**: TelemetryPayload fixed-size tagged structure for each event type
- [x] **TELE-03**: TelemetrySink with moodycamel::BlockingConcurrentQueue, multi-producer safe
- [x] **TELE-04**: TelemetryCollector consuming from sink, maintaining aggregation state
- [x] **TELE-05**: TelemetrySession facade with record_*() methods and current_snapshot()/final_summary()
- [x] **TELE-06**: steady_clock timestamp enforcement in all event emission

### Collector Computation

- [x] **COLL-01**: time_to_first_byte_ms calculation (TaskStarted → FirstByteReceived)
- [x] **COLL-02**: avg_network_speed and avg_disk_speed from DownloadDelta/PersistDelta events
- [x] **COLL-03**: max_memory_bytes tracking from MemorySample events
- [x] **COLL-04**: max_inflight_bytes tracking
- [x] **COLL-05**: total_pause_count and queue_full_pause_count from QueuePaused events
- [x] **COLL-06**: packets_enqueued_total, avg_packet_size_bytes, max_packet_size_bytes from DownloadDelta
- [x] **COLL-07**: Running snapshot generation with watermark_timestamp_ns

### Migration

- [ ] **MIGR-01**: download_engine emits TelemetryEvents via TelemetrySession instead of updating metrics directly
- [ ] **MIGR-02**: persistence_thread emits PersistDelta and QueuePaused events
- [ ] **MIGR-03**: SessionState telemetry fields removed (downloaded_bytes, first_network_byte_at, performance_metrics, etc.)
- [ ] **MIGR-04**: download_engine no longer calculates speeds, TTFB, or summary fields directly

### Integration

- [ ] **INTG-01**: CLI progress display reads from TelemetrySession::current_snapshot()
- [ ] **INTG-02**: PerformanceSummary final export via TelemetrySession::final_summary()
- [ ] **INTG-03**: benchmark.py smoke test passes with identical output
- [ ] **INTG-04**: profiler.py smoke test passes with identical output

### Cleanup

- [ ] **CLEN-01**: Delete acceptance.py and all diagnostic export paths
- [ ] **CLEN-02**: Remove deprecated metric calculation code from SessionState and download_engine

## v2 Requirements

### Optional Enhancements

- **TELE-EX1**: Pluggable sink interface for testing
- **COLL-EX1**: Pluggable sub-aggregator for extensible metrics

## Out of Scope

| Feature | Reason |
|---------|--------|
| OpenTelemetry SDK integration | Exception-based, heavy dependencies, over-engineered for internal-only use |
| Runtime external query API | Internal boundary only, future consideration |
| Generic logging bus | Purpose-built for download telemetry, not a general-purpose logger |
| Automatic retry policy changes | Download semantics unchanged |
| benchmark.py/profiler.py internal modifications | Interfaces must remain compatible, not their internals |
| MemorySample downsampling under pressure | Loss unacceptable for accurate profiling |
| Bounded queue with drop policy | Unbounded with monitoring is simpler for v1 |

## Traceability

| Requirement | Phase | Status |
|-------------|-------|--------|
| TELE-01 | Phase 1 | Complete |
| TELE-02 | Phase 1 | Complete |
| TELE-03 | Phase 1 | Complete |
| TELE-04 | Phase 1 | Complete |
| TELE-05 | Phase 1 | Complete |
| TELE-06 | Phase 1 | Complete |
| COLL-01 | Phase 2 | Complete |
| COLL-02 | Phase 2 | Complete |
| COLL-03 | Phase 2 | Complete |
| COLL-04 | Phase 2 | Complete |
| COLL-05 | Phase 2 | Complete |
| COLL-06 | Phase 2 | Complete |
| COLL-07 | Phase 2 | Complete |
| MIGR-01 | Phase 3 | Pending |
| MIGR-02 | Phase 3 | Pending |
| MIGR-03 | Phase 4 | Pending |
| MIGR-04 | Phase 4 | Pending |
| INTG-01 | Phase 4 | Pending |
| INTG-02 | Phase 4 | Pending |
| INTG-03 | Phase 5 | Pending |
| INTG-04 | Phase 5 | Pending |
| CLEN-01 | Phase 5 | Pending |
| CLEN-02 | Phase 5 | Pending |

**Coverage:**
- v1 requirements: 24 total
- Mapped to phases: 24
- Unmapped: 0 ✓

---
*Requirements defined: 2026-03-24*
*Last updated: 2026-03-24 after Phase 2 verification*

**Roadmap:** .planning/ROADMAP.md (5 phases)
**State:** .planning/STATE.md

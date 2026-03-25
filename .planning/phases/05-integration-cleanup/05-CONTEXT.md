# Phase 5: Integration & Cleanup - Context

**Gathered:** 2026-03-25
**Status:** Ready for planning

<domain>
## Phase Boundary

Verify backward compatibility for benchmark.py and profiler.py (INTG-03, INTG-04), delete acceptance.py and diagnostic export paths (CLEN-01), confirm deprecated metric calculation code is removed from SessionState and download_engine (CLEN-02). This is the final integration and cleanup phase.

</domain>

<decisions>
## Implementation Decisions

### Deprecated code cleanup scope (CLEN-02)
- **D-01:** Only metric calculation related code is deprecated — telemetry state that has been replaced by TelemetrySession
- **D-02:** Based on Phase 4 verification, MIGR-04 is complete: download_engine no longer calculates speeds, TTFB, or summary fields directly
- **D-03:** SessionState no longer contains `performance_metrics` field (removed in Phase 4 per MIGR-03)
- **D-04:** download_engine uses `record_first_byte_received()` via TelemetrySession, not direct calculation
- **D-05:** Remaining SessionState fields (downloaded_bytes, persisted_bytes, vdl_offset, etc.) are operational state needed for download coordination — not deprecated metric code

### Smoke test verification (INTG-03, INTG-04)
- **D-06:** benchmark.py and profiler.py must produce identical output before and after refactoring
- **D-07:** "Identical" means: same field values in PerformanceSummary, same file structure, same exit codes
- **D-08:** Smoke test passes if: benchmark.py completes without error, profiler.py completes without error, output fields match expected values

### acceptance.py deletion (CLEN-01)
- **D-09:** acceptance.py at `scripts/performance/acceptance.py` is marked for deletion
- **D-10:** All diagnostic export paths referenced by acceptance.py should be removed if not used elsewhere
- **D-11:** No backup needed — code is in git history

### Agent discretion
- Exact verification test implementation details
- How to handle minor floating-point differences in output comparison
- Build artifact cleanup after smoke tests

</decisions>

<canonical_refs>
## Canonical References

### Requirements
- `.planning/REQUIREMENTS.md` — INTG-03, INTG-04, CLEN-01, CLEN-02 (Phase 5 requirements)

### Phase context
- `.planning/phases/01-telemetry-skeleton/01-CONTEXT.md` — Established architecture
- `.planning/phases/02-collector-computation/02-CONTEXT.md` — Aggregation decisions
- `.planning/phases/03-event-emission-migration/03-CONTEXT.md` — Event emission decisions
- `.planning/phases/04-state-decoupling/04-CONTEXT.md` — State removal decisions

### Smoke test scripts
- `scripts/performance/benchmark.py` — Must remain unchanged and produce identical output
- `scripts/performance/profiler.py` — Must remain unchanged and produce identical output
- `scripts/performance/acceptance.py` — To be deleted

### Existing code
- `src/core/models.hpp` — SessionState definition (check for remaining deprecated fields)
- `src/download/download_engine.cpp` — Should use TelemetrySession only, no direct metric calculation

</canonical_refs>

<code_context>
## Existing Code Insights

### SessionState current state (from models.hpp)
- `downloaded_bytes`, `persisted_bytes` — operational counters, not metric calculation
- `vdl_offset`, `queued_packets`, `queued_bytes` — coordination state
- `telemetry_session_` — the TelemetrySession instance for telemetry
- No `performance_metrics` field — removed in Phase 4

### download_engine current state
- Uses `telemetry_session_.record_first_byte_received()` for TTFB tracking
- No direct speed calculation — delegated to TelemetryCollector
- No direct summary calculation — delegated to TelemetrySession::final_summary()

### Smoke test scripts
- benchmark.py: Runs downloads with various configurations, outputs CSV and markdown report
- profiler.py: Runs downloads with Windows Performance Toolkit, outputs ETL traces and CSV
- acceptance.py: Redundant with benchmark+profiler paths, to be deleted

</code_context>

<deferred>
## Deferred Ideas

None — Phase 5 discussion stayed within scope.

</deferred>

---

*Phase: 05-integration-cleanup*
*Context gathered: 2026-03-25*

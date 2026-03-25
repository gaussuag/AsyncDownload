# Phase 5: Integration & Cleanup - Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in CONTEXT.md — this log preserves the alternatives considered.

**Date:** 2026-03-25
**Phase:** 05-integration-cleanup
**Areas discussed:** Deprecated code cleanup scope

---

## Deprecated Code Cleanup Scope

| Option | Description | Selected |
|--------|-------------|----------|
| Only metric calculation related | Delete only code replaced by TelemetrySession | ✓ |
| All telemetry related | Delete all SessionState telemetry state | |
| Conservative cleanup | Only delete clearly replaced code, keep useful helper functions | |

**User's choice:** Only metric calculation related code is deprecated
**Notes:** 
- Phase 4 already completed MIGR-04 (download_engine no longer calculates metrics directly)
- Phase 4 already completed MIGR-03 (SessionState performance_metrics field removed)
- Remaining SessionState fields (downloaded_bytes, persisted_bytes, etc.) are operational state, not deprecated

---

## Smoke Test Verification

**User's choice:** Not explicitly discussed — requirements from ROADMAP.md apply:
- benchmark.py must produce identical output
- profiler.py must produce identical output
- "Identical" means same field values, same file structure, same exit codes

---

## acceptance.py Deletion

**User's choice:** Not explicitly discussed — requirements from ROADMAP.md apply:
- acceptance.py to be deleted from `scripts/performance/acceptance.py`
- Diagnostic export paths to be removed if not used elsewhere
- No backup needed — code is in git history

---

## Agent Discretion

- Exact verification test implementation details
- How to handle minor floating-point differences in output comparison
- Build artifact cleanup after smoke tests

## Deferred Ideas

None — discussion stayed within Phase 5 scope.

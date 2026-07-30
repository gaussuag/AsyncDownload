# AsyncDownload Refactor Execution Status

## Current frontier

| Item | Value |
| --- | --- |
| Active stage | 1 — Validated Download Policy |
| Last passed stage | 0 — Characterization Baseline |
| Stage 0 base | `622339136e563752885d8b5ee6a6a0157e28266c` |
| Stage 0 test rollback point | `98f891f` |
| Current branch | `codex/refactor-00-characterization` |
| Next slice | Read Phase 1 contract, revalidate symbols, and add invalid-option tests |

## Stage ledger

| Stage | Status | Evidence | Rollback frontier |
| --- | --- | --- | --- |
| 0 — Characterization Baseline | passed | `evidence/phase_00_characterization.md` | commits after `6223391` |
| 1 — Validated Download Policy | active | pending | pending |
| 2 — Packet Flow / Backpressure | locked | pending | depends on Stage 1 |
| 3 — Range Lifecycle | locked | pending | depends on Stage 2 |
| 4 — Recovery Checkpoint | locked | pending | depends on Stage 3 |
| 5 — HTTP Transfer | locked | pending | depends on Stage 4 |
| 6 — Telemetry Session | locked | pending | depends on Stage 5 |

## Persistent environment facts

- Debug direct execution has two Windows CLI process-launch failures; one reports `error=740`.
- Release CTest passes all 44 tests at the Stage 0 frontier.
- Formal benchmark server is the existing static server at `127.0.0.1:4287` serving
  `1gb_files.zip`.
- Stage 0 benchmark artifacts are
  `build/benchmarks/20260730_200312_phase-0-pre` and
  `build/benchmarks/20260730_202548_phase-0-post`.
- `skills/asyncdownload-performance` and `skills/history-archive` are absent at current HEAD but
  available in base commit `7a96af2`; their workflows govern this execution as a fallback.


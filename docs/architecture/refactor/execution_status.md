# AsyncDownload Refactor Execution Status

## Current frontier

| Item | Value |
| --- | --- |
| Active stage | 2 — Packet Flow / Backpressure |
| Last passed stage | 1 — Validated Download Policy |
| Stage 0 base | `622339136e563752885d8b5ee6a6a0157e28266c` |
| Stage 0 test rollback point | `98f891f` |
| Current branch | `codex/refactor-01-policy` |
| Next slice | Read Phase 2 ticket and phase contract, then add its expected-red tests |

## Stage ledger

| Stage | Status | Evidence | Rollback frontier |
| --- | --- | --- | --- |
| 0 — Characterization Baseline | passed | `evidence/phase_00_characterization.md` | commits after `6223391` |
| 1 — Validated Download Policy | passed | `evidence/phase_01_validated_download_policy.md` | commits after `a2dee41` |
| 2 — Packet Flow / Backpressure | active | pending | pending |
| 3 — Range Lifecycle | locked | pending | depends on Stage 2 |
| 4 — Recovery Checkpoint | locked | pending | depends on Stage 3 |
| 5 — HTTP Transfer | locked | pending | depends on Stage 4 |
| 6 — Telemetry Session | locked | pending | depends on Stage 5 |

## Persistent environment facts

- Debug direct execution has two Windows CLI process-launch failures; one reports `error=740`.
- Release CTest passes 96 tests with 2 architecture-conditional skips at the Stage 1 frontier.
- Formal benchmark server is the existing static server at `127.0.0.1:4287` serving
  `1gb_files.zip`.
- Stage 0 benchmark artifacts are
  `build/benchmarks/20260730_200312_phase-0-pre` and
  `build/benchmarks/20260730_202548_phase-0-post`.
- Stage 1 post benchmark artifact is
  `build/benchmarks/20260730_223607_phase-1-post`.
- `skills/asyncdownload-performance` and `skills/history-archive` are absent at current HEAD but
  available in base commit `7a96af2`; their workflows govern this execution as a fallback.

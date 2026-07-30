# AsyncDownload Refactor Execution Status

## Current frontier

| Item | Value |
| --- | --- |
| Active stage | 4 — Recovery Checkpoint (unlocked) |
| Last passed stage | 3 — Range Lifecycle |
| Stage 0 base | `622339136e563752885d8b5ee6a6a0157e28266c` |
| Stage 0 test rollback point | `98f891f` |
| Current branch | `codex/refactor-03-range-lifecycle` (Stage 3 evidence frontier) |
| Next slice | Create the Stage 4 branch, read its ticket and phase contract, then run 04.1 |

## Stage ledger

| Stage | Status | Evidence | Rollback frontier |
| --- | --- | --- | --- |
| 0 — Characterization Baseline | passed | `evidence/phase_00_characterization.md` | commits after `6223391` |
| 1 — Validated Download Policy | passed | `evidence/phase_01_validated_download_policy.md` | commits after `a2dee41` |
| 2 — Packet Flow / Backpressure | passed | `evidence/phase_02_packet_flow_backpressure.md` | commits after `eca01fc` |
| 3 — Range Lifecycle | passed | `evidence/phase_03_range_lifecycle.md` | commits after `f271997` |
| 4 — Recovery Checkpoint | unlocked | pending | depends on Stage 3 |
| 5 — HTTP Transfer | locked | pending | depends on Stage 4 |
| 6 — Telemetry Session | locked | pending | depends on Stage 5 |

## Persistent environment facts

- Debug CTest passes 130 tests with 8 explicit environment/architecture skips at the Stage 2
  frontier.
- Release CTest passes 130 tests with 2 architecture-conditional skips at the Stage 2 frontier.
- Formal benchmark server is the existing static server at `127.0.0.1:4287` serving
  `1gb_files.zip`.
- Stage 0 benchmark artifacts are
  `build/benchmarks/20260730_200312_phase-0-pre` and
  `build/benchmarks/20260730_202548_phase-0-post`.
- Stage 1 post benchmark artifact is
  `build/benchmarks/20260730_223607_phase-1-post`.
- Stage 2 benchmark artifacts are
  `build/benchmarks/20260730_230342_phase-2-pre` and
  `build/benchmarks/20260731_004945_phase-2-post`.
- Stage 2 WPR attempted artifact is
  `build/profiles/20260731_005636_phase-2-profile`; WPR start was denied by the local
  system-performance tracing policy.
- Stage 3 formal benchmark artifact is
  `build/benchmarks/20260731_030514_phase-03-11-legacy-deletion`; all 140 runs passed.
- Stage 3 Debug and Release CTest pass 190 active tests. Debug has the same 8 explicit skips and
  Release has the same 2 architecture skips.
- Stage 3 WPR attempted artifact is `build/profiles/20260731_032735_phase-03-gate`; WPR start was
  denied by the same local system-performance tracing policy and error code.
- `skills/asyncdownload-performance` and `skills/history-archive` are absent at current HEAD but
  available in base commit `7a96af2`; their workflows govern this execution as a fallback.

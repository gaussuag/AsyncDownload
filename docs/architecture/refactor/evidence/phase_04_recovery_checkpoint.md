# Phase 4 Evidence: Recovery Checkpoint

## Identity

| Item | Value |
| --- | --- |
| Stage | 4 — Recovery Checkpoint |
| Base commit | `af1a61d` |
| Branch | `codex/refactor-04-recovery-checkpoint` |
| First test commit | `d3782a2` |
| Last production deletion | `e3ff8e7` |
| Performance evidence | `fa0359f` |
| Change classes | S, C, test, perf |

The whole-stage rollback anchor is `af1a61d`. Dependent slices are reverted in reverse order.

## Slice ledger

| Commit | Class | Slice and rollback unit |
| --- | --- | --- |
| `d3782a2` | test | Characterize legacy schema, identity, CRC rollback, removal, and replace behavior |
| `2def7ea` | S | Add recovery values, codec, and the owning RecoveryCheckpoint module |
| `bf48bac` | S | Migrate artifact inventory, identity, load, bitmap projection, and CRC validation |
| `2fa14e3` | C | Reject finished-coverage holes before serialized VDL |
| `6f449ba` | C | Invalidate metadata before destructive part reset |
| `fba690a` | S | Move part writes and storage ownership behind RecoveryCheckpoint |
| `2b28041` | C | Freeze checkpoint images and publish the exact committed VDL |
| `8b3ec94` | C | Fail a commit when any required CRC read is incomplete |
| `2ab7519` | C | Check temporary close and atomically replace formal metadata |
| `555fc7b` | C | Preserve forced checkpoint requests with a mandatory successor |
| `d3213eb` | S | Unify normal and resume-complete finalization and classify cleanup |
| `e2073ad` | C | Atomically replace output and ignore corrupt orphan metadata safely |
| `e3ff8e7` | S | Remove direct leaf access, legacy accessors, and the finalize adapter |
| `fa0359f` | perf | Record the formal Recovery Checkpoint performance gate |

Every row is an independent Git rollback point. Correctness commits include both the red test and
the corresponding fix, so reverse rollback does not leave the active suite red.

## Contract result

- `RecoveryCheckpoint` exclusively owns the part file, metadata store, recovery identity,
  validated bitmap, committed generation, and committed VDL.
- `open()` inventories part and metadata before parsing. Only a candidate pair is parsed; corrupt
  orphan metadata is invalidated and starts fresh without poisoning the task.
- Candidate identity preserves the legacy exact URL/path/size/block/alignment rules and the
  conditional ETag/Last-Modified rules.
- Serialized VDL is rejected when the projected finished bitmap contains a prefix hole.
- Required discard confirms metadata invalidation before the old part can be reset.
- Persistence writes only through `RecoveryCheckpoint::write()`.
- `prepare()` freezes bitmap and Range facts on the Persistence thread. The worker reads no live
  bitmap, Range Lifecycle, RangeWriteState, or Session state.
- `commit()` orders part flush, complete CRC reads, atomic metadata replacement, and exact
  committed-VDL publication.
- One commit is pending at a time. A range-complete or shutdown force that overlaps a commit
  creates one mandatory latest-image successor; multiple pending forces can merge but cannot
  disappear.
- Normal completion and resume-complete both call `RecoveryCheckpoint::finalize()`.
- Finalize requires the internal committed VDL to equal total size. Metadata cleanup reports
  removed/not-found/failed without hiding an already available output.
- Windows output promotion uses one replace operation with write-through semantics; overwrite
  failure preserves the old output, part, and metadata.
- Public request/result/CLI behavior, legacy metadata shape, pretty JSON, flush cadence, CRC
  strategy, FileWriter handle shape, and the formal 10-key Performance Summary remain compatible.

## Correctness evidence

The deterministic recovery suites cover:

- fresh, resumed, complete, identity-mismatch, sparse, and non-Range recovery;
- transient downloading reset and legacy persisted-range projection;
- missing and mismatched CRC rollback beyond VDL;
- recovery and commit CRC read failures;
- VDL-prefix holes without artifact mutation;
- metadata invalidation failure and both discard crash boundaries;
- frozen-image races before and after part flush;
- metadata crash boundaries before and after atomic replace;
- range-complete and shutdown successor generations;
- multiple pending force requests merging into one successor;
- successor submit failure retaining the last successful checkpoint;
- finalize preconditions and removed/not-found/failed cleanup classification;
- atomic output promotion crash boundaries;
- overwrite-disabled output and checkpoint preservation;
- corrupt orphan metadata versus a corrupt candidate pair.

The expected-red evidence was observed before each C slice. The old implementation either accepted
the unsafe state, lost a forced generation, removed the formal artifact before replacement, or
reported the recovery/finalize stub instead of the target result.

## Functional verification

| Command or group | Result |
| --- | --- |
| `scripts\build.bat` | pass |
| Debug main test binary | 201 pass, 2 architecture skips |
| Debug Packet Flow fault binary | 8/8 pass |
| Debug Range Lifecycle fault binary | 6/6 pass |
| Debug Recovery fault binary | 16/16 pass |
| `scripts\build.bat release` | pass |
| Release main test binary | 201 pass, 2 architecture skips |
| Release Packet Flow fault binary | 8/8 pass |
| Release Range Lifecycle fault binary | 6/6 pass |
| Release Recovery fault binary | 16/16 pass |
| Formal Release benchmark | 160/160 pass |
| Formal summary schema | 160/160 exact 10-key sets |

The main suite includes the Windows CLI integration paths. The two skips are the same
architecture-unrepresentable queue capacity and remote block-count boundaries from prior stages.
No failure or skip set expanded. CTest aggregation is not used for the final count because this
desktop environment intermittently fails to inherit the Python child-process path across multiple
CTest cases; every executable was run directly instead.

## Static deletion audit

| Audit | Result |
| --- | --- |
| `metadata_matches`, `validate_resumed_blocks`, `rebuild_bitmap_from_snapshots` | zero production matches |
| `MetadataStore` / `FileWriter` under download or persistence | zero production matches |
| Persistence live bitmap VDL calculation | zero matches |
| `build_crc_samples` / `build_metadata_state` in Persistence | zero matches |
| `legacy_file_writer` / `legacy_metadata_store` | zero matches |
| `finalize_storage_phase` | zero matches |
| Compact `dump()` | zero matches; `dump(2)` retained |
| Dedicated-handle / incremental-CRC experiments | zero matches |

`FileWriter`, `MetadataStore`, the codec, and `core::crc32` remain private leaf implementations
inside recovery/storage/metadata as permitted by the phase contract.

## Performance verification

The Stage 3 formal frontier and Stage 4 post frontier use the same host, static loopback URL,
1 GiB file, Release CLI, and 20 repeats per case. The current suite added
`throughput_candidate`, so the percentage comparison uses the seven common cases.

| Case | Stage 3 median MB/s | Stage 4 median MB/s | Change |
| --- | ---: | ---: | ---: |
| `baseline_default` | 582.37 | 696.57 | +19.61% |
| `balanced_candidate` | 495.38 | 522.87 | +5.55% |
| `deep_buffer_candidate` | 500.14 | 488.87 | -2.25% |
| `memory_guard` | 676.40 | 690.73 | +2.12% |
| `scheduler_stress` | 489.96 | 491.85 | +0.39% |
| `queue_backpressure_stress` | 543.40 | 565.37 | +4.04% |
| `gap_tolerance_probe` | 490.19 | 486.56 | -0.74% |

No common case crosses the -5% regression gate. Network and disk medians remain equal, pause
medians are unchanged per case, and memory/inflight changes remain within approximately
`-4.2%` to `+0.5%`. `memory_guard` max-memory median changes from `12,439,560` to
`12,382,637` bytes.

Artifacts:

- formal pre:
  `build/benchmarks/20260731_030514_phase-03-11-legacy-deletion`;
- formal post:
  `build/benchmarks/20260731_053020_phase-04-post`.

Sticky-force generation count is intentionally attributed to barrier-controlled fault tests,
because the formal 10-key Summary does not expose checkpoint generations or flush count.

## Reverse-order rollback

Starting from this evidence frontier, revert `fa0359f`, `e3ff8e7`, `e2073ad`, `d3213eb`,
`555fc7b`, `2ab7519`, `8b3ec94`, `2b28041`, `fba690a`, `6f449ba`, `2fa14e3`, `bf48bac`,
`2def7ea`, and `d3782a2` in that order. The unchanged Range Lifecycle frontier is `af1a61d`.

## Gate result

Stage 4 passes its recovery compatibility, ownership, load/validate, discard, frozen-image,
generation, CRC, metadata replacement, forced-successor, finalize, promotion, failure,
Debug/Release, schema, deletion, and performance gates. Stage 5 — HTTP Transfer — is unlocked.

# Phase 3 Evidence: Range Lifecycle

## Identity

| Item | Value |
| --- | --- |
| Stage | 3 — Range Lifecycle |
| Base commit | `f271997` |
| Branch | `codex/refactor-03-range-lifecycle` |
| First test commit | `497309d` |
| Last production deletion | `9cee9d9` |
| Final acceptance correction | `c6e0b8f` |
| Change classes | S, C, test, perf |

The whole-stage rollback anchor is `f271997`. Dependent slices are reverted in reverse order.

## Slice ledger

| Commit | Class | Slice and rollback unit |
| --- | --- | --- |
| `497309d` | test | Characterize geometry, scheduling, lease identity, persistence, and completion |
| `fe0f91c` | S | Add dependency-neutral range values and pure scheduler proposals |
| `f56f55e` | S | Add the reducer-owned Range Lifecycle state machine |
| `ac9896c` | S | Drive HTTP windows with immutable Range Leases |
| `b35db41` | S | Gate Register/Resize geometry on ordered Persistence acknowledgements |
| `43d7b97` | S | Move write-local state into Persistence and publish coalesced facts |
| `8d8aebc` | C | Map lifecycle, effect, command, and persistence allocation failures |
| `9652bc5` | C | Define duplicate, stale, future-generation, and overlap outcomes |
| `7cf83a5` | C | Close duplicate/reorder accounting on every path |
| `40917ec` | C | Require durable completion identity and persistence invariants |
| `75be1eb` | S | Derive progress and final completion from Lifecycle snapshots |
| `9cee9d9` | S | Delete legacy shared Range runtime state and dead fields |
| `c6e0b8f` | test/C | Complete fixed-seed properties and remove the all-finished planner fallback |

Every row is an independent Git rollback point. `c6e0b8f` contains the final acceptance tests and
the one behavior correction they exposed: a recovery bitmap with no unfinished block now produces
no Range instead of incorrectly scheduling the entire file.

## Contract result

- `RangeLifecycle` is the sole owner of Range geometry, phase, dispatch cursor, generation,
  active Lease, persisted frontier, completion identity, and finished count.
- `RangeScheduler` is a pure proposal module. It does not allocate ids or mutate shared Range
  state.
- A `RangeLease` is immutable and carries the Range id, monotonically increasing generation, and
  exact byte span used by HTTP and Packet Flow.
- Runtime steal publishes Resize and Register effects. The engine arms the new transfer only
  after ordered Persistence success acknowledgements.
- Persistence owns tail buffering, reorder nodes, persisted frontier, geometry revision, and
  completion validation in `RangeWriteState`.
- `RangeFactSlot` publishes coalesced persisted, gap, and terminal completion facts with
  release/acquire revision ordering.
- Duplicate and stale facts are idempotent; future generations, conflicting outcomes, partial
  overlaps, and completion identity mismatches become deterministic terminal errors.
- Completion requires network close, expected end, empty tail, empty reorder map, no gap, and a
  matching persisted frontier. Only matching `PersistenceCommitted` increments finished count.
- Failed, cancelled, and finished Ranges never reopen. Late persisted facts may advance the
  terminal frontier but cannot restart scheduling.
- Public API, CLI behavior, policy defaults, metadata JSON shape, Packet Flow ownership, and the
  formal 10-key Performance Summary are unchanged.

## Acceptance discoveries

The fixed-seed property suite first ran red against the post-03.11 implementation. Trial zero used
an all-finished bitmap and proved that `RangeScheduler::build_unfinished_spans()` replaced the
empty unfinished union with a full-file span. That fallback contradicted sparse recovery semantics
and was removed in `c6e0b8f`. The same suite also confirmed that replaying the immediately previous
outcome can correctly classify as duplicate rather than stale when its identity and payload are
identical; the invariant is no state mutation in either classification.

The final property and stress set covers:

- 256 fixed-seed random bitmap/size/connection trials with exact unfinished-byte union;
- 256 fixed-seed repeated-steal trials with disjointness, byte conservation, and alignment;
- 256 fixed-seed lease/persistence sequences with monotonic generations/frontiers, duplicate and
  stale insertion, no early finish, and exact-once terminal count;
- one million coalesced persisted facts with a retained terminal completion;
- generation overflow without cursor advancement;
- Register/Resize success acknowledgements before the first Data packet;
- failed, cancelled, and finished terminal phases that cannot reacquire.

## Functional verification

| Command or group | Result |
| --- | --- |
| `scripts\build.bat` | pass |
| `ctest --test-dir build -C Debug --output-on-failure` | 190/190 active tests pass, 8 explicit skips |
| `scripts\build.bat release` | pass |
| `ctest --test-dir build -C Release --output-on-failure` | 190/190 active tests pass, 2 architecture skips |
| Range Lifecycle fixed-seed properties | 768 trials pass |
| One-million-fact concurrency test | pass |
| Range Lifecycle deterministic fault binary | 6/6 pass |
| Final Release benchmark smoke | 8/8 runs pass |
| Formal summary schema | exact expected 10-key set |

Debug retains the same environment classification as earlier stages: two unrepresentable
architecture boundaries and six CLI subprocess cases that report `CreateProcess error=740`.
Release executes the six CLI paths and skips only the two architecture-impossible boundaries.
The failure set and reasons did not expand.

## Static deletion audit

| Audit | Result |
| --- | --- |
| `core::RangeContext` and its shared atomics | zero production/test matches |
| Runtime core `TailBuffer` | removed; behavior lives in `RangeWriteState` |
| `mark_range_status` / `rollback_inflight_window` | zero matches |
| Scheduler `next_range_id_` and mutable Range interface | zero matches |
| Persistence `ranges_mutex_` and raw Range registry | zero matches |
| `current_metadata_state` / `all_ranges_completed` | zero matches |
| Sampled packet and unused reorder counters | zero matches |
| `SessionState::cancel_requested` | zero matches |
| Legacy recovery DTO | retained as `RangeStateSnapshot` for Phase 4 |

## Performance verification

The same machine, static loopback URL, 1 GiB file, Release CLI, `regression_v2` suite, seven
protected cases, and 20 repeats were used at the Stage 2 and Stage 3 formal frontiers.

| Case | Stage 2 median MB/s | Stage 3 median MB/s | Change |
| --- | ---: | ---: | ---: |
| `baseline_default` | 568.89 | 582.37 | +2.37% |
| `balanced_candidate` | 505.54 | 495.38 | -2.01% |
| `deep_buffer_candidate` | 485.42 | 500.14 | +3.03% |
| `memory_guard` | 671.26 | 676.40 | +0.77% |
| `scheduler_stress` | 501.00 | 489.96 | -2.20% |
| `queue_backpressure_stress` | 540.09 | 543.40 | +0.61% |
| `gap_tolerance_probe` | 491.27 | 490.19 | -0.22% |

Both protected throughput cases remain within the 5% keeper threshold. Network and disk medians
remain identical in every case. Memory changed by +0.04%, -0.85%, -1.85%, +0.46%, -0.01%,
0.00%, and -0.15% in table order. Pause medians remain stable: `4`, `32`, `0`, `17 → 16`, `8`,
`4`, and `16`. Packet aggregation remains approximately 64.5 KiB with a 65,536-byte maximum and
about 16,640 packets per run.

Artifacts:

- formal pre:
  `build/benchmarks/20260731_004945_phase-2-post`;
- 03.6 exact pre/post canary:
  `build/benchmarks/20260731_033115_phase-03-06-allocation-pre` and
  `build/benchmarks/20260731_033223_phase-03-06-allocation-post`;
- correction canaries:
  `build/benchmarks/20260731_022159_phase-03-07-identity`,
  `build/benchmarks/20260731_023038_phase-03-08-accounting`, and
  `build/benchmarks/20260731_023549_phase-03-09-completion`;
- structural canaries:
  `build/benchmarks/20260731_024718_phase-03-10-lifecycle-snapshot`;
- formal post:
  `build/benchmarks/20260731_030514_phase-03-11-legacy-deletion`;
- final production-state smoke:
  `build/benchmarks/20260731_032802_phase-03-final-smoke`.

Single-repeat correction artifacts are attribution canaries, not statistical keeper decisions.
The 140/140 formal pre and post artifacts decide the gate.

The required WPR gate was retried at
`build/profiles/20260731_032735_phase-03-gate`. WPR stopped before the first download with the same
`0xc5585011` system-performance tracing policy denial recorded in Stage 2. No profiler number was
substituted for build, test, schema, or benchmark evidence.

## Reverse-order rollback

Starting from this evidence frontier, revert `c6e0b8f`, `9cee9d9`, `75be1eb`, `40917ec`,
`7cf83a5`, `9652bc5`, `8d8aebc`, `43d7b97`, `b35db41`, `ac9896c`, `f56f55e`, `fe0f91c`, and
`497309d` in that order. The unchanged Packet Flow frontier is `f271997`.

## Gate result

Stage 3 passes its ownership, geometry, lease, persistence-fact, completion, allocation,
identity, accounting, property, concurrency, deletion, Debug/Release, schema, and performance
gates. The known Windows profiler policy denial is unchanged and isolated. Stage 4 — Recovery
Checkpoint — is unlocked.

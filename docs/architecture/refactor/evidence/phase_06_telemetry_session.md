# Phase 6 Evidence: Telemetry Session

## Identity

| Item | Value |
| --- | --- |
| Stage | 6 — Telemetry Session |
| Base commit | `d1b58ce` |
| Branch | `codex/refactor-06-telemetry-session` |
| First test commit | `11d9f23` |
| Last correctness commit | `e30a711` |
| Change classes | S, C, test, perf |

The whole-stage rollback anchor is `d1b58ce`. Dependent slices are reverted in reverse order.

## Slice ledger

| Commit | Class | Slice and rollback unit |
| --- | --- | --- |
| `11d9f23` | test | Characterize lifecycle, timing, calculation, schema, and public contracts |
| `87729b7` | S | Add four uniquely named deterministic Session variants |
| `4a2b28b` | S | Make Session the sole aggregation and synchronization owner |
| `beee447` | S | Build progress explicitly from authoritative module snapshots |
| `0d3c0d8` | S | Align Packet Flow, HTTP, and Persistence telemetry producers |
| `1d3c5c4` | C | Saturate totals, counts, projections, and narrowing |
| `f160a7b` | test | Collapse core Collector tests into the Session surface |
| `88ff80e` | test | Prove exact concurrent aggregation and completion behavior |
| `e30a711` | test | Distinguish successful physical writes from completion failures |

Every production row is an independent Git rollback point. Correctness rows preserve their
deterministic boundary evidence.

## Contract and ownership result

- `TelemetrySession` solely owns task lifecycle, timestamps, EMA state, observed byte totals,
  memory/inflight peaks, pause and packet counters, snapshots, summaries, and one task-local
  mutex.
- `TelemetryCollector` is a source-compatible shell whose only state is one Session. Production
  callers do not depend on it and it contains no aggregation logic or synchronization.
- The declaration hub preserves the existing public include paths, method types, default
  arguments, `noexcept`, enum values, and transitive visibility.
- The four deterministic methods are uniquely named `_at` variants, so existing pointer-to-member
  expressions do not become ambiguous.
- There is no event allocation, queue, background worker, cached snapshot, or second atomic
  aggregation path.
- All normal-range EMA, task-average, TTFB, pause, packet, memory, and inflight formulas remain
  compatible. Overflow now saturates instead of wrapping, narrowing, or becoming negative.

## Progress and producer result

Production progress starts from an empty `ProgressSnapshot` and merges:

- object size, resume base, persisted bytes, and VDL from the owning task/recovery facts;
- queued packets, accounted bytes, and published data from Packet Flow;
- active and paused transfers from HTTP;
- only network and disk speeds from Telemetry.

Downloaded and persisted absolute counters use checked addition, and inflight is a non-negative
projection of the same-round values. Callbacks receive a value after all module snapshots have
been released.

The exact observation producers are:

- successful Packet Flow Data Packet publish: one download delta and one packet observation;
- Packet Flow accounted-byte sample: memory peak;
- Packet Flow queue or memory pause edge: one corresponding pause observation;
- accepted nonzero HTTP body: earliest first byte;
- HTTP gap bit `0 -> 1`: one gap pause observation;
- successful Persistence physical write and counter advance: one persist delta;
- orchestrator completion: one shared timestamp for completion and final summary.

Rejected admission, replay, control packets, headers, probes, failed writes, reorder movement,
flush, checkpoint, duplicate gap state, stale tokens, normal exact-window completion, and long-body
terminal failure do not create duplicate observations.

## Correctness evidence

The deterministic Session matrix covers reset, before-start ignore, earliest first byte, repeated
start, completed ignore, duplicate completion, incomplete and completed summaries, timestamp
clamping, out-of-order samples, zero duration, exact `0.8/0.2` EMA, task averages, memory and
inflight peaks, pause predicates, packet statistics, zero defaults, and independent tasks.

Overflow tests cover:

- download, persist, and packet totals near `UINT64_MAX`;
- packet and pause counts near `SIZE_MAX` through a private friend seam;
- portable `uint64_t` to `size_t` narrowing;
- inflight beyond `INT64_MAX`;
- peak monotonicity, repeated saturation, and non-negative finite projections.

The concurrency test starts HTTP, Packet Flow, Persistence, and query threads with latches rather
than sleeps. It records 10,000 events per producer, asserts exact totals and earliest first byte,
continues writers after completion, verifies completion immutability and independent Session state,
and passes 20 repeated executions.

## Functional verification

| Command or group | Result |
| --- | --- |
| `scripts\build.bat` | pass |
| Debug main test binary | 271 pass, 2 architecture skips |
| Debug HTTP fault binary | 18/18 pass |
| Debug Packet Flow fault binary | 8/8 pass |
| Debug Range Lifecycle fault binary | 6/6 pass |
| Debug Recovery fault binary | 16/16 pass |
| `scripts\build.bat release` | pass |
| Release main test binary | 271 pass, 2 architecture skips |
| Release HTTP fault binary | 18/18 pass |
| Release Packet Flow fault binary | 8/8 pass |
| Release Range Lifecycle fault binary | 6/6 pass |
| Release Recovery fault binary | 16/16 pass |
| Telemetry concurrency repeat | 20/20 pass |
| Progress builder and integration group | 5/5 pass |
| Producer suites | 76/76 pass |
| Saturation group, Debug and Release | 17/17 each |
| Formal pre/post benchmark | 160/160 runs pass |
| Memory confirmation benchmark | 80/80 runs pass |

The two skips are the existing architecture-unrepresentable queue-capacity and remote-block-count
boundaries. No failure or skip set expanded. Auxiliary fault binaries were run sequentially
because their fixed temporary paths collide when Debug and Release processes execute concurrently.
The main binaries were run directly because this desktop environment intermittently fails to
propagate the bundled Python path through aggregated CTest execution.

## Schema and deletion audit

The automated schema audit compared the C++ member set, CLI key set, Python source and target key
sets, raw numeric fields, and integration parser. All complete summaries have exactly:

```text
avg_network_speed
avg_disk_speed
time_to_first_byte_ms
max_memory_bytes
max_inflight_bytes
total_pause_count
queue_full_pause_count
packets_enqueued_total
avg_packet_size_bytes
max_packet_size_bytes
```

A complete parser smoke passed and a missing `avg_network_speed` smoke was rejected.

| Audit | Result |
| --- | --- |
| Production `TelemetryCollector` references | shell implementation only |
| `collector_` member | zero matches |
| Aggregation fields | Session declaration/implementation only |
| Retired Sink/Event/worker symbols | zero matches |
| Retired diagnostics | zero matches |
| Collector in download/flow/http/persistence/range/recovery | zero matches |
| Formal summaries | exact 10-key schema |

## Performance verification

The actual base `d1b58ce` was rebuilt in an isolated worktree. Both Release binaries used the same
host, static loopback 1 GiB identity object, server, four cases, configuration, output root, and 20
repeats.

| Case | Pre median MB/s | Post median MB/s | Change | TTFB | Pause |
| --- | ---: | ---: | ---: | ---: | ---: |
| `baseline_default` | 399.35 | 594.17 | +48.79% | 12.5 -> 4 ms | 81.5 -> 4 |
| `balanced_candidate` | 472.84 | 510.98 | +8.07% | 16 -> 6 ms | 959.5 -> 32 |
| `memory_guard` | 445.26 | 575.07 | +29.15% | 14 -> 4 ms | 207.5 -> 18 |
| `scheduler_stress` | 320.22 | 483.97 | +51.14% | 14 -> 5 ms | 360 -> 8 |

Network and disk values are identical for this workload. No protected case approaches the `-5%`
gate, TTFB improves in every case, and pause counts decrease rather than regress. Average packet
size remains about 64 KiB and maximum packet size remains exactly 65,536 bytes.

The host changed throughput modes across the full pre/post boundary, so the memory signal was
raised to 40 repeats as required. Consecutive actual-base/current `memory_guard` confirmation gave:

| Signal | Contemporary pre | Contemporary post | Change |
| --- | ---: | ---: | ---: |
| network/disk median | 466.97 MB/s | 579.56 MB/s | +24.11% |
| TTFB median | 13.5 ms | 4 ms | -70.37% |
| max memory | 4,526,952 bytes | 12,398,719 bytes | +173.89% |
| max inflight | 4,275,018 bytes | 12,135,545 bytes | +183.87% |
| pause median | 213 | 17 | -92.02% |
| average packet | 64,516 bytes | 64,520 bytes | +0.01% |
| maximum packet | 65,536 bytes | 65,536 bytes | unchanged |

The higher producer/persistence overlap has explicit throughput, latency, and pause benefit. The
post `memory_guard` peak remains only 9.12% of post `baseline_default` memory and 8.94% of its
inflight peak, so the intended low-memory shape remains distinct. There is no multi-case
no-benefit rise and no blocking memory or inflight condition.

Artifacts:

- formal actual base: `build/benchmarks/20260731_084241_phase-6-pre`;
- formal Stage 6 post: `build/benchmarks/20260731_084708_phase-6-post`;
- 40-run actual-base memory confirmation:
  `build/benchmarks/20260731_085231_phase-6-pre-memory-confirmation`;
- 40-run Stage 6 memory confirmation:
  `build/benchmarks/20260731_085440_phase-6-post-memory-confirmation`;
- saturation smoke: `build/benchmarks/20260731_082416_phase-6-saturation-smoke`.

WPR was attempted at `build/profiles/20260731_085656_phase-6-profile` and failed at `wpr-start`
with the known system-performance policy error `0xc5585011`. Source inspection answered the
permitted structural questions: every record/query operation takes the single Session mutex;
Collector adds only one forwarding call; there is no per-event allocation, queue, worker, or
second aggregation state; Packet Flow and Persistence retain their existing data paths.

## Reverse-order rollback

Starting from this evidence frontier, revert `e30a711`, `88ff80e`, `f160a7b`, `1d3c5c4`,
`0d3c0d8`, `beee447`, `4a2b28b`, `87729b7`, and `11d9f23` in that order. The unchanged HTTP
Transfer frontier is `d1b58ce`.

## Gate result

Stage 6 passes its public compatibility, lifecycle, timing, aggregation, progress-source,
producer, concurrency, saturation, Debug/Release, fault, integration, exact-schema, deletion,
performance, and profiler-structure gates. Stages 0–6 and the complete progressive architecture
refactor roadmap are accepted.

# Phase 1 Evidence: Validated Download Policy

## Identity

| Item | Value |
| --- | --- |
| Stage | 1 — Validated Download Policy |
| Base commit | `a2dee41` |
| Branch | `codex/refactor-01-policy` |
| First test commit | `bc03068` |
| Last implementation commit | `16f6590` |
| Change classes | S, C |

The stage rollback anchor is `a2dee41`. Each implementation slice is an independent commit after
the expected-red characterization commit.

## Slice ledger

| Commit | Slice | Class |
| --- | --- | --- |
| `bc03068` | Characterize invalid policy and public aggregate compatibility | S |
| `165f723` | Add two-stage validated/effective policy module | S |
| `aef848e` | Reject invalid options before curl or HTTP probe | C |
| `70ec1fc` | Bind remote facts without mutating raw options | S |
| `4f25529` | Migrate scheduler and checked range arithmetic | S |
| `9c42ba3` | Migrate flow control and checked backpressure projection | S |
| `f97e0c0` | Migrate persistence and unify TailBuffer capacity | S |
| `5093b28` | Migrate recovery identity matching | S |
| `e99014a` | Restart sparse state when Range is unavailable | C |
| `a23b6f6` | Make effective policy mandatory session state | S |
| `16f6590` | Centralize CLI option semantics in policy | S |

## Contract result

- Public `DownloadOptions` fields, defaults, order, aggregate initialization, and CLI field names
  remain compatible.
- `validate_download_options` is the only source for option and cross-field invariants.
- `bind_remote_facts` creates immutable scheduling, flow-control, persistence, and recovery views.
- Invalid policy returns `invalid_request` before curl initialization, probe, file creation, queue
  construction, or worker creation.
- Range mode preserves requested connections and effective request windows.
- Non-Range mode uses one connection, one full-object GET, no Range header, no work stealing, and
  no sparse resume.
- Non-Range sparse legacy state restarts cleanly; complete trusted state finalizes without a body
  GET.
- `SessionState` owns `EffectiveDownloadPolicy` and no longer stores mutable `DownloadOptions`.
- `RangeScheduler` and `PersistenceThread` no longer depend on `DownloadOptions`.
- Recovery metadata schema and formal 10-key CLI summary schema are unchanged.

## Functional verification

| Command or group | Result |
| --- | --- |
| `scripts\build.bat` | pass |
| `DownloadPolicyTest.*` | 33 pass, 2 platform-impossible cases skipped |
| `DownloadClientTest.*` | 4/4 pass |
| `RangeSchedulerTest.*` | 7/7 pass |
| `PersistenceThreadTest.*` | 7/7 pass |
| Non-Range integration group | 3/3 pass |
| Full Debug binary outside restricted environment | 90 pass, 6 skip, 2 fail |
| `scripts\build.bat release` | pass |
| `ctest --test-dir build -C Release --output-on-failure` | 96 pass, 2 skip, 0 fail |

The two Debug failures are unchanged from Phase 0:

- `DownloadIntegrationTest.ResumeAfterInterruptedCliDownload`: CLI process reports Windows
  `error=740`.
- `DownloadIntegrationTest.LoadsDownloadOptionsFromConfigFile`: CLI `start_process` returns false.

The four new CLI integration tests detect the same `error=740` and report an explicit skip in the
Debug binary. Direct invocation of the same Debug CLI verified invalid watermark order, unsafe
alignment, and positional zero override: all returned exit code 1 with `invalid request` before
network I/O. Release CTest executed and passed all four CLI tests, including both direct and
wrapped config shapes. The Debug failure set did not expand.

The two policy skips are architecture-conditional:

- queue capacity above `PTRDIFF_MAX` cannot be constructed when `size_t` and `ptrdiff_t` have equal
  width;
- remote block count above `SIZE_MAX` cannot be constructed on a 64-bit target with an
  `int64_t` remote size.

## Static audits

| Audit | Result |
| --- | --- |
| `session.options` / `session_.options` | no matches |
| `DownloadOptions` in scheduler or persistence | no matches |
| engine raw non-Range option mutation | no matches |
| TailBuffer raw `4096` in production targets | one named constant definition only |
| public headers importing internal policy | no matches |
| metadata schema additions | none |
| summary key changes | none |

## Performance verification

The Phase 1 pre artifact is the Phase 0 post run at the exact parent commit. The post run used the
same Release host, server, URL, 1 GiB object, suite, and 20 repeats per case.

| Artifact | Path |
| --- | --- |
| Pre | `build/benchmarks/20260730_202548_phase-0-post` |
| Post | `build/benchmarks/20260730_223607_phase-1-post` |

The post suite completed 160/160 runs without retry failure.

| Case | Pre net/disk MB/s | Post net/disk MB/s | Change | Memory change | Inflight change | Pause median |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `baseline_default` | 538.89 | 534.30 | -0.85% | -0.02% | -0.02% | 4 → 4 |
| `balanced_candidate` | 501.13 | 489.38 | -2.34% | -0.04% | -0.06% | 32 → 32 |
| `memory_guard` | 639.35 | 615.28 | -3.77% | +0.66% | +0.68% | 14 → 16 |
| `scheduler_stress` | 471.88 | 492.68 | +4.41% | +0.11% | +0.12% | 8 → 8 |

Default and balanced throughput remain inside the blocking -5% gate. `memory_guard` peak remains
near the current same-environment baseline, and its pause median increase is 14.29%, below the
approximately 15% investigation threshold. The historical approximately 4.2 MiB absolute-memory
discrepancy remains the Phase 0 environment question; this stage did not materially change it.
Packet total/average/max keys remain present, and max packet size remains 65536 bytes in all four
gate cases.

## Third-party audit

- The existing `BlockingConcurrentQueue(capacity, 1, 1)` constructor shape is unchanged.
- Queue capacity is validated against `PTRDIFF_MAX` before construction.
- libcurl connection limits receive a checked `long` value from policy.
- Existing packet aggregation, connection isolation, queue behavior, and metadata serialization
  are unchanged.

## Gate result

Stage 1 passes its functional, compatibility, lifecycle, static, and performance gates. Stage 2 is
unlocked.

Open environment facts:

- Debug CLI child creation still reports Windows `error=740`; Release CTest passes those paths.
- The current memory baseline remains above the historical March snapshot, with negligible
  Phase 1 same-environment delta.

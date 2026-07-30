# Phase 0 Evidence: Characterization Baseline

## Identity

| Item | Value |
| --- | --- |
| Stage | 0 — Characterization Baseline |
| Base commit | `622339136e563752885d8b5ee6a6a0157e28266c` |
| Branch | `codex/refactor-00-characterization` |
| Test commit | `98f891f` |
| Change class | S |
| Production changes | none |

The stage rollback anchor is the base commit. Revert the commits after `6223391` on this branch while
Stage 0 has no dependents. The test rollback unit begins at `98f891f`; this evidence file belongs to
the following documentation commit.

## Contract observations

- Public `DownloadOptions` defaults are executable assertions.
- Existing `DownloadRequest` aggregate initialization is executable.
- Missing URL and missing output path independently return `invalid_request` before I/O.
- CLI summary output is asserted to contain exactly the formal 10-key schema.
- Existing recovery, HTTP connection isolation, progress, persistence, and telemetry
  characterization remains active.
- Public headers, CLI arguments, recovery files, and production behavior have no diff in this stage.

## Functional verification

### Before

| Command | Result |
| --- | --- |
| `scripts\build.bat` | pass |
| Debug test binary in restricted process environment | 33/40 pass; 7 child-process tests fail to start |
| Debug test binary outside restricted process environment | 38/40 pass |
| `scripts\build.bat release` | pass |
| `ctest --test-dir build -C Release --output-on-failure` | 40/40 pass |

The two Debug failures outside the restricted process environment were:

- `DownloadIntegrationTest.ResumeAfterInterruptedCliDownload`: `failed to start CLI process, error=740`
- `DownloadIntegrationTest.LoadsDownloadOptionsFromConfigFile`: CLI `start_process` returned false

### After

| Command | Result |
| --- | --- |
| `scripts\build.bat` | pass |
| `PublicCompatibilityTest.*` | 4/4 pass |
| Debug test binary outside restricted process environment | 42/44 pass |
| `scripts\build.bat release` | pass |
| Focused Release CTest | 5/5 pass |
| `ctest --test-dir build -C Release --output-on-failure` | 44/44 pass |

The two Debug failures are the same two CLI process-launch failures with the same assertion paths.
The failure set did not expand. Release CTest can launch the Release CLI and passes both tests.

## Performance verification

Both runs used the same Release binary, host, local server, 1 GiB object, suite, case list, 20
repeats, and 500 ms inter-run delay.

| Artifact | Path |
| --- | --- |
| Pre | `build/benchmarks/20260730_200312_phase-0-pre` |
| Post | `build/benchmarks/20260730_202548_phase-0-post` |

| Case | Pre net/disk MB/s | Post net/disk MB/s | Change | Memory change | Inflight change |
| --- | ---: | ---: | ---: | ---: | ---: |
| `baseline_default` | 537.20 | 538.89 | +0.31% | -0.12% | -0.12% |
| `balanced_candidate` | 484.83 | 501.13 | +3.36% | -0.34% | -0.14% |
| `memory_guard` | 590.50 | 639.35 | +8.27% | +0.07% | -0.02% |
| `scheduler_stress` | 473.48 | 471.88 | -0.34% | -0.11% | -0.12% |

No blocking throughput regression occurred. `memory_guard` median pause count moved from 12 to 14,
a single-case increase of 16.67%. No production binary changed, other cases did not regress, and
memory/inflight stayed flat, so this remains a recorded variance rather than a structural
regression.

The current `memory_guard` absolute memory median is about 12.4 MB, above the historical
approximately 4.2 MB snapshot. The same current binary produced the pre and post values, and the
post delta is +0.07%. Subsequent stages must compare against the Phase 0 same-environment artifacts;
the historical absolute difference remains an open environment/baseline question.

## Audits

- Public contract diff: none.
- CLI contract diff: none.
- Recovery format diff: none.
- Summary schema diff: none; exact 10-key test added.
- Ownership/threading/atomic diff: none.
- Third-party contract impact: none.
- Profiler: not required because Stage 0 does not touch packet, persistence, or curl production code.

## Gate result

Stage 0 passes its gate. Stage 1 is unlocked.

Open risks:

- Debug CLI process creation continues to fail with Windows `error=740`.
- Current absolute memory levels differ from the historical March snapshot despite a performance
  neutral Stage 0 pre/post comparison.
- Repository HEAD deleted the two local skill directories that `AGENTS.md` still names; the
  workflows were recovered from base commit `7a96af2` for this execution.


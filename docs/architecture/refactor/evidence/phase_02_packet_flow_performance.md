# Phase 2 Packet Flow Performance Evidence

## Run identity

| Item | Value |
| --- | --- |
| Stage base | `eca01fc` |
| Post implementation | `9a1cef1` |
| URL | `http://127.0.0.1:4287/1gb_files.zip` |
| Object size | 1 GiB |
| Build | Release |
| Repeats | 20 per case |
| Successful runs | 140/140 |

The pre and post runs used the same host, static server, object, case definitions, and inter-run
delay.

| Artifact | Path |
| --- | --- |
| Pre | `build/benchmarks/20260730_230342_phase-2-pre` |
| Post | `build/benchmarks/20260731_004945_phase-2-post` |

## Keeper comparison

| Case | Pre net/disk MB/s | Post net/disk MB/s | Change | TTFB ms | Memory bytes pre → post | Pause median pre → post |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `baseline_default` | 514.88 | 568.89 | +10.49% | 4 → 4 | 135,933,301 → 135,686,970 | 4 → 4 |
| `balanced_candidate` | 509.63 | 505.54 | -0.80% | 6 → 6 | 38,212,687 → 38,441,600 | 32 → 32 |
| `memory_guard` | 641.63 | 671.26 | +4.62% | 4 → 4 | 12,390,678 → 12,382,476 | 16 → 17 |
| `scheduler_stress` | 498.16 | 501.00 | +0.57% | 4 → 4 | 137,767,946 → 137,678,193 | 8 → 8 |

Neither protected throughput case crosses the -5% blocking threshold. `memory_guard` preserves
the low-memory shape; its pause median changes by one episode and remains below the approximately
15% investigation threshold. TTFB is unchanged.

## Risk probes

| Case | Throughput change | Memory bytes pre → post | Inflight bytes pre → post | Pause median pre → post |
| --- | ---: | ---: | ---: | ---: |
| `deep_buffer_candidate` | +0.74% | 225,921,713 → 218,017,022 | 225,222,373 → 217,323,539 | 0 → 0 |
| `queue_backpressure_stress` | +3.59% | 67,093,535 → 67,093,548 | 66,989,174 → 66,956,406 | 4 → 4 |
| `gap_tolerance_probe` | +0.16% | 136,655,168 → 136,712,177 | 135,594,003 → 135,633,521 | 16 → 16 |

No risk probe shows a material memory, inflight, or pause regression.

## Packet and schema invariants

- `packets_enqueued_total` medians remain 16,640–16,642 for the 4 MiB-window cases and 16,449
  for `scheduler_stress`.
- Median average packet sizes remain approximately 64 KiB.
- Maximum packet size remains exactly 65,536 bytes in every case.
- The formal six main and four auxiliary summary keys are unchanged.
- Queue pause remains an episode count. Repeated failed admissions do not increment it.

## Profiler attempt

The required profiler command was run for `throughput_candidate` and `scheduler_stress`.

| Item | Value |
| --- | --- |
| Artifact | `build/profiles/20260731_005636_phase-2-profile` |
| Result | WPR start rejected before the first download |
| Exit code | `3310899217` (`0xc5585011`) |
| Concrete failure | `Failed to enable the policy to profile system performance.` |

The environment did not grant the Windows system-performance tracing privilege, so no fresh ETW
stack sample was produced. This does not waive or replace the benchmark gate. The implementation
audit confirms that the production adapter remains a compile-time concrete member, data uses
`try_enqueue`, there is no virtual or function-pointer dispatch, and no per-packet pimpl or shared
state allocation was added. The unchanged packet totals and sizes show that the callback
aggregation shape did not fragment. The existing March WPR baseline remains the available
reference for the packet allocation/copy and persistence consume stacks.

## Gate result

The Phase 2 performance neutrality gate passes. All 140 post runs succeeded, protected throughput
stayed inside the keeper threshold, and memory, inflight, pause, packet-shape, and schema checks
remained stable.

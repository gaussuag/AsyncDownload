# Phase 5 Evidence: HTTP Transfer

## Identity

| Item | Value |
| --- | --- |
| Stage | 5 — HTTP Transfer |
| Base commit | `6f77a1d` |
| Branch | `codex/refactor-05-http-transfer` |
| First test commit | `0a92032` |
| Last production deletion | `04e0fed` |
| Change classes | S, C, test, perf |
| libcurl | vcpkg `8.18.0#1`, local source and installed headers inspected |

The whole-stage rollback anchor is `6f77a1d`. Dependent slices are reverted in reverse order.

## Slice ledger

| Commit | Class | Slice and rollback unit |
| --- | --- | --- |
| `0a92032` | test | Characterize legacy probe, status, pause replay, port, and retry behavior |
| `c4823b1` | S | Add the dependency-neutral HTTP port and deterministic fake |
| `9666c94` | S | Migrate probe behavior behind the HTTP port |
| `c6e8c07` | C | Validate the final probe response block |
| `594d779` | S | Add the fixed-slot Curl transfer session |
| `049c6db` | S | Migrate network producers to the HTTP session |
| `1cb3fe2` | S | Map HTTP events to Range Leases |
| `56f5680` | C | Validate transfer headers before body admission |
| `7951ca8` | C | Require an identity HTTP representation |
| `0a3d40b` | C | Classify short bodies and terminate long bodies |
| `749274c` | C | Check Curl runtime failures and preserve the first cause |
| `850ee79` | S | Centralize cancel, event drain, and session cleanup |
| `04e0fed` | S | Delete the legacy probe and Curl orchestration state |

Every production row is an independent Git rollback point. Correctness rows include their
deterministic red evidence and fix.

## Contract result

- `HttpTransferPort` owns probe and session creation; `HttpTransferSession` owns the fixed easy
  slots, multi handle, identity header list, callback replay, response validation, cancellation,
  event delivery, and cleanup.
- The engine sees only probe/start/poll/gap/cancel/close and Lease events. It contains no Curl
  handle, callback, option, message, or response-parser state.
- Probe uses HEAD first and a one-byte `0-0` Range fallback only when the defined HEAD contract
  permits it. Redirect parsing accepts only the final header block.
- Transfer success requires the permitted 200/206 status, exact half-open Lease geometry, exact
  `Content-Range` when required, a valid optional `Content-Length`, identity representation, and
  exact accepted body length.
- Probe and transfer explicitly request identity, disable content decoding, and reject a
  non-identity response before body admission.
- A short body is a classified Lease failure. A long body returns `CURL_WRITEFUNC_ERROR`
  immediately and cannot remain paused forever.
- All production setopt/getinfo/pause/multi operations are checked. Fatal multi failure stops all
  active handles, emits one failure event per Lease, and is never retried.
- Session shutdown drains events and cleans easy handles before the multi handle and header list.
  An upstream primary cause is not replaced by cleanup failure.
- HTTP/1.1, fresh/forbid reuse, connection limits, scheduler geometry, 100 ms wait, 1 ms final
  draft compatibility wait, Packet Flow accounting, persistence, recovery, public API, CLI, and
  metadata contracts remain unchanged.

## Curl contract sources

The implementation was checked against the locally installed vcpkg Curl `8.18.0#1` source,
headers, and generated documentation. The inspected contracts include:

- write-callback pause replay and `CURL_WRITEFUNC_ERROR`;
- `curl_easy_pause` receive resume;
- `curl_multi_perform`, `curl_multi_wait`, `curl_multi_info_read`, add/remove, and cleanup;
- `CURLOPT_ACCEPT_ENCODING`, `CURLOPT_HTTP_CONTENT_DECODING`, `CURLOPT_FRESH_CONNECT`, and
  `CURLOPT_FORBID_REUSE`;
- `CURLINFO_RESPONSE_CODE`, `CURLINFO_CONTENT_LENGTH_DOWNLOAD_T`, and
  `CURLINFO_SPEED_DOWNLOAD_T`.

The concrete failure conditions are represented by the compile-time Curl runtime adapter and its
18 deterministic fault tests rather than inferred from return-code names.

## Correctness evidence

The deterministic and real-server suites cover:

- HEAD 200/405/non-200, missing length, zero length, unsolicited 206, and fallback `0-0`;
- redirects and final-header-block selection;
- exact and malformed `Content-Range`, duplicate and malformed `Content-Length`, and invalid
  status;
- ignored partial Range without hidden retry;
- identity request headers and non-identity probe/transfer rejection;
- paused callback replay without duplicate output;
- exact, short, and long bodies;
- distinct client ports for concurrent physical requests;
- slot/token generation, capacity, event order, stale token rejection, gap pause, cancellation,
  and close;
- setopt/getinfo/add/remove/perform/wait/pause/init/allocation/cleanup failures;
- first-cause preservation and easy-before-multi-before-header cleanup.

The Release integration tests prove that failed partial requests are issued once and concurrent
Range requests use distinct client ports.

## Functional verification

| Command or group | Result |
| --- | --- |
| `scripts\build.bat` | pass |
| Debug main test binary | 251 pass, 2 architecture skips |
| Debug HTTP fault binary | 18/18 pass |
| Debug Packet Flow fault binary | 8/8 pass |
| Debug Range Lifecycle fault binary | 6/6 pass |
| Debug Recovery fault binary | 16/16 pass |
| `scripts\build.bat release` | pass |
| Release main test binary | 251 pass, 2 architecture skips |
| Release HTTP fault binary | 18/18 pass |
| Release Packet Flow fault binary | 8/8 pass |
| Release Range Lifecycle fault binary | 6/6 pass |
| Release Recovery fault binary | 16/16 pass |
| Formal Release benchmark | 160/160 pass |
| Formal summary schema | 160/160 exact 10-key sets |

The two skips are the existing architecture-unrepresentable queue-capacity and remote-block-count
boundaries. No failure or skip set expanded. Test executables were run directly because this
desktop environment intermittently fails to propagate the Python child-process path through
aggregated CTest execution.

## Static deletion and dependency audit

| Audit | Result |
| --- | --- |
| Legacy `HttpProbe` class and files | deleted |
| `RemoteProbeResult` | zero production matches |
| Legacy engine Curl helpers and transfer fields | zero production matches |
| Curl C API under download/core/public headers | zero matches |
| Curl C API outside the private HTTP implementation/adapter | zero matches |
| Empty `CURLOPT_ACCEPT_ENCODING` request | zero matches |
| HTTP/Curl types under `include/asyncdownload` | zero matches |
| First-byte production producer | only accepted HTTP callback |
| Download delta production producer | only successful Packet Flow publish |
| Queue/memory episode producer | only Packet Flow |
| Gap episode producer | only HTTP gap transition |

The factory name `create_curl_http_transfer_port` is the only Curl-named engine dependency; it
returns the dependency-neutral `HttpTransferPort`.

## Performance verification

The full Stage 4 frontier and Stage 5 post run use the same host, static loopback URL, 1 GiB
identity object, Release configuration, eight cases, and 20 repeats:

| Case | Stage 4 median MB/s | Stage 5 median MB/s | Change |
| --- | ---: | ---: | ---: |
| `baseline_default` | 696.57 | 571.94 | -17.89% |
| `throughput_candidate` | 495.31 | 484.83 | -2.12% |
| `balanced_candidate` | 522.87 | 515.38 | -1.43% |
| `deep_buffer_candidate` | 488.87 | 492.31 | +0.70% |
| `memory_guard` | 690.73 | 664.63 | -3.78% |
| `scheduler_stress` | 491.85 | 506.47 | +2.97% |
| `queue_backpressure_stress` | 565.37 | 571.79 | +1.14% |
| `gap_tolerance_probe` | 486.56 | 471.89 | -3.01% |

Seven cases remain inside the 5% gate with stable memory, inflight, pause, and packet shape.
`baseline_default` was double-modal and crossed the threshold, so it was elevated to 40 repeats.
The first post confirmation was 588.35 MB/s and confirmed that a cross-time comparison remained
unstable.

The Stage 4 source frontier was then rebuilt at `6f77a1d` in an isolated worktree. Both binaries
were run consecutively with the same server and the same main-worktree output root:

| Signal | Contemporary Stage 4 | Contemporary Stage 5 | Change |
| --- | ---: | ---: | ---: |
| network/disk median | 356.36 MB/s | 582.88 MB/s | +63.56% |
| TTFB median | 13 ms | 4 ms | -69.23% |
| pause median | 55.5 | 4 | -92.79% |
| average packet | 64,520 bytes | 64,516 bytes | -0.01% |
| maximum packet | 65,536 bytes | 65,536 bytes | unchanged |

The contemporary 40-run A/B rejects a Stage 5 baseline regression and demonstrates that the
earlier protected-anchor delta was host scheduling/cache noise. Stage 5 memory/inflight are higher
in this A/B because the faster producer reaches the existing backpressure regime; the full
20-repeat comparison keeps `memory_guard` at 12,414,804 bytes versus 12,382,637 bytes and shows no
multi-case no-benefit memory increase.

Artifacts:

- full actual base: `build/benchmarks/20260731_053020_phase-04-post`;
- full Stage 5 post: `build/benchmarks/20260731_071747_phase-5-post`;
- Stage 5 40-run first confirmation:
  `build/benchmarks/20260731_072610_phase-5-post-baseline-confirmation`;
- normalized contemporary pre:
  `build/benchmarks/20260731_073556_phase-5-pre-normalized-confirmation`;
- normalized contemporary post:
  `build/benchmarks/20260731_073819_phase-5-post-normalized-confirmation`.

WPR was attempted at `build/profiles/20260731_072838_phase-5-profile` and failed at `wpr-start`
with the known system-performance policy error `0xc5585011`. Source and telemetry inspection
therefore answered the permitted hot-path questions:

- response header parsing runs only in the header callback; the body callback reads the cached
  parsed header state for validation;
- the body callback adds no heap allocation, payload copy, mutex, `std::function`, or virtual
  dispatch;
- high-level port/session virtual calls occur only at orchestration boundaries;
- Curl owns paused callback replay and the existing Packet Flow admission path remains unchanged;
- the 1 ms wait is confined to final lane flush and is not on successful body admission.

## Reverse-order rollback

Starting from this evidence frontier, revert `04e0fed`, `850ee79`, `749274c`, `0a3d40b`,
`7951ca8`, `56f5680`, `1cb3fe2`, `049c6db`, `594d779`, `c6e8c07`, `9666c94`, `c4823b1`, and
`0a92032` in that order. The unchanged Recovery Checkpoint frontier is `6f77a1d`.

## Gate result

Stage 5 passes its probe, response, identity, body-length, callback replay, Lease mapping, failure,
shutdown, ownership, compatibility, Debug/Release, schema, deletion, and performance gates.
Stage 6 — Telemetry Session — is unlocked.

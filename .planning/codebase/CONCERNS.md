# Codebase Concerns

**Analysis Date:** 2026-03-21

## Tech Debt

**`src/download/download_engine.cpp`:**
- Issue: The orchestrator is a large monolith that mixes libcurl multi lifecycle, pause/resume policy, backpressure, recovery, metadata coordination, and progress accounting in one file.
- Why: The current design centralizes control flow to keep the download pipeline working end-to-end.
- Impact: Small changes in queue pausing, resume logic, or completion handling can regress unrelated behavior because the state machine is tightly coupled.
- Fix approach: Split policy and state handling into smaller units, especially queue pause handling, transfer bookkeeping, and recovery finalization.

**`src/main.cpp` and `src/metadata/metadata_store.cpp`:**
- Issue: CLI option parsing, config loading, summary formatting, and metadata serialization/deserialization are all permissive and spread across a small number of entry points.
- Why: The project favors direct file-based configuration and JSON metadata over a larger settings layer.
- Impact: Malformed config or metadata can fall back to defaults silently, which makes recovery behavior harder to reason about and harder to debug.
- Fix approach: Add stricter validation and explicit versioned schema checks for `DownloadOptions` and `MetadataState`.

## Security Considerations

**User-controlled filesystem paths in `src/main.cpp`, `src/storage/file_writer.cpp`, and `src/metadata/metadata_store.cpp`:**
- Risk: The CLI accepts output, config, summary, temporary, and metadata paths from user input and then removes or renames files on disk.
- Current mitigation: `overwrite_existing` gates some destructive behavior, but there is no path sandboxing or symlink defense in the inspected code.
- Recommendations: Canonicalize and validate target paths before destructive operations, and reject unexpected symlink or parent traversal cases when the tool is used in automated or shared environments.

## Performance Bottlenecks

**Single-threaded persistence path in `src/persistence/persistence_thread.cpp` and `src/storage/file_writer.cpp`:**
- Problem: Packet draining, ordered append, tail-buffer handling, bitmap updates, and file IO are serialized through one persistence thread and one file handle.
- Measurement: I did not find a repo-side benchmark number for this path in the files inspected; the only explicit controls are `flush_threshold_bytes` and `flush_interval` in `include/asyncdownload/types.hpp`.
- Cause: The design intentionally favors correctness and recovery semantics over parallel disk writes.
- Improvement path: Add benchmark coverage for flush-heavy and gap-heavy workloads, then measure whether metadata batching or less frequent flushes are safe.

**`src/download/http_probe.cpp`:**
- Problem: Every download still pays a probe cost, and the fallback path may issue an extra `Range: 0-0` request when `HEAD` is not usable.
- Measurement: No direct timing data is recorded in the inspected docs or tests.
- Cause: The implementation prefers resilience to a minimal number of round trips.
- Improvement path: Track probe latency in benchmarks and confirm that the fallback path does not dominate small-file downloads or high-latency links.

## Fragile Areas

**Recovery and tail handling in `src/persistence/persistence_thread.cpp`:**
- Why fragile: The file mixes `persisted_offset`, `tail_buffer`, gap detection, async flush scheduling, CRC sampling, and VDL advancement in one state machine.
- Common failures: Off-by-one errors around tail flushes, stuck `pause_for_gap` state, or metadata/VDL updates getting out of sync with the last flushed bytes.
- Safe modification: Change one recovery or flush rule at a time and add a regression test that exercises the exact offset boundaries being changed.
- Test coverage: `tests/persistence/persistence_thread_test.cpp` covers gap pause and queued-byte accounting, but not flush failure, metadata save failure, or recovery after partially written tails.

**Range splitting and steal logic in `src/download/range_scheduler.cpp`:**
- Why fragile: The scheduler depends on block alignment, `accept_ranges`, and off-by-one boundary math when splitting or stealing a range.
- Common failures: Incorrect range boundaries, failed stealing on edge sizes, or new windows that do not align with the bitmap model.
- Safe modification: Keep start/end math aligned with `block_size` and add tests for small totals, non-range servers, and boundary-sized spans.
- Test coverage: `tests/download/range_scheduler_test.cpp` checks aligned splits and a steal path, but not the non-Range fallback or short-file edge cases.

## Scaling Limits

**Out-of-order buffering in `src/persistence/persistence_thread.cpp`:**
- Current capacity: The queue and in-memory map scale with packet count and with the size of the largest gap between `persisted_offset` and the next contiguous packet.
- Limit: When `max_gap_bytes` is large, a single range can accumulate a significant in-memory backlog before gap pause kicks in.
- Symptoms at limit: Higher memory pressure, slower drain of ordered packets, and more frequent backpressure pauses.
- Scaling path: Keep `max_gap_bytes` conservative for large files and add workload benchmarks that sweep packet size, gap size, and queue capacity.

## Dependencies at Risk

**`vcpkg.json` and the libcurl-dependent paths in `src/download/http_probe.cpp`:**
- Risk: The repository depends on `curl` and `nlohmann-json`, and the probe path assumes libcurl behavior around `HEAD`, `Range`, and header parsing.
- Impact: A backend or version change in libcurl can alter probe behavior, especially for servers that do not support `HEAD` cleanly.
- Migration plan: Pin and exercise the supported libcurl behavior in CI, and keep the fallback `Range: 0-0` path under regression tests.

## Missing Critical Features

**Cross-platform recovery verification outside Windows in `tests/download/download_resume_integration_test.cpp` and `tests/support/range_server.py`:**
- Problem: The most important end-to-end recovery tests are Windows-centric and rely on a local Python server harness.
- Current workaround: The repo has unit tests for core pieces, but the full resume path still depends on platform-specific process and path behavior.
- Blocks: POSIX regressions in resume, file rename, or file preallocation can slip through without a matching integration run.
- Implementation complexity: Medium; the server harness already exists, so the main gap is cross-platform execution and assertions.

## Test Coverage Gaps

**`src/storage/file_writer.cpp`:**
- What's not tested: `finalize()`, rename failure handling, flush failure handling, and resume/open interactions on existing `.part` files.
- Risk: Final file promotion or cleanup can fail after the download has otherwise succeeded.
- Priority: High
- Difficulty to test: These paths need filesystem state setup and failure injection.

**`src/metadata/metadata_store.cpp`:**
- What's not tested: Invalid JSON, missing fields, corrupted metadata, and partial `.tmp` recovery behavior.
- Risk: A broken metadata file can still be loaded with default values, which may produce a misleading recovery state.
- Priority: High
- Difficulty to test: Requires malformed-file fixtures and explicit assertions about fallback behavior.

**`src/main.cpp`:**
- What's not tested: Config-file parsing failures, summary-file write failures, and override precedence between `--config` and CLI arguments.
- Risk: The CLI can accept partially valid input and continue with surprising defaults.
- Priority: Medium
- Difficulty to test: Straightforward once config fixtures and summary-file paths are added.

**`tests/download/download_resume_integration_test.cpp`:**
- What's not tested: Real server error cases such as redirect chains, probe failures, and partial-range rejection.
- Risk: Resume logic can look correct against the local test server while still failing on less cooperative HTTP endpoints.
- Priority: High
- Difficulty to test: Moderate; the current Python server harness can be extended, but it needs more cases.

---

*Concerns audit: 2026-03-21*
*Update as issues are fixed or new ones discovered*

# Pitfalls Research: C++ Telemetry Refactoring

**Domain:** AsyncDownload Telemetry Module — C++ Event-Driven Diagnostics
**Researched:** 2026-03-24
**Confidence:** HIGH

## Critical Pitfalls

### Pitfall 1: Telemetry State Re-Infiltration

**What goes wrong:**
After refactoring to use TelemetrySession, developers add new telemetry state back into SessionState for "convenience" — a `max_inflight_bytes` field here, a `total_pause_count` there. Within two phases, SessionState has telemetry runtime state again, defeating the entire refactoring purpose.

**Why it happens:**
SessionState is the natural "grab-bag" object everyone touches. It's easier to add a field there than to trace through the TelemetrySession API. The coupling was never explicitly forbidden, just "discouraged" in documentation.

**How to avoid:**
1. Add a compile-time assertion or static analysis rule: `static_assert(sizeof(SessionState::performance_metrics) == 0)` after migration is complete
2. Make TelemetrySession the **only** class in the include tree that knows about metric accumulation
3. Add a linter rule that flags any `performance_metrics` access outside the telemetry module
4. Document the architectural boundary: SessionState is forbidden territory for telemetry state

**Warning signs:**
- Any `performance_metrics` field added to any struct outside `telemetry/` directory
- Any `fetch_add` or `update_peak` call on session state
- New atomic counters appearing near download logic

**Phase to address:** Phase 2 (TelemetryCollector implementation) — enforce boundaries before state migration begins

---

### Pitfall 2: Summary Calibre Drift

**What goes wrong:**
`benchmark.py` and `profiler.py` output changes after refactoring. Identical inputs produce different `avg_network_speed`, `time_to_first_byte_ms`, or `total_pause_count` values. The architectural separation was "successful" but the numbers are wrong.

**Why it happens:**
The current `build_performance_summary()` in download_engine.cpp uses specific timing and aggregation logic. If TelemetryCollector implements equivalent logic differently — using event timestamps vs wall-clock sampling, different windowing, or different overflow handling — the final numbers diverge.

**How to avoid:**
1. **Before migration**: Document every calculation formula currently in `build_performance_summary()` (lines 958-1003 in download_engine.cpp) with exact arithmetic
2. **During collector design**: Each formula must have a corresponding test that reproduces the current output for a known event sequence
3. **Golden reference**: Create a deterministic event sequence that produces known output; verify collector output matches exactly
4. **Never optimize for "cleaner" numbers** — preserve existing behavioural output even if internal implementation changes

**Warning signs:**
- Any change to how `average_network_bytes_per_second` is calculated
- Changes to how `time_to_first_byte_ms` derives from TaskStarted/FirstByteReceived events
- Modifications to pause count aggregation logic
- Changes in how packet sizes feed into `avg_packet_size_bytes`

**Phase to address:** Phase 3 (Metrics Migration) — must have deterministic output verification before declaring migration complete

---

### Pitfall 3: Partial Event Emission Migration

**What goes wrong:**
Some pause-related code paths emit TelemetryEvents while others still directly mutate SessionState. The final `total_pause_count` is correct in simple cases but wrong when specific pause triggers fire. Memory peaks work correctly but queue-full pauses are counted twice or not at all.

**Why it happens:**
Pause handling is scattered across multiple functions: `start_queue_pause()`, `start_memory_pause()`, `apply_gap_pauses()`, and CURL_WRITEFUNCTION callback. During migration, it's tempting to emit events from the "main" paths and forget edge cases like window-boundary pauses (line 645 in download_engine.cpp) or gap pauses (line 770).

**How to avoid:**
1. **Audit all `performance_metrics` mutations first** — list every site before starting migration:
   - `start_queue_pause()`: total_pause_count + queue_full_pause_count
   - `start_memory_pause()`: total_pause_count
   - Gap pause path (download_engine.cpp:770): total_pause_count
   - Window boundary pause (download_engine.cpp:645): total_pause_count
   - Persistence thread (persistence_thread.cpp:32, 189): max_inflight_bytes, max_memory_bytes
2. **Migrate one metric family at a time** — don't start on pause metrics until packet metrics are verified
3. **Mark migrated sites with comments** — after migrating, leave a `// Telemetry: now emits TelemetryEvent` comment so reviewers can audit completeness

**Warning signs:**
- Any `performance_metrics` mutation remaining after TelemetrySession is "complete"
- Test failures only on edge-case pause scenarios
- Different counts between CLI summary and profiler.py output

**Phase to address:** Phase 3 (Metrics Migration) — requires complete audit of existing mutation sites

---

### Pitfall 4: Timestamp Source Mixing

**What goes wrong:**
`time_to_first_byte_ms` returns nonsensical values (negative, huge, or zero when it should be non-zero). Runtime snapshots show negative durations. Event sequence analysis reveals timestamps that go "backwards" in time.

**Why it happens:**
The architecture mandates `steady_clock` for all timestamps. But `std::chrono::system_clock` is the default in many code patterns, and the existing code already mixes `task_started_at` (steady_clock, line 178 in models.hpp) with other time points. During refactoring, a developer might use `system_clock::now()` for simplicity, breaking monotonicity guarantees.

**How to avoid:**
1. **TelemetryEvent struct must contain only `uint64_t timestamp_ns`** — never expose a `std::chrono::time_point` publicly
2. **Timestamp collection happens at emission site**: `event.timestamp_ns = Clock::now().time_since_epoch().count()` — never let the collector set timestamps
3. **Validation in collector**: If `current_event.timestamp_ns < last_event.timestamp_ns`, log error and reject or correct
4. **Use a type alias**: `using TelemetryTimestamp = std::uint64_t;` with a compile-time guarantee it's derived from `steady_clock::now()`

**Warning signs:**
- Any `std::chrono::system_clock` usage in telemetry-related code
- Time durations that can be negative (indicates clock going backwards)
- TTFB calculations that produce >100x expected values

**Phase to address:** Phase 1 (TelemetryEvent + TelemetrySink) — enforce timestamp discipline from the start

---

### Pitfall 5: Queue Memory Explosion

**What goes wrong:**
On slow networks or when the collector thread stalls, events back up in `moodycamel::BlockingConcurrentQueue`. Memory usage grows unbounded. In extreme cases, the process runs out of memory or the queue's internal ring buffer wraps in unexpected ways.

**Why it happens:**
The design is multi-producer, single-consumer. If `TelemetryCollector` takes too long processing each event (complex aggregations, locks, allocations), producers continue enqueueing faster than the consumer can drain. The queue has a capacity limit, but if producers block on a full queue, download performance degrades.

**How to avoid:**
1. **Collector processing must be O(1) amortized per event** — no locks that all events contend for, no allocations in the hot path
2. **Monitor queue depth**: Expose queue size via `TelemetrySession::queue_size()` for health checks
3. **Define capacity limits**: What happens when queue is 80% full? 100%? Document the backpressure strategy
4. **Consider bounded queue with drop policy**: If events must be dropped under pressure, which ones? (Hint: MemorySamples can be downsampled; TaskStarted/TaskCompleted must not be)
5. **Test under slow collector**: Add a test that artificially slows collector processing and verifies memory stays bounded

**Warning signs:**
- Memory usage growing monotonically during download
- `queue.size()` increasing over time without bound
- Event processing latency increasing as download progresses

**Phase to address:** Phase 2 (TelemetryCollector implementation) — capacity and backpressure design

---

### Pitfall 6: Collector God Object

**What goes wrong:**
`TelemetryCollector` becomes a 1000-line class with 50 member variables handling every metric type. Adding a new metric requires understanding the entire class. Testing requires mocking the entire event stream. The god object replicates the original problem in a different location.

**Why it happens:**
The collector aggregates all state — pause counts, packet sizes, TTFB, memory peaks, speeds. Without modularity, developers add new aggregation fields and logic to the same class, and reviewers don't object because "it's the collector's job."

**How to avoid:**
1. **Separate aggregation concerns**: Use composition inside collector:
   - `PauseAggregator` for pause counts
   - `PacketAggregator` for packet size statistics
   - `SpeedCalculator` for network/disk rates
   - `TTFBTracker` for time-to-first-byte
2. **Each aggregator is independently testable** with deterministic input/output
3. **Collector orchestrates, doesn't compute** — it routes events to sub-aggregators and combines results for snapshots/summary
4. **Add metrics without modifying core collector logic** — the event routing should be extensibility points

**Warning signs:**
- Any single class exceeding 500 lines after migration
- More than 10 atomic/mutex member variables in collector
- A "metrics" struct inside collector that mirrors `RuntimePerformanceMetrics`

**Phase to address:** Phase 2 (TelemetryCollector implementation) — design for composability from day one

---

### Pitfall 7: Snapshot/Summary Divergence

**What goes wrong:**
CLI progress display shows different speeds than `benchmark.py` final summary. Runtime `current_snapshot().network_bytes_per_second` reports 10MB/s but final `average_network_bytes_per_second` is 8MB/s for the same download. Users trust the wrong numbers for capacity planning.

**Why it happens:**
Runtime snapshots and final summary use different calculation paths. Snapshot uses a rolling window (last N seconds); summary uses total bytes / total time. If the event stream feeds both but timing or windowing differs, the numbers diverge even for the same underlying data.

**How to avoid:**
1. **Snapshot and summary must share the same aggregation logic** — the snapshot should be a "partial summary" computed from the same event stream interpretation
2. **Document the relationship**: If snapshot is a rolling window, explicitly state its size and how it relates to final average
3. **Verification test**: Run a download, capture snapshot at T seconds, compare accumulated snapshot average to final summary average — they should be within documented tolerance
4. **No separate calculation paths** — one code path feeds both snapshot and summary, not two independent implementations

**Warning signs:**
- Different formulas for "average speed" appearing in different member functions
- Snapshot tests and summary tests using different input data
- Comments like "this is approximate" near snapshot calculation

**Phase to address:** Phase 2 (TelemetryCollector) — design single aggregation path that serves both consumers

---

### Pitfall 8: Lost Events During Drain

**What goes wrong:**
When download completes, the final summary is missing events. `packets_enqueued_total` is correct but `time_to_first_byte_ms` is zero because the `FirstByteReceived` event was still in the queue when the collector checked. The download "finished" but the telemetry data is incomplete.

**Why it happens:**
The orchestrator calls `final_summary()` immediately after the network phase stops, but the collector's event queue still contains unprocessed events. The collector drains on a separate thread, and there's no synchronization between "download complete" and "collector drain complete."

**How to avoid:**
1. **Define "download complete" semantics**: Is it when network stops? When persistence drains? When file finalize succeeds?
2. **Collector must support blocking drain**: `final_summary()` should block until the queue is empty (or have a `drain_timeout_ms` parameter)
3. **Synchronization point**: After network phase ends, collector should process remaining events before `build_performance_summary()` is called
4. **Test the drain path**: Add a test that enqueues events, immediately requests final_summary, and verifies all events were processed

**Warning signs:**
- `final_summary()` returning before queue is empty
- `time_to_first_byte_ms == 0` in final summary when network clearly received data
- `total_pause_count` different from sum of individual pause events

**Phase to address:** Phase 4 (Integration + Verification) — requires explicit drain synchronization design

---

## Technical Debt Patterns

| Shortcut | Immediate Benefit | Long-term Cost | When Acceptable |
|----------|-------------------|----------------|-----------------|
| Keep some atomic counters in SessionState "temporarily" | Faster migration, less code change | Re-infiltration risk, boundary violation | Never — creates permanent dual-state |
| Use system_clock for timestamps | Simpler debugging, human-readable | Monotonicity violation, wrong TTFB | Never — violates steady_clock requirement |
| Skip unit tests for collector | Faster initial implementation | Regression risk, calculation drift | Only during initial skeleton phase |
| Copy SessionState to TelemetryCollector at startup | No event replay needed | Couples startup/shutdown logic to collector | Only for initial summary, not ongoing |
| Process events on producer thread | No separate consumer thread | Blocking risk, priority inversion | Never — collector must be async consumer |
| Aggregate in-place instead of sub-aggregators | Less code initially | God object, untestable monolith | Only if <5 metrics total |

---

## Performance Traps

| Trap | Symptoms | Prevention | When It Breaks |
|------|----------|------------|----------------|
| Collector lock contention | All producer threads block waiting for collector lock | Use lock-free data structures internally; one mutex for snapshot publication only | 10+ concurrent event sources, high-frequency emission |
| Snapshot computation on every call | Slow UI updates, progress callback blocks | Pre-compute snapshot on event arrival, serve pre-computed value | Large number of active ranges, slow callbacks |
| Queue overflow drops critical events | TaskCompleted never processed, summary empty | Bounded queue with priority: never drop TaskStarted/Completed; MemorySample is droppable | High-speed network (>1Gbps), slow collector |
| Event allocation in hot path | Memory pressure, GC-like pauses | Pre-allocate event pool; use fixed-size tagged union payload | High-frequency DownloadDelta events |

---

## "Looks Done But Isn't" Checklist

- [ ] **Event Emission:** All `performance_metrics` mutation sites converted to `TelemetrySession::record_*()` calls — verify no `fetch_add`, `update_peak`, or direct atomic writes remain in download_engine.cpp or persistence_thread.cpp
- [ ] **Collector Boundaries:** TelemetryCollector is the **only** class that maintains aggregation state — verify SessionState::performance_metrics is empty after migration
- [ ] **Timestamp Validity:** All TelemetryEvent::timestamp_ns derived from `steady_clock::now().time_since_epoch().count()` — verify no `system_clock` usage in telemetry code path
- [ ] **Summary Calibration:** Output of `TelemetrySession::final_summary()` matches output of existing `build_performance_summary()` for identical downloads — run benchmark.py before/after comparison
- [ ] **Queue Drain:** `final_summary()` blocks until queue is empty — verify TTFB and final counts are correct even for short downloads
- [ ] **Snapshot Consistency:** `current_snapshot().network_bytes_per_second` and `final_summary().average_network_bytes_per_second` derive from same logic — verify proportional relationship documented and tested
- [ ] **Memory Bounds:** Event queue memory bounded regardless of download duration — verify no unbounded growth under sustained high-frequency emission

---

## Recovery Strategies

| Pitfall | Recovery Cost | Recovery Steps |
|---------|---------------|----------------|
| Telemetry State Re-Infiltration | HIGH | Requires manual audit of all structs, potentially rolling back phase 3 work, re-enforcing boundaries |
| Summary Calibre Drift | MEDIUM | Re-run migration with golden reference tests, compare output byte-by-byte, fix collector formulas |
| Partial Event Emission | MEDIUM | Audit remaining mutation sites, add missing event emissions, re-run edge-case tests |
| Timestamp Mixing | HIGH | Replace all system_clock with steady_clock, verify TTFB calculation, re-baseline all time-dependent tests |
| Queue Explosion | MEDIUM | Add bounded queue with backpressure, drop MemorySample under pressure, add monitoring |
| Collector God Object | LOW | Refactor collector into sub-aggregators — incremental, no boundary changes |
| Snapshot/Summary Divergence | MEDIUM | Unify aggregation path, add consistency tests, document tolerance |
| Lost Events on Drain | HIGH | Redesign drain protocol, add blocking drain with timeout, add end-to-end event count verification |

---

## Pitfall-to-Phase Mapping

| Pitfall | Prevention Phase | Verification |
|---------|------------------|--------------|
| Telemetry State Re-Infiltration | Phase 2: Boundaries enforced before state migration | Code review + linter rules; SessionState::performance_metrics empty |
| Summary Calibre Drift | Phase 3: Metrics migration with golden tests | Before/after benchmark.py output matches exactly |
| Partial Event Emission | Phase 3: Audit all mutation sites first | No `performance_metrics` access outside telemetry module |
| Timestamp Mixing | Phase 1: TelemetryEvent design | Unit test verifies monotonic timestamps |
| Queue Explosion | Phase 2: Collector capacity design | Stress test with bounded queue, verify memory limits |
| Collector God Object | Phase 2: Sub-aggregator design | Each aggregator independently testable, <500 lines per class |
| Snapshot/Summary Divergence | Phase 2: Unified aggregation design | Consistency test documents expected relationship |
| Lost Events on Drain | Phase 4: Drain protocol design | End-to-end event count verification test |

---

## Sources

- Existing codebase analysis: download_engine.cpp (lines 138-1033), persistence_thread.cpp (lines 28-33), models.hpp (SessionState lines 151-195)
- Architecture design: .planning/performance.md (steady_clock requirement, collector responsibilities, snapshot/summary split)
- Project constraints: .planning/PROJECT.md (backward compatibility requirement for benchmark.py/profiler.py)
- Domain knowledge: C++ event-driven telemetry patterns, lock-free queue backpressure strategies

---
*Pitfalls research for: AsyncDownload C++ Telemetry Refactoring*
*Researched: 2026-03-24*

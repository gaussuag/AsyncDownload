# Phase 2: Collector Computation - Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in CONTEXT.md — this log preserves the alternatives considered.

**Date:** 2026-03-24
**Phase:** 02-collector-computation
**Areas discussed:** Aggregation algorithm, Snapshot consistency, State storage strategy, Final summary computation, Error handling

---

## Aggregation Algorithm

| Option | Description | Selected |
|--------|-------------|----------|
| TTFB: Immediately on FirstByteReceived | Incremental update, store result | ✓ |
| TTFB: Only at final_summary() | Compute on demand | |
| TTFB: Every current_snapshot() call | Real-time update | |

**User's choice:** TTFB stored only in summary_, computed when FirstByteReceived arrives
**Notes:** TTFB 本质上就是两个时间戳的差值，不是复杂计算

| Option | Description | Selected |
|--------|-------------|----------|
| Speed: Simple cumulative average | total_bytes / total_time | |
| Speed: Weighted moving average | EMA approach | ✓ |
| Speed: Time-weighted average | Based on actual duration per segment | |

**User's choice:** EMA (Exponential Moving Average) with α = 0.8
**Notes:** 业界标准做法，如 aria2 等下载器使用 EMA 平滑速度波动

| Option | Description | Selected |
|--------|-------------|----------|
| EMA α = 0.8 (default) | Balance between smoothness and responsiveness | ✓ |
| EMA α = 0.9 (smoother) | More stable speed curve | |
| EMA α = 0.7 (faster response) | More responsive to speed changes | |

**User's choice:** α = 0.8

| Option | Description | Selected |
|--------|-------------|----------|
| Packet size: Incremental calculation | total_bytes / count on each event | ✓ |
| Packet size: Only at final_summary() | Store all bytes, compute once at end | |

**User's choice:** Incremental calculation

---

## Snapshot Consistency

| Option | Description | Selected |
|--------|-------------|----------|
| Copy-on-read | Atomically copy entire struct on read | ✓ |
| Read-write lock (std::shared_mutex) | Shared readers, exclusive writer | |
| RCU (Read-Copy-Update) | Copy on write, readers read old version | |

**User's choice:** Copy-on-read

| Option | Description | Selected |
|--------|-------------|----------|
| Recent event timestamp | last_event.timestamp_ns | |
| Current time (steady_clock::now()) | Snapshot generation time | ✓ |
| First event timestamp | task_started timestamp | |

**User's choice:** steady_clock::now() at moment of call — acts as snapshot ID with monotonically flowing time

| Option | Description | Selected |
|--------|-------------|----------|
| Return immediately (recommended) | Return current aggregated state | ✓ |
| Brief wait (e.g., 1ms) | Wait up to 1ms for queue drain | |
| Full wait | Wait for queue to fully drain | |

**User's choice:** Return immediately

---

## State Storage Strategy

| Option | Description | Selected |
|--------|-------------|----------|
| Full storage | Keep all intermediate state for debugging | ✓ |
| Minimal storage | Store only peaks and totals, recalculate on demand | |
| Hybrid mode | Critical values in members, debug info in logs | |

**User's choice:** Full storage

| Option | Description | Selected |
|--------|-------------|----------|
| std::atomic | Use atomic for counters/flags | ✓ |
| All mutex | Unified mutex protection | |
| Lock-free | Lock-free accumulation algorithm | |

**User's choice:** std::atomic

---

## Final Summary Computation

| Option | Description | Selected |
|--------|-------------|----------|
| Pre-compute on TaskCompleted | Pre-calculate when TaskCompleted event arrives | ✓ |
| Compute on final_summary() call | Calculate on demand, ensure final state | ✓ |
| Both | TaskCompleted pre-compute + final_summary() recalculate | ✓ |

**User's choice:** Both — TaskCompleted pre-computes, final_summary() also recalculates for confirmation

| Option | Description | Selected |
|--------|-------------|----------|
| All metrics (recommended) | Include all direct and derived metrics | ✓ |
| Derived metrics only | TTFB, speeds, avg_packet only at final_summary | |
| Direct metrics only | Only peaks and counts | |

**User's choice:** All metrics

---

## Error Handling

| Option | Description | Selected |
|--------|-------------|----------|
| Ignore subsequent | Keep first TaskStarted timestamp, ignore later ones | ✓ |
| Overwrite | Reset with new timestamp | |
| Log warning | Ignore and record warning | |

**User's choice:** Ignore subsequent

| Option | Description | Selected |
|--------|-------------|----------|
| Ignore (recommended) | Silently ignore events after TaskCompleted | ✓ |
| Log warning | Ignore and record warning | |

**User's choice:** Silently ignore

| Option | Description | Selected |
|--------|-------------|----------|
| Silently ignore (recommended) | Ignore out-of-order events | ✓ |
| Log warning | Ignore and record warning | |
| Count as anomaly | Track but mark as abnormal | |

**User's choice:** Silently ignore

---

## Agent Discretion

Areas where user deferred to agent:
- Exact member variable naming within TelemetryCollector
- Internal helper methods for aggregation computation
- How to flush/reset state between tasks

## Deferred Ideas

None — discussion stayed within Phase 2 scope.

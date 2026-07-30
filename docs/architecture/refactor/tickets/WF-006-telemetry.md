# WF-006：Telemetry Session

- Type: `task`
- Status: `resolved`
- Depends on: `WF-005`
- Produces: `phases/06_telemetry_session.md`

## Question

如何保留 `TelemetrySession` 稳定 seam，同时消除与 `TelemetryCollector` 重复的 interface 和
测试表面？

## Investigation boundary

- 保持正式 10 项 Performance Summary schema。
- 不重新引入 TelemetrySink、完整 event 总线或已退役诊断字段。
- 不让 telemetry 成为核心状态的第二真相源。

## Close criteria

- 定义 session interface、状态所有权、快照一致性和线程语义。
- 明确 collector 的吸收或内部化方式。
- 给出调用方、测试和 benchmark schema 的迁移步骤。

## Resolution

选择“`TelemetrySession` state owner + `TelemetryCollector` public compatibility shell”。

详细实现合同：

- [阶段 6：Telemetry Session](../phases/06_telemetry_session.md)

已关闭的决策：

- `TelemetrySession` 独占 task lifecycle、steady-clock 时间规则、EMA、observed byte totals、
  memory/inflight peaks、pause/packet counters、snapshot 兼容投影和正式 summary 计算；
- `TelemetryCollector` 保留全部现有公开签名，但 private 只保存一个 `TelemetrySession` 并
  委托，不再拥有 mutex、状态、计算 helper 或 production caller；
- 两个 class 的现有 method type、default argument、`noexcept`、enum 数值和旧 include 路径
  保持源码兼容；
- 为 deterministic Session tests 增加唯一命名的 `_at` variants，不增加会让
  `auto p = &TelemetrySession::record_download_delta` 变歧义的同名 overload；
- 使用一个 public declaration hub 消除 Session/Collector include cycle，同时保留当前
  transitive include 可见性；class private layout改变要求 static library全量重编译，不承诺
  precompiled binary ABI；
- 正式 6 main + 4 auxiliary Performance Summary 的名称、类型、公式、CLI key 和 Python
  parser type逐项固定；
- production progress 从空 `ProgressSnapshot` 显式合并各权威 module，只从 Telemetry
  复制 network/disk EMA；
- Packet Flow successful Data Packet publish 是 download delta、network EMA 和 packet
  summary 的唯一 producer；HTTP slot raw rate只形成 `PacketLaneObservation`；
- HTTP accepted nonzero body是 first-byte 唯一 producer；Gap 只由
  `HttpTransferSession::set_gap_paused()` bit `0 → 1` 记录；
- Queue/Memory pause和Accounted Bytes peak只由 Packet Flow记录；Persistence successful
  physical write是 persist delta唯一 producer；
- normal exact-window 与 long-body terminal error都不再产生 window pause；
- record/query同步使用一个 task-local mutex，无 event queue、worker、allocation或第二份
  atomic aggregation；
- overflow saturation作为独立 correctness commit，不与 state ownership move混合；
- 迁移拆成 tests-first、deterministic variants、ownership inversion、progress merge、
  producer alignment、overflow hardening、test collapse和 evidence 8 个可回滚 slices。

Evidence：

- 当前公开 header 与实现：
  `telemetry_session.hpp/.cpp`、`telemetry_collector.hpp/.cpp`、
  `telemetry_event.hpp`、`performance_metrics.hpp`、`types.hpp`；
- 当前 producer：
  `DownloadEngine`、HTTP callback/queue pause helpers、`PersistenceThread`；
- 当前 tests：
  `telemetry_collector_test.cpp`、`telemetry_event_emission_test.cpp`、
  `telemetry_skeleton_test.cpp`、`ReportsDetailedProgressSnapshot`；
- 历史漂移：
  `.planning/phases/01-telemetry-skeleton`、提交 `9687749` 与 `7a96af2`；
- performance source of truth：
  `performance_playbook_zh.md`、
  `performance_baseline_20260311_regression_v2_zh.md`、
  `optimization_regression_guide_zh.md`、
  `performance_optimization_history_zh.md`、
  `profiler_behavior_baseline_20260311_zh.md`；
- Phase 2/5 交叉合同已校正为
  `PacketFlow publish → record_download_delta` 唯一路径，避免 HTTP double-count。

合同已给出 exact C++ interface、source/P2M/ABI matrix、10-key 字典、idempotency、
time-order、overflow、mutex/lifetime/memory-order、producer migration、progress merge、
tests、schema/benchmark/profiler gates、deletion checks、rollback 和 stop conditions。

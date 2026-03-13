# AsyncDownload Queue/Backpressure 指标补充方案

## 1. 目的

这份文档用于定义下一轮 queue/backpressure 诊断需要补充的指标。

目标不是直接带来吞吐收益，而是先回答下面 3 个问题：

1. `queue_full_pause` 从发生到真正恢复，实际持续了多久。
2. queue-full pause 是否被当前 memory-backpressure 的恢复语义放大。
3. 仅用 `queued_packets` 观察队列是否已经不足以描述 `64KiB` 聚合后的真实预算行为。

如果这 3 个问题不能先被量化，后续再做 queue/backpressure 架构调整，仍然会停留在猜测层面。

## 2. 非目标

这轮线程不应默认做下面这些事：

- 不直接修改 queue/full pause 与 memory pause 的行为语义
- 不把 packet 粒度、window 大小、flush 策略再当成主实验方向
- 不把新指标线程和新的 keeper 优化混在一起

这轮的判定标准是“诊断能力是否补齐”，不是“吞吐是否已经提升”。

## 3. 当前缺口

截至当前版本，已有指标能告诉我们：

- pause 总次数
- `queued_packets`
- `max_memory_bytes`
- `flush_pending_write`
- `crc_sample_read`
- 主顺序追加链与乱序链的命中比例

但还不能回答：

- 单次 queue-full pause 的持续时间分布
- queue-full pause 何时已经具备恢复条件，却仍被 memory 低水位门槛挡住
- queue 满时真实积压的是多少字节，而不是多少 packet
- 哪些 case 的 pause churn 本质上是 queue 满，哪些是恢复过慢

## 4. 建议新增指标

### 4.1 运行态状态字段

这些字段不一定全部导出到最终 benchmark summary，但需要先在运行态存在：

- `SessionState::queued_bytes`
  - 语义：当前仍在 network -> persistence 队列里的累计 `accounted_bytes`
  - 作用：给 `queued_packets` 增加字节维度
- `TransferHandle::queue_pause_active`
  - 语义：当前 handle 最近一次 pause 是否由 queue-full 触发
  - 作用：把 queue-full pause 和 memory pause 的来源分开记录，但暂不改变行为
- `TransferHandle::queue_pause_started_at`
  - 语义：当前 queue-full pause 的开始时间
- `TransferHandle::memory_pause_started_at`
  - 语义：当前 memory pause 的开始时间
- `TransferHandle::queue_resume_blocked_by_memory_started_at`
  - 语义：queue-full 已经具备恢复条件，但仍被 memory 恢复门槛挡住时的开始时间

### 4.2 建议新增 direct metrics

这些字段建议进 `include/asyncdownload/performance_metrics.hpp` 的 direct 指标组。

- `max_queued_bytes`
  - 语义：队列内累计字节峰值
- `max_queue_paused_handles`
  - 语义：同一时刻处于 queue-full pause 的 handle 峰值
- `max_memory_paused_handles`
  - 语义：同一时刻处于 memory pause 的 handle 峰值
- `queue_full_resume_count`
  - 语义：成功从 queue-full pause 恢复的次数
- `memory_resume_count`
  - 语义：成功从 memory pause 恢复的次数
- `queue_resume_blocked_by_memory_count`
  - 语义：queue-full 已满足恢复条件，但因为 memory 低水位尚未满足而继续等待的次数
- `queue_pause_overlap_memory_count`
  - 语义：queue-full pause 生命周期中曾与 memory pause 重叠的次数
- `queue_full_pause_start_queued_packets_total`
  - 语义：所有 queue-full pause 开始时的 `queued_packets` 求和
- `queue_full_pause_start_queued_bytes_total`
  - 语义：所有 queue-full pause 开始时的 `queued_bytes` 求和
- `memory_pause_start_queued_packets_total`
  - 语义：所有 memory pause 开始时的 `queued_packets` 求和
- `memory_pause_start_queued_bytes_total`
  - 语义：所有 memory pause 开始时的 `queued_bytes` 求和

### 4.3 建议新增 sampled duration metrics

这些字段建议沿用现有 sampled metric 结构保存 runtime raw counters，再在 `build_performance_summary()` 里转成导出值。

- `queue_full_pause_duration`
  - 语义：一次 queue-full pause 从开始到恢复的持续时长
- `memory_pause_duration`
  - 语义：一次 memory pause 从开始到恢复的持续时长
- `queue_resume_blocked_by_memory_duration`
  - 语义：queue-full 已具备恢复条件，但实际恢复仍被 memory 门槛延后的持续时长

### 4.4 建议新增 derived summary metrics

这些字段建议只存在于 `PerformanceSummary`，不一定需要平铺进 runtime direct struct。

- `queue_full_pause_avg_ms`
- `queue_full_pause_max_ms`
- `memory_pause_avg_ms`
- `memory_pause_max_ms`
- `queue_resume_blocked_by_memory_avg_ms`
- `queue_resume_blocked_by_memory_max_ms`
- `queue_full_pause_start_queued_packets_avg`
- `queue_full_pause_start_queued_bytes_avg`
- `memory_pause_start_queued_packets_avg`
- `memory_pause_start_queued_bytes_avg`

## 5. 指标采集口径

### 5.1 `queued_bytes`

口径建议与 `queued_packets` 保持一致：

- enqueue 成功时，加上 `packet.accounted_bytes`
- Persistence 真正开始处理 packet 时，减去 `packet.accounted_bytes`

不要把下面这些内容混进 `queued_bytes`：

- `TransferHandle` 本地聚合缓冲
- `out_of_order_queue`
- tail buffer

否则它就不再对应“network -> persistence 之间的队列积压”。

### 5.2 queue-full pause 的开始时机

建议只在 `download_engine.cpp` 里真实走到“因为 `try_enqueue()` 失败而返回 pause”时记一次开始。

也就是现有这些点：

- write callback 内，buffer flush 失败并返回 `CURL_WRITEFUNC_PAUSE`
- window 边界 flush 失败并进入 pause

要求：

- 同一段连续 pause 生命周期只记一次开始，不要每次循环都重复累加
- 只有实际恢复时才完成一次 duration 样本

### 5.3 memory pause 的开始时机

建议只在 `apply_memory_backpressure()` 内，第一次把一个未暂停 handle 切成 memory pause 时记一次开始。

不要把 queue-full 触发的 pause 也算到 memory pause 里。

### 5.4 “queue 已可恢复但被 memory 挡住”的判定

当前语义下，不建议为了采指标就改恢复逻辑。先按下面的保守规则记录：

- 若 handle 处于 `queue_pause_active`
- 且当前 `queued_packets < queue_capacity_packets`
- 但 `resume_paused_transfers()` 因 `current_bytes > backpressure_low_bytes` 直接返回

则认为这次 queue-full pause 已满足 queue 侧恢复条件，但被 memory 门槛继续阻塞。

这时：

- 若该 handle 还没有 `queue_resume_blocked_by_memory_started_at`，则启动一次阻塞计时
- 真正恢复时结束这次阻塞计时

### 5.5 pause overlap 的判定

建议使用最小可解释规则：

- 若一个 handle 已处于 `queue_pause_active`
- 后续又被 memory backpressure 命中

则 `queue_pause_overlap_memory_count += 1`

一段 queue pause 生命周期最多记一次 overlap，避免重复膨胀。

## 6. 建议修改点

### 6.1 C++ 结构定义

需要检查并可能修改：

- `include/asyncdownload/performance_metrics.hpp`
- `include/asyncdownload/types.hpp`
- `src/core/models.hpp`

### 6.2 运行态采集点

主要在：

- `src/download/download_engine.cpp`
  - enqueue 成功时更新 `queued_bytes`
  - queue-full pause 开始
  - memory pause 开始
  - pause overlap 统计
  - resume 时完成 queue/full 与 memory duration
  - 识别 queue-ready-but-memory-blocked
- `src/persistence/persistence_thread.cpp`
  - dequeue 开始处理 packet 时扣减 `queued_bytes`

### 6.3 summary 与导出链路

需要同步：

- `src/download/download_engine.cpp`
  - `build_performance_summary()`
- `src/main.cpp`
  - summary 输出 key
- `scripts/performance/performance_common.py`
  - `SUMMARY_SPECS`

### 6.4 测试

至少需要补：

- `tests/persistence/persistence_thread_test.cpp`
  - `queued_bytes` 入队/出队口径
- `tests/download/*`
  - queue-full pause 和 memory pause 生命周期的最小行为断言
- 如现有测试不方便覆盖，可新增更小的 `download_engine` 定位测试

## 7. 导出命名建议

建议对外 summary key 直接使用下面这些名字，避免新 thread 再做命名猜测：

- `max_queued_bytes`
- `max_queue_paused_handles`
- `max_memory_paused_handles`
- `queue_full_resume_count`
- `memory_resume_count`
- `queue_resume_blocked_by_memory_count`
- `queue_pause_overlap_memory_count`
- `queue_full_pause_avg_ms`
- `queue_full_pause_max_ms`
- `memory_pause_avg_ms`
- `memory_pause_max_ms`
- `queue_resume_blocked_by_memory_avg_ms`
- `queue_resume_blocked_by_memory_max_ms`
- `queue_full_pause_start_queued_packets_avg`
- `queue_full_pause_start_queued_bytes_avg`
- `memory_pause_start_queued_packets_avg`
- `memory_pause_start_queued_bytes_avg`

## 8. 验证方案

### 8.1 基础验证

```powershell
scripts\build.bat release
ctest -C Release --output-on-failure -E "DownloadIntegrationTest.*"
```

### 8.2 指标验证 benchmark

建议先跑最小核心 case：

```powershell
python scripts/performance/benchmark.py --url "http://127.0.0.1:4287/1gb_files.zip" --benchmark-suite regression_v2 --case-list baseline_default,balanced_candidate,memory_guard,scheduler_stress --repeats 5 --label "queue-pause-instrumentation-validation"
```

### 8.3 验证时应重点检查

- `max_queued_bytes` 是否在 4 个 case 中都非零
- `queue_full_pause_avg_ms` 是否在 `baseline_default` / `balanced_candidate` / `memory_guard` 中都有信号
- `queue_resume_blocked_by_memory_*` 是否在 `memory_guard` 中明显高于 `baseline_default`
- `queue_full_pause_start_queued_bytes_avg` 与 `queue_full_pause_start_queued_packets_avg` 是否能表现出“同样 packet 数但字节预算不同”的情况
- 新指标接入后，旧 summary key 不得消失

## 9. 预期产出

这轮指标补齐完成后，后续 thread 应能明确回答：

1. `queue_full_pause` 的主要成本是“pause 次数太多”，还是“恢复太慢”。
2. `memory_guard` 的劣化更多来自真实内存约束，还是 queue-full pause 被 memory 低水位门槛延长。
3. 继续用 `queue_capacity_packets` 作为主预算控制量是否已经不够。

如果这些问题被回答清楚，下一轮才适合真正进入 queue/backpressure 架构调整。

## 10. 新 Thread 建议

建议新 thread 直接以这份文档作为实现说明，并使用下面这个起手目标：

`先补齐 queue/backpressure 诊断指标，不改当前 pause/resume 语义；完成 build、测试和 4 个核心 case 的 benchmark 导出验证。`

# AsyncDownload Queue/Backpressure 优化 Thread 初始化

## 1. Thread 信息

- `thread_key`: `性能优化314-019ce7c5-5110-7be3-b85b-ebf4605f772f`
- 初始化时间: `2026-03-13 23:22:43`
- 线程定位: queue/backpressure 约束闭环与 pause churn 优化线程

这份文档用于固定当前性能优化 thread 的正式起点，避免后续实现和验证继续混用上一条“queue/backpressure 指标开发线程”的目标、结论和边界。

## 2. 当前正式起点

当前 thread 的文档起点应以以下文档为准：

- [performance_playbook_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/performance_playbook_zh.md)
- [performance_baseline_20260311_regression_v2_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/performance_baseline_20260311_regression_v2_zh.md)
- [optimization_regression_guide_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/optimization_regression_guide_zh.md)
- [performance_optimization_history_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/performance_optimization_history_zh.md)
- [profiler_behavior_baseline_20260311_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/profiler_behavior_baseline_20260311_zh.md)
- [queue_backpressure_instrumentation_plan_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/queue_backpressure_instrumentation_plan_zh.md)

当前代码层面的主要实现入口：

- [performance_metrics.hpp](/D:/git_repository/coding_with_agents/AsyncDownload/include/asyncdownload/performance_metrics.hpp)
- [types.hpp](/D:/git_repository/coding_with_agents/AsyncDownload/include/asyncdownload/types.hpp)
- [models.hpp](/D:/git_repository/coding_with_agents/AsyncDownload/src/core/models.hpp)
- [download_engine.cpp](/D:/git_repository/coding_with_agents/AsyncDownload/src/download/download_engine.cpp)
- [persistence_thread.cpp](/D:/git_repository/coding_with_agents/AsyncDownload/src/persistence/persistence_thread.cpp)
- [main.cpp](/D:/git_repository/coding_with_agents/AsyncDownload/src/main.cpp)
- [performance_common.py](/D:/git_repository/coding_with_agents/AsyncDownload/scripts/performance/performance_common.py)
- [concurrentqueue.h](/D:/git_repository/coding_with_agents/AsyncDownload/libs/concurrentqueue/include/concurrentqueue/concurrentqueue.h)
- [blockingconcurrentqueue.h](/D:/git_repository/coding_with_agents/AsyncDownload/libs/concurrentqueue/include/concurrentqueue/blockingconcurrentqueue.h)

## 3. 当前已确认的线程起点

截至 `2026-03-13`，当前 thread 的共同前提是：

1. 当前正式 benchmark 基线仍然是 `regression_v2`，主回归观察顺序仍以 `baseline_default`、`balanced_candidate`、`memory_guard`、`scheduler_stress` 为先。
2. queue/backpressure 生命周期指标已经补齐，当前不再缺“观测面”，而是缺“解释面”和“keeper 级闭环”。
3. `BlockingConcurrentQueue` 已按 explicit producer + 预分配方式修正，第三方 queue 的使用方式错位已被收敛，这个修正应当保留。
4. 但多组 case 的运行态 queue 峰值仍长期停在约 `~1000 packets / ~64-66 MiB`，说明当前约束并不只来自先前的 implicit producer/no-allocation 错位。
5. `queue_backpressure_stress` 仍然表现出双层放大：
   - queue pause 会先被 queue 写入边界触发
   - 恢复阶段又可能继续被 memory low-watermark 延后
6. 因此这条新 thread 的主要任务，不再是补指标，也不是直接追求新的峰值吞吐，而是把“真实 queue 峰值为什么停在这里”和“pause churn 为什么仍然高”继续闭环到可操作的优化结论。

## 4. 本线程默认主目标

如果后续没有新的目标指定，本 thread 默认采用以下主目标：

1. 以“更平滑的 stress-case 行为”为主目标，先解释并尽量收敛 queue/full pause 的高频 churn。
2. 把 `deep_buffer_candidate` 与 `queue_backpressure_stress` 的真实 queue 峰值约束闭环到具体机制，而不是继续停留在现象层。
3. 在不破坏 `balanced_candidate` 吞吐优势和 `memory_guard` 低内存优势的前提下，再评估 queue-full pause 与 memory low-watermark 恢复门槛是否值得解耦。

这意味着当前默认优先级是：

1. 解释约束
2. 处理 churn
3. 再判断 keeper 改法

不是直接把“更高吞吐”当成第一目标。

## 5. 工作边界

后续在这个 thread 里继续推进时，默认遵守以下边界：

1. 不把 packet 聚合、window 大小、flush 策略重新拉回本线程作为主实验方向，除非新证据明确表明主瓶颈已经迁移。
2. 不把“已经修正 explicit producer 用法”再次当作待验证假设；后续应在这个修正过的基础上继续分析。
3. 不因为单个 smoke case 变快就直接判定 keeper 成立，正式收益判断仍以 `Release + regression_v2` benchmark 为准。
4. 若下一步需要显式修改 queue/full pause 与 memory backpressure 的恢复语义，应先说明语义边界风险，再进入代码实现。
5. 若后续发现主瓶颈已经从 queue/backpressure churn 迁移到别的热点，应视为新的优化闭环，并重新判断是否该切线程。

## 6. 初始化后的标准验证入口

后续每轮实际改动完成后，优先按以下顺序验证：

1. 构建

```powershell
scripts\build.bat release
```

2. 测试

```powershell
ctest -C Release --output-on-failure -E "DownloadIntegrationTest.*"
```

3. 定向 smoke benchmark

```powershell
python scripts/performance/benchmark.py --url "http://127.0.0.1:4287/1gb_files.zip" --benchmark-suite regression_v2 --case-list baseline_default,balanced_candidate,memory_guard,deep_buffer_candidate,queue_backpressure_stress --repeats 5 --label "queue-churn-smoke"
```

4. 需要 keeper 结论时，再补正式 benchmark

```powershell
python scripts/performance/benchmark.py --url "http://127.0.0.1:4287/1gb_files.zip" --benchmark-suite regression_v2 --repeats 20 --label "queue-churn-post-change"
```

5. 只有在需要解释热点迁移或确认 pause 路径时，才单独补 profiler

```powershell
python scripts/performance/profiler.py --url "http://127.0.0.1:4287/1gb_files.zip" --benchmark-suite regression --case-list throughput_candidate,scheduler_stress --label "queue-churn-profile"
```

## 7. 推荐的起步检查点

如果下一步继续进入实现，建议先围绕以下问题展开：

1. `resume_paused_transfers()` 中，queue-ready 与 memory-ready 当前是否被同一个恢复门槛绑得过紧。
2. `scheduler_window_bytes * max_connections`、`queued_bytes` 峰值和 `max_queued_packets` 之间，是否已经形成稳定的运行态预算上限。
3. `deep_buffer_candidate` 与 `queue_backpressure_stress` 中，真实进入 pause 的时机更像“生产预算撞顶”还是“恢复路径太保守”。
4. `memory_guard` 中的 `queue_resume_blocked_by_memory_*` 指标，是否已经足以证明 memory low-watermark 在放大 queue pause 生命周期。
5. 若要提出新的 keeper 改法，它与之前被采纳或被降级的实验相比，具体差异到底是什么。

## 8. 初始化结论

这个新的 queue/backpressure 优化 thread 已完成起步基线固定：

- 新 `thread_key` 已确定
- 当前正式基线、历史结论和实现入口已固定
- 默认主目标已明确为“先解释约束并收敛 stress-case pause churn”
- 正式验证顺序已固定为 build -> test -> smoke benchmark -> 正式 benchmark -> profiler

后续可以直接从这份初始化文档进入下一轮代码分析、keeper 设计和回归验证。

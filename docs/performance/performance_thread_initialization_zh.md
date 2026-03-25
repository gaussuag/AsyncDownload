# AsyncDownload 性能指标清理 Thread 初始化

## 1. Thread 信息

- `thread_key`: `指标清理-019d19e5-2d63-70f0-a8d9-48c1a895e92e`
- 初始化时间: `2026-03-23 16:58:47`
- 线程定位: 性能指标清理与导出链路收敛线程

这份文档用于固定当前性能线程的正式起点，避免后续实现和验证继续混用上一条 queue/backpressure 优化线程的目标、结论和边界。

## 2. 当前正式起点

当前 thread 的文档起点应以以下文档为准：

- [performance_playbook_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/performance_playbook_zh.md)
- [performance_baseline_20260311_regression_v2_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/performance_baseline_20260311_regression_v2_zh.md)
- [optimization_regression_guide_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/optimization_regression_guide_zh.md)
- [performance_optimization_history_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/performance_optimization_history_zh.md)
- [queue_backpressure_instrumentation_plan_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/queue_backpressure_instrumentation_plan_zh.md)

当前代码层面的主要实现入口：

- [performance_metrics.hpp](/D:/git_repository/coding_with_agents/AsyncDownload/include/asyncdownload/performance_metrics.hpp)
- [types.hpp](/D:/git_repository/coding_with_agents/AsyncDownload/include/asyncdownload/types.hpp)
- [models.hpp](/D:/git_repository/coding_with_agents/AsyncDownload/src/core/models.hpp)
- [download_engine.cpp](/D:/git_repository/coding_with_agents/AsyncDownload/src/download/download_engine.cpp)
- [persistence_thread.cpp](/D:/git_repository/coding_with_agents/AsyncDownload/src/persistence/persistence_thread.cpp)
- [main.cpp](/D:/git_repository/coding_with_agents/AsyncDownload/src/main.cpp)
- [performance_common.py](/D:/git_repository/coding_with_agents/AsyncDownload/scripts/performance/performance_common.py)
- [persistence_thread_test.cpp](/D:/git_repository/coding_with_agents/AsyncDownload/tests/persistence/persistence_thread_test.cpp)
- [download_resume_integration_test.cpp](/D:/git_repository/coding_with_agents/AsyncDownload/tests/download/download_resume_integration_test.cpp)

## 3. 当前已确认的线程起点

截至 `2026-03-23`，当前 thread 的共同前提是：

1. 当前正式 benchmark 基线仍然是 `regression_v2`，后续任何指标清理都不能默认破坏与这份基线的可比性。
1. 当前正式 benchmark 主链已固定为：
   - `avg_network_speed`
   - `avg_disk_speed`
   - `time_to_first_byte_ms`
   - `max_memory_bytes`
   - `max_inflight_bytes`
   - `total_pause_count`
2. 当前正式辅助指标已固定为：
   - `queue_full_pause_count`
   - `packets_enqueued_total`
   - `avg_packet_size_bytes`
   - `max_packet_size_bytes`
3. 资源诊断与恢复/长稳验证不再保留独立 `acceptance.py` 入口；需要时直接组合 benchmark/profiler smoke 与定向 gtest。
4. 性能指标体系已经历多轮观测补强，当前 `include/asyncdownload/performance_metrics.hpp`、`include/asyncdownload/types.hpp`、`src/main.cpp`、`scripts/performance/performance_common.py` 与相关测试之间存在明显的并行维护面。
5. 当前导出链路混合了 3 类内容：长期 keeper 指标、为某一轮机制闭环而补的诊断指标、以及 latency/pause duration 这类采样摘要；本线程首先要把这几类边界重新理清。
6. `src/main.cpp` 的 summary 输出与 `scripts/performance/performance_common.py` 的 `SUMMARY_SPECS` 已经承载很大的扁平字段面，新增或删除一个指标会同时牵动 CLI、Python 聚合、测试和文档，不适合继续无约束扩张。
7. 旧的 queue/backpressure 分析已经形成一批机制层结论，例如 explicit producer 预分配修正应保留、pre-high memory pause 触发链已闭环、多个低侵入实验已 reject；本线程不应默认重新打开这些已闭环结论。
8. 当前最主要的维护风险不是下载正确性，而是 benchmark schema 连续性、历史结果可比性、文档解释一致性，以及误删仍有解释价值的指标后导致后续线程重新补同类观测。

## 4. 本线程默认主目标

如果后续没有新的目标指定，本 thread 默认采用以下主目标：

1. 清理当前性能指标体系，区分哪些指标应作为长期保留的正式观测面，哪些只是某轮闭环用过的临时诊断面。
2. 在不破坏现有 benchmark 输出语义、历史基线可比性和关键测试的前提下，收敛指标定义、summary 导出和 Python schema 的维护面。
3. 为后续性能线程固定一套更清晰的指标维护规则，让“新增一个指标”或“删除一个指标”都能明确知道需要同步检查哪些层。

这意味着当前默认优先级是：

1. 盘点与分类
2. 收敛与清理
3. 再判断是否需要 schema 级调整

不是直接把“更高吞吐”或“更低 pause”当成当前线程的第一目标。

## 5. 工作边界

后续在这个 thread 里继续推进时，默认遵守以下边界：

1. 不把 queue/backpressure 语义改造、flush 策略实验或新的 keeper 优化重新拉回本线程作为主任务，除非指标清理直接暴露出明确的正确性缺陷。
2. 不因为某个指标最近没有被提到，就直接把它视为可删项；删除或降级前，先核实它是否仍被 benchmark、文档、测试或历史比较使用。
3. 不默认改动现有外部 summary key 名称；若确实需要改名或删除，应先形成明确的迁移理由，并同步更新相关文档与验证链路。
4. 不把“指标数量多”本身当成充分理由；清理的依据必须落到具体口径重复、解释价值下降、维护成本过高或导出语义不清。
5. 若某个指标的存在意义依赖第三方库契约或失败路径解释，仍须先核实源码或官方契约，再决定是否收敛或删除。

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

3. 定向 summary 导出验证

```powershell
build\tests\Release\AsyncDownload_tests.exe --gtest_filter=DownloadIntegrationTest.ReportsDetailedProgressSnapshot
```

4. 若本轮改动影响 CLI summary key 或 Python 聚合 schema，再补最小 benchmark 导出验证

```powershell
python scripts/performance/benchmark.py --url "http://127.0.0.1:4287/1gb_files.zip" --benchmark-suite regression_v2 --case-list baseline_default,balanced_candidate,memory_guard,scheduler_stress --repeats 3 --label "metrics-cleanup-smoke"
```

5. 只有在指标清理改变了 profiler 解释口径时，才单独补 profiler 对照

```powershell
python scripts/performance/profiler.py --url "http://127.0.0.1:4287/1gb_files.zip" --benchmark-suite regression --case-list throughput_candidate,scheduler_stress --label "metrics-cleanup-profile"
```

## 7. 推荐的起步检查点

如果下一步继续进入实现，建议先围绕以下问题展开：

1. 当前导出指标里，哪些是正式 benchmark 对比长期需要保留的 keeper 字段，哪些只是为迭代 `006` 到 `016` 的局部机制闭环服务。
2. `performance_metrics.hpp`、`PerformanceSummary`、`write_summary()`、`SUMMARY_SPECS` 和测试断言之间，哪些字段已经形成重复维护但没有额外解释收益。
3. 对于任何候选删减项，当前文档、历史 benchmark 结果和测试是否仍然依赖它来解释 queue/backpressure、memory pause 或 persistence 慢态。
4. 当前是否需要新增更强的“指标分层规则”或导出维护约束，而不是单纯继续在现有扁平结构上做增删。
5. 若最终判断某些字段应当降级、合并或移出正式导出链，应该如何保证回归脚本、历史对照和文档解释仍然连续。

## 8. 初始化结论

这个新的性能指标清理 thread 已完成起步基线固定：

- 新 `thread_key` 已确定
- 当前正式文档起点、代码入口和验证入口已固定
- 默认主目标已明确为“先清理和收敛指标体系，再决定是否需要 schema 级调整”
- 当前线程默认采用 compatibility-first 原则，不直接以改名或删字段作为起手动作
- 上一轮 queue/backpressure 优化线程形成的机制层结论默认保留，本线程主要处理观测面与维护面的收敛

后续可以直接从这份初始化文档进入指标盘点、分层分类和导出链路清理。

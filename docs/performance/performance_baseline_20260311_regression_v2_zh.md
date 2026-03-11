# AsyncDownload 性能基线快照（2026-03-11，regression_v2）

## 1. 目的

这份文档用于固化聚合缓冲优化落地之后的新正式性能基线。

从这一版开始，后续继续做代码优化、防劣化检查、before/after 对比时，主 benchmark 基线优先参考 `regression_v2`，而不是继续沿用聚合前的 `regression` 结论。

说明：

- 这份文档保存的是已经整理过的关键结果，即使 `build/` 目录后续被清理，基线数字仍然可追溯。
- 旧文档 [performance_baseline_20260310_regression_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/performance_baseline_20260310_regression_zh.md) 继续保留，但其主要作用已经变成“聚合前历史基线”。

## 2. 基线来源

当前正式 `regression_v2` 基线数据来自：

- `build/benchmarks/20260311_211702_post-aggregation-regression-v2`

关键辅助判断来自：

- `build/benchmarks/20260311_181340_post-aggregation-release`
- `build/benchmarks/20260311_151155_pre-change-release-40`

行为级 profiler 参考基线见：

- [profiler_behavior_baseline_20260311_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/profiler_behavior_baseline_20260311_zh.md)

优化方案说明见：

- [transfer_handle_packet_aggregation_plan_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/transfer_handle_packet_aggregation_plan_zh.md)

## 3. 测试环境与入口

- URL: `http://127.0.0.1:4287/1gb_files.zip`
- 文件大小: `1073741824 bytes`
- 场景: `loopback_external_url`
- CLI: `build/src/Release/AsyncDownload.exe`
- benchmark 入口:

```powershell
python scripts\performance\benchmark.py --url "http://127.0.0.1:4287/1gb_files.zip" --benchmark-suite regression_v2 --repeats 20 --label "post-change"
```

- 当前 benchmark 默认 `inter-run-delay-ms = 500`

说明：

- 所有正式性能回归判断都必须使用 `Release`。
- `regression` 保留为历史对照套件。
- `regression_v2` 是聚合后继续优化时的主回归套件。

## 4. regression_v2 套件定义

当前 `regression_v2` 包含以下 case：

1. `baseline_default`
   `conn=4, queue=1024, window=4MiB`
2. `throughput_candidate`
   `conn=16, queue=1024, window=4MiB`
3. `balanced_candidate`
   `conn=16, queue=256, window=4MiB, gap=32MiB`
4. `deep_buffer_candidate`
   `conn=16, queue=4096, window=4MiB, gap=64MiB`
5. `memory_guard`
   `conn=4, queue=64, backpressure=64/32MiB`
6. `scheduler_stress`
   `conn=8, queue=1024, window=16MiB`
7. `queue_backpressure_stress`
   `conn=4, queue=4096, backpressure=64/32MiB`
8. `gap_tolerance_probe`
   `conn=16, queue=1024, window=4MiB, gap=64MiB`

这些参数是在聚合后重新缩放 `queue_capacity_packets` 得到的，目的是让 queue 的字节预算重新回到更接近聚合前的比较尺度。

## 5. 当前正式基线数据

以下数值均来自 `20260311_211702_post-aggregation-regression-v2/aggregated_cases.csv` 的中位值。

| Case | Avg Net MB/s | Gain vs Baseline | Max Memory Bytes | Total Pause Count | Queue Full Pause Count | 结论 |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| `baseline_default` | 573.21 | 0.00% | 65,387,689 | 215,715.5 | 215,715.5 | 当前 `regression_v2` 默认锚点 |
| `throughput_candidate` | 598.50 | 4.41% | 66,583,376.5 | 158,960 | 158,960 | 只有小幅收益，且高内存 |
| `balanced_candidate` | 674.60 | 17.69% | 17,523,154 | 261,103 | 261,103 | 当前最值得关注的综合平衡点 |
| `deep_buffer_candidate` | 573.93 | 0.13% | 66,304,141 | 197,699 | 197,699 | 深缓冲几乎无收益，不值得主推 |
| `memory_guard` | 752.69 | 31.31% | 4,197,196 | 261,119.5 | 261,119.5 | 当前最强低内存高收益路径 |
| `scheduler_stress` | 705.24 | 23.03% | 66,846,247 | 142,811.5 | 142,811.5 | 不再是坏点，聚合后已显著改善 |
| `queue_backpressure_stress` | 609.21 | 6.28% | 65,871,401 | 4 | 4 | 证明深队列不是主要收益来源 |
| `gap_tolerance_probe` | 610.32 | 6.47% | 66,421,055 | 193,283.5 | 193,283.5 | 收益有限，仍不是主目标 |

补充的公共形态指标：

- `avg_packet_size_bytes`：大多约 `64.5KB`
- `max_packet_size_bytes`：`65536`
- `packets_enqueued_total`：大多约 `16640`

这说明聚合后的 packet 形态已经稳定切换到 `64KB` 级别。

## 6. 当前已确认结论

### 6.1 优化方向被证实

- “小 packet 太多”确实是主热点之一。
- `TransferHandle` 聚合缓冲把 packet 从约 `16KB` 放大到约 `64KB` 后：
  - `packets_enqueued_total` 显著下降
  - 吞吐整体提升
  - profiler 中怀疑的 packet 分配/拷贝热点方向被命中

### 6.2 旧 regression 结论不能直接平移

- 聚合不仅降低了 per-packet 开销，也改变了 `queue_capacity_packets` 的实际字节语义。
- 因此聚合后的旧 `regression` 结果不能直接作为后续唯一主基线。
- `regression_v2` 的存在就是为了把比较尺度重新拉回可解释状态。

### 6.3 当前最值得继续围绕的 case

当前更值得作为后续优化核心参考的是：

1. `baseline_default`
2. `balanced_candidate`
3. `memory_guard`
4. `scheduler_stress`

其中：

- `balanced_candidate` 代表高吞吐与较低内存的平衡点
- `memory_guard` 代表当前最强的低内存高收益路径
- `scheduler_stress` 代表聚合后被明显修复、但仍值得继续关注稳定性的路径

### 6.4 当前不建议继续围绕的 case

以下 case 暂时不适合作为主优化目标：

- `deep_buffer_candidate`
- `throughput_candidate`
- `gap_tolerance_probe`

原因：

- 收益有限
- 或收益与内存代价不匹配
- 或仍然不能证明它们比 `balanced_candidate` / `memory_guard` 更值得投资

## 7. 当前 pause 指标的判断

聚合后，`queue_full_pause_count` 相比聚合前已经明显下降，但仍然偏高。

例如：

- `baseline_default`: `293,908 -> 215,715.5`
- `throughput_candidate`: `307,593.5 -> 158,960`
- `scheduler_stress`: `373,634.5 -> 142,811.5`

这说明：

- 第一阶段优化已经有效
- 但系统仍然在较大程度上依赖 `queue_full_pause` 进行调节

因此，当前状态可以接受为“第一阶段成果”，但不应视为最终状态。

## 8. 后续优化时的对比优先级

每次继续做代码优化后，优先看以下顺序：

1. `baseline_default`
2. `balanced_candidate`
3. `memory_guard`
4. `scheduler_stress`
5. `throughput_candidate`
6. `queue_backpressure_stress`
7. `gap_tolerance_probe`
8. `deep_buffer_candidate`

原因：

- 先看默认路径和主平衡点
- 再看低内存收益是否保持
- 再看被修复后的压力路径是否继续改善
- 最后看那些已经被降级为次要参考的组合

## 9. 当前版本结论

截至 `2026-03-11`：

- `regression_v2` 已经可以作为后续继续优化时的主 benchmark 基线
- `regression` 继续保留为聚合前后的历史对照，不应删除
- 第 1 次聚合优化方向是正确的
- 当前最值得继续优化的目标是：
  - 降低高 pause churn
  - 保住 `balanced_candidate` 的吞吐优势
  - 保住 `memory_guard` 的低内存优势
  - 继续改善 `scheduler_stress` 的稳定性和效率

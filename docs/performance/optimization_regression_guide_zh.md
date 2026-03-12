# AsyncDownload 代码优化防劣化指南

## 1. 目的

这份指南用于约束后续代码优化的验证方式，避免“局部吞吐变快，但整体行为变差”。

本指南中的所有回归判断都基于 `Release` 版本可执行文件，不再使用 `Debug` 数据作为正式对比基线。

当前建议优先使用的固定回归套件：

- `python scripts/performance/benchmark.py --benchmark-suite regression_v2`

历史对照套件：

- `python scripts/performance/benchmark.py --benchmark-suite regression`

当前正式核心参考数据来自：

- `build/benchmarks/20260311_211702_post-aggregation-regression-v2`

当前 profiler 行为基线参考文档：

- `docs/performance/profiler_behavior_baseline_20260311_zh.md`

说明：

- benchmark 用于正式性能回归判断。
- benchmark 结果应作为性能优化方向的主驱动依据。
- profiler 用于热点与行为路径对比，不用于单独给出指导性性能结论。
- profiler 结果不能替代 benchmark 结果做吞吐基线判断。
- 如果 benchmark 还没有说明收益或退化趋势，不应先凭 profiler 下优化方向结论。
- benchmark 与 profiler 应串行执行，不要在同一轮 benchmark 执行期间同时开启 profiler。
- `regression_v2` 是聚合后继续迭代时的主回归套件。
- `regression` 保留为聚合前后的历史对照套件，不要覆盖旧结论。
- 新正式基线快照见 [performance_baseline_20260311_regression_v2_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/performance_baseline_20260311_regression_v2_zh.md)。
- 第一次性能优化记录见 [performance_optimization_history_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/performance_optimization_history_zh.md)。

## 2. 优化后的验证原则

每次做完一批代码改动后，不要直接凭单个 case 的吞吐判断“优化成功”，而是按下面的顺序看：

1. 默认基线是否退化
2. 平衡型 case 是否提升
3. 高吞吐 case 是否提升
4. 内存保护 case 是否仍成立
5. 高波动 canary 是否更稳定

补充原则：

1. 先跑 benchmark，再决定是否需要 profiler
2. benchmark 负责回答“有没有收益、有没有退化、主要影响了哪些 case”
3. profiler 负责回答“热点在哪里、热点有没有迁移、哪条路径值得继续查”
4. 不要一边跑 benchmark 一边开 profiler
5. 如果需要 profiler，应在 benchmark 结束并完成初步判断后单独执行

## 3. 固定验证流程

### 3.1 改动前

保存一轮基准结果，推荐：

```powershell
python scripts\performance\benchmark.py --url "http://127.0.0.1:4287/1gb_files.zip" --benchmark-suite regression_v2 --repeats 20 --label "pre-change"
```

要求：

- 使用 `Release` 可执行文件
- 如果不显式传 `--exe`，当前 benchmark 默认已指向 `build/src/Release/AsyncDownload.exe`

### 3.2 改动后

使用同一台机器、同一 URL、同一 repeats 再跑一轮：

```powershell
python scripts\performance\benchmark.py --url "http://127.0.0.1:4287/1gb_files.zip" --benchmark-suite regression_v2 --repeats 20 --label "post-change"
```

说明：

- 当前正式 `regression_v2` 基线已经用 `20` 次复测校准。
- 如果后续结果接近阈值，建议继续按相同 suite 补高重复次数确认。
- 日常优化回归建议先用 `20` 次。
- 如果结果接近阈值、或者涉及高波动路径，再补到 `40` 次。

### 3.3 对比时必须看这些指标

每个 case 至少看以下 5 个指标：

- `avg_network_speed_mb_s_median`
- `max_memory_bytes_median`
- `max_inflight_bytes_median`
- `total_pause_count_median`
- `wall_clock_duration_ms_median`

如果只看吞吐，不看内存、inflight、pause，很容易把“堆积更多数据”误判成优化。

### 3.4 benchmark 与 profiler 的执行顺序

建议固定按下面顺序执行：

1. 改动前 benchmark
2. 改动后 benchmark
3. 先根据 benchmark 判断：
   - 是否确实有收益
   - 是否出现退化
   - 哪些 case 值得继续深挖
4. 只有在需要解释热点、确认热点迁移、或者决定下一刀优化方向时，再单独跑 profiler

换句话说：

- benchmark 是主流程
- profiler 是解释和确认流程
- profiler 不应与 benchmark 混跑

## 4. 推荐的判定规则

### 4.1 默认基线 `baseline_default`

重点：

- 默认行为是否变差
- 默认内存/暂停是否失控

建议判定：

- 吞吐下降 `> 5%`：需要调查
- 内存上涨 `> 15%` 且吞吐无明显提升：视为可疑
- pause 上涨 `> 15%`：需要调查

补充：

- 当前 `regression_v2` 下，`baseline_default` 是正式默认锚点，但不再是唯一最快路径。

### 4.2 平衡点 `balanced_candidate`

重点：

- 它是当前最值得优先维护的高吞吐低内存平衡点之一

建议判定：

- 吞吐提升 `> 5%` 且内存变化可控：可以认为优化有效
- 吞吐不变，但内存下降 `> 15%`：也可以认为优化有效
- 吞吐下降 `> 5%`：优先阻止合入

补充：

- 当前更重要的是保护它相对 `baseline_default` 的吞吐优势和低内存特性。

### 4.3 高吞吐路线

适用 case：

- `throughput_candidate`
- `deep_buffer_candidate`
- `gap_tolerance_probe`

重点：

- 判断高并发路线是否仍有必要保留
- 判断代价是否失控

建议判定：

- 吞吐提升 `> 5%` 且内存/inflight 没有明显恶化：正向优化
- 吞吐提升 `< 5%` 但内存上涨 `> 20%`：不建议接受
- 吞吐下降 `> 7%`：通常视为明显退化

### 4.4 低内存保护路线

适用 case：

- `memory_guard`

重点：

- 保护性路径是否被破坏
- 低内存路径是否继续保持当前优势

建议判定：

- 吞吐小幅波动可以接受
- 内存如果明显偏离当前 `~4.2 MB` 水平，需要重点检查
- 如果内存翻倍但吞吐没有实质提升，应视为退化

### 4.5 高波动 canary

适用 case：

- `scheduler_stress`

重点：

- 不要求它绝对最快
- 更关注是否变得更稳定

建议判定：

- 优先看 `avg_network_speed_mb_s_stdev`
- 优先看 `wall_clock_duration_ms_stdev`
- 优先看最低值是否抬升

如果中位值没变，但低谷变少、波动收敛，也算正向变化。

补充：

- `scheduler_stress` 在 `regression_v2` 下不再是旧坏点，但仍然值得持续观察其稳定性和内存代价。
- `gap_tolerance_probe` 仍然是次级探针，不是当前主收益点。

## 5. 哪些变化不能直接当作优化

以下情况不要直接判断为“优化成功”：

- 吞吐略升，但 `max_memory_bytes` 明显上升
- 吞吐略升，但 `max_inflight_bytes` 明显上升
- 吞吐略升，但 `total_pause_count` 爆炸
- 只有单个 case 提升，其他 case 普遍退化
- 只在 `deep_buffer_candidate` 提升，但默认基线和 balanced case 都变差

## 6. 哪些变化值得优先接受

以下类型的变化通常更有价值：

- `baseline_default` 和 `balanced_candidate` 同时提升
- `memory_guard` 与 `balanced_candidate` 同时成立
- `scheduler_stress` 波动显著收敛
- pause 降低，而吞吐持平或略升
- 内存下降，而吞吐持平或略升

## 7. 建议的防劣化门槛

当前项目还处于性能探索阶段，因此门槛不要定得过死，但可以先用以下经验线：

### 7.1 建议阻止合入的情况

- `baseline_default` 吞吐下降 `> 5%`
- `balanced_candidate` 吞吐下降 `> 5%`
- `memory_guard` 内存显著上升且没有收益
- 多个 case 的 pause 同时明显恶化

### 7.2 建议继续观察的情况

- 吞吐变化在 `±5%` 内
- 某些 case 变快，但另一些 case 轻微变慢
- `scheduler_stress` 中位值变化不大，但波动有变化
- `gap_tolerance_probe` 提升，但最低值或时长尾部变差

### 7.3 建议认为优化成立的情况

- `baseline_default` 与 `balanced_candidate` 同向提升
- 或吞吐持平，但默认/平衡 case 内存明显下降
- 或吞吐持平，但 pause 和时延明显下降

## 8. 每次优化后的检查清单

1. benchmark 是否用的是同一 URL、同一 suite、同一 repeats
2. 默认基线是否退化
3. `balanced_candidate` 是否变好
4. `memory_guard` 是否仍然保持高吞吐且低内存
5. 高并发 case 是否只是“用更高内存换速度”
6. `scheduler_stress` 是否继续改善
7. `gap_tolerance_probe` 是否出现异常尾部
8. 如果结果接近噪声区间，是否需要追加重复次数

## 9. 如果结果不明确怎么办

如果前后变化落在噪声区间内，不要立刻下结论，建议：

1. 追加 repeats 到 `10`
2. 保持同一时间段连续测试
3. 重点复看：
   - `baseline_default`
   - `balanced_candidate`
   - `memory_guard`
   - `scheduler_stress`

如果问题落在高波动或尾部异常上，建议直接追加到 `20` 或 `40`，而不是只补到 `10`。

## 10. 当前建议

当前阶段最适合的优化目标不是单纯追求更高峰值吞吐，而是优先追求：

1. `baseline_default` 不退化
2. `balanced_candidate` 不退化并尽量继续提升
3. `memory_guard` 不丢掉低内存优势
4. `scheduler_stress` 继续稳定

这四件事同时满足，才更像是对下载器真实质量有价值的优化。

补充结论：

- 后续所有复测都要使用 `Release` 版本。
- `baseline_default` 现在是正式主锚点。
- `memory_guard` 与 `queue_backpressure_stress` 适合作为保守路径参考点。
- `throughput_candidate`、`balanced_candidate`、`deep_buffer_candidate` 更适合作为“高并发路径修复进度”观察点。
- `gap_tolerance_probe` 适合作为 gap 路径风险观察点。

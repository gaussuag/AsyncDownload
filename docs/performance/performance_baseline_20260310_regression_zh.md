# AsyncDownload 性能基线快照（2026-03-10）

## 1. 目的

这份文档用于固化当前代码版本在本地回环环境下的可复用性能基线，作为后续代码优化前后的对比依据。

这份基线不是“理论最优值”，而是当前仓库、当前机器、当前本地服务器、当前 benchmark 工具版本下的一组可重复参考值。

## 2. 基线来源

当前正式 `Release regression` 基线数据来自：

- `build/benchmarks/20260311_113724_pre-change-release-retest`
- `build/benchmarks/20260311_151155_pre-change-release-40`

关键辅助判断来自以下 sweep 结果：

- `build/benchmarks/20260311_103039_pre-change`
- `build/benchmarks/20260310_184639_retest-core-cases-40`

## 3. 测试环境

- URL: `http://127.0.0.1:4287/1gb_files.zip`
- 文件大小: `1073741824 bytes`
- 场景: `loopback_external_url`
- benchmark 入口: `python scripts/performance/benchmark.py --benchmark-suite regression`
- 当前正式核心基线 repeats: `20`
- 当前更高重复次数确认样本 repeats: `40`
- CLI: `build/src/Release/AsyncDownload.exe`
- 平台: Windows 10

说明：

- 之前基于 `Debug` 版本采集的回归数据仅保留为历史参考，不再作为正式性能基线。
- 从当前版本开始，所有性能复测、before/after 对比、回归判断都必须使用 `Release` 可执行文件。

## 4. 当前固定回归套件

当前 `regression` 套件包含以下 case：

1. `baseline_default`
   默认参数，作为所有改动前后的锚点。
2. `throughput_candidate`
   `max_connections=16, scheduler_window_bytes=4MiB`
3. `balanced_candidate`
   `max_connections=16, scheduler_window_bytes=4MiB, queue_capacity_packets=1024, max_gap_bytes=32MiB`
4. `deep_buffer_candidate`
   `max_connections=16, scheduler_window_bytes=4MiB, queue_capacity_packets=16384, max_gap_bytes=64MiB`
5. `memory_guard`
   `max_connections=4, queue_capacity_packets=256, backpressure=64/32MiB`
6. `scheduler_stress`
   `max_connections=8, scheduler_window_bytes=16MiB`
7. `queue_backpressure_stress`
   `queue_capacity_packets=16384, backpressure=64/32MiB`
8. `gap_tolerance_probe`
   `max_connections=16, scheduler_window_bytes=4MiB, max_gap_bytes=64MiB`

## 5. 当前正式基线数据

以下表格分两层：

- `正式主基线`：来自 `20260311_113724_pre-change-release-retest/aggregated_cases.csv`
- `40 次确认样本`：来自 `20260311_151155_pre-change-release-40/aggregated_cases.csv`

两组数值都使用中位值。

| Case | 主基线 Avg Net MB/s | 40 次确认 Avg Net MB/s | 主基线 Max Memory Bytes | 40 次确认 Max Memory Bytes | 主基线 Total Pause Count | 40 次确认 Total Pause Count | 说明 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| `baseline_default` | 502.74 | 534.32 | 25,117,763 | 20,586,090 | 280,394.5 | 293,908 | 当前默认基线，也是当前最快组之一 |
| `throughput_candidate` | 346.38 | 356.98 | 51,189,798 | 45,597,889 | 325,568 | 307,593.5 | 高并发候选，但当前 Release 下仍明显落后于 baseline |
| `balanced_candidate` | 333.42 | 355.50 | 18,678,779 | 18,966,437 | 355,854.5 | 331,618 | 低内存，但当前 Release 下吞吐不占优 |
| `deep_buffer_candidate` | 342.76 | 349.37 | 45,358,637.5 | 45,589,159 | 333,625.5 | 324,872.5 | 吞吐不占优，且内存代价更高 |
| `memory_guard` | 474.86 | 542.43 | 4,581,016 | 4,638,400 | 271,057 | 265,552 | 低内存保护路径，当前 Release 下稳定接近或略高于 baseline |
| `queue_backpressure_stress` | 450.70 | 501.11 | 21,235,906 | 25,002,379 | 306,016.5 | 278,661.5 | 第二梯队，说明保守并发/背压路径仍较强 |
| `scheduler_stress` | 311.25 | 324.87 | 31,384,067 | 33,514,114.5 | 409,265 | 373,634.5 | 当前最差且最不稳定的 canary |
| `gap_tolerance_probe` | 333.12 | 345.72 | 49,454,062 | 45,366,927 | 383,185 | 312,889.5 | 高 gap 路径没有吞吐优势，但 `40` 次样本里极端尾部没有上一轮那么夸张 |

说明：

- 当前正式主基线仍以 `20` 次样本为准。
- `40` 次样本的作用是确认方向是否稳定，而不是替代主基线目录。
- 这两轮 `Release` 数据都已经覆盖 `memory_guard` 与 `queue_backpressure_stress`，后续不再依赖旧 `Debug` 结果补位。

## 6. 已确认的稳定结论

这些结论来自三轮 `Release regression` 数据交叉确认，后续优化时可以直接作为判断背景。

### 6.1 当前 Release 路径结论

- 当前 `Release` 下，`baseline_default`（4 连接）稳定快于 `16` 连接的高并发候选。
- `memory_guard`（4 连接、低队列）也稳定接近 baseline。
- `queue_backpressure_stress`（4 连接、深队列）仍然高于所有 `16` 连接 case。
- 这说明当前正式性能基线下，最优区间已经不再是之前 `Debug` 观察到的 `16` 连接路线。

参考：

- `20260311_103039_pre-change`
- `20260311_113724_pre-change-release-retest`
- `20260311_151155_pre-change-release-40`

### 6.2 高并发路径结论

- `throughput_candidate`、`balanced_candidate`、`deep_buffer_candidate`、`gap_tolerance_probe` 在 `Release` 下整体落在 `333-346 MB/s`，全部明显低于 baseline。
- `scheduler_stress` 仍然是最差路径。
- `gap_tolerance_probe` 在 `Release` 下整体仍然没有明显收益。
- 第二轮 `20` 次复测里它出现了严重尾部慢跑样本，但 `40` 次确认样本里没有同等级别的极端尾部。
- 因此当前更准确的表述是：它仍然是风险探针，但极端慢跑未稳定复现。

### 6.3 队列与内存结论

- `memory_guard` 继续证明低队列、低内存不是当前主瓶颈。
- `deep_buffer_candidate` 再次说明更深缓冲没有换来更高吞吐。
- 当前更像是高并发路径存在调度或收敛成本，而不是“队列不够深”。

### 6.4 gap 路径结论

- `gap_tolerance_probe` 三轮 `Release` 都没有体现出高于 baseline 的收益。
- `20` 次复测里它出现过明显极端慢跑，但 `40` 次确认样本没有复现同级别异常。
- 因此它依然应被视为风险探针，但不再把“极端慢跑稳定必现”当作已确认结论。

## 7. 如何解读这些基线

### 7.1 哪些 case 用来判断“默认性能”

- `baseline_default`

用途：

- 判断默认配置是否退化
- 判断基础吞吐、pause、内存占用是否异常

### 7.2 哪些 case 用来判断“高并发路径是否改善”

- `throughput_candidate`
- `balanced_candidate`
- `deep_buffer_candidate`
- `gap_tolerance_probe`

用途：

- 判断高并发路径是否被修复
- 判断提升是否只是以更高内存和更高 inflight 为代价

### 7.3 哪些 case 用来判断“保护性行为是否退化”

- `memory_guard`
- `queue_backpressure_stress`

用途：

- 判断内存保护是否被破坏
- 判断队列/背压路径是否出现新的 pause 爆炸

### 7.4 哪些 case 用来判断“高波动路径是否更稳定”

- `scheduler_stress`

用途：

- 不把它当作性能目标值
- 把它当作不稳定路径的告警器
- 重点看中位值、最低值、时长波动，而不是只看平均速度

## 8. 当前噪声范围

从当前三轮 `Release regression` 看，本地回环环境仍存在非忽略漂移。

默认 baseline 在不同轮次中的中位吞吐大致落在：

- `473 MB/s` 到 `534 MB/s`

因此当前可以先采用以下经验判断：

- 变化 `< 5%`：视为不够确定，优先看更多重复或更多 case 是否同向变化
- 变化 `5% - 10%`：可能是真变化，但需要结合内存、pause、inflight 一起判断
- 变化 `> 10%`：通常已经具有较强可信度
- 变化 `> 15%`：基本可以视为明确变化

补充：

- `Release` 下 `gap_tolerance_probe` 已经出现过非常明显的异常慢跑尾部。
- 但 `40` 次确认样本没有复现同等级别极端值。
- 因此后续 before/after 对比应优先使用中位值，不要用单次最差值直接下结论。

## 9. 当前推荐的对比优先级

每次代码改动后，优先看以下顺序：

1. `baseline_default`
2. `memory_guard`
3. `queue_backpressure_stress`
4. `throughput_candidate`
5. `balanced_candidate`
6. `deep_buffer_candidate`
7. `scheduler_stress`
8. `gap_tolerance_probe`

原因：

- 先看默认路径是否退化
- 再看低内存和保守背压路径是否受损
- 再看高并发路径有没有被修复
- 最后看高波动和 gap 风险路径有没有继续恶化

## 10. 当前版本结论

截至 `2026-03-11`：

- `Release regression` 套件已经可以作为后续优化前后的固定正式回归基准
- 所有后续复测都必须使用 `Release` 可执行文件
- `baseline_default` 是当前正式主基线
- `memory_guard` 与 `queue_backpressure_stress` 是当前第二梯队参考点
- 高并发 `16` 连接路径当前整体不占优，应视为待修复对象
- `scheduler_stress` 与 `gap_tolerance_probe` 是当前最值得警惕的风险探针
- `20260311_151155_pre-change-release-40` 已进一步确认上述 `Release` 方向稳定成立

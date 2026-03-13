# AsyncDownload 性能优化记录

## 1. 目的

这份文档用于记录已经发生过的性能优化迭代，包括：

- 优化前的基线形态
- 做了什么改动
- 为什么这样改
- 优化后的结果如何解读

它不是“当前正式基线文档”的替代品，而是性能演进历史。

当前正式 benchmark 基线请同时参考：

- [performance_baseline_20260310_regression_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/performance_baseline_20260310_regression_zh.md)
- [performance_baseline_20260311_regression_v2_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/performance_baseline_20260311_regression_v2_zh.md)

## 2. 优化迭代 001：TransferHandle 聚合缓冲

### 2.1 背景

在第一次 profiler 基线中，两个热点方向比较明确：

1. 网络回调里的 packet 分配与 payload 拷贝
2. `PersistenceThread -> handle_data_packet -> append_bytes -> FileWriter::write` 这条 per-packet 消费链

当时 benchmark 与 profiler 一起指向的问题是：

- 平均 packet 大小只有约 `16KB`
- `packets_enqueued_total` 约 `65,800 / GiB`
- 多个核心 case 都存在很高的 `queue_full_pause_count`

结论是：

- 当前下载器正确性设计没有问题
- 但“每 callback 一包”的粒度太细
- 小 packet 数量过多放大了：
  - 分配/释放
  - payload 拷贝
  - 入队/出队
  - 持久化线程 per-packet 固定成本

### 2.2 技术方案

方案说明文档见：

- [transfer_handle_packet_aggregation_plan_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/transfer_handle_packet_aggregation_plan_zh.md)

本次落地方案是：

- 在 `TransferHandle` 上增加本地聚合缓冲
- 不再每次 `libcurl` callback 立刻生成 `DataPacket`
- 先把 callback 数据追加到本地缓冲
- 达到约 `64KB` 或在完成/暂停/清理等关键时机时，再 flush 成一个 `DataPacket`

实现边界：

- 没有改 `PersistenceThread` 接口语义
- 没有改恢复链路
- 没有改 `DataPacket` 对外模型
- 第一版重点是降低 packet 数，而不是引入复杂对象池或零拷贝

### 2.3 涉及代码

主要改动文件：

- [download_engine.cpp](/D:/git_repository/coding_with_agents/AsyncDownload/src/download/download_engine.cpp)

核心改动方向：

- `TransferHandle` 新增本地聚合状态
- callback 先 append 到 handle 缓冲
- 到阈值或关键生命周期节点时再 flush 入队
- 预留 `64KB` 缓冲容量，尽量避免正常路径频繁扩容

### 2.4 优化前基线（聚合前，Release regression）

参考目录：

- `build/benchmarks/20260311_151155_pre-change-release-40`

代表性指标如下：

| Case | Avg Net MB/s | Max Memory Bytes | Total Pause Count | Packets Enqueued | Avg Packet Size Bytes |
| --- | ---: | ---: | ---: | ---: | ---: |
| `baseline_default` | 534.32 | 20,586,090 | 293,908 | 65,804 | 16,317.27 |
| `throughput_candidate` | 356.98 | 45,597,889 | 307,593.5 | 65,792 | 16,320.25 |
| `balanced_candidate` | 355.50 | 18,966,437 | 331,618 | 65,792 | 16,320.25 |
| `memory_guard` | 542.43 | 4,638,400 | 265,552 | 65,805 | 16,317.02 |
| `scheduler_stress` | 324.87 | 33,514,114.5 | 373,634.5 | 65,603 | 16,367.27 |

当时的主要判断：

- 高并发路径整体不占优
- `scheduler_stress` 是明显坏点
- `queue_full_pause` 是主问题之一

### 2.5 第一次优化后的直接结果（旧 regression 语义下）

参考目录：

- `build/benchmarks/20260311_181340_post-aggregation-release`

这一轮最重要的变化是：

| Case | Avg Net MB/s | Max Memory Bytes | Total Pause Count | Packets Enqueued | Avg Packet Size Bytes |
| --- | ---: | ---: | ---: | ---: | ---: |
| `baseline_default` | 605.74 | 65,821,926.5 | 199,010.5 | 16,641.5 | 64,521.94 |
| `throughput_candidate` | 650.93 | 66,396,296.5 | 174,519 | 16,640 | 64,527.75 |
| `balanced_candidate` | 598.32 | 66,281,576.5 | 168,261.5 | 16,640 | 64,527.75 |
| `memory_guard` | 596.75 | 16,293,297 | 274,729.5 | 16,641 | 64,523.88 |
| `scheduler_stress` | 703.54 | 66,535,103.5 | 140,928 | 16,448 | 65,281.00 |

可以确认的结果：

- packet 粒度已经成功从 `16KB` 放大到 `64KB` 级
- `packets_enqueued_total` 大约降到了原来的 `1/4`
- 吞吐整体显著提升
- 多数 case 的 pause 也下降了

但这轮结果不能直接作为后续唯一主基线，因为：

- `queue_capacity_packets` 是按“包个数”计，不是按字节计
- packet 变大后，很多 case 的 queue 字节预算被放大了约 `4x`
- 所以这轮里有一部分收益，混入了“背压语义变化”的影响

### 2.6 第一次优化后的正式重定标结果（regression_v2）

参考目录：

- `build/benchmarks/20260311_211702_post-aggregation-regression-v2`

这是聚合后重新缩放 queue 的正式可比版本。

| Case | Avg Net MB/s | Gain vs New Baseline | Max Memory Bytes | Total Pause Count | 结论 |
| --- | ---: | ---: | ---: | ---: | --- |
| `baseline_default` | 573.21 | 0.00% | 65,387,689 | 215,715.5 | 新默认锚点 |
| `throughput_candidate` | 598.50 | 4.41% | 66,583,376.5 | 158,960 | 小幅收益，但不再是主目标 |
| `balanced_candidate` | 674.60 | 17.69% | 17,523,154 | 261,103 | 当前最重要的平衡型收益点 |
| `deep_buffer_candidate` | 573.93 | 0.13% | 66,304,141 | 197,699 | 深缓冲路线基本无价值 |
| `memory_guard` | 752.69 | 31.31% | 4,197,196 | 261,119.5 | 当前最强低内存高收益路径 |
| `scheduler_stress` | 705.24 | 23.03% | 66,846,247 | 142,811.5 | 已被明显修复，不再是旧坏点 |
| `queue_backpressure_stress` | 609.21 | 6.28% | 65,871,401 | 4 | 证明深队列不是主要收益来源 |
| `gap_tolerance_probe` | 610.32 | 6.47% | 66,421,055 | 193,283.5 | 仍不是主收益点 |

### 2.7 这次优化带来的正确结论

被证实正确的方向：

- “小 packet 过多”确实是主热点之一
- 在 `TransferHandle` 上做聚合是正确的第一刀
- profiler 里怀疑的 packet 分配/拷贝热点被命中

被修正的理解：

- 聚合不只是降低了 per-packet 成本
- 它还改变了 queue/backpressure 的有效字节预算
- 因此聚合后的旧 `regression` 结果不能直接拿来当长期主基线

### 2.8 第一次优化后的新策略

在这次优化之后，后续继续优化时应采用：

- `regression_v2` 作为主 benchmark 套件
- `regression` 作为历史对照套件

新的主要目标应改为：

1. 保住 `balanced_candidate` 的收益
2. 保住 `memory_guard` 的低内存优势
3. 继续降低高 pause churn
4. 继续观察 `scheduler_stress` 的稳定性与效率

而不再把以下路线作为主方向：

- `deep_buffer_candidate`
- “单纯加深 queue 就会更快”的思路

## 3. 优化迭代 002：PersistenceThread finished 位图增量推进（未采纳）

### 3.1 背景

在第一次优化完成并重定标到 `regression_v2` 之后，profiler 热点已经明显迁移：

- 网络侧的小 packet 问题已经被压下去
- `gap_pause_count` 仍然基本为 `0`
- 主背压信号仍然是 `queue_full_pause`
- 持久化热点继续集中在：
  - `PersistenceThread::handle_data_packet`
  - `PersistenceThread::append_bytes`
  - `FileWriter::write`

在复查 [persistence_thread.cpp](/D:/git_repository/coding_with_agents/AsyncDownload/src/persistence/persistence_thread.cpp)
时，一个可疑点比较突出：

- `update_finished_blocks()` 每次都从 `range.start_offset` 扫到 `range.persisted_offset`
- 如果一个 range 被拆成很多 packet 逐步推进，就会重复扫描已经确认过的 finished 区段

因此这轮实验的假设是：

- 当前持久化链里有一部分成本并不是写盘本身，而是 finished 位图的重复推进
- 如果把 finished 位图改成“增量推进”，有机会进一步降低 `PersistenceThread` 的每 packet 固定成本

### 3.2 实验方案

本轮尝试的实现方向是：

- 在 `RangeContext` 上增加 finished 位图的独立扫描前沿
- 把 `update_finished_blocks()` 改成只推进“上次 finished 前沿之后的新部分”
- 在 `drain_ordered_packets()` 里把连续 packet drain 完之后，再统一推进 finished 位图

这个方案的边界是：

- 不改恢复语义
- 不改 `append_bytes()` 的对齐写盘规则
- 不改 `TransferHandle` 聚合语义
- 目标仅限于削减持久化 bookkeeping 成本

### 3.3 验证结果

基础验证通过：

- `scripts\\build.bat release`
- `ctest -C Release --output-on-failure -E "DownloadIntegrationTest.*"`

profiler 信号是积极的。最终采用的 profiler 结果目录为：

- `build/profiles/20260311_233833_persistence-finished-scan-opt-profile-rerun`

代表性变化如下：

| Case | 旧 profiler 行为基线 | 本轮实验 profiler | 变化 |
| --- | ---: | ---: | --- |
| `throughput_candidate` avg net MB/s | 205.79 | 287.32 | 提升明显 |
| `throughput_candidate` queue_full_pause_count | 900,348 | 808,380 | 有所下降 |
| `scheduler_stress` avg net MB/s | 209.02 | 407.32 | 提升明显 |
| `scheduler_stress` queue_full_pause_count | 1,214,248 | 275,677 | 大幅下降 |

但正式 benchmark 结果没有通过 keeper 判定。串行 benchmark 结果目录为：

- `build/benchmarks/20260311_234146_persistence-finished-scan-opt-serial`
- `build/benchmarks/20260311_234944_persistence-finished-scan-opt-core-serial`

与当前正式基线相比，核心 `regression_v2` case 的收益结构没有保住：

| Case | 当前正式基线 avg net MB/s | 本轮实验 avg net MB/s | 结论 |
| --- | ---: | ---: | --- |
| `baseline_default` | 573.21 | 605.77 | 默认锚点略快，但不是主要判定点 |
| `balanced_candidate` | 674.60 | 564.66 | 明显退化，不可接受 |
| `memory_guard` | 752.69 | 600.93 | 明显退化，不可接受 |
| `scheduler_stress` | 705.24 | 611.35 | 明显退化，不可接受 |

### 3.4 结论

这轮实验最终不采纳，原因不是“完全没收益”，而是：

- profiler 说明它可能确实压低了一部分持久化线程开销
- 但正式 benchmark 说明它没有保住当前 `regression_v2` 的主收益结构
- 尤其 `balanced_candidate`、`memory_guard`、`scheduler_stress` 都明显低于正式基线

因此这轮实验的正确结论是：

- “finished 位图重复推进”可能是次级成本，但它不是当前最值得单独下刀的主瓶颈
- 单独优化这部分 bookkeeping，不能稳定转化为正式 benchmark 的 keeper 收益
- 下一轮更值得优先尝试的，仍然是“持久化消费链批量化”，也就是继续沿着：
  - `PersistenceThread::handle_data_packet`
  - `append_bytes()`
  - `FileWriter::write()`
  这条链，减少连续 packet 场景下的重复处理和碎片化写入

## 4. 优化迭代 003：PersistenceThread staging 批量写盘实验（未采纳）

### 4.1 背景

在补齐持久化链耗时采样之后，新的判断变得更明确：

- `handle_data_packet` / `append_bytes` / `FileWriter::write` 的平均 per-packet 耗时并不夸张
- 更可疑的是 `FileWriter::write` 长尾和 `queue_full_pause` 放大
- 因此下一轮实验不再主打“继续压 bookkeeping”，而是尝试直接减少真实写盘次数

为此，这轮先补了新的观测指标：

- `file_write_calls_total`
- `staged_write_flush_count`
- `staged_write_bytes_total`

这些指标的作用不是替代采样耗时，而是直接回答：

- 真实 `write()` 次数有没有下降
- staging 批量写盘在真实 benchmark 中到底有没有触发

### 4.2 实验方案

本轮尝试的实现方向是：

- 仍然只在 `PersistenceThread` 内处理，不改网络聚合
- 只针对“同一个 range 且 offset 连续”的 packet 尝试本地 staging 合并
- 用一个受控大小的 staging buffer，把多个连续且对齐的 packet 先拼成更大的连续内存，再一次 `FileWriter::write()`

设计边界：

- 不改 `DataPacket` 对外模型
- 不改恢复语义
- 不改 `tail_buffer` 的 4KB 对齐规则
- 目标明确限定为减少 `FileWriter::write()` 调用次数，而不是再次去压轻量 bookkeeping

### 4.3 验证结果

基础验证通过：

- `scripts\\build.bat release`
- `ctest -C Release --output-on-failure -E "DownloadIntegrationTest.*"`

改造前 benchmark 目录：

- `build/benchmarks/20260312_185212_pre-persistence-staged-write-core`

实验版 benchmark 目录：

- `build/benchmarks/20260312_185737_post-persistence-staged-write-core`

核心 case 对比如下：

| Case | 改造前 avg net MB/s | 实验版 avg net MB/s | `queue_full_pause_count` 变化 | `max_memory_bytes` 变化 |
| --- | ---: | ---: | ---: | ---: |
| `baseline_default` | 794.50 | 740.43 | `121,972 -> 118,200` | `-0.24%` |
| `balanced_candidate` | 667.99 | 647.85 | `163,090 -> 216,948.5` | `+1.33%` |
| `memory_guard` | 348.10 | 341.57 | `655,444.5 -> 785,202.5` | `+3.56%` |
| `scheduler_stress` | 583.30 | 566.94 | `152,189 -> 156,143` | `-0.65%` |

更关键的是新的观测指标：

- `file_write_calls_total` 没有出现确定性下降
- `staged_write_flush_count` 在 `40/40` 个 raw run 中全部是 `0`
- `staged_write_bytes_total` 在 `40/40` 个 raw run 中全部是 `0`

这说明实验版虽然在合成单测里可以触发 staging 合并，但在真实 benchmark 路径上几乎没有真正走到这条代码。

### 4.4 结论

这轮实验最终不采纳，并且代码实验已回退；保留的只有新的观测指标。

正确结论是：

- 这次 staging 方案没有命中真实热路径
- 问题不只是“收益不够大”，而是“真实下载路径下几乎根本没触发”
- 当前实现把 batching 落在“当前 packet 命中前沿后，再去 `out_of_order_queue` 里找连续后继”这一层，但 benchmark 结果表明，这不是当前系统里连续 packet 的主要形成位置
- 因此这条具体实现路线不能作为 keeper，也不能继续沿着同一触发点细抠

这轮实验留下的有效资产是：

- `file_write_calls_total`
- `staged_write_flush_count`
- `staged_write_bytes_total`

后续如果继续尝试“减少真实写盘次数”，必须优先回答一个新问题：

- **真实连续 packet 到底是在主消费队列阶段形成，还是在 range 内乱序重排阶段形成？**

在没有先回答这个问题之前，不应再次把类似 staging 路线直接推进成主实现方案。

## 5. 性能指标结构化重构（已采纳）

### 5.1 背景

在连续补充持久化链采样、写盘计数等观测指标之后，性能指标导出链路开始出现明显的维护痛点：

- `SessionState` 在运行态持续堆叠平铺字段，新增一个稳定指标时需要同时改内部存储、summary 构建、CLI 输出和 Python `SUMMARY_SPECS`
- 很多 direct 性能指标在 `SessionState` 与 `PerformanceSummary` 两侧同名重复出现，但没有共享结构定义
- 之前尝试过用 `.def` 做导出层单一事实源，但可读性和可发现性一般，不利于后续 agent 直接理解

这轮改造的目标不是新增性能优化收益，而是把**性能指标定义本身**整理成更可维护的结构，同时明确不改变现有 benchmark 语义、summary key 和采样逻辑。

### 5.2 改造方案

本轮最终落地的方向不是继续保留 `.def`，而是把共享定义收口到新的头文件：

- 新增 `include/asyncdownload/performance_metrics.hpp`
- 把 direct 性能指标拆成可复用的字段组，例如：
  - `TimeToFirstMetrics`
  - `ResourcePeakMetrics`
  - `PauseMetrics`
  - `TransferCountMetrics`
  - `DurationCountMetrics`
- `SessionState` 不再继续平铺新增性能字段，而是统一收口到 `performance_metrics`
- `PerformanceSummary` 继承 `SummaryDirectPerformanceMetrics`，继续保留少量 derived 字段和面向导出的 latency 摘要
- latency 指标在内部改成结构化存储：
  - `latency.handle_data_packet`
  - `latency.append_bytes`
  - `latency.file_write`
- CLI summary 和 Python benchmark schema 继续保持原有扁平 key，不改变外部消费语义

设计边界：

- 不改 `SessionState` 里非性能状态字段
- 不改持久化链采样逻辑与现有计数口径
- 不改 `main.cpp` 输出的 summary key
- 不改 `scripts/performance/performance_common.py` 的 CSV 目标字段名和统计语义
- 共享的是字段组定义，不是让运行态和导出态直接共用同一个 concrete struct
- 运行态仍然使用 `atomic`/原始累计值，导出态仍然是普通快照值

### 5.3 验证结果

基础验证通过：

- `scripts\\build.bat release`
- `ctest -C Release --output-on-failure -E "DownloadIntegrationTest.*"`

结构化重构前 benchmark 目录：

- `build/benchmarks/20260312_215513_metrics-export-refactor-post`

结构化重构后 benchmark 目录：

- `build/benchmarks/20260312_223814_metrics-struct-refactor-post`

由于第一轮结构化重构 benchmark 里 `baseline_default` 出现了较明显单轮波动，又补跑了一轮 core rerun 复验：

- `build/benchmarks/20260312_224233_metrics-struct-refactor-post-rerun`

最终以复验结果做 keeper 判断。核心 case 中位值对比如下：

| Case | 改造前 avg net MB/s | 复验 avg net MB/s | `queue_full_pause_count` 变化 | `max_memory_bytes` 变化 |
| --- | ---: | ---: | ---: | ---: |
| `baseline_default` | 800.01 | 752.66 | `150,632.5 -> 107,538.5` | `-0.45%` |
| `balanced_candidate` | 685.87 | 673.25 | `253,839.5 -> 261,449` | `+0.56%` |
| `memory_guard` | 346.23 | 369.32 | `643,949 -> 533,773.5` | `+1.18%` |
| `scheduler_stress` | 693.90 | 695.00 | `146,406.5 -> 104,881` | `+0.46%` |

从复验结果看：

- `regression_v2` 的核心 keeper case 没有出现可归因的系统性回退
- `balanced_candidate`、`scheduler_stress` 的吞吐变化都落在原本波动范围内
- `memory_guard` 反而有一定改善
- `baseline_default` 的单轮下探仍存在，但前后两轮本身都带有较高方差，更像运行波动而不是结构化重构直接引入的统一退化
- summary、`raw_runs.csv` 和 `aggregated_cases.csv` 的字段语义保持不变

### 5.4 结论

这轮改造采纳，原因是：

- direct 性能指标的共享定义已经收口到 `performance_metrics.hpp`
- `SessionState` 不再继续平铺新增性能字段，结构明显更清晰
- benchmark 没有表现出可归因于这次重构的系统性性能回退
- 现有导出 key 与 benchmark schema 保持兼容，没有破坏已有性能基线解释

当前阶段保留的边界是：

- Python `SUMMARY_SPECS` 仍然是显式表，不把它也做成复杂元编程
- 这轮没有继续推进 `alignas` 分组或线程域内存布局重排
- 这轮没有改变 summary 对外扁平 key，只是在内部把 direct 字段和 latency 指标结构化
- 如果后续继续扩展性能指标，优先在共享字段组中扩展，再决定是否需要进一步统一 Python schema 来源

后续如果需要继续提升可维护性，再单独评估：

- Python schema 是否也应读取同一份头文件元数据
- `SessionState` 内部是否还需要按写入域进一步分组
- 是否有必要为热路径指标做显式 cache-line 隔离

## 6. 优化迭代 004：Flush/CRC 链路定位与增量观测（部分采纳）

### 6.1 背景

在 `PersistenceThread staging` 批量写盘实验失败之后，新的关键问题不再是“继续把连续 packet 往 staging 里拼”，而是先回答两件更基础的事情：

1. 真实 benchmark 路径里，连续 packet 到底主要形成在主消费链，还是乱序重排链。
2. `queue_full_pause` 背后究竟更像是写盘本身慢，还是 `flush + CRC read + metadata save` 这一整条恢复链在放大长尾。

为此，这一轮先补的不是新优化逻辑，而是新观测指标。

### 6.2 新增并保留的观测指标

这轮最终保留下来的改动是新的性能指标，而不是新的优化实现。

新增 latency 指标：

- `metadata_snapshot`
- `crc_sample_read`
- `flush_pending_write`

新增 direct 指标：

- `direct_append_packets_total`
- `out_of_order_insert_packets_total`
- `drained_ordered_packets_total`
- `out_of_order_queue_peak_packets`
- `out_of_order_queue_peak_bytes`
- `crc_sample_blocks_total`
- `crc_sample_bytes_total`

这些指标已经接入：

- 运行态累积
- summary 导出
- CLI 输出
- Python benchmark schema
- `PersistenceThread` 相关单测断言

### 6.3 指标验证结果

基础验证通过：

- `scripts\\build.bat release`
- `ctest -C Release --output-on-failure -E "DownloadIntegrationTest.*"`

核心 benchmark 目录：

- `build/benchmarks/20260312_232220_flush-triage-metrics-core`

这轮最关键的观察结论是：

1. 真实下载路径里，数据几乎都走了主顺序追加链。
   - `direct_append_packets_total` 基本贴近 `packets_enqueued_total`
   - `out_of_order_insert_packets_total` 在核心 case 中接近 `0`
   - `drained_ordered_packets_total` 在核心 case 中接近 `0`
   - `out_of_order_queue_peak_packets / bytes` 也接近 `0`
2. 这直接说明前一轮 `PersistenceThread staging` 没命中真实热路径。
3. `flush_pending_write_sample_count` 与 `file_write_calls_total` 同量级，说明写盘主链经常与挂起中的 flush/meta 任务重叠。
4. `metadata_snapshot` 本身不是热点，平均耗时只有百微秒级。
5. `crc_sample_read` 体量很大，尤其在 `memory_guard` 下更明显。
   - `crc_sample_blocks_total_median = 13,300`
   - `crc_sample_bytes_total_median = 871,628,800`

因此这轮真正建立起来的新的事实是：

- 乱序重排不是当前主矛盾
- 继续沿着 `out_of_order_queue` 触发点做 batching 应降级
- `flush + CRC read + metadata save` 仍然是值得重点怀疑的长尾链路

### 6.4 本轮已否决实验

在这组新指标的支撑下，这一轮还连续验证了 3 个实验方向，最终都不采纳。

#### 6.4.1 CRC sample cache（未采纳）

实验目录：

- `build/benchmarks/20260312_234454_crc-sample-cache-opt-core`

思路是缓存已经生成过的 CRC 样本，避免在多次 flush 之间重复回读同一个 block。

结果：

- `baseline_default` 吞吐 `743.11 -> 730.91 MB/s`
- `scheduler_stress` 吞吐 `650.99 -> 575.60 MB/s`
- `crc_sample_read_sample_count` 并没有出现足够明显的系统性下降

结论：

- 这条路线没有换来足够的 `crc_sample_read` 收缩
- 但引入了额外查表和状态维护成本
- 不是 keeper

#### 6.4.2 放宽 flush 周期（未采纳）

实验目录：

- `build/benchmarks/20260312_234800_flush-64m-5000ms-core`

实验配置：

- `flush_threshold_bytes = 64 MiB`
- `flush_interval_ms = 5000`

思路是减少 `flush + metadata + CRC` 触发频率，观察是否能直接换来吞吐提升。

结果：

- `baseline_default` 吞吐基本持平，但 pause 上涨约 `29%`
- `balanced_candidate` 吞吐近似持平，但 pause 大幅恶化
- `scheduler_stress` 吞吐 `650.99 -> 326.74 MB/s`

结论：

- 这条路线虽然减少了 flush 次数，但明显破坏了 stress case 的行为结构
- 不能作为可接受的默认策略

#### 6.4.3 写入时增量 CRC（未采纳）

实验目录：

- `build/benchmarks/20260312_235150_incremental-crc-cache-core`

思路是对当前顺序写入链上的整块数据做增量 CRC，尽量减少 flush 后的磁盘回读。

结果：

- `baseline_default` 吞吐 `743.11 -> 723.67 MB/s`
- `scheduler_stress` 吞吐 `650.99 -> 588.17 MB/s`
- `crc_sample_read_sample_count` 下降幅度仍然很有限

结论：

- 这条路线在当前实现边界下没有足够大的 read 减量
- 但已经引入了“CRC 更多依赖写入路径而不是 flush 后回读”的设计风险
- 因此不继续保留

### 6.5 这轮之后的正式结论

这一轮最终采纳的是“新指标体系”，而不是新的性能优化代码。

新的正式结论应更新为：

1. `PersistenceThread staging` 之所以失败，不是收益太小，而是命中路径本身就不对。
2. 真实数据几乎都走主顺序追加链，乱序重排目前不是主要优化面。
3. `flush + CRC read + metadata save` 仍是重要成本来源，但当前几种低风险局部优化都没有形成 keeper。
4. 下一步最像主方向的，已经不是继续抠 `PersistenceThread` 局部，而是重新审视：
   - `queue_capacity_packets`
   - queue-full pause 的恢复语义
   - queue / backpressure 是否仍然与 `64KB` 聚合后的字节预算相匹配

这意味着下一阶段如果继续推进，大概率会触到 queue/backpressure 语义边界，而不再只是局部实现细抠。

## 7. 优化迭代 005：开放性多方向筛查（未采纳）

### 7.1 背景

在上一轮已经补齐 Flush/CRC 观测指标，并且连续否决了：

- `PersistenceThread staging`
- CRC sample cache
- 放宽 flush 周期
- 写入时增量 CRC

之后，新的工作目标不再是沿着单一局部实现继续细抠，而是做一轮更开放的筛查，快速判断还有哪些方向值得继续深挖。

这轮筛查的约束是：

- 仍以 `baseline_default` 为主锚点
- 仍要求 `balanced_candidate` / `memory_guard` / `scheduler_stress` 不能系统性变差
- 每次只做一个可独立回退的实验，不把多个方向混在一起

### 7.2 本轮筛查的 5 个实验方向

#### 7.2.1 连接复用：取消每个 window 强制新建连接（未采纳）

实验目录：

- `build/benchmarks/20260313_105914_connection-reuse-screen-core`

实现思路：

- 保留每个并发请求独立 `easy handle`
- 不改 distinct ports 并发隔离测试
- 只去掉 `CURLOPT_FRESH_CONNECT=1` / `CURLOPT_FORBID_REUSE=1`

额外验证：

- `DownloadIntegrationTest.UsesDistinctClientPortsAcrossConcurrentRanges` 仍然通过

结果：

- `baseline_default` 吞吐 `743.11 -> 671.80 MB/s`
- `scheduler_stress` 吞吐 `650.99 -> 579.52 MB/s`
- `balanced_candidate` pause 明显恶化

结论：

- “每个 window 都新建连接”确实不是免费，但简单打开连接复用并没有形成 keeper
- 这说明当前系统的请求层成本，不适合直接靠关闭 fresh connect 解决

#### 7.2.2 window 结束时的 final flush 等待粒度优化（未采纳）

实验目录：

- `build/benchmarks/20260313_110436_window-flush-yield-screen-core`

实现思路：

- 只改 `flush_transfer_buffer_blocking()`
- 把固定 `1ms sleep` 改成“先 `yield()` 若干次，再进入慢等待”

结果：

- `baseline_default` 吞吐 `743.11 -> 729.15 MB/s`
- `scheduler_stress` 吞吐 `650.99 -> 570.16 MB/s`
- `balanced_candidate` pause 继续显著放大

结论：

- request 收尾阶段的等待粒度并不是当前主矛盾
- 即便这个等待点可能真实存在，也不足以解释主要吞吐差距

#### 7.2.3 把 TransferHandle 聚合目标提升到 `128KiB`（未采纳）

实验目录：

- `build/benchmarks/20260313_110904_agg-128k-screen-core`

实现思路：

- 不碰 `PersistenceThread`
- 只把网络侧本地聚合缓冲从 `64KiB` 提到 `128KiB`
- 试图进一步压低主顺序追加链上的 packet 数

结果：

- `baseline_default` 吞吐 `743.11 -> 709.90 MB/s`
- `memory_guard` 吞吐 `348.89 -> 345.30 MB/s`
- `scheduler_stress` 吞吐 `650.99 -> 569.38 MB/s`

结论：

- 当前系统在 `64KiB` 左右已经接近更合适的平衡点
- 继续放大 packet 粒度，没有把 per-packet 成本稳定转成收益

#### 7.2.4 FileWriter 分离只读句柄，降低 CRC 回读与写盘锁竞争（未采纳）

实验目录：

- `build/benchmarks/20260313_111618_filewriter-read-handle-screen-core`

实现思路：

- `FileWriter::flush()` 仍然走原写句柄
- `FileWriter::read()` 改为优先走单独只读句柄
- 目标是只拆掉 CRC sample read 与主写入链之间的锁竞争

结果：

- `baseline_default` 吞吐 `743.11 -> 679.23 MB/s`
- `balanced_candidate` 吞吐 `671.48 -> 587.16 MB/s`
- `memory_guard` 略有改善，但不足以抵消主 keeper case 退化
- `crc_sample_read_avg_us` 确实略降，但收益远小于行为副作用

结论：

- 读锁争用并不是当前主吞吐瓶颈
- 即使把 CRC 回读和主写句柄拆开，也没有换来主收益结构改善

#### 7.2.5 高背压预算 profile 内部抬高 window 下限到 `8MiB`（未采纳）

实验目录：

- `build/benchmarks/20260313_112157_window-8m-screen-core`

实现思路：

- 在高背压预算 case 中，不再完全照用 `scheduler_window_bytes=4MiB`
- 把内部实际 window 下限抬到 `8MiB`
- 目标是减少 window 数和 fresh-connect 请求数

结果：

- `scheduler_stress` 出现灾难性长尾
- 单轮最差 run 已经掉到远低于当前可接受范围
- 这条路线直接触碰了 `scheduler_window_bytes` 语义边界

结论：

- `scheduler_window_bytes` 的行为语义比预期更敏感
- 不能通过“内部偷偷放大 window”来换吞吐
- 这条路线在当前设计边界下应直接降级

### 7.3 这轮筛查之后的新结论

这轮最重要的新增结论不是“找到了 keeper”，而是进一步缩小了可行方向：

1. 请求层固定成本虽然真实存在，但不能简单靠关闭 fresh connect 或放大 window 来解决。
2. 主顺序追加链仍然是正确命中面，但 `64KiB` 聚合之后继续放大粒度，并没有形成下一轮稳定收益。
3. `flush + CRC read + metadata save` 仍然可疑，但“拆读句柄”和“减少 request 收尾等待粒度”都不足以命中主矛盾。
4. 真正还值得继续深挖的，仍然更像是 queue/backpressure 语义本身，而不是继续改单个局部 helper。

### 7.4 下一阶段建议

如果继续推进，下一轮更值得做的不是再试一个局部微优化，而是先补能直接回答下面问题的观测：

- queue-full pause 到 resume 的真实时长分布
- queue-full 与 memory-backpressure 当前是否仍被同一恢复机制放大
- `queued_packets` 之外，是否还需要一个更接近“字节预算”的运行态指标

也就是说，下一阶段的主方向应重新收敛回：

- queue / backpressure 语义定位
- 而不是继续做请求层或 FileWriter 的小修小补

## 8. 当前状态总结

截至目前，第一次性能优化可以定性为：

- **方向正确**
- **收益明显**
- **但也改变了系统的队列语义**

因此这次优化不是终点，而是一个阶段性拐点：

- packet 过碎问题已经被明显缓解
- 下一阶段应更多关注：
  - pause churn
  - 队列与背压语义
  - 在低内存约束下维持高吞吐
  - 持久化消费链的批量化降本，而不是只压单点 bookkeeping

## 9. 优化迭代 006：Queue/Backpressure 诊断指标补齐（采纳）

### 9.1 背景

在上一轮开放性筛查之后，已经能够确定一个事实：

- 当前更缺的不是新的局部微优化
- 而是 queue/full pause 与 memory backpressure 之间缺少足够可解释的观测

尤其是下面几个问题，在旧指标下仍然回答不了：

- `queue_full_pause` 从开始到真正恢复，持续了多久
- queue 已经满足恢复条件时，是否仍被 memory 低水位门槛继续阻塞
- 聚合到 `64KiB` 之后，只看 `queued_packets` 是否已经不足以表达真实积压预算

因此这一轮线程的目标不是直接抬吞吐，而是先把 queue/backpressure 的诊断口径补齐。

### 9.2 本轮落地内容

方案文档见：

- [queue_backpressure_instrumentation_plan_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/queue_backpressure_instrumentation_plan_zh.md)

本轮实际落地的内容包括：

- 在 `SessionState` 增加 `queued_bytes`
- 将 queue-full pause 与 memory pause 的运行态来源拆开记录
- 增加 queue/memory pause 峰值、恢复次数、overlap 次数
- 增加 queue pause、memory pause、queue-ready-but-memory-blocked 三组 duration 采样
- 在 summary 导出链路中补齐对应 direct/derived 指标
- 在 `PersistenceThread` dequeue 入口按 `packet.accounted_bytes` 扣减 `queued_bytes`

为了保证新的 summary 字段不会把验证链路本身卡死，还顺手修正了两个测试基础设施问题：

- `DownloadIntegrationTest` 现在优先使用与当前测试配置一致的 CLI 二进制
- CLI 配置加载测试不再通过阻塞读取 stdout 的方式等待外部进程退出

### 9.3 涉及代码

主要改动文件：

- [performance_metrics.hpp](/D:/git_repository/coding_with_agents/AsyncDownload/include/asyncdownload/performance_metrics.hpp)
- [types.hpp](/D:/git_repository/coding_with_agents/AsyncDownload/include/asyncdownload/types.hpp)
- [models.hpp](/D:/git_repository/coding_with_agents/AsyncDownload/src/core/models.hpp)
- [download_engine.cpp](/D:/git_repository/coding_with_agents/AsyncDownload/src/download/download_engine.cpp)
- [persistence_thread.cpp](/D:/git_repository/coding_with_agents/AsyncDownload/src/persistence/persistence_thread.cpp)
- [main.cpp](/D:/git_repository/coding_with_agents/AsyncDownload/src/main.cpp)
- [performance_common.py](/D:/git_repository/coding_with_agents/AsyncDownload/scripts/performance/performance_common.py)
- [persistence_thread_test.cpp](/D:/git_repository/coding_with_agents/AsyncDownload/tests/persistence/persistence_thread_test.cpp)
- [download_resume_integration_test.cpp](/D:/git_repository/coding_with_agents/AsyncDownload/tests/download/download_resume_integration_test.cpp)

### 9.4 验证结果

本轮没有直接重跑 benchmark，对 keeper 结论也没有做新一轮吞吐判定。

完成的是“代码链路已接通，验证链路可用”的确认：

- `scripts\build.bat release` 通过
- `PersistenceThreadTest.*` 全部通过
- `DownloadIntegrationTest.LoadsDownloadOptionsFromConfigFile` 通过
- `DownloadIntegrationTest.ReportsDetailedProgressSnapshot` 通过
- `ctest -C Release --output-on-failure -E "DownloadIntegrationTest.*"` 全部通过

### 9.5 这轮之后的意义

到这里，后续 queue/backpressure 优化终于有了可直接依赖的诊断面：

- 可以区分 queue pause 和 memory pause 的生命周期
- 可以量化 queue-ready-but-memory-blocked 的放大程度
- 可以把 `queued_packets` 与 `queued_bytes` 结合起来看实际预算形态

因此下一轮如果继续做 queue/backpressure 语义调整，至少已经不再是“只看 pause 总次数”的盲改。

## 10. 优化迭代 007：Queue pause 触发原因诊断（采纳）

### 10.1 背景

在补齐 queue/backpressure 生命周期指标之后，新的 benchmark 结果又暴露了一个更具体的问题：

- `deep_buffer_candidate` 和 `queue_backpressure_stress` 的 `queue_capacity_packets=4096`
- 但运行态 `max_queued_packets` 仍长期停在 `~1000`
- 单看 `queue_full_pause_count` 还不能判断，到底是我们配置的 packet cap 先命中，还是底层 queue 写入路径更早失败

这意味着下一轮如果直接改 queue resume 语义，仍然会混着一个更底层的未知量。

### 10.2 本轮落地内容

本轮新增了两条 direct 指标，用于区分 queue pause 的直接触发来源：

- `queue_full_pause_capacity_reached_count`
- `queue_full_pause_try_enqueue_failure_count`

接入位置保持最小化：

- 运行态判断放在 `download_engine.cpp` 的 `flush_transfer_buffer()` 和 `start_queue_pause()`
- summary 导出接到 `performance_metrics.hpp`、`main.cpp`、`performance_common.py`
- 集成测试只补最小断言，确认新字段进入 `PerformanceSummary`

### 10.3 涉及代码

主要改动文件：

- [performance_metrics.hpp](/D:/git_repository/coding_with_agents/AsyncDownload/include/asyncdownload/performance_metrics.hpp)
- [download_engine.cpp](/D:/git_repository/coding_with_agents/AsyncDownload/src/download/download_engine.cpp)
- [main.cpp](/D:/git_repository/coding_with_agents/AsyncDownload/src/main.cpp)
- [performance_common.py](/D:/git_repository/coding_with_agents/AsyncDownload/scripts/performance/performance_common.py)
- [download_resume_integration_test.cpp](/D:/git_repository/coding_with_agents/AsyncDownload/tests/download/download_resume_integration_test.cpp)

### 10.4 验证结果

基础验证通过：

- `scripts\build.bat release`
- `DownloadIntegrationTest.ReportsDetailedProgressSnapshot`
- `ctest -C Release --output-on-failure -E "DownloadIntegrationTest.*"`

随后做了一轮针对性 smoke benchmark：

- `build/benchmarks/20260313_205401_queue-pause-cause-smoke`

关键结果如下：

- `baseline_default`
  - `max_queued_packets_median = 1013`
  - `queue_full_pause_capacity_reached_count_median = 0`
  - `queue_full_pause_try_enqueue_failure_count_median = 85206`
- `deep_buffer_candidate`
  - `max_queued_packets_median = 1016`
  - `queue_full_pause_capacity_reached_count_median = 0`
  - `queue_full_pause_try_enqueue_failure_count_median = 118199`
- `queue_backpressure_stress`
  - `max_queued_packets_median = 1004`
  - `queue_full_pause_capacity_reached_count_median = 0`
  - `queue_full_pause_try_enqueue_failure_count_median = 4`
  - `queue_resume_blocked_by_memory_count_median = 4`
- `memory_guard`
  - `max_queued_packets_median = 62`
  - `queue_full_pause_capacity_reached_count_median = 0`
  - `queue_full_pause_try_enqueue_failure_count_median = 609667`

### 10.5 当时先记录下来的现象与待闭环问题

这轮 smoke benchmark 当场已经足以先记录下面这些现象：

1. 当前主 case 的 queue pause，几乎都不是 `queue_capacity_packets` 先触发。
2. 直接触发信号主要来自 `try_enqueue` 更早失败。
3. `queue=4096` 的 case 运行态 `max_queued_packets` 仍长期停在 `~1000` 左右。
4. `queue_backpressure_stress` 仍然有双层放大：
   - queue pause 本身由 `try_enqueue` 失败触发
   - 恢复阶段又继续被 memory low-watermark 放大

但到这一步为止，真正已经闭环的只有“观测层”和“本地代码路径层”：

- 观测层：新的指标已经证明主触发信号来自 `try_enqueue` failure，而不是逻辑 packet cap。
- 代码路径层：pause 触发链路已经定位到 [download_engine.cpp](/D:/git_repository/coding_with_agents/AsyncDownload/src/download/download_engine.cpp) 的 `flush_transfer_buffer()`。

当时仍然必须继续追问的待闭环问题是：

- `try_enqueue` 为什么会在 `queue_capacity_packets` 之前失败
- 这是业务语义使然，还是当前对第三方 queue 的使用方式与预期语义不匹配

这一步应该先写入文档保留现场，但不能直接把它升级成最终根因结论。

### 10.6 补充源码分析后的机制闭环

在保留上面的开放问题之后，后续继续补读了本地调用点和第三方 queue 源码：

- 当前队列构造与写入方式在 [download_engine.cpp](/D:/git_repository/coding_with_agents/AsyncDownload/src/download/download_engine.cpp)：
  - `BlockingConcurrentQueue<core::DataPacket> data_queue(session.options.queue_capacity_packets)`
  - `data_queue->try_enqueue(std::move(packet))`
- 第三方实现位于：
  - [blockingconcurrentqueue.h](/D:/git_repository/coding_with_agents/AsyncDownload/libs/concurrentqueue/include/concurrentqueue/blockingconcurrentqueue.h)
  - [concurrentqueue.h](/D:/git_repository/coding_with_agents/AsyncDownload/libs/concurrentqueue/include/concurrentqueue/concurrentqueue.h)

补充源码分析后，机制层已经能闭环到下面几条事实：

1. 我们当前使用的是单参数构造器加隐式 producer，不是 explicit producer token 路径。
2. 运行态调用的是 `try_enqueue`，它走的是 no-allocation 的 `CannotAlloc` 路径，而不是可按需扩容的 `enqueue`。
3. 单参数构造器的 `capacity` 语义并不等价于“预先保证这么多个 `try_enqueue` 在无额外分配下都成功”；源码注释明确说，无额外分配时的可插入数量还取决于 producer 数量和 block size。
4. 当前第三方实现里的 `BLOCK_SIZE = 32`，而 implicit producer 默认的 `IMPLICIT_INITIAL_INDEX_SIZE = 32`。这意味着仅从初始 block index 和 block 大小看，`32 * 32 = 1024` 就已经与 benchmark 里长期出现的 `~1000` queue 深度高度吻合。
5. 一旦 implicit producer 需要新的 block index 或新的 block，而 `try_enqueue` 又不允许分配，源码就会直接返回 `false`。

因此，这里的 `try_enqueue` failure 已经不再只是一个现象标签，而是可以进一步解释为：

- 当前实现用的是“隐式 producer + no-allocation `try_enqueue`”这条路径
- 这条路径的无额外分配能力并不等价于我们对 `queue_capacity_packets` 的直觉语义
- 所以 `queue=4096` 的 case 会在明显更早的位置先撞上第三方 queue 的无分配边界

### 10.7 修正后的最终结论

把 smoke benchmark 诊断和后续源码闭环合起来，这一轮的最终结论应修正为：

1. 本轮新增指标首先成功暴露了一个高价值开放问题：queue pause 的直接触发源不是逻辑 `queue_capacity_packets`，而是 `try_enqueue` failure。
2. 这个开放问题在后续源码分析中已经进一步闭环到机制层：根因不是“系统天然只需要约 `1000` 个 packet queue”，而是我们当前对 `BlockingConcurrentQueue` 的使用方式，并没有把 `queue_capacity_packets` 变成等价的无分配 `try_enqueue` 可用容量。
3. 因此，`queue_capacity_packets` 当前的对外语义与底层 queue 实际行为之间存在错位；这是一个第三方库使用方式与业务配置语义不完全对齐的问题，不应再被写成简单的“深队列没有价值”。
4. `queue_backpressure_stress` 的问题仍然是双层的：
   - 第一层是 queue pause 会先被 `try_enqueue` 的无分配边界触发
   - 第二层是恢复阶段又继续被 memory low-watermark 放大

因此下一轮更值得优先做的，不再是继续泛泛猜“queue 是否应该更深”，而是按顺序处理下面两个问题：

- 先修正 queue 的构造与 producer 使用方式，使 `queue_capacity_packets` 与真实可用 queue 容量重新对齐
- 再单独评估 queue-full pause 是否应该继续与 memory low-watermark 共享同一恢复门槛

## 11. 优化迭代 008：BlockingConcurrentQueue explicit producer 预分配修正（部分采纳）

### 11.1 背景

在上一轮已经通过 benchmark 现象和第三方源码补读，确认了一个更具体的问题：

- 当前代码使用的是 `BlockingConcurrentQueue(capacity)` + 隐式 producer + `try_enqueue`
- 这不等价于“`queue_capacity_packets` 个无额外分配的 `try_enqueue` 槽位已经准备好”
- 因此 queue 语义和底层 queue 的 no-allocation 行为之间存在错位

这轮的目标不是先改 pause/resume 语义，而是先把第三方 queue 的使用方式修正到更符合预期的形式。

### 11.2 本轮改动

本轮在 [download_engine.cpp](/D:/git_repository/coding_with_agents/AsyncDownload/src/download/download_engine.cpp) 做了最小改法：

- 把 `BlockingConcurrentQueue<core::DataPacket>` 改为三参数构造：
  - `minCapacity = queue_capacity_packets`
  - `maxExplicitProducers = 1`
  - `maxImplicitProducers = 1`
- 为网络写入路径引入 explicit producer token
- 数据 packet 热路径改成 `try_enqueue(token, std::move(packet))`
- 控制 packet 改成 `enqueue(token, std::move(packet))`，保留“不要丢 completion 通知”的语义

这次改动的目的，是让第三方 queue 的预分配和 no-allocation 写入路径尽量与我们对 `queue_capacity_packets` 的语义保持一致，而不是继续沿用默认隐式 producer 路径。

### 11.3 验证结果

基础验证通过：

- `scripts\build.bat release`
- `ctest -C Release --output-on-failure -E "DownloadIntegrationTest.*"`
- `AsyncDownload_tests.exe --gtest_filter=DownloadIntegrationTest.ReportsDetailedProgressSnapshot`

随后做了两轮 smoke benchmark：

- `build/benchmarks/20260313_213406_queue-explicit-producer-prealloc-smoke`
- `build/benchmarks/20260313_213628_queue-explicit-producer-prealloc-core-smoke`

几条关键观察如下：

1. 这次改法确实更接近第三方库的正确用法。
   - 在定向 smoke 里，`deep_buffer_candidate` 的
     `queue_full_pause_try_enqueue_failure_count_median`
     从 `118,199` 降到了 `29`
   - 同一轮里 `queue_full_pause_count_median` 也从 `118,199` 降到了 `10`

2. 但这个修正本身，并没有稳定把运行态 queue 深度直接推到 `4096`。
   - 两轮 smoke 里，`deep_buffer_candidate` 和 `queue_backpressure_stress`
     的 `max_queued_packets_median` 仍然都大多落在 `~1000`
   - 对应 `max_queued_bytes_median` 仍然大多落在 `~64-66MiB`

3. 在更宽一点的 core smoke 里，pause churn 仍然会重新出现。
   - `deep_buffer_candidate` 的中位 `queue_full_pause_count` 仍然回到 `111,586`
   - `queue_backpressure_stress` 仍然保持 `queue_resume_blocked_by_memory_count = 4`

### 11.4 结论

这轮应按“部分采纳”理解：

1. 从第三方库使用方式上看，这次改法是正确的，应当保留。
   - 它把 queue 的构造和生产者用法，修正到了更符合 `moodycamel::BlockingConcurrentQueue` 预期的形式
   - 这属于语义对齐和风险收敛，不只是可选微调

2. 但从性能和运行态行为上看，这次改法本身不是终点。
   - 它没有稳定证明“effective queue depth 会因此直接扩展到 `4096`”
   - 说明先前观测到的 `~1000 packet / ~64MiB` 峰值，不是单一的第三方 queue 预分配问题

3. 当前更合理的新判断是：
   - 这次修正解决的是“第三方 queue 用法错位”
   - 但运行态 queue 仍然还受到其它机制共同约束，例如：
     - `scheduler_window_bytes * max_connections` 形成的并发生产预算
     - `memory high/low watermark` 的恢复门槛
     - queue-full pause 的高频微抖动语义

因此下一步不该再把“改成 explicit producer 预分配”当成最终答案，而应该在这个修正过的基础上，继续回答两个问题：

- 为什么多个 case 的真实 queue 峰值仍然稳定停在 `~64MiB`
- queue-full pause 的恢复门槛是否仍然过于容易形成高频 churn

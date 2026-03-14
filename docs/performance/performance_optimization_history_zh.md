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

## 12. 优化迭代 009：Queue resume 语义探索（未采纳）

### 12.1 背景

在 [20260313_215204_new-data-queue-regression-v2-codex-rerun](/D:/git_repository/coding_with_agents/AsyncDownload/build/benchmarks/20260313_215204_new-data-queue-regression-v2-codex-rerun/report.md) 里，explicit producer 预分配修正之后又出现了一个更细的问题：

1. 多个主 case 的吞吐和 `queue_full_pause_count` 相比 `2026-03-11` 正式基线已经明显改善。
2. 但 `max_queued_packets` / `max_queued_bytes` 仍然大多稳定停在 `~1000 / ~64-66MiB`。
3. `queue_backpressure_stress` 的
   `queue_resume_blocked_by_memory_count_median = 4`，
   `queue_resume_blocked_by_memory_avg_ms_median = 120.745`，
   而 `memory_pause_count_median = 0`。

这说明当前更值得确认的问题，不再是“explicit producer 是否正确”，而是：

- `resume_paused_transfers()` 里的 queue resume 语义是不是本身还过于保守
- 当前低水位恢复门槛，是否同时承担了一个隐含的运行态预算控制职责

因此这轮不再改第三方 queue 用法，而是对 queue resume 行为做 3 次小范围探索。

### 12.2 探索内容

这轮围绕 [download_engine.cpp](/D:/git_repository/coding_with_agents/AsyncDownload/src/download/download_engine.cpp) 的 `resume_paused_transfers()` 做了 3 个候选实验：

1. 实验 A：把 queue pause 的恢复门槛从 `backpressure_low_bytes` 提前到 `backpressure_high_bytes`
2. 实验 B：保留原来的 memory low-watermark 条件，但给 queue pause 自身加一段 packet drain hysteresis 后再恢复
3. 实验 C：保留原来的恢复门槛，只限制每次事件循环最多恢复一个 queue-paused handle

3 个实验都只在本地代码里改动，最终都没有保留进工作树。

### 12.3 验证结果

每个实验都按同一顺序做了基础验证：

- `scripts\build.bat release`
- `ctest -C Release --output-on-failure -E "DownloadIntegrationTest.*"`

随后分别跑了 5 次重复的定向 benchmark：

- 实验 A：
  - `build/benchmarks/20260313_235111_queue-resume-high-watermark-exp1`
- 实验 B：
  - `build/benchmarks/20260313_235420_queue-resume-drain-hysteresis-exp2`
- 实验 C：
  - `build/benchmarks/20260313_235652_queue-resume-one-handle-exp3`

几条最关键的结果如下。

#### 12.3.1 实验 A：提前到 high-watermark 恢复（拒绝）

观测层：

- `baseline_default`
  - `queue_full_pause_count_median: 150,744 -> 4`
  - `max_memory_bytes_median: 65.6MiB -> 129.8MiB`
  - `avg_network_speed_mb_s_median: 702.20 -> 664.07`
- `deep_buffer_candidate`
  - `queue_full_pause_count_median: 139,933 -> 0`
  - `max_memory_bytes_median: 63.2MiB -> 154.8MiB`
  - `avg_network_speed_mb_s_median: 647.10 -> 575.60`
- `queue_backpressure_stress`
  - `avg_network_speed_mb_s_median: 752.12 -> 649.33`
  - 中位 pause 仍是 `4`

代码路径层：

- 改动命中的正是 `resume_paused_transfers()` 中 queue pause 的恢复条件

机制层结论：

- queue pause 提前恢复，确实能把大量微小 queue pause 合并掉
- 但这些 pause 并没有“免费消失”，而是被转换成了更高的 inflight 和 memory 峰值

因此实验 A 不采纳。

#### 12.3.2 实验 B：queue drain hysteresis 后再恢复（拒绝）

观测层：

- `baseline_default`
  - `queue_full_pause_count_median: 150,744 -> 4`
  - `max_memory_bytes_median: 65.6MiB -> 129.6MiB`
  - `avg_network_speed_mb_s_median: 702.20 -> 666.23`
- `deep_buffer_candidate`
  - `queue_full_pause_count_median: 139,933 -> 0`
  - `max_memory_bytes_median: 63.2MiB -> 161.2MiB`
  - `avg_network_speed_mb_s_median: 647.10 -> 563.88`
- `queue_backpressure_stress`
  - `avg_network_speed_mb_s_median: 752.12 -> 654.73`
  - 中位 pause 仍是 `4`

机制层结论：

- 给 queue pause 自身增加 hysteresis，同样能减少恢复抖动
- 但它依然把系统推进到更高的运行态积压区间
- 说明当前高频 queue pause 不只是“恢复太快”的问题，它还在和全局 inflight/memory 预算耦合

因此实验 B 不采纳。

#### 12.3.3 实验 C：每轮最多恢复一个 queue handle（拒绝）

观测层：

- `baseline_default`
  - `avg_network_speed_mb_s_median: 702.20 -> 672.80`
  - `max_memory_bytes_median: 65.6MiB -> 129.9MiB`
- `balanced_candidate`
  - `avg_network_speed_mb_s_median: 671.70 -> 499.51`
- `deep_buffer_candidate`
  - `avg_network_speed_mb_s_median: 647.10 -> 468.86`
  - `max_memory_bytes_median: 63.2MiB -> 256.0MiB`
- `queue_backpressure_stress`
  - `avg_network_speed_mb_s_median: 752.12 -> 593.97`

机制层结论：

- 单次只恢复一个 handle 并没有带来更平滑的 keeper 形态
- 它反而让某些 case 同时出现更差的吞吐和更高的积压峰值
- 因此问题也不主要在“同时恢复太多 handle”本身

因此实验 C 不采纳。

### 12.4 这轮探索的最终结论

这 3 轮 reject 实验合起来，给出了一个比“恢复门槛太保守”更强的系统信号：

1. 当前 queue pause 的恢复条件，不只是一个 pause/resume 语义开关。
2. 它还实际承担着一个隐藏的运行态 inflight/memory 预算控制职责。
3. 因此，简单放宽 queue resume 条件，或者只在恢复节奏上做小抖动修饰，都只是在把 queue pause churn 转换成更高的 memory/inflight 峰值，并不稳定地产生 keeper 收益。

换句话说，这轮已经足以把“继续直接改 queue resume 语义”降级：

- 这不是一个低风险、低耦合的下一刀
- 当前更值得优先闭环的，是 `~64-66MiB` 真实运行态峰值到底由哪条预算链共同形成

### 12.5 下一步建议

下一轮更值得优先做的，不是继续改 resume 语义，而是先回答下面两个问题：

1. `scheduler_window_bytes * max_connections`、`downloaded_bytes - persisted_bytes`、`queued_bytes` 三者之间，当前到底是哪条预算先形成了 `~64-66MiB` 上限
2. 若要继续改 queue/backpressure 行为，是否应该先把“预算语义”与“恢复语义”拆开，而不是直接调低/调高当前 low-watermark 门槛

## 13. 优化迭代 010：Budget chain 观测补强（采纳）

### 13.1 背景

在前一轮 queue/backpressure 语义探索全部 reject 之后，当前最需要闭环的问题已经变成：

1. `scheduler_window_bytes * max_connections` 是否真的在形成当前运行态上限。
2. `downloaded_bytes - persisted_bytes` 里，到底有多少字节仍然堆在 network -> persistence queue 里，多少已经离开 queue 但还没落盘。
3. queue pause / memory pause 开始时，真实触发点更接近哪一层预算。

之前已有的 direct 指标里：

- `max_inflight_bytes` 只能看到总积压
- `max_queued_bytes` 是 accounted memory 口径
- `queue_full_pause_start_queued_bytes_*` 也还是 accounted memory 口径

它们不足以直接回答：

- 当前 inflight 是否几乎全部还停在 queue 里
- `scheduler_window_bytes * max_connections` 与实际峰值的距离
- pause 开始时是 queue payload、总 inflight 还是 memory watermark 先撞顶

### 13.2 本轮新增观测

本轮新增了 3 组指标。

第一组是峰值预算拆解：

- `max_active_window_bytes`
- `max_queued_payload_bytes`
- `max_post_queue_inflight_bytes`

其中：

- `max_active_window_bytes` 用当前活跃 handle 的 window 长度求和，直接观察 scheduler 侧同时派发的请求预算
- `max_queued_payload_bytes` 用 payload 字节口径统计 queue 内真实积压，避免与 accounted memory 混口径
- `max_post_queue_inflight_bytes` 用 `max(0, inflight_bytes - queued_payload_bytes)` 估计“已离开 queue 但尚未持久化”的区间

第二组是 pause 起点快照：

- `queue_full_pause_start_queued_payload_bytes_total`
- `queue_full_pause_start_inflight_bytes_total`
- `queue_full_pause_start_memory_bytes_total`
- `memory_pause_start_queued_payload_bytes_total`
- `memory_pause_start_inflight_bytes_total`
- `memory_pause_start_memory_bytes_total`

第三组是对应的平均值 summary：

- `queue_full_pause_start_queued_payload_bytes_avg`
- `queue_full_pause_start_inflight_bytes_avg`
- `queue_full_pause_start_memory_bytes_avg`
- `memory_pause_start_queued_payload_bytes_avg`
- `memory_pause_start_inflight_bytes_avg`
- `memory_pause_start_memory_bytes_avg`

为了让 payload 口径闭环，本轮还补了运行态状态：

- [models.hpp](/D:/git_repository/coding_with_agents/AsyncDownload/src/core/models.hpp)
  里的 `queued_payload_bytes`

并在 enqueue / dequeue 两侧同时维护它。

### 13.3 涉及代码

主要改动文件：

- [performance_metrics.hpp](/D:/git_repository/coding_with_agents/AsyncDownload/include/asyncdownload/performance_metrics.hpp)
- [types.hpp](/D:/git_repository/coding_with_agents/AsyncDownload/include/asyncdownload/types.hpp)
- [models.hpp](/D:/git_repository/coding_with_agents/AsyncDownload/src/core/models.hpp)
- [download_engine.cpp](/D:/git_repository/coding_with_agents/AsyncDownload/src/download/download_engine.cpp)
- [persistence_thread.cpp](/D:/git_repository/coding_with_agents/AsyncDownload/src/persistence/persistence_thread.cpp)
- [main.cpp](/D:/git_repository/coding_with_agents/AsyncDownload/src/main.cpp)
- [performance_common.py](/D:/git_repository/coding_with_agents/AsyncDownload/scripts/performance/performance_common.py)
- [download_resume_integration_test.cpp](/D:/git_repository/coding_with_agents/AsyncDownload/tests/download/download_resume_integration_test.cpp)

### 13.4 验证结果

基础验证通过：

- `scripts\build.bat release`
- `ctest -C Release --output-on-failure -E "DownloadIntegrationTest.*"`
- `AsyncDownload_tests.exe --gtest_filter=DownloadIntegrationTest.ReportsDetailedProgressSnapshot`

随后跑了一轮定向 smoke benchmark：

- `build/benchmarks/20260314_100841_budget-chain-instrumentation-smoke`

使用 case：

- `baseline_default`
- `balanced_candidate`
- `memory_guard`
- `deep_buffer_candidate`
- `queue_backpressure_stress`

每个 case 重复 `5` 次。

### 13.5 这轮观测直接给出的新信号

#### 13.5.1 `scheduler_window_bytes * max_connections` 不是当前主上限

新的 `max_active_window_bytes` 直接把 scheduler 预算显式化之后，可以看到：

- `baseline_default`
  - `max_active_window_bytes_median = 16MiB`
  - `max_inflight_bytes_median = 129.17MiB`
- `deep_buffer_candidate`
  - `max_active_window_bytes_median = 64MiB`
  - `max_inflight_bytes_median = 149.58MiB`
- `queue_backpressure_stress`
  - `max_active_window_bytes_median = 16MiB`
  - `max_inflight_bytes_median = 63.89MiB`

也就是说，当前主 case 的运行态峰值已经明显高于“活跃 window 总预算”本身。

这说明：

- `scheduler_window_bytes * max_connections` 仍然重要
- 但它不是现在最先撞上的那条硬上限

#### 13.5.2 `inflight` 几乎全部还停在 queue 里

这轮最强的新信号来自：

- `max_queued_payload_bytes`
- `max_post_queue_inflight_bytes`

几个代表 case 的中位值都很一致：

- `baseline_default`
  - `max_inflight_bytes_median = 135,449,718`
  - `max_queued_payload_bytes_median = 135,380,388`
  - `max_post_queue_inflight_bytes_median = 80,973`
- `deep_buffer_candidate`
  - `max_inflight_bytes_median = 156,848,299`
  - `max_queued_payload_bytes_median = 156,729,947`
  - `max_post_queue_inflight_bytes_median = 126,240`
- `queue_backpressure_stress`
  - `max_inflight_bytes_median = 66,989,176`
  - `max_queued_payload_bytes_median = 66,912,256`
  - `max_post_queue_inflight_bytes_median = 80,973`

按比例看，`max_queued_payload_bytes / max_inflight_bytes` 在这几个 case 都接近 `0.999`，而
`max_post_queue_inflight_bytes / max_inflight_bytes` 只有约 `0.0006 ~ 0.0034`。

这基本可以把“有一大块 inflight 是离开 queue 后才卡住”的猜测先降级。

当前更强的机制信号是：

- 大部分 inflight 积压就是 queue 自身
- queue 外但尚未持久化的那一层非常薄

#### 13.5.3 pause 起点也基本对齐到 queue payload

pause 起点快照进一步强化了上面的判断。

`baseline_default` 的 queue pause 起点中位平均值：

- `queue_full_pause_start_queued_payload_bytes_avg_median = 135,380,388`
- `queue_full_pause_start_inflight_bytes_avg_median = 135,449,718`
- `queue_full_pause_start_memory_bytes_avg_median = 135,822,052`

`queue_backpressure_stress` 的 memory pause 起点中位平均值：

- `memory_pause_start_queued_payload_bytes_avg_median = 66,887,679`
- `memory_pause_start_inflight_bytes_avg_median = 66,962,702`
- `memory_pause_start_memory_bytes_avg_median = 67,093,504`

这说明当前 pause 进入点附近，3 个层次几乎贴在一起：

- queue payload
- 总 inflight
- 当前 memory

换句话说，pause 触发时系统里几乎没有一个“很厚的 queue 外 inflight 区间”。

#### 13.5.4 `queue_backpressure_stress` 在这轮 smoke 里更像纯 memory watermark case

这轮 `queue_backpressure_stress` 的中位形态是：

- `queue_full_pause_count_median = 0`
- `memory_pause_count_median = 4`
- `queue_resume_blocked_by_memory_count_median = 0`

并且 memory pause 起点稳定在约 `67MiB`。

因此在这轮 smoke 环境里，这个 case 更像：

- queue 已经不是主 pause 触发源
- 当前直接撞上的主要是 memory watermark

这和 `2026-03-13` 那轮里“queue pause + memory 恢复放大”的形态不完全一样，应该先作为新的观测事实保留。

#### 13.5.5 `deep_buffer_candidate` 出现了明显的双峰形态

`deep_buffer_candidate` 的 raw run 里出现了两个很清晰的簇：

- 一组：
  - `memory_pause_count = 0`
  - `max_memory ≈ 132 ~ 150MiB`
  - `avg_network_speed ≈ 719 ~ 722 MB/s`
- 另一组：
  - `memory_pause_count = 16`
  - `max_memory ≈ 256MiB`
  - `avg_network_speed ≈ 486 ~ 495 MB/s`

这说明这个 case 现在不是简单的“更深 queue 更快”或“更深 queue 更慢”，而是：

- 一旦运行态跨过某个 memory 区间，就会掉进明显更差的一种模式

这是这轮额外暴露出来的高价值开放问题。

### 13.6 需要谨慎解读的地方

这轮 smoke 里，`baseline_default` / `balanced_candidate` / `memory_guard`
的 `queue_full_pause_capacity_reached_count_median` 都非零，而
`queue_full_pause_try_enqueue_failure_count_median = 0`。

这里不能直接写成“现在 pause 已经完全变成逻辑 packet cap 触发”。

原因是：

- `start_queue_pause()` 仍然只会在 enqueue 失败后进入
- `capacity_reached` 只是当前本地分类分支：失败发生时，`queued_packets >= queue_capacity_packets`

所以更准确的表述应是：

- 在这轮 smoke 里，enqueue failure 发生时，queue 深度已经高于我们配置的 packet cap
- 这个现象与 `2026-03-13` rerun 里的“更早的 try_enqueue failure”不同
- 但它还不是新的最终根因结论

这一步应先保留为新观测，而不是直接覆盖上一轮已经写明的第三方 queue 机制结论。

### 13.7 当前更收敛的结论

这轮 instrumentation 之后，可以先把当前链路拆解收敛到下面几层：

1. `scheduler_window_bytes * max_connections` 不是当前主 case 的首个硬上限。
2. 当前大部分 `inflight` 几乎就是 queue payload 本身，而不是 queue 外滞留。
3. queue pause 和 memory pause 的进入点，基本都发生在“queue payload 已经接近总 inflight / memory”的阶段。
4. 因此下一步如果还要继续拆 budget chain，优先级应该放在：
   - queue payload 为什么能涨到当前这个量级
   - memory watermark 为什么在某些 case 上形成分叉运行态
   - `deep_buffer_candidate` 的双峰模式到底由什么切换出来

换句话说，下一步最值得优先追的，不再是“queue 外还有没有一大段隐形 inflight”，因为这轮观测已经基本把那条路径降级了。

## 14. 优化迭代 011：Memory pause 触发分层闭环（采纳）

### 14.1 背景

在上一轮已经确认：

- `deep_buffer_candidate` 存在明显双峰
- 慢簇会伴随 `memory_pause_count > 0`
- 但新加的 `memory_high_watermark_episode_count` / `memory_low_watermark_recovery_count`
  在 `deep_buffer_candidate` 里没有亮起来

这意味着还存在一个机制层空洞：

- 如果慢簇真的是“已经越过 high watermark，再进入 memory backpressure episode”，
  那么 episode 指标不应持续为 `0`
- 反过来，如果 episode 指标始终不亮，就说明当前 `memory_pause` 很可能在更早的地方已经被触发

### 14.2 本轮补充内容

为此，这轮又补了一组更小但更直接的触发分层指标：

- `memory_pause_pre_high_watermark_count`
- `memory_pause_at_or_above_high_watermark_count`
- `memory_pause_start_high_watermark_gap_bytes_total`
- `memory_pause_start_high_watermark_gap_bytes_avg`

接入点保持在最小范围：

- 仍然只在 [download_engine.cpp](/D:/git_repository/coding_with_agents/AsyncDownload/src/download/download_engine.cpp)
  的 `start_memory_pause()` 里判断
- 判断条件直接用当前 `memory_bytes` 与 `backpressure_high_bytes` 比较

同时继续保留上一轮新增的 episode 级指标：

- `memory_high_watermark_episode_count`
- `memory_low_watermark_recovery_count`

这样可以把 memory backpressure 分成两类：

1. **pre-high pause**
   - 还没真正站上 high watermark，就因为下一包会越线而提前 pause
2. **at/above-high pause**
   - 已经在 high watermark 上方，再由事件循环里的 memory backpressure 路径处理

### 14.3 机制闭环

为了避免只凭现象命名，这轮补读了本地实现：

- [memory_accounting.cpp](/D:/git_repository/coding_with_agents/AsyncDownload/src/core/memory_accounting.cpp)

关键规则在 `should_pause_for_backpressure()`：

- `incoming_bytes == 0` 时不 pause
- `current_bytes == 0` 时不 pause
- 否则只要 `current_bytes + incoming_bytes > high_watermark` 就返回 `true`

这条规则解释了为什么会出现：

- `memory_pause_count > 0`
- 但 `memory_high_watermark_episode_count = 0`

因为当前 write callback 的 memory pause 可以在“当前 memory 仍略低于 high watermark”时，就由于**下一次 incoming packet 会越线**而提前触发。

### 14.4 验证结果

基础验证通过：

- `scripts\build.bat release`
- `ctest -C Release --output-on-failure -E "DownloadIntegrationTest.*"`
- `AsyncDownload_tests.exe --gtest_filter=DownloadIntegrationTest.ReportsDetailedProgressSnapshot`

随后对 `deep_buffer_candidate` 单独跑了两轮 `12` 次重复 benchmark：

- `build/benchmarks/20260314_102246_deep-buffer-memory-watermark-episode`
- `build/benchmarks/20260314_102600_deep-buffer-memory-pause-trigger-split`

最终以第二轮分层触发指标为主做结论。

### 14.5 新结论

#### 14.5.1 慢簇是 pre-high pause，不是越线后的 high-watermark episode

在 [20260314_102600_deep-buffer-memory-pause-trigger-split](/D:/git_repository/coding_with_agents/AsyncDownload/build/benchmarks/20260314_102600_deep-buffer-memory-pause-trigger-split/aggregated_cases.csv) 里：

- `memory_high_watermark_episode_count_median = 0`
- `memory_low_watermark_recovery_count_median = 0`
- `memory_pause_at_or_above_high_watermark_count_median = 0`

而 raw run 里所有慢簇都满足：

- `memory_pause_count = 16`
- `memory_pause_pre_high_watermark_count = 16`
- `memory_pause_at_or_above_high_watermark_count = 0`

这已经足以把机制层闭环到：

- 慢簇不是“系统先超过 high watermark，然后再靠 episode drain 回到 low watermark”
- 慢簇是 write callback 里**预防 incoming packet 越线**的 memory pause 连续命中

#### 14.5.2 触发点离 high watermark 很近

慢簇里：

- `memory_pause_start_memory_bytes_avg ≈ 268,421,356 ~ 268,423,118`
- `backpressure_high_bytes = 268,435,456`
- `memory_pause_start_high_watermark_gap_bytes_avg ≈ 12,338 ~ 14,100`

这说明 pre-high pause 触发时，系统离 high watermark 只差大约 `12-14 KiB`。

也就是说，当前不是“早早就被 conservative memory policy 打断”，而是：

- 运行态已经把 queue payload 推到了几乎贴线的位置
- 下一包再进来就会越线
- 因此 callback 在最后这几 KiB 处开始频繁触发 memory pause

#### 14.5.3 双峰切换点已经从“low watermark 恢复太慢”降级到“是否进入 pre-high pause 串”

在这轮 `12` 次里，形态很清晰：

- 快簇：
  - `~687-735 MB/s`
  - `memory_pause_count = 0`
  - `max_memory ≈ 119-145 MiB`
- 慢簇：
  - `~468-586 MB/s`
  - `memory_pause_count = 16`
  - `memory_pause_pre_high_watermark_count = 16`
  - `max_memory ≈ 256 MiB`

因此当前更准确的说法应变成：

- `deep_buffer_candidate` 的主要模式切换点，不是“是否进入 high-watermark 之后的事件循环 episode”
- 而是“运行态是否会把 queue payload 推到 high watermark 附近，并开始反复命中 pre-high pause”

### 14.6 对下一步的影响

这轮之后，下一步优先级应进一步收敛：

1. 不再优先假设问题在 `apply_memory_backpressure()` 或 low-watermark 恢复本身。
2. 优先追问：为什么同一组配置里，有些 run 会把 queue payload 推到 `~256MiB` 附近，而另一些 run 稳定停在 `~120-145MiB`。
3. 更具体地说，应继续拆：
   - packet 进入 queue 的节奏
   - flush / persist 消费回落的节奏
   - callback 侧 memory headroom 被吃光前的最后一段增长轨迹

换句话说，下一轮最该继续补的，不是“high watermark 之后发生了什么”，而是“为什么某些 run 能在 high watermark 之前一路冲到只剩 `~12-14 KiB` headroom”。

## 15. 优化迭代 012：Callback pre-high headroom / local-buffer 分层闭环（采纳）

这一轮的目标不是直接改背压语义，而是继续回答上轮遗留的问题：

- `deep_buffer_candidate` 的 pre-high pause 到底是被单次 incoming chunk 打出来的，还是被多个 handle 的本地聚合 buffer 叠出来的。
- callback 本地 buffer 在整个 budget 链里到底是主积压，还是只是在最后几 KiB 里充当触发器。

这轮最终采纳的指标包括：

- `max_active_buffered_accounted_bytes`
- `memory_pause_start_incoming_bytes_*`
- `memory_pause_start_delta_accounted_bytes_*`
- `memory_pause_start_current_handle_buffered_payload_bytes_*`
- `memory_pause_start_current_handle_buffered_accounted_bytes_*`
- `memory_pause_start_projected_handle_buffered_payload_bytes_*`
- `memory_pause_start_projected_handle_buffered_accounted_bytes_*`
- `memory_pause_start_active_buffered_accounted_bytes_*`

中途曾短暂尝试过 `active_buffered_payload` 聚合口径，但该口径在 flush / move 路径上不稳定，不能作为最终结论依据。对应目录：

- `build/benchmarks/20260314_103729_deep-buffer-buffered-headroom-split`
- `build/benchmarks/20260314_103907_deep-buffer-buffered-headroom-split-rerun`
- `build/benchmarks/20260314_104258_deep-buffer-buffered-headroom-split-v2`

最终以 [20260314_104712_deep-buffer-buffered-accounted-split](/D:/git_repository/coding_with_agents/AsyncDownload/build/benchmarks/20260314_104712_deep-buffer-buffered-accounted-split/aggregated_cases.csv) 为准做结论。

基础验证通过：

- `scripts\build.bat release`
- `ctest -C Release --output-on-failure -E "DownloadIntegrationTest.*"`
- `AsyncDownload_tests.exe --gtest_filter=DownloadIntegrationTest.ReportsDetailedProgressSnapshot`

### 15.1 新结论

#### 15.1.1 pre-high 慢簇不是“单次大包”，而是“多 handle 本地 partial packet 叠加后的最后一跳”

在最终采纳的 benchmark 里，唯一命中 memory pause 的慢 run 是 repeat `3`：

- `avg_network_speed_mb_s = 598.83`
- `memory_pause_count = 16`
- `memory_pause_pre_high_watermark_count = 16`
- `memory_pause_start_high_watermark_gap_bytes_avg = 11,906`
- `memory_pause_start_incoming_bytes_avg = 16,384`
- `memory_pause_start_delta_accounted_bytes_avg = 16,384`
- `memory_pause_start_current_handle_buffered_payload_bytes_avg = 35,840`
- `memory_pause_start_projected_handle_buffered_payload_bytes_avg = 52,224`
- `memory_pause_start_active_buffered_accounted_bytes_avg = 574,464`

这组数据说明：

- 触发 pre-high pause 的最后一跳只是一笔常规的 `~16 KiB` callback incoming
- 当前 handle 在触发前通常已经本地攒了 `~35 KiB`
- projected 后会变成 `~52 KiB`，离单 packet `64 KiB` 聚合上限并不远
- 同时，其它活跃 handle 的本地 partial packet 也在一起占用约 `~560 KiB` accounted 空间

因此这轮可以把机制层结论推进到：

- pre-high pause 不是“某个异常大 chunk 把系统一下打爆”
- 而是 queue payload 已经逼近 high watermark 时，多个 handle 的本地 partial packet 再叠上最后一个 `~16 KiB` incoming，把 callback admission 推过红线

#### 15.1.2 callback 本地 buffer 不是主积压体量，但它确实是最后的触发器

同一轮里，所有 run 的 `max_active_buffered_accounted_bytes` 只在 `951,296 ~ 1,049,600` 之间波动。

这和 `16` 个连接、每个连接一个 `~64 KiB` 聚合 packet 的上界是同一量级。

这说明：

- callback 本地聚合 buffer 本身只是一层 `~1 MiB` 量级的小预算
- 真正占据绝大多数 inflight / memory 的仍然是 queue payload 本体
- 但当 queue payload 已经把系统推到离 high watermark 只剩十几 KiB 时，这层 `~1 MiB` 级的 local buffer 会决定“最后一个 incoming 能不能进来”

也就是说，callback local buffer 在当前机制里不是主积压来源，但确实是 pre-high pause 的最后触发器。

#### 15.1.3 同时暴露了第二类“无 memory pause 也会变慢”的运行态

这轮还有几次慢 run 完全没有命中 memory pause：

- repeat `12`
  - `avg_network_speed_mb_s = 533.06`
  - `memory_pause_count = 0`
  - `max_queued_packets = 3,581`
  - `max_queued_payload_bytes = 230,572,934`
- repeat `8`
  - `avg_network_speed_mb_s = 562.64`
  - `memory_pause_count = 0`
  - `max_queued_payload_bytes = 126,140,718`
  - `handle_data_packet_avg_us = 202.07`
  - `append_bytes_avg_us = 201.32`
  - `file_write_avg_us = 112.36`

这说明当前至少还存在两类慢态：

- 一类是已经闭环的 pre-high pause 慢态
- 另一类不是由 memory pause 直接触发
  - 有时表现为 queue payload 自己长到 `~230 MiB`
  - 有时表现为 `handle_data_packet / append / file_write` 整体时延同步抬高

因此 callback 侧新指标已经解释了“为什么会进入 pre-high pause 串”，但还没有解释所有吞吐双峰。

### 15.2 对下一步的影响

下一步优先级应继续收敛，但要分成两条：

1. 已闭环主线：
   - pre-high pause 的最后触发条件已经足够清楚
   - 后续不必再把它假设成“单个异常大 incoming chunk”问题
2. 新的主开放问题：
   - 为什么有些 run 在完全没有 memory pause 的情况下，queue payload 仍会长到 `~230 MiB`
   - 为什么另一些慢 run 会表现成 `handle_data_packet / append / file_write` 同步变慢

这意味着下一轮最值得补的，不再是 callback 本地 buffer 的口径，而是：

- queue payload 增长到 `~230 MiB` 时，persistence 侧到底发生了什么节奏变化
- 无 pause 慢 run 里，`handle_data_packet` 和 `file_write` 的时延抬升是 OS / 文件写入波动，还是当前实现里的另一个队列化路径

## 16. 优化迭代 013：Persistence write-shape / flush contention 拆解（部分采纳）

这一轮的目标是把上一轮遗留的“无 memory pause 也会变慢”继续压缩到 persistence 直写路径内部。

这轮最终保留的是写形态观测，不保留实验代码。新增并采纳的指标包括：

- `aligned_write_calls_total`
- `aligned_write_bytes_total`
- `tail_write_calls_total`
- `tail_write_bytes_total`
- `average_aligned_write_size_bytes`
- `average_tail_write_size_bytes`

对应采纳 benchmark：

- [20260314_111232_deep-buffer-write-shape-split](/D:/git_repository/coding_with_agents/AsyncDownload/build/benchmarks/20260314_111232_deep-buffer-write-shape-split/aggregated_cases.csv)

对应 reject benchmark：

- [20260314_111437_deep-buffer-flush-backlog-gate-exp1](/D:/git_repository/coding_with_agents/AsyncDownload/build/benchmarks/20260314_111437_deep-buffer-flush-backlog-gate-exp1/aggregated_cases.csv)
- [20260314_111653_deep-buffer-split-filewriter-handles-exp2](/D:/git_repository/coding_with_agents/AsyncDownload/build/benchmarks/20260314_111653_deep-buffer-split-filewriter-handles-exp2/aggregated_cases.csv)

基础验证通过：

- `scripts\build.bat release`
- `ctest -C Release --output-on-failure -E "DownloadIntegrationTest.*"`
- `AsyncDownload_tests.exe --gtest_filter=DownloadIntegrationTest.ReportsDetailedProgressSnapshot`

### 16.1 新结论

#### 16.1.1 写入大小形态不是当前慢态分叉的主因

在采纳数据 [20260314_111232_deep-buffer-write-shape-split](/D:/git_repository/coding_with_agents/AsyncDownload/build/benchmarks/20260314_111232_deep-buffer-write-shape-split/raw_runs.csv) 里：

- `aligned_write_calls_total` 稳定在 `16,414 ~ 16,433`
- `tail_write_calls_total` 稳定在 `13,354 ~ 14,560`
- `average_aligned_write_size_bytes` 稳定在 `~61.8 KiB`
- `average_tail_write_size_bytes` 恒定为 `4,096`

即使是无 `memory_pause` 的较慢 run，例如 repeat `7`：

- `avg_network_speed_mb_s = 620.98`
- `memory_pause_count = 0`
- `max_queued_payload_bytes = 252,444,066`
- `average_aligned_write_size_bytes = 61,826.97`
- `average_tail_write_size_bytes = 4,096`

它的 write shape 仍然与快 run 基本同构。

这说明：

- 当前慢态不是因为“写成了更多更碎的小块”
- `tail flush` 4KiB 写入本身一直存在，但它没有解释吞吐双峰

#### 16.1.2 更强的信号来自 pending flush 期间的写入退化

同一组采纳数据里，慢 run 与快 run 的 `flush_pending_write_avg_us` 和 `flush_time_ms_total` 呈现明显分层：

- pre-high memory pause 慢 run
  - repeat `4`: `481.66 MB/s`，`flush_pending_write_avg_us = 40.08`，`flush_time_ms_total = 721`
  - repeat `12`: `534.45 MB/s`，`flush_pending_write_avg_us = 40.69`，`flush_time_ms_total = 761`
- 无 pause 但 queue payload 很高的慢 run
  - repeat `7`: `620.98 MB/s`，`memory_pause_count = 0`，`max_queued_payload_bytes = 252,444,066`
  - 同时 `flush_pending_write_avg_us = 27.33`，`flush_time_ms_total = 306`
- 快 run
  - repeat `8`: `746.90 MB/s`，`flush_pending_write_avg_us = 22.71`，`flush_time_ms_total = 193`
  - repeat `3`: `743.11 MB/s`，`flush_pending_write_avg_us = 22.79`，`flush_time_ms_total = 208`

结合 [persistence_thread.cpp](/D:/git_repository/coding_with_agents/AsyncDownload/src/persistence/persistence_thread.cpp) 里的 `maybe_schedule_flush()` 和 [file_writer.cpp](/D:/git_repository/coding_with_agents/AsyncDownload/src/storage/file_writer.cpp) 里的 `write()` / `flush()` 共用同一把锁，可以把当前结论推进到机制层强信号：

- 真正拖慢 persistence 主链的，更像是 pending flush 期间的写入阻塞
- 慢态分叉更接近 flush 持锁时间与直写路径的串行化，而不是写入批次形态本身

#### 16.1.3 两个尝试性改法都不是 keeper

实验一：`queue backlog` 高时延后 threshold flush

- 目录：[20260314_111437_deep-buffer-flush-backlog-gate-exp1](/D:/git_repository/coding_with_agents/AsyncDownload/build/benchmarks/20260314_111437_deep-buffer-flush-backlog-gate-exp1/aggregated_cases.csv)
- 结果：`flush_count` 仍然固定在 `3`
- 中位吞吐从 `721.41 MB/s` 降到 `699.22 MB/s`
- 还出现了 `memory_pause_count = 32` 的 run

这说明当前主问题不是“threshold flush 发起得太早”，而是“flush 一旦运行，后面的写路径会怎样被拖慢”。

实验二：Windows 下拆分 `FileWriter` 的写句柄与 flush/read 句柄

- 目录：[20260314_111653_deep-buffer-split-filewriter-handles-exp2](/D:/git_repository/coding_with_agents/AsyncDownload/build/benchmarks/20260314_111653_deep-buffer-split-filewriter-handles-exp2/aggregated_cases.csv)
- 中位吞吐从 `721.41 MB/s` 掉到 `593.20 MB/s`
- `flush_pending_write_avg_us` 没有下降到更优区间
- 新增了多次无 `memory_pause` 的重慢 run，例如 repeat `11 = 411.41 MB/s`

这说明“简单拆用户态句柄”并没有消除当前 slow mode，至少在现有 Windows I/O 语义下不是 keeper 方向。

### 16.2 对下一步的影响

下一轮如果继续做 keeper 级优化，优先级应改成：

1. 把 `flush` 对主写链的阻塞语义当成主问题，而不是继续拆 write shape。
2. 不再重复尝试“微调 threshold flush 触发条件”或“简单拆句柄”这两类方向，除非能明确说明实现语义为什么不同。
3. 若继续改 flush 路径，应先正面讨论 durability / metadata cadence 的设计边界，因为更激进的优化很可能会触及恢复语义，而不只是热路径局部优化。

## 17. 优化迭代 014：Incremental CRC sample cache 探索（未采纳）

### 17.1 背景

在上一轮已经确认：

- `pending_flush` 期间的写路径阻塞比 write shape 本身更像主卡点
- 但把 `flush_time_ms_total`、`metadata_save_time_ms_total`、`crc_sample_read_*` 合起来看时，CRC 采样读取本身也不是小数

以 [20260314_111232_deep-buffer-write-shape-split](/D:/git_repository/coding_with_agents/AsyncDownload/build/benchmarks/20260314_111232_deep-buffer-write-shape-split/raw_runs.csv) 为例：

- 快 run 的 `crc_sample_read` 估算总时长通常也在 `~165-180ms`
- 慢 run 则可能连同 `FlushFileBuffers` 一起把 pending flush 窗口推到 `~800-1000ms`

而 [persistence_thread.cpp](/D:/git_repository/coding_with_agents/AsyncDownload/src/persistence/persistence_thread.cpp) 里的 `build_crc_samples()` 每次 flush 都会从 snapshot 位图重新遍历 `VDL` 之后的 `finished` block，并重新读盘做 CRC。

因此这轮想验证的问题是：

- “重复 CRC 采样读”是否是当前 pending flush 的高占比重复工作
- 如果把这部分改成增量缓存，能否形成 keeper 级吞吐改善

### 17.2 实验内容

实验代码做了两件事：

1. 给 `build_crc_samples()` 增加基于 offset 的增量缓存，只对新出现的 `finished` block 做实际读盘和 CRC 计算。
2. 额外导出 `crc_sample_reused_blocks_total` / `crc_sample_reused_bytes_total`，确认这条缓存路径有没有真正命中。

同时补了一个定向单测，证明在同一 `VDL` 之后的 `finished` block 跨多轮 flush 时确实可以命中 reuse。

最终实验代码没有保留在工作树中。

### 17.3 验证结果

基础验证通过：

- `scripts\build.bat release`
- `ctest -C Release --output-on-failure -E "DownloadIntegrationTest.*"`
- `AsyncDownload_tests.exe --gtest_filter=DownloadIntegrationTest.ReportsDetailedProgressSnapshot`
- `AsyncDownload_tests.exe --gtest_filter=PersistenceThreadTest.ReusesCrcSamplesAcrossFlushes`

随后对 `deep_buffer_candidate` 做了两轮 `12` 次重复 benchmark：

- [20260314_120417_deep-buffer-incremental-crc-cache-exp1](/D:/git_repository/coding_with_agents/AsyncDownload/build/benchmarks/20260314_120417_deep-buffer-incremental-crc-cache-exp1/aggregated_cases.csv)
- [20260314_120554_deep-buffer-incremental-crc-cache-exp1-rerun](/D:/git_repository/coding_with_agents/AsyncDownload/build/benchmarks/20260314_120554_deep-buffer-incremental-crc-cache-exp1-rerun/aggregated_cases.csv)

### 17.4 新结论

#### 17.4.1 增量 CRC cache 确实命中了真实重复工作

这轮不是“假设命中”，而是有明确运行数据：

- 第一轮里，所有 run 的 `crc_sample_reused_blocks_total` 都在 `~1131-1227`
- `crc_sample_blocks_total` 则从上一轮常见的 `~5800-8000` 降到了 `~3900-4900`

这说明：

- `build_crc_samples()` 里确实存在大量跨 flush 的重复 CRC 重读
- 增量 cache 在实现层是有效命中的，不是空转逻辑

#### 17.4.2 但它没有把 `deep_buffer_candidate` 收敛成 keeper 形态

尽管 CRC 读盘量明显下降，benchmark 结果仍然不稳定：

- 第一轮：
  - 中位吞吐 `703.54 MB/s`
  - 仍出现 `451.10 MB/s`、`458.17 MB/s` 两个无 `memory_pause` 的重慢 run
- 第二轮 rerun：
  - 中位吞吐 `700.66 MB/s`
  - 又出现 `440.43 MB/s` 的无 `memory_pause` 重慢 run

而上一轮作为对照的 [20260314_111232_deep-buffer-write-shape-split](/D:/git_repository/coding_with_agents/AsyncDownload/build/benchmarks/20260314_111232_deep-buffer-write-shape-split/aggregated_cases.csv) 中位吞吐是 `721.41 MB/s`。

这说明：

- CRC repeated read 是真实成本
- 但把它缓存掉之后，`deep_buffer_candidate` 仍会进入另一类无 pause 慢态
- 所以它不是当前 keeper 级根因

#### 17.4.3 当前更像“次级成本被削弱了，但主阻塞还在”

从 slow run 结构看：

- 某些 run 的 `crc_sample_read` 总量确实下降了
- 但 `flush_pending_write_avg_us` 和 `flush_time_ms_total` 仍然能形成明显的慢态分叉
- 特别是 rerun 里的重慢 run `repeat 5 = 440.43 MB/s`，仍然是在 `memory_pause_count = 0` 的情况下出现

因此更稳妥的机制层结论应是：

- 重复 CRC 读盘是 pending flush 窗口里的真实次级成本
- 但当前主链继续被卡住，更强信号仍然在 `FlushFileBuffers` / pending-flush 阻塞这一层

### 17.5 为什么不采纳

这轮之所以 reject，不是因为实验没命中，而是因为：

1. 它命中了真实重复工作，但 benchmark 结果没有形成 keeper 级收益。
2. 两轮 `12` 次 rerun 都出现了新的无 `memory_pause` 重慢 run，说明这条路无法稳定收敛当前双峰。
3. 当前更像是“减少了一个可见成本项”，但没有改变主阻塞的决定性结构。

因此这轮不保留实现代码，只保留文档结论。

### 17.6 对下一步的影响

下一步如果继续追 flush 主线，应把优先级继续收窄为：

1. 把 `FlushFileBuffers` 与 pending-flush 阻塞当成主解释面。
2. 不再把“简单的 incremental CRC cache”当成新的 keeper 候选，除非能明确说明还有别的机制差异。
3. 若后续要继续动 flush / metadata / CRC 语义，必须先明确恢复语义愿意接受的 tradeoff，而不是只从热路径局部出发。

## 18. 优化迭代 015：Dedicated CRC read handle 探索（未采纳）

### 18.1 背景

在增量 CRC cache 被 reject 之后，flush 主线里还剩一个可疑点：

- `build_crc_samples()` 的读盘与 `write()` / `flush()` 仍共用同一个 `FileWriter` 句柄和同一把互斥锁
- 之前 reject 的“拆 FileWriter 写/flush 句柄”实验同时改了写和 flush 语义，无法单独回答“只把 CRC read 拆出去会不会有帮助”

因此这轮只做一个更窄的实验：

- 保持 `write()` 与 `flush()` 仍走原来的主句柄
- 仅在 Windows 下为 `read()` 额外打开一个只读句柄，试图把 CRC sample read 从主写句柄的锁竞争里拆开

### 18.2 验证结果

基础验证通过：

- `scripts\build.bat release`
- `ctest -C Release --output-on-failure -E "DownloadIntegrationTest.*"`
- `AsyncDownload_tests.exe --gtest_filter=DownloadIntegrationTest.ReportsDetailedProgressSnapshot`

随后对 `deep_buffer_candidate` 做了 12 次定向 benchmark：

- [20260314_175014_deep-buffer-dedicated-read-handle-exp1](/D:/git_repository/coding_with_agents/AsyncDownload/build/benchmarks/20260314_175014_deep-buffer-dedicated-read-handle-exp1/aggregated_cases.csv)

对照上一轮采纳基线 [20260314_111232_deep-buffer-write-shape-split](/D:/git_repository/coding_with_agents/AsyncDownload/build/benchmarks/20260314_111232_deep-buffer-write-shape-split/aggregated_cases.csv)：

- 基线中位吞吐：`721.41 MB/s`
- 实验中位吞吐：`662.44 MB/s`
- 基线均值：`662.31 MB/s`
- 实验均值：`606.61 MB/s`

并且实验里出现了更差的重慢 run：

- repeat `9 = 361.97 MB/s`，`memory_pause_count = 16`，`flush_pending_write_avg_us = 44.51`
- repeat `1 = 398.13 MB/s`，`memory_pause_count = 0`，`flush_time_ms_total = 349`
- repeat `8 = 409.93 MB/s`，`memory_pause_count = 16`，`flush_pending_write_avg_us = 45.30`

### 18.3 结论

这轮给出的信号很直接：

1. 只把 CRC read 拆到独立只读句柄，并没有把当前 slow mode 消掉。
2. `flush_pending_write_avg_us` 仍然能抬到比基线更差的区间，说明主阻塞并不在“CRC read 与主句柄共用”这一条简单解释上。
3. 在现有 Windows I/O 语义下，这条路不仅没形成 keeper 收益，反而引入了更坏的尾部结果。

因此这轮实验不采纳，代码已撤回，只保留 reject 结论。

### 18.4 对下一步的影响

后续如果还要重访 “read / flush / write 句柄分离” 方向，必须先能说明：

- 为什么新的实现语义不同于此前 reject 的“拆 FileWriter 写/flush 句柄”
- 为什么它不只是另一种“简单拆句柄”

在没有新机制差异前，不应重复进入这条路线。

## 19. 优化迭代 016：Compact metadata JSON 探索（未采纳）

### 19.1 背景

在继续阅读 [metadata_store.cpp](/D:/git_repository/coding_with_agents/AsyncDownload/src/metadata/metadata_store.cpp) 之后，可以看到：

- 每次 flush 结束后都会写一份 JSON metadata
- 当前序列化使用 `value.dump(2)`，也就是格式化输出

这条改动不触碰 durability / recovery 语义，只会减少 metadata 文件体积和文本格式化开销，因此是一个低风险、低侵入的实验候选。

### 19.2 实验内容

实验只做一处修改：

- 把 `MetadataStore::save()` 中的 `value.dump(2)` 改成 `value.dump()`

也就是保留相同 JSON 字段与保存顺序，只去掉缩进格式化。

### 19.3 验证结果

基础验证通过：

- `scripts\build.bat release`
- `ctest -C Release --output-on-failure -E "DownloadIntegrationTest.*"`
- `AsyncDownload_tests.exe --gtest_filter=DownloadIntegrationTest.ReportsDetailedProgressSnapshot`

随后对 `deep_buffer_candidate` 做了 12 次定向 benchmark：

- [20260314_175523_deep-buffer-compact-metadata-json-exp1](/D:/git_repository/coding_with_agents/AsyncDownload/build/benchmarks/20260314_175523_deep-buffer-compact-metadata-json-exp1/aggregated_cases.csv)

对照上一轮采纳基线 [20260314_111232_deep-buffer-write-shape-split](/D:/git_repository/coding_with_agents/AsyncDownload/build/benchmarks/20260314_111232_deep-buffer-write-shape-split/aggregated_cases.csv)：

- 基线中位吞吐：`721.41 MB/s`
- 实验中位吞吐：`706.46 MB/s`
- 基线均值：`662.31 MB/s`
- 实验均值：`613.72 MB/s`

实验仍然出现了新的重慢 run：

- repeat `3 = 401.25 MB/s`，`memory_pause_count = 0`，`flush_time_ms_total = 216`
- repeat `12 = 412.90 MB/s`，`memory_pause_count = 16`，`flush_time_ms_total = 818`
- repeat `10 = 485.08 MB/s`，`memory_pause_count = 16`，`flush_time_ms_total = 728`

同时 metadata 保存时长没有形成决定性变化：

- 基线慢 run 里 `metadata_save_time_ms_total` 已多在 `62-88ms`
- 这轮慢 run 里仍在 `64-81ms` 区间

### 19.4 结论

这说明：

1. metadata pretty-print 不是当前 `deep_buffer_candidate` 的决定性成本项。
2. 去掉缩进只能略微减少 metadata 自身开销，但不会改变 `FlushFileBuffers` / pending-flush 阻塞所主导的运行态。
3. 当前双峰慢态在无 pause 和 pre-high pause 两条支路上都仍然存在。

因此这轮实验不采纳，代码已撤回，只保留 reject 结论。

### 19.5 对下一步的影响

当前还值得继续追的方向已经越来越集中到：

1. `FlushFileBuffers` 本身的持锁阻塞语义
2. flush 后 metadata/CRC 这整段 pending-flush 窗口如何与主写链并存

而“简单缩小 metadata 文件体积”这条路，在没有别的机制差异前不值得再重复。

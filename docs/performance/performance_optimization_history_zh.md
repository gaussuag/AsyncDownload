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

## 3. 当前状态总结

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

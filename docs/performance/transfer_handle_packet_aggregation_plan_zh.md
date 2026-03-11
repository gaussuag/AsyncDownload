# AsyncDownload 技术优化说明：TransferHandle 侧 Packet 聚合

## 1. 背景

当前版本的性能 benchmark 与 profiler 已经形成了两个比较稳定的结论：

1. 当前主 pause 路径是 `queue_full_pause`，不是 `gap_pause`
2. 当前最值得优先优化的两个热点是：
   - 网络回调中的小 packet 分配与拷贝
   - 持久化线程的 per-packet 消费链

基于当前代码与 profiler 行为基线，第一刀最值得尝试的方向是：

- 在 `TransferHandle` 上增加聚合缓冲
- 改成“每个 handle 先攒一批再入队”

这份文档用于说明该优化方向的技术背景、设计边界、实现思路、风险与验证方式。

## 2. 当前实现现状

### 2.1 数据流

当前主链路可以概括为：

1. `DownloadEngine` 驱动 `libcurl multi`
2. 每次 write callback 到来时，立刻构造一个 `DataPacket`
3. 把本次 callback 的 payload 拷贝进 `packet.payload`
4. 直接把这个 packet 入 `BlockingConcurrentQueue`
5. `PersistenceThread` 单线程出队
6. 每个 packet 再分别执行：
   - `lookup_range`
   - 顺序判断
   - `append_bytes`
   - 乱序 map 插入或 drain
   - bitmap 更新
   - inflight / persisted 统计更新

### 2.2 相关代码位置

- 网络回调与入队：
  - `src/download/download_engine.cpp`
- `DataPacket` 结构：
  - `src/core/models.hpp`
- 持久化线程消费：
  - `src/persistence/persistence_thread.cpp`
- 写盘：
  - `src/storage/file_writer.cpp`

### 2.3 当前实现的优点

当前方案的优点很明确：

- 控制流直观
- 每个 callback 的数据生命周期清楚
- packet 边界天然与 callback 边界一致
- 正确性语义比较容易维持

### 2.4 当前实现的问题

profiler 已经表明，当前实现正在付出很高的小包成本。

具体表现为：

- write callback 栈中频繁出现：
  - `std::vector<unsigned char>::_Assign_counted_range`
  - `std::_Allocate`
  - `operator new`
- 持久化线程热点链中频繁出现：
  - `PersistenceThread::handle_data_packet`
  - `PersistenceThread::append_bytes`
  - `FileWriter::write`

这意味着当前是典型的：

- 小 packet 太多
- 每个 packet 都有固定分配和拷贝成本
- 每个 packet 都会触发一次完整的持久化处理链

## 3. 当前问题本质

### 3.1 一回调一包

当前代码是“一次 libcurl write callback 基本等于一个 `DataPacket`”。

在当前环境中，packet 大小大约稳定在 `16KB` 左右，这会导致：

- 一个 `1GiB` 文件对应非常多的 packet
- 队列压力高
- `PersistenceThread` 以很细的粒度处理数据

### 3.2 两个热点被同一根因放大

当前两个热点实际上有共同根因：

- `packet` 粒度太细

它同时放大了：

1. 网络侧的 packet 构造、分配、拷贝成本
2. 持久化侧的 per-packet 固定成本

所以，优先减少 packet 数量，有机会同时改善两个热点。

## 4. 优化目标

本次优化不追求一次性解决所有问题，目标限定为：

1. 降低 `DataPacket` 数量
2. 降低 write callback 中的分配与拷贝次数
3. 降低 `PersistenceThread` 的 per-packet 处理频率
4. 不改变当前恢复语义与 VDL / bitmap / CRC 的正确性
5. 不改变 range / window 调度模型

## 5. 核心方案

### 5.1 方案概述

在 `TransferHandle` 上新增一个聚合缓冲，不再把每次 callback 数据立刻入队，而是：

1. 把 callback 数据先追加到当前 handle 的本地缓冲
2. 当缓冲达到阈值时，再封装成一个 `DataPacket`
3. 一次性入队

这样，逻辑从：

- “callback 粒度入队”

变成：

- “聚合阈值粒度入队”

### 5.2 建议的第一版聚合粒度

建议第一版阈值使用：

- `64KB`

理由：

- 与当前 `block_size=64KB` 一致
- 明显大于当前约 `16KB` 的 callback 粒度
- 可以在不引入过高复杂度的前提下，显著减少 packet 数量
- 对 tail、window 结束和错误收尾都比较容易处理

## 6. 建议的结构变化

### 6.1 `TransferHandle` 新增状态

建议在 `TransferHandle` 中新增：

- `buffered_offset`
  - 当前聚合缓冲对应的起始文件偏移
- `buffered_payload`
  - 当前聚合中的字节缓冲

必要时可再补：

- `buffer_active`
  - 当前是否已有待发送聚合片段

### 6.2 新的行为模型

write callback 到来后：

1. 如果当前聚合缓冲为空：
   - 以本次数据的逻辑起始 offset 作为 `buffered_offset`
   - 直接 append

2. 如果当前数据与缓冲是连续的：
   - 继续 append

3. 如果当前数据与缓冲不连续：
   - 先把已有缓冲 flush 成一个 `DataPacket`
   - 再开始新的缓冲

4. 如果缓冲达到阈值：
   - flush 成一个 `DataPacket`

## 7. 需要保证的正确性约束

这是这次优化最重要的部分。

### 7.1 单 handle 内的 offset 必须严格连续

聚合只允许发生在：

- 同一个 `TransferHandle`
- 同一个正在进行的 window
- 且逻辑 offset 连续

只要不连续，就必须立即先 flush 旧缓冲，再开启新缓冲。

### 7.2 任何可能结束当前 window 生命周期的时刻，都必须先 flush 聚合缓冲

至少包括：

- 当前 window 已满
- 当前 transfer 完成
- `curl` 请求结束
- 请求失败
- handle 被暂停前
- 网络阶段停止前

否则就会出现“callback 已经收到数据，但还没入队，后续状态又切换了”的丢数据风险。

### 7.3 聚合不能改变 packet 的逻辑顺序语义

即使一个 packet 由多次 callback 聚合而成，它仍然必须满足：

- `packet.offset` 是这批连续字节的真实起始偏移
- `packet.payload` 覆盖一个严格连续的字节区间

对 `PersistenceThread` 来说，它应当仍然像处理一个普通连续 packet 一样工作。

### 7.4 内存会计要同步调整

当前内存会计依赖：

- `payload`
- packet 固定开销
- map 节点开销

聚合后需要明确：

- 缓冲尚未入队前，这部分内存是否要进入全局内存会计
- 如果进入，应在聚合缓冲增长时同步累加
- flush 成 packet 后，不应重复计算

这一点必须设计清楚，否则背压判断会失真。

## 8. 建议的实现边界

### 8.1 第一版不做零拷贝

建议第一版不要追求：

- `DataPacket` 共享底层缓冲
- scatter/gather
- 自定义对象池

第一版目标应该是：

- 保守地把多个 callback 合成一个 packet
- 在现有 `DataPacket` 结构下先拿到收益

也就是说，第一版可以接受：

- callback 时 append 到 `TransferHandle::buffered_payload`
- flush 时把这个 `vector` move 进 `DataPacket`

先把“每 callback 一次分配/入队/出队/持久化处理”改成“每 64KB 一次”。

### 8.2 第一版不改持久化线程对外接口

第一版建议尽量保持：

- `PersistenceThread`
- `DataPacket`
- `FileWriter`

对外语义不变，只改变 packet 生成策略。

这样更适合把优化收益和风险限定在最小范围内。

## 9. 预期收益

如果实现正确，这个优化理论上会带来以下收益：

### 9.1 网络侧收益

- `vector` 分配次数下降
- payload 拷贝次数下降
- `DataPacket` 构造次数下降
- moodycamel 入队次数下降

### 9.2 持久化侧收益

- 出队次数下降
- `handle_data_packet()` 调用次数下降
- `append_bytes()` 调用次数下降
- 乱序 map 插入次数下降
- 统计更新和释放开销下降

### 9.3 系统级收益

- `queue_full_pause_count` 有机会下降
- `max_queued_packets` 有机会下降
- `avg_packet_size_bytes` 应明显上升
- 高并发路径更可能改善

## 10. 风险与副作用

### 10.1 正确性风险

最大的风险不是性能，而是：

- flush 时机漏掉导致数据遗失
- offset 不连续时聚合错误
- pause / complete / failure 路径下缓冲没吐干净

### 10.2 背压行为变化

因为 packet 数会下降：

- `queued_packets` 指标会变小
- `queue_capacity_packets` 的实际字节承载能力会变大

这意味着：

- 某些既有 benchmark 结果会出现形态变化
- 需要重新观察 `queue_full_pause_count`

### 10.3 内存行为变化

聚合缓冲会把一部分内存从“队列中 packet”转移到“handle 本地缓冲”。

如果内存会计没有处理好，就可能出现：

- 实际内存涨了，但背压统计没反映

## 11. 验证方案

### 11.1 正确性验证

至少验证：

1. 正常下载成功
2. 中途打断恢复成功
3. `Release` 下 benchmark 全部主要 case 不出现数据错误
4. 文件校验正确

### 11.2 benchmark 验证

改动后复跑：

```powershell
python scripts/performance/benchmark.py --url "http://127.0.0.1:4287/1gb_files.zip" --benchmark-suite regression --repeats 20 --label "post-change"
```

重点看：

- `baseline_default`
- `throughput_candidate`
- `scheduler_stress`
- `avg_packet_size_bytes`
- `packets_enqueued_total`
- `queue_full_pause_count`
- `max_queued_packets`

### 11.3 profiler 验证

改动后复跑：

```powershell
python scripts/performance/profiler.py --url "http://127.0.0.1:4287/1gb_files.zip" --benchmark-suite regression --case-list throughput_candidate,scheduler_stress --label "post-change-profile-compare"
```

重点看：

- `_Assign_counted_range`
- `_Allocate`
- `operator new`
- `PersistenceThread::handle_data_packet`
- `PersistenceThread::append_bytes`
- `FileWriter::write`

如果优化生效，理想现象是：

- packet 分配/拷贝栈明显降温
- 持久化线程 per-packet 热点占比下降

## 12. 当前建议

综合当前 benchmark、profiler 和代码结构，建议的执行顺序是：

1. 先实现 `TransferHandle` 聚合缓冲
2. 保持 `PersistenceThread` 接口语义不变
3. 先用 `64KB` 作为第一版聚合阈值
4. 先验证正确性与行为变化
5. 如果收益明确，再考虑第二阶段优化：
   - 减少 `PersistenceThread` 内部 per-packet 固定成本
   - 进一步优化 flush / metadata 干扰

## 13. 结论

当前这次优化方案的价值在于：

- 它同时命中当前两个主要热点
- 它不需要先改恢复语义
- 它不需要先推翻现有 range / window / persistence 设计
- 它更像是“降低工作单位个数”，而不是在现有小包模型上继续硬抠细节

因此，当前版本最值得先落地尝试的优化方向，就是：

- 在 `TransferHandle` 上增加聚合缓冲
- 改成“每个 handle 先攒一批再入队”

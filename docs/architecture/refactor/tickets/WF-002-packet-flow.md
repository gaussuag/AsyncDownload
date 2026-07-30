# WF-002：Packet Flow / Backpressure

- Type: `task`
- Status: `resolved`
- Depends on: `WF-001`
- Produces: `phases/02_packet_flow_backpressure.md`

## Question

如何让一个 deep module 独占 packet queue、逻辑预算、内存计费和 pause 生命周期，而不改变
现有性能语义？

## Investigation boundary

- 必须依据本地 moodycamel 源码定义 adapter 契约。
- 保留非阻塞 curl callback 与单 persistence writer。
- 不重试历史上已拒绝的 queue resume 实验。

## Close criteria

- 定义 producer/consumer interface、admission result、所有权和关闭协议。
- 定义 queue pause 与 memory pause 的独立状态机。
- 给出竞态、内存序、故障注入、性能 gate 和回滚路径。

## Resolution

选定“单 producer ordered stream + logical credits + RAII lease”的 Packet Flow interface。

详细实现合同：

- [阶段 2：Packet Flow / Backpressure](../phases/02_packet_flow_backpressure.md)

已关闭的决策：

- `FlowControlPolicy::packet_budget` 由 Packet Flow 的 logical admission credit 实现，不再把
  moodycamel constructor capacity 或 `try_enqueue()` failure 当成业务硬容量。
- 一个 orchestrator-confined `PacketProducer` 独占一个 explicit producer token；禁止
  implicit enqueue；custom traits 必须把 `INITIAL_IMPLICIT_PRODUCER_HASH_SIZE` 设为 0，
  不能误把 constructor 的 `maxImplicitProducers = 0` 当成禁用开关。
- Data Packet、Range Complete Control Packet 与 private close marker 共享同一 producer
  stream 和单调 sequence。
- Data Packet 按值携带 dependency-neutral `LeaseId`、half-open `ByteSpan` 与 offset；
  Range Complete 按值携带 `CompletionId` 与 exclusive `expected_end`。Packet Flow 只做
  结构/排序校验，generation、stale 和 range bounds 语义由阶段 3 Range Lifecycle 拥有。
- Control bypass Data admission gate并使用 allocating enqueue；真实 backend/allocation
  failure是 terminal error，不能静默丢弃。
- queue credit在consumer receive时归还；Accounted Bytes由move-only `PacketLease`持有到
  direct append、reorder drain、discard或abort完成。
- Queue Pause与Memory Pause使用独立reason bits；保持当前low-watermark恢复耦合和Top 20%
  memory selection，不混入历史reject的resume实验。
- persistence → orchestrator不通过Packet Flow建立通用反向事件总线；Gap和Range completion
  facts由后续Range Lifecycle拥有。
- close不再由PersistenceThread从第二个producer stream发布shutdown packet。
- `PacketLease` 使用 inline move-only state，不引入 per-packet pimpl allocation；
  `PacketProducer` / `PacketConsumer` 均不可复制、不可移动。
- production 只有 `MoodycamelPacketQueueAdapter`；fault adapter 是 test-only compile-time
  failure seam，不链接进产品，也不引入 hot-path virtual dispatch。
- control publish 与 reconcile 使用可表达 `published/closed/failed`、容量错误和无部分副作用
  的结果结构；所有改变状态的 cold entry 能报告错误。
- allocation/failure mapping、logical hard packet budget、request-local accounting 与
  close ordering 分别作为四个独立 C 类 `fix:` slice；各自有 red→green tests、Release
  evidence 和最小 rollback，不藏进结构删除提交。

Evidence：

- 当前实现符号：`TransferHandle`、`flush_transfer_buffer()`、
  `resume_paused_transfers()`、`PersistenceThread::process_loop()`、
  `PersistenceThread::release_packet_memory()`；
- vendored source：
  `blockingconcurrentqueue.h:48-53,119-169,204-253,369-385`，
  `concurrentqueue.h:343,414-420,685-700,803-847,1127-1145,1848-1950`；
- official libcurl contracts：`CURLOPT_WRITEFUNCTION` 与 `curl_easy_pause()`；
- performance source of truth：
  `performance_baseline_20260311_regression_v2_zh.md`、
  `optimization_regression_guide_zh.md`、
  `performance_optimization_history_zh.md`、
  `profiler_behavior_baseline_20260311_zh.md`。

合同已给出exact C++ types/signatures、ownership ledger、state machine、memory order、
close/failure protocol、tiny commits、deterministic/fault/concurrency tests、Release gate、
per-slice rollback、deletion checks与历史实验禁止项。

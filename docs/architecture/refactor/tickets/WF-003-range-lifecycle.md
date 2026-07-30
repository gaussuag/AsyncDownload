# WF-003：Range Lifecycle

- Type: `task`
- Status: `resolved`
- Depends on: `WF-002`
- Produces: `phases/03_range_lifecycle.md`

## Question

如何把 scheduler、engine 与 persistence 共同修改的 `RangeContext` 变成只有一个所有者的
显式状态机？

## Investigation boundary

- 保留 block 对齐、steal、release/acquire 发布与 persistence 确认完成。
- 不改变下载切分算法和默认 window 大小。
- 删除无真实读取方的生命周期字段时必须先证明无消费方。

## Close criteria

- 定义状态、事件、转换表、非法事件与重复事件语义。
- 定义 Lease 身份、防陈旧提交和网络失败回滚。
- 给出 scheduler、engine、persistence 的迁移顺序和测试。

## Resolution

选择“Orchestrator 单所有者 + Persistence Facts”的深模块形状。详细实现合同见
[阶段 3：Range Lifecycle 单所有者状态机](../phases/03_range_lifecycle.md)。

结论：

- `RangeLifecycle` 独占 Range 几何、阶段、gap 阻塞事实、Lease/Completion identity，不复制
  queue/memory/window pause；
- `RangeScheduler` 降为纯 initial/window/steal policy，不再写共享状态；
- Lifecycle 直接消费 Phase 1 `SchedulingPolicy + total_size`，不再复制第二套 `RangeRules`；
- `RangeWriteState` 只保留 Persistence 的 tail、乱序 map 与写盘前沿；
- 每个 HTTP window 使用单调 `LeaseId`，stale、duplicate、future generation 均有确定语义；
- network success 只进入 `awaiting_persistence`，matching `PersistenceCommitted` 是唯一
  `finished` 路径；
- donor/new Range 继续保持 block 对齐和现有 steal 阈值/中点算法；
- registration/resize 通过 submit → Persistence apply → ACK barrier，全部 success ACK
  后才允许 HTTP arm；Data/completion FIFO 与 fact slot 都有明确 release/acquire 合同；
- Persistence 以首代 1、同代 span 一致、新代只允许 +1 判定 future generation；
- fact snapshot 固定按 persisted → gap → completion 展开，失败终态的晚到 persisted fact
  同步推进 dispatch frontier但绝不重启调度；
- legacy metadata 字段在 Phase 3 继续兼容投影，运行时死字段在最终 slice 删除；
- 结构迁移与 allocation、duplicate/generation、accounting、completion correctness 分开，
  共 12 个 tests-first 小提交，每个都有验证、性能 gate 和 rollback frontier。

核实证据来自 `core::RangeContext`、`RangeScheduler`、`DownloadEngine`、
`PersistenceThread`、`AtomicBlockBitmap` 及现有 scheduler/persistence/resume tests。

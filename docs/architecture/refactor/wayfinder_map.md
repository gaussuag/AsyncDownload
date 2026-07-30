# Wayfinder Map：AsyncDownload 渐进式架构重构

## Destination

在不改变公开接口、CLI、恢复兼容性和已验证运行语义的前提下，把当前集中在
`DownloadEngine::run`、`RangeContext` 与 `PersistenceThread` 中的跨模块规则迁移到 6 个
具有高 leverage 和高 locality 的深模块：

1. Effective Download Policy
2. Packet Flow / Backpressure
3. Range Lifecycle
4. Recovery Checkpoint
5. HTTP Transfer
6. Telemetry Session

完成态必须同时满足：

- 每个模块有一个小而完整的 interface，调用方不再修改其内部状态。
- 第三方 queue 与 libcurl 的具体规则只存在于各自模块内。
- 恢复协议的提交顺序和可信度判断只有一个实现来源。
- 单元测试主要通过新 interface 验证行为，旧数据袋测试随迁移删除。
- 每个阶段在进入下一阶段前可单独合并、验证并回滚；一旦后续阶段已经依赖其 interface，
  回滚更早阶段必须先按逆序回滚 dependents。
- Release benchmark 的正式 10 项指标保持 schema 连续，结构阶段不默认追求吞吐提升。

## Notes

- 基线提交：`7a96af2b137055758bdc221872bdb26692ccf089`。
- Git 热点集中在 `src/download/download_engine.cpp`、`src/core/models.hpp` 和
  `src/persistence/persistence_thread.cpp`。
- `BlockingConcurrentQueue(capacity)` 的 capacity 是预分配下界，不是逻辑硬容量；
  `try_enqueue` 是 no-allocation 路径，返回 `false` 不能直接解释为“业务队列已满”。
- libcurl write callback 返回 `CURL_WRITEFUNC_PAUSE` 表示本批数据未消费，恢复后会重放。
- `align_up` / `align_down` 的 bit-mask 实现要求 alignment 为 2 的幂；当前公开 options 没有
  在库入口统一校验。
- `TailBuffer` 当前固定为 4096 bytes，而 `io_alignment` 是调用方可配置字段。
- 只有 persistence 路径可以确认 Range 已持久化完成。
- 性能历史已经拒绝简单放宽 queue resume、增加 hysteresis、单轮只恢复一个 handle、
  拆 FileWriter 句柄、incremental CRC cache 和 compact metadata JSON 等实验。

## Decisions so far

- [特征化与兼容基线](tickets/WF-000-characterization.md) — 固定兼容、环境失败和性能
  gate，结构重构默认保持性能语义。
- [Validated Download Policy](tickets/WF-001-policy.md) — 保留 `DownloadOptions` 兼容输入，
  内部只消费不可变 Effective Download Policy。
- [Packet Flow / Backpressure](tickets/WF-002-packet-flow.md) — 先隐藏 concrete queue、packet
  ownership 和 Queue/Memory Pause，再迁移 Range。
- [Range Lifecycle](tickets/WF-003-range-lifecycle.md) — 统一 Lease、stale/duplicate 与唯一
  持久化完成事实。
- [Recovery Checkpoint](tickets/WF-004-recovery.md) — 在统一 Range facts 后集中恢复信任和
  checkpoint 提交协议。
- [HTTP Transfer](tickets/WF-005-http-transfer.md) — 在内部 seams 稳定后隔离 libcurl，
  不让 HTTP 接管调度或恢复规则。
- [Telemetry Session](tickets/WF-006-telemetry.md) — Session 成为状态 owner，Collector 只留
  公开源码兼容 shell。
- [Code Agent 交接](tickets/WF-007-handoff.md) — 固定阶段 0 → 1 → 2 → 3 → 4 → 5 → 6，
  禁止异常、RTTI、自动 retry、HTTP/2 和新持久化格式。

## Not yet specified

无。当前 destination 内的调查问题已经全部解决。

## Out of scope

- 改变默认下载参数或调优数值。
- 在完成全部阶段后公开新的高级 policy interface。
- 自动重试、镜像源、HTTP/2、HTTP/3、代理和认证。
- 改变 `.part` / metadata 文件格式或恢复容错策略。
- 把本地文件、metadata store 或 libcurl 替换为远程基础设施。
- 重写 benchmark schema、重新引入 byte-budget 或其它已退役诊断指标。
- 支持多 persistence writer 或为它预留抽象。
- 纯粹为了缩短文件而机械拆分 `DownloadEngine::run`。

## Frontier

无。全部 tickets 已 resolved，详细结论由上方 Decisions so far 链接到各自唯一来源。

## Dependency graph

```mermaid
flowchart LR
    W0["Characterization Baseline"] --> W1["Validated Download Policy"]
    W1 --> W2["Packet Flow / Backpressure"]
    W2 --> W3["Range Lifecycle"]
    W3 --> W4["Recovery Checkpoint"]
    W4 --> W5["HTTP Transfer"]
    W5 --> W6["Telemetry Session"]
    W6 --> W7["Code Agent Handoff"]
```

## Global stage gate

每个阶段必须按以下顺序执行：

1. 在旧实现上增加或确认特征化测试。
2. 增加新 module interface 与最小 implementation。
3. 通过 adapter 或兼容构造把一个调用链迁移到新 module。
4. 迁移剩余调用方并验证错误路径、线程语义和性能 schema。
5. 删除旧路径、旧字段及只证明旧结构存在的测试。
6. 执行该阶段定义的 Debug、Release、集成与 benchmark gate。
7. 提交阶段决策记录和当前 frontier 的回滚点；之后才允许进入下一阶段。

当前 stage 尚无 dependent 时可以独立 revert。frontier 已继续推进后，回滚较早 stage 必须
从最新 stage 开始按 6 → 0 的逆序撤销所有 dependents，不能删除仍被下游编译依赖的
interface。

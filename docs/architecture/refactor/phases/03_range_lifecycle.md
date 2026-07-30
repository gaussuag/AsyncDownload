# 阶段 3：Range Lifecycle 单所有者状态机

## 1. Outcome

本阶段把 `RangeScheduler`、`DownloadEngine` 与 `PersistenceThread` 共同修改的
`core::RangeContext` 替换为一个深模块：`range::RangeLifecycle`。

阶段完成后：

- Orchestrator 线程是 Range 生命周期状态的唯一写入者；
- 每次 HTTP window 都有不可复用的 `LeaseId`；
- scheduler 只计算初始切分、下一 window 与 steal 提案，不再修改运行时状态；
- Persistence 只拥有写盘所需的局部状态，并通过 release/acquire 发布持久化事实；
- 网络完成只能把 Range 推进到 `awaiting_persistence`；
- 只有匹配的 Persistence 完成事实才能把 Range 推进到 `finished`；
- block 对齐、当前 steal 算法、默认 window、非 Range 降级与恢复文件格式保持兼容；
- 每个迁移 slice 都能独立验证和回滚。

本文是交给 Code Agent 的实现合同，不是建议清单。除“风险与待核实事实”明确列出的项目外，
实现时不得重新发明状态、事件或错误语义。

## 2. 前置条件与阶段边界

本阶段依赖：

1. [阶段 0：特征化与兼容基线](00_characterization_baseline.md)；
2. 阶段 1 已生成经过验证的不可变下载 policy；
3. [阶段 2：Packet Flow / Backpressure](02_packet_flow_backpressure.md)已提供单逻辑
   producer 到单 Persistence consumer 的 Packet Flow 深模块。

阶段 2 的 Packet Flow 只负责 Orchestrator → Persistence 的 data/control 顺序、所有权与
credit。它不是 Persistence → Orchestrator 的通用事件总线。本阶段单独定义低频、
可合并的 Range Fact 发布机制。

本阶段必须保持下列阶段边界：

- 不修改 `DownloadClient`、`DownloadRequest`、`DownloadResult` 或 CLI 接口；
- 不改变默认 `block_size = 64 KiB`；
- 不改变默认 `scheduler_window_bytes = 4 MiB`；
- 不改变现有 steal 候选、阈值和中点算法；
- 不增加自动 retry；
- 不修改 `.config.json` 字段或含义；
- 不改变 tail、乱序重排、bitmap、flush、VDL、CRC 的算法；
- 不把 libcurl 类型带进 Range Lifecycle 接口；
- 不把 public cancellation 加入本阶段；
- 不以本阶段为名重新尝试历史上已拒绝的性能优化。

Phase 4 才集中 Recovery Checkpoint；Phase 5 才集中 HTTP 协议。本阶段只建立它们可以依赖的
Range 生命周期合同。

## 3. 已核实的当前行为

以下结论来自当前基线源码，而不是从类型名称推断。

### 3.1 当前共享状态

`src/core/models.hpp::RangeContext` 同时装载四类不同事实：

| 字段 | 当前写入者 | 当前读取者 | 实际含义 |
| --- | --- | --- | --- |
| `start_offset` | 构造时 | scheduler、persistence、恢复快照 | 逻辑 Range 起点 |
| `end_offset` | scheduler steal | scheduler、engine、metadata | 可被 steal 缩短的含尾终点 |
| `current_offset` | engine 派发/失败回滚 | scheduler、engine、metadata | 已租出前沿，不是写盘前沿 |
| `persisted_offset` | Persistence | Persistence、退出重建、metadata | 按序写入的排他前沿 |
| `status` | engine、Persistence | progress、metadata | 多写者的粗粒度展示状态 |
| `pause_for_gap` | Persistence | engine | Persistence → engine 的 gap 信号 |
| `pause_for_memory` | engine | 无生产读取者 | 遗留写入字段 |
| `completion_notified` | engine | engine | `range_complete` 的本地去重位 |
| `marked_finished` | Persistence | scheduler、engine | Persistence 发布的完成位 |
| `tail_buffer` | Persistence | Persistence | 写盘局部状态 |
| `out_of_order_queue` | Persistence | Persistence | 写盘局部状态 |

当前对象由 `DownloadEngine::run()` 中的
`std::vector<std::unique_ptr<RangeContext>>` 间接拥有。engine、scheduler 与 persistence
持有裸指针；pointee 在 vector 重新分配时保持稳定，生命周期依赖
“先停止并 join Persistence，再销毁 ranges”的隐式顺序。

### 3.2 当前隐式租约

`RangeScheduler::next_window()` 读取 `current_offset` 和 `end_offset`，返回含尾区间。
`arm_transfer()` 在 HTTP 请求真正加入 multi handle 前，把 `current_offset` 直接推进到
`window.end + 1`。`TransferHandle` 再保存：

- `request_start`；
- `request_end`；
- `next_offset`；
- 一个裸 `RangeContext*`。

当前没有 Lease 类型、世代号或跨回调身份校验。请求失败时
`rollback_inflight_window()` 把 `current_offset` 回退到 `TransferHandle::next_offset`，
随后把 Range 标为 `failed`，整个任务停止；当前不会在同一 Session 内 retry。

### 3.3 当前 steal 合同

`RangeScheduler::steal_largest_range()` 的实现合同为：

1. `accept_ranges == false` 时禁止 steal；
2. 跳过 `marked_finished`；
3. 候选剩余量为 `end_offset - current_offset + 1`；
4. 只从最大未派发尾部偷取；
5. 基本下限是 `2 * block_size`；
6. `max_connections >= 16` 时，下限取
   `max(2 * block_size, 2 * scheduler_window_bytes)`；
7. 中点为
   `align_down(current + remaining / 2, block_size)`；
8. donor 新终点是 `midpoint - 1`，新 Range 是 `[midpoint, old_end]`。

它不切断正在飞行的 HTTP window，因为 `current_offset` 在派发前已越过整个 active window。
当前 donor `end_offset` 用 release store 发布，scheduler/engine 用 acquire load 读取。
write callback 实际读取的是 `TransferHandle::request_end`，不会重新读取 donor
`end_offset`。

### 3.4 当前完成合同

所有 data packet 和 `range_complete` control packet 使用同一个显式 producer token，
因此当前正确性依赖“同一 producer 的 data 先于 completion 被 Persistence 观察”。

window 成功时：

- Range 还有未派发尾部：状态改回 `empty`，重新放入 `pending_ranges`；
- Range 没有尾部：`completion_notified.exchange(true, acq_rel)`，首次成功者发送
  `range_complete`；
- engine 不写 `marked_finished`。

Persistence 收到 `range_complete` 后：

1. 调用 `flush_tail()`；
2. 调用 `update_finished_blocks()`；
3. release store `marked_finished = true`；
4. release store `status = finished`；
5. 请求一次异步 flush/metadata 保存。

`finished` 在当前语义中表示“Persistence 已按顺序写入、内存 tail 已写到文件句柄、bitmap
已推进”，不表示该 Range 已独立完成 OS flush 或 metadata checkpoint。VDL 仍然只能在
`FileWriter::flush()` 和 metadata 保存成功后推进。本阶段必须保留这个区别。

### 3.5 当前未封装的异常行为

当前实现没有验证：

- `range_complete` 到达时 `persisted_offset == end_offset + 1`；
- `out_of_order_queue` 已为空；
- completion 属于哪个 Lease 世代；
- duplicate completion 是否与第一次 completion 相同；
- stale callback 或 stale control 是否来自已撤销 Lease；
- duplicate data 的内存会计是否只归还一次；
- offset 小于 `persisted_offset` 的旧 packet 是否应丢弃。

目前 duplicate future packet 的 `map::emplace()` 结果未检查，但 map node 会计和计数已经
增加；旧 offset packet 会永久留在 map 中。这些不是可保留的兼容行为。应先写失败测试，
再通过新的内部合同把它们变成确定性 duplicate/stale/error 结果。

### 3.6 已证明的遗留或无消费字段

在当前生产源码和测试中：

- `RangeContext::pause_for_memory` 只有 store，没有 load；
- `PersistenceThread::current_metadata_state()` 没有调用方；
- `PersistenceThread::all_ranges_completed()` 没有调用方；
- `sampled_data_packet_counter_` 没有读写；
- `current_out_of_order_packets_` 与 `current_out_of_order_bytes_` 只自增自减，没有消费方；
- `RangeStateSnapshot::end_offset`、`current_offset`、`status` 会序列化和 round trip，
  但恢复判定只使用 `start_offset` 与 `persisted_offset` 重建 bitmap；
- `RangeStatus::paused` 被 progress 读取，其他运行状态主要被写入或序列化；
- `marked_finished` 才是当前完成计数与调度跳过条件。

这份证明允许删除运行时遗留字段，但不允许在本阶段删除旧 metadata JSON 字段。旧字段由兼容
投影继续产生和读取，格式清理推迟到 Phase 4 的版本化决策。

## 4. 模块职责

### 4.1 `RangeLifecycle` 负责

- 拥有所有 Range 的身份、几何范围、阶段、gap 阻塞事实和 Lease 世代；
- 根据 scheduler policy 取得下一 Lease；
- 应用 steal 提案并保证 donor/new Range 几何守恒；
- 校验网络 Lease 成功、失败、duplicate 与 stale 事件；
- 接收 Persistence 发布的单调前沿、gap 与 completion 事实；
- 只在匹配的 Persistence completion 后发布 `finished`；
- 生成结果汇总和兼容 metadata 所需的值快照；
- 在失败或取消后阻止新 Lease；
- 返回需要 Orchestrator 应用的 registration/completion effects。

### 4.2 `RangeLifecycle` 不负责

- HTTP status、header、body 长度或 libcurl handle 生命周期；
- packet payload、credit、Accounted Bytes 或 queue 水位；
- 文件写入、对齐 tail、乱序 map、bitmap、flush、VDL 或 CRC；
- retry policy；
- 恢复文件可信度判定；
- queue/memory admission、window-boundary pause、telemetry 聚合；
- 把所有 transfer pause 复制成第二份 Range 状态；
- public cancellation。

### 4.3 Persistence 局部状态负责

`tail_buffer`、`out_of_order_queue`、按序写盘前沿和 completion 校验迁移到
`persistence::RangeWriteState`。它不是第二份 Range 生命周期状态；它只是单 writer
实现为完成写盘所需的局部状态。

删除 `RangeLifecycle` 后，Lease、世代、防 stale、阶段转换、完成校验和 scheduler 协调会重新
散落到 engine/scheduler/persistence，满足 deletion test，说明该模块具有实际深度。

### 4.4 依赖分类与 seam

| 依赖 | 类别 | seam 决策 |
| --- | --- | --- |
| Phase 1 `SchedulingPolicy` | in-process | `create()` 直接读取 checked value，不复制 `RangeRules` |
| `RangeScheduler` | in-process | Lifecycle implementation 内的纯 policy，不暴露新 port |
| Phase 2 `PacketFlow` | in-process | Lifecycle 返回 completion effect，Orchestrator 使用现有 concrete producer |
| Persistence registration | in-process | 低频 command/ACK，只传 geometry/publisher 与安装结果，不传生命周期状态 |
| Persistence facts | in-process | 每 Range 单 producer fact slot，是 Lifecycle 的 internal seam |
| `FileWriter`/bitmap | local-substitutable | 留在 Persistence interface 后，不进入 Lifecycle |
| libcurl/HTTP server | true external | 由 HTTP module 验证后转换成网络事实，Lifecycle 不 mock libcurl |

本阶段不为只有一个 production implementation 的依赖新增虚拟 port。测试通过
`RangeLifecycle` 的同一 interface 输入事件并断言返回 effects/snapshot；interface 就是
test surface。

## 5. 备选接口

### 5.1 方案 A：多线程共享 `RangeAggregate`

engine 和 Persistence 都调用一个带原子方法的对象：

```cpp
range.acquire_window();
range.publish_persisted(offset);
range.finish();
```

优点是迁移短；缺点是两个线程仍然写同一生命周期对象，所有方法都要暴露原子、允许状态和调用
顺序。它把字段访问换成方法调用，却没有消除共享知识，接口仍然浅，拒绝。

### 5.2 方案 B：Orchestrator 单所有者 + Persistence Facts

Orchestrator 独占 `RangeLifecycle`。网络事实直接调用 `apply()`；Persistence 只写每 Range
的 fact slot，Orchestrator 用 acquire 读取后应用。写盘局部状态留在 Persistence。

优点：

- 所有转换在一个 reducer 内；
- Range 记录本身不需要 atomics；
- Lease/stale/duplicate 规则只有一个实现；
- HTTP 和 Persistence 都不需要知道完整状态机；
- 测试可完全通过同一 interface 驱动。

代价是需要一个很小的反向事实发布机制和阶段化迁移，选用此方案。

### 5.3 方案 C：Persistence 拥有完整生命周期

所有 scheduler/acquire 请求都通过命令往返 Persistence。它提供最强的写盘中心化，但每个
HTTP window 派发都多一次跨线程握手，curl 事件循环需要学习异步调度协议，且 scheduler 与
持久化被错误耦合。性能风险和接口复杂度都更高，拒绝。

## 6. 选定模块形状

### 6.1 强类型和值语义

内部统一使用半开区间 `[begin, end)`。只有 HTTP module 和 legacy metadata 兼容投影
转换为现有含尾终点。

```cpp
namespace asyncdownload::range {

using ByteOffset = std::int64_t;

struct RangeId {
    std::uint64_t value = 0;
    friend bool operator==(const RangeId&, const RangeId&) = default;
};

struct LeaseId {
    RangeId range{};
    std::uint64_t generation = 0;
    friend bool operator==(const LeaseId&, const LeaseId&) = default;
};

struct CompletionId {
    RangeId range{};
    std::uint64_t generation = 0;
    friend bool operator==(const CompletionId&, const CompletionId&) = default;
};

struct ByteSpan {
    ByteOffset begin = 0;
    ByteOffset end = 0;

    friend bool operator==(const ByteSpan&, const ByteSpan&) = default;
};

enum class RangePhase : std::uint8_t {
    ready = 0,
    leased = 1,
    awaiting_persistence = 2,
    finished = 3,
    failed = 4,
    cancelled = 5
};

struct RangeLease {
    LeaseId id{};
    ByteSpan bytes{};
    bool use_http_range = true;
};

}
```

约束：

- `RangeId` 在一个 Session 内唯一，`0` 是合法首个 id；
- `LeaseId::generation` 对同一 Range 从 `1` 开始严格递增，永不复用；
- `CompletionId` 在 Range 最后一个成功 Lease 后生成，对 duplicate control 保持相同；
- `0 <= begin < end <= total_size`；
- HTTP Range header 转换为 `"begin-(end - 1)"`；
- 转换前必须检查 `end > begin`，不得对空 span 做 `end - 1`。

Range module 不再定义一份与 Phase 1 重复的 `RangeRules`。唯一输入映射是：

| Range 规则 | 唯一来源 |
| --- | --- |
| total object bytes | `EffectiveDownloadPolicy::remote_facts().total_size` |
| block bytes | `EffectiveDownloadPolicy::scheduling().block_bytes` |
| transfer window | `EffectiveDownloadPolicy::scheduling().transfer_window_bytes` |
| connection limit | `EffectiveDownloadPolicy::scheduling().connection_limit` |
| 是否发 Range request | `EffectiveDownloadPolicy::scheduling().issue_range_requests` |
| 是否允许 steal | `EffectiveDownloadPolicy::scheduling().allow_work_stealing` |

Phase 1 已保证所有 byte 数为正、能表示为 `ByteOffset` 且乘加可 checked。Lifecycle 与
Scheduler 直接读取 `download::SchedulingPolicy` 的这些字段，不做 `int64_t → size_t` 的
隐式窄化，也不再从 raw `DownloadOptions` 或 `accept_ranges` 重建第二套 policy。

Lifecycle 内部记录的目标形状为：

```cpp
enum class LeaseOutcomeKind : std::uint8_t {
    succeeded = 0,
    failed = 1
};

struct LeaseOutcome {
    LeaseId lease{};
    LeaseOutcomeKind kind = LeaseOutcomeKind::succeeded;
    ByteOffset observed_through = 0;
};

struct RangeRecord {
    RangeId id{};
    ByteSpan bytes{};
    ByteOffset dispatch_cursor = 0;
    ByteOffset persisted_through = 0;
    RangePhase phase = RangePhase::ready;
    bool gap_blocked = false;
    std::uint64_t geometry_revision = 0;
    std::uint64_t next_lease_generation = 1;
    std::optional<RangeLease> active_lease;
    std::optional<CompletionId> completion;
    std::optional<LeaseOutcome> last_lease_outcome;
    std::unique_ptr<RangeFactSlot> facts;
};
```

`last_lease_outcome` 只保留最近一次已结束 Lease，用于区分相同 duplicate 与冲突 outcome；
更早世代统一按 stale 处理，不建立无界历史表。

### 6.2 网络事件

```cpp
struct LeaseSucceeded {
    LeaseId lease{};
    ByteOffset received_through = 0;
};

struct LeaseFailed {
    LeaseId lease{};
    ByteOffset accepted_through = 0;
    std::error_code cause;
};

struct CancelRequested {};

struct EffectApplicationFailed {
    std::error_code cause;
};
```

`received_through` 与 `accepted_through` 都是排他前沿。HTTP module 必须先完成 status、
Content-Range 和 body 长度验证，再发布 `LeaseSucceeded`。

### 6.3 Persistence 事实

```cpp
struct PersistedThrough {
    RangeId range{};
    ByteOffset offset = 0;
};

struct GapPauseChanged {
    RangeId range{};
    bool active = false;
};

struct PersistenceCommitted {
    CompletionId completion{};
    ByteOffset persisted_through = 0;
};

struct PersistenceFailed {
    std::optional<RangeId> range;
    std::error_code cause;
};

using RangeEvent = std::variant<
    LeaseSucceeded,
    LeaseFailed,
    PersistedThrough,
    GapPauseChanged,
    PersistenceCommitted,
    PersistenceFailed,
    CancelRequested,
    EffectApplicationFailed>;
```

`PersistenceCommitted` 只能在以下条件全部成立后发布：

- 匹配的 range-complete control 已按 Packet Flow 顺序被消费；
- `out_of_order_queue` 为空；
- `persisted_through == expected_range_end`；
- tail buffer 为空，或已经成功写到文件句柄；
- `update_finished_blocks()` 已执行成功路径；
- 没有 Persistence error。

它不等待本 Range 独立完成 OS flush。flush/metadata/VDL 仍属于 Phase 4 checkpoint 语义。

### 6.4 显式 effects

Lifecycle 不定义虚拟 command sink，也不把 Phase 2 Packet Flow 扩成通用命令总线。状态转换
返回按值 effect，Orchestrator 对现有具体模块执行它们；测试直接断言 effect 值，不需要仅为
测试引入新的 seam 或间接层。

```cpp
struct RegisterRangeEffect {
    RangeId range{};
    ByteSpan bytes{};
    std::uint64_t geometry_revision = 0;
    RangeFactPublisher facts{};
};

struct ResizeRangeEffect {
    RangeId range{};
    ByteOffset new_end = 0;
    std::uint64_t geometry_revision = 0;
};

struct PublishRangeCompleteEffect {
    CompletionId completion{};
    ByteOffset expected_end = 0;
};

using RangeEffect = std::variant<
    RegisterRangeEffect,
    ResizeRangeEffect,
    PublishRangeCompleteEffect>;

template <std::size_t Capacity>
struct EffectBatch {
    std::array<RangeEffect, Capacity> values{};
    std::size_t size = 0;
};

struct InitialEffectBatch {
    std::vector<RangeEffect> values;
};
```

`RegisterRangeEffect` / `ResizeRangeEffect` 不和 Data Packet 共用 Packet Flow，但必须有一个
明确的 Persistence ACK barrier。Persistence 的 concrete interface 增加以下低频命令协议；
它不是 virtual port，也不承载 Lease 调度：

```cpp
namespace asyncdownload::persistence {

using RangeRegistrationTicket = std::uint64_t;
using RangeGeometryCommand = std::variant<
    range::RegisterRangeEffect,
    range::ResizeRangeEffect>;

struct RangeRegistrationSubmitResult {
    RangeRegistrationTicket ticket = 0;
    std::error_code error;
};

struct RangeRegistrationAck {
    RangeRegistrationTicket ticket = 0;
    range::RangeId range{};
    std::uint64_t geometry_revision = 0;
    std::error_code error;
};

struct RangeRegistrationPollResult {
    std::optional<RangeRegistrationAck> ack;
    std::error_code error;
};

}
```

`PersistenceThread::submit_range_geometry(command)` 返回 ticket；
`poll_range_geometry_ack()` 返回零或一个 ACK。ticket 从 `1` 开始且单调递增，ACK 必须精确
回显 ticket、RangeId 和 geometry revision。mailbox 与 ACK storage 都是有界、按 Session
持有的 concrete implementation。checked capacity 为
`max(initial_range_count, 2 * scheduling.connection_limit)`：initial create 每 Range 一个
Register，单个 dynamic steal 最多一个 Resize + 一个 Register，pending arm 数又不得超过
connection limit。Orchestrator 每轮先 drain ACK 再 acquire；在此前提下容量不足说明协议
失配，是 terminal `internal_error`，不得丢命令、动态扩容或阻塞 curl owner thread。

effect 执行规则：

- `RegisterRangeEffect` 和 `ResizeRangeEffect` 进入 Persistence 的低频 registration
  mailbox；mailbox 只携带几何副本和 fact publisher，不携带 Lease outcome、完成真相或
  scheduler 决策；
- `PublishRangeCompleteEffect` 通过 Phase 2 `PacketProducer` 发布现有
  range-complete control，payload 扩展为 `CompletionId + expected_end`；
- ordinary Lease 不发布 control；Phase 2 data DTO 按值携带 `LeaseId + lease_span`；
- create 的所有 registration effect 在启动 HTTP 前提交，并由已经运行的 Persistence
  thread 逐条应用和 ACK；
- dynamic steal 的全部 resize/register effect 都提交后，Orchestrator 保存 pending Lease，
  继续驱动 event loop，但不得 arm 对应 HTTP transfer；
- 只有该 Lease 的全部 ticket 收到成功 ACK 后才能 arm；任何 missing、duplicate、wrong
  identity/revision 或 error ACK 都转成 `EffectApplicationFailed` 并停止任务；
- completion effect 在最后一份 data 已由同一 `PacketProducer` 发布后应用。

`expected_end` 是排他终点，等于 Range `ByteSpan::end` 和最终合法
`persisted_through`；只有生成 HTTP header 时才转换为 `expected_end - 1`。

registration command 的 publish/consume 以及 ACK 的 publish/poll 分别通过 mutex
unlock/lock 或等价 release/acquire 建立 happens-before。Persistence 只有在
`RangeWriteState` 已成功 create/resize 后才发布 success ACK。这样 success ACK
happens-before HTTP arm，HTTP arm 又 happens-before该 Lease 的首个 Data publish；
Persistence 因而不会先看到 unknown future Lease。

mailbox 不是第二个 Range 生命周期：Persistence 只能取走注册值构造/更新自己的
`RangeWriteState`，ACK 只报告“几何副本已安装”或错误，不能写回 phase、Lease outcome 或
scheduler 决策。

Packet Flow 的严格顺序仍是：

```text
DataPacket(LeaseId)... -> RangeComplete(CompletionId, expected_end)
```

任何 submit、ACK 或 completion publish 失败都必须在启动或继续 HTTP 前转成
`EffectApplicationFailed`；不得“先 arm transfer，稍后再补注册”，不得在没有 ACK 时把
超时猜成成功，也不得丢失 completion。任务停止时必须取消所有尚未 arm 的 pending Lease、
停止新调度、关闭 Packet Flow producer 并 join Persistence；本 Session 不重试 registration。

### 6.5 深模块 interface

```cpp
enum class EventDisposition : std::uint8_t {
    applied = 0,
    duplicate = 1,
    stale = 2,
    rejected = 3
};

struct AcquireResult {
    std::optional<RangeLease> lease;
    EffectBatch<2> effects;
    std::error_code error;
};

struct ApplyResult {
    EventDisposition disposition = EventDisposition::applied;
    std::error_code error;
    bool scheduler_may_run = false;
    bool task_should_stop = false;
    EffectBatch<1> effects;
};

struct RangeSnapshot {
    RangeId id{};
    ByteSpan bytes{};
    ByteOffset dispatch_cursor = 0;
    ByteOffset persisted_through = 0;
    RangePhase phase = RangePhase::ready;
    bool gap_blocked = false;
    std::optional<LeaseId> active_lease;
};

struct LifecycleSnapshot {
    std::vector<RangeSnapshot> ranges;
    std::size_t finished_ranges = 0;
    bool has_schedulable_work = false;
    bool all_finished = false;
};

struct SnapshotResult {
    LifecycleSnapshot value;
    std::error_code error;
};

struct RangeLifecycleCreation {
    std::unique_ptr<class RangeLifecycle> value;
    InitialEffectBatch effects;
    std::error_code error;
};

class RangeLifecycle {
public:
    [[nodiscard]] static RangeLifecycleCreation create(
        ByteOffset total_size,
        const download::SchedulingPolicy& scheduling,
        std::span<const ByteSpan> initial_ranges) noexcept;

    [[nodiscard]] AcquireResult acquire() noexcept;
    [[nodiscard]] ApplyResult apply(const RangeEvent& event) noexcept;
    [[nodiscard]] ApplyResult drain_persistence_facts() noexcept;
    [[nodiscard]] SnapshotResult snapshot() const noexcept;
};
```

`create()` 内部捕获 allocation failure 并返回 `std::error_code`；接口不得向 engine 抛异常。
`snapshot()` 也在内部把 allocation failure 转成错误。调用方不接触 Range record、
scheduler view 或 fact slot。

`acquire()` 的行为为：

1. 优先从已有 `ready` Range 取得下一 window；
2. 没有 ready Range 时，向内部 `RangeScheduler` 请求一个 steal proposal；
3. proposal 合法时，由 Lifecycle 原子地缩短 donor、创建新 Range、返回 effects 并取得 Lease；
4. 没有合法工作时返回 `lease == std::nullopt` 且无错误；
5. Orchestrator 提交全部 geometry effects，并在收到每个 matching success ACK 后才能 arm
   Lease；
6. submit/ACK/effect 失败时 apply `EffectApplicationFailed` 并停止任务，不能 arm Lease。

### 6.6 Fact slot

每个 Range 由 Lifecycle 创建一个地址稳定的 fact slot，生命周期持续到 Persistence join
之后。Persistence 只持有对应 publisher，不持有 Lifecycle record。

fact slot 是单向 publication mailbox，不是第二个共享 `RangeContext`：

- 它没有 geometry、phase、active Lease、scheduler cursor 或 retry 决策；
- Persistence 只写，不把 slot 当作可查询状态；
- 只有 `RangeLifecycle::drain_persistence_facts()` 读取；
- 读取值必须先转成 `RangeEvent` 并经过同一个 reducer，才能成为权威状态；
- progress、result、scheduler、metadata 都不得直接读取 slot。

目标 interface：

```cpp
struct RangeFactSnapshot {
    ByteOffset persisted_through = 0;
    bool gap_paused = false;
    std::uint64_t committed_generation = 0;
    std::uint64_t revision = 0;
};

class RangeFactPublisher;

class RangeFactSlot {
public:
    RangeFactSlot(RangeId range, ByteOffset initial_offset) noexcept;

    [[nodiscard]] RangeFactPublisher publisher() noexcept;

    [[nodiscard]] std::optional<RangeFactSnapshot>
    read_since(std::uint64_t last_revision) const noexcept;

private:
    const RangeId range_;
    std::atomic<ByteOffset> persisted_through_;
    std::atomic<bool> gap_paused_{false};
    std::atomic<std::uint64_t> committed_generation_{0};
    std::atomic<std::uint64_t> revision_{0};
};

class RangeFactPublisher {
public:
    RangeFactPublisher() noexcept = default;

    void publish_persisted_through(ByteOffset offset) noexcept;
    void publish_gap_pause(bool active) noexcept;
    [[nodiscard]] std::error_code publish_committed(
        CompletionId completion,
        ByteOffset offset) noexcept;

private:
    explicit RangeFactPublisher(RangeFactSlot& slot) noexcept;

    RangeFactSlot* slot_ = nullptr;

    friend class RangeFactSlot;
};
```

publisher 绑定单个 immutable `RangeId`，用它拒绝错误 Range 的 completion；
`committed_generation == 0` 表示未完成；有效 Lease/Completion generation 从 `1` 开始。

热路径事实采用合并发布：

- `persisted_through` 只保留最大排他前沿；
- `gap_paused` 只保留最新布尔值；
- completion generation 是 sticky terminal fact；
- `revision` 是单 writer 单调序号。

普通 progress 发布对对应字段做 release store，再 release 增加 `revision`；
Orchestrator acquire 读取 `revision`，变化后对每个 payload 做 acquire load。
completion 发布必须先 release store `persisted_through`，再 release store
`committed_generation`，最后 release 增加 `revision`。读取到非零 generation 的 acquire
load 后，必须能看到此前发布的前沿。所有 payload 都是 atomic，不允许用普通字段实现
seqlock 造成 C++ data race。

`revision` 只是合并与扫描提示，不是唯一正确性载体。即使多个更新合并，单调
`persisted_through`、最新 `gap_paused` 和 sticky `committed_generation` 仍然可恢复。
`read_since()` 返回快照后，Lifecycle 把它展开为前述 Persistence facts，再通过同一个
`apply()` reducer 处理。

单个 `RangeFactSnapshot` 的展开顺序是固定协议，不得按 variant 枚举顺序或 caller 偏好：

1. 若 `persisted_through` 比 Lifecycle 已观察值大，先 apply `PersistedThrough`；
2. 若 `gap_paused` 变化，再 apply `GapPauseChanged`；
3. 若 `committed_generation != 0` 且尚未观察，最后 apply `PersistenceCommitted`，其
   `persisted_through` 使用同一 snapshot 的值。

因此一个 publication 中同时出现新 persisted frontier 与 completion 时，completion 校验
一定看到已经推进的前沿，不会因 drain 顺序误报 mismatch。任一步返回 terminal error 时停止
展开，保留 first error，不把后续 completion 当成成功。

这条路径：

- 不按 packet 分配对象；
- 不在 write callback 中加锁；
- 不会因反向队列满而丢 terminal fact；
- 允许多个 persisted 更新合并；
- 最多沿用当前 `curl_multi_wait` 的 100 ms 观察延迟。

Persistence 的首个错误仍通过明确的 `PersistenceFailed`/现有 error 检查进入
`RangeLifecycle::apply()`，不得只置一个无人观察的 flag。

## 7. 核心不变量

对每个非空 Range 始终满足：

```text
0 <= bytes.begin
bytes.begin <= persisted_through
persisted_through <= dispatch_cursor
dispatch_cursor <= bytes.end
bytes.end <= total_size
```

补充规则：

- `ready` 没有 active Lease；
- `leased` 恰有一个 active Lease；
- `ready && gap_blocked` 不可取得新 Lease，直到 Persistence 发布 gap cleared；
- 同一 Range 同时最多一个 active Lease；
- active Lease 必须从取得时的 `dispatch_cursor` 开始；
- Lease span 不跨越 Range span；
- `awaiting_persistence` 没有 active Lease，且没有未派发尾部；
- `finished` 必须有匹配的 `PersistenceCommitted`；
- `failed`、`cancelled`、`finished` 不再取得 Lease；
- `failed`/`cancelled` 仍可接收单调 Persistence facts，以保存失败退出前真正写入的前沿；
- 终态收到 facts 不会回到非终态；
- gap 是正交阻塞事实，不是独立阶段；
- Lifecycle 只保留影响 Range 有效性的 `gap_blocked`，不复制其他 pause/telemetry 状态；
- Range geometry revision 单调递增；
- steal 前后所有 Range 的字节并集不变且互不重叠；
- steal 点按 `block_size` 对齐；
- 除文件末尾外，初始 Range 的半开终点按 `block_size` 对齐；
- Persistence 是 `persisted_through` 和 completion 的唯一事实来源。

失败 Lease 的合同：

- matching failure 立即撤销 active Lease；
- 生命周期记录的 `dispatch_cursor` 回到当前 `persisted_through`；
- 因此所有尚未持久化字节重新属于未完成集合；
- 本 Session 保持当前“不自动 retry”，Range 进入 `failed`；
- Packet Flow 中已经被接受的数据仍由 Persistence drain；
- 后续 `PersistedThrough` 可以单调推进失败 Range 的持久化前沿；为保持
  `persisted_through <= dispatch_cursor`，终态 reducer 同时执行
  `dispatch_cursor = max(dispatch_cursor, persisted_through)`，但绝不把 Range 变回
  schedulable、创建 Lease 或改变失败结果；
- 下次 Session 从 bitmap/checkpoint 重建，不信任网络 accepted 前沿。

## 8. 完整状态转换表

下表中的“错误”均通过现有项目错误体系返回；内部协议破坏映射到
`DownloadErrc::internal_error`，网络错误保留已有映射。

| 当前阶段 | 事件/操作 | Guard | 下一阶段 | 结果与副作用 |
| --- | --- | --- | --- | --- |
| create | 有效初始 span | 全部合法、互不重叠 | `ready` | 分配 RangeId，发布 Register |
| create | 空/越界/重叠 span | 任一非法 | 无对象 | 返回确定性错误 |
| `ready` | `acquire()` | cursor < end、非 gap blocked 且 generation 可递增 | `leased` | 以当前 `next_lease_generation` 签发 immutable Lease（首代为 1），checked 递增 next generation，保存 active Lease，并把 `dispatch_cursor` 推进到 `lease.bytes.end` |
| `ready` | `acquire()` | generation 已不可递增 | `failed` | 不签发 Lease、不修改 cursor；返回 internal error |
| `ready` | `acquire()` | gap blocked | `ready` | 返回空 Lease，等待 fact cleared |
| `ready` | `acquire()` | cursor == end | `awaiting_persistence` | 返回唯一 completion effect |
| 无 ready | `acquire()` | 有合法 steal | donor 保持原阶段，新 Range `leased` | 新 Range 以 generation 1 签发并保存 active Lease，next generation 变为 2，dispatch cursor 推到 Lease end；返回 Resize/Register effects 与 Lease |
| 无 ready | `acquire()` | 无合法 steal | 不变 | 返回空 Lease、无错误 |
| `leased` | matching `LeaseSucceeded` | received == lease.end 且 lease.end < range.end | `ready` | 有尾部；清 active，允许下次调度 |
| `leased` | matching `LeaseSucceeded` | received == lease.end 且 lease.end == range.end | `awaiting_persistence` | 清 active，返回唯一 completion effect |
| `leased` | matching `LeaseSucceeded` | received 不等于 lease.end | `failed` | rejected，任务停止 |
| `leased` | matching `LeaseFailed` | accepted 在 lease 内 | `failed` | 清 active，cursor 回到 persisted，任务停止 |
| `leased` | matching `LeaseFailed` | accepted 越界 | `failed` | internal error，任务停止 |
| 任意非终态 | stale Lease event | generation 小于当前/已撤销 | 不变 | `stale`，无副作用 |
| `ready`/`awaiting_persistence` | 最近成功 Lease 的相同 success | 内容完全相同 | 不变 | `duplicate` |
| 任意 | 相同 LeaseId 的冲突 outcome | success/failure 或前沿冲突 | `failed` | rejected，任务停止 |
| 任意 | future/未知 Lease generation | 大于已签发 generation | `failed` | 协议破坏，任务停止 |
| 非终态 | `PersistedThrough` | persisted <= offset <= end | 原阶段 | 单调推进 persisted |
| `failed`/`cancelled` | `PersistedThrough` | persisted <= offset <= end | 原终态 | 同步单调推进 persisted 与 dispatch；不得重启调度 |
| 任意 | persisted regression | offset < 已观察前沿 | 不变 | `stale`；不降级 |
| 任意 | persisted beyond end | offset > end | `failed` | internal error |
| 任意非终态 | `GapPauseChanged` | 值变化 | 原阶段 | 更新 gap bit |
| 任意 | duplicate gap value | 值未变化 | 原阶段 | `duplicate` |
| `awaiting_persistence` | matching `PersistenceCommitted` | persisted == end | `finished` | finished_ranges + 1 |
| `awaiting_persistence` | matching commit | persisted != end | `failed` | completion 条件破坏 |
| `finished` | 相同 completion | id、前沿完全相同 | `finished` | `duplicate` |
| 任意 | stale completion | 旧 CompletionId | 不变 | `stale` |
| 非 awaiting | future/未知 completion | 未签发 | `failed` | 协议破坏 |
| 任意非终态 | matching `PersistenceFailed` | 有错误 | `failed` | 任务停止 |
| `finished` | `PersistenceFailed` | checkpoint 后续失败 | `finished` Range 不回退 | task error 由 Session 收尾处理 |
| 任意非终态 | `EffectApplicationFailed` | effect 应用失败 | `failed` | 禁止新 Lease，任务停止 |
| `ready`/`leased`/`awaiting_persistence` | `CancelRequested` | 任意 | `cancelled` | 禁止新 Lease，由 Orchestrator 关闭网络/Packet Flow |
| `failed` | `CancelRequested` | 任意 | `failed` | duplicate terminal intent |
| `finished` | `CancelRequested` | 任意 | `finished` | 不撤销已完成事实 |
| `cancelled` | Lease success/failure | 任意 | `cancelled` | stale，不恢复调度 |
| `failed`/`cancelled` | persisted/gap facts | 单调合法 | 原阶段 | 只更新恢复可见事实 |
| `failed`/`cancelled` | completion | 任意 | 原阶段 | 不升级 finished |

### 8.1 短 final window

若 `remaining < scheduler_window_bytes`，Lease 使用精确半开区间
`[cursor, range.end)`。只要 HTTP module 验证实际 body 恰好覆盖此区间，
`LeaseSucceeded.received_through == range.end` 就合法。不得要求 window 或文件末尾填满
`scheduler_window_bytes`。

最后一个 block 可以短于 `block_size`。Persistence 的 tail 写盘只计算逻辑字节，
bitmap 仍按 `min(block_begin + block_size, total_size)` 判断完整覆盖。

### 8.2 非 Range

当 `accept_ranges == false`：

- effective connections 是 1；
- scheduler 禁止 split 和 steal；
- Lifecycle 只接受覆盖 `[0, total_size)` 的单 Range 计划；
- 只签发一个 `use_http_range = false` 的完整对象 Lease；
- HTTP module 不发送 Range header；
- short body 仍然是失败，不得把提前 EOF 当 completion；
- 旧 bitmap 有未完成区域时是否重用局部块属于 Phase 4/5；在没有可证明的安全协议前，
  必须按完整对象重下，而不能把非 Range 响应解释成局部 span。

该合同实现了项目文档中“非 Range 降级到单连接整文件请求”的意图。迁移前必须加入测试，确认
当前 partial-resume 路径不会被误当成多个完整对象请求。

## 9. Scheduler 关系

`RangeScheduler` 保留为纯 in-process policy，无共享可变 Range。

建议内部形状：

```cpp
struct RangeCandidate {
    RangeId id{};
    ByteSpan bytes{};
    ByteOffset dispatch_cursor = 0;
    RangePhase phase = RangePhase::ready;
};

struct StealPlan {
    RangeId donor{};
    ByteOffset split = 0;
};

struct RangePlanResult {
    std::vector<ByteSpan> ranges;
    std::error_code error;
};

class RangeScheduler {
public:
    [[nodiscard]] RangePlanResult
    plan_initial(const core::AtomicBlockBitmap& bitmap) const noexcept;

    [[nodiscard]] ByteSpan next_window(const RangeCandidate& candidate) const noexcept;

    [[nodiscard]] std::optional<StealPlan>
    choose_steal(std::span<const RangeCandidate> candidates) const noexcept;
};
```

Scheduler：

- 不分配 `RangeId`；
- 不持有 `next_range_id_`；
- 不 store donor end；
- 不读取 `marked_finished` atomics；
- 不执行 registration/completion effects；
- 不知道 Lease generation；
- 不知道 HTTP handle；
- 不知道持久化前沿。

Lifecycle 验证并应用 proposal。即使 scheduler 返回错误 proposal，Lifecycle 也不能破坏
不变量。

steal 必须保留当前算法：

```text
remaining = end - dispatch_cursor
minimum = 2 * block_size
if max_connections >= 16:
    minimum = max(minimum, 2 * scheduler_window_bytes)
split = align_down(dispatch_cursor + remaining / 2, block_size)
```

proposal 只有在 `split > dispatch_cursor && split < end` 时有效。应用次序：

1. 创建新 Range record 和 fact slot，但尚不可调度；
2. 返回 donor `ResizeRangeEffect`；
3. 返回 new `RegisterRangeEffect`；
4. Lifecycle 返回时内存转换已提交；Orchestrator 必须先成功应用两个 effects；
5. 从新 Range 取得 Lease，data DTO 自带 Lease identity/span；
6. 任一 effect 失败都不 arm HTTP，并 apply `EffectApplicationFailed`。

正在飞行的 donor Lease 保存不可变 `RangeLease::bytes`，split 必须在其终点之后，callback
不再读取可变 donor end。

## 10. 网络事实与 Persistence 事实如何进入

### 10.1 网络路径

```text
RangeLifecycle.acquire()
  -> RangeLease
  -> HTTP module arms easy handle with immutable LeaseId/ByteSpan
  -> callback submits DataChunk carrying LeaseId/ByteSpan
  -> Packet Flow publishes ordered DataPacket
  -> HTTP validation
  -> RangeLifecycle.apply(LeaseSucceeded/LeaseFailed)
```

`TransferHandle` 不再保存裸 `RangeContext*`，只保存：

- `RangeLease`；
- HTTP module 的 request/callback 状态；
- Phase 2 producer handle；
- HTTP/Packet Flow 自己拥有的 pause 原因。

callback 产出的 `DataChunk` 与 Packet Flow 内部 `DataPacket` 必须携带 `LeaseId` 和
`lease_span`。回调从 immutable Lease 得到允许的终点，
不会观察 steal 后的 geometry。pause 返回时本批数据未消费，unpause 后由 libcurl 重放；
只有真正接纳进 transfer buffer 的字节才能推进 HTTP module 的 accepted frontier。

### 10.2 Persistence 路径

```text
Packet Flow control/data FIFO
  -> Persistence RangeWriteState
  -> ordered write / tail / bitmap
  -> RangeFactPublisher release publication
  -> Orchestrator drain acquire
  -> RangeLifecycle applies facts
```

`RangeWriteState` 至少包含：

```cpp
struct RangeWriteState {
    RangeId id{};
    ByteSpan bytes{};
    std::uint64_t geometry_revision = 0;
    std::uint64_t last_observed_lease_generation = 0;
    std::optional<ByteSpan> last_observed_lease_span;
    ByteOffset observed_dispatch_through = 0;
    ByteOffset persisted_through = 0;
    bool gap_blocked = false;
    bool local_failure = false;
    TailBuffer tail;
    std::map<ByteOffset, flow::PacketLease> out_of_order;
    std::optional<CompletionId> pending_completion;
    RangeFactPublisher facts;
};
```

它只由 Persistence 线程修改。`ranges_mutex_` 不再保护共享 `RangeContext*`；动态注册经
success ACK 后才允许 arm，因此任何对应 data/control 都在 geometry 安装之后发布。

packet 校验规则：

- `PacketLease::data()->lease.range` 必须等于目标 Range；
- 一个 Range 的首个已观察 Lease generation 必须是 `1`；
- 同 generation 后续 Data 的 `lease_span` 必须与首次观察值完全一致；
- 新 generation 只能等于 `last_observed_lease_generation + 1`；generation 跳跃定义为
  unknown future Lease protocol error；
- generation 增加时，新 span 必须位于当前 registered geometry 内，且 begin 不早于上个
  observed span 的 end；成功后原子更新 generation/span；
- Range Complete 的 `CompletionId::generation` 必须等于最后观察到的 generation，
  `expected_end` 必须等于 registered `bytes.end`；
- packet size 必须大于 0；
- `packet.end <= range.end`；
- 完全落后于 `persisted_through` 的 duplicate lease 调用 `complete()`，由 RAII 恰好归还一次
  credit/memory；
- map 中相同 `(LeaseId, offset, size)` 的 duplicate 不进入 map，当前 lease 调用
  `complete()`；
- 部分重叠、同 offset 不同 payload size、generation 跳跃和 completion generation
  mismatch 都是 internal error；
- stale Lease 已入队但数据在失败 control 之前发布时，仍允许按 FIFO drain；
- completion control 不能越过该 producer 已发布的 data。

Persistence 不读取 numeric `accounted_bytes`。Phase 2 的 move-only `PacketLease` 内联持有
accounting ledger；进入乱序 map 前只调用一次 `account_reorder_node()`，写盘、duplicate、
error 与 teardown 路径最终都由 `complete()`/析构归还一次。

`observed_dispatch_through` 只用于 legacy metadata 的 `current_offset` 兼容字段。Persistence
在首次消费某个 Lease 的 Data Packet 时，用 `lease_span.end` 做单调 max；处理最终 completion
时也用 `expected_end` 做 max。它不是调度真相，恢复也不读取它。一个在收到任何数据前失败
的 Lease 可以不进入这个投影。

## 11. 所有权、生命周期与线程模型

### 11.1 所有权

| 对象 | 所有者 | 借用者 |
| --- | --- | --- |
| `RangeLifecycle` | `DownloadEngine::run()` 栈内 unique owner | Orchestrator 线程 |
| Range records | `RangeLifecycle` | 无外部借用 |
| fact slots | `RangeLifecycle` 的稳定 heap storage | 对应 `RangeFactPublisher` |
| `RangeLease` | HTTP module 按值持有 | callback 只读 |
| `RangeWriteState` | `PersistenceThread` | Persistence 线程 |
| packet payload/accounting | move-only `flow::PacketLease` | Persistence handler/map |
| compatibility snapshots | 按值返回 | progress/metadata 兼容投影 |

销毁顺序必须为：

```text
停止 arm 新 Lease
移除/停止所有 curl handles
关闭 Packet Flow producer
Persistence drain data/control
Persistence 发布最后 facts
Persistence stop + join
Orchestrator 最后一次 drain_persistence_facts()
销毁 RangeLifecycle/fact slots
```

不得使用 detached thread、全局 Range registry 或跨 Session LeaseId。

### 11.2 线程权限

- 只有 Orchestrator 调用 `acquire()`、`apply()`、`drain_persistence_facts()` 和
  `snapshot()`；
- curl write callback 只读按值 Lease，不调用 Lifecycle；
- Persistence 只修改 `RangeWriteState` 和对应 fact slot；
- worker pool 不修改 Lifecycle 或 RangeWriteState；
- progress callback 接收值快照，不持有内部引用；
- metadata worker 接收值快照，不读 Lifecycle。

Debug build 应记录 owner thread id 并在 Lifecycle interface 入口断言同线程；Release
不得增加热路径锁。

### 11.3 memory order

目标实现中的同步合同：

| 发布 | 写入 | 读取 | memory order |
| --- | --- | --- | --- |
| Orchestrator → Persistence data/completion | Phase 2 enqueue | dequeue | release/acquire，由 Packet Flow 合同提供 |
| Orchestrator → registration mailbox | register/resize command | Persistence apply | mutex 或等价 release/acquire |
| Persistence → Orchestrator geometry ACK | installed state/error | poll ACK | mutex 或等价 release/acquire |
| Persistence persisted/gap fact | payload 后 revision | drain revision 后 payload | release/acquire |
| Persistence completion | persisted/end/generation 后 revision | acquire revision 后校验 | release/acquire |
| stop/error | 首个错误和 stop flag | event loop | 保留现有 release/acquire |

迁移期只要 legacy `end_offset` 仍被共享：

- donor 缩短继续用 `store(memory_order_release)`；
- scheduler/engine/metadata 继续用 `load(memory_order_acquire)`；
- 不得提前改成 relaxed。

最终形状中 geometry 只由 Orchestrator 修改；Persistence 通过低频 registration mailbox
取得副本并 ACK，HTTP callback 只在 ACK 后通过 immutable Lease 取得副本。因此不再需要
共享 `end_offset` atomic，但原有 happens-before 保证转移到了
command → apply → ACK → arm seam，并未消失。

## 12. 兼容与 anti-corruption 迁移

### 12.1 public/CLI

没有 public header 变化。正常执行路径的 `DownloadResult::completed_ranges` 从 Lifecycle
最终值快照生成，语义仍是“Persistence 确认完成的 Range 数量”。Phase 4
`RecoveryDisposition::complete` fast path 不创建 Lifecycle；该路径从 validated recovery
result 取得 current compatibility value（required block count），不得伪造一个 Lifecycle。
progress 的 `paused_ranges` 仍由 Orchestrator 汇总 active transfer 的
queue/memory/window pause 与 Lifecycle 的 gap fact；Lifecycle 不成为 pause/telemetry 的
第二真相源。字段名和类型不变。

### 12.2 legacy metadata

本阶段保留 `RangeStateSnapshot` JSON，但快照必须在 Persistence 线程从
`RangeWriteState` 生成，不能跨线程读取 `RangeLifecycle`：

- `range_id` 从 `RangeWriteState.id` 转换；
- `start_offset = RangeWriteState.bytes.begin`；
- `end_offset = RangeWriteState.bytes.end - 1`；
- `current_offset = max(observed_dispatch_through, persisted_through)`；
- `persisted_offset = RangeWriteState.persisted_through`；
- `status` 通过 Persistence 局部事实做兼容映射。

建议映射：

| Persistence 局部事实 | legacy status |
| --- | --- |
| `local_failure` | `failed` |
| completion 已消费、tail/map 为空且 `persisted_through == bytes.end` | `finished` |
| `gap_blocked` | `paused` |
| 已观察 Data/Completion 但未完成 | `downloading` |
| 尚未观察 Data/Completion | `empty` |

加载旧 metadata 时仍只把 `start_offset/persisted_offset` 投影到 bitmap，不把 legacy
`status/current_offset` 当作可恢复 active Lease。Lease generation 每次 Session 重新开始。

### 12.3 迁移兼容投影

迁移期间可以存在 `LegacyRangeProjection`，但必须满足：

- 只由 Persistence 从 `RangeWriteState` 生成，不接受 Orchestrator 写入；
- 只为阶段 4 前尚未迁移的 metadata caller 提供值快照；
- 不允许 scheduler/engine 重新取得可写 `RangeContext*`；
- 每迁移一个 caller 就删除对应 projection；
- 阶段 4 仍由 Persistence 从 `RangeWriteState` 冻结
  `RecoveryRangeFact` value DTO；RecoveryCheckpoint 只接收 DTO 并拥有 codec、immutable
  image 与 commit 协议，不读取 `RangeWriteState`，也不接管 legacy runtime projection。

Lifecycle 的 `snapshot()` 只服务 progress、最终结果与调度观察，不作为 checkpoint 输入。

不得建立“新 Lifecycle 是真相，旧 RangeContext 也能被修正”的双向同步。

## 13. 预计文件布局

Code Agent 可以按仓库 CMake 结构调整精确清单，但职责不得重新混合：

```text
src/range/range_types.hpp
src/range/range_lifecycle.hpp
src/range/range_lifecycle.cpp
src/range/range_fact_slot.hpp
src/download/range_scheduler.hpp
src/download/range_scheduler.cpp
src/persistence/range_write_state.hpp
src/persistence/range_write_state.cpp
src/download/download_engine.cpp
src/persistence/persistence_thread.hpp
src/persistence/persistence_thread.cpp
src/core/models.hpp
tests/range/range_lifecycle_test.cpp
tests/range/range_lifecycle_property_test.cpp
tests/range/range_fact_slot_test.cpp
tests/download/range_scheduler_test.cpp
tests/persistence/persistence_thread_test.cpp
tests/download/download_resume_integration_test.cpp
```

## 14. Tests-first 小提交计划

每个 slice 独立 build、test、记录 evidence 后再进入下一个。`S` 只迁移结构且保持当前合法
行为；`C` 以明确的 expected-red 测试开始并单独使用 `fix:` commit。不要 squash 到无法
按逆依赖顺序回滚。

### 03.1 特征化当前合同

只加测试：

- 初始 span block 对齐；
- steal 最大未派发尾部；
- `< 16` 与 `>= 16` 两种 steal 下限；
- active window 不被 steal 切断；
- final short window；
- 非 Range 禁止 steal、单连接；
- network completion 不直接 finished；
- Persistence completion 才 finished；
- duplicate/stale 当前缺口用失败测试明确。

Rollback：只回滚新测试；发现的风险必须保留在 ticket，不能从文档消失。

### 03.2（S）引入强类型、纯 reducer 与 scheduler proposal

新增 `range_types`、未接线 `RangeLifecycle`、纯 `RangeScheduler` proposal tests。生产主链仍走
legacy。

验证：新 unit/property tests + 全量测试。

Rollback：删除新文件和 CMake 条目，不影响生产行为。

### 03.3（S）引入 LeaseId，但保留 legacy Persistence 投影

- `TransferHandle` 改为按值持有 `RangeLease`；
- 使用 Phase 2 已定义的 Lease-aware Data DTO 与 Completion-aware control DTO；
- callback 使用 immutable Lease span；
- engine 的 pending/active 状态改由 Lifecycle 驱动；
- Persistence 仍通过只读 compatibility registration 获得旧几何。

验证：合法 callback、失败 Lease、short final 和既有 happy-path tests。对 stale、duplicate
和 allocation failure 的目标行为只保留 expected-red 证据，直到后续 C slice。

Rollback：回滚本 slice，03.2 的未接线模块仍可保留。

### 03.4（S）把 scheduler mutation 收入 Lifecycle并建立 geometry ACK barrier

- `RangeScheduler` 只返回 value proposal；
- Lifecycle 分配 RangeId/generation；
- Lifecycle 应用 steal、发布 Resize/Register effects；
- Orchestrator 通过 Persistence command/ACK protocol 安装 geometry，收到全部 success ACK
  后才 arm HTTP；
- 删除 scheduler 对 `RangeContext` atomics 的依赖。

验证：几何 property tests、ACK happens-before首个 Data、registration failure stop、运行时
steal integration、distinct ports。

Rollback：恢复 legacy scheduler 投影；不得改默认 window/steal 参数掩盖失败。

### 03.5（S）引入 Persistence `RangeWriteState` 与 fact slot

- 移动 tail、map、persisted 前沿；
- packet/control 按当前合法顺序通过 LeaseId 关联；
- Persistence 发布 persisted/gap/completion facts；
- Lifecycle acquire drain；
- fact snapshot 固定按 persisted → gap → completion 展开；
- 只迁移当前通过的 completion happy path，不在本提交改变 malformed/duplicate 行为。

验证：Persistence happy-path 单元、fact slot concurrency、gap pause、内存归还和现有
错误路径；所有前序 expected-red 仍按清单可追踪。

Rollback：恢复 LegacyRangeProjection；Phase 2 Packet Flow 不回滚。

### 03.6（C）把 allocation failure 映射为错误

- 为 Lifecycle create/snapshot、effect batch、registration command 和 Persistence local
  allocation 增加确定性 fault seam；
- 把当前可能越过 `noexcept` 导致 terminate 的 failure 映射为 first
  `internal_error`/`not_enough_memory`；
- 不和 ownership move 或 header rename 合并。

验证：每个 allocation point 的 red→green、无 terminate、无部分 state mutation、artifact
保留。Rollback：只回滚错误映射；03.2–03.5 的结构仍可保留。

### 03.7（C）定义 duplicate、stale、future generation 与 overlap

- 实施首代为 1、同代 span 一致、新代只能 `+1` 的 observed-generation 规则；
- stale/duplicate Data 恰好释放 PacketLease 一次；
- duplicate map key 不增加 node accounting；
- partial overlap、generation jump 和 completion identity mismatch 返回 terminal error；
- Lifecycle duplicate/stale/conflicting outcome 走第 8 节 reducer。

验证：所有相关 expected-red 逐项 red→green，包含乱序 map、stale completion 与 generation
jump。Rollback：只恢复旧失败分类，不回滚 RangeWriteState。

### 03.8（C）修复 Persistence duplicate accounting

- 检查 `map::emplace()` 结果；
- 失败插入、duplicate、partial overlap、abort drain 的 payload、queue credit、payload
  accounting 和 map-node accounting 各释放一次；
- underflow 变为 deterministic first error，不 reset counter 掩盖。

验证：定向 accounting ledger、failure injection 与一百万次 stress。Rollback：只回滚
accounting fix；保留 03.7 identity reducer。

### 03.9（C）收紧 completion correctness

- matching completion 必须同时满足 expected end、无 gap、tail 已写、reorder map 为空和
  final persisted frontier；
- stale/duplicate completion 幂等，冲突 completion terminal failure；
- 失败/取消后晚到 persisted fact 只推进终态 frontier/dispatch，不重启调度。

验证：gap、tail write failure、persisted/end mismatch、duplicate/stale completion
red→green。Rollback：只回滚 completion hardening；不回滚 fact slot。

### 03.10（S）切换完成、progress 与结果汇总

- engine 只读 Lifecycle snapshot；
- 移除 `completion_notified`、`marked_finished` 和共享 `RangeStatus`；
- `completed_ranges` 只由 matching Persistence completion 计数；
- progress pause 由 Orchestrator 汇总 transfer pause 与 Lifecycle gap fact；
- 失败/取消后完成最后 fact drain。

验证：progress、server failure、resume、final result、duplicate completion。

Rollback：恢复 snapshot 兼容投影，不回滚 03.5 的 Persistence 局部状态。

### 03.11（S）删除 legacy 共享对象与死字段

- 删除 `RangeContext`；
- 删除运行时 `TailBuffer` 的 core 位置；
- 删除未使用 Persistence methods/counters；
- 保留 `RangeStateSnapshot` legacy JSON DTO；
- 更新架构文档与 include/CMake。

验证：`rg` 不再发现生产代码直接修改旧字段；Debug/Release/benchmark 全 gate。

Rollback：只回滚删除提交；前面迁移后的 interface 必须仍可运行。

### 03.12（perf/docs）阶段 gate 与证据

先对 S slices 做一次同机 pre/post，再对 03.6–03.9 每个 C slice 保存可归因的 benchmark
结果；运行 Debug、Release、integration、正式 10-key schema 和 profiler gate。最后记录
actual commit、known 740 failure、reverse-order rollback ids 与下一阶段解锁结论。此 slice
不修改 production 参数。

## 15. 测试合同

### 15.1 Unit tests

`RangeLifecycleTest` 至少覆盖：

- creates_disjoint_initial_ranges；
- rejects_empty_overlapping_or_out_of_bounds_ranges；
- acquires_one_active_lease_per_range；
- signs_first_lease_at_generation_one_and_advances_dispatch_cursor_to_lease_end；
- increments_generation_without_reuse；
- fails_without_cursor_change_before_generation_overflow；
- requeues_range_after_nonfinal_lease_success；
- waits_for_persistence_after_final_lease_success；
- marks_finished_only_after_matching_persistence_commit；
- returns_unpersisted_suffix_after_lease_failure；
- ignores_stale_lease_success；
- treats_identical_lease_success_as_duplicate；
- rejects_conflicting_duplicate_outcome；
- rejects_future_generation；
- accepts_short_final_window；
- applies_gap_fact_without_changing_phase；
- ignores_duplicate_gap_transition；
- keeps_persisted_frontier_monotonic；
- rejects_persisted_frontier_past_range_end；
- never_reopens_failed_cancelled_or_finished_range；
- records_late_persisted_fact_after_failure_without_retry；
- treats_duplicate_completion_as_idempotent；
- rejects_completion_before_network_close；
- does_not_finish_cancelled_range_from_late_completion。

`RangeSchedulerTest` 至少覆盖：

- bitmap holes 的 initial union；
- 文件短于 block；
- 正好 `2 * block_size` 的 steal 边界；
- `max_connections == 15/16` 的阈值差异；
- 最大候选并列的确定性选择；
- midpoint align down 后无合法 split；
- donor active Lease 之后才允许 split；
- non-Range 返回完整对象且禁止 steal。

`PersistenceThreadTest` 至少覆盖：

- completion 有 gap 时失败且不发布 committed；
- completion 前 tail 成功写盘；
- tail write 失败不发布 committed；
- persisted/end mismatch；
- duplicate close 同 id 幂等；
- stale close 不完成新 generation；
- duplicate old data 只释放一次 memory/credit；
- duplicate map key 不泄漏 map overhead；
- partial overlap 为确定性 error；
- dynamic stolen Range 在首个 data 前已注册；
- failure 后 drain 已发布 data，但不把 Range 标 finished。

### 15.2 Property tests

固定 seed 并在失败输出 seed。至少验证：

1. 对随机 total、bitmap holes、连接数，初始 Range 并集恰好等于 unfinished blocks；
2. 任意次数合法 steal 后，并集与总字节数守恒、Range 互不重叠；
3. 所有非文件末尾边界按 block 对齐；
4. 连续 acquire/success 产生的 Lease 不重叠并完整覆盖 Range；
5. 任意 stale/duplicate event 插入不改变最终有效状态；
6. `persisted_through` 永不回退且不超过 Range end；
7. 没有 PersistenceCommitted 时，无事件序列能得到 `finished`；
8. matching completion 最多增加一次 finished count。

### 15.3 Concurrency tests

- Persistence 写 fact payload 后 release revision，Orchestrator acquire 后总能看到对应 payload；
- 一百万次 persisted coalescing 不回退、不丢 terminal completion；
- Register/Resize command 必须在 Persistence success ACK 后才允许 arm；测试用 barrier 证明
  ACK happens-before首个 Data；
- registration submit/ACK error 或 Persistence stop 会终止 pending Lease，不会无限等待；
- Data/RangeComplete/close 只在 Packet Flow 的单 producer stream 内保持顺序；
- 动态 steal 注册与 Persistence lookup 无 use-after-free；
- stop/join 后最后一次 drain 能看到 terminal fact；
- sanitizer 可用时在 Clang/GCC 跑 TSAN；MSVC 环境至少跑高重复 stress；
- test teardown 不依赖 sleep 猜测，使用明确 barrier/latch。

### 15.4 Integration tests

复用 `tests/support/range_server.py`：

- 多 window Range 下载内容一致；
- 文件大小不是 block/window 整数倍；
- runtime steal 后内容、完成数和 distinct ports 正确；
- 非 Range 只发一个完整对象请求；
- short body 失败并保留恢复产物；
- server failure 不产生虚假 finished；
- gap pause/resume 后 matching completion；
- interrupted/resume、CRC rollback、transient block reset；
- progress `paused_ranges` 正确汇总 transfer pause 与 gap fact，且 Lifecycle 不复制
  queue/memory/window 状态；
- 成功时所有 Range finished，失败时只统计 Persistence 已确认者。

当前 Windows `error=740` 的两个 CLI 子进程测试按阶段 0 分类；失败集合和原因不得扩大。
library-level Range tests必须独立通过。

## 16. 删除清单

最终 slice 应删除或搬移：

- `core::RangeContext` 整体；
- `RangeContext::end_offset/current_offset/status` 共享 atomics；
- `RangeContext::pause_for_gap/pause_for_memory`；
- `RangeContext::completion_notified/marked_finished`；
- `RangeContext::tail_buffer/out_of_order_queue`；
- engine 的 `mark_range_status()`；
- engine 的 `rollback_inflight_window()`；
- scheduler 的 `next_range_id_`；
- scheduler 对 `unique_ptr<RangeContext>` 的 interface；
- Persistence 的 `ranges_mutex_` 与裸 Range pointer registry；
- 未使用的 `current_metadata_state()`；
- 未使用的 `all_ranges_completed()`；
- 未使用的 `sampled_data_packet_counter_`；
- 无消费方的 out-of-order packet/byte counters；
- `SessionState::cancel_requested`，但只有在全仓再次证明无消费方且 stop 语义已覆盖后删除。

以下内容只搬移，不删除行为：

- tail buffer → `RangeWriteState`；
- out-of-order map → `RangeWriteState`；
- persisted frontier → `RangeWriteState` + published fact；
- legacy metadata DTO → Phase 4 前保留。

## 17. 性能中性合同

结构重构不得顺带改变吞吐策略。

必须保持：

- write callback 不新增 allocation、mutex 或 Lifecycle 调用；
- packet 聚合大小仍为当前 64 KiB；
- queue admission/backpressure 由 Phase 2 interface 决定；
- scheduler window 和 steal 算法不变；
- Persistence 仍是单 writer；
- tail/map/write/bitmap/flush 算法不变；
- persisted facts 可合并，不为每个 callback chunk 分配事件；
- Range registration effect 与 completion control 都是低频操作；
- fact slot 数量与实际 Range 数量同阶；
- 不重新引入已删除的无目的 queue byte/runtime metrics。

本阶段触及 scheduler、callback bookkeeping 和 Persistence 热路径，必须做正式 Release 前后
benchmark。使用阶段 0 的同一 URL、机器、server、case、repeat 和环境。

合并 gate：

| Signal | Gate |
| --- | --- |
| Debug/Release build | 全部通过 |
| 原有通过测试 | 不得新增失败 |
| `baseline_default` network/disk median | 任一下降不得超过 5% |
| `balanced_candidate` network/disk median | 任一下降不得超过 5% |
| `memory_guard` peak memory | 保持约 4.2 MiB 既有行为，除非单独批准 |
| pause count | 多 case 约 15% 物质回归必须调查 |
| packet size metrics | 聚合分布不因本阶段改变 |
| scheduler behavior | window/range/steal 数仅因确定性修 bug 改变 |

若 regression 超阈值，先回滚触发的 slice，再用 profiler 定位；不得调整默认配置掩盖。

## 18. 每个 slice 的 rollback 证据

每个提交都保存：

```text
Base commit:
Slice:
Old owner(s):
New owner:
Interface diff:
Thread/memory-order diff:
Tests added before migration:
Debug/Release result:
Benchmark pre/post:
Known environment failures:
Rollback commit:
Fields deleted:
Open risk:
```

Rollback 原则：

- 单个 slice 失败只回滚该 slice；
- 不使用 destructive git 命令；
- 不把 Phase 1/2 已验证模块一起回滚；
- compatibility projection 最多跨相邻迁移 slices，不能作为永久 fallback；
- 一旦新 interface tests 成为 test surface，删除通过旧共享字段的重复测试；
- 回滚后 active suite 只保留旧实现能通过的 duplicate/stale/failure characterization；
- 仅新 correctness 才能通过的 tests 随对应 C commit 回滚或显式禁用，其用例合同与 red
  输出保留在 evidence；baseline 不得保持 red。

## 19. 验收清单

- [ ] Range 生命周期只有 Orchestrator 一个写入线程。
- [ ] scheduler 不修改 Range runtime state。
- [ ] Persistence 不持有 `RangeContext*`。
- [ ] callback 只读 immutable `RangeLease`。
- [ ] LeaseId 对同一 Range 严格递增且不复用。
- [ ] stale、duplicate、future generation 有确定性结果。
- [ ] Lease 失败后未持久化区域回到未完成集合，但当前 Session 不自动 retry。
- [ ] network success 不能产生 `finished`。
- [ ] matching Persistence completion 是唯一 `finished` 路径。
- [ ] completion 检查最终前沿、gap、tail 与 identity。
- [ ] final short window/block 正确。
- [ ] non-Range 禁止 split/steal，并只使用完整对象 Lease。
- [ ] steal 保持当前阈值、中点和 block 对齐。
- [ ] registration、Packet Flow 与 fact publication 的 release/acquire 合同有 concurrency test。
- [ ] legacy metadata 仍能加载，JSON 字段未删除。
- [ ] public C++、CLI、progress、result 接口兼容。
- [ ] 所有旧共享字段和无消费字段已有删除证明。
- [ ] Debug/Release、集成测试和正式 benchmark 通过 gate。
- [ ] 每个 slice 可独立回滚。

## 20. 风险与待核实事实

以下事实必须在相应 slice 开始前重新核对，但不需要重新询问用户：

1. Phase 2 已把共享 value types 固定在 `src/range/range_types.hpp`，并让 Data DTO 携带
   `LeaseId/ByteSpan`、completion control 携带 `CompletionId/expected_end`。两阶段文档
   必须保持 source-compatible；若后续任一方改名，先同步合同再写生产代码。
2. 当前非 Range + partial resume 的 planner 可能生成多个 unfinished spans，而
   `next_window()` 又返回完整对象。先用测试暴露，再按“单连接完整对象”合同修正；不要保留
   可能重复整文件请求的偶然行为。
3. 当前 Persistence duplicate packet 路径存在会计泄漏风险。修复必须断言 payload、
   packet credit、map overhead 各归还一次。
4. 旧 metadata 的 `end/current/status` 没有恢复消费方，但外部用户可能检查 JSON。Phase 3
   继续写这些字段，Phase 4 才能决定版本化删除。
5. 当前 public interface 没有 cancellation 入口，`cancel_requested` 也没有生产消费方。
   本阶段只实现内部 terminal event，不扩张 public interface。
6. `finished` 不等于 durable VDL。不得为了“更强完成”让每个 Range 等待独立 flush，这会
   改变性能与 checkpoint 语义。
7. 当前 write callback 没有 acquire load donor end；它依赖 immutable request end。
   目标继续使用 immutable Lease，不增加 callback 原子读取。

若 Code Agent 发现与上述核实事实冲突的当前源码，必须保存具体符号、分支和测试证据，暂停
对应 slice；其他独立 slice 可以继续。

## 21. Code Agent 起步顺序

1. 读取本文件、阶段 0、Phase 1/2 最终文档和当前
   `docs/architecture/refactor/domain_glossary.md`；
2. 记录实际 base commit 与 dirty worktree；
3. 只实现 03.1 tests-first；
4. 跑 Debug/Release 基线并分类已知 `error=740`；
5. 逐 slice 迁移，不同时修改 HTTP/Recovery/Telemetry policy；
6. 每个 slice 后做 `rg` 证明写入者减少；
7. 03.5 后跑 fact/ACK concurrency stress；
8. 03.6–03.9 每个 C slice 后跑定向 Release 与可归因 benchmark；
9. 03.11 后跑完整正式 benchmark；
10. 只有全部验收项和 rollback 记录齐全，才把 Phase 3 标记完成。

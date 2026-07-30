# 阶段 2：Packet Flow / Backpressure

## 1. Outcome

本阶段把网络生产者与 persistence consumer 之间分散的 packet queue、逻辑 packet budget、
Accounted Bytes 和 Queue/Memory Pause 生命周期收敛到一个 deep module：
`asyncdownload::flow::PacketFlow`。

阶段完成后：

- `DownloadEngine`、`TransferHandle`、`PersistenceThread`、`RangeContext` 和测试不再 include、
  构造或操作 `moodycamel::BlockingConcurrentQueue`；
- `FlowControlPolicy::packet_budget` 是 Data Packet admission 的逻辑硬预算，不再借用 vendor
  constructor capacity 或 `try_enqueue()` 的返回值表达；
- vendor no-allocation room 暂时不可用与逻辑 budget 耗尽是两个可区分的内部结果；
- Data Packet、Range Complete Control Packet 与 close marker 由同一个 explicit producer
  stream 发布，consumer 可以依赖其 total order；
- payload 从 callback local draft 到 queue、consumer 和 reorder map 始终只有一个所有者；
- Accounted Bytes 的增加、所有权转移、reorder overhead 和最终释放只有一个实现来源；
- Queue Pause 与 Memory Pause 是独立、可重叠、只在状态边沿计数的状态机；
- libcurl callback 在 admission 失败时不消费本批数据，恢复后可以安全接收 replay；
- 正常关闭先停 producer，再按序 drain，最后才结束 persistence 和 flush；
- 正式 Performance Summary 的 10 个 key、64 KiB 聚合和当前 high/low 恢复比较保持不变。

本阶段依赖：

1. [阶段 0：特征化与兼容基线](00_characterization_baseline.md)；
2. [阶段 1：Validated Download Policy](01_validated_download_policy.md)。

Code Agent MUST 先完成这两个阶段。`PacketFlow` 只接收已经验证、不可变的
`FlowControlPolicy`，不得重新读取 Raw Download Options。

## 2. 已核实的当前机制

以下事实来自当前基线源码与 vendored dependency source，不是目标设计假设。

### 2.1 当前项目实现

| 位置 / symbol | 已核实行为 | 结果 |
| --- | --- | --- |
| `src/download/download_engine.cpp:35` `DataQueue` | concrete queue alias 位于 engine implementation | vendor 类型从 seam 泄漏到 persistence 和 tests |
| `TransferHandle::data_queue` / `data_queue_producer` | 每个 handle 保存同一 queue 和 explicit token 的裸指针 | token confinement 依赖调用约定，类型不保护 |
| `TransferHandle::buffered_*` | 每个 handle 保存 64 KiB local aggregation draft 和已计费字节 | draft ownership 与全局会计分离 |
| `reset_transfer_buffer()` | 清空 draft，但不负责统一归还 Accounted Bytes | 所有 cleanup caller 必须自行保证计费已转移或释放 |
| `append_to_transfer_buffer()` | payload 增长时按 delta 增加 global memory accounting | local draft 已进入 memory budget |
| `flush_transfer_buffer()` | 构造 `core::DataPacket`，调用 token overload `try_enqueue` | `false` 被上层解释为 Queue Pause |
| `start_queue_pause()` | 每个连续 episode 只记录一次 `queue_full` pause | 正式 summary 依赖这一边沿语义 |
| `should_pause_for_backpressure()` | `current == 0` 时允许首包超过 high；否则 projected total 超 high 时 pause | 这是已有单测锁定的 Memory Pause admission 语义 |
| `apply_memory_backpressure()` | 已经超过 high 时，暂停 eligible handles 中速度最快的 Top 20%，最少 1 个 | 是第二条 Memory Pause 入口 |
| `resume_paused_transfers()` | Queue Pause 需要 queue depth `< packet_budget` 且 memory `<= low`；Memory Pause 需要 memory `<= low` | Queue 与 Memory 的恢复当前共享 low watermark |
| `flush_transfer_buffer_blocking()` + `stop_network_phase()` | stop 先设 `stop_requested=true`；若 draft 暂因 queue budget 无法发布，blocking flush 立即把该临时状态转为 HTTP failure | task/network stop 可能丢失已经接受但尚未发布的恢复进度 |
| `enqueue_control_packet()` | Range Complete 使用同一 explicit token 的 allocating `enqueue()` | completion 不因 no-allocation room 暂时不足而静默丢失 |
| `PersistenceThread::process_loop()` | `wait_dequeue_timed(..., 100000)`，每次 timeout 轮询 flush | consumer 当前最多每 100 ms 获得一次无 packet 的控制机会 |
| `PersistenceThread::handle_packet()` | regular packet 开始处理时 `queued_packets -= 1` | queue credit 的释放点是 dequeue，不是落盘完成 |
| `PersistenceThread::handle_data_packet()` | direct packet 落盘后释放；out-of-order packet 加 48 bytes map overhead 后继续持有 | queue credit 与 Accounted Bytes 生命周期不同 |
| `release_packet_memory()` | 调用 process-global `MemoryAccounting::subtract()` | 多任务串行假设隐藏在 global singleton 中 |
| `PersistenceThread::stop()` | 不使用 network explicit token，改走 implicit producer 发布 shutdown | shutdown 与 network stream 之间没有可依赖的跨 producer total order |
| `SessionState::queued_packets` | engine、persistence、progress 与 resume 共同读写 | count、budget、observability 和 pause policy 分散 |
| `RangeContext::out_of_order_queue` | concrete `DataPacket` 被保存在 public data bag 中 | packet ownership 泄漏到阶段 3 要替换的结构 |

当前 queue 构造为：

```cpp
BlockingConcurrentQueue<DataPacket>(
    packet_budget,
    1,
    1);
```

network data 和 Range Complete 使用一个 explicit producer token；shutdown 使用 implicit
producer。`BlockingConcurrentQueue` 的 consumer 会在多个 producer subqueue 之间选择，
因此“都在同一个 queue object 中”不足以建立跨 producer FIFO。目标设计不能继续依赖
`PersistenceThread::stop()` 注释中的这一假设。

### 2.2 moodycamel source contract

本阶段结论依据仓库内 vendored source：

- `libs/concurrentqueue/include/concurrentqueue/blockingconcurrentqueue.h:48-53`
  明确说明 constructor `capacity` 只是至少预分配多少 element slots；无额外 allocation 的
  实际容量还取决于 producer 数量与 block size；
- 同文件 `67-68` 的三参数 constructor 把 `minCapacity`、explicit producer 数和 implicit
  producer 数交给 inner queue 计算预分配；
- 同文件 `119-169` 的 `enqueue()` 允许 allocation；失败条件是 allocation failure 或
  `MAX_SUBQUEUE_SIZE`；
- 同文件 `204-253` 的 `try_enqueue()` 不 allocation，当前 producer 没有可复用 block/index
  room 时会返回 `false`；
- `concurrentqueue.h:343` 固定默认 `BLOCK_SIZE = 32`；
- `concurrentqueue.h:364,797-800,992-1008` 规定
  `INITIAL_IMPLICIT_PRODUCER_HASH_SIZE = 0` 才会在 traits 层禁用 implicit production；
- `concurrentqueue.h:352-375` 说明 producer index 和 `MAX_SUBQUEUE_SIZE` 都按 block 工作；
- `concurrentqueue.h:414-420` 建议每个 thread 每类最多一个 token；
- `concurrentqueue.h:685-700` 说明 token 在 allocation failure、move 后或 queue destruction
  后会失效；
- `concurrentqueue.h:803-847` 给出单参数和三参数 constructor 的实际预分配公式；
- `concurrentqueue.h:1848-1899` 显示 `CannotAlloc` 在需要新 index 或 block 时直接返回
  `false`；
- `concurrentqueue.h:1915-1950` 显示 element 只在资源准备完成后才 move-construct 到 queue；
- `concurrentqueue.h:1127-1145` 显示 consumer 在 producer subqueue 之间启发式选择，不能把
  多 producer stream 当作一个全局 FIFO；
- `blockingconcurrentqueue.h:369-385` 说明 timed wait 不 allocation，timeout 时不修改输出。

由此得到本阶段必须使用的机制结论：

1. `packet_budget` 不能通过 vendor constructor capacity 自动成为业务硬容量；
2. `try_enqueue() == false` 只说明 no-allocation 路径当前没有 room，不能命名为
   “逻辑队列已满”；
3. allocating `enqueue() == false` 在目标 traits 下是 terminal resource/backend failure，
   不能被无限重试或静默忽略；
4. 同一 explicit producer stream 内可以建立发布顺序；不同 producer stream 之间不能建立
   本阶段需要的 total order；
5. producer token 必须完全隐藏在 Packet Flow implementation 中；
6. constructor 的 `maxImplicitProducers = 0` 只影响预分配，不能替代
   `INITIAL_IMPLICIT_PRODUCER_HASH_SIZE = 0`；目标 adapter 两者都要设置。

### 2.3 libcurl pause/replay contract

官方 libcurl contract：

- [`CURLOPT_WRITEFUNCTION`](https://curl.se/libcurl/c/CURLOPT_WRITEFUNCTION.html) 规定 callback
  返回 `CURL_WRITEFUNC_PAUSE` 时 transfer 被暂停，并把 body callback 单次 batch 上限定义为
  `CURL_MAX_WRITE_SIZE`；
- [`curl_easy_pause`](https://curl.se/libcurl/c/curl_easy_pause.html) 进一步规定，返回 pause
  表示 callback 没有消费本批任何数据；unpause 后同一批数据会再次交付；
- `curl_easy_pause(..., CURLPAUSE_CONT)` 可能在函数返回前同步调用 write callback；
- `curl_easy_pause()` 不能由另一个 thread 调用；
- pause 时 libcurl 会持有尚未交付的数据，因此这部分 vendor internal memory 不属于
  Packet Flow 的 Accounted Bytes。

因此：

- `PacketProducer::accept()` 只有在返回 `accepted` 时才允许 caller 推进 `next_offset` 和
  `request_bytes`；
- Queue/Memory Pause 返回时，`consumed_bytes` 必须为 0；
- 已经在本次调用中成功发布的旧 draft 可以保留其副作用，但当前 incoming bytes 必须完全
  未消费；
- caller 必须先清除 Packet Flow pause latch，再调用 `curl_easy_pause(..., CONT)`；
- unpause 前后不得把同一 incoming bytes append 两次；
- persistence thread 只发布可见事实，绝不直接调用 `curl_easy_pause()`。

### 2.4 兼容事实与待修正缺口

必须保持：

- 64 KiB TransferHandle aggregation；
- 首个 packet 在 global accounted bytes 为 0 时允许超过 high watermark；
- high 以上 Top 20% fastest eligible handles 的 Memory Pause 行为；
- Queue Pause 与 Memory Pause 到 low watermark 才恢复的现有耦合；
- Range Complete 在前置 Data Packet 之后被 persistence 观察；
- queue credit 在 dequeue 时归还；
- out-of-order map node 固定按 48 bytes 计费；
- Data Packet enqueue 成功才增加 downloaded/packet telemetry；
- Range Complete 和 close 不计入 `packets_enqueued_total` 或 packet-size summary；
- callback 不阻塞 persistence。

必须修正：

- 业务 packet budget 与 vendor preallocation/no-allocation room 混为一谈；
- shutdown 从第二个 producer stream 发出，无法证明不会越过 network packets；
- global accounting 可以跨 Download Request 污染；
- count reservation 发生在 vendor enqueue 之后，存在 consumer 先 dequeue 的竞态窗口；
- `fetch_sub()` 对 underflow 没有防护；
- test helper 直接构造 concrete queue 和手工增减 accounting，测试的是旧结构而不是行为。

## 3. Module responsibilities

### 3.1 Packet Flow owns

`PacketFlow` MUST 独占：

- 每个 producer lane 的 64 KiB local draft；
- Data Packet 与 Control Packet 的 internal envelope；
- 单一 explicit producer token；
- vendor queue construction、preallocation 和调用方式；
- Data admission 的逻辑 hard packet credit；
- vendor no-allocation failure 的独立分类；
- queue depth、Accounted Bytes 与 consumer progress 的运行态计数；
- payload、packet envelope 和 reorder node 的 accounting ledger；
- Queue Pause、Memory Pause 及其 overlap 状态；
- high-watermark Top 20% selection 和 low-watermark resume decision；
- packet sequence；
- close marker、flow state、first terminal error 和 abort drain；
- queue/memory/packet-shape telemetry 的唯一发送点；
- 生产、消费、关闭和 failure injection 的测试 surface。

删除 Packet Flow 后，上述复杂性会重新散回 callback、TransferHandle、SessionState、
PersistenceThread、RangeContext 和 tests；该 module 通过 deletion test。

### 3.2 Packet Flow does not own

Packet Flow MUST NOT：

- 创建、配置、pause 或 cleanup CURL handles；
- 决定 HTTP response 是否有效；
- 决定 Range、Transfer Window 或 Range Lease；
- 修改 Range Lifecycle 的 status、persisted offset、bitmap 或 completion fact；
- 拥有 Gap Pause；Gap Pause 是 persistence 发现的 Range ordering fact；
- 排序或写入文件；
- 触发 flush、CRC、metadata 或 recovery checkpoint；
- 解释 Raw Download Options；
- 改变 64 KiB aggregation target、window size、connection count 或 flush cadence；
- 对外暴露 moodycamel 类型、token、approximate size 或 block size；
- 成为 persistence → orchestrator 的通用事件总线；
- 新增正式 Performance Summary key。

Gap Pause、window-boundary pause 与 Packet Flow pause 在 HTTP caller 中组合。Packet Flow 只返回
自身 Queue/Memory reasons 是否激活；caller 只有在所有 reason 都清除时才 unpause CURL。

## 4. Interface designs considered

### 4.1 方案 A：只包装 concrete queue

形状：

```cpp
class DataQueue {
public:
    bool try_push(DataPacket packet);
    bool wait_pop(DataPacket& packet);
};
```

拒绝原因：

- packet budget、memory accounting、pause state、close ordering 仍留在 callers；
- `bool` 仍无法区分 logical budget、vendor no-allocation room、closed 和 failure；
- payload ownership 仍靠手工约定；
- 删除 wrapper 后，复杂性几乎不增加，是 shallow module。

### 4.2 方案 B：Data Queue 与 Control Queue 分离

形状：

```text
Data Packet ───► bounded data queue ───┐
                                      ├──► persistence merge loop
Control Packet ─► priority queue ─────┘
```

优点是 Control Packet 不与 data credit 竞争。拒绝原因：

- consumer 必须重新建立两个 queue 之间的 total order；
- Range Complete 可能越过尚未消费的 Data Packet；
- sequence merge、dual wakeup 和 dual shutdown 增加 interface；
- 两个 queue 都只有一个 production adapter，seam 没有额外 leverage；
- 本阶段不需要 priority semantics，只需要不可丢和有序。

### 4.3 方案 C：单 producer ordered stream + logical credits + RAII lease，选定

选定形状：

- 一个 orchestrator thread-confined `PacketProducer`；
- 多个轻量 `ProducerLane`，每个对应一个可复用 TransferHandle slot；
- 所有 lane 共享一个 private explicit producer token；
- 一个 persistence thread-confined `PacketConsumer`；
- Data Packet 使用 non-allocating `try_enqueue(token, ...)`；
- Control Packet 与 close marker 使用 allocating `enqueue(token, ...)`；
- Data admission 先取得 logical packet credit，再接触 vendor queue；
- queue credit 在 receive 成功时归还；
- Accounted Bytes 由 move-only `PacketLease` 持有到 payload 真正释放；
- queue envelope 与 `PacketLease` 都使用 inline discriminated state；payload `vector` 只 move，
  不增加 per-packet control-block/pimpl allocation；
- close marker 与 Data/Control 使用同一 producer stream。

选定原因：

- external interface 只有 create、producer accept/flush/control/reconcile/close 和 consumer
  receive/fail；
- queue、token、ledger、pause 和 close 的实现复杂性都隐藏在 module 内；
- caller 和 tests 使用同一个 seam；
- Control ordering 不需要第二条 queue 或跨 producer merge；
- logical budget 与 backend failure 可以分别测试；
- internal queue adapter 可在不污染 external interface 的前提下注入故障；
- 保留单 network producer、单 persistence consumer 的当前线程模型。

## 5. Dependency direction and files

新增内部文件：

```text
src/range/range_types.hpp
src/flow/packet_flow.hpp
src/flow/packet_flow.cpp
src/flow/packet_queue_adapter.hpp
tests/flow/packet_flow_test.cpp
```

允许的 dependency direction：

```text
download::FlowControlPolicy
telemetry::TelemetrySession
range::dependency-neutral value types
            │
            ▼
       flow::PacketFlow
            │
            ├── production compile-time seam ──► MoodycamelPacketQueueAdapter
            │
            ├── test-only compile-time seam ───► FaultInjectingPacketQueueAdapter
            │
            ├──► DownloadEngine / HTTP callback uses PacketProducer
            └──► PersistenceThread uses PacketConsumer
```

`FlowControlPolicy` 与 telemetry 是 in-process dependencies。使用现有
`TelemetrySession` concrete facade，不新增 telemetry port；当前只有一个实现，引入 port 会形成
hypothetical seam。

vendor queue seam 只有一个 product implementation：

- production `MoodycamelPacketQueueAdapter`。

`FaultInjectingPacketQueueAdapter` 只是 test target 编译时替换的 deterministic failure
seam，不是第二个产品实现，不得链接进 production binary。该 seam MUST 是
implementation-private compile-time seam，不能在 hot path 使用 virtual dispatch，也不能进入
public/internal module interface。测试仍通过同一 Packet Flow interface 观察结果。

`PacketFlow::Implementation` SHOULD 是以 queue adapter type 参数化的 private template。
production factory只实例化 moodycamel type；test-only translation unit实例化 fault type并返回
相同的 `std::unique_ptr<PacketFlow>`。不得在每个 packet operation 上保存 function pointer、
`std::function`、virtual queue base 或 runtime adapter tag。

`src/range/range_types.hpp` 只定义 Packet Flow 和后续 Range Lifecycle 共同使用的
dependency-neutral value types，不包含 range state、scheduler、writer 或 event publication。
本阶段创建该 header；阶段 3 在不移动或复制这些类型的前提下取得其语义所有权。

禁止的 dependency：

- `packet_flow.hpp` include `core/models.hpp`、libcurl、FileWriter、MetadataStore、
  RangeScheduler 或 Range Lifecycle implementation；
- `download_engine.cpp` / `persistence_thread.hpp` include moodycamel；
- `core/models.hpp` 保存 vendor queue、token 或 global accounting reference；
- public `include/asyncdownload/` include `src/flow/packet_flow.hpp`。

## 6. Exact C++ interface

Code Agent SHOULD 使用以下最终 interface。若阶段 1 合并后 internal namespace 有机械冲突，
可以调整私有 helper 名称，但 MUST 保持方法数量、结果分类、所有权和 ordering 语义。

`src/range/range_types.hpp`：

```cpp
#pragma once

#include <cstdint>

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

}
```

`src/flow/packet_flow.hpp`：

```cpp
#pragma once

#include "download/download_policy.hpp"
#include "range/range_types.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <system_error>
#include <vector>

namespace asyncdownload::telemetry {
class TelemetrySession;
}

namespace asyncdownload::flow {

using PacketLaneId = std::uint32_t;
using PacketSequence = std::uint64_t;

enum class PacketKind : std::uint8_t {
    data = 0,
    control = 1
};

enum class ControlPacketKind : std::uint8_t {
    range_complete = 0
};

enum class PacketAdmissionCode : std::uint8_t {
    accepted = 0,
    packet_budget_exhausted,
    memory_budget_exhausted,
    backend_temporarily_unavailable,
    closed,
    failed
};

enum class PacketFlowState : std::uint8_t {
    open = 0,
    closing,
    closed,
    failed
};

enum class PacketPauseReason : std::uint8_t {
    none = 0,
    queue = 1,
    memory = 2
};

enum class PacketPauseActionKind : std::uint8_t {
    pause_receive = 0,
    resume_candidate
};

enum class PacketReceiveCode : std::uint8_t {
    packet = 0,
    timeout,
    closed,
    failed
};

enum class PacketPublishCode : std::uint8_t {
    published = 0,
    closed,
    failed
};

struct DataChunk {
    range::LeaseId lease{};
    range::ByteSpan lease_span{};
    range::ByteOffset offset = 0;
    std::span<const std::uint8_t> bytes{};
};

struct DataPacket {
    range::LeaseId lease{};
    range::ByteSpan lease_span{};
    range::ByteOffset offset = 0;
    std::vector<std::uint8_t> payload;
};

struct ControlPacket {
    ControlPacketKind kind = ControlPacketKind::range_complete;
    range::CompletionId completion{};
    range::ByteOffset expected_end = 0;
};

struct PacketAdmission {
    PacketAdmissionCode code = PacketAdmissionCode::failed;
    std::size_t consumed_bytes = 0;
    std::size_t published_bytes = 0;
    std::uint8_t active_pause_mask = 0;
    std::error_code error{};

    [[nodiscard]] bool accepted() const noexcept;
    [[nodiscard]] bool must_pause() const noexcept;
};

struct PacketFlowSnapshot {
    PacketFlowState state = PacketFlowState::open;
    std::size_t queued_packets = 0;
    std::size_t accounted_bytes = 0;
    std::uint64_t published_data_bytes = 0;
    std::error_code error{};
};

struct PacketLaneObservation {
    PacketLaneId lane_id = 0;
    double bytes_per_second = 0.0;
    bool eligible_for_memory_pause = false;
};

struct PacketPauseAction {
    PacketLaneId lane_id = 0;
    PacketPauseActionKind kind = PacketPauseActionKind::pause_receive;
    std::uint8_t active_pause_mask = 0;
};

struct PacketReceiveResult {
    PacketReceiveCode code = PacketReceiveCode::failed;
    std::error_code error{};
};

struct PacketPublishResult {
    PacketPublishCode code = PacketPublishCode::failed;
    std::error_code error{};
};

struct PacketReconcileResult {
    std::size_t action_count = 0;
    std::error_code error{};
};

class PacketFlow;

class ProducerLane {
public:
    ProducerLane() noexcept = default;
    ProducerLane(ProducerLane&& other) noexcept;
    ProducerLane& operator=(ProducerLane&& other) noexcept;
    ~ProducerLane();

    ProducerLane(const ProducerLane&) = delete;
    ProducerLane& operator=(const ProducerLane&) = delete;

    [[nodiscard]] PacketLaneId id() const noexcept;

private:
    PacketFlow* owner_ = nullptr;
    PacketLaneId id_ = 0;

    friend class PacketProducer;
};

class PacketLease {
public:
    PacketLease() noexcept = default;
    PacketLease(PacketLease&& other) noexcept;
    PacketLease& operator=(PacketLease&& other) noexcept;
    ~PacketLease();

    PacketLease(const PacketLease&) = delete;
    PacketLease& operator=(const PacketLease&) = delete;

    [[nodiscard]] bool has_value() const noexcept;
    [[nodiscard]] PacketKind kind() const noexcept;
    [[nodiscard]] PacketSequence sequence() const noexcept;
    [[nodiscard]] const DataPacket* data() const noexcept;
    [[nodiscard]] const ControlPacket* control() const noexcept;
    [[nodiscard]] std::error_code account_reorder_node() noexcept;
    void complete() noexcept;

private:
    PacketFlow* owner_ = nullptr;
    DataPacket data_{};
    ControlPacket control_{};
    PacketSequence sequence_ = 0;
    std::size_t accounted_bytes_ = 0;
    PacketKind kind_ = PacketKind::data;
    bool has_value_ = false;
    bool reorder_node_accounted_ = false;

    friend class PacketConsumer;
};

class PacketProducer {
public:
    PacketProducer(const PacketProducer&) = delete;
    PacketProducer& operator=(const PacketProducer&) = delete;
    PacketProducer(PacketProducer&&) = delete;
    PacketProducer& operator=(PacketProducer&&) = delete;

    [[nodiscard]] std::error_code open_lane(ProducerLane& lane) noexcept;
    [[nodiscard]] PacketAdmission accept(ProducerLane& lane,
                                         DataChunk chunk) noexcept;
    [[nodiscard]] PacketAdmission flush(ProducerLane& lane) noexcept;
    [[nodiscard]] std::error_code discard(ProducerLane& lane) noexcept;

    [[nodiscard]] PacketPublishResult publish(ControlPacket packet) noexcept;

    [[nodiscard]] PacketReconcileResult reconcile(
        std::span<const PacketLaneObservation> observations,
        std::span<PacketPauseAction> actions) noexcept;

    [[nodiscard]] bool paused(const ProducerLane& lane) const noexcept;
    [[nodiscard]] std::uint8_t pause_mask(const ProducerLane& lane) const noexcept;
    [[nodiscard]] PacketFlowSnapshot snapshot() const noexcept;
    [[nodiscard]] std::error_code close() noexcept;

private:
    explicit PacketProducer(PacketFlow& owner) noexcept;

    PacketFlow& owner_;

    friend class PacketFlow;
};

class PacketConsumer {
public:
    PacketConsumer(const PacketConsumer&) = delete;
    PacketConsumer& operator=(const PacketConsumer&) = delete;
    PacketConsumer(PacketConsumer&&) = delete;
    PacketConsumer& operator=(PacketConsumer&&) = delete;

    [[nodiscard]] PacketReceiveResult receive(
        PacketLease& lease,
        std::chrono::microseconds timeout) noexcept;

    [[nodiscard]] std::error_code fail(std::error_code error) noexcept;

private:
    explicit PacketConsumer(PacketFlow& owner) noexcept;

    PacketFlow& owner_;

    friend class PacketFlow;
};

class PacketFlow {
public:
    [[nodiscard]] static std::error_code create(
        const download::FlowControlPolicy& policy,
        telemetry::TelemetrySession& telemetry,
        std::unique_ptr<PacketFlow>& result) noexcept;

    ~PacketFlow();

    PacketFlow(const PacketFlow&) = delete;
    PacketFlow& operator=(const PacketFlow&) = delete;

    [[nodiscard]] PacketProducer& producer() noexcept;
    [[nodiscard]] PacketConsumer& consumer() noexcept;

private:
    class Implementation;

    explicit PacketFlow(std::unique_ptr<Implementation> implementation) noexcept;

    std::unique_ptr<Implementation> implementation_;
    PacketProducer producer_;
    PacketConsumer consumer_;
};

}
```

### 6.1 Interface constraints

- `PacketFlow::create()` 不得让 allocation exception 穿透；vendor constructor、`vector`
  growth 和 module allocation failure 映射为 `std::error_code`。
- `ProducerLane` 只是 lane capability，不包含或暴露 vendor token。
- 一个 `PacketFlow` 只有一个 `PacketProducer` 与一个 `PacketConsumer`。
- `PacketProducer` 和 `PacketConsumer` 均不可复制、不可移动，capability 地址在
  `PacketFlow` 生命周期内稳定。
- `PacketProducer` 的所有方法只允许 orchestrator thread 调用。
- `PacketConsumer::receive()` 和 `fail()` 只允许 persistence thread 调用。
- `PacketLease` 可以在 persistence thread 内 move 到 reorder map，但不能跨到另一个 thread。
- `PacketLease` 使用 inline state；除 `DataPacket::payload` 的现有 `vector` storage 与 vendor
  queue node 外，不得为每个 packet 新增 pimpl、shared state 或额外 heap allocation。
- `receive()` 要求输出 lease 当前 `has_value() == false`；`packet` 结果使它为 true，
  `timeout`、`closed` 和 `failed` 均保持其 empty。
- `kind()` 和 `sequence()` 只允许在 `has_value()` 为 true 时调用；`data()` / `control()`
  对不匹配的 kind 返回 `nullptr`。
- `PacketFlow` 生命周期必须覆盖所有 lane、lease、persistence thread 和 Range reorder storage。
- `PacketAdmission::error` 只在 `failed` 时非空。
- `PacketPublishResult::error` 只在 `failed` 时非空；`closed` 是非错误终态，调用方不得把
  control 当作已发布。
- `PacketReconcileResult::error` 为空时，`action_count <= actions.size()`。
- `DataChunk::bytes.size()` 的 module contract 上限是 64 KiB；当前 libcurl body batch 的
  `CURL_MAX_WRITE_SIZE` 低于该值。超限、空 span、offset arithmetic overflow 或 span 越界
  返回 terminal `failed` + `std::errc::invalid_argument`，且不修改 draft/accounting。
- `packet_budget_exhausted` 与 `backend_temporarily_unavailable` 都进入 Queue Pause，但测试和
  internal diagnostics 可以区分。
- `closed` 不等于 failure；close 后的 publish/accept 分别返回
  `PacketPublishCode::closed` / `PacketAdmissionCode::closed`，且不修改输入状态。
- `published_data_bytes` 只在 Data Packet 成功进入 ordered stream 后 checked 增加；draft
  append、Control、close、replay failure 或 admission pause 都不增加。它是 progress
  新增网络字节的 request-local authoritative counter；恢复的初始 trusted bytes 不进入该
  counter，也不产生 network telemetry。

## 7. Data admission and ownership

### 7.1 Producer lane and local draft

每个 TransferHandle slot 在 network loop 启动前调用一次 `open_lane()`。Packet Flow
implementation 为该 lane 保存：

- 当前 draft 的 immutable `LeaseId`；
- 当前 draft 的 immutable half-open `ByteSpan`；
- draft 起始 offset；
- contiguous payload；
- draft 的 Accounted Bytes；
- Queue Pause bit；
- Memory Pause bit。

`accept(lane, chunk)` 的固定算法：

1. 检查 flow 是 `open`、lane 属于当前 flow、`chunk.bytes` 非空且不超过 64 KiB；
2. checked 验证 `0 <= lease_span.begin < lease_span.end`、
   `offset >= lease_span.begin` 且 `offset + bytes.size() <= lease_span.end`；这里只做
   结构/bounds 验证，不判断 lease 是否 stale；
3. 若 draft 非空，要求 `LeaseId` 与 `ByteSpan` 完全相同且
   `draft.offset + draft.payload.size() == chunk.offset`；
4. 若加入 incoming 会超过 64 KiB，先尝试发布旧 draft；
5. 旧 draft 发布失败时返回 Queue Pause，incoming 完全不复制；
6. 计算新 draft 的 projected Accounted Bytes；
7. 按当前 memory admission 规则尝试增加 delta；
8. memory admission 失败时返回 Memory Pause，incoming 完全不复制；
9. reserve/copy incoming；allocation failure 回滚新增 accounting 并返回 terminal failure；
10. 返回 `accepted`，`consumed_bytes == chunk.bytes.size()`；
11. 若本调用发布了旧 draft，`published_bytes` 返回旧 payload bytes。

`LeaseId`、`CompletionId` 和 lease bounds 的语义由阶段 3 Range Lifecycle 验证。Packet Flow
只保证按值原样运输、draft 内不可变、checked half-open bounds 和发布顺序；它不注册 lease、
不判断 generation 是否 current，也不发布 persisted/finished fact。

`flush(lane)`：

- empty draft 是 `accepted` no-op；
- 成功时把 draft move 到 queue envelope，不重复增加 Accounted Bytes；
- logical credit 或 vendor room 不可用时，draft 原封不动保留在 lane；
- 成功后清空 lane draft，`published_bytes` 等于 payload size；
- Data Packet enqueue 成功时统一记录 packet/download telemetry。

`discard(lane)`：

- 仅用于 Packet Flow/Persistence 已 terminal、draft 已无法合法发布，或尚未接线 lane 的
  teardown；
- 普通、可恢复或 task-cancelled HTTP failure 在 Packet Flow 仍 open 时必须先
  `flush(lane)`，保留已经接受的恢复进度，不得直接 discard；
- 释放仍属于 local draft 的全部 Accounted Bytes；
- 不增加 downloaded bytes、packet telemetry 或 queue count；
- 清除该 lane 的 Queue/Memory Pause bits；
- 不回滚已经成功发布的 packet。
- lane 不属于当前 flow、已经 detach 或调用线程不符合 owner contract 时返回
  `internal_error`，且不修改任何 ledger 或 lane 状态。

### 7.2 Data Packet ownership path

唯一合法路径：

```text
libcurl callback bytes
    │ accepted
    ▼
Packet Flow lane draft
    │ flush: move
    ▼
private queue envelope
    │ receive: move + queue credit returned
    ▼
PacketLease on persistence thread
    ├── direct append ───────────────► complete
    └── reorder map: move lease ─────► drain ─► complete
```

禁止：

- callback 在 admission 失败后复制 incoming 到第二个 buffer；
- queue 保存指向 callback memory 的 span；
- persistence 从 lease copy payload 到另一份 owned vector 再提前 complete lease；
- reorder map 只保存 `DataPacket` 而丢掉 lease；
- caller 手工修改 `accounted_bytes`；
- caller 在 `try_enqueue()` 后自行恢复被 move 的 payload；
- packet 同时被 local draft、queue 和 consumer 两处视为 owner。

## 8. Logical hard packet budget versus vendor failure

### 8.1 Logical credit

`FlowControlPolicy::packet_budget` 是 Data admission 的逻辑 hard budget。

计数口径：

- `queued_packets` 包含已发布、尚未 receive 的 Data Packet 与 Range Complete Control Packet；
- internal close marker 不进入 progress count；
- Data admission 只有在 `queued_packets < packet_budget` 时才可以取得一个 credit；
- Control Packet bypass Data admission gate，但仍增加 `queued_packets`；
- 因 Control Packet 暂时使 depth 超过 budget 时，后续 Data admission 必须等待 depth 回落；
- receive 成功、在 packet 交给 persistence 前归还一个 queue credit；
- PacketLease 的后续处理不占 queue credit，但继续占 Accounted Bytes。

Data publish 必须先原子 reserve credit，再调用 vendor queue。若 vendor call 失败，credit
立即回滚。这样 consumer 不可能 dequeue 一个尚未计数的 packet，也不会出现 count underflow
窗口。

### 8.2 Vendor adapter contract

production adapter：

- 使用一个 custom traits 禁用 implicit producer path；
- 三参数预分配使用 `maxExplicitProducers = 1`、`maxImplicitProducers = 0`；
- 预分配 arithmetic 必须 checked；
- 只创建一个 explicit producer token；
- Data Packet 只调用 token overload `try_enqueue()`；
- Range Complete 与 close marker 调用同一 token 的 `enqueue()`；
- consumer 使用一个 consumer token；
- `size_approx()` 不参与业务 budget、resume 或 acceptance。

custom traits 的必要形状：

```cpp
struct PacketQueueTraits final : moodycamel::ConcurrentQueueDefaultTraits {
    static constexpr std::size_t INITIAL_IMPLICIT_PRODUCER_HASH_SIZE = 0;
};

using PacketQueue = moodycamel::BlockingConcurrentQueue<
    PacketEnvelope,
    PacketQueueTraits>;
```

`maxImplicitProducers = 0` 仍传给三参数 constructor，但它只描述预分配；真正让无 token
`enqueue()`/`try_enqueue()` 返回 false 的是上述 trait。production adapter 自身也不得提供
无 token overload。

结果分类：

| Condition | Admission result | State effect | Payload |
| --- | --- | --- | --- |
| logical credit unavailable | `packet_budget_exhausted` | enter Queue Pause once | draft retained |
| credit available, `try_enqueue` false | `backend_temporarily_unavailable` | enter Queue Pause once | draft retained |
| flow closing/closed | `closed` | no new pause count | draft retained until discard |
| Data envelope construction/allocation error | `failed` | flow failed | draft retained then discarded |
| Control allocating enqueue false | terminal `failed` | flow failed | Control not reported as delivered |
| close marker enqueue false | terminal `failed` | consumer observes failure after timed wake fallback | no silent success |

Control publish 使用独立的 `PacketPublishResult`：成功为 `published`；flow 已进入
closing/closed 时为无错误的 `closed`；构造、backend、顺序或 flow failure 为带
first error 的 `failed`。调用方只有看到 `published` 才能推进 completion effect。

`backend_temporarily_unavailable` 不得重新命名为 `queue full` 根因。为了保持当前行为，本阶段
不增加 consumer epoch、drain hysteresis 或 retry backoff；它与 logical credit Queue Pause
沿用相同 low-watermark resume gate。

### 8.3 Control reserve behavior

Control Packet 不得因 Data Packet hard budget 暂时不可用而丢失：

- caller 先成功 flush 与 `completion.range` 相关的所有 lane draft；
- `expected_end` 是 half-open Range 的 exclusive end；HTTP inclusive last byte 只能由
  HTTP caller 在检查非空后转换为 `expected_end - 1`；
- `publish(range_complete)` checked 验证 `expected_end > 0`，并检查没有
  `lease.range == completion.range` 的残留 draft；
- Packet Flow 不比较 lease generation 与 completion generation，也不判断
  `expected_end` 是否等于 Range `ByteSpan::end` 或实际 persisted frontier；这些是阶段 3 的
  semantic check；
- Control 使用 allocating enqueue，不调用 no-allocation `try_enqueue`；
- Control enqueue failure 是 terminal error，不是“稍后当作成功”；
- Control 与 Data 使用同一 token 和 sequence；
- Control 被 receive 时才归还其 queue count。

这里的“不可丢”不等于“真实 OOM 时伪造成功”。terminal allocation/backend failure 必须停止
network、保留恢复产物并返回非零错误。

## 9. Data/Control ordering and sequence

Packet Flow 为每个成功发布的 Data/Control envelope 分配单调递增
`PacketSequence`。sequence 只由 orchestrator-confined producer 写入。

必须成立：

1. 同一 lane 的 draft 内 bytes 按 offset contiguous；
2. `LeaseId` 与 `ByteSpan` 在一个 draft 和发布后的 Data Packet 内保持不可变；
3. 同一 Range 的 Data Packets 按 producer publish 顺序进入 stream；
4. `range_complete(completion, expected_end)` 的 sequence 大于
   `completion.range` 所有已发布 Data Packet；
5. Packet Flow 按值保留 CompletionId 和 expected_end，不把它们降级为裸 RangeId；
6. close marker 的 sequence 大于全部 regular packets；
7. single consumer 观察到严格递增 sequence；
8. persistence 只有处理 Range Complete 后才允许 flush final tail 和发布 persisted
   completion；
9. close marker 由 `receive()` 内部消费；PersistenceThread 不再认识 shutdown packet kind。

不要求不同 Range 按 file offset 全局排序。total order 描述的是发布事件顺序，Range 内的
offset reorder 仍由 persistence/阶段 3 Range Lifecycle 处理。

本阶段 MUST 增加一个当前实现会暴露风险的 characterization：

- network explicit stream 中先放 Data/Range Complete；
- shutdown 从另一个 producer stream 发出；
- 证明不能把 cross-producer dequeue order 当作合同。

该测试用于说明旧假设不可依赖，不要求通过随机运行复现数据丢失。目标 Packet Flow test 必须
通过 single producer sequence 明确证明新 ordering。

## 10. Accounted Bytes ledger

### 10.1 Accounted categories

保持现有口径：

| Ownership location | Accounted Bytes |
| --- | ---: |
| empty lane draft | `0` |
| non-empty lane draft | `payload.size() + sizeof(DataPacket)` |
| queued Data Packet | 与 draft 相同，不重复增加 |
| consumer PacketLease direct path | 与 queued packet 相同 |
| reorder map PacketLease | 上述值 `+ 48` |
| Data Packet complete/discard | `0`，原值恰好减一次 |
| Control Packet / close marker | `0` |
| fixed tail buffer | 不进入本阶段 ledger，保持现状 |
| libcurl paused internal buffer | vendor-owned，不可由 Packet Flow可靠观测 |

`sizeof(DataPacket)` 是本阶段新类型的固定 envelope accounting。若它与旧
`sizeof(core::DataPacket)` 不同，pre/post benchmark evidence 必须记录差值；不得加入任意
magic compensation 伪造旧数值。

### 10.2 Ledger operations

只有 Packet Flow implementation 可以：

- grow/shrink lane draft accounting；
- 在 publish 时转移 ledger owner；
- 在 `account_reorder_node()` 首次调用时增加固定 48 bytes；
- 在 lease complete/destructor 时减少；
- 在 abort drain 时减少；
- 记录 memory peak sample。

每笔 owner transfer 的规则：

```text
reserve once
move ownership zero or more times
add reorder overhead at most once
release exactly once
```

`PacketLease::account_reorder_node()` 重复调用返回 `internal_error`，不得再次增加。lease
move assignment 必须先 complete 自身旧 owner，再接管新 owner。

所有减法使用 checked compare/CAS，不使用可 wrap 的裸 `fetch_sub`。发现 underflow：

- 保留 first terminal error；
- flow 进入 `failed`；
- 测试必须看到 failure；
- 不能 reset counter 掩盖。

### 10.3 Session locality

删除 process-global `global_memory_accounting()`。每个 Download Request 的 accounting
存放在对应 Packet Flow instance 中：

- 不需要任务开始时 global `reset()`；
- 两个并发 DownloadClient request 不互相影响；
- progress 的 `queued_packets` 与 `memory_bytes` 从 `PacketFlowSnapshot` 读取；
- progress/result 的 `downloaded_bytes` 由 merger checked 相加
  `recovery_initial_trusted_bytes + PacketFlowSnapshot::published_data_bytes`；
- `max_memory_bytes` 仍由 Telemetry Session 的 task-local event 累积；
- `max_inflight_bytes` 的既有 downloaded/persisted 口径不变。

## 11. Queue/Memory Pause state machine

### 11.1 Independent reason bits

每个 lane 维护两个 plain bits，只有 orchestrator thread 读写：

- `queue`;
- `memory`.

Queue Pause entry：

- Data publish 命中 logical budget；
- 或 vendor no-allocation publish failure。

Memory Pause entry：

- callback projected Accounted Bytes 会超过 high；
- 或 event-loop reconcile 发现 current Accounted Bytes 已经超过 high，并把当前 lane 选入
  fastest Top 20%。

一段连续 episode 只在 bit `0 → 1` 时调用一次：

```cpp
TelemetrySession::record_pause(TelemetryPauseReason::queue_full, true);
TelemetrySession::record_pause(TelemetryPauseReason::memory_pressure, false);
```

重复 admission failure 不重复增加 pause count。

### 11.2 Reconcile algorithm

`reconcile(observations, actions)` 的调用前置条件是
`actions.size() >= observations.size()`。module 先完成线程、lane identity、重复 lane、
数值和容量校验；任一校验失败返回 error、`action_count == 0`，并且不得修改 pause bits。
因此调用方不会面对“已经改变一半状态但输出 buffer 不够”的结果。验证通过后固定执行：

1. 读取一次 current Accounted Bytes；
2. 若 `current > high`，从 `eligible_for_memory_pause` 且尚未 memory-paused 的 lanes 中按
   `bytes_per_second` 降序选择 `max(1, ceil(count / 5))`；
3. rate 相等时按 `PacketLaneId` 升序，保证 deterministic tests；
4. 对新选中 lane 设置 memory bit，输出 `pause_receive`；
5. 若 `current > low`，不清除任何 Queue/Memory bit；
6. 若 `current <= low`：
   - 清除全部 memory bits；
   - 仅当 `queued_packets < packet_budget` 时清除 queue bit；
7. 只有某 lane 的 Packet Flow mask 从非 0 变为 0 时输出 `resume_candidate`；
8. caller 再检查 Gap Pause 和 window-boundary pause；仍有其它 reason 时不 unpause CURL。

成功时 action 写入 `actions.first(action_count)`；其余元素保持未指定，调用方不得读取。
单次 reconcile 最多为每个 observation 产生一个 action，所以前述容量合同是充分条件。

该算法有意保持现有 Queue Pause 对 low watermark 的依赖。不得在本阶段改为：

- high watermark 恢复；
- packet drain hysteresis；
- 每轮只恢复一个 handle；
- consumer-progress epoch；
- time-based backoff。

### 11.3 Composite caller rule

HTTP caller 的 composite pause：

```text
should_pause =
    PacketFlow queue bit
    OR PacketFlow memory bit
    OR Range Lifecycle gap fact
    OR HTTP window-boundary fact
```

`RangeStatus::paused` 在阶段 3 前仍可以由 compatibility adapter 更新，但 Packet Flow 不写
RangeContext。阶段 3 完成后，Range Lifecycle 根据显式 facts 构造状态。

当 Packet Flow 输出 `resume_candidate`：

1. caller 先把 lane state 视为 runnable；
2. caller acquire-read Gap fact；
3. caller 检查 window-boundary fact；
4. 全部清除后，由 orchestrator thread 调用 `curl_easy_pause(easy, CURLPAUSE_CONT)`；
5. 因 callback 可能在 `curl_easy_pause` 返回前重入，TransferHandle/lane 必须已经处于完整
   可接收状态。

## 12. Close and error paths

### 12.1 Normal close

正常或可恢复 network failure 的顺序：

```text
stop new scheduling
    ↓
remove/stop CURL production
    ↓
flush every non-empty lane draft
    ↓
publish pending Range Complete controls
    ↓
PacketProducer::close()
    ↓
enqueue private close marker with the same explicit token
    ↓
PacketConsumer drains all prior packets
    ↓
receive returns closed
    ↓
Persistence final flush/checkpoint
    ↓
join persistence thread
```

`close()` 前置条件：

- 没有 active producer operation；
- 所有 lane draft empty；
- 没有尚未 publish 的 Range Complete；
- 只调用一次。

若 draft 仍非空，`close()` 返回 internal invalid-order error，不得偷偷 discard。

本阶段保留当前 `flush_transfer_buffer_blocking()` 的 1 ms compatibility wait 语义，只把它
改为调用 `PacketProducer::flush()`。不要借结构重构把它改成 yield、event-loop deferred
finalization 或新的 retry strategy；相关实验已经被 reject。阶段 5 可以在独立 benchmark
实验中重新设计 HTTP request finalization。

Slice 9 必须同时修正 stop 路径的现有 early-abort：

- `stop_requested` 只禁止新调度和新 network production，不把既有 draft 的
  `packet_budget_exhausted` / `backend_temporarily_unavailable` 转成 terminal failure；
- 当 Packet Flow 仍 open 且 Persistence consumer 仍 active 时，每 1 ms 重试
  `PacketProducer::flush()`，直到 draft 发布或上游进入 terminal；
- Persistence/Packet Flow terminal 时停止等待，保留 first error并 `discard(lane)`；
- 只有 draft 全部 empty 后才允许发布 Control/close；
- 该循环不是 HTTP retry，不重发请求，也不改变 queue resume threshold。

### 12.2 Persistence failure

FileWriter、metadata 或 checkpoint error：

1. persistence thread 保存 first error；
2. 同一 thread 调用 `PacketConsumer::fail(error)` 并检查返回值；
3. flow state release-store 为 `failed`；
4. consumer adapter 在该 thread drain/destroy 尚未交付的 envelopes；
5. drain 为每个 Data Packet 归还 queue credit 与 Accounted Bytes；
6. producer acquire-read failure 后，所有新 accept/flush/control 都返回 `failed`；
7. orchestrator 停止 CURL；
8. 每个 lane `discard()` 归还 local draft；
9. join，保留 `.part` 与 metadata。

consumer fail 与正在进行的 producer admission 通过 internal active-operation counter
线性化。fail 先阻止新 admission，再等待已经进入 operation 的单 producer 调用完成，之后才
drain。不得让一个 late enqueue 落在 abort drain 之后。

### 12.3 Control/close allocation failure

Range Complete 或 close marker 使用 allocating enqueue。如果返回 `false`：

- 不得报告 control delivered；
- flow 进入 failed；
- consumer timed wait 最迟在当前 100 ms poll interval 后观察 failure；
- consumer drain remaining packets；
- outer DownloadResult 返回非零 error；
- recovery artifacts 保留。

本阶段不把 vendor `false` 精确伪装成 OOM。production traits 已禁用 implicit producer，且
`MAX_SUBQUEUE_SIZE` 维持默认；adapter 可以把确定捕获的 `std::bad_alloc` 映射到
`std::errc::not_enough_memory`，其它不可能细分的 backend failure 映射到
`DownloadErrc::internal_error`。

### 12.4 Destruction

`PacketFlow::~PacketFlow()` 的合法前置状态是 `closed` 或 `failed`，且：

- persistence thread 已 join；
- queue empty；
- 所有 PacketLease 已 complete；
- 所有 ProducerLane 已销毁或 detach；
- Accounted Bytes 为 0；
- queued packets 为 0。

Debug build 使用 assertion；Release build 若发现残留，必须 best-effort release internal
owners 并记录 first error，不能 terminate process 或 throw。

## 13. Threading and memory order

### 13.1 Thread ownership

| State | Writer | Reader | Synchronization |
| --- | --- | --- | --- |
| lane draft/pause bits/rate choice | orchestrator only | orchestrator only | plain values |
| explicit producer token | orchestrator only | none | type/private confinement |
| consumer token | persistence only | none | type/private confinement |
| envelope payload | producer then consumer | one owner at a time | queue publish/consume |
| `queued_packets` | producer reserve, consumer release | both/progress | atomic |
| `accounted_bytes` | producer/reorder add, consumer release | both/progress | atomic |
| flow state | producer close or consumer fail | both | release/acquire |
| first error | first failing side | both | low-frequency mutex |
| sequence next | producer only | consumer after dequeue | queue synchronization |
| PacketLease internal owner | persistence only | persistence only | plain values |

### 13.2 Required orders

- flow state transition：`store(..., memory_order_release)`；
- flow state observation：`load(memory_order_acquire)`；
- active operation enter/leave：`fetch_add/fetch_sub(memory_order_acq_rel)`；
- queue/count credit reserve/rollback/release：checked CAS/RMW `memory_order_acq_rel`；
- Accounted Bytes reserve/release：checked CAS/RMW `memory_order_acq_rel`；
- progress-only snapshot after state unchanged：`memory_order_acquire`；
- vendor queue own release/acquire publishes envelope payload；
- 不为 payload 再加一套手写 fence；
- Range `end_offset`、Gap fact 和 persisted completion 的 memory order 由阶段 3 定义，
  Packet Flow 不复写。

纯统计 counter 可以 relaxed 的前提是它不参与 admission、close、pause 或 ownership
correctness。`queued_packets` 和 `accounted_bytes` 参与 correctness，不能按“只是指标”用
relaxed 解释。

### 13.3 Token confinement

- token 是 `PacketFlow::Implementation` private member；
- `PacketProducer` 不可复制；
- `ProducerLane` 不包含 token；
- TransferHandle 只保存 lane capability；
- `PersistenceThread::stop()` 不再 enqueue；
- custom traits 禁止任何 implicit enqueue；
- Debug build 记录 producer thread id，并在所有 producer entry 断言一致；
- producer/consumer capability 只能在其 owner thread 使用；这是内部类型的调用前置条件，
  由构造与生命周期结构保证，不能作为可恢复的跨线程 API 使用；
- 所有有返回值的 cold mutation entry（open/discard/publish/reconcile/close/fail）在
  Release 检测到 wrong-thread 时返回 internal failure 且无副作用；
- `accept`/`flush` 的 hot path 不增加 Release thread-id lookup；wrong-thread misuse 由
  Debug assertion、owner-thread integration test 和无跨线程借用的类型布局阻止；
- RAII `PacketLease` cleanup 只在 Persistence owner thread 执行，不建立跨线程容错合同；
- flow creation、lane creation 在 network/persistence thread 启动前完成。

## 14. Migration mapping

### 14.1 DownloadEngine / TransferHandle

目标替换：

| Delete / replace | New owner |
| --- | --- |
| `using DataQueue` | private adapter in Packet Flow |
| `TransferHandle::data_queue` | delete |
| `TransferHandle::data_queue_producer` | delete |
| `TransferHandle::buffered_*` | Packet Flow lane draft |
| raw range id/offset packet construction | `LeaseId` + `ByteSpan` + checked `ByteOffset` |
| `TransferHandle::paused_by_memory` | Packet Flow lane bit |
| `TransferHandle::queue_pause_active` | Packet Flow lane bit |
| `reset_transfer_buffer()` | `producer.discard()` or successful `flush()` |
| `append_to_transfer_buffer()` | `producer.accept()` |
| `flush_transfer_buffer()` | `producer.flush()` |
| `enqueue_control_packet()` | `producer.publish()` |
| `apply_memory_backpressure()` | build observations + `producer.reconcile()` |
| memory/queue part of `resume_paused_transfers()` | `producer.reconcile()` actions |
| `global_memory_accounting()` reads | `producer.snapshot()` |
| `SessionState::queued_packets` reads | `producer.snapshot()` |

成功 Data publish 后，Packet Flow 在同一线性化点 checked 推进
`published_data_bytes`，再用同一个 bytes 值调用 Telemetry Session 的
`record_download_delta()`。engine 不再用 `PacketAdmission::published_bytes` 维护第二个
`SessionState::downloaded_bytes`，也不得重复记录 Telemetry。

`record_first_byte_received()` 仍在 HTTP callback 于
`PacketAdmissionCode::accepted && consumed_bytes > 0` 时调用；它不能推迟到 draft publish，
否则 `time_to_first_byte_ms` 会随 aggregation 延迟而改变。

Gap/window pause compatibility fields暂留到阶段 3/5；本阶段不能机械删除其它 reason。

### 14.2 PersistenceThread

constructor 从 concrete queue 改为：

```cpp
PersistenceThread(
    core::SessionState& session,
    flow::PacketConsumer& packet_consumer,
    core::AtomicBlockBitmap& bitmap,
    storage::FileWriter& file_writer,
    metadata::MetadataStore& metadata_store,
    BS::thread_pool<>& workers);
```

处理规则：

- `receive(packet)`：分派 Data/Control；
- `timeout`：保持当前 poll flush；
- `closed`：发起 final flush 并退出；
- `failed`：保留 error 并退出；
- receive 返回 packet 时 queue credit 已归还；
- direct append 后 `lease.complete()`；
- out-of-order 时先 `lease.account_reorder_node()`，再把整个 lease move 到 map；
- drain 时 move lease、append、complete；
- append/write error 先 complete current lease，再 `consumer.fail(error)`；
- `stop()` 和 shutdown packet 删除；
- destructor 不再从第二个 producer stream 写 queue。

阶段 3 前允许 `RangeContext::out_of_order_queue` 临时改存 `PacketLease`。这是明确的迁移
adapter，不是最终 Range interface；阶段 3 必须把它吸收进 Range Lifecycle。

### 14.3 SessionState and MemoryAccounting

删除：

- `SessionState::queued_packets`；
- `SessionState::downloaded_bytes`（在 progress caller 迁移到
  `recovery_initial_trusted_bytes + PacketFlowSnapshot::published_data_bytes` 后）；
- `RangeContext::pause_for_memory`；
- `core::MemoryAccounting`；
- `global_memory_accounting()`；
- `global_packet_overhead()`；
- `should_pause_for_backpressure()`。

对应算法移动到 Packet Flow private implementation。`memory_accounting_test.cpp` 的行为测试
迁移到 Packet Flow interface 后删除旧文件，不能两层重复保留。

## 15. Tiny-commit migration

每个 slice 必须独立 build、test、commit，并在没有 dependent slice 时可直接 rollback；
已有 dependent 时按逆序回滚。`S` 不改变失败语义，`C` 必须有独立 expected-red。

| Slice | Class | Commit intent | Change | Verification | Slice rollback |
| ---: | --- | --- | --- | --- | --- |
| 1 | test | `test: characterize packet flow ownership and ordering` | 锁定当前 FIFO、pause、accounting、curl replay 与 cross-producer shutdown 风险 | 定向旧行为 tests；风险 test 记录为不依赖的旧假设 | 只移除新 tests |
| 2 | S | `refactor: add packet flow values, module, and queue seam` | 加 dependency-neutral range values、final interface、唯一 production adapter、test-only fault seam；尚无 caller | create/close、value-copy、adapter contract tests | 删除新 module/value files |
| 3 | S | `refactor: centralize packet draft and lease ownership` | 实现 lane、inline envelope/lease、move/discard；用 compile-time legacy admission/accounting/failure bridge保持语义 | ownership、allocation-shape、existing failure tests | 回滚 module implementation，不动 callers |
| 4 | S | `refactor: migrate network packet production` | TransferHandle 改持 lane；callback 走 producer；保留 persistence compatibility consumer | callback replay、64 KiB shape、network tests | 恢复旧 TransferHandle helpers |
| 5 | S | `refactor: migrate persistence packet consumption` | constructor 接 PacketConsumer，reorder map 存 lease；compatibility ledger仍释放旧global counter | persistence ordering/ownership tests | 恢复旧 consumer adapter；producer module可保留 |
| 6 | C | `fix: map packet construction and allocation failures` | checked bounds/arithmetic、Data/Control/backend allocation failure从 terminate/silent ambiguity 变为 first error；不改 budget | 每个 fault point red→green、无 partial mutation、artifacts retained | 只回滚 failure mapping；保留 ownership seam |
| 7 | C | `fix: enforce logical hard packet budget` | 用 checked logical credit替换“constructor capacity/try_enqueue false即业务容量”的旧语义 | budget 1/2/33 red→green、Control bypass、credit rollback、concurrency | 只回滚 hard-budget activation；保留 vendor 事实 |
| 8 | C | `fix: isolate packet accounting per request` | 把 global singleton 切换为 PacketFlow-local checked ledger，不再 task-start reset | concurrent/sequential isolation red→green、underflow、abort drain | 只回滚 local-ledger activation；不回滚 lease ownership |
| 9 | C | `fix: serialize completion/close and drain admitted drafts on stop` | Range Complete/close 同 token；删除 cross-producer shutdown；移除 stop_requested 对 open-flow temporary flush failure 的 early-abort；仅上游 terminal 才 discard | ordering、close wake、queue-full stop draft red→green、allocation failure、failure drain tests | 整体恢复旧 stop path；不得只回滚 drain fix而保留相互矛盾的 close 路径 |
| 10 | S | `refactor: centralize queue and memory pause state` | reconcile 接管 Queue/Memory bits、Top 20% 和 low resume；engine仅组合其它 reasons | deterministic state-machine tests + CURL integration | 恢复 pause helpers；Packet Flow queue仍可保留 |
| 11 | S | `refactor: remove packet flow compatibility structures` | 删除 legacy bridges、vendor includes、Session counters、MemoryAccounting、old tests/helpers | `rg` deletion checks + full suite | 恢复该 slice 删除项；不回滚已验证 C fixes |
| 12 | perf | `perf: verify packet flow behavior neutrality` | 无生产调参；分别归因 S slices 与四个 C slices，保存 pre/post Release benchmark 和 profiler explanation | 第 17 节 gate | 回滚明确触发 gate 的最小 slice |
| 13 | docs | `docs: record packet flow migration evidence` | 记录 actual commit、tests、benchmark、rollback ids、known 740、C 类 before/after | exit checklist | 仅回滚 evidence doc |

Slice 3 至 5 的 compatibility bridge 必须是 compile-time、短生命周期迁移结构，并在 Slice 11
删除。不得把 old/new runtime path 做成长期 feature flag。bridge 只允许保持旧 admission 与
global accounting 行为；不能泄漏进 final header。

Slice 6 至 Slice 9 是四个独立的 **C 类 correctness/concurrency change**，不得与
`refactor:` commit 合并、squash 或藏进 deletion。每个 C 类 evidence package 必须单列：

```text
Behavior before:
Red/characterization test:
Fix commit:
Behavior after:
Targeted Debug/Release result:
Benchmark pre artifact:
Benchmark post artifact:
Rollback commit:
Failure/concurrency behavior diff:
```

Slice 9 的 `Behavior before` 必须记录
`stop_requested && queued_packets >= capacity` early return 和
`stop_network_phase()` 先置 stop 的具体 branch；red/after evidence 必须证明 open Flow 下
draft 被发布而不是映射为 HTTP failure，同时 upstream terminal 仍能有界退出。

逻辑 hard budget 若导致第 17 节 keeper gate 失败：

1. 保留 wrapper、ordering 与 ownership tests；
2. 只回滚 Slice 7 hard-budget activation；
3. 把实际 pre/post evidence带回 Wayfinder；
4. 不通过增大默认 budget 或放宽 resume 隐藏回归；
5. 阶段保持未完成，直到 hard-budget contract有新的明确决策。

request-local accounting 若出现 regression 或无法证明并发隔离：

1. 只回滚 Slice 8 local-ledger activation；
2. 保留 PacketLease ownership、hard-budget 与 ordering slices；
3. 保存两个同时运行的 Download Request 的 red/green trace 与最终 counter；
4. 不以 task-start `global.reset()`、串行化 DownloadClient 或扩大 high watermark 作为修复；
5. 阶段保持未完成并回到 Wayfinder。

## 16. Exact test plan

### 16.1 Packet Flow interface tests

新增 `tests/flow/packet_flow_test.cpp`，至少包含：

- `PacketFlowTest.CreatesOneProducerAndOneConsumerFromValidatedPolicy`
- `PacketFlowTest.RejectsWrongThreadColdMutationWithoutSideEffect`
- `PacketFlowTest.AcceptsContiguousChunksIntoOneLaneDraft`
- `PacketFlowTest.PreservesLeaseIdentityAndHalfOpenSpanByValue`
- `PacketFlowTest.RejectsChunkOutsideLeaseSpanWithoutMutation`
- `PacketFlowTest.RejectsOffsetSizeOverflowWithoutMutation`
- `PacketFlowTest.RejectsChunkLargerThanAggregationTargetWithoutMutation`
- `PacketFlowTest.RejectsNonContiguousChunkWithoutMutatingDraft`
- `PacketFlowTest.PublishesAtCurrentSixtyFourKibAggregationShape`
- `PacketFlowTest.FlushesFinalShortDraft`
- `PacketFlowTest.RetainsDraftWhenLogicalPacketBudgetIsExhausted`
- `PacketFlowTest.DistinguishesBackendNoAllocationFailureFromLogicalBudget`
- `PacketFlowTest.ReturnsIncomingBytesUnconsumedOnQueuePause`
- `PacketFlowTest.ReturnsIncomingBytesUnconsumedOnMemoryPause`
- `PacketFlowTest.AllowsFirstPacketWhenItAloneExceedsHighWatermark`
- `PacketFlowTest.PausesProjectedSecondPacketAboveHighWatermark`
- `PacketFlowTest.PublishesRangeCompleteAfterAllRangeData`
- `PacketFlowTest.PreservesCompletionIdentityAndExpectedEndByValue`
- `PacketFlowTest.RejectsNonPositiveCompletionExpectedEnd`
- `PacketFlowTest.RejectsRangeCompleteWhileRangeDraftRemains`
- `PacketFlowTest.ControlBypassesDataAdmissionBudgetWithoutBeingDropped`
- `PacketFlowTest.ConsumerObservesStrictlyIncreasingSequence`
- `PacketFlowTest.CloseMarkerFollowsAllRegularPackets`
- `PacketFlowTest.CloseWakesTimedConsumer`
- `PacketFlowTest.RejectsPublicationAfterClose`
- `PacketFlowTest.ControlAllocationFailureBecomesTerminalFailure`

### 16.2 Accounting tests

- `PacketFlowTest.AccountsDraftEnvelopeAndPayloadOnce`
- `PacketFlowTest.PublishTransfersAccountingWithoutIncrement`
- `PacketFlowTest.ReceiveReturnsQueueCreditButRetainsAccountedBytes`
- `PacketFlowTest.ReorderNodeAddsExactlyFortyEightBytesOnce`
- `PacketFlowTest.LeaseCompletionReleasesAllAccountedBytes`
- `PacketFlowTest.MovedLeaseHasExactlyOneOwner`
- `PacketFlowTest.DiscardReleasesLocalDraft`
- `PacketFlowTest.ConsumerFailureDrainsQueuedAccounting`
- `PacketFlowTest.DetectsAccountingUnderflowAsTerminalFailure`
- `PacketFlowTest.ConcurrentDownloadInstancesDoNotShareAccounting`

每个 test结束必须断言：

```text
snapshot.queued_packets == 0
snapshot.accounted_bytes == 0
```

不要在 test 中调用 private counter add/subtract。

### 16.3 Pause state-machine tests

- `PacketFlowTest.QueuePauseCountsOnlyOnEpisodeEntry`
- `PacketFlowTest.MemoryPauseCountsOnlyOnEpisodeEntry`
- `PacketFlowTest.QueuePauseWaitsForPacketCreditAndLowWatermark`
- `PacketFlowTest.MemoryPauseWaitsForLowWatermark`
- `PacketFlowTest.OverlappingQueueAndMemoryPauseResumesOnlyAfterBothClear`
- `PacketFlowTest.SelectsFastestTwentyPercentWithMinimumOne`
- `PacketFlowTest.UsesLaneIdAsDeterministicRateTieBreaker`
- `PacketFlowTest.DoesNotResumeWhenOnlyQueueCreditIsAvailableAboveLow`
- `PacketFlowTest.ReturnsResumeCandidateWithoutCallingCurl`

Telemetry assertion继续只看当前正式：

- `total_pause_count`;
- `queue_full_pause_count`;
- `packets_enqueued_total`;
- average/max packet size；
- max memory / inflight。

不要重新引入历史细分 summary keys。

### 16.4 Fault tests

private fault adapter提供 deterministic knobs：

- next Data `try_enqueue` returns false；
- next Control allocating enqueue returns false；
- next close marker enqueue returns false；
- queue constructor allocation failure；
- envelope allocation failure；
- timed receive timeout；
- consumer fail while one producer operation active。

测试必须分别证明：

- Data backend failure 保留 draft、credit 回滚、不是 terminal；
- Control/close failure 是 terminal；
- late producer operation不会落在 abort drain之后；
- first error不被后续 symptom覆盖；
- fail path不死锁，lease/accounting归零。

### 16.5 Concurrency tests

使用 deterministic barrier/latch，不用裸 `sleep` 判断正确性：

- 一个 producer 发布至少 100,000 个带 sequence 的小 packet，一个 consumer并发 receive；
- budget使用 `1`、`2`、`33`，覆盖一个 block上下；
- consumer随机但固定 seed地把部分 lease暂存 reorder map；
- 每个 sequence恰好观察一次；
- payload checksum与 offset连续；
- close只在 producer发布完成后发生；
- 运行至少 100轮 small close/fail race；
- 最终 queue/accounted归零；
- 没有 deadlock或 timeout扩散。

若 CI 有 ThreadSanitizer，Linux/Clang job SHOULD 跑该 test；Windows主 gate仍使用项目现有
MSVC配置。

### 16.6 libcurl integration

扩展本地 server fixture：

- `max_connections = 2`；
- 很小的 packet budget；
- high/low足以分别触发 Queue和Memory Pause；
- server固定 chunk size与delay；
- 记录每个返回给 callback的逻辑区间。

新增：

- `DownloadIntegrationTest.ReplaysPausedWriteChunkExactlyOnce`
- `DownloadIntegrationTest.CompletesWithLogicalPacketBudgetOne`
- `DownloadIntegrationTest.DrainsAdmittedPacketsOnNetworkFailure`
- `DownloadIntegrationTest.FlushesQueueFullDraftAfterStopRequestedWhilePersistenceActive`
- `DownloadIntegrationTest.StopsProducerAfterPersistenceFailure`

断言：

- output bytes与 source完全一致；
- 没有重复或缺失区间；
- callback pause返回前 incoming没有推进 offset；
- unpause由 orchestrator thread调用；
- Range Complete晚于该 Range全部 data；
- failure保留恢复 artifacts；
- stop flag置位后、Flow open且Persistence active时，既有draft仍恰好发布/持久化一次；
- Persistence terminal时不会因1 ms compatibility loop永久等待；
- 正式 summary key集合不变。

### 16.7 Replace, do not layer

新 interface tests绿后删除或改写：

- `tests/core/memory_accounting_test.cpp`；
- `persistence_thread_test.cpp` 中 `enqueue_data_packet()` 的手工 accounting；
- 直接构造 `BlockingConcurrentQueue<DataPacket>` 的 persistence tests；
- 只断言 `SessionState::queued_packets` 字段存在的 tests；
- shutdown packet kind结构 tests。

保留 persistence行为 tests，但让其通过 `PacketFlow::consumer()` seam投递与观察。测试不得
穿透 Packet Flow去读 vendor queue。

## 17. Performance neutrality gate

### 17.1 Fixed behavior

本阶段不是主动性能优化。MUST 保持：

- 64 KiB aggregation target；
- callback copy/move形状；
- no new per-packet pimpl、shared-state或runtime-adapter allocation；
- `try_enqueue` data hot path；
- one explicit network producer；
- single persistence consumer；
- Top 20% high-watermark selection；
- Queue/Memory low-watermark resume比较；
- 1 ms final-draft compatibility wait；
- window、connection、flush、CRC、metadata和recovery语义；
- 正式 6 main + 4 auxiliary summary keys。

逻辑 hard budget是语义修正，不是调参。不得用更大 budget抵消结果。

### 17.2 Benchmark

在实际阶段 base commit 上先生成 pre，再用同一机器、对象、server、case、环境生成 post：

```powershell
scripts\build.bat release

python scripts\performance\benchmark.py `
  --url "http://127.0.0.1:4287/1gb_files.zip" `
  --benchmark-suite regression_v2 `
  --case-list baseline_default,balanced_candidate,memory_guard,scheduler_stress `
  --repeats 20 `
  --label "phase-2-pre"

python scripts\performance\benchmark.py `
  --url "http://127.0.0.1:4287/1gb_files.zip" `
  --benchmark-suite regression_v2 `
  --case-list baseline_default,balanced_candidate,memory_guard,scheduler_stress `
  --repeats 20 `
  --label "phase-2-post"
```

另外各跑 20 次风险探针：

- `deep_buffer_candidate`;
- `queue_backpressure_stress`;
- `gap_tolerance_probe`。

结果接近 gate或出现双峰时提升到 40 repeats。

阻断 gate：

| Signal | Gate |
| --- | --- |
| `baseline_default` median network/disk throughput | 任一下降超过 5% 阻断 |
| `balanced_candidate` median network/disk throughput | 任一下降超过 5% 阻断 |
| `time_to_first_byte_ms` | 系统性恶化需解释，不能用吞吐豁免 |
| `memory_guard` max memory | 保持实际 pre的低内存形态；无收益时不得显著上升 |
| `max_inflight_bytes` | 多 case显著上升且无收益阻断 |
| `total_pause_count` | 多 case约 15% 以上物质回归需调查 |
| `queue_full_pause_count` | 原因口径保持 episode count，不能按 admission尝试重复累加 |
| packet shape | average/max保持约 64 KiB；total与文件/aggregation相符 |
| schema | 10 keys无增删改名 |

历史 `2026-03-11` 数字只作背景；keeper判断以当前 commit的同机 pre/post为准。

### 17.3 Profiler role

Packet Flow触及 callback、queue与 persistence hot path，因此 benchmark完成后单独执行：

```powershell
python scripts\performance\profiler.py `
  --url "http://127.0.0.1:4287/1gb_files.zip" `
  --benchmark-suite regression `
  --case-list throughput_candidate,scheduler_stress `
  --label "phase-2-profile"
```

profiler只回答：

- vendor adapter/wrapper是否成为新热点；
- packet allocation/copy栈是否意外增加；
- persistence consume链是否改变形状；
- queue pause仍由哪个 admission结果触发。

profiler不能：

- 用绝对 MB/s替代 benchmark；
- 豁免 keeper gate失败；
- 因 wrapper出现就绕过 Packet Flow interface；
- 推动本阶段顺便做新的优化实验。

## 18. Historical experiments explicitly prohibited

本阶段 MUST NOT 重新实施或混入：

- 把 aggregation从 64 KiB提高到 128 KiB；
- 仅加深 queue或改变默认 `queue_capacity_packets`；
- 把 Queue Pause提前到 high watermark恢复；
- 给 Queue Pause增加 drain hysteresis；
- 每轮只恢复一个 handle；
- 把 final flush 1 ms wait改为 yield/backoff；
- 取消 fresh connection或改变物理连接意图；
- 暗中把 window提高到 8 MiB；
- Persistence finished bitmap增量扫描；
- out-of-order staging batching；
- 放宽 flush cadence；
- queue backlog高时延迟 threshold flush；
- incremental/write-time CRC cache；
- 拆 FileWriter write/flush/read handles；
- compact metadata JSON；
- 连接复用、HTTP/2或自动 retry；
- 把历史 queue/backpressure细分诊断字段重新加入正式 summary。

内部 `accounted_bytes` 是 correctness ledger，不等于重新公开 `queued_bytes` diagnostics。若后续
确实需要 byte-budget诊断导出，必须另开性能线程，给出完整用途、采集口径、schema、tests和
benchmark方案。

## 19. Static and deletion checks

阶段完成时：

```powershell
rg -n "BlockingConcurrentQueue|producer_token_t|consumer_token_t" `
  src tests -g "*.cpp" -g "*.hpp"

rg -n "global_memory_accounting|global_packet_overhead|should_pause_for_backpressure" `
  src tests

rg -n "queued_packets|pause_for_memory|queue_pause_active|buffered_accounted_bytes" `
  src tests

rg -n "PacketKind::shutdown|enqueue_control_packet|flush_transfer_buffer\\(" `
  src tests
```

期望：

- vendor queue和token只在 `src/flow/packet_queue_adapter.hpp` / implementation-private test
  adapter出现；
- global memory symbols为 0；
- Session/Range/TransferHandle不再拥有 Packet Flow fields；
- shutdown packet kind为 0；
- old helpers为 0；
- `queued_packets` 只可以作为 public progress字段名和 Packet Flow snapshot成员出现；
- performance docs里的历史文字不在删除范围。

额外 dependency check：

```powershell
rg -n "flow/packet_flow" include
rg -n "concurrentqueue" src/download src/persistence src/core tests
```

第一条应为 0；第二条只允许 adapter contract test明确引用 vendored source行为，不能是
production caller。

## 20. Rollback strategy

### 20.1 Per-slice rollback

使用第 15 节 commit为最小回滚单位：

- interface/module未接 caller时可直接回滚；
- producer迁移失败只回滚 network adapter；
- consumer迁移失败只回滚 persistence adapter；
- Slice 9 的 close ordering 与 stop-time admitted-draft drain 作为一个 C 单元回滚，不与其它
  性能 slice 混合，也不能拆开只留一半；
- pause迁移失败恢复旧 helpers，但保留 Packet Flow queue/ownership；
- hard budget benchmark失败只回滚 activation，不调整 defaults；
- 每次回滚后重新断言 accounting/queue为 0和旧 tests集合未扩大。

### 20.2 Whole-stage rollback

反向顺序：

1. 恢复 progress/global accounting reads；
2. 恢复 Queue/Memory Pause helpers；
3. 恢复 PersistenceThread concrete queue consumer；
4. 恢复 TransferHandle draft/queue pointers；
5. 恢复旧 shutdown；
6. 移除 Packet Flow module；
7. 只保留旧实现能通过的 characterization；仅新 correctness 才能通过的 tests 随对应 C
   commit 回滚或显式禁用，风险用例合同与 red 输出保留在 evidence。

本阶段没有 public header或metadata格式迁移，不需要数据迁移脚本。若实现出现
`.config.json` diff，说明 scope泄漏，应回滚而不是补迁移。整阶段回滚后的 baseline 不得
保持 red。

### 20.3 Runtime dual path prohibited

不得增加：

- `use_new_packet_flow`;
- old/new queue feature flag；
- 两份 accounting counter交叉校验长期保留；
- fallback到 implicit producer；
- control queue fallback。

回滚依赖小 commit，不依赖运行时双轨。

## 21. Risks and stop conditions

Code Agent遇到以下情况必须停止当前 slice并回到 Wayfinder，不得自行扩大 scope：

- hard packet budget无法在不改变 public default的情况下通过 keeper benchmark；
- Control ordering需要第二个 producer或第二条 queue；
- persistence必须跨 thread持有 PacketLease；
- Range Complete必须在 Data Packet前被观察才能维持现有实现；
- Accounted Bytes无法在所有 path归零；
- vendor queue必须改 source才能提供所需语义；
- libcurl replay test出现同一 bytes重复或缺失；
- 为完成阶段必须改变 recovery格式、flush cadence、window或connection语义；
- 正式 summary需要新增/删除 key才能解释 correctness；
- producer token不能限定到orchestrator thread；
- current base已不再使用单 producer/single persistence writer模型。

## 22. Code Agent exit checklist

- [ ] 记录实际 base commit和已有工作树改动。
- [ ] 阶段 0 Packet Flow characterization已完成。
- [ ] 阶段 1 `FlowControlPolicy`已实现且不可变。
- [ ] Packet Flow external seam不暴露vendor类型。
- [ ] 只存在一个 explicit producer token且无implicit enqueue。
- [ ] Data admission hard budget与vendor failure结果分离。
- [ ] 64 KiB aggregation与callback copy形状保持。
- [ ] Queue/Memory Pause各自只在episode entry计数。
- [ ] low-watermark resume现有语义保持。
- [ ] libcurl pause返回时incoming bytes未消费。
- [ ] unpause前lane state已清除且只由orchestrator thread调用。
- [ ] Data/Control/close sequence严格递增。
- [ ] Range Complete晚于该Range全部Data Packet。
- [ ] close marker晚于全部regular packets。
- [ ] persistence不再enqueue shutdown。
- [ ] queue credit在receive时归还。
- [ ] Accounted Bytes在lease complete/discard时恰好归还一次。
- [ ] reorder overhead只增加一次且固定48 bytes。
- [ ] concurrent Download Requests不共享accounting。
- [ ] failure mapping、logical hard budget、request-local accounting，以及
  completion/close + stop-time admitted-draft drain 分别以 C 类 `fix:` commit 交付。
- [ ] 四个 C 类 slice 各自保存 before/red/after/Release/rollback evidence。
- [ ] normal close、network failure、persistence failure和allocation failure均无deadlock。
- [ ] flow open + persistence active 时，stop_requested 不会把 queue-full draft 转成 terminal；
      draft 以既有 1 ms 语义发布后才 close。
- [ ] old queue/global accounting fields和tests已删除。
- [ ] Debug与Release full tests通过；既有Windows 740失败集合未扩大。
- [ ] libcurl replay与failure integration通过。
- [ ] Release pre/post benchmark gate通过；风险探针已运行。
- [ ] profiler只用于解释hot-path变化。
- [ ] 正式10项summary key和packet metric口径未变。
- [ ] 没有混入第18节历史reject实验。
- [ ] 每个slice有独立commit、verification evidence和rollback id。

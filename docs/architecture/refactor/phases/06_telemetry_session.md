# 阶段 6：Telemetry Session

## 1. Outcome

本阶段把当前位于 `TelemetryCollector` 中的任务级聚合状态、时间规则和正式
Performance Summary 计算收进唯一的 deep production module：
`asyncdownload::telemetry::TelemetrySession`。

阶段完成后：

- production producer 只依赖 `TelemetrySession`；
- `TelemetrySession` 是任务级遥测状态和计算规则的唯一所有者；
- `TelemetryCollector` 只作为公开源码兼容 shell 存在，不再被描述为独立架构 module；
- `TelemetrySession::final_summary()` 仍是正式 10 项 Performance Summary 的唯一实现路径；
- `ProgressSnapshot` 的业务字段由 orchestrator 从各权威 module 显式合并，Telemetry 不成为
  下载状态、恢复状态或背压状态的第二真相源；
- `steady_clock`、现有计算公式、pause episode 口径和成功/失败结束语义保持不变；
- 不重新引入 `TelemetrySink`、后台消费线程、事件总线、诊断文件或已退役字段；
- `TelemetrySession` 与 `TelemetryCollector` 的现有公开调用方式保持源码兼容；
- 结构迁移、producer 接线、算术加固和测试清理分别提交，可以独立验证与回滚。

本阶段依赖：

1. [阶段 0：特征化与兼容基线](00_characterization_baseline.md)；
2. [阶段 1：Validated Download Policy](01_validated_download_policy.md)；
3. [阶段 2：Packet Flow / Backpressure](02_packet_flow_backpressure.md)；
4. [阶段 3：Range Lifecycle](03_range_lifecycle.md)；
5. [阶段 4：Recovery Checkpoint](04_recovery_checkpoint.md)；
6. [阶段 5：HTTP Transfer](05_http_transfer.md)。

Code Agent MUST 按阶段顺序实施。Phase 6 文档中出现的 `PacketFlowSnapshot`、
`RangeLifecycle`、Recovery committed VDL 和 HTTP transfer snapshot 均指前序阶段最终
interface，不得为了让本文档适配旧结构而恢复 `SessionState` 的遗留字段。

## 2. 已核实的当前事实

本节描述当前 `main` 源码和已提交历史，而不是目标设计假设。

### 2.1 当前公开 interface

以下文件都位于公开 include tree：

- `include/asyncdownload/telemetry.hpp`；
- `include/asyncdownload/telemetry/telemetry_event.hpp`；
- `include/asyncdownload/telemetry/telemetry_session.hpp`；
- `include/asyncdownload/telemetry/telemetry_collector.hpp`；
- `include/asyncdownload/performance_metrics.hpp`；
- `include/asyncdownload/types.hpp`。

当前 `TelemetrySession`：

- 默认构造；
- 不可复制；
- 暴露 7 个 `record_*` 方法；
- 暴露 `current_snapshot()` 与 `final_summary()`；
- private 只保存一个 `TelemetryCollector collector_`；
- `src/telemetry/telemetry_session.cpp` 的 9 个方法全部是单行委托。

当前 `TelemetryCollector`：

- 公开暴露与 Session 高度重叠的 7 个 `record_*` 和 2 个 query；
- 独占 `std::mutex`、任务时间、EMA、累计字节、峰值、pause 和 packet 统计；
- 每个 public record/query 都取得同一把 mutex；
- 没有 worker thread、queue、sink 或异步 drain；
- 在 `current_snapshot()` 中即时构造 snapshot，没有第二份 `snapshot_` 缓存。

因此当前依赖方向是：

```text
production callers
        │
        ▼
TelemetrySession
        │  nine pass-through calls
        ▼
TelemetryCollector
        │
        └── owns all aggregation state
```

`TelemetrySession` 名义上是 facade，实际是 shallow module。删除它只需要把 production
调用点机械改成 `TelemetryCollector`，不会隐藏任何复杂性；当前形状没有通过 deletion test。

### 2.2 当前计算规则

`src/telemetry/telemetry_collector.cpp` 当前固定了这些规则：

- EMA 权重为 previous `0.8`、current `0.2`；
- 第一个 delta 只建立 sample timestamp，不产生非零瞬时速度；
- 后续瞬时速度为 `bytes * 1,000,000,000 / delta_ns`；
- 非正时间差产生 `0.0` 瞬时速度；
- inflight 为 `max(0, downloaded - persisted)`；
- `record_download_delta()` 每调用一次就增加 packet count；
- average packet size 为累计 packet bytes 除以 packet count；
- task average network/disk speed 的分母都是 task start 到 task end；
- incomplete task 的 end 是 `max(latest_event_at, final_summary(now) 的 now)`；
- completed task 的 end 固定为 `max(completed timestamp, latest_event_at)`；
- TTFB 是最早 first-byte timestamp 与 task start 的毫秒差；
- first byte 早于 task start、未记录 first byte 或尚未 start 时，TTFB 为 `0`；
- start 前和 completion 后的非 start 更新被忽略；
- 再次 `record_task_started()` 会完整重置 collector 并开始新一轮 task；
- completion 只接受第一次，后续 completion 被忽略。

这些规则是正式 summary 的现有行为。结构迁移必须先通过特征化测试逐项复制，不能在移动
所有权时顺便换成滑动窗口、wall clock、不同 EMA 权重或不同 task duration。

### 2.3 当前 production producer

当前调用关系如下：

| Producer | 当前调用 | 已核实触发点 |
| --- | --- | --- |
| `DownloadEngine::run()` | `record_task_started(run_started)` | probe 成功并构造 Session 后 |
| HTTP write callback helper | `record_first_byte_received()` | callback 接受非零有效 body bytes 后 |
| packet enqueue path | `record_download_delta(packet_size)` | Data Packet enqueue 成功后 |
| packet/accounting path | `record_memory_sample(current)` | accounted memory 增长后 |
| queue pause edge | `record_pause(queue_full, true)` | queue episode `false → true` |
| memory pause edge | `record_pause(memory_pressure, false)` | memory episode `false → true` |
| gap pause edge | `record_pause(gap, false)` | gap episode `false → true` |
| window pause edge | `record_pause(none, false)` | window-boundary episode `false → true` |
| `PersistenceThread` | `record_persist_delta(bytes)` | aligned write 或 final tail 成功后 |
| success finalize | `record_task_completed(now)` | output 成功完成后 |
| result assembly | `final_summary(now)` | 成功或失败返回前 |

当前失败路径不调用 `record_task_completed()`。失败任务的 summary 按 incomplete task 使用
传入的 `now`。正式 summary 没有 status key；成功/失败仍由 `DownloadResult::error`、CLI
exit code 和 stderr 表达。

表中的 window-boundary pause 是 Phase 5 前的当前事实，不是最终 producer 合同。Phase 5
把 normal exact-window 完成改为正常 session 进展，并把额外 body 映射为 terminal
`body_too_long`；目标结构不再为 window overflow 记录 pause。

### 2.4 当前 ProgressSnapshot merge

当前 `invoke_progress()`：

1. 先调用 `TelemetrySession::current_snapshot()`；
2. 再用 `SessionState` 原子计数覆盖 downloaded、persisted 和 inflight；
3. 用 session/global/range/handle 状态覆盖 total、VDL、queue、memory、pause 和 active；
4. 保留 Telemetry 产生的 network/disk EMA；
5. 在 Telemetry mutex 已释放后调用用户 callback。

这证明 `TelemetrySession::current_snapshot()` 中的累计 downloaded、persisted、inflight 和
memory 目前只是兼容投影；production progress 已经把业务事实覆盖为其它来源。Phase 2–5
进一步把 queue/memory、Range、VDL 和 HTTP request 事实收回各自 module 后，Phase 6 必须把
这种“先取得一个几乎完整 snapshot 再覆盖”的隐式 merge 改成显式字段合并。

### 2.5 规划与源码漂移

`.planning/phases/01-telemetry-skeleton/01-CONTEXT.md` 中的决策并非都仍是现行合同：

| 历史决策 | 当前状态 | Phase 6 处理 |
| --- | --- | --- |
| D-01/D-02：公开 header 与 `src/telemetry` | 仍成立 | 保持 |
| D-03：`telemetry.hpp` 聚合 header | 仍成立 | 保持 include 可用 |
| D-04：Event/Sink/Collector/Session 四模块 | 已漂移 | 不恢复 Sink；Collector 降为 shell |
| D-05：Session 是 primary facade | 仍成立但实现过浅 | 深化为状态所有者 |
| D-06：7 个 record 方法 | 基本成立 | 保留现有签名 |
| D-07：snapshot/summary query | 实际类型是 `ProgressSnapshot` | 保留当前源码类型 |
| D-08：SessionState 不保存 telemetry state | 文字有歧义 | SessionState 可拥有 Session，不复制内部字段 |
| D-09：Session 生命周期绑定 task | 仍成立 | 固定为 ownership contract |
| D-10～D-13：固定事件、tagged union、发射时间 | 已退役 | 不重建事件 transport |
| D-14：engine 创建并传给 producer | 仍成立 | 前序 deep modules 借用 Session |
| D-15：Progress/Performance 兼容 | 仍成立 | 作为 merge gate |

历史 `01-VERIFICATION.md` 曾验证 `TelemetrySink`、固定 `TelemetryEvent`、后台 collector 和
friend test access；当前源码已经没有这些类型与文件。历史 `01-UAT.md` 还停在 1/6 pass、
5/6 pending，也不能作为当前验收结果。

提交 `9687749` 明确完成了这些删除：

- 删除 `TelemetrySink`；
- 删除 tagged event/payload；
- 删除 collector worker thread；
- 改为 caller thread 上同步聚合；
- 为所有 collector record 方法提供显式 `steady_clock::time_point`；
- Session 直接委托 collector。

后续提交 `7a96af2` 又删除 collector 的 `ProgressSnapshot snapshot_` 缓存，改为 query 时从
累计值即时构造。由此可见事件 transport 和宽运行态缓存是有意退役的历史，不是 Phase 6
需要“补回”的缺失实现。

### 2.6 性能历史固定的边界

性能历史迭代 017、018、024 已经确认：

- 正式 keeper schema 是 6 个主指标加 4 个辅助指标；
- CLI summary、Python parser、benchmark/profiler 与测试必须同步；
- status、downloaded、persisted、resumed 不属于正式 summary；
- queue/backpressure 细分诊断、pause duration、CRC、写形态和资源字段已退出正式链；
- `TelemetrySession::final_summary()` 是正式 summary 的唯一计算路径；
- `SessionState` 的旧速度/base 缓存和 collector 的 snapshot cache 已删除；
- 半退役的 queued bytes 与边缘 progress 字段不能借 Phase 6 重新进入。

Phase 6 是结构重构，不是指标扩张或性能实验。不得借“深模块”之名恢复旧字段。

## 3. Compatibility contract

### 3.1 MUST 保持的公开类型

- `asyncdownload::telemetry::TelemetryClock` 仍是 `std::chrono::steady_clock`；
- `TelemetryPauseReason` 仍以 `std::uint8_t` 为 underlying type；
- enum 值保持：
  - `none = 0`；
  - `queue_full = 1`；
  - `memory_pressure = 2`；
  - `gap = 3`；
- `none` 为公开兼容值，但 Phase 5 后 production 不再用它表示 window overflow；
- `telemetry_timestamp_ns(time_point)` 的名称、参数、返回类型和 `noexcept` 保持；
- `TelemetrySession` 与 `TelemetryCollector` 的默认构造和不可复制性质保持；
- 两个 class 的现有 public method 名称、参数类型、const、默认参数和 `noexcept` 保持；
- `ProgressSnapshot` 与 `PerformanceSummary` 的公开字段不在本阶段增删改名；
- `include/asyncdownload/telemetry.hpp` 继续使两种 class、clock 和 pause reason 可见；
- 直接 include `telemetry_session.hpp` 或 `telemetry_collector.hpp` 的现有代码继续编译。

### 3.2 必须保留的精确现有签名

以下签名是兼容基线，不能用同名但不同参数的函数替换：

```cpp
class TelemetrySession {
public:
    TelemetrySession() = default;

    TelemetrySession(const TelemetrySession&) = delete;
    TelemetrySession& operator=(const TelemetrySession&) = delete;

    void record_task_started(
        TelemetryClock::time_point timestamp = TelemetryClock::now()) noexcept;
    void record_first_byte_received(
        TelemetryClock::time_point timestamp = TelemetryClock::now()) noexcept;
    void record_download_delta(std::uint64_t bytes) noexcept;
    void record_persist_delta(std::uint64_t bytes) noexcept;
    void record_pause(TelemetryPauseReason reason, bool is_queue_full) noexcept;
    void record_memory_sample(std::uint64_t memory_bytes) noexcept;
    void record_task_completed(
        TelemetryClock::time_point timestamp = TelemetryClock::now()) noexcept;

    [[nodiscard]] ProgressSnapshot current_snapshot() const noexcept;
    [[nodiscard]] PerformanceSummary final_summary(
        TelemetryClock::time_point now = TelemetryClock::now()) const noexcept;
};
```

```cpp
class TelemetryCollector {
public:
    TelemetryCollector() = default;

    TelemetryCollector(const TelemetryCollector&) = delete;
    TelemetryCollector& operator=(const TelemetryCollector&) = delete;

    void record_task_started(
        TelemetryClock::time_point timestamp = TelemetryClock::now()) noexcept;
    void record_first_byte_received(
        TelemetryClock::time_point timestamp = TelemetryClock::now()) noexcept;
    void record_download_delta(
        std::uint64_t bytes,
        TelemetryClock::time_point timestamp = TelemetryClock::now()) noexcept;
    void record_persist_delta(
        std::uint64_t bytes,
        TelemetryClock::time_point timestamp = TelemetryClock::now()) noexcept;
    void record_pause(
        TelemetryPauseReason reason,
        bool is_queue_full,
        TelemetryClock::time_point timestamp = TelemetryClock::now()) noexcept;
    void record_memory_sample(
        std::uint64_t memory_bytes,
        TelemetryClock::time_point timestamp = TelemetryClock::now()) noexcept;
    void record_task_completed(
        TelemetryClock::time_point timestamp = TelemetryClock::now()) noexcept;

    [[nodiscard]] ProgressSnapshot current_snapshot() const noexcept;
    [[nodiscard]] PerformanceSummary final_summary(
        TelemetryClock::time_point now = TelemetryClock::now()) const noexcept;
};
```

### 3.3 Deterministic timestamp variants

Session 的 download、persist、pause 和 memory 方法当前不能注入时间，导致核心计算测试仍然
依赖直接测试 Collector。Phase 6 需要让测试通过 deep Session interface，但不能给现有方法
增加默认 timestamp 参数，也不能增加同名 overload。

选定新增四个名称唯一的 deterministic variant：

```cpp
void record_download_delta_at(
    std::uint64_t bytes,
    TelemetryClock::time_point timestamp) noexcept;
void record_persist_delta_at(
    std::uint64_t bytes,
    TelemetryClock::time_point timestamp) noexcept;
void record_pause_at(
    TelemetryPauseReason reason,
    bool is_queue_full,
    TelemetryClock::time_point timestamp) noexcept;
void record_memory_sample_at(
    std::uint64_t memory_bytes,
    TelemetryClock::time_point timestamp) noexcept;
```

现有无 timestamp 方法在入口捕获一次 `TelemetryClock::now()`，然后委托对应 `_at` 方法。
start、first byte、completed 和 summary 已经可以显式传入 timestamp，不新增重复名称。

不选择以下同名 overload：

```cpp
void record_download_delta(
    std::uint64_t bytes,
    TelemetryClock::time_point timestamp) noexcept;
```

虽然普通调用仍可编译，但 `auto member = &TelemetrySession::record_download_delta;` 会因 overload
set 变得不明确。名称唯一的 `_at` variant 保留了现有 pointer-to-member 源码。

### 3.4 Source、pointer-to-member 与 ABI matrix

| 使用方式 | Phase 6 保证 | 说明 |
| --- | --- | --- |
| `TelemetrySession session;` | 保持 | 默认构造仍可用 |
| 现有 9 个 method 普通调用 | 保持 | 原签名和符号保留 |
| Collector 现有 method 调用 | 保持 | shell 委托 Session |
| 显式 timestamp 的 Collector 调用 | 保持 | 默认参数与函数类型不变 |
| typed pointer-to-member | 保持 | 原方法未改签名、未形成同名 overload |
| `auto p = &TelemetrySession::record_download_delta` | 保持 | `_at` 使用不同名称 |
| `decltype(&TelemetryCollector::record_pause)` | 保持 | Collector 不增加同名 overload |
| 只 include 任一旧 class header | 保持 | 旧路径继续提供完整定义 |
| 依赖 private object layout 或 `sizeof` | 不保证 | private layout 不是源码合同 |
| 已编译 object 与新 static library 混链 | 不保证 | class layout 改变，必须全量重编译 |
| DLL stable ABI | 不在当前项目合同内 | 当前 CMake 构建 `STATIC` library |

Phase 6 必须提供 compile-only compatibility fixture，用 `static_cast` 固定现有 member function
类型。不能只依赖普通调用测试推断签名没有变化。

### 3.5 Header compatibility

当前 `telemetry_session.hpp` 直接 include collector header，因此只 include Session header 的
代码也能看到 `TelemetryCollector`。为了不制造隐蔽的 transitive-include break，选定新增一个
声明汇聚 header：

```text
include/asyncdownload/telemetry/telemetry_contract.hpp
```

它按顺序定义 `TelemetrySession` 和 `TelemetryCollector`。两个旧 header 都只 include 该
contract header；`telemetry.hpp` 继续 include 旧路径。这样：

- 不产生 Session ↔ Collector 循环 include；
- Collector 可以按值拥有完整 `TelemetrySession`；
- 两个旧 include 路径和当前 transitive 可见性都保留；
- `telemetry_contract.hpp` 只是公开声明的机械汇聚，不是第三个运行时 module；
- production 仍只依赖 `TelemetrySession`。

不得把 class 搬入 `src/` private header，因为现有公开 class 是 concrete type，调用方需要
完整大小才能栈构造。

## 4. Module responsibilities

### 4.1 Telemetry Session owns

`TelemetrySession` MUST 独占：

- task started/completed 状态；
- task start、completion 和 latest accepted event 时间；
- first valid byte 的最早时间；
- network/disk 上一次 sample 时间；
- network/disk EMA；
- 当前任务的 observed download/persist delta 累计；
- packet count、packet byte total 和 max packet size；
- latest/max observed Accounted Bytes；
- max observed inflight；
- total pause 与 queue-full pause episode count；
- `current_snapshot()` 的兼容 telemetry 投影；
- `final_summary()` 的全部 10 项计算；
- event acceptance、duplicate、time-order 和 overflow 规则；
- 保护上述状态的唯一 mutex。

删除 Telemetry Session 后，时间处理、EMA、峰值、packet 统计、summary schema 和并发保护会
重新散回 Packet Flow、HTTP Transfer、Persistence、engine 和 Collector shell。目标 module
通过 deletion test。

### 4.2 Telemetry Session does not own

Telemetry Session MUST NOT：

- 决定 Data Packet 是否接纳；
- 修改 packet budget、Accounted Bytes ledger 或 pause bits；
- 决定 Range phase、Lease、gap、steal 或 completion；
- 决定 HTTP response、body 边界、retry 或 curl pause；
- 决定 physical write、flush、bitmap、CRC、metadata 或 committed VDL；
- 决定 task 成功、失败或错误码；
- 调用 progress callback；
- 保存 URL、path、Download Policy 或 recovery identity；
- 建立 background worker、queue、event transport 或 diagnostic export；
- 新增正式 summary key；
- 保存已退役的 queued bytes、pause duration、CRC、write shape 或 resource counters；
- 向业务 module 返回可参与 correctness 决策的状态。

Telemetry 中的 downloaded/persisted/inflight 是 **observed metric accumulators**。它们只用于
速度、峰值和兼容 snapshot，不能被 scheduler、backpressure、recovery、final result 或
progress merge 当成权威业务事实。

### 4.3 TelemetryCollector compatibility shell

`TelemetryCollector` 的唯一职责是：

- 保存一个 private `TelemetrySession session_`；
- 保留全部现有 public signatures；
- 把 timestamp-aware record 调用委托给 Session 的已有显式 timestamp 方法或 `_at` variant；
- 把 query 委托给 Session；
- 允许已有外部源码继续独立构造 Collector。

Collector shell 不拥有：

- mutex；
- aggregate state；
- 计算 helper；
- 独立测试矩阵；
- production caller；
- “collector thread”或事件消费语义。

直接构造 Collector 会创建一个独立 Session。这只是兼容用法，不与 production task 的
Telemetry Session 共享状态。不得给 Collector 增加 `session()` accessor、shared pointer 或
全局 registry。

## 5. Interface designs considered

### 5.1 方案 A：保持 Session → Collector，拒绝

优点：

- 最小代码 diff；
- private layout 变化较少。

拒绝原因：

- Session 仍是 9 个 pass-through 方法；
- Collector 与 Session 继续暴露重复 interface；
- 核心行为测试仍然绕过 production interface；
- 历史命名继续误导维护者认为存在异步 collector；
- 删除 Session 不损失任何复杂性。

### 5.2 方案 B：Session state owner + Collector shell，选定

形状：

```text
Packet Flow ───────┐
HTTP Transfer ─────┼──► TelemetrySession ──► snapshot / final summary
Persistence ───────┘             ▲
                                 │ delegates only
external legacy caller ─► TelemetryCollector
```

优点：

- production interface 与 state owner 对齐；
- 现有 Collector 源码继续编译；
- 核心测试经过 Session；
- 没有 background handoff、allocation 或新 virtual seam；
- Collector shell 可在未来 major version 中有计划地移除，而不妨碍当前迁移。

### 5.3 方案 C：删除 Collector 或做 type alias，拒绝

删除 class 会直接破坏公开 header 用户。`using TelemetryCollector = TelemetrySession` 也不兼容：

- 两者现有 method signatures 不同；
- type identity、pointer-to-member 和 overload resolution 会改变；
- Collector 的 timestamp-aware 调用无法完整表达；
- 旧链接符号消失。

### 5.4 方案 D：恢复 Sink/Event/background collector，拒绝

该形状增加 queue、worker、drain、shutdown 和 eventual-consistency 规则，却没有当前 consumer
需要异步事件流。它已经由 `9687749` 删除，且性能历史要求缩小维护面。Phase 6 禁止恢复。

### 5.5 方案 E：heap pimpl，拒绝

pimpl 可以为未来 ABI 隐藏 layout，但当前 class 默认构造不能返回 allocation error，项目又要求
record/query `noexcept`。为每个 Session 引入 heap allocation、failure fallback 和另一层间接
访问不值得。当前交付的是 static library，Phase 6 明确要求全量重编译，不伪造 binary ABI
承诺。

## 6. Exact target C++ interface

`telemetry_contract.hpp` 中的目标声明如下。private helper 名称可以机械调整，但 public 部分、
状态所有权和计算规则 MUST 保持。

```cpp
#pragma once

#include "asyncdownload/telemetry/telemetry_event.hpp"
#include "asyncdownload/types.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>

namespace asyncdownload::telemetry {

class TelemetrySession {
public:
    TelemetrySession() = default;

    TelemetrySession(const TelemetrySession&) = delete;
    TelemetrySession& operator=(const TelemetrySession&) = delete;

    void record_task_started(
        TelemetryClock::time_point timestamp = TelemetryClock::now()) noexcept;
    void record_first_byte_received(
        TelemetryClock::time_point timestamp = TelemetryClock::now()) noexcept;
    void record_download_delta(std::uint64_t bytes) noexcept;
    void record_persist_delta(std::uint64_t bytes) noexcept;
    void record_pause(TelemetryPauseReason reason, bool is_queue_full) noexcept;
    void record_memory_sample(std::uint64_t memory_bytes) noexcept;
    void record_task_completed(
        TelemetryClock::time_point timestamp = TelemetryClock::now()) noexcept;

    void record_download_delta_at(
        std::uint64_t bytes,
        TelemetryClock::time_point timestamp) noexcept;
    void record_persist_delta_at(
        std::uint64_t bytes,
        TelemetryClock::time_point timestamp) noexcept;
    void record_pause_at(
        TelemetryPauseReason reason,
        bool is_queue_full,
        TelemetryClock::time_point timestamp) noexcept;
    void record_memory_sample_at(
        std::uint64_t memory_bytes,
        TelemetryClock::time_point timestamp) noexcept;

    [[nodiscard]] ProgressSnapshot current_snapshot() const noexcept;
    [[nodiscard]] PerformanceSummary final_summary(
        TelemetryClock::time_point now = TelemetryClock::now()) const noexcept;

private:
    struct AggregationState {
        bool task_started = false;
        bool task_completed = false;
        TelemetryClock::time_point task_started_at{};
        TelemetryClock::time_point task_completed_at{};
        TelemetryClock::time_point latest_event_at{};
        std::optional<TelemetryClock::time_point> first_byte_received_at{};
        std::optional<TelemetryClock::time_point> last_network_sample_at{};
        std::optional<TelemetryClock::time_point> last_disk_sample_at{};
        double network_speed_ema = 0.0;
        double disk_speed_ema = 0.0;
        std::uint64_t total_download_bytes = 0;
        std::uint64_t total_persist_bytes = 0;
        std::size_t packets_enqueued_total = 0;
        std::uint64_t total_packet_bytes = 0;
        std::size_t max_packet_size_bytes = 0;
        std::size_t max_memory_bytes = 0;
        std::int64_t max_inflight_bytes = 0;
        std::size_t total_pause_count = 0;
        std::size_t queue_full_pause_count = 0;
        std::size_t latest_memory_bytes = 0;
    };

    void reset_state(TelemetryClock::time_point timestamp) noexcept;
    void update_latest_event_timestamp(
        TelemetryClock::time_point timestamp) noexcept;

    mutable std::mutex state_mutex_{};
    AggregationState state_{};
};

class TelemetryCollector {
public:
    TelemetryCollector() = default;

    TelemetryCollector(const TelemetryCollector&) = delete;
    TelemetryCollector& operator=(const TelemetryCollector&) = delete;

    void record_task_started(
        TelemetryClock::time_point timestamp = TelemetryClock::now()) noexcept;
    void record_first_byte_received(
        TelemetryClock::time_point timestamp = TelemetryClock::now()) noexcept;
    void record_download_delta(
        std::uint64_t bytes,
        TelemetryClock::time_point timestamp = TelemetryClock::now()) noexcept;
    void record_persist_delta(
        std::uint64_t bytes,
        TelemetryClock::time_point timestamp = TelemetryClock::now()) noexcept;
    void record_pause(
        TelemetryPauseReason reason,
        bool is_queue_full,
        TelemetryClock::time_point timestamp = TelemetryClock::now()) noexcept;
    void record_memory_sample(
        std::uint64_t memory_bytes,
        TelemetryClock::time_point timestamp = TelemetryClock::now()) noexcept;
    void record_task_completed(
        TelemetryClock::time_point timestamp = TelemetryClock::now()) noexcept;

    [[nodiscard]] ProgressSnapshot current_snapshot() const noexcept;
    [[nodiscard]] PerformanceSummary final_summary(
        TelemetryClock::time_point now = TelemetryClock::now()) const noexcept;

private:
    TelemetrySession session_{};
};

}
```

`telemetry_session.hpp` 与 `telemetry_collector.hpp` 只保留 `#pragma once` 和对
`telemetry_contract.hpp` 的 include。兼容 wrappers 不建立新的运行时 module，也不各复制一份
声明。

`src/telemetry/telemetry_session.cpp` 保存全部聚合实现和 helper。
`src/telemetry/telemetry_collector.cpp` 只保存 shell 委托。不得创建
`src/telemetry/telemetry_state.cpp` 再让 Session 与 Collector 同时依赖它，否则 state owner
又变得含糊。

## 7. Event acceptance and idempotency

这里的 “event” 只表示一次同步 record 调用，不表示恢复 `TelemetryEvent` 类型或 event queue。

### 7.1 Task lifecycle

| 调用 | start 前 | active task | completed 后 |
| --- | --- | --- | --- |
| `record_task_started(t)` | reset 并 active | reset 并开始新 task | reset 并开始新 task |
| `record_first_byte_received(t)` | ignore | 接受 earliest t | ignore |
| `record_download_delta*` | ignore | 累加一次 | ignore |
| `record_persist_delta*` | ignore | 累加一次 | ignore |
| `record_pause*` | ignore | 累加一次 | ignore |
| `record_memory_sample*` | ignore | 更新 latest/max | ignore |
| `record_task_completed(t)` | ignore | first call 完成 | ignore |

`record_task_started()` 不是幂等调用；重复调用代表显式开始一个新 observation epoch，并清空
全部旧值。这与当前 `reset_state()` 一致。

### 7.2 Duplicate rules

- first byte：相同或更晚 timestamp 不改变结果；更早 timestamp 替换 earliest；
- task completion：第一次接受，后续忽略；
- download/persist delta：不去重，每次调用都代表新的字节事实；
- pause：Session 不做 episode 去重，producer 必须只在 pause bit `0 → 1` 时调用；
- memory：相同 sample 对 max 幂等，但 latest 仍按最后接受的调用赋值；
- packet count：每个 accepted `record_download_delta*` 增加一次，包括显式传入 `0`；
- production 不得传入 zero-byte packet；zero 行为仅为公开兼容和确定性合同。

Session 不能按 bytes、timestamp 或 reason 猜测 duplicate。真正的 identity 和 episode edge 由
Packet Flow、Range Lifecycle 与 HTTP Transfer 拥有。

## 8. Timestamp and calculation contract

### 8.1 Clock

- 所有内部时间使用 `TelemetryClock`；
- `TelemetryClock` 必须继续是 `std::chrono::steady_clock`；
- 不使用 `system_clock`、wall-clock timestamp 或跨进程 epoch；
- `_at` 方法只用于确定性调用和已有 timestamp 的 producer；
- 无 timestamp 方法在进入 Session 时捕获一次 `TelemetryClock::now()`；
- mutex 等待时间不应改变本次 record 的 timestamp。

### 8.2 Latest event

每个 active record 调用执行：

```text
latest_event_at = max(latest_event_at, supplied_timestamp)
```

completion 执行：

```text
task_completed_at = max(completion_timestamp, latest_event_at)
latest_event_at = task_completed_at
```

因此显式 completion timestamp 不能让 task end 早于已经接受的最新事件。

### 8.3 Out-of-order samples

为了保持现有计算，download/disk sample 的规则仍是：

1. 若没有 previous sample，只保存 supplied timestamp；
2. 否则用 `supplied - previous` 计算 interval；
3. interval `<= 0` 时 instantaneous speed 为 `0.0`；
4. 若旧 EMA 为 `0.0`，EMA 直接取 instantaneous；
5. 否则 `EMA = 0.8 * old + 0.2 * instantaneous`；
6. 最后把 previous sample 设置为 supplied timestamp；
7. total bytes 无论 timestamp 是否倒序都照常增加。

这意味着显式倒序 sample 可能使 EMA 衰减，并把 previous timestamp 暂时后移到更早时间。
Phase 6 不把它改成 discard、clamp 或 reorder，因为这会改变现有计算。若真实并发 evidence
证明该行为是缺陷，应另开 correctness ticket；不得藏在所有权迁移中。

### 8.4 Snapshot speed

`current_snapshot()` 中：

- network speed 是 network EMA；
- disk speed 是 disk EMA；
- downloaded/persisted 是当前 task 的 observed delta totals；
- inflight 是 observed totals 的非负差；
- memory 是最后一次 accepted memory sample；
- 其它 `ProgressSnapshot` 字段为默认值。

这只是公开兼容投影。production progress merge 只能复制其中两个 speed 字段。

### 8.5 Final duration

```text
if completed:
    end = task_completed_at
else:
    end = max(latest_event_at, supplied_now)
```

只有 `task_started && end > task_started_at` 时计算 average speed；否则两项 average 为 `0.0`。
分母同时用于 network 和 disk：

```text
average_network = total_download_bytes / duration_seconds
average_disk = total_persist_bytes / duration_seconds
```

不得改为从 first byte 开始、分别使用最后 network/disk sample、EMA average 或 wall-clock
process duration。

### 8.6 TTFB

生产侧只在 HTTP Transfer 已验证本次 callback bytes 有效，并且 Packet Flow admission 返回
`accepted && consumed_bytes > 0` 后记录 first byte。不得在 64 KiB draft publish 时记录，
否则 aggregation 会人为增加 TTFB。

summary 规则：

```text
if task started
and first byte exists
and first byte >= task start:
    TTFB = floor(milliseconds(first byte - task start))
else:
    TTFB = 0
```

TTFB `0` 同时可以表示小于 1 ms、没有有效 first byte 或非法早时间；正式 schema 当前没有
optional/valid bit，本阶段不改变。

## 9. Overflow and numeric rules

当前实现的裸 unsigned addition 和 narrowing cast 在不可实现的大输入上可能 wrap 或产生
implementation-defined 结果。record interface 又没有错误返回值，因此选定 **饱和而不回绕**：

- `uint64_t` byte totals 在 `UINT64_MAX` 饱和；
- `size_t` counters 在 `SIZE_MAX` 饱和；
- `uint64_t → size_t` 在 `SIZE_MAX` 饱和；
- snapshot/summary 的 `uint64_t → int64_t` 在 `INT64_MAX` 饱和；
- inflight 先在 unsigned domain 做非负差，再饱和到 `INT64_MAX`；
- `total_packet_bytes` 饱和后 average packet size 使用饱和值；
- max fields 保持单调，不因 overflow 变小；
- duration `<= 0` 仍产生 `0.0` speed；
- 不产生 NaN、负 count 或 modulo-wrap 的公开指标；
- 不新增 overflow key、异常或 error code。

饱和只改变超出公开结果类型可表示范围的病理输入；正常下载的公式和数值不变。它必须是独立
`fix:` commit：

1. 先用 deterministic Session tests 证明当前 wrap 风险；
2. 再加入 implementation-private saturating helpers；
3. 单独跑 Debug/Release 与性能 smoke；
4. 若任何真实 case 改变，立即回滚该 commit，不回滚 state ownership。

不得用 `long double`、任意精度整数或 per-event allocation 实现该规则。

## 10. Formal 10-key Performance Summary

### 10.1 C++ members and meanings

| # | C++ member | C++ type | 精确定义 |
| ---: | --- | --- | --- |
| 1 | `average_network_bytes_per_second` | `double` | task observed download bytes / task duration seconds |
| 2 | `average_disk_bytes_per_second` | `double` | task observed persisted bytes / task duration seconds |
| 3 | `time_to_first_byte_ms` | `std::int64_t` | earliest valid first byte 与 start 的整毫秒差 |
| 4 | `max_memory_bytes` | `std::size_t` | accepted Accounted Bytes samples 的最大值 |
| 5 | `max_inflight_bytes` | `std::int64_t` | observed `max(0, downloaded - persisted)` 峰值 |
| 6 | `total_pause_count` | `std::size_t` | producer 报告的全部 pause episode entry 数 |
| 7 | `queue_full_pause_count` | `std::size_t` | `is_queue_full` 或 reason 为 queue_full 的 entry 数 |
| 8 | `packets_enqueued_total` | `std::size_t` | successful Data Packet publish 的数量 |
| 9 | `average_packet_size_bytes` | `double` | published Data Packet bytes / packet count |
| 10 | `max_packet_size_bytes` | `std::size_t` | 单个 published Data Packet 最大 payload bytes |

`PerformanceSummary` 继续继承
`performance::SummaryPerformanceMetrics`。本阶段不改变该 inheritance shape，避免 aggregate
和 member access 源码变化。

### 10.2 CLI keys and parser types

| CLI key | 来源 | CLI unit | Python parser type |
| --- | --- | --- | --- |
| `avg_network_speed` | average network | MiB/s，显示为 `MB/s` | `speed → float` |
| `avg_disk_speed` | average disk | MiB/s，显示为 `MB/s` | `speed → float` |
| `time_to_first_byte_ms` | TTFB | ms | `int` |
| `max_memory_bytes` | max memory | bytes | `int` |
| `max_inflight_bytes` | max inflight | bytes | `int` |
| `total_pause_count` | all pauses | count | `int` |
| `queue_full_pause_count` | queue pauses | count | `int` |
| `packets_enqueued_total` | data packets | count | `int` |
| `avg_packet_size_bytes` | average packet | bytes | `float` |
| `max_packet_size_bytes` | max packet | bytes | `int` |

`src/main.cpp` 当前将 bytes/s 除以 `1024 * 1024`，所以实际是 MiB/s；不得在本阶段把计算换成
十进制 MB/s 或改 key。`scripts/performance/performance_common.py::SUMMARY_SPECS` 的 10 个
source keys、target keys 和 parser types必须逐项相同。

### 10.3 非 summary 字段

以下内容不得加入正式 summary：

- status/error；
- total/downloaded/persisted/resumed；
- active/finished ranges；
- queue/memory/gap/window pause 分项；
- pause duration 或 episode latency；
- queued payload/accounted bytes；
- CRC、flush、metadata 或 write shape；
- CPU、thread、handle 或 diagnostic export；
- watermark timestamp。

运行结果、ProgressSnapshot 和内部诊断有各自 owner，不能借 summary 建立第二条输出链。

## 11. ProgressSnapshot merge responsibility

### 11.1 Sole merger

最终 `ProgressSnapshot` 只能由 orchestrator-confined progress builder 合并。建议保留为
`DownloadEngine` private helper 或 Phase 5 HTTP orchestrator helper，不新增 public module。

合并来源固定为：

| Progress field | 权威来源 |
| --- | --- |
| `total_bytes` | Remote Object Facts / task facts |
| `downloaded_bytes` | checked `Recovery restored trusted bytes + PacketFlowSnapshot::published_data_bytes` |
| `persisted_bytes` | Persistence Writer absolute counter；启动前以 validated `RestoredCheckpoint::trusted_bytes` 初始化 |
| `vdl_offset` | Recovery Checkpoint 最后成功 committed VDL |
| `inflight_bytes` | merger 对 absolute downloaded/persisted 计算 |
| `queued_packets` | `PacketFlowSnapshot` |
| `active_requests` | `HttpSessionSnapshot::active_transfers` |
| `paused_ranges` | `HttpSessionSnapshot::paused_transfers` |
| `memory_bytes` | `PacketFlowSnapshot::accounted_bytes` |
| `network_bytes_per_second` | Telemetry Session current snapshot |
| `disk_bytes_per_second` | Telemetry Session current snapshot |
| `resumed` | recovery/task fact |

Phase 3 §12.1 中“Orchestrator 自行汇总 queue/memory/window/gap”的文字是 Phase 5 前的
compatibility shape。Phase 5 完成后，HTTP Session 已经组合 Packet Flow pause actions 与
Lifecycle gap fact，且 window overflow 已变成 terminal error；Phase 6 以
`HttpSessionSnapshot::paused_transfers` 为最终接口，不重新做第二次 pause merge。

### 11.2 Required merge shape

builder MUST 从空 `ProgressSnapshot{}` 开始，逐字段赋值。不得：

- 先复制 Telemetry snapshot 再依赖覆盖顺序；
- 从 Telemetry 复制 downloaded、persisted、inflight 或 memory；
- 从 Lifecycle 直接复制 queue/memory pause；
- 从 Packet Flow 推导 VDL 或 completed ranges；
- 从 Recovery 计算 network/disk speed。

Recovery base 只在 task 初始化时固定一次；Packet Flow counter 只统计本 Session 新成功发布的
Data Packet。二者 checked 相加，overflow 为 internal error，不 clamp。Persistence absolute
counter 同样在 Writer 启动前以该 validated trusted base 初始化，之后只随成功新写入 checked
增加。这样 resume progress 的 downloaded/persisted 包含相同已可信基数，不会制造虚假
inflight，也不会把恢复字节误记成当前 network/disk throughput 或 packet telemetry。

Telemetry query 的使用方式必须显式：

```cpp
const auto telemetry_snapshot = telemetry.current_snapshot();
snapshot.network_bytes_per_second =
    telemetry_snapshot.network_bytes_per_second;
snapshot.disk_bytes_per_second =
    telemetry_snapshot.disk_bytes_per_second;
```

用户 callback 必须在所有内部锁释放后调用。callback 抛出的异常继续由现有 progress adapter
吞掉，不把 exception 穿过 `noexcept` 下载 interface；Phase 6 不改变 callback policy。

### 11.3 Snapshot consistency

ProgressSnapshot 是一次 best-effort 观察，不是跨 module transaction：

- 每个 module snapshot 自身必须一致；
- merger 按固定顺序读取；
- inflight 只由同一轮读取的 absolute downloaded/persisted 计算；
- `persisted_bytes` 不得在 snapshot 中超过本轮 `downloaded_bytes` 而产生负 inflight；
- 若并发读取造成 persisted 暂时更大，inflight clamp 为 0；
- callback 不获得内部 reference；
- correctness 不能依赖多个字段处于同一纳秒。

若未来需要全局一致 cut，必须另行设计 publication epoch；Phase 6 不新增全局锁。

## 12. Ownership, lifetime and threading

### 12.1 Ownership

| Object | Owner | Borrowers |
| --- | --- | --- |
| production `TelemetrySession` | 一个 Download Request 的 session aggregate | Packet Flow、HTTP、Persistence、orchestrator |
| Session aggregation state | `TelemetrySession` by value | 无外部借用 |
| Session mutex | `TelemetrySession` | 所有 record/query |
| compatibility Collector | 外部 caller 或兼容测试 | 无 production borrower |
| Collector inner Session | Collector by value | Collector methods only |
| returned snapshot/summary | caller by value | 无内部 reference |

production Session 必须先于 Packet Flow、HTTP Transfer 和 Persistence 构造，晚于这些 borrower
销毁。正确销毁顺序：

```text
stop HTTP production
close and drain Packet Flow
join Persistence and checkpoint worker
take final summary
destroy dependent modules
destroy TelemetrySession
```

mutex 只保护仍然存活的 object，不解决 use-after-free。不得把 Session 装入 global singleton、
`shared_ptr` graph 或 detached worker。

### 12.2 Thread contract

- HTTP/orchestrator、Packet Flow 和 Persistence 可以并发 record；
- record/query 都用 Session 的同一把 mutex 串行化 aggregation state；
- no timestamp wrapper 在取得 mutex 前捕获 timestamp；
- query 在 mutex 内只做 bounded arithmetic 和按值 copy；
- 不在 mutex 内调用用户 callback、filesystem、curl、queue 或 recovery；
- 不在 mutex 内等待另一个 project mutex；
- Collector shell 不增加第二把 mutex；
- 没有 background telemetry thread；
- 没有 condition variable、drain 或 shutdown method。

### 12.3 Synchronization and memory order

- 全部 aggregation fields 是 mutex 保护的 plain values，不另加 atomic；
- record 在同一 mutex critical section 内完成 acceptance、累计、peak 和 timestamp 更新；
- query 在同一 critical section 内读取一个内部一致的 aggregation cut；
- mutex unlock → 后续 lock 提供所需 happens-before，不再手写 fence；
- Collector shell 不缓存 query 结果，也不发布第二份 atomic state；
- Packet Flow、Range、Recovery 和 HTTP 各自的 publication memory order 由前序阶段负责；
- Session 不读取这些 module 的 live state，因此不复制它们的 acquire/release 规则；
- producer 必须先完成自己的 authoritative transition，再调用 record；
- Telemetry record 完成不反向证明业务 transition durable 或 globally visible；
- progress merger分别读取各 module snapshot，再按值组合，不用 Telemetry mutex充当全局 barrier。

不得把 mutex 替换成一组 relaxed atomics。EMA、first byte、task lifecycle、packet average 和
completion clamp 是多字段不变量，分散 atomics 会制造不可解释的混合 snapshot。

### 12.4 Performance contract

当前 record hot path 已经取得 Collector mutex。迁移后：

- mutex acquisition 数不能增加；
- 每次 record 不得分配；
- 不新增 virtual dispatch、`std::function`、shared ownership 或 queue；
- Session wrapper → Collector wrapper 的一层调用应被删除；
- helper 保持 translation-unit private；
- summary/snapshot 仍是低频 query；
- state 按 task local，两个 Download Request 不共享 cache line 或 global counter；
- timestamp injection 不改变 production hot path 的一次 `now()` 形状。

### 12.5 Error and exception contract

- 全部现有和新增 record/query 方法继续 `noexcept`；
- record 没有可返回的业务错误，overflow 使用饱和规则；
- query 不分配，不返回 `std::error_code`；
- 不捕获并隐藏 allocation error，因为实现不得在 record/query 中分配；
- 不以 diagnostic counter 表示内部失败；
- 若平台 mutex primitive 本身无法工作，`noexcept` 终止语义与当前实现相同，本阶段不建立
  fallback state；
- Collector shell 不吞掉 Session failure，也不增加 try/catch。

## 13. Producer migration after Phases 2–5

### 13.1 Download orchestrator

orchestrator：

- 构造 task-owned `TelemetrySession`；
- 用进入主下载流程时的同一个 `run_started` 调用 `record_task_started(run_started)`；
- 把 non-owning reference 传给 Packet Flow、HTTP Transfer 和 Persistence；
- 成功 finalize 后捕获一个 `completed_at`，先 record completed，再以同一值取 final summary；
- 失败时不伪造 completion，使用捕获的 `failed_at` 调用 `final_summary(failed_at)`；
- 把 summary 赋给 `DownloadResult::performance`；
- 不直接记录 packet、memory、queue 或 persistence 指标。

resume-complete fast path 必须使用同一 success helper，不能复制一套遥测结束协议。

### 13.2 Packet Flow

Phase 2 `PacketFlow` 是以下指标的唯一 producer：

- Data Packet successful publish → `record_download_delta(bytes)`；
- Accounted Bytes 变大后的 current sample → `record_memory_sample(current)`；
- Queue Pause bit `0 → 1` → `record_pause(queue_full, true)`；
- Memory Pause bit `0 → 1` → `record_pause(memory_pressure, false)`。

约束：

- Data Packet admission 失败不记录 download delta；
- draft append 不增加 packet count；
- Data Control、Range Complete 和 close marker 不记录 packet；
- replayed curl batch 只在最终 successful publish 后记录一次；
- queue/memory 重叠各自只在自己的 edge 记录一次；
- memory sample 来自 request-local Packet Flow ledger，不读 global accounting；
- Phase 6 不新增 Packet Flow telemetry port，继续借用 concrete Session。

### 13.3 HTTP Transfer

Phase 5 的 `http::HttpTransferPort` / `HttpTransferSession`：

- 在 validated body bytes 被 Packet Flow 接受后记录 first byte；
- first-byte helper可以被后续 callback 重复调用，Session 保留 earliest；
- `poll()` 是调用 `PacketProducer::reconcile()` 并组合 queue/memory 与 Range gap 的唯一位置；
- 只有 HTTP Session owner thread 调用 `curl_easy_pause()`；
- queue/memory episode 仍由 Packet Flow 唯一记录，HTTP 只应用 pause actions；
- normal exact-window 完成不记录 pause；
- 超过 window 的额外 body 返回 terminal `body_too_long`，不记录 `none` pause；
- `HttpSessionSnapshot` 只提供 state、active、available、paused、`pending_events` 和
  session error 观测；
- per-slot rate 只供内部 Packet Flow reconcile，不作为 progress 或 summary 字段导出；
- transport retry 不在本阶段引入，因此不存在跨 attempt 指标合并新规则；
- HTTP status/header/protocol failure 不直接修改 summary；
- curl callback 不持有 Collector 或 aggregation state pointer。

first byte 不得移动到 probe、header callback、socket connect、draft append 或 packet flush。

### 13.4 Range Lifecycle and gap

Range Lifecycle 只拥有 gap fact，不拥有 pause telemetry。orchestrator 把该 fact 通过
`HttpTransferSession::set_gap_paused(token, active)` 交给 HTTP Session。该方法是实际
transfer-gap episode 的唯一 owner；对应 slot 的 gap bit `0 → 1` 时调用：

```cpp
telemetry.record_pause(TelemetryPauseReason::gap, false);
```

duplicate `true` 不计数，`1 → 0` 只恢复不计数，无 active/stale token 不计数并返回 Phase 5
定义的确定性错误。orchestrator 只转交 fact，不调用 Telemetry。Lifecycle 不保存
`total_pause_count`，Telemetry 不保存 authoritative `gap_blocked`。

### 13.5 Persistence

Persistence Writer：

- 在启动线程前，以 checked `RestoredCheckpoint::trusted_bytes` 初始化 authoritative
  absolute persisted counter；fresh 为 0；
- physical aligned write 成功后记录实际写入 bytes；
- final tail 成功写入后记录 tail bytes；
- write 失败不记录未写入 bytes；
- move 到 reorder map、bitmap update、flush、CRC 和 metadata 不记录 persisted delta；
- checkpoint worker 不借用 Telemetry Session；
- record 发生在 authoritative persisted counter 成功推进之后；
- restored trusted base 不调用 `record_persist_delta()`，不形成 disk telemetry sample；
- 使用一个局部 bytes 值同时推进 counter 与 record，避免两处长度计算漂移。

### 13.6 Compatibility Collector

全仓 production source 不得构造 `TelemetryCollector`。允许出现的位置：

- `src/telemetry/telemetry_collector.cpp`；
- Collector compatibility tests；
- public compile fixture；
- 历史文档。

## 14. Tests-first tiny commits

每个 slice 必须独立 build、test、commit 和 rollback。

| Slice | Commit intent | Change | Verification | Slice rollback |
| ---: | --- | --- | --- | --- |
| 06.1 | `test: characterize telemetry contracts` | 锁定现有计算、时间、lifecycle、schema 和 signatures | 新 Session characterization 先对当前 facade 通过；记录已知缺口 | 只删新 tests |
| 06.2 | `feat: add deterministic telemetry session variants` | 新增四个 `_at` 方法，仍委托 current Collector | compile fixture + deterministic calls | 删除唯一名称方法 |
| 06.3 | `refactor: make telemetry session own aggregation` | 增加 contract header；状态与计算搬入 Session；Collector 变 shell | Session 全矩阵 + Collector 最小等价 | revert ownership commit |
| 06.4 | `refactor: make progress merging explicit` | 从空 snapshot 合并 Phase 2–5 facts，只复制 telemetry speeds | progress merge unit/integration | 恢复旧 merge helper |
| 06.5 | `refactor: align telemetry producers with module owners` | 清除 engine 重复 producer；固定 flow/http/persistence edges | cross-module event-count tests | 按 producer 文件回滚 |
| 06.6 | `fix: saturate telemetry arithmetic` | checked/saturating helper 与 overflow tests | deterministic edge tests + Release smoke | 只回滚 arithmetic commit |
| 06.7 | `test: collapse collector tests into session surface` | 核心 tests 迁 Session；Collector 留最小兼容 tests | telemetry suites + deletion checks | 恢复旧 tests，不回滚 production |
| 06.8 | `docs: record telemetry migration evidence` | 保存 build/test/schema/benchmark/rollback evidence | exit checklist | 只回滚 evidence |

06.3 是主要结构 slice，不能同时修改计算公式。06.6 是明确 correctness slice，不能藏在
06.3。06.4/06.5 若前序 Phase 的实际文件过大，应按 owner 分拆，但每个 commit仍需独立通过。

## 15. Exact test contract

### 15.1 Public compatibility compile fixture

新增 `tests/telemetry/telemetry_public_compatibility_test.cpp`，覆盖：

- `#include "asyncdownload/telemetry.hpp"`；
- 只 include `telemetry_session.hpp`；
- 只 include `telemetry_collector.hpp`；
- 两种 class 默认构造；
- 所有现有普通调用；
- 所有 Collector 显式 timestamp 调用；
- Session 新 `_at` 调用；
- enum underlying type 和数值；
- `telemetry_timestamp_ns()`；
- `ProgressSnapshot`/`PerformanceSummary` 返回类型；
- exact pointer-to-member type。

至少包含等价 static assertions：

```cpp
using SessionDownloadMember =
    void (TelemetrySession::*)(std::uint64_t) noexcept;
using SessionPauseMember =
    void (TelemetrySession::*)(TelemetryPauseReason, bool) noexcept;
using CollectorDownloadMember =
    void (TelemetryCollector::*)(
        std::uint64_t,
        TelemetryClock::time_point) noexcept;

static_assert(std::is_same_v<
    decltype(static_cast<SessionDownloadMember>(
        &TelemetrySession::record_download_delta)),
    SessionDownloadMember>);
static_assert(std::is_same_v<
    decltype(static_cast<SessionPauseMember>(
        &TelemetrySession::record_pause)),
    SessionPauseMember>);
static_assert(std::is_same_v<
    decltype(static_cast<CollectorDownloadMember>(
        &TelemetryCollector::record_download_delta)),
    CollectorDownloadMember>);
```

### 15.2 Session behavior tests

核心计算全部通过 `TelemetrySession` 测试：

- start resets all state；
- before-start updates ignored；
- first byte earliest and later duplicates ignored；
- second start starts a clean epoch；
- completed 后所有 update ignored；
- duplicate completion ignored；
- incomplete summary uses supplied now；
- completion timestamp clamps to latest event；
- completed summary 不受 later supplied now 影响；
- out-of-order delta 保持现有 zero-instantaneous/EMA 行为；
- zero/non-positive duration yields zero average；
- first network sample建立 timestamp但 speed为 0；
- EMA exact `0.8/0.2`；
- network/disk task averages；
- memory latest 与 peak；
- inflight current 与 peak；
- pause total 与 queue predicate；
- packet count/total/average/max；
- zero packet兼容行为；
- every formal field default is zero；
- second task does not leak first task state。

确定性 test 不允许 `sleep_for()`，全部使用 explicit time points 和 `_at`。

### 15.3 Overflow tests

06.6 增加：

- download/persist/packet byte total near `UINT64_MAX`；
- packet/pause count near `SIZE_MAX` 的 test-only state seed seam；
- `uint64_t` packet/memory 到小 `size_t` 的 portable helper tests；
- inflight 超过 `INT64_MAX`；
- max fields 不回退；
- no negative/NaN；
- saturation 后 repeated records 保持饱和；
- summary/schema type不变。

test-only seed seam 必须是 private friend 或 translation-unit helper，不进入 public interface，也
不能给 production 提供 set-counter 方法。

### 15.4 Collector compatibility tests

Collector 只保留最小 tests：

1. 默认构造；
2. 每个现有 signature 可调用；
3. 显式 timestamp 的一组事件得到与直接 Session 相同的 snapshot/summary；
4. start reset 与 completed ignore由 shell 正确转发；
5. Collector 与另一个 Session 状态互不共享。

不得把完整 Session matrix 复制到 Collector。Collector tests 的目的只是证明 shell，不是让
shell看起来像第二个 deep module。

### 15.5 Producer tests

Packet Flow：

- accepted publish 记录一次 packet/bytes；
- admission rejected/paused/failed/closed 记录 0；
- draft 多次 append、一次 publish只记录 1 packet；
- replay 不重复；
- control/close 不计 packet；
- queue/memory episode edge count正确；
- current accounted sample产生正确 peak。

HTTP Transfer：

- headers/probe 不触发 first byte；
- rejected body 不触发；
- accepted nonzero body触发；
- repeated callback保留 earliest；
- normal exact-window 不增加 pause；
- long body terminal error不增加 `none` pause；
- active token 的 gap `0 → 1` 记录一次；
- duplicate gap `true`、gap clear 和 stale token不增加 pause。

Persistence：

- resume 在 Writer 启动前把 absolute persisted 初始化为 trusted bytes，但 telemetry
  persisted total 仍为 0；
- direct aligned write精确记录；
- final tail精确记录；
- reorder move不记录；
- write failure不记录；
- flush/checkpoint不记录；
- fresh task 的 absolute persisted 与 telemetry observed persisted 最终都等于 object size；
- resume 后 absolute persisted 等于 trusted base 加本次成功新写入；disk telemetry 只等于
  本次成功新写入。

### 15.6 Progress merge tests

构造彼此故意不同的 telemetry projection 和 authoritative facts，证明：

- progress downloaded/persisted来自 task facts，不来自 Telemetry；
- resume trusted base 同时进入 absolute downloaded/persisted，初始 inflight 为 0；
- complete fast path 的 downloaded/persisted 都等于 validated trusted object size；
- queue/memory来自 Packet Flow；
- VDL来自 Recovery；
- active来自 HTTP；
- paused来自 composite merge；
- 只有 network/disk speed来自 Telemetry；
- callback取得值副本；
- callback不在任何 module lock内；
- inflight按同轮 absolute counters计算且不为负。

该测试是“不成为第二真相源”的主要 executable proof。

### 15.7 Concurrency tests

- HTTP、Packet Flow 和 Persistence 三个线程并发 record；
- query thread 并发读取 snapshot/summary；
- barrier/latch 明确开始和结束，不用 sleep猜测；
- 最终 totals/counts与每线程输入精确一致；
- first byte保留最早 timestamp；
- completion 后 writers继续调用但结果不变；
- 两个独立 Session 无交叉污染；
- sanitizer可用时运行 TSAN；MSVC 至少高重复 stress。

测试结束前 join 全部 producer。不得用“mutex没有崩溃”代替精确结果断言。

## 16. Functional and schema gates

### 16.1 Build and test

每个 slice：

```powershell
scripts\build.bat
build\tests\Debug\AsyncDownload_tests.exe `
  --gtest_filter=TelemetrySessionTest.*:TelemetryCollectorCompatibilityTest.*:TelemetryPublicCompatibilityTest.*
```

阶段结束：

```powershell
scripts\build.bat
scripts\build.bat release
build\tests\Debug\AsyncDownload_tests.exe
build\tests\Release\AsyncDownload_tests.exe
```

阶段 0 已记录的 Windows `error=740` 两个 CLI process tests：

- 不能扩大；
- 不能改变失败原因；
- 在允许正常 process launch 的环境中必须重跑；
- library-level telemetry/progress tests必须独立全绿。

### 16.2 Exact schema check

至少自动比较：

```text
SummaryPerformanceMetrics member set
main.cpp write_summary key set
performance_common.py SUMMARY_SPECS source key set
benchmark raw/aggregated numeric field set
integration summary-file assertions
```

期望集合必须精确等于：

```text
avg_network_speed
avg_disk_speed
time_to_first_byte_ms
max_memory_bytes
max_inflight_bytes
total_pause_count
queue_full_pause_count
packets_enqueued_total
avg_packet_size_bytes
max_packet_size_bytes
```

既不能缺 key，也不能多出 status/diagnostic key。Python smoke 必须证明缺任一 key仍会失败，而
完整 10 key 可解析。

### 16.3 Static/deletion checks

```powershell
rg -n "TelemetryCollector" src `
  -g "*.cpp" -g "*.hpp"

rg -n "collector_|state_mutex_|network_speed_ema_|total_packet_bytes_" `
  include/asyncdownload/telemetry src/telemetry

rg -n "TelemetrySink|TelemetryEventType|TelemetryPayload|make_telemetry_event" `
  include src tests

rg -n "processed_event_count|wait_until_drained|start_consuming|stop_consuming" `
  include src tests

rg -n "queued_bytes|queued_payload_bytes|watermark_timestamp_ns|diagnostic-file" `
  include src tests scripts
```

期望：

- production `TelemetryCollector` 只在其 shell implementation 出现；
- aggregation fields 只在 `TelemetrySession` declaration/implementation 出现；
- `collector_` 不再是 Session member；
- retired Sink/Event/worker symbols为 0；
- retired diagnostics不因 Phase 6 回归；
- historical `.planning` 和 performance docs不在删除检查范围。

## 17. Performance neutrality gate

### 17.1 Fixed behavior

本阶段 MUST 保持：

- 64 KiB packet aggregation；
- successful Data Packet publish 计数点；
- first-byte 计数点；
- EMA `0.8/0.2`；
- task-duration average公式；
- pause episode edge语义；
- Phase 5 后 normal exact-window 和 long-body terminal error 都不增加 pause；
- Queue Pause 的 `is_queue_full || reason == queue_full` 口径；
- Packet Flow high/low 与 Top 20%；
- Persistence write/flush/checkpoint cadence；
- HTTP connection/window/replay语义；
- exact 10-key schema；
- no per-event allocation；
- one task-local mutex；
- synchronous aggregation。

### 17.2 Formal benchmark

在实际 Phase 6 base commit 先生成 pre；post 使用同一机器、对象、server、case、环境：

```powershell
scripts\build.bat release

python scripts\performance\benchmark.py `
  --url "http://127.0.0.1:4287/1gb_files.zip" `
  --benchmark-suite regression_v2 `
  --case-list baseline_default,balanced_candidate,memory_guard,scheduler_stress `
  --repeats 20 `
  --label "phase-6-pre"

python scripts\performance\benchmark.py `
  --url "http://127.0.0.1:4287/1gb_files.zip" `
  --benchmark-suite regression_v2 `
  --case-list baseline_default,balanced_candidate,memory_guard,scheduler_stress `
  --repeats 20 `
  --label "phase-6-post"
```

接近 gate、存在双峰或 telemetry mutex 栈形状异常时提高到 40 repeats。

阻断 gate：

| Signal | Gate |
| --- | --- |
| Debug/Release | build pass |
| previous passing tests | no new failure |
| summary schema | exact 10 keys |
| baseline network/disk median | 任一下降超过 5% 阻断 |
| balanced network/disk median | 任一下降超过 5% 阻断 |
| TTFB | 系统性恶化必须调查 |
| memory_guard peak | 保持实际 pre低内存形态；无收益不得显著上升 |
| max inflight | 多 case显著上升且无收益阻断 |
| pause counts | 多 case约 15% 物质回归需调查 |
| packet shape | average/max约 64 KiB，count语义不变 |

历史 2026-03-11 数字只作背景；keeper 结论以当前 base 的同机 pre/post 为准。

### 17.3 Profiler

benchmark 通过后再单独运行：

```powershell
python scripts\performance\profiler.py `
  --url "http://127.0.0.1:4287/1gb_files.zip" `
  --benchmark-suite regression `
  --case-list throughput_candidate,scheduler_stress `
  --label "phase-6-profile"
```

只回答：

- `TelemetrySession::record_*` mutex 是否成为新热点；
- 删除 wrapper 后 call stack 是否收敛；
- 是否出现 per-event allocation、queue 或 worker；
- Packet Flow/Persistence hot path是否因 telemetry ownership move改变形状。

profiler 不能豁免 benchmark 或 schema gate。

## 18. Historical experiments explicitly prohibited

本阶段不得重新引入：

- `TelemetrySink` 或 moodycamel telemetry queue；
- `TelemetryEventType`、tagged payload 或固定 event union；
- background Collector thread；
- timed dequeue、drain、sentinel 或 processed-event diagnostics；
- diagnostic JSON/`acceptance.py`；
- queue/backpressure 细分字段；
- memory/pause duration/latency histogram；
- CRC、flush、write shape 或 resource metrics；
- second progress speed calculation；
- collector snapshot cache；
- `SessionState` telemetry base/last-sample caches；
- 128 KiB aggregation；
- queue depth/resume实验；
- incremental CRC、relaxed flush、compact metadata或handle split；
- retry、HTTP/2、connection reuse或其它前序阶段禁止项。

若确实需要新正式 metric，必须另开性能 ticket，先定义用途、producer、schema、scripts、tests
与 benchmark；不能顺手加入 Phase 6。

## 19. Rollback strategy

### 19.1 Per-slice rollback

- 06.1 tests：删除新增 characterization，不改 production；
- 06.2 deterministic variants：删除 `_at` 和新测试，旧签名不受影响；
- 06.3 ownership inversion：恢复 Session `collector_` 与 Collector state；其它阶段不回滚；
- 06.4 progress merge：恢复旧 merge helper，不回滚 Session owner；
- 06.5 producer alignment：按 Packet Flow/HTTP/Persistence 分别回滚；
- 06.6 saturation：恢复旧 arithmetic，Session owner保持；
- 06.7 tests cleanup：恢复旧测试文件，不回滚 production；
- 06.8 evidence：只回滚文档。

### 19.2 Whole-stage rollback

反向顺序：

1. 恢复 tests；
2. 回滚 arithmetic hardening；
3. 恢复 producer/progress adapters；
4. 恢复 Collector state 与 Session pass-through；
5. 删除 deterministic variants 与 contract header；
6. 保留旧实现也能通过的signature/behavior characterization和本设计文档；
7. 同步回滚只有saturating arithmetic才能通过的overflow green tests，把其red输出保留在
   evidence；不得让whole-stage rollback后的baseline测试失败。

不得回滚 Phase 1–5，也不得恢复 Sink/Event/background worker。

### 19.3 No runtime dual path

禁止 feature flag 在 Session-owned 与 Collector-owned aggregation 之间切换。双轨会立即产生：

- 两把 mutex；
- 两份 totals/peaks；
- summary选择歧义；
- producer重复发送；
- 测试表面翻倍。

回滚依赖小 commit，不依赖运行时双写。

## 20. Risks and stop conditions

以下条件出现时，Code Agent 必须停止对应 slice、保存证据并回到本票据；其它独立 slice可继续：

1. Phase 2–5 的最终 interface 与本文引用不一致，且无法通过机械名称调整接线；
2. 仓库内或用户提供的 downstream fixture 依赖 precompiled binary ABI，而不是全量重编译；
3. 公开 compatibility fixture证明某个现有 method type、default argument或 include路径改变；
4. 当前 base 的 summary不再是本文 10 key；
5. 正式 Python parser和 CLI key已经发生未记录漂移；
6. 某个 producer无法证明 exact-once edge，可能 double-count或漏计；
7. progress merge没有可识别的 authoritative source；
8. Session mutex发生锁顺序环或 callback在锁内；
9. saturation commit改变正常范围 benchmark/test值；
10. benchmark超过 gate；
11. profiler出现新的 telemetry allocation/queue/worker热点；
12. 新失败不能归类为阶段 0 已知环境失败。

如果唯一冲突是 private helper 名称、文件机械位置或前序 module 的最终 namespace，Code Agent
自行做最小调整并在 evidence 记录，不需要重新询问用户。

## 21. Deletion and depth checks

阶段完成后应能回答：

- 删除 `TelemetryCollector` shell 是否会破坏外部源码，但不会损失 production复杂性？
  - 是；因此它是明确兼容 shell，不是假装 deep module。
- 删除 `TelemetrySession` 是否会让聚合、时间、summary、并发和 schema规则散回多个 caller？
  - 是；因此 Session 是 deep module。
- production 是否存在第二个 summary builder？
  - 否。
- production progress 是否读取 Telemetry 的业务累计值？
  - 否，只读两项 speed。
- 是否存在第二份 pause、packet、memory或persisted truth？
  - Telemetry只有派生 observation；correctness owner仍是前序 module。
- 核心行为测试是否经过 Session interface？
  - 是。
- Collector tests 是否只验证 compatibility？
  - 是。

## 22. Code Agent exit checklist

- [ ] 记录实际 base commit、dirty worktree和 Phase 1–5完成状态。
- [ ] 先提交 Session behavior与public compatibility characterization。
- [ ] 原有两个 class的公开签名逐项保持。
- [ ] deterministic variant使用 `_at`，未建立同名 overload。
- [ ] 两个旧 include路径与聚合 header继续编译。
- [ ] Session独占mutex和全部aggregation state。
- [ ] Collector只保存一个Session并委托。
- [ ] production不构造Collector。
- [ ] 计算公式与timestamp规则由deterministic tests锁定。
- [ ] overflow saturation位于独立`fix:` commit。
- [ ] ProgressSnapshot从空值显式merge。
- [ ] production只从Telemetry复制network/disk speed到progress。
- [ ] resume trusted base同时初始化absolute downloaded/persisted，初始inflight为0，且不进入
      network/disk telemetry。
- [ ] Packet Flow/HTTP/Persistence producer exact-once tests通过。
- [ ] success和failure final summary语义保持。
- [ ] exact 10-key schema在C++、CLI、Python和tests一致。
- [ ] retired event/sink/worker/diagnostic symbols未恢复。
- [ ] Debug/Release和全量测试通过或仅保留同原因的已知740环境失败。
- [ ] formal Release pre/post benchmark通过gate。
- [ ] profiler未发现新telemetry hot-path结构。
- [ ] 每个slice记录独立验证和rollback commit。
- [ ] 架构与性能文档只在证据确实变化时同步更新。

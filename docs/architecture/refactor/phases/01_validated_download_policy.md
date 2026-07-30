# 阶段 1：Validated Download Policy

## 1. Outcome

本阶段把公开 `DownloadOptions` 从“可被任意内部调用方直接解释和修改的数据袋”收敛为两步
构造的内部 policy：

1. `ValidatedDownloadPolicy` 在任何网络、文件或线程资源创建前验证调用方输入；
2. `EffectiveDownloadPolicy` 在 HTTP probe 后绑定远端对象事实，并只对运行期暴露不可变
   的 scheduler、flow、persistence 和 recovery 视图。

阶段完成后：

- `DownloadOptions` 的字段、默认值、aggregate initialization 和 CLI 字段保持源码兼容；
- 所有跨字段不变量只有 `download_policy` module 一个实现来源；
- 非 Range 降级不再通过修改 `SessionState::options` 表达；
- `RangeScheduler`、network loop 和 `PersistenceThread` 不再自行解释 raw options；
- 无效配置确定性返回现有错误，不进入 probe、文件创建、queue 构造或 thread 创建；
- 当前合法默认配置的有效行为不变；
- metadata 继续保存调用方请求的 `block_size` 和 `io_alignment`，格式不变。

本阶段的功能和性能 gate 继承
[阶段 0](00_characterization_baseline.md)。Code Agent MUST 先完成阶段 0 的特征化，再实现
本文件。

## 2. 已核实的当前事实

以下事实来自当前基线源码，不是目标设计假设。

| 位置 | 当前事实 | 结构或正确性后果 |
| --- | --- | --- |
| [`types.hpp`](../../../../include/asyncdownload/types.hpp) `DownloadOptions` | 11 个公开字段都有默认值，调用方可直接 aggregate initialize 或逐字段修改 | 不能用新的公开构造函数、必填类型或字段重排破坏源码兼容 |
| [`main.cpp`](../../../../src/main.cpp) `load_download_options_from_config` | CLI 只做单字段类型、正负和部分 zero 检查 | 不验证 power-of-two、watermark 顺序或字段间关系 |
| [`main.cpp`](../../../../src/main.cpp) `main` | 合并顺序是 defaults → config → positional connections | policy 必须在全部覆盖完成后构造 |
| [`client.cpp`](../../../../src/client.cpp) `DownloadClient::download` | 直接创建 `DownloadEngine` 并调用 `run` | library caller 会绕过 CLI 校验 |
| [`download_engine.cpp`](../../../../src/download/download_engine.cpp) `DownloadEngine::run` | 只验证 URL 和 output path 非空，然后先 probe | 非法 options 当前可能在发生网络 I/O 后才暴露，或根本不暴露 |
| 同文件的 session 初始化 | `session.options = request.options`，非 Range 时再原地把 connections 改为 1、window 改为 total size | raw request 和 effective runtime policy 共用一个可变对象 |
| 同文件 `metadata_matches` | 恢复身份比较使用 `request.options.block_size` 和 `io_alignment` | 恢复身份必须继续使用 raw、已验证值，不能使用任意降级后的字段 |
| [`alignment.hpp`](../../../../src/core/alignment.hpp) `align_up` / `align_down` | 用 `alignment - 1` bit mask | alignment 为 0 或非 2 的幂时结果不满足函数语义 |
| [`range_scheduler.cpp`](../../../../src/download/range_scheduler.cpp) | 保存完整 `DownloadOptions`；执行 `block_size * 2`、`window * 2`、`start + window - 1` | 调度器既依赖无关字段，又存在未保护的乘加 |
| 同文件的非 Range 路径 | `build_initial_ranges` 仍从恢复 bitmap 生成 unfinished spans，但 `next_window` 对每个 span 都返回整个对象 | 稀疏恢复状态可能把多个 full-body GET 绑定到局部 Range；这不是可保留的安全行为 |
| [`models.hpp`](../../../../src/core/models.hpp) `TailBuffer` | 存储固定为 `std::array<uint8_t, 4096>` | `io_alignment > 4096` 时，persistence 的 `memcpy` 可以越界 |
| [`persistence_thread.cpp`](../../../../src/persistence/persistence_thread.cpp) `append_bytes` | copy 长度由 `session_.options.io_alignment` 决定 | public field 与固定 buffer 容量没有被绑定 |
| 同文件 `flush_tail` | 本地 staging array 同样固定为 4096 | 不能仅修 `RangeContext::TailBuffer` 而遗漏 flush staging |
| 同文件 `build_metadata_state` | metadata 保存 `session_.options.block_size` 和 `io_alignment` | policy 迁移不得更改恢复格式和值 |
| [`block_bitmap.cpp`](../../../../src/core/block_bitmap.cpp) `required_block_count` | 用 `total_size + block_size - 1` 向上取整 | 对大值需要改为除法加余数，不能依赖可能溢出的加法 |
| [`memory_accounting.cpp`](../../../../src/core/memory_accounting.cpp) `should_pause_for_backpressure` | 用 `current_bytes + incoming_bytes > high` | 背压判断需要等价的减法式 checked comparison |
| vendored `blockingconcurrentqueue.h` 48-53 | constructor capacity 是预分配 slot 下界，不是硬容量 | policy 只能把该字段定义为逻辑 packet budget；不能把 vendor 返回值当业务容量事实 |
| vendored `concurrentqueue.h` 834-847 | 三参数 constructor 根据 capacity 和 producer 数计算预分配 block | queue 创建前必须保证计数可表示，分配失败仍是运行期资源错误 |
| [`file_writer.cpp`](../../../../src/storage/file_writer.cpp) | Windows 单次 `ReadFile` 长度经 `DWORD` 传入 | CRC block 的当前单次读取要求 `block_size` 可由 32-bit I/O 长度表示 |
| [`range_scheduler_test.cpp`](../../../../tests/download/range_scheduler_test.cpp) | 只覆盖默认合法 alignment 下的切分和 steal | 当前没有 options contract 测试 |
| [`main_test.cpp`](../../../../tests/main_test.cpp) | 只覆盖空 request 返回 `invalid_request` | library 入口的无效 policy 需要新增确定性测试 |
| [`download_resume_integration_test.cpp`](../../../../tests/download/download_resume_integration_test.cpp) | 已覆盖配置加载、恢复身份和 Range 并发；测试 server 总是声明 Range | 需要新增非 Range server fixture，而不是从现有 Range 测试推断降级正确 |

## 3. 兼容合同

### 3.1 MUST 保持

- `asyncdownload::DownloadOptions` 的命名空间、字段名、字段顺序、字段类型和默认值不变。
- `DownloadRequest::options` 仍是 `DownloadOptions`，现有调用方不包含内部 header。
- CLI 仍接受顶层配置对象或 `{ "download_options": { ... } }`。
- CLI 覆盖顺序仍是 defaults → config → positional connections。
- 未知 JSON 字段继续按当前行为忽略；本阶段不引入 strict schema。
- 合法配置的实际连接数、window、queue budget、水位、block、flush 和 overwrite 行为不变。
- public library 的 invalid policy 返回
  `make_error_code(DownloadErrc::invalid_request)`，不抛异常。
- CLI 对 invalid policy 仍以非零退出码和 stderr 报错。
- `.part`、`.config.json`、metadata 字段、bitmap、VDL 和 CRC sample 格式不变。
- Range server 下的现有恢复文件继续比较 raw `block_size` 和 `io_alignment`。
- Performance Summary 的 10 个正式 key 不变。

### 3.2 对“当前合法配置”的定义

“当前 parser 接受”不等于“当前合法”。以下输入会破坏当前实现的前置条件，因此允许从未定义
或资源失控行为收紧为确定性 `invalid_request`：

- 0 值被 direct C++ caller 注入必须为正的字段；
- 非 2 的幂 `block_size` 或 `io_alignment`；
- `io_alignment > 4096`；
- `block_size % io_alignment != 0`；
- `backpressure_low_bytes > backpressure_high_bytes`；
- 不能安全转换到 consumer 所需整数类型的值；
- 会使 queue preallocation、bitmap count 或 scheduler arithmetic 不可表示的值；
- 负的 `flush_interval`。

这些输入不得先通过旧实现跑一遍再把异常结果固化为兼容合同。

### 3.3 Non-goals

- 不改变任何默认参数。
- 不公开新的 policy interface。
- 不增加、删除或重命名配置字段。
- 不把 queue capacity 改造成 vendor queue 的硬容量；阶段 2 负责 packet admission。
- 不调整 Queue Pause、Memory Pause 或 Gap Pause 的开始/恢复语义。
- 不动态扩展 `TailBuffer`；本阶段选择验证 `io_alignment <= 4096`。
- 不改变 metadata 格式、CRC 算法或 VDL 语义。
- 不实现 retry、镜像、HTTP/2、认证或新的 probe 行为。
- 不基于文件大小自动减少 Range 模式的连接数。
- 不把本阶段包装成通用配置框架或 public builder。
- 不借机修复阶段 2-6 拥有的其他结构问题。

## 4. 设计选择

### 4.1 方案 A：验证后继续修改 `DownloadOptions` 副本

形状：

```cpp
std::error_code validate(DownloadOptions& options, const RemoteProbeResult* probe) noexcept;
```

优点是改动少。拒绝原因：

- 同一类型在 probe 前代表 raw input，probe 后又代表 effective policy；
- 调用方无法从类型判断 remote-dependent 字段是否已经可用；
- non-Range 降级仍然依赖原地修改；
- recovery 很容易误用已降级值；
- 删除这个 module 后，只需把修改语句放回 engine，复杂度几乎不增加，未通过 deletion test。

这是 shallow module。

### 4.2 方案 B：一个可选字段很多的 `DownloadPolicy`

形状：

```cpp
class DownloadPolicy {
public:
    std::error_code validate() noexcept;
    std::error_code bind_remote(RemoteObjectFacts facts) noexcept;
    bool remote_bound() const noexcept;
};
```

它隐藏了一部分实现，但类型允许“已 validate、未 bind”与“已 bind”两种状态。每个 getter
仍必须规定在哪种状态可调用，测试也要覆盖 invalid ordering。拒绝原因是 interface 把构造
顺序和 partial state 暴露给所有调用方。

### 4.3 方案 C：两个阶段类型，选定

形状：

```text
DownloadOptions
    │ validate_download_options
    ▼
ValidatedDownloadPolicy
    │ bind_remote_facts
    ▼
EffectiveDownloadPolicy
```

选定原因：

- 只有两个构造操作，interface 小；
- 类型本身表达“probe 前”和“probe 后”，不存在 partial state；
- raw options 只读保留，effective views 只读派生；
- 每个 consumer 只学习与自己相关的 view；
- 删除该 module 会让校验、归一化、checked conversion 和 non-Range 降级重新散落到
  engine、scheduler、queue、persistence 和 recovery，符合 deletion test；
- interface 是自然测试面，测试不需要创建 CURL、文件或线程。

依赖分类为 **in-process**：全部是纯值校验和归一化，不需要 adapter。不要为了测试增加
`IPolicyValidator`；只有一个实现时那会是假 seam。

## 5. 目标 module 与 dependency direction

新增内部文件：

```text
src/download/download_policy.hpp
src/download/download_policy.cpp
tests/download/download_policy_test.cpp
```

允许的 dependency direction：

```text
include/asyncdownload/types.hpp
include/asyncdownload/error.hpp
            │
            ▼
src/download/download_policy
            │
            ├──► DownloadEngine orchestration
            ├──► RangeScheduler
            ├──► Packet Flow migration seam
            ├──► PersistenceThread
            └──► Recovery identity checks
```

禁止：

- `download_policy` include libcurl、moodycamel queue、file writer 或 metadata store；
- public headers include `download_policy.hpp`；
- `download_policy` 读取文件、环境变量或全局状态；
- consumer 接收 `DownloadOptions` 后自行重复解释字段；
- 为 production/test 各建一个 policy adapter。

远端 probe 结果由 engine 映射为最小 `RemoteObjectFacts`。这样 policy 不依赖
`HttpProbe` 的 concrete result，也不替 probe 解释 HTTP header。

## 6. 精确 C++ interface

Code Agent SHOULD 使用以下类型和方法名。若因已合并阶段引起机械冲突，可以调整文件内私有
命名，但 MUST 保持相同的 interface 语义、错误粒度和两阶段类型。

```cpp
#pragma once

#include "asyncdownload/types.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <system_error>

namespace asyncdownload::download {

enum class DownloadPolicyErrc : std::uint8_t {
    none = 0,
    max_connections_zero,
    max_connections_not_representable,
    queue_capacity_zero,
    queue_capacity_not_representable,
    scheduler_window_zero,
    scheduler_window_not_representable,
    backpressure_high_zero,
    backpressure_low_above_high,
    block_size_zero,
    block_size_not_power_of_two,
    block_size_not_representable,
    io_alignment_zero,
    io_alignment_not_power_of_two,
    io_alignment_exceeds_tail_capacity,
    block_size_not_aligned_for_io,
    max_gap_zero,
    max_gap_not_representable,
    flush_threshold_zero,
    flush_interval_negative,
    remote_size_invalid,
    remote_block_count_not_representable
};

struct DownloadPolicyFailure {
    DownloadPolicyErrc reason = DownloadPolicyErrc::none;
    std::error_code error{};
};

template <typename T>
struct DownloadPolicyResult {
    std::optional<T> value;
    DownloadPolicyFailure failure{};

    [[nodiscard]] bool ok() const noexcept {
        return value.has_value() && !failure.error;
    }
};

struct RemoteObjectFacts {
    std::int64_t total_size = 0;
    bool accept_ranges = false;
};

struct SchedulingPolicy {
    std::size_t connection_limit = 0;
    std::int64_t transfer_window_bytes = 0;
    std::int64_t block_bytes = 0;
    bool issue_range_requests = false;
    bool allow_work_stealing = false;
};

struct FlowControlPolicy {
    std::size_t packet_budget = 0;
    std::size_t memory_high_bytes = 0;
    std::size_t memory_low_bytes = 0;
};

struct PersistencePolicy {
    std::size_t block_bytes = 0;
    std::size_t io_alignment_bytes = 0;
    std::int64_t max_gap_bytes = 0;
    std::size_t flush_threshold_bytes = 0;
    std::chrono::milliseconds flush_interval{};
    bool overwrite_existing = true;
};

struct RecoveryIdentityPolicy {
    std::size_t block_bytes = 0;
    std::size_t io_alignment_bytes = 0;
    bool allow_sparse_resume = false;
};

class EffectiveDownloadPolicy;

class ValidatedDownloadPolicy {
public:
    [[nodiscard]] const DownloadOptions& raw_options() const noexcept;

private:
    explicit ValidatedDownloadPolicy(DownloadOptions raw_options) noexcept;

    DownloadOptions raw_options_{};

    friend DownloadPolicyResult<ValidatedDownloadPolicy>
    validate_download_options(const DownloadOptions&) noexcept;

    friend DownloadPolicyResult<EffectiveDownloadPolicy>
    bind_remote_facts(const ValidatedDownloadPolicy&, RemoteObjectFacts) noexcept;
};

class EffectiveDownloadPolicy {
public:
    [[nodiscard]] const DownloadOptions& raw_options() const noexcept;
    [[nodiscard]] const SchedulingPolicy& scheduling() const noexcept;
    [[nodiscard]] const FlowControlPolicy& flow_control() const noexcept;
    [[nodiscard]] const PersistencePolicy& persistence() const noexcept;
    [[nodiscard]] const RecoveryIdentityPolicy& recovery_identity() const noexcept;
    [[nodiscard]] const RemoteObjectFacts& remote_facts() const noexcept;

private:
    EffectiveDownloadPolicy() = default;

    DownloadOptions raw_options_{};
    SchedulingPolicy scheduling_{};
    FlowControlPolicy flow_control_{};
    PersistencePolicy persistence_{};
    RecoveryIdentityPolicy recovery_identity_{};
    RemoteObjectFacts remote_facts_{};

    friend DownloadPolicyResult<EffectiveDownloadPolicy>
    bind_remote_facts(const ValidatedDownloadPolicy&, RemoteObjectFacts) noexcept;
};

[[nodiscard]] DownloadPolicyResult<ValidatedDownloadPolicy>
validate_download_options(const DownloadOptions& options) noexcept;

[[nodiscard]] DownloadPolicyResult<EffectiveDownloadPolicy>
bind_remote_facts(const ValidatedDownloadPolicy& policy,
                  RemoteObjectFacts facts) noexcept;

} // namespace asyncdownload::download
```

Implementation code必须遵守仓库规则，不新增说明性代码注释。上面的代码块是 interface
合同，不要求逐字复制注释。

### 6.1 为什么保留 internal reason

public caller 继续只看到稳定的 `DownloadErrc::invalid_request`。内部
`DownloadPolicyErrc` 用于：

- table-driven unit test 精确指出哪条不变量失败；
- CLI 日后可以在不改变 public error enum 的前提下改善诊断；
- 防止测试仅断言“失败了”而遗漏错误分支。

不要把 `DownloadPolicyErrc` 放进 `include/asyncdownload/`，也不要新增 public error category。

## 7. Raw 与 Effective 构造顺序

### 7.1 Engine 顺序

`DownloadEngine::run` 的目标顺序固定为：

```text
validate request URL/path
    ↓
validate_download_options(request.options)
    ↓ failure: invalid_request, no I/O
initialize curl global
    ↓
HTTP probe
    ↓
bind_remote_facts(validated, {total_size, accept_ranges})
    ↓ failure: probe/invalid-response error, no file/thread/queue
construct SessionState with EffectiveDownloadPolicy
    ↓
load recovery state / open file / scheduler / queue / threads
```

不得先把 options 写入 `SessionState`，再逐字段修正。不得让 CLI 成为唯一 validator。

### 7.2 Pre-probe 规则

`validate_download_options`：

- 深拷贝一份 raw `DownloadOptions`；
- 不修改任何字段；
- 不访问网络、文件或全局状态；
- 第一个失败规则立即返回；
- 成功结果中 `raw_options()` 与调用方输入逐字段相等；
- 对当前 defaults 必须成功。

### 7.3 Post-probe Range 模式

当 `facts.accept_ranges == true`：

| Effective field | Value |
| --- | --- |
| `connection_limit` | raw `max_connections`，不按文件大小自动 clamp |
| `transfer_window_bytes` | `min(raw scheduler_window_bytes, total_size)` |
| `block_bytes` | raw `block_size` 的 checked `int64_t` 值 |
| `issue_range_requests` | `true` |
| `allow_work_stealing` | `max_connections > 1` |
| `packet_budget` | raw `queue_capacity_packets` |
| high/low | raw values |
| persistence fields | raw values |
| `allow_sparse_resume` | `true` |

window 归一化到 total size只消除永远不可到达的尾部；`RangeScheduler::next_window` 当前已经用
range end 做同样的 `min`，因此不会改变合法请求的实际字节范围。

### 7.4 Post-probe 非 Range 模式

当 `facts.accept_ranges == false`：

| Effective field | Value |
| --- | --- |
| `connection_limit` | `1` |
| `transfer_window_bytes` | `facts.total_size` |
| `block_bytes` | raw `block_size` |
| `issue_range_requests` | `false` |
| `allow_work_stealing` | `false` |
| queue/watermark/persistence fields | raw values，不调参 |
| `allow_sparse_resume` | `false` |

raw `max_connections` 和 `scheduler_window_bytes` 仍保存在 `raw_options()` 中，不得覆盖。这样：

- 日志和诊断能区分 requested 与 effective；
- recovery identity 始终使用原始已验证值；
- 后续 module 不会把非 Range 的单连接行为误写回用户配置。

非 Range policy 只声明“不允许 sparse resume”。阶段 1 的 engine migration MUST 在恢复选择
处读取该事实：

- 没有旧恢复状态：执行一次无 `Range` header 的完整 GET；
- 旧状态已经证明整个对象完整：允许直接 finalize；
- 旧状态存在任意未完成洞：不得按洞发多个完整 GET，必须按现有“不能安全恢复则重新开始”
  路径清理旧 metadata 并重新下载；
- 不改变 metadata schema。

阶段 4 会把这条规则移入 Recovery Checkpoint module；在此之前不能留下两个实现来源。

## 8. Validation contract

### 8.1 基础 helper

policy implementation 内部只允许使用不会溢出的 helper：

```cpp
[[nodiscard]] bool is_power_of_two(std::size_t value) noexcept;

template <typename Target, typename Source>
[[nodiscard]] bool can_represent(Source value) noexcept;

[[nodiscard]] bool checked_block_count(std::int64_t total_size,
                                       std::size_t block_size,
                                       std::size_t& result) noexcept;
```

规则：

- `is_power_of_two(0)` 必须为 `false`；
- 禁止用 `(value + divisor - 1) / divisor` 做向上取整；
- block count 使用 `quotient + (remainder != 0 ? 1 : 0)`，且加法 checked；
- scheduler end 使用 remaining-first 算法，不能先计算 `start + window - 1`；
- `2 * block` 或 `2 * window` 判断改写为除法/比较或 checked multiply；
- backpressure 比较改写为
  `incoming > high || current > high - incoming`，先处理 `incoming == 0`；
- 所有 `size_t → int64_t`、`size_t → long` 转换必须在 policy 成功后才执行。

### 8.2 TailBuffer 常量

在 `core/models.hpp` 为现有固定存储引入单一内部常量：

```cpp
inline constexpr std::size_t TAIL_BUFFER_CAPACITY_BYTES = 4096;
```

`TailBuffer::data` 和 `flush_tail` staging array 都使用该常量。policy 的
`io_alignment_exceeds_tail_capacity` 也使用同一常量；禁止在三个位置重复裸写 `4096`。

这不是新增 public 配置。若未来需要大于 4096 的动态 tail，必须作为独立性能和内存设计，
不能在本阶段静默改变 `RangeContext` 的 allocation shape。

### 8.3 失败表

校验按下表顺序执行，使测试和错误 reason 稳定：

| Order | Field/combination | Accepted condition | Failure reason | Public error |
| ---: | --- | --- | --- | --- |
| 1 | `max_connections` | `> 0` | `max_connections_zero` | `invalid_request` |
| 2 | `max_connections` | 可表示为 `long` | `max_connections_not_representable` | `invalid_request` |
| 3 | `queue_capacity_packets` | `> 0` | `queue_capacity_zero` | `invalid_request` |
| 4 | queue count | `<= min(SIZE_MAX, PTRDIFF_MAX)` | `queue_capacity_not_representable` | `invalid_request` |
| 5 | `scheduler_window_bytes` | `> 0` | `scheduler_window_zero` | `invalid_request` |
| 6 | scheduler window | 可表示为正 `int64_t` | `scheduler_window_not_representable` | `invalid_request` |
| 7 | `backpressure_high_bytes` | `> 0` | `backpressure_high_zero` | `invalid_request` |
| 8 | watermark pair | `low <= high` | `backpressure_low_above_high` | `invalid_request` |
| 9 | `block_size` | `> 0` | `block_size_zero` | `invalid_request` |
| 10 | `block_size` | 2 的幂 | `block_size_not_power_of_two` | `invalid_request` |
| 11 | `block_size` | 可表示为正 `int64_t` 且当前平台单次 CRC read length 可表示 | `block_size_not_representable` | `invalid_request` |
| 12 | `io_alignment` | `> 0` | `io_alignment_zero` | `invalid_request` |
| 13 | `io_alignment` | 2 的幂 | `io_alignment_not_power_of_two` | `invalid_request` |
| 14 | `io_alignment` | `<= TAIL_BUFFER_CAPACITY_BYTES` | `io_alignment_exceeds_tail_capacity` | `invalid_request` |
| 15 | block/alignment | `block_size % io_alignment == 0` | `block_size_not_aligned_for_io` | `invalid_request` |
| 16 | `max_gap_bytes` | `> 0` | `max_gap_zero` | `invalid_request` |
| 17 | max gap | 可表示为正 `int64_t` | `max_gap_not_representable` | `invalid_request` |
| 18 | `flush_threshold_bytes` | `> 0` | `flush_threshold_zero` | `invalid_request` |
| 19 | `flush_interval` | `count() >= 0` | `flush_interval_negative` | `invalid_request` |
| bind 1 | `total_size` | `> 0` | `remote_size_invalid` | `http_probe_failed` |
| bind 2 | block count | quotient/remainder 结果可表示为 `size_t` | `remote_block_count_not_representable` | `http_invalid_response` |

Windows 当前单次 CRC read 经 `DWORD` 传长，因此 Windows 的最大合法 `block_size` 是不大于
`DWORD_MAX` 的最大 2 的幂，即 `2^31`。POSIX 可使用其 `ssize_t` 上限。建议在内部 helper
按平台能力计算，不新增魔法配置值。

### 8.4 明确允许的边界

以下组合 MUST 保持合法：

- `backpressure_low_bytes == 0`；
- `backpressure_low_bytes == backpressure_high_bytes`；
- `flush_interval == 0ms`；
- `max_connections == 1`；
- `queue_capacity_packets == 1`；
- `scheduler_window_bytes < block_size`；
- `scheduler_window_bytes > remote total_size`；
- `block_size > remote total_size`；
- `io_alignment < 4096`，只要是 2 的幂且整除 `block_size`；
- `max_gap_bytes < scheduler_window_bytes`；
- Range 模式下 `max_connections` 大于可切出的 Range 数量。

不要增加没有正确性证据的调参约束，例如：

- queue capacity 必须大于 connections；
- high watermark 必须大于一个 packet；
- max gap 必须大于 block 或 window；
- window 必须是 block 的整数倍；
- connections 必须小于固定经验值。

这些约束会改变合法配置行为，且不属于 policy correctness。

### 8.5 Queue contract

本阶段把 `queue_capacity_packets` 定义为 **requested logical packet budget**，保留当前数值。
vendored queue constructor只承诺至少预分配 capacity slots；`try_enqueue(false)` 只说明
no-allocation enqueue 当下失败。阶段 1：

- 验证 count 为正且计数可表示；
- 保持现有三参数 constructor 的 1 explicit / 1 implicit producer shape；
- 保持 packet budget 数值；
- 不根据 vendor `try_enqueue` 结果重写 policy；
- 不声称 vendor queue 是硬容量。

阶段 2 必须通过 Packet Flow interface 统一 logical admission 与 vendor failure。阶段 1
不得抢先改变 pause 次数或 resume 门槛。

## 9. Consumer migration contract

### 9.1 `DownloadEngine`

`DownloadEngine::run`：

- 在 `CurlGlobal` 和 `HttpProbe` 前构造 `ValidatedDownloadPolicy`；
- policy failure 直接写 `result.error` 并返回；
- probe 后构造 `EffectiveDownloadPolicy`；
- `SessionState` 只保存 effective policy，不保存可变 `DownloadOptions options`；
- queue、thread pool、handle count、curl multi limits、水位和 finalize overwrite 都从对应 view
  读取；
- 不再包含 `session.options.max_connections = 1` 或修改 window 的分支。

### 9.2 `RangeScheduler`

构造 interface 改为：

```cpp
RangeScheduler(SchedulingPolicy policy,
               std::int64_t total_size) noexcept;
```

删除 `DownloadOptions options_` 和独立 `accept_ranges_`。scheduler 只知道：

- effective connection limit；
- effective transfer window；
- block bytes；
- 是否发 Range；
- 是否允许 steal。

所有 window end 和 minimum-steal 计算使用 checked/remaining-first 算法。

### 9.3 `PersistenceThread`

`PersistenceThread` 只读取 `PersistencePolicy`。允许 session 继续拥有 policy，并让
persistence 在 thread join 前持有其 `const` reference；禁止复制后再修改。

所有 tail array 使用 `TAIL_BUFFER_CAPACITY_BYTES`。append 前通过 construction invariant
知道 alignment 合法，但 debug build 可保留 assertion；assertion 不能代替入口校验。

### 9.4 Recovery identity

`metadata_matches` 接收 `RecoveryIdentityPolicy`，不再接收完整 `DownloadRequest` 仅为读取
options。比较值保持：

```text
metadata.block_size == recovery_identity.block_bytes
metadata.io_alignment == recovery_identity.io_alignment_bytes
```

URL、paths、size、ETag 和 Last-Modified 的现有比较保持。是否 sparse resume 读取
`allow_sparse_resume`，不根据 mutable session options 猜测。

### 9.5 CLI parser

CLI loader 只负责：

- JSON 解析；
- object shape；
- 字段类型；
- 数值是否可表示为目标 public field；
- defaults/config/positional 的覆盖。

zero、power-of-two、watermark ordering 等语义全部交给 policy module。`parse_connections`
SHOULD 用 `std::from_chars` 替换 exception-based `stoull`，但结果语义和参数格式不变。

若保留 CLI 在 parse 阶段拒绝 0，会形成第二个语义实现来源，因此必须删除该重复判断，并让
engine policy 产生最终失败。CLI process exit code仍为 1。

## 10. Ownership、lifetime 与线程

- caller 拥有原始 `DownloadRequest`；engine 只在同步 `download()` 调用期间读取。
- `ValidatedDownloadPolicy` 是 engine stack value，生命周期覆盖 probe。
- `EffectiveDownloadPolicy` 按值移入 `SessionState`，生命周期覆盖 scheduler、queue、
  persistence thread、flush worker 和 network cleanup。
- 所有 consumer 只能取得 `const` view 或 view 的值副本。
- policy 不包含 raw pointer、reference、mutex、atomic、thread 或 global singleton。
- policy 构造发生在任何 worker thread 启动前；不需要 memory order。
- session 构造完成后不再写 policy，线程只读，不需要额外同步。
- progress callback 不获得 policy reference。
- `DownloadResult` 不新增 requested/effective policy 字段。

## 11. Tiny-commit migration

每个 slice 必须独立 build、test、commit，并能只回滚该 slice。不要一次提交整个阶段。

| Slice | Commit intent | Change | Verification | Slice rollback |
| ---: | --- | --- | --- | --- |
| 1 | `test: characterize download option invariants` | 新增 policy table test 的失败用例和 defaults fixture；生产代码尚未满足时保持明确 red | 只跑新 test，记录 red reason | 删除该 slice 新测试；不动生产代码 |
| 2 | `refactor: add validated download policy module` | 增加两个阶段类型、纯校验、bind 和 unit tests | `DownloadPolicyTest.*` 全绿 | 删除 3 个新文件；无调用方变化 |
| 3 | `fix: reject invalid policy before io` | engine 在 CurlGlobal/probe 前调用 pre-probe validation | invalid URL/options tests；证明错误为 `invalid_request` 而非 HTTP error | 恢复 engine 调用点；policy module仍可独立存在 |
| 4 | `refactor: bind remote facts without mutating options` | probe 后构造 effective policy；删除 non-Range 原地修改 | Range/non-Range policy tests | 恢复 session copy/mutation；不删除 policy tests |
| 5 | `refactor: migrate range scheduler to scheduling policy` | 改 constructor、字段和 overflow-safe arithmetic | scheduler tests + policy edge tests | 恢复旧 constructor；effective policy仍可供其他 caller |
| 6 | `refactor: migrate flow and persistence option reads` | queue、watermark、gap、flush、alignment、overwrite 改读 views；统一 tail constant | persistence tests、memory tests | 按 consumer 回滚字段读取；不回滚 validator |
| 7 | `refactor: migrate recovery identity to effective policy` | metadata match 改读 recovery view，不改变 Range 恢复结果 | recovery tests + legacy fixture | 恢复旧 matcher；metadata schema不变 |
| 8 | `fix: restart sparse state for non-range resources` | 单独实现 non-Range sparse state 的 clean restart；不与 matcher move 混合 | non-Range sparse/complete integration | 回滚该 correctness commit；前 7 个结构 slice 保持可用 |
| 9 | `refactor: remove mutable session options path` | 删除 `SessionState::options` 和所有 raw field interpretation | `rg` deletion checks + full suite | 恢复该字段和最后一批读取；前面 policy可保留 |
| 10 | `refactor: centralize cli option semantics` | CLI 只保留 parse/representation；connections 改无异常解析 | CLI valid/invalid config tests | 恢复 parser semantic checks；library policy不能回滚 |
| 11 | `docs: record policy migration evidence` | 保存功能、Release benchmark 和已知 740 结果 | 阶段 exit checklist | 只回滚 evidence 文档 |

Slice 8 是明确的 **C 类 correctness change**，因为当前 non-Range scheduler 对每个 unfinished
span 返回 whole-object window。它必须保留独立 commit 和前后测试证据，不能伪装成字段移动。

Slice 6 若太大，按 `flow reads`、`persistence reads` 两个 commit拆分；每个 commit仍必须保持
单一 policy 事实来源，不能长期保留旧新分支开关。

## 12. Exact test plan

### 12.1 `tests/download/download_policy_test.cpp`

使用 table-driven tests。每行同时断言 `failure.reason` 和 public `failure.error`。

#### Validated policy

- `DownloadPolicyTest.AcceptsCurrentDefaultsWithoutChangingRawValues`
- `DownloadPolicyTest.AcceptsZeroLowWatermark`
- `DownloadPolicyTest.AcceptsEqualWatermarks`
- `DownloadPolicyTest.AcceptsZeroFlushInterval`
- `DownloadPolicyTest.AcceptsWindowSmallerThanBlock`
- `DownloadPolicyTest.AcceptsPowerOfTwoIoAlignmentBelowTailCapacity`
- `DownloadPolicyTest.RejectsZeroMaxConnections`
- `DownloadPolicyTest.RejectsUnrepresentableMaxConnections`
- `DownloadPolicyTest.RejectsZeroQueueCapacity`
- `DownloadPolicyTest.RejectsUnrepresentableQueueCapacity`
- `DownloadPolicyTest.RejectsZeroSchedulerWindow`
- `DownloadPolicyTest.RejectsUnrepresentableSchedulerWindow`
- `DownloadPolicyTest.RejectsZeroHighWatermark`
- `DownloadPolicyTest.RejectsLowWatermarkAboveHighWatermark`
- `DownloadPolicyTest.RejectsZeroBlockSize`
- `DownloadPolicyTest.RejectsNonPowerOfTwoBlockSize`
- `DownloadPolicyTest.RejectsUnrepresentableBlockSize`
- `DownloadPolicyTest.RejectsZeroIoAlignment`
- `DownloadPolicyTest.RejectsNonPowerOfTwoIoAlignment`
- `DownloadPolicyTest.RejectsIoAlignmentAboveTailCapacity`
- `DownloadPolicyTest.RejectsBlockSizeNotDivisibleByIoAlignment`
- `DownloadPolicyTest.RejectsZeroMaxGap`
- `DownloadPolicyTest.RejectsUnrepresentableMaxGap`
- `DownloadPolicyTest.RejectsZeroFlushThreshold`
- `DownloadPolicyTest.RejectsNegativeFlushInterval`
- `DownloadPolicyTest.ReturnsFirstFailureInDocumentedOrder`

对只在 64-bit 或 Windows 可构造的边界用 compile-time conditional；不得用整数 wrap 构造
测试值。

#### Remote binding

- `DownloadPolicyTest.RejectsNonPositiveRemoteSize`
- `DownloadPolicyTest.RejectsUnrepresentableRemoteBlockCount`
- `DownloadPolicyTest.PreservesRangeConnectionLimit`
- `DownloadPolicyTest.ClampsRangeWindowToRemoteSize`
- `DownloadPolicyTest.EnablesWorkStealingOnlyForMultipleRangeConnections`
- `DownloadPolicyTest.DowngradesNonRangeToOneConnectionAndFullWindow`
- `DownloadPolicyTest.DisablesSparseResumeWithoutRangeSupport`
- `DownloadPolicyTest.PreservesRawOptionsAfterNonRangeDowngrade`
- `DownloadPolicyTest.CopiesFlowAndPersistenceValuesExactly`

### 12.2 Library entry tests

扩展 [`main_test.cpp`](../../../../tests/main_test.cpp)：

- `DownloadClientTest.RejectsInvalidPolicyBeforeHttpProbe`
- `DownloadClientTest.RejectsUnsafeAlignmentBeforeCreatingArtifacts`
- `DownloadClientTest.RejectsInvalidWatermarksBeforeCreatingArtifacts`

fixture 使用非空、不可访问 URL 和唯一 temp output；断言：

- error 是 `invalid_request`；
- `.part` 与 `.config.json` 不存在；
- elapsed time不依赖网络 timeout；不要用脆弱的严格毫秒阈值。

若需要确定性证明“未 probe”，给 `DownloadEngine` 增加的 seam 必须等待阶段 5；本阶段优先通过
错误分类和无 artifact 观察，不能引入只有一个 adapter 的 probe port。

### 12.3 Scheduler arithmetic tests

扩展
[`range_scheduler_test.cpp`](../../../../tests/download/range_scheduler_test.cpp)：

- `RangeSchedulerTest.UsesEffectiveWindowWithoutOverflowAtLargeOffsets`
- `RangeSchedulerTest.DoesNotOverflowWhenBlockIsLargerThanHalfRemaining`
- `RangeSchedulerTest.DoesNotStealWhenEffectivePolicyDisablesStealing`
- `RangeSchedulerTest.NonRangePolicyReturnsSingleFullWindow`
- `RangeSchedulerTest.FinalShortWindowEndsAtObjectBoundary`

测试只通过 `SchedulingPolicy` interface，不再构造完整 `DownloadOptions`。

### 12.4 Persistence safety tests

扩展
[`persistence_thread_test.cpp`](../../../../tests/persistence/persistence_thread_test.cpp)：

- `PersistenceThreadTest.AcceptsValidatedAlignmentAtTailCapacity`
- `PersistenceThreadTest.FlushesFinalTailWithoutWritingPastObjectEnd`
- `PersistenceThreadTest.UsesPolicyBlockAndAlignmentInMetadata`

非法 alignment 不需要穿透到 persistence；其测试属于 policy interface。不要保留“直接给
PersistenceThread 一个非法 SessionState 看是否崩溃”的旧结构测试。

### 12.5 CLI tests

在现有 CLI integration fixture 中增加：

- `DownloadIntegrationTest.RejectsConfigWithInvalidWatermarkOrder`
- `DownloadIntegrationTest.RejectsConfigWithUnsafeIoAlignment`
- `DownloadIntegrationTest.AppliesConnectionsOverrideBeforePolicyValidation`
- `DownloadIntegrationTest.PreservesDirectAndWrappedConfigShapes`

前两个断言 exit code非 0、stderr 包含 invalid request、没有输出 artifact。第三个使用 config
中的合法 connections，再用 positional `0` 覆盖，确认最终合并值被 policy 拒绝。

当前 Windows 环境可能对 CLI process 报 `error=740`。这只能记录为既有环境阻塞；不能删除
library-level policy tests，也不能把 740 当成 invalid-config 测试通过。

### 12.6 Non-Range integration

给 [`range_server.py`](../../../../tests/support/range_server.py) 增加 test-only
`--disable-ranges`：

- HEAD 返回 length 但不返回 `Accept-Ranges: bytes`；
- GET 忽略/拒绝 Range header并返回单个 200 full body；
- request log记录 method、Range header和 client port。

新增：

- `DownloadIntegrationTest.DowngradesNonRangeServerToSingleFullRequest`
- `DownloadIntegrationTest.RestartsSparseResumeStateWhenRangeIsUnavailable`
- `DownloadIntegrationTest.FinalizesAlreadyCompleteStateWithoutRangeSupport`

必须断言：

- 输出字节与 source完全相等；
- effective active connection上限为 1；
- fresh download只出现一次正文 GET；
- 正文 GET 没有 Range header；
- sparse legacy state不会触发多个 full-body GET；
- 已完整且可信的旧状态不被无意义重下；
- metadata 格式没有新字段。

## 13. Acceptance criteria

### 13.1 Static/deletion checks

以下命令结果必须满足：

```powershell
rg -n "session\\.options|options_" src/download src/persistence src/core
rg -n "DownloadOptions" src/download/range_scheduler.* src/persistence
rg -n "max_connections = 1|scheduler_window_bytes =" src/download/download_engine.cpp
rg -n "4096" src/core/models.hpp src/persistence/persistence_thread.cpp
```

期望：

- runtime consumers 不再读 mutable `session.options`；
- `RangeScheduler` 与 `PersistenceThread` 不依赖 `DownloadOptions`；
- engine 不再原地修改 options 做降级；
- tail capacity只有命名常量定义处出现裸值。

保留在 public type、config template、benchmark config 和 docs 中的 `DownloadOptions` 属于兼容
输入，不在删除范围。

### 13.2 Functional gate

```powershell
scripts\build.bat
build\tests\Debug\AsyncDownload_tests.exe --gtest_filter=DownloadPolicyTest.*
build\tests\Debug\AsyncDownload_tests.exe --gtest_filter=DownloadClientTest.*
build\tests\Debug\AsyncDownload_tests.exe --gtest_filter=RangeSchedulerTest.*
build\tests\Debug\AsyncDownload_tests.exe --gtest_filter=PersistenceThreadTest.*
build\tests\Debug\AsyncDownload_tests.exe
scripts\build.bat release
ctest --test-dir build -C Release --output-on-failure
```

通过标准：

- Debug/Release build成功；
- 新 policy tests全部通过；
- 先前通过的测试继续通过；
- 已知 740 失败集合不扩大，失败原因不变；
- public headers没有新增 internal include；
- default options 的 effective Range 行为与基线相同；
- non-Range fresh、sparse restart和complete finalize tests通过；
- legacy metadata fixture仍可加载和恢复；
- invalid policy不创建 artifact、不发起可观察网络下载；
- 所有 error通过返回值表达，无新异常穿透。

### 13.3 Public compatibility compile fixture

增加一个编译期 fixture，保留既有 aggregate shape：

```cpp
const asyncdownload::DownloadOptions options{
    4,
    4096,
    4 * 1024 * 1024,
    256 * 1024 * 1024,
    128 * 1024 * 1024,
    64 * 1024,
    4 * 1024,
    32 * 1024 * 1024,
    16 * 1024 * 1024,
    std::chrono::milliseconds(2000),
    true
};
```

不要把这个 fixture 变成字段值的唯一默认测试；另有逐字段 defaults assertion。

## 14. Performance neutrality gate

本阶段不是性能优化。以下运行语义必须保持：

- Range 模式 raw connection limit不变；
- 实际 transfer window不变；
- queue capacity数值和 vendor constructor shape不变；
- 64 KiB packet aggregation不变；
- high/low thresholds 和 pause/resume comparisons的合法输入结果不变；
- flush threshold/interval不变；
- TailBuffer allocation shape不变；
- distinct physical connection现有意图不变；
- telemetry与正式 summary schema不变。

按
[`performance_playbook_zh.md`](../../../performance/performance_playbook_zh.md)
和阶段 0 的命令执行 Release `regression_v2` 前后各 20 次，同机、同对象、同 server、同 case。

阻断 gate：

| Signal | Gate |
| --- | --- |
| `baseline_default` network/disk median | 任一下降不得超过 5% |
| `balanced_candidate` network/disk median | 任一下降不得超过 5% |
| `memory_guard` peak | 应保持既有约 4.2 MiB 形态，偏移需解释 |
| multi-case pause count | 约 15% 物质回归必须调查 |
| packet metrics | total/average/max 的语义和 key 不变 |
| profiler | 只解释热点迁移，不能豁免 benchmark 失败 |

若 policy accessor 出现在 profiler 热点，不得缓存一个可变 `DownloadOptions` 绕过 interface；
优先让 consumer 构造时复制小 view。不得在本阶段顺便重开性能历史已拒绝的 queue resume、
connection reuse、incremental CRC 或 metadata compact 实验。

## 15. Rollback strategy

### 15.1 逐 slice 回滚

使用第 11 节每个 slice 的 commit作为回滚单位。任何 slice失败：

1. 保存该 slice 前后测试与 benchmark evidence；
2. 只回滚该 slice；
3. 确认上一 slice的 tests仍绿；
4. 不调整 defaults掩盖回归；
5. 把未解决事实记录在本阶段 evidence，再决定是否重做。

### 15.2 整阶段回滚

整阶段回滚顺序与迁移相反：

1. 恢复 CLI semantic checks；
2. 恢复 `SessionState::options` 和 consumer reads；
3. 恢复 recovery matcher；
4. 恢复 persistence、flow、scheduler constructors；
5. 恢复 engine 原地 non-Range 降级；
6. 移除 policy module；
7. active suite 只保留旧实现能通过的 characterization；仅新 correctness 才能通过的 tests
   随对应 C commit 回滚或显式禁用，其用例合同与 red 输出保留在 evidence 和本文件。

metadata 没有格式迁移，因此无需数据回滚。若实现出现 metadata diff，说明超出本阶段 scope，
不得通过编写迁移脚本补救，应回滚该 diff。整阶段回滚后的 baseline 不得保持 red。

### 15.3 Runtime fallback 禁止项

不要增加长期 feature flag在 old/new policy path之间切换。两条 interpretation path会重新制造
本阶段要消除的重复事实。回滚依赖小 commit，而不是运行期双轨。

## 16. 删除清单

本阶段完成时删除：

- `SessionState::options` 可变 `DownloadOptions` 字段；
- engine 中 non-Range 时修改 options 的两条赋值；
- `RangeScheduler::options_`；
- `RangeScheduler::accept_ranges_`；
- consumer 内所有 `size_t → int64_t/long` 的 unchecked cast；
- CLI `read_size_field(..., allow_zero)` 中的字段语义参数；
- CLI connections parser中的 zero 语义判断；
- tail 路径重复裸写的 `4096`；
- 只证明调用方可以构造非法 internal state 的新旧测试。

本阶段明确不删除：

- public `DownloadOptions`；
- config template字段；
- metadata 的 `block_size`、`io_alignment`；
- RangeScheduler 行为测试；
- PersistenceThread 行为测试；
- performance script中的 raw option fields。

## 17. Risks 与尚未确认的事实

### 17.1 已关闭风险

- `io_alignment > 4096`：本阶段以确定性 rejection关闭，不动态扩容。
- 非 power-of-two block alignment：入口拒绝，不再让 bit-mask helper猜测。
- raw/effective 混淆：两个类型关闭 partial state。
- low > high：入口拒绝，避免即时恢复或 thrash。
- scheduler/bitmap overflow：改成 checked/remaining-first arithmetic。
- 非 Range sparse holes：policy显式禁止 sparse resume，engine执行 clean restart。

### 17.2 实现时必须验证，但不应擅自扩 scope

- vendored queue的预分配公式与阶段 2 logical budget仍有语义差异；阶段 1只保证可表示。
- Windows `DWORD` 单次 CRC read限制可以在阶段 4通过 chunked read解除；解除前 policy上限保留。
- test server当前没有 non-Range模式；必须用真实 HTTP fixture验证，不能只测 policy bool。
- CLI process在当前环境有 740；需要正常 process launch环境补证。
- `accept_ranges` 是 probe得到的保守事实；Range-ignored response的运行时降级由阶段 5设计，
  本阶段不自动 retry。
- 公开 config当前对 unknown字段宽松；是否 strict不在当前 destination。
- equal watermarks与 zero flush interval虽性能上激进，但当前语义可执行，故保持合法。

若实现中发现必须改变 public字段、metadata格式、默认值或 Range请求语义才能完成阶段，停止该
slice并回到 Wayfinder map；不要由 Code Agent自行决定。

## 18. Code Agent exit checklist

- [ ] 记录实际 base commit和工作树已有改动。
- [ ] 阶段 0 policy特征化已完成。
- [ ] `DownloadOptions` public shape与 defaults未变。
- [ ] 两阶段 policy interface已实现且只在 `src/` 内。
- [ ] 所有跨字段规则只存在于 policy implementation。
- [ ] invalid policy在任何 I/O前返回 `invalid_request`。
- [ ] raw options在 non-Range bind后逐字段不变。
- [ ] non-Range effective policy为 1 connection、full window、无 steal、无 sparse resume。
- [ ] Range effective policy保持现有合法配置行为。
- [ ] scheduler、flow、persistence、recovery只读各自 view。
- [ ] fixed TailBuffer容量只有一个常量来源。
- [ ] unchecked乘加和 narrowing cast已删除。
- [ ] metadata格式和 recovery identity值未变。
- [ ] Debug、Release、完整 tests和 non-Range integration已执行。
- [ ] 已知 740未被误报为业务通过。
- [ ] Release benchmark gate通过，或当前 slice已回滚。
- [ ] deletion checks通过。
- [ ] 每个 slice都有独立 commit、evidence和回滚点。

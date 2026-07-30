# 阶段 4：Recovery Checkpoint

## 1. Outcome

本阶段把 `.part + .config.json + bitmap + VDL + CRC32` 的恢复协议收进一个深模块
`asyncdownload::recovery::RecoveryCheckpoint`。

阶段完成后：

- `DownloadEngine` 不再解释 metadata 身份、恢复位图、VDL、CRC 或清理顺序；
- `PersistenceThread` 不再直接持有 `FileWriter` 和 `MetadataStore`；
- `RecoveryCheckpoint` 独占 part file、metadata store 和 checkpoint 提交协议；
- Phase 3 的 `RangeWriteState` 仍是持久化前沿的唯一运行时事实来源；
- 每次提交只使用一个冻结的 checkpoint image，不在 worker 中读取 live bitmap、
  `RangeLifecycle` 或 `RangeWriteState`；
- periodic commit 的顺序固定为
  `freeze -> part flush -> CRC reads -> metadata replace -> publish committed VDL`；
- successful finalize 的顺序固定为
  `final checkpoint -> final part flush -> promote output -> classify metadata cleanup`；
- 有效的旧 `.config.json` 仍可读取，字段名、值类型、pretty JSON 和路径规则不变；
- 非 Range 资源只允许“完整 checkpoint 直接 finalize”或“丢弃稀疏状态后完整重下”；
- 下载失败继续保留可恢复产物；
- 已经成功提升的正式输出不会因为 metadata 清理失败而被报告为下载失败。

本阶段依赖
[阶段 0](00_characterization_baseline.md)、
[阶段 1](01_validated_download_policy.md)、
[阶段 2](02_packet_flow_backpressure.md) 和
[阶段 3](03_range_lifecycle.md)。Code Agent MUST 按顺序完成前置阶段，不能在旧
`RangeContext` 多写者结构上直接引入本模块。

## 2. Compatibility and scope

### 2.1 MUST 保持

- 临时内容路径仍为 `<output>.part`。
- metadata 路径仍为 `<output>.config.json`。
- metadata 临时写路径仍可使用 `<output>.config.json.tmp`。
- metadata 的现有 JSON 字段全部保留，不增加必填 version。
- `bitmap_states` 的编码仍为 `empty = 0`、`downloading = 1`、`finished = 2`。
- VDL 仍表示从文件头开始连续安全落盘的排他前沿。
- VDL 之后标成 finished 的 block 仍通过 CRC32 样本验证。
- transient `downloading` block 在恢复时回到 `empty`。
- 有效 legacy range 的 `start_offset/persisted_offset` 仍可补建 finished bitmap。
- URL、paths、size、block size、I/O alignment、ETag 和 Last-Modified 的身份规则保持。
- metadata 的未知字段继续忽略，缺失数组继续按空数组读取。
- `DownloadRequest`、`DownloadOptions`、`DownloadResult`、CLI 和正式 10 项 Performance
  Summary 不变。
- flush byte threshold、flush interval、final forced flush 和“同一时刻最多一个提交”
  的 cadence 不变。
- 网络失败、HTTP 失败、Persistence 失败和取消继续保留 `.part` 与最近一次成功 metadata。
- 所有错误通过返回值或 `std::error_code` 表达，不向调用方抛异常。

### 2.2 本阶段明确不做

- 不改变 block size、I/O alignment、flush threshold、flush interval 或默认值。
- 不增加自动 retry、checkpoint retry loop 或后台清理线程。
- 不增加 metadata version、二进制格式、journal、数据库或 mmap 格式。
- 不把 JSON 改成 compact output；继续使用 `dump(2)` 的可读格式。
- 不引入 incremental CRC cache，也不在写入路径计算可复用 CRC。
- 不拆分独立 CRC read handle、flush handle 或 write handle。
- 不用简单的多句柄拆分规避 `FileWriter` 的串行化合同。
- 不改变 packet aggregation、queue/backpressure、Range steal 或 HTTP 行为。
- 不把 `RangeLifecycle` 快照当成持久化事实来源。
- 不让测试用通用 `IFileSystem` mock 取代真实临时文件系统。

### 2.3 变更分类

本阶段区分两类提交：

| Class | Meaning | Merge rule |
| --- | --- | --- |
| S | 只移动职责、interface 或所有权的结构提交 | 必须保持既有成功和失败语义 |
| C | 修复 crash/error/malformed artifact 行为的 correctness 提交 | 单独 commit、先写失败测试、单独 rollback |

冻结 checkpoint image、安全失效旧 metadata、CRC read 失败传播和原子替换都属于 C 类。
Code Agent 不得把它们伪装成 rename、move 或 constructor migration。

## 3. 已核实的当前事实

以下结论来自当前基线源码；不是目标设计的假设。

| Current location | Verified fact | Consequence |
| --- | --- | --- |
| `DownloadEngine::metadata_matches()` | URL、output path、part path、total size、block size 和 alignment 必须相同 | 身份规则散在 engine |
| 同函数 | ETag/Last-Modified 只有双方都非空且不同时才 mismatch | 缺失 validator 的 legacy metadata 仍可恢复 |
| `DownloadEngine::run()` | 先调用 `MetadataStore::load()`，之后才检查 `.part` 是否存在 | orphan metadata 即使没有 `.part` 也可能因 parse error 阻塞任务 |
| 同函数 | `can_resume` 要求 `.part` 存在、metadata 存在且身份匹配 | 单独 `.part` 从不被信任 |
| 同函数 | fresh path 先 `FileWriter::open(..., resume=false)`，之后忽略 `remove()` 结果 | crash 可落在“part 已重置、旧 metadata 仍有效”的危险窗口 |
| `rebuild_bitmap_from_snapshots()` | 只读取 legacy range 的 `start_offset/persisted_offset` | `end/current/status` 不参与当前恢复判定 |
| `validate_resumed_blocks()` | VDL 之前的 finished block 不读 CRC | VDL 必须永远不能超报 |
| 同函数 | VDL 之后缺少 CRC sample 的 finished block 回到 `empty` | 缺样本是安全回退，不是信任 |
| 同函数 | 恢复阶段 CRC read 失败会返回 `file_read_failed` | 当前 load/validate 路径不会静默跳过读错误 |
| `PersistenceThread::build_crc_samples()` | checkpoint 提交阶段 CRC read 失败被 `continue` 跳过 | 提交阶段读错误被隐藏，保存的 checkpoint 退化 |
| `PersistenceThread::build_metadata_state()` | 在 Persistence 线程复制 bitmap/range 快照 | snapshot 起点来自正确的单 writer |
| `maybe_schedule_flush()` worker | `flush()` 后从 live `bitmap_` 重算 VDL | worker 可观察到 snapshot 之后的新状态 |
| 同 worker | CRC 使用旧 snapshot bitmap，但 VDL 使用 live bitmap | 同一 metadata 的 bitmap、VDL、CRC 可能不属于同一版本 |
| `poll_pending_flush()` | save 成功后再次从 live bitmap 计算 session VDL | progress VDL 可能不等于刚保存的 metadata VDL |
| `MetadataStore::save()` | 写 `.tmp` 后先 `remove(path)`，再 `rename(tmp, path)` | old metadata 删除后存在无正式 metadata 的 crash window |
| 同函数 | `ofstream::close()` 的失败未检查 | “tmp 写完”没有完整错误确认 |
| 同函数 | JSON 使用 `value.dump(2)` | compact JSON 已被性能历史否决 |
| `MetadataStore::remove()` | 无论 filesystem remove 是否失败都返回 success | caller 无法区分 removed、not found 和 failed |
| `finalize_storage_phase()` | output promotion 成功后忽略 metadata remove 结果 | 完成结果不因清理失败而失败，但错误完全不可观察 |
| resume-complete fast path | `safe_vdl >= total_size` 时直接 finalize，再忽略 remove 结果 | fast path 与 normal path 重复相同协议 |
| `FileWriter::finalize()` | 先 flush、close；overwrite 时先 remove output，再 rename part | overwrite promotion 有 output name gap |
| `FileWriter` | write/read/flush/finalize 共用一把 mutex 和同一文件句柄 | current serialization 是恢复正确性与性能的共同事实 |
| `AtomicBlockBitmap::restore()` | 只恢复 `min(required, serialized)` 个字节 | 短 bitmap 的剩余 block 保持 empty |
| `AtomicBlockBitmap::reset_transient_states()` | 所有 downloading 变为 empty | transient 状态从不跨进程可信 |
| Phase 3 `RangeWriteState` | 保存 range geometry、persisted frontier、tail、reorder 和 completion | checkpoint range 前沿应从这里冻结 |
| Phase 3 ordered effects/packets | Register/Resize 在 arm 前应用并 success-ACK；Packet Flow 保证 `Data... -> RangeComplete` | Persistence 可在本地维护 legacy range 投影 |

### 3.1 当前安全缺口

必须在实现时分别建立失败测试：

1. **live VDL overclaim**：worker 在 `flush()` 返回后读取 live bitmap；下一批尚未 flush 的
   写入可能被计入 VDL。
2. **fresh restart stale-pair**：part 被重置后、旧 metadata 删除前 crash；下次启动可能把
   旧 VDL 应用到新 part。
3. **replace gap**：old metadata remove 成功、tmp rename 前 crash；只剩 tmp。
4. **hidden CRC read error**：sample read 失败仍保存缺少样本的新 checkpoint。
5. **hidden cleanup error**：正式输出已经存在，但 metadata remove 失败没有结构化结果。

这些是 observation-level 以上的实现事实，均可映射到具体分支和操作顺序。修复必须是 C 类
提交。

## 4. Durable Checkpoint invariants

### 4.1 信任不变量

对任意成功提交的 checkpoint：

```text
0 <= VDL <= total_size
VDL == contiguous_finished_bytes(frozen_bitmap)
all block offsets < VDL are represented as FINISHED in frozen_bitmap
each FINISHED block offset >= VDL has exactly one CRC sample
sample.length == min(block_size, total_size - sample.offset)
sample.crc32 == CRC32(part bytes after successful part flush)
range.persisted_through never exceeds range.bytes.end
```

恢复后：

```text
trusted_bitmap =
    reset_transient(
        restore(serialized_bitmap)
        + legacy persisted range projection)

for each FINISHED block at or after serialized VDL:
    missing sample -> EMPTY
    CRC mismatch -> EMPTY
    read error -> recovery error

safe_vdl = contiguous_finished_bytes(trusted_bitmap)
```

`safe_vdl` 可以小于 serialized VDL，只能发生在 malformed metadata 的 C 类防御路径；对合法
checkpoint 两者一致。CRC 通过后，VDL 之后的连续 finished block 可以让 `safe_vdl`
向前推进。

### 4.2 提交不变量

- checkpoint image 在 Persistence 线程冻结。
- image 冻结之后不可修改，不含 atomic、pointer 或 borrowed span。
- worker 只读 frozen image。
- part flush 成功前不得生成或替换新 metadata。
- CRC 只读取 frozen bitmap 中要求采样的 block。
- 任一要求的 CRC read 失败时不得替换 old metadata。
- metadata replace 成功前不得发布 committed VDL。
- 发布值必须是该 frozen image 的 VDL，不能重新读取 live bitmap。
- 一个 Session 同时最多有一个 pending commit。
- failed commit 不推进 committed generation 或 public progress VDL。

### 4.3 完成不变量

- Range `finished` 只代表 Phase 3 Persistence 已确认全部字节写入，不代表独立 durability。
- task finalize 必须等待最后一个 forced checkpoint 成功。
- `RecoveryCheckpoint::finalize()` 只接受内部记录的 committed VDL 等于 total size。
- output promotion 成功是主完成事实。
- promotion 后 metadata cleanup 失败是 cleanup failure，不得覆盖主完成事实。

## 5. Module designs considered

### 5.1 Design A：borrowed coordinator

形状：

```text
DownloadEngine owns FileWriter + MetadataStore
PersistenceThread writes FileWriter directly
RecoveryCoordinator borrows both for load/commit/finalize
```

优点：

- 迁移改动小；
- 现有 leaf classes 基本不动。

拒绝原因：

- callers 仍可绕过 coordinator 调用 `flush/save/remove/finalize`；
- 删除 coordinator 后，只需把顺序代码移回 engine/persistence，模块没有足够 leverage；
- FileWriter/MetadataStore 的 lifetime 和错误组合仍由 callers 学习；
- interface 不能保证“part flush 成功后才能替换 metadata”。

这是 shallow module。

### 5.2 Design B：pure state machine + filesystem ports

形状：

```text
RecoveryStateMachine
    -> IPartFile
    -> IMetadataStore
    -> IFileSystem
```

优点：

- 纯状态转换容易做 table-driven tests；
- 每个 I/O 步骤都可 mock。

拒绝原因：

- 本地 filesystem 是 local-substitutable dependency，真实临时目录测试更接近合同；
- 三个通用 interface 只各有一个 production implementation；
- mock 容易证明“调用了 rename”，却不能证明真实 rename、sharing、flush 和 path 行为；
- 状态机与 I/O owner 分离后，仍需另一层对象维持 lifetime 和 crash ordering。

不建立通用 `IFileSystem` seam。

### 5.3 Design C：owning Recovery Checkpoint

形状：

```text
DownloadEngine unique-owns RecoveryCheckpoint
PersistenceThread borrows RecoveryCheckpoint
RecoveryCheckpoint private-owns FileWriter + MetadataStore
```

选择本设计，原因：

- external interface 只表达 open/write/prepare/commit/finalize/close；
- part、metadata、identity、CRC 和 cleanup 的组合规则都隐藏在 implementation；
- Persistence 仍决定 cadence，但不能自行执行协议步骤；
- 删除 module 会让身份、可信度、flush/CRC/save、finalize/cleanup 再次散回多个 caller；
- temp filesystem tests 可直接覆盖真实路径和 artifact 状态；
- 只在无法由正常 filesystem 稳定触发的错误点使用 private fault seam。

## 6. Selected dependency direction

```text
download::EffectiveDownloadPolicy
range::RangeWriteState snapshots
        │
        ▼
recovery::RecoveryCheckpoint
        ├── private owns ──► storage::FileWriter
        ├── private owns ──► metadata::MetadataStore / JSON codec
        ├── uses ──────────► core::crc32
        └── uses ──────────► local filesystem

DownloadEngine ── unique owner
PersistenceThread ── bounded lifetime borrower
BS thread-pool task ── bounded lifetime borrower during one commit
```

Dependency categories：

| Dependency | Category | Rule |
| --- | --- | --- |
| Effective policy / identity values | in-process | 按值或 const view |
| Range checkpoint facts | in-process | Persistence 线程冻结为值 |
| `FileWriter` | module-private local implementation | caller 不可直接访问 |
| `MetadataStore` / JSON codec | module-private local implementation | caller 不可直接访问 |
| local filesystem | local-substitutable | 首选真实 temp filesystem |
| CRC32 | in-process pure function | 不增加 interface |
| thread pool | in-process executor owned elsewhere | 不把 BS 类型暴露到 recovery interface |

`RecoveryCheckpoint` MUST own `FileWriter` 和 `MetadataStore`。若仍由 engine 或 persistence
分别持有，阶段目标未完成。

## 7. Exact target types and signatures

精确文件名可按 CMake 调整，但角色、值语义和错误合同不得改变。

```cpp
namespace asyncdownload::recovery {

using CheckpointGeneration = std::uint64_t;

enum class RecoveryDisposition : std::uint8_t {
    fresh = 0,
    resumed = 1,
    complete = 2
};

enum class CleanupStatus : std::uint8_t {
    removed = 0,
    not_found = 1,
    failed = 2
};

enum class DiscardReason : std::uint8_t {
    identity_mismatch = 0,
    orphan_artifact = 1,
    non_range_partial = 2
};

struct RemoteRecoveryIdentity {
    std::string url;
    std::int64_t total_size = 0;
    bool accept_ranges = false;
    std::string etag;
    std::string last_modified;
};

struct RecoveryOpenRequest {
    core::SessionPaths paths;
    RemoteRecoveryIdentity remote;
    download::RecoveryIdentityPolicy policy;
    bool overwrite_existing = false;
};

struct RecoveryRangeFact {
    range::RangeId id{};
    range::ByteSpan bytes{};
    range::ByteOffset dispatch_cursor = 0;
    range::ByteOffset persisted_through = 0;
    std::uint8_t legacy_status = 0;
};

struct RestoredCheckpoint {
    RecoveryDisposition disposition = RecoveryDisposition::fresh;
    std::vector<std::uint8_t> bitmap_states;
    std::int64_t trusted_bytes = 0;
    std::int64_t safe_vdl = 0;
    std::size_t completed_ranges = 0;
};

class PreparedCheckpoint {
public:
    PreparedCheckpoint(PreparedCheckpoint&&) noexcept;
    PreparedCheckpoint& operator=(PreparedCheckpoint&&) noexcept;
    ~PreparedCheckpoint();

    PreparedCheckpoint(const PreparedCheckpoint&) = delete;
    PreparedCheckpoint& operator=(const PreparedCheckpoint&) = delete;

private:
    class Implementation;
    explicit PreparedCheckpoint(
        std::unique_ptr<Implementation> implementation) noexcept;
    std::unique_ptr<Implementation> implementation_;
    friend class RecoveryCheckpoint;
};

struct PrepareCheckpointResult {
    std::unique_ptr<PreparedCheckpoint> checkpoint;
    std::error_code error;
};

struct CheckpointCommitResult {
    CheckpointGeneration generation = 0;
    std::int64_t committed_vdl = 0;
    std::error_code error;
};

struct CleanupResult {
    CleanupStatus status = CleanupStatus::not_found;
    std::error_code error;
};

struct FinalizeResult {
    bool output_available = false;
    CleanupResult metadata_cleanup;
    std::error_code error;
};

class RecoveryCheckpoint;

struct RecoveryOpenResult {
    std::unique_ptr<RecoveryCheckpoint> checkpoint;
    RestoredCheckpoint restored;
    CleanupResult stale_cleanup;
    std::error_code error;
};

class RecoveryCheckpoint {
public:
    [[nodiscard]] static RecoveryOpenResult open(
        const RecoveryOpenRequest& request) noexcept;

    ~RecoveryCheckpoint();

    RecoveryCheckpoint(const RecoveryCheckpoint&) = delete;
    RecoveryCheckpoint& operator=(const RecoveryCheckpoint&) = delete;

    [[nodiscard]] std::error_code write(
        std::int64_t offset,
        std::span<const std::uint8_t> bytes) noexcept;

    [[nodiscard]] PrepareCheckpointResult prepare(
        std::span<const std::uint8_t> bitmap_states,
        std::span<const RecoveryRangeFact> ranges) noexcept;

    [[nodiscard]] CheckpointCommitResult commit(
        std::unique_ptr<PreparedCheckpoint> checkpoint) noexcept;

    [[nodiscard]] FinalizeResult finalize() noexcept;

    void close_preserving_artifacts() noexcept;

    [[nodiscard]] const core::SessionPaths& paths() const noexcept;

private:
    class Implementation;

    explicit RecoveryCheckpoint(
        std::unique_ptr<Implementation> implementation) noexcept;

    [[nodiscard]] CleanupResult discard_candidate(
        DiscardReason reason) noexcept;

    std::unique_ptr<Implementation> implementation_;
};

}
```

### 7.1 Signature rules

- `open()` catches allocation、JSON 和 filesystem exceptions，并映射为 `std::error_code`。
- `open().checkpoint == nullptr` 当且仅当 `error` 非空。
- `PreparedCheckpoint` 完全拥有 identity、bitmap、range projection 和 metadata candidate。
- `prepare()` 在 Persistence 线程执行；它不得启动 I/O 或 worker。
- `commit()` 在一个 worker 上同步执行；异步由 `PersistenceThread` 的现有 future 管理。
- `commit()` 不接收 bitmap、Range pointer、Session pointer 或 callback。
- `write()` 是 Persistence Writer 的唯一 part write seam。
- `finalize()` 不接收 caller-provided “complete=true”；它校验内部 last committed VDL。
- `RestoredCheckpoint::completed_ranges` 只用于未创建 Lifecycle 的
  `RecoveryDisposition::complete` fast path：此时必须等于 validated required block count；
  `fresh`/`resumed` 时为 `0`，正常执行结果仍由 Phase 3 Lifecycle 生成。
- `close_preserving_artifacts()` 幂等，只关闭 handle，不删除任何恢复 artifact。
- production interface 不暴露 `FileWriter`、`MetadataStore`、nlohmann JSON 或 BS thread pool。

若 Phase 1 的 policy 类型最终名称不同，可以机械改名；必须保留 raw identity fields 与
`allow_sparse_resume` 的含义。

### 7.2 Restored progress handoff

`RestoredCheckpoint::trusted_bytes` 既是恢复下载基数，也是持久化绝对计数的初始化值：

- `open()` 已保证它非负、可表示且不超过 total size；
- 对 `fresh`/`resumed`，Orchestrator 在启动 Persistence Writer 前 checked 转换并设置
  `initial_persisted_bytes = trusted_bytes`；fresh 值为 0；
- Writer 后续每次成功物理写入都 checked 增加该 absolute counter；
- `RecoveryDisposition::complete` 不启动 Writer，result/progress 直接使用同一个 validated
  trusted base；
- 恢复基数不是本次 task 的物理写入，不调用 `record_persist_delta()`，不增加 disk
  telemetry；
- 初始化或转换失败必须在 Persistence thread 启动前返回 internal error，不得退回 0
  继续。

这样 resume 的 absolute downloaded/persisted 都包含相同可信基数，既不会把旧字节误算为
inflight，也不会把它们计入本次 network/disk throughput。

## 8. Open, load, identity and validate semantics

### 8.1 Artifact classification

`open()` 先取得不修改 filesystem 的 artifact inventory：

| `.part` | `.config.json` | Classification |
| --- | --- | --- |
| absent | absent | fresh |
| present | absent | untrusted part |
| absent | present | orphan metadata |
| present | present | candidate pair |

inventory 只检查存在性和明确的 filesystem error。`exists(path, ec)` 的 `ec` 不得被解释为
absent。

### 8.2 Load order

目标顺序：

1. validate `RecoveryOpenRequest` 的内部前置条件；
2. inventory `.part` 与 `.config.json`；
3. 只有 candidate pair 才解析 metadata 作为恢复候选；
4. candidate parse error 返回 `metadata_parse_failed`，不修改 artifacts；
5. identity mismatch 进入 required discard；
6. identity match 才以 resume mode 打开 part；
7. 恢复 bitmap、reset transient、投影 legacy ranges、CRC validate；
8. 根据 `allow_sparse_resume` 和 safe VDL 选择 resumed、complete 或 fresh restart。

orphan metadata 不应因为 JSON 损坏阻止 fresh task；这是 C 类 cleanup hardening。结构迁移
提交先保持现状，随后由独立测试证明该规则。

### 8.3 Identity matrix

比较不做 path canonicalization、URL normalization 或 header normalization，以保持 legacy
行为。

| Field | Current-compatible match rule | Missing legacy value |
| --- | --- | --- |
| `url` | exact string equality | mismatch |
| `output_path` | exact `filesystem::path` equality | mismatch |
| `temporary_path` | exact `filesystem::path` equality | mismatch |
| `total_size` | exact equality | mismatch |
| `block_size` | exact raw validated option | mismatch |
| `io_alignment` | exact raw validated option | mismatch |
| `etag` | mismatch only when both sides non-empty and unequal | accepted |
| `last_modified` | mismatch only when both sides non-empty and unequal | accepted |
| `accept_ranges` | serialized but not identity | ignored |
| `resumed` | serialized history flag | ignored |
| connection/window/queue/flush fields | not in schema | ignored |

remote validator 缺失意味着“没有冲突证据”，不是“内容已经证明相同”。本阶段保留该容错策略，
不自行升级为强 validator requirement。

### 8.4 Structural validation

合法 legacy metadata 必须继续加载。malformed-state 防御作为独立 C 类提交，至少检查：

- `total_size > 0` 且等于 remote；
- `0 <= vdl_offset <= total_size`；
- VDL 是 block 边界或 `total_size`；
- `bitmap_states.size() <= required_block_count`，短 bitmap 允许；
- bitmap 每个值属于 `0..2`；
- range 满足
  `0 <= start <= persisted <= current <= end + 1 <= total_size`；
- range 之间不借 persisted projection 制造 overlap；
- CRC offset 是合法 block 起点；
- CRC length 等于该 block 的 expected length；
- 同一 offset 最多一个 CRC sample；
- integer multiplication/addition 使用 checked arithmetic。

serialized range projection 应用到 candidate bitmap 后、任何 CRC 信任判断前，必须额外验证：
`[0, serialized_vdl)` 覆盖的每个 required block 都处于 finished。因为 VDL 本身声称这一
连续前缀已经可信，前缀中出现 empty/downloading/hole 是 metadata 自相矛盾；此时返回
`metadata_parse_failed` 且不 reset、remove 或 rewrite 任何 artifact。不得只把
`safe_vdl` 回退后继续无 CRC 地信任旧 VDL 内 hole 之后的 finished blocks。

如果旧的有效 fixture 不满足某条规则，Code Agent 必须先保存 fixture 和当前消费证据，再调整
规则；不能直接把有效用户 artifact 判坏。

### 8.5 Bitmap reconstruction

恢复候选的确定性顺序：

1. 创建 required block count 的全 empty bitmap；
2. 复制 serialized bitmap 的可用前缀；
3. 把所有 downloading 变为 empty；
4. 对每个合法 legacy range 的 `[start_offset, persisted_offset)` 调用 finished projection；
5. 验证 `[0, serialized_vdl)` 的全部 required blocks 均为 finished；任一 hole 按
   structural validation error 结束，artifacts 原样保留；
6. 对每个 finished block：
   - block offset `< serialized_vdl`：保留；
   - offset `>= serialized_vdl` 且缺 sample：改为 empty；
   - sample length 非 expected length：metadata parse/validation error；
   - CRC mismatch：改为 empty；
   - file read error：返回 `file_read_failed`，保留 artifacts；
7. 重新计算 `trusted_bytes` 与 `safe_vdl`。

`trusted_bytes` 是所有 trusted finished blocks 的有效字节和；`safe_vdl` 是连续前缀。两者不能
混用。

### 8.6 Resume selection

| Condition | Result |
| --- | --- |
| no trusted candidate | fresh |
| identity mismatch | required discard, then fresh |
| Range-capable and safe state incomplete | resumed |
| any mode and `safe_vdl == total_size` | complete |
| non-Range and any incomplete/holey state | required discard, then fresh |

non-Range fresh restart 规则从 Phase 1 移入本 module 后，Phase 1 engine adapter 中的临时实现
必须删除，不能留下两个事实来源。

## 9. Source of range snapshots after Phase 3

checkpoint 不能调用 `RangeLifecycle::snapshot()`，原因是：

- Lifecycle 由 Orchestrator thread 独占；
- commit cadence 由 Persistence thread 触发；
- worker 不能跨线程读取 Lifecycle；
- Lifecycle 的 `dispatch_cursor/phase` 不是 durable frontier。

目标 source 是 Persistence thread 内的 `RangeWriteState`，并结合 Phase 3 在 arm 前已经
应用并 success-ACK 的 registration effects 与按 FIFO 消费的 Data/RangeComplete packets：

| Legacy field | Phase 3 source | Durability role |
| --- | --- | --- |
| `range_id` | `RangeWriteState.id` | correlation only |
| `start_offset` | `RangeWriteState.bytes.begin` | persisted projection |
| `end_offset` | `RangeWriteState.bytes.end - 1` | compatibility |
| `current_offset` | `max(observed_dispatch_through, persisted_through)`；前者来自已消费 packet 的 `lease_span.end` / completion `expected_end` | compatibility only |
| `persisted_offset` | `RangeWriteState.persisted_through` | authoritative |
| `status` | `RecoveryRangeFact.legacy_status`，由 Persistence 局部事实映射 | compatibility only |

`RangeWriteState` 增加的 `observed_dispatch_through`、`gap_blocked` 与 `local_failure` 只用于
写入和 legacy 投影；它们由同一 Persistence thread 根据 registration、Data、completion 和
本地错误更新，不是第二份 Range Lifecycle。恢复不消费 `legacy_status/current_offset`。

status mapping 保持 Phase 3 约定：

| Persistence local fact | Legacy status |
| --- | --- |
| local failure | failed |
| completion consumed、tail/map empty、persisted at end | finished |
| gap blocked | paused |
| Data/Completion observed but incomplete | downloading |
| no Data/Completion observed | empty |

queue/memory pause 可能只存在于 Orchestrator；periodic metadata 不需要跨线程同步这类瞬时展示
状态。内部恢复从不消费 legacy status。字段继续存在，但持久化正确性只依赖
`persisted_offset`。

`prepare()` 的 caller contract：

```text
Persistence owns and reads all RangeWriteState values
    -> snapshot bitmap bytes
    -> build vector<RecoveryRangeFact>
    -> RecoveryCheckpoint::prepare()
    -> immutable PreparedCheckpoint
    -> submit one worker task
```

## 10. Precise commit protocol

### 10.1 Prepare on Persistence thread

`prepare()`：

1. 验证当前没有未领取的 prepared image，且没有 pending commit。
2. 复制 bitmap bytes。
3. 用每个 `persisted_through` 补齐 frozen bitmap 的 finished blocks。
4. 生成完整 legacy range JSON projection values。
5. 从 frozen bitmap 计算 frozen VDL。
6. 分配单调 `CheckpointGeneration`。
7. 把 identity、bitmap、ranges、VDL 封装为 immutable `PreparedCheckpoint`。

此步骤不 flush、不 read part、不 save metadata，也不更新 session VDL。

### 10.2 Commit on worker

`commit()` 必须严格执行：

```text
1. FileWriter.flush()
2. for each frozen FINISHED block with offset >= frozen VDL:
       FileWriter.read(offset, expected_length)
       CRC32(bytes)
       append exactly one sample
3. MetadataStore.replace(frozen state + samples)
4. record generation and frozen VDL as last committed
5. return CheckpointCommitResult
```

错误短路：

| Failure | New metadata visible? | Result |
| --- | --- | --- |
| flush | no | `file_flush_failed` |
| CRC read | no | `file_read_failed` |
| CRC allocation | no | `internal_error` |
| JSON serialize/tmp write/close | no | `metadata_save_failed` |
| atomic replace | old or new only | `metadata_save_failed` unless new confirmed |

不得在 step 1 后重新计算 bitmap 或 VDL。后续 Persistence write 可以继续，但只能进入下一
generation。

### 10.3 Publish on Persistence thread

future ready 后：

1. `get()` result；
2. error 非空时保存 first error，并按 Phase 2 `PacketConsumer::fail(error)` 结束任务；
3. generation 必须等于 pending generation；
4. 把 `result.committed_vdl` release-publish 到 progress/session；
5. 清 pending state；
6. 若 sticky `force_checkpoint_pending` 为 true，在消费下一个 Packet Flow packet 前立即
   prepare 并 submit 最新 frozen image。

publish 不重新读取 bitmap。future completion/get 已提供 worker → Persistence 的
happens-before；public progress atomic 保留 release/acquire。

### 10.4 Cadence values unchanged；forced request 不得丢失

threshold/interval 数值与比较保持：

```text
force
OR bytes_since_flush >= flush_threshold_bytes
OR now - last_flush_time >= flush_interval
```

- 同一时刻最多一个 pending commit。
- range completion 仍 force checkpoint。
- shutdown/final drain 仍 force final checkpoint。
- 普通 threshold/interval 仍在现有 Persistence loop 检查，提交成功后的
  `bytes_since_flush` 与 clock reset 时机保持。
- 本阶段不改变 threshold、interval、不 retry failed commit，也不增加第二个并行 commit。

当前实现的 `maybe_schedule_flush(true)` 在已有 pending future 时直接返回，force intent
没有 sticky state；shutdown/range-complete 因而可能丢失最终 image。目标协议以独立 C 类
修复：

```text
request_checkpoint(force):
    if force:
        force_checkpoint_pending = true
    collect_ready_pending_result()
    if terminal error:
        stop
    if a commit is still pending:
        return
    if !force_checkpoint_pending && !threshold_due && !interval_due:
        return
    freeze latest bitmap + RecoveryRangeFact values
    submit exactly one commit
    if submit succeeds:
        force_checkpoint_pending = false
        reset normal cadence counters/time at the existing successful-submit point
```

规则：

- `force_checkpoint_pending` 只由 Persistence thread 读写，初值为 false；
- pending generation 完成后，sticky force 的 successor image 在消费下一个 packet 前冻结；
- 多个 pending 期间的 force 请求可以合并为一个 mandatory successor generation，因为该
  image 覆盖此前已处理的全部完成事实；它们不能合并成“零次”；
- prepare、task submit 或 successor commit 失败保存 first error并停止，不把 force clear
  后继续；
- shutdown 在 Packet Flow drain 完成后设置 force，并循环 `wait/get → schedule successor
  → wait/get`，直到 `pending == false && force_checkpoint_pending == false`；之后才允许
  Persistence join/finalize；
- range-complete force 在 pending 时不阻塞 curl owner thread，Persistence 自己在 future
  完成后补交；
- 此修复可能增加过去被错误跳过的一次 flush/CRC/metadata commit，必须单独 benchmark，
  不能被描述为纯结构或“原样保留”。

## 11. Finalize and discard semantics

### 11.1 Normal finalize

前置条件：

- network producer 已停止；
- Packet Flow 已 close/drain；
- Persistence 已 flush tails；
- final forced checkpoint 已成功；
- Persistence 已 join；
- no pending commit；
- internal last committed VDL 等于 total size。

顺序：

```text
RecoveryCheckpoint::finalize()
    -> final FileWriter.flush()
    -> close part handle
    -> promote .part to output
    -> remove metadata
    -> return primary result + cleanup result
```

`output_available` 只有在 promotion 成功后为 true。

### 11.2 Resume-complete finalize

如果 `open()` 验证后 `safe_vdl == total_size`：

- 不启动 Packet Flow、Persistence 或 HTTP；
- checkpoint 内部 last committed VDL 设为 total size；
- `RestoredCheckpoint::completed_ranges` 设为 validated required block count，并原样映射到
  `DownloadResult::completed_ranges`，保持当前 fast-path 兼容值；
- 调用相同 `finalize()`；
- 不复制 fast-path finalize/remove 代码。

### 11.3 Failed/incomplete close

任何 download、HTTP、Packet Flow、Persistence 或 commit failure：

- 先停止 network；
- drain/终止 Persistence 并领取 pending result；
- 调用 `close_preserving_artifacts()`；
- 不调用 discard 或 metadata cleanup；
- 返回 first primary error；
- 保留 `.part` 与最近一次成功 `.config.json`。

### 11.4 Required discard

identity mismatch、orphan pair cleanup 或 non-Range partial restart 的 destructive 顺序：

```text
1. invalidate/remove old metadata and confirm result
2. only after required invalidation succeeds:
       close old part handle if any
       reset/create/preallocate .part in fresh mode
3. start fresh Session with empty bitmap and VDL 0
```

required metadata invalidation 失败时：

- 不重置 part；
- 返回 `metadata_save_failed`；
- 保留原 artifact pair；
- caller 不得继续网络下载。

这是对当前“先 reset part、后忽略 remove”的 C 类修复。

### 11.5 Cleanup after output promotion

metadata cleanup 结果必须区分：

- `removed`：文件存在且删除成功；
- `not_found`：本来就不存在，仍是成功 cleanup；
- `failed`：保留真实 filesystem error。

若 promotion 已成功：

```text
FinalizeResult.output_available = true
FinalizeResult.error = {}
FinalizeResult.metadata_cleanup.status = failed
FinalizeResult.metadata_cleanup.error = filesystem error
```

engine adapter 仍返回成功 `DownloadResult`。cleanup error 只能用于内部测试、debug trace 或未来
独立诊断，不加入 Performance Summary，也不覆盖 `DownloadResult::error`。

## 12. Crash-point matrix

表中的“target restart”是假定 C 类 hardening 已完成后的要求。

### 12.1 Periodic checkpoint

| Crash/failure point | Files after crash | Target restart |
| --- | --- | --- |
| before frozen image | last pair | resume old checkpoint |
| after image, before part flush | last pair | resume old checkpoint |
| during part flush | last pair | resume old checkpoint; newer bytes untrusted |
| after part flush, before CRC | last pair | resume old checkpoint |
| during required CRC read | last pair | current run fails; old checkpoint retained |
| after CRC, before tmp create | last pair | resume old checkpoint |
| during tmp write | last pair + partial `.tmp` | ignore tmp, resume old checkpoint |
| after tmp close, before replace | last pair + complete `.tmp` | ignore tmp, resume old checkpoint |
| during atomic replace | old or new config, never absent | validate whichever config is visible |
| after replace, before future return | new pair | resume new checkpoint |
| after future return, before public VDL publish | new pair, stale in-memory progress | restart uses new checkpoint |
| after public VDL publish | new pair | resume new checkpoint |

### 12.2 Required discard / fresh restart

| Crash point | Files | Target restart |
| --- | --- | --- |
| before metadata invalidation | old pair | evaluate old pair normally |
| during invalidation failure | old pair | return cleanup error, no part mutation |
| after invalidation, before part reset | old part only | part untrusted; restart fresh |
| during part reset/preallocate | no valid config + partial/fresh part | restart fresh |
| after reset, before first checkpoint | fresh part only | restart fresh |
| after first new checkpoint | new pair | resume new checkpoint |

旧 metadata 绝不能与已重置 part 同时作为有效 pair 留下。

### 12.3 Finalization

| Crash/failure point | Files | Target restart/result |
| --- | --- | --- |
| before final checkpoint | part + last config | resume last checkpoint |
| after final checkpoint | complete part + complete config | direct finalize allowed |
| during final flush | part + complete config | retry validate/finalize |
| before promotion | part + complete config | direct finalize allowed |
| promotion fails | part + config; old output unchanged when overwrite | primary failure |
| during atomic overwrite promotion | old or new output; no empty-name gap | inspect output/part deterministically |
| after promotion, before config cleanup | output + stale config, no part | output is completed; config orphan |
| config cleanup fails | output + stale config | public result remains success |
| after config cleanup | output only | completed |

overwrite promotion 的原子化是独立 C 类提交。实现必须使用平台真实 contract：

- POSIX 使用 same-filesystem rename replace；
- Windows 使用能替换现有目标的系统 operation，而不是先 remove output；
- 不跨 filesystem 隐式 copy；
- 无法满足时返回 primary finalize error，保留 part 和 metadata。

本文只要求 process-crash consistency；若声称 power-loss durability，必须另行证明 metadata
file 与 parent directory flush contract。

## 13. Metadata compatibility table

target writer 继续输出以下字段，名称和值类型不变：

| JSON field | Type | Writer source | Reader/default | Recovery use |
| --- | --- | --- | --- | --- |
| `url` | string | remote identity | `""` | exact identity |
| `output_path` | string | paths | `""` | exact identity |
| `temporary_path` | string | paths | `""` | exact identity |
| `total_size` | signed integer | remote identity | `0` | exact identity |
| `vdl_offset` | signed integer | frozen bitmap | `0` | CRC trust split |
| `accept_ranges` | boolean | probe fact | `false` | history only |
| `resumed` | boolean | Session history | `false` | history only |
| `etag` | string | probe fact | `""` | conditional identity |
| `last_modified` | string | probe fact | `""` | conditional identity |
| `block_size` | unsigned integer | raw recovery policy | `0` | exact identity |
| `io_alignment` | unsigned integer | raw recovery policy | `0` | exact identity |
| `bitmap_states` | byte array | frozen bitmap | empty | trust candidate |
| `ranges` | object array | frozen Persistence facts | empty | persisted projection |
| `crc_samples` | object array | post-flush reads | empty | VDL-tail validation |

每个 range object 继续包含：

```json
{
  "range_id": 0,
  "start_offset": 0,
  "end_offset": 0,
  "current_offset": 0,
  "persisted_offset": 0,
  "status": 0
}
```

每个 sample 继续包含：

```json
{
  "offset": 0,
  "crc32": 0,
  "length": 0
}
```

Compatibility rules：

- writer 继续 `dump(2)`；
- reader 继续忽略 unknown fields；
- `ranges`/`crc_samples` 缺失或非 array 时按 empty；
- bitmap 短于 required count 时剩余 empty；
- bitmap 多余数据在 C 类 structural validation 后拒绝，不 silent truncate；
- `ranges[].end/current/status` 继续 round trip，但不授予 active Lease；
- valid legacy artifact 不需要 migration 或 rewrite 才能恢复；
- 不写 version、不重排路径、不 canonicalize URL/path；
- orphan `.tmp` 永远不作为正式 metadata 读取。

## 14. Error classification

### 14.1 Primary errors

下列错误阻止完成，并进入 `DownloadResult::error`：

| Operation | Error |
| --- | --- |
| metadata open/parse/structural decode | `metadata_parse_failed` |
| required metadata invalidate/replace | `metadata_save_failed` |
| part open/preallocate | existing file errors |
| recovery/commit CRC read | `file_read_failed` |
| part write | `file_write_failed` |
| part flush | `file_flush_failed` |
| output promotion | `file_write_failed` |
| impossible state/generation/order | `internal_error` |

保持 first-error rule。后续 close/cleanup symptom 不能覆盖 primary error。

### 14.2 Safe rollback, not errors

- identity mismatch；
- missing CRC sample beyond VDL；
- CRC mismatch beyond VDL；
- transient downloading reset；
- short legacy bitmap；
- non-Range partial state requiring clean restart。

这些产生确定性 fresh/partial recovery 结果，不把坏 block 当成可信。

### 14.3 Advisory cleanup errors

只有“output promotion 已成功”之后的 metadata removal failure 是 advisory。它必须结构化返回，
但 public download 保持 success。

promotion 之前的 required invalidation failure 不是 advisory；它会阻止 part reset，以免制造
stale pair。

### 14.4 CRC sample read failure hardening

当前 commit path 会跳过失败 sample。目标规则：

- 不保存 sample set 不完整的新 metadata；
- old metadata 保持可见；
- commit 返回 `file_read_failed`；
- Persistence 保存该 first error 并停止；
- artifacts 可在下一次启动按 old checkpoint 恢复。

这是独立 C 类提交。不得和 `build_crc_samples()` 搬家合并。

## 15. Ownership, lifetime, thread and memory rules

### 15.1 Ownership

| Object | Owner | Borrower |
| --- | --- | --- |
| `RecoveryCheckpoint` | `DownloadEngine::run()` stack unique owner | Persistence, one worker |
| `FileWriter` | RecoveryCheckpoint implementation | none outside module |
| `MetadataStore`/codec | RecoveryCheckpoint implementation | none outside module |
| `PreparedCheckpoint` | Persistence until task submit, then worker future | worker only |
| `RangeWriteState` | PersistenceThread | prepare caller only |
| restored bitmap value | engine, then Range/Persistence setup | by-value consumers |
| commit result | future, then Persistence | progress adapter |

销毁顺序：

```text
stop HTTP production
close/drain Packet Flow
Persistence final prepare/commit
wait pending future
Persistence join
finalize OR close_preserving_artifacts
destroy RecoveryCheckpoint
```

不得有 detached task、worker-held raw pointer after future get 或 process-global checkpoint。

### 15.2 Thread permissions

| Method/state | Allowed thread |
| --- | --- |
| `open()` | Orchestrator before Persistence start |
| `write()` | Persistence writer only |
| `prepare()` | Persistence writer only |
| `commit()` | exactly one worker at a time |
| pending future/cadence | Persistence writer only |
| `finalize()` | Orchestrator after Persistence join |
| `close_preserving_artifacts()` | Orchestrator after join, or idempotent destructor |
| metadata codec | open thread or commit worker, never concurrently |

Debug build SHOULD capture owner thread ids and reject wrong-thread calls with deterministic
`internal_error` and no side effect。Release 不在 per-write hot path 增加 thread-id lookup。

### 15.3 Synchronization

- `FileWriter` 保留一把 mutex 串行 write/read/flush/finalize。
- `PreparedCheckpoint` 只有 immutable plain values，不需要 atomic。
- Persistence submit 之前完成 image 写入；task handoff 提供 release/acquire。
- future ready/get 提供 worker completion happens-before。
- committed VDL 从 Persistence release store，progress acquire load。
- cleanup/finalize 在 join 后单线程执行。
- generation 由 Persistence 单线程分配并严格递增；`0` 表示没有 commit。

不得用 relaxed atomic 参与 checkpoint publication、stop 或 ownership。纯统计字段仍可按前置
阶段合同使用 relaxed。

### 15.4 Exception and future rules

- `prepare()` 捕获 vector/string/JSON candidate allocation failure。
- worker task 最外层捕获所有异常并返回 `internal_error` 或 metadata error。
- future 必须始终被 `get()`；不允许只 wait 后丢弃。
- task submission failure 不丢失 prepared image，返回 first error 并停止。
- destructor 不等待未知 detached work；合法销毁前 pending future 必须已领取。

## 16. Expected file layout

建议：

```text
src/recovery/recovery_checkpoint.hpp
src/recovery/recovery_checkpoint.cpp
src/recovery/recovery_types.hpp
src/recovery/metadata_codec.hpp
src/recovery/metadata_codec.cpp
src/storage/file_writer.hpp
src/storage/file_writer.cpp
src/metadata/metadata_store.hpp
src/metadata/metadata_store.cpp
src/download/download_engine.cpp
src/persistence/persistence_thread.hpp
src/persistence/persistence_thread.cpp
src/core/models.hpp
tests/recovery/recovery_checkpoint_test.cpp
tests/recovery/recovery_compatibility_test.cpp
tests/recovery/recovery_crash_matrix_test.cpp
tests/recovery/recovery_failure_test.cpp
tests/download/download_resume_integration_test.cpp
```

`FileWriter` 和 `MetadataStore` 可以保留为 module-private leaf implementation，但 CMake/include
方向必须保证 engine、persistence 和其他生产模块不能直接依赖它们。

## 17. Tests-first tiny commits and rollback

每个 slice 单独 build/test/evidence，不 squash 到不可回滚。

| Slice | Class | Commit intent | Verification | Rollback |
| --- | --- | --- | --- | --- |
| 04.1 | test | characterize legacy schema、identity、CRC rollback、remove/replace/crash gaps | tests 在旧实现上绿；已知缺口为明确 red | 只移除新 tests，保留 ticket findings |
| 04.2 | S | add recovery types、codec、unwired owning module | codec golden/roundtrip、open fresh temp FS | 删除新 module/CMake |
| 04.3 | S | migrate load/identity/validate from engine | all legacy resume tests；engine helpers unused | 恢复 engine adapter，新 module 可保留 |
| 04.4 | C | reject inconsistent finished coverage before serialized VDL | VDL-prefix-hole red→green；error 保留 artifacts | 回滚 malformed hardening；保留 load migration |
| 04.5 | C | invalidate metadata before destructive fresh part reset | crash-after-reset red→green、remove failure | 回滚本 fix；不回滚 04.3 |
| 04.6 | S | migrate part writes and ownership into checkpoint | Persistence real-file tests、no direct writer access | 恢复 borrowed writer adapter |
| 04.7 | C | freeze checkpoint image and publish exact committed VDL | barrier-controlled race test、crash matrix | 回滚 fix；保留 owning module |
| 04.8 | C | reject incomplete CRC sample reads | nth-read failure retains old metadata | 回滚 error behavior only |
| 04.9 | C | replace metadata atomically and check tmp close | old-or-new crash tests on supported platforms | 恢复 legacy replace implementation |
| 04.10 | C | preserve forced checkpoint requests while commit is pending | pending+range-complete/shutdown red→green；successor generation、future get | 回滚 sticky force；保留 frozen image |
| 04.11 | S | migrate normal/fast finalize and cleanup classification | both paths use same interface；fast-path count；cleanup failure success result | 恢复 finalize adapter |
| 04.12 | C | atomic overwrite promotion and orphan metadata rule | promotion crash matrix、orphan corrupt config | 回滚 hardening only |
| 04.13 | S | delete direct leaf access and legacy helpers | `rg` deletion checks、full tests | 回滚 deletion commit only |
| 04.14 | perf | verify S/C changes independently | formal benchmark、forced successor attribution、profiler if needed | 回滚最小 regression slice |
| 04.15 | docs | record compatibility/performance/rollback evidence | stage checklist | docs-only rollback |

04.4、04.5、04.7、04.8、04.9、04.10、04.12 是 correctness commits。每个 commit
message MUST 使用 `fix:` 或仓库等价分类，并在 evidence 中写清行为差异。

回滚任一 C slice 或整阶段时，active test suite 只能保留旧实现也能通过的
characterization。只有对应 correctness fix 才能通过的 green tests 必须随该 C commit
回滚或显式禁用；其用例合同与失败输出保留在 evidence。回滚后的 baseline 不得保持 red。

## 18. Deterministic test contract

### 18.1 Test filesystem policy

大多数测试必须：

- 创建唯一 `std::filesystem::temp_directory_path()` 子目录；
- 使用真实 `.part/.config.json/.tmp/output`；
- 真实执行 preallocate、write、flush、read、rename 和 remove；
- 测试结束只删除该唯一子目录；
- 不依赖 sleep 判断 I/O 完成；
- 不使用内存 filesystem 代替 OS path semantics。

只对难以由权限稳定触发的 operation failure 使用 private compile-time fault seam：

```text
RecoveryCheckpointImplementation<RealLocalFileOps>
RecoveryCheckpointImplementation<FaultInjectingLocalFileOps>
```

`FaultInjectingLocalFileOps` 必须包装真实 temp filesystem；它只在指定 operation
before/after point 返回错误或 simulated crash，不伪造文件内容。

该 seam 不进入 production header，不形成通用 virtual filesystem interface。

### 18.2 Compatibility tests

至少包括：

- `LoadsCurrentPrettyPrintedMetadataFixture`
- `RoundTripsEveryLegacyFieldWithoutAddingVersion`
- `IgnoresUnknownFields`
- `TreatsMissingRangesAndCrcArraysAsEmpty`
- `AcceptsMissingEtagWhenRemoteHasEtag`
- `AcceptsMissingLastModifiedWhenRemoteHasValue`
- `RejectsConflictingNonEmptyEtag`
- `RejectsConflictingNonEmptyLastModified`
- `RequiresExactUrlPathsSizeBlockAndAlignment`
- `RestoresShortBitmapWithEmptySuffix`
- `ResetsDownloadingBlocks`
- `ProjectsLegacyPersistedOffsets`
- `DoesNotRestoreLegacyActiveLeaseOrStatus`
- `KeepsDumpIndentationAtTwoSpaces`

golden fixture 应手写并提交；不能先由新 writer 生成再让 reader 读取，那只证明自洽。

### 18.3 Recovery trust tests

- bitmap/range projection 在 serialized VDL 前留下 hole 时返回
  `metadata_parse_failed`，且 part/config bytes 原样保留；
- missing sample beyond VDL 回到 empty；
- mismatched CRC 只回退对应 block；
- valid CRC 保留 sparse finished block；
- last short block 使用 exact expected length；
- duplicate/unaligned/out-of-range sample 拒绝；
- CRC read failure 返回 error 且不修改 artifacts；
- safe VDL 由 validated bitmap 重算；
- trusted bytes 与 safe VDL 分开统计；
- resumed Writer 在启动线程前以 trusted bytes 初始化 absolute persisted counter，fresh
  初始化为 0，恢复基数不发 disk telemetry；
- complete validated checkpoint 不启动 network，并把 required block count 投影为
  `DownloadResult::completed_ranges`。

### 18.4 Commit ordering tests

使用 barrier 控制真实 writer 与 fault ops：

- frozen image 后新增 write 不进入当前 metadata；
- write 抢在 flush lock 前时也只能被下一 image 声明；
- flush 完成后 live bitmap advance 不改变 commit VDL；
- commit result VDL 精确等于 metadata VDL；
- public VDL 只在 metadata replace 成功后推进；
- 同时第二个 commit 被拒绝；
- stale generation result 不能覆盖新 generation；
- range-complete force 在旧 commit pending 时形成一个 mandatory successor generation；
- shutdown force 在旧 commit pending 时形成一个 latest-image successor，且两次 future
  都被 `get()`；
- 多个 pending force 最多合并成一个覆盖全部已处理事实的 successor，不能变成零次；
- successor prepare/submit/commit failure 保留 first error 与最近一次成功 artifacts；
- final forced checkpoint 必须被 future get。

关键回归测试：

```text
freeze bitmap at block N
allow worker flush
write and mark block N+1 after flush
allow worker continue
assert metadata VDL == frozen block N frontier
assert no CRC/sample claims N+1
```

### 18.5 Failure tests

至少定向注入：

- part open failure；
- preallocate failure；
- part write failure；
- part flush failure；
- first/nth CRC read failure；
- metadata parent directory creation failure；
- tmp open failure；
- tmp write failure；
- tmp close failure；
- metadata replace failure；
- required invalidate failure；
- output promotion failure；
- final metadata cleanup failure。

每个测试同时断言：

- primary error；
- old/new artifacts；
- committed VDL 是否推进；
- output 是否存在；
- first error 未被 cleanup 覆盖。

### 18.6 Crash matrix tests

private fault seam 为每个 operation boundary 提供一次性 `stop_after(point)`。测试在返回
`simulated_crash` 后销毁当前 session、关闭 handle，并用新的 `RecoveryCheckpoint::open()`
读取同一 temp directory。

至少覆盖：

- after part flush；
- after last CRC read；
- after tmp close；
- before/after metadata replace；
- after metadata invalidate；
- after part reset；
- before/after output promotion；
- before metadata cleanup。

若要增加 subprocess crash harness，它是额外证据，不能替代 library-level deterministic
tests。当前 Windows `error=740` 需按阶段 0 单独分类。

## 19. Integration gates

### 19.1 Library-level

- fresh Range download；
- interrupted Range resume；
- CRC rollback past VDL；
- downloading reset；
- server failure preserves artifacts；
- resume-complete direct finalize；
- identity mismatch clean restart；
- non-Range no artifact full download；
- non-Range partial artifact clean restart；
- non-Range complete artifact direct finalize；
- successful output with metadata cleanup failure remains success；
- corrupt orphan metadata without part does not poison fresh run；
- overwrite disabled preserves existing output and recoverable part。

### 19.2 Cross-module

- Phase 3 final `PersistenceCommitted` 仍早于 task final checkpoint 判定；
- Phase 2 consumer failure drains packet ownership before checkpoint close；
- no metadata worker reads `RangeLifecycle`；
- no engine path calls FileWriter/MetadataStore directly；
- progress VDL equals last successful commit result；
- failed commit stops the Session without deleting artifacts；
- final output bytes equal source。

### 19.3 Existing environment failures

`ResumeAfterInterruptedCliDownload` 与 `LoadsDownloadOptionsFromConfigFile` 的 Windows
`error=740` 按阶段 0 处理：

- 不能把 740 当业务测试通过；
- library-level recovery tests 必须独立通过；
- failure set 和 reason 不得扩大；
- 正常 process-launch 环境补跑 CLI integration。

## 20. Performance-neutral contract

性能历史已经建立：

- CRC repeated read 是真实次级成本，但 incremental cache 没有 keeper 收益；
- 放宽 flush cadence 让 scheduler stress 明显恶化；
- simple write/flush/read handle splitting 产生更差吞吐和 slow runs；
- compact metadata JSON 没有解决 pending-flush slow mode；
- pending `FlushFileBuffers` 与主写路径共锁是当前强信号。

因此本阶段 MUST 保持：

- flush threshold 和 interval；
- range-complete/shutdown forced commit 的目标语义；
- 一个 pending commit；
- 每次 commit 重新读取所需 CRC blocks；
- pretty JSON；
- 一个 FileWriter handle/mutex serialization shape；
- packet aggregation、queue budget 和 pause rules；
- no new default metric fields。

明确禁止在本阶段重新引入：

- incremental CRC cache；
- write-time CRC；
- compact JSON；
- dedicated read handle；
- split write/flush handles；
- backlog-gated flush；
- relaxed cadence；
- new byte-budget diagnostics without separate plan。

冻结 image 可能减少错误的 live overclaim，但不能作为吞吐优化宣传。
04.10 sticky force 会补回当前实现错误跳过的 mandatory successor commit，是明确 C 类行为
变化。benchmark 必须分别报告 normal cadence case 与 pending-force stress；若正常 workload
出现显著额外 commit，先核查 force episode 计数，不能放宽 threshold 或重新丢弃 force 来
掩盖。

### 20.1 Verification

触及 Persistence/flush hot path，必须执行阶段 0 的 Release pre/post benchmark：

- `baseline_default`
- `balanced_candidate`
- `memory_guard`
- `scheduler_stress`

风险探针：

- `deep_buffer_candidate`
- `queue_backpressure_stress`
- `gap_tolerance_probe`

gate：

| Signal | Gate |
| --- | --- |
| Debug/Release build | pass |
| prior passing tests | no new failure |
| baseline/balanced network/disk speed | 任一 median decline 不得超过 5% |
| memory_guard peak | 保持约 4.2 MiB 既有行为，除非单独批准 |
| pause count | multi-case 约 15% 物质回归需调查 |
| packet metrics | shape 不因本阶段改变 |
| flush cadence | threshold/interval/forced count 语义不变 |
| summary schema | exact 10 keys |

超阈值先回滚触发 slice，再用 profiler 解释；不得改默认参数掩盖。

## 21. Deletion list

最终 slice 应删除或收口：

- `DownloadEngine::metadata_matches()`；
- `DownloadEngine::rebuild_bitmap_from_snapshots()`；
- `DownloadEngine::validate_resumed_blocks()`；
- `DownloadEngine::sum_finished_bytes()` 的恢复用途；
- `finalize_storage_phase()`；
- resume-complete fast path 的重复 finalize/remove 代码；
- engine 对 `MetadataStore::load/remove` 的直接调用；
- engine 对 `FileWriter::open/finalize/close` 的直接调用；
- `PersistenceThread::build_metadata_state()`；
- `PersistenceThread::build_crc_samples()`；
- Persistence constructor 的 `FileWriter&` 与 `MetadataStore&`；
- worker 对 live `bitmap_` 和 `SessionState` 的读取；
- `MetadataStore::remove()` 永远 success 的 interface；
- 生产 caller 对 `core::MetadataState` 的直接构造；
- 只测试 leaf 调用而不经过 RecoveryCheckpoint 的重复 tests。

可以保留为 private implementation：

- `storage::FileWriter`；
- metadata JSON codec/store；
- `core::crc32`；
- legacy DTO fields。

Deletion checks：

```powershell
rg -n "metadata_matches|validate_resumed_blocks|rebuild_bitmap_from_snapshots" src
rg -n "MetadataStore|FileWriter" src/download src/persistence
rg -n "bitmap_\\.contiguous_finished_bytes" src/persistence
rg -n "build_crc_samples|build_metadata_state" src/persistence
rg -n "dump\\(\\)" src/recovery src/metadata
rg -n "dedicated.*handle|incremental.*crc" src
```

预期：

- 前四项生产命中为 0；
- compact `dump()` 为 0，`dump(2)` 保留；
- banned experiment 为 0。

## 22. Evidence per slice

每个提交保存：

```text
Base commit:
Phase/slice:
Class: S or C
Old owner:
New owner:
Artifact/schema diff:
Success behavior diff:
Failure/crash behavior diff:
Tests added before code:
Debug/Release result:
Benchmark pre/post:
Known 740 result:
Fault point exercised:
Rollback commit:
Deleted symbols:
Open risk:
```

对 C 类提交，`Failure/crash behavior diff` 不能为空。对 S 类提交，该项必须是 `none`，否则
说明隐藏了 correctness change。

## 23. Acceptance checklist

- [ ] RecoveryCheckpoint unique-owns FileWriter and MetadataStore.
- [ ] engine/persistence 不直接操作恢复 leaf types.
- [ ] valid legacy JSON fixture 无迁移可加载.
- [ ] 现有字段、类型、pretty formatting 和 path suffix 不变.
- [ ] identity matrix 与当前兼容规则一致.
- [ ] transient downloading reset.
- [ ] VDL-tail missing/mismatch CRC 确定性回退.
- [ ] recovery CRC read error 传播.
- [ ] commit CRC read error 不再静默跳过.
- [ ] checkpoint image 在 Persistence 线程冻结.
- [ ] worker 不读 live bitmap、Lifecycle、RangeWriteState 或 Session.
- [ ] flush → CRC → metadata replace → VDL publish 顺序有测试.
- [ ] pending commit 期间的 range-complete/shutdown force 形成 mandatory successor，所有
  future 都被 get.
- [ ] metadata replace crash 后只可能看到 old 或 new config.
- [ ] required discard 先确认 metadata invalidation，再 reset part.
- [ ] non-Range partial state clean restart；complete state direct finalize.
- [ ] failed task 保留 part 和 last successful metadata.
- [ ] finalize 必须以 full committed VDL 为前置.
- [ ] normal 与 resume-complete 共享同一 finalize interface.
- [ ] validated trusted bytes 在 Writer 启动前初始化 absolute persisted；resume 初始
      downloaded/persisted 相同且不产生 disk telemetry.
- [ ] promotion 后 cleanup failure 不把 completed output 报成失败.
- [ ] overwrite promotion 不先删除旧 output 制造 name gap.
- [ ] no detached worker；future 全部 get；销毁顺序明确.
- [ ] threshold/interval 数值、CRC strategy、FileWriter handle shape 不变；sticky force
  correctness 差异单独归因.
- [ ] incremental CRC、compact JSON、handle splitting 没有重现.
- [ ] temp filesystem、fault、crash、integration tests 全部通过.
- [ ] Debug、Release、正式 benchmark 通过 gate.
- [ ] 每个 S/C slice 可独立回滚且 evidence 完整.

## 24. Risks to re-verify during implementation

以下事实需要 Code Agent 在对应 slice 开始前重新核对，但不需要询问用户：

1. Phase 2/3 最终类型名可能与本文不同；允许机械映射，不允许改变 ownership/FIFO/fact
   contracts。
2. Windows atomic metadata/output replace 的真实 platform contract 必须查本机 SDK 文档和
   return path；不能凭函数名推断。
3. POSIX rename 只在同 filesystem 内原子；paths 都从同 output parent 派生，仍需测试。
4. `FileWriter::flush()` 是 OS flush contract，不自动证明 metadata file 和 parent directory
   的 power-loss durability；文档不得夸大。
5. 当前 `ofstream` write/close error 可观测性不足；04.9 必须验证 stream state。
6. legacy malformed artifact 是否存在外部 fixture 未知；valid fixture 是兼容底线，malformed
   hardening 必须以安全为目标并单独分类。
7. current `FileWriter::open(resume=true)` 的 oversized part 行为跨 Windows/POSIX 不同；
   若增加 exact-size validation，必须作为额外 C 类提交，不混入 migration。
8. output cleanup error 当前没有 public diagnostic channel；本阶段只保留 internal
   `FinalizeResult`，不扩 Performance Summary。
9. CRC sample generation 仍可能是次级成本；性能线程已否决简单 cache，本阶段不重开。

若发现 valid legacy artifact 无法按本文加载，暂停对应 slice并保存 fixture、字段和当前
reader branch 证据。其他独立 slice 可以继续。

## 25. Code Agent start order

1. 读取阶段 0–4、`docs/architecture/refactor/domain_glossary.md`、性能 playbook 和性能历史的
   flush/CRC sections。
2. 记录 actual base commit、dirty worktree、toolchain 和已有 740 分类。
3. 只实现 04.1 characterization，不先搬生产代码。
4. 用手写 golden metadata 与真实 temp filesystem 建立 compatibility surface。
5. 增加未接线 owning module 和 codec。
6. 逐个迁移 load/validate、write、commit、finalize caller。
7. 每个 C 类缺口先 red test，再单独修复。
8. 每个 slice 后执行 deletion/ownership `rg`。
9. 04.7 后执行 frozen-image barrier race 与 crash matrix。
10. 04.10 后执行 pending-force successor stress 与单独 benchmark attribution。
11. 04.13 后执行 Debug、Release、integration 和 formal benchmark。
12. 保存每个 rollback commit/evidence；所有 checklist 完成后才进入 Phase 5。

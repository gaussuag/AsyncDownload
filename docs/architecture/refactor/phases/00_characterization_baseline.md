# 阶段 0：特征化与兼容基线

## 1. Outcome

本阶段不改变生产行为。它为后续 6 个结构阶段建立可重复的观察面，使 Code Agent 能区分：

- 既有行为与新回归；
- 兼容合同与偶然实现；
- 功能失败与 Windows 进程启动环境失败；
- benchmark 数值回归与 profiler 热点迁移。

阶段完成后，后续每个阶段都必须复用本文件中的 gate；不得用“重构后测试看起来正常”
替代前后对照。

## 2. Baseline identity

本文档编制时的仓库基线：

| Item | Value |
| --- | --- |
| Commit | `7a96af2b137055758bdc221872bdb26692ccf089` |
| Branch relation | `main` ahead of `origin/main` by 31 commits |
| Language | C++20 |
| Build system | CMake + vcpkg |
| Test binary | `build/tests/Debug/AsyncDownload_tests.exe` |
| Formal performance binary | `build/src/Release/AsyncDownload.exe` |
| Formal performance suite | `regression_v2` |

Code Agent MUST 在开始每一阶段时记录实际 base commit。若不再是上述 commit，必须先确认
本路线引用的符号仍存在；不得假定行号稳定。

## 3. Verified current baseline

在上述 commit 上已经得到以下观察：

- `scripts\build.bat` 在允许 vcpkg 写入其外部安装目录后成功。
- Debug 测试共 40 个。
- 在受限进程环境中运行时，33 个通过，7 个依赖子进程的下载集成测试失败。
- 在放宽进程限制后，38 个通过；以下 2 个仍因 Windows `error=740` 失败：
  - `DownloadIntegrationTest.ResumeAfterInterruptedCliDownload`
  - `DownloadIntegrationTest.LoadsDownloadOptionsFromConfigFile`
- 同一环境下以下恢复与网络行为测试通过：
  - `ResumesAfterCrcRollbackPastVdl`
  - `ResetsDownloadingBlocksToEmptyOnResume`
  - `UsesDistinctClientPortsAcrossConcurrentRanges`
  - `ReportsDetailedProgressSnapshot`
  - `PreservesResumeArtifactsAfterServerFailure`

`error=740` 是当前环境中的既有进程启动限制，不是允许忽略测试的永久豁免。Code Agent
SHOULD 在具备正常 Windows process launch 的环境中重跑两个测试。若条件不具备，必须：

1. 保留失败原文；
2. 单独运行所有不依赖 CLI 子进程的测试；
3. 不把“仍然是 740”写成业务验证通过；
4. 不允许失败集合扩大。

## 4. Compatibility contract

### 4.1 Public C++ contract

后续阶段 MUST 保持：

- `asyncdownload::DownloadClient` 的现有构造和调用方式；
- `DownloadRequest`、`DownloadOptions`、`DownloadResult` 和 progress callback 的源码兼容；
- 合法的 aggregate initialization 继续可编译；
- 错误通过现有返回值、`std::error_code` 或项目错误枚举表达，不抛异常；
- 调用方不需要包含 `src/` 下的内部 header。

阶段 1 可以增加内部不可变 policy，但 MUST NOT 把它变成现有调用方的必填公开类型。

当前 recovery 在启动时已经验证为完整、因而不创建新的 Range/HTTP Session 的 fast path，
`DownloadResult::completed_ranges` 返回 required bitmap block count。阶段 4 必须先用
library-level fixture 锁定该值，并通过 validated recovery result 显式投影；不能因阶段 3
改为 Lifecycle count 而把 fast path 变成 `0`。

### 4.2 CLI contract

后续阶段 MUST 保持：

- 现有参数名、配置字段名和默认值；
- 合法配置的解释；
- 成功为 `0`、失败为非 `0` 的退出码约定；
- 当前 stderr 错误通道；
- 正式 Performance Summary 的 10 个 key：
  - `avg_network_speed`
  - `avg_disk_speed`
  - `time_to_first_byte_ms`
  - `max_memory_bytes`
  - `max_inflight_bytes`
  - `total_pause_count`
  - `queue_full_pause_count`
  - `packets_enqueued_total`
  - `avg_packet_size_bytes`
  - `max_packet_size_bytes`

运行结果元数据不得重新塞回 Performance Summary。

### 4.3 Recovery contract

后续阶段 MUST 保持：

- 临时文件仍使用 `.part`；
- metadata、bitmap、VDL 和 CRC sample 的现有字段与解释；
- 合法旧恢复文件仍可继续下载；
- transient `downloading` block 在恢复时重置；
- VDL 之后的 CRC 不匹配会回退不可信 block；
- 下载失败时保留可恢复产物；
- 只有持久化事实能确认 Range 完成；
- 完成后的 finalize 与临时 metadata 清理保持现有语义。

阶段 4 可以集中协议，但没有单独格式迁移设计时 MUST NOT 改文件格式。

### 4.4 Threading and ownership contract

后续阶段 MUST 保持：

- libcurl callback 不阻塞等待 persistence；
- 单一 persistence writer；
- packet payload 在 network → persistence 移交后只有一个所有者；
- completion/control 通知不会因数据队列暂时不可接纳而丢失；
- 共享完成事实的 release/acquire 可见性；
- shutdown 能唤醒等待方并完成线程 join；
- 不新增 detached thread 或进程级可变单例。

### 4.5 HTTP contract

后续阶段 MUST 保持：

- HEAD 探测失败时的 Range fallback；
- 支持 Range 时的并行连续字节请求；
- 不支持 Range 时降级为单连接；
- 每个并发 Range 使用独立物理连接的现有意图；
- write callback 返回 pause 时，本批数据未消费并由 libcurl 在 unpause 后重放；
- 当前 transfer 路径对最终 status 与实际接收字节数的校验；
- 当前不自动 retry。

当前 `HttpProbe` 会解析 fallback probe 的 `Content-Range`，但真正的 Range transfer 只校验
status 与累计 body 长度，没有解析并核对 transfer response 的 `Content-Range`。因此
malformed/mismatched `Content-Range` 是阶段 5 必须先用失败测试证明、再以独立 correctness
提交补齐的安全缺口，不是需要原样保持的当前行为。

### 4.6 Performance contract

结构重构的默认目标是性能中性：

- benchmark 是数值真相源；
- profiler 只用于行为和热点结构对比；
- benchmark 必须使用 Release；
- 正式主比较 case：
  - `baseline_default`
  - `balanced_candidate`
  - `memory_guard`
  - `scheduler_stress`
- `deep_buffer_candidate`、`queue_backpressure_stress` 与 `gap_tolerance_probe` 用作风险探针。

## 5. Baseline commands

### 5.1 Clean state and toolchain

```powershell
git status --short --branch
git rev-parse HEAD
$env:VCPKG_ROOT
cmake --version
```

工作树不是 clean 时，Code Agent MUST 记录已有改动，不得覆盖用户改动，也不得用 destructive
git 命令清理。

### 5.2 Debug build and test inventory

```powershell
scripts\build.bat
build\tests\Debug\AsyncDownload_tests.exe --gtest_list_tests
build\tests\Debug\AsyncDownload_tests.exe
```

测试数量变化必须能映射到本阶段明确新增或删除的测试。

### 5.3 Release build

```powershell
scripts\build.bat release
ctest --test-dir build -C Release --output-on-failure
```

### 5.4 Formal benchmark

本地提供稳定的 1 GiB HTTP Range 对象后执行：

```powershell
python scripts\performance\benchmark.py `
  --url "http://127.0.0.1:4287/1gb_files.zip" `
  --benchmark-suite regression_v2 `
  --case-list baseline_default,balanced_candidate,memory_guard,scheduler_stress `
  --repeats 20 `
  --label "phase-N-pre"
```

阶段完成后使用同一机器、文件、server、case、repeat 和环境执行 `phase-N-post`。接近阈值时
把 repeats 提升到 40。

### 5.5 Profiler comparison

只有 benchmark 显示变化或阶段触及 packet/persistence 热路径时才运行：

```powershell
python scripts\performance\profiler.py `
  --url "http://127.0.0.1:4287/1gb_files.zip" `
  --benchmark-suite regression `
  --case-list throughput_candidate,scheduler_stress `
  --label "phase-N-profile"
```

不得把 profiler 下的绝对 MB/s 与正式 benchmark 做数值对比。

## 6. Characterization matrix

本节是各后续阶段的 tests-first 输入清单，不要求阶段 0 在尚不存在的 deep interface 上提交
测试。阶段 0 只合并当前外部/生产 seam 能稳定观察且通过的 characterization 与 fixture。
标为安全缺口、需要新 seam 或目标为“明确失败”的条目，在对应阶段的第一个 slice 先形成
expected-red 证据，再与该阶段修复一起达到可合并的 green 状态；主分支不得停留在全量失败
测试状态。

### 6.1 Stage 1 prerequisites: policy

在迁移前增加 table-driven tests，锁定：

| Behavior | Required observation |
| --- | --- |
| Empty URL/path | 返回现有失败，不启动 I/O |
| Zero connection/window/queue/alignment | 明确失败 |
| Alignment not power of two | 明确失败，不进入 `align_up/down` |
| Alignment exceeds TailBuffer capability | 明确失败，不发生 copy |
| `low > high` | 明确失败 |
| Arithmetic overflow | 明确失败 |
| Range unsupported | effective connection count becomes 1 |
| Valid current defaults | 行为与现有实现一致 |

若某个非法组合当前会触发未定义行为，不需要先把未定义行为“特征化成兼容合同”；应先写
失败测试，再由阶段 1 把它变成确定性错误。

### 6.2 Stage 2 prerequisites: packet flow

迁移前锁定：

- Data Packet FIFO 与 control ordering；
- data admission 失败时 payload 所有权仍在 producer；
- control packet 不丢；
- enqueue/dequeue 的 packet count 一增一减；
- Accounted Bytes 在每个所有权路径上一增一减；
- shutdown 时 blocked consumer 被唤醒；
- Queue Pause 与 Memory Pause 分别开始、保持和恢复；
- callback pause 前未消费 incoming bytes；
- no-allocation `try_enqueue` failure 不等于逻辑 budget 命中。

这些测试应通过待引入的 Packet Flow interface，而不是继续直接构造
`BlockingConcurrentQueue`。

### 6.3 Stage 3 prerequisites: range lifecycle

迁移前锁定：

- initial ranges 按 block 对齐；
- steal 只取得最大未派发 tail；
- Lease 失败归还未持久化窗口；
- 网络完成不直接把 Range 标成 finished；
- persistence 完成发布 finished；
- stale 或 duplicate completion 不推进新 Lease；
- final short block/window 合法；
- transient state 恢复后归零。

### 6.4 Stage 4 prerequisites: recovery

迁移前锁定：

- metadata round trip；
- legacy metadata load；
- 文件 preallocation 与 overwrite policy；
- bitmap hole、VDL 与 CRC rollback；
- metadata mismatch 从干净状态开始；
- interrupted download 能继续；
- server failure 保留 artifacts；
- flush/save/read/remove failure 的错误传播；
- finalize 前后各崩溃点的可接受恢复结果。

Windows CLI 启动失败不能替代 library-level recovery test。

### 6.5 Stage 5 prerequisites: HTTP

迁移前使用可脚本化 server 锁定：

- HEAD success；
- HEAD rejected → one-byte Range probe；
- Range-supported 206；
- Range-ignored 200 降级；
- malformed/mismatched `Content-Range`；
- short body、long body、zero-byte object；
- callback pause/unpause 后同一批数据只提交一次；
- status/transport/callback/storage cancellation 的错误映射；
- concurrent ranges 使用 distinct client ports。

### 6.6 Stage 6 prerequisites: telemetry

迁移前锁定：

- TTFB 只记录第一次有效网络数据；
- first-byte 与 completion 的 duplicate 调用保持幂等；
- download/persist delta 每次有效调用都累计；不得按相同 bytes 或 timestamp 猜测去重；
- pause count 由各 producer 的原因位 `0 → 1` edge 保证 exact-once，Telemetry 不自行猜测
  duplicate；
- network/disk average 计算；
- memory/inflight peak；
- pause count；
- completed 后忽略更新；
- incomplete summary 使用传入时间；
- 正式 10 项 summary 的类型、名称和含义；
- progress snapshot 不成为业务状态真相源。

## 7. Regression thresholds

阶段合并 gate：

| Signal | Gate |
| --- | --- |
| Debug/Release build | MUST pass |
| Previously passing tests | MUST remain passing |
| Known environment failures | MUST NOT expand or change failure reason |
| `baseline_default` `avg_network_speed` / `avg_disk_speed` | either post median decline > 5% blocks |
| `balanced_candidate` `avg_network_speed` / `avg_disk_speed` | either post median decline > 5% blocks |
| `time_to_first_byte_ms` | multi-case material increase around 15% requires investigation; unexplained change blocks |
| `memory_guard` peak memory | SHOULD remain near established ~4.2 MiB; > 15% rise without throughput benefit blocks pending evidence |
| `max_inflight_bytes` | multi-case > 15% rise without throughput benefit blocks pending evidence |
| Pause count | multi-case material regression around 15% requires investigation; unexplained change blocks |
| Summary schema | exact 10-key set MUST remain |
| Recovery artifacts | legacy fixture MUST remain loadable |
| Profiler | used to explain hotspot movement, never to waive benchmark failure |

若结果超过阈值，默认动作是回滚当前结构 slice 并定位；不得靠调整默认配置掩盖。

## 8. Evidence package per stage

每阶段保存一份变更说明，至少包括：

```text
Base commit:
Stage:
Changed modules:
Public contract diff:
Recovery format diff:
Test pre result:
Test post result:
Benchmark pre artifact:
Benchmark post artifact:
Profiler artifact, if any:
Known environment failures:
Rollback commit:
Open risks:
```

benchmark 原始产物保持在现有 `build/benchmarks/` 规则内，不提交 build artifacts。

## 9. Small-commit plan

1. `test: characterize currently observable legacy behavior`
2. `test: add currently green compatibility fixtures`
3. `docs: record pre-change functional and performance evidence`

本阶段不应包含生产代码。如果为测试可达性必须增加 seam，该 seam 应推迟到对应结构阶段，
并在该阶段的第一 slice 中实现。非法 options、Packet Flow、Lease、checkpoint fault 和
HTTP malformed-response 等尚无安全观察面的 red tests 只在本阶段记录精确 test contract，
不作为失败测试提交。

## 10. Rollback

本阶段只增加测试和文档：

- 任一新测试若依赖非确定性时间、端口或环境，先修正 fixture，不放宽断言。
- 测试无法跨平台稳定运行时，保留 library-level deterministic test，把真实 server test
  标成明确的平台合同测试。
- 回滚本阶段不得删除已经发现的兼容风险；风险必须回到对应 Wayfinder ticket。

## 11. Exit checklist

- [ ] 记录实际 base commit、工具链和工作树状态。
- [ ] Debug 与 Release build 完成。
- [ ] 40 个现有测试已分类为 pass、业务 fail 或环境 fail。
- [ ] 公开 C++、CLI、恢复、HTTP、线程与性能合同有可执行观察。
- [ ] 当前可观察行为已有 green characterization；后续 expected-red 项已有精确 test contract。
- [ ] 已生成阶段前 benchmark；若环境不具备，明确阻塞而不是伪造结果。
- [ ] 没有生产行为变化。

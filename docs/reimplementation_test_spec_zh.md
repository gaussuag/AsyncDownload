# AsyncDownload 重写验收测试规范

## 1. 文档目标

这份文档不是描述“当前仓库里有哪些测试文件”，而是把当前实现已经覆盖到的功能维度整理成一套可复用的验收规范。

如果你将来完全重写这个下载器，那么至少应该拿这份清单去验证：

- 你做出来的版本是否具备和当前版本同等级别的功能能力
- 恢复语义、持久化语义、调度语义是否一致
- 不是只会“把文件下完”，而是在关键异常场景下也有一致行为

这份规范分成三层：

1. 核心单元语义
2. 组件级行为
3. 端到端集成行为

## 2. 通过标准

建议把你的重写版本至少做到以下通过标准：

- `必须通过`
  所有 P0 和 P1 用例
- `建议通过`
  所有 P2 用例

优先级含义：

- `P0`
  不通过就不能认为“和当前实现功能维度一致”
- `P1`
  关键工程行为，不通过说明恢复或稳定性能力明显不足
- `P2`
  可观测性或工程强化项，不通过不代表主链完全失效，但能力维度会变弱

## 3. 核心单元语义

### 3.1 请求合法性

#### T-REQ-001 `P0`

目标：

- 空请求必须被拒绝

输入：

- `DownloadRequest.url == ""`
- `DownloadRequest.output_path == ""`

期望：

- `download()` 返回失败
- 错误码等价于 `invalid_request`

当前参考：

- `DownloadClientTest.RejectsEmptyRequest`
- 文件：[main_test.cpp](D:/git_repository/coding_with_agents/AsyncDownload/tests/main_test.cpp)

### 3.2 位图 finished 判定

#### T-BITMAP-001 `P0`

目标：

- 一个块只有在被完整覆盖时才能变成 `FINISHED`

输入：

- `block_size = 64KB`
- 调用 `mark_finished_range(0, 128KB, 64KB, 256KB)`

期望：

- block 0 = `FINISHED`
- block 1 = `FINISHED`
- block 2 = `EMPTY`

当前参考：

- `BlockBitmapTest.MarksFullyCoveredBlocksFinished`
- 文件：[block_bitmap_test.cpp](D:/git_repository/coding_with_agents/AsyncDownload/tests/core/block_bitmap_test.cpp)

#### T-BITMAP-002 `P0`

目标：

- `VDL` 只能停在第一个洞之前，不能跳过空洞统计后续 finished 块

输入：

- block 0 = `FINISHED`
- block 2 = `FINISHED`

期望：

- `contiguous_finished_bytes(...) == 64KB`

当前参考：

- `BlockBitmapTest.ContiguousFinishedBytesStopsAtFirstHole`

#### T-BITMAP-003 `P1`

目标：

- 被触达但未完整覆盖的块必须标记为 `DOWNLOADING`

输入：

- `mark_downloading_range(32KB, 96KB, 64KB, 256KB)`

期望：

- block 0 = `DOWNLOADING`
- block 1 = `DOWNLOADING`
- block 2 = `EMPTY`

当前参考：

- `BlockBitmapTest.MarksTouchedBlocksAsDownloadingBeforeTheyFinish`

#### T-BITMAP-004 `P1`

目标：

- `DOWNLOADING` 不能覆盖已有的 `FINISHED`

输入：

- 先把 block 1 设为 `FINISHED`
- 再对该块调用 `mark_downloading_range`

期望：

- block 1 仍然是 `FINISHED`

当前参考：

- `BlockBitmapTest.DownloadingMarkDoesNotOverwriteFinishedBlocks`

#### T-BITMAP-005 `P0`

目标：

- 恢复时，`DOWNLOADING` 必须回滚成 `EMPTY`

输入：

- bitmap 中混有 `FINISHED`、`DOWNLOADING`、`EMPTY`
- 调用 `reset_transient_states()`

期望：

- `FINISHED` 保留
- `DOWNLOADING -> EMPTY`

当前参考：

- `BlockBitmapTest.ResetTransientStatesClearsDownloadingOnly`

### 3.3 内存背压边界

#### T-MEM-001 `P1`

目标：

- 即使高水位小于单包大小，首包也必须允许通过，避免系统活锁

输入：

- 当前队列内存为 0
- incoming packet = 128KB
- high watermark = 1KB

期望：

- `should_pause_for_backpressure(...) == false`

当前参考：

- `MemoryAccountingTest.AllowsFirstPacketAboveHighWatermark`

#### T-MEM-002 `P0`

目标：

- 当系统已经存在积压时，再入队超阈值数据必须触发暂停

输入：

- current bytes = 64KB
- incoming bytes = 64KB
- high watermark = 96KB

期望：

- 需要 pause

当前参考：

- `MemoryAccountingTest.PausesWhenQueuedBytesAlreadyExistAndThresholdWouldBeExceeded`

#### T-MEM-003 `P1`

目标：

- 空包不应触发背压

当前参考：

- `MemoryAccountingTest.IgnoresEmptyIncomingPacket`

### 3.4 调度器切分与 steal

#### T-SCHED-001 `P0`

目标：

- 初始 range 必须按 block 对齐切分

输入：

- `max_connections = 4`
- `block_size = 64KB`
- bitmap 中 block 1 已 finished

期望：

- 生成多个 range
- 除文件头外，其余 range 起点按 `64KB` 对齐

当前参考：

- `RangeSchedulerTest.BuildsAlignedInitialRanges`

#### T-SCHED-002 `P1`

目标：

- 安全简化版 work stealing 必须从最大未派发尾部切分，并保持 block 对齐

输入：

- 一个大 range
- `current_offset` 已推进到 `2 * block_size`

期望：

- `steal_largest_range()` 返回新 range
- 新 range 起点按 `block_size` 对齐
- donor 的 `end_offset` 被缩短

当前参考：

- `RangeSchedulerTest.StealsLargestUndispatchedTail`

## 4. 组件级行为

### 4.1 持久化线程与 gap 熔断

#### T-PERSIST-001 `P0`

目标：

- 当乱序缺口超过阈值时，range 必须触发 `pause_for_gap`

输入：

- `max_gap_bytes = 4096`
- 先注入 offset = `8KB` 的 packet
- `persisted_offset` 仍在 0

期望：

- `pause_for_gap == true`

当前参考：

- `PersistenceThreadTest.PausesRangeWhenGapExceedsThreshold`

#### T-PERSIST-002 `P1`

目标：

- 部分落盘后，位图应先变成 `DOWNLOADING`，而不是直接 `FINISHED`

输入：

- 写入一个 `4096` 字节 packet
- `block_size = 64KB`

期望：

- 对应 block 为 `DOWNLOADING`

当前参考：

- `PersistenceThreadTest.MarksPartiallyPersistedBlocksAsDownloading`

#### T-PERSIST-003 `P0`

目标：

- 当缺失前缀补齐后，gap pause 必须被清除，且最终 range 能完成

输入：

- 先注入尾部 packet 形成大 gap
- 再注入头部 packet 补齐
- 最后注入 `range_complete`

期望：

- `pause_for_gap` 由 true 变回 false
- `persisted_bytes` 推进到总长度
- 相关块最终都变成 `FINISHED`

当前参考：

- `PersistenceThreadTest.ClearsGapPauseAfterMissingDataArrives`

### 4.2 文件写入器

#### T-FILE-001 `P0`

目标：

- 打开临时文件时必须按目标大小完成预分配

输入：

- `open(path, 128KB, false, true)`

期望：

- 文件存在
- 文件大小等于 `128KB`

当前参考：

- `FileWriterTest.OpenPreallocatesTargetSize`

#### T-FILE-002 `P1`

目标：

- 当 `overwrite_existing = false` 时，已有文件不能被静默覆盖

输入：

- 已存在 `.part`
- 再次 `open(..., false, false)`

期望：

- 打开失败

当前参考：

- `FileWriterTest.OpenFailsWhenOverwriteIsDisabledAndFileExists`

### 4.3 元数据存储

#### T-META-001 `P0`

目标：

- `MetadataState` 必须可完整保存并恢复

输入：

- 包含 URL、路径、大小、VDL、bitmap、ranges、crc_samples 的完整状态

期望：

- `save()` 成功
- `load()` 后关键字段一致

当前参考：

- `MetadataStoreTest.SavesAndLoadsState`

## 5. 端到端集成行为

这些是你重写后最重要的一组验收用例。只通过单元测试，不足以说明功能维度一致。

### 5.1 中断后续传

#### T-E2E-001 `P0`

目标：

- CLI 启动下载后，中途强杀进程，再重新下载，必须能从恢复文件继续完成

步骤：

1. 启动本地支持 Range 的 HTTP 测试服务器
2. 用 CLI 下载一个 `64MB` 文件
3. 等 `.config.json` 真正落盘
4. 强杀 CLI
5. 确认 `.part` 和 `.config.json` 存在
6. 用库接口或 CLI 再次下载同一 URL 与输出路径

期望：

- 第二次下载成功
- `result.resumed == true`
- 最终正式文件存在
- `.part` 与 `.config.json` 被清理
- 正式文件与源文件完全一致

当前参考：

- `DownloadIntegrationTest.ResumeAfterInterruptedCliDownload`
- 文件：[download_resume_integration_test.cpp](D:/git_repository/coding_with_agents/AsyncDownload/tests/download/download_resume_integration_test.cpp)

### 5.2 VDL 之后坏块回滚

#### T-E2E-002 `P0`

目标：

- 恢复时，`VDL` 之后即使某块被标记为 `FINISHED`，只要 CRC 不匹配，就必须回滚并重新下载

步骤：

1. 构造一个 `.part`
2. 构造匹配的 `.config.json`
3. 其中设置：
   - `vdl_offset = 一个块`
   - 后续多个块标记为 `FINISHED`
4. 人为篡改 `VDL` 之后的其中一个块内容
5. 让服务端提供原始正确文件
6. 发起恢复下载

期望：

- 任务成功
- `resumed == true`
- 坏块被重新下载
- 最终文件与源文件一致

当前参考：

- `DownloadIntegrationTest.ResumesAfterCrcRollbackPastVdl`

### 5.3 恢复时 DOWNLOADING 回滚

#### T-E2E-003 `P0`

目标：

- metadata 里残留的 `DOWNLOADING` 块必须在恢复时回滚为 `EMPTY` 并重下

步骤：

1. 构造 `.part` 和 `.config.json`
2. block 0 = `FINISHED`
3. block 1 = `DOWNLOADING`
4. 把 block 1 内容改坏
5. 发起恢复下载

期望：

- 任务成功
- `resumed == true`
- 最终文件与源文件一致
- `DOWNLOADING` 不能被当作已完成块跳过

当前参考：

- `DownloadIntegrationTest.ResetsDownloadingBlocksToEmptyOnResume`

### 5.4 并发连接隔离

#### T-E2E-004 `P1`

目标：

- 并发 Range 下载时，不应退化为单连接串行复用

步骤：

1. 本地测试服务器记录每个 GET 请求的客户端端口
2. 以 `max_connections = 4` 下载一个支持 Range 的 `32MB` 文件

期望：

- 下载成功
- 服务端记录到至少 4 个不同客户端端口
- 最终文件与源文件一致

当前参考：

- `DownloadIntegrationTest.UsesDistinctClientPortsAcrossConcurrentRanges`

### 5.5 进度快照能力

#### T-E2E-005 `P1`

目标：

- 下载过程中必须能对外提供细化的进度快照，而不是只有累计字节数

步骤：

1. 下载一个 `32MB` 文件
2. 注册进度回调并收集所有 `ProgressSnapshot`

期望：

- 至少收到一批 progress callback
- 至少一个快照里：
  - `active_requests > 0`
  - `network_bytes_per_second > 0`
  - `disk_bytes_per_second > 0`
  - `vdl_offset > 0`
- 所有快照中：
  - `inflight_bytes >= 0`

当前参考：

- `DownloadIntegrationTest.ReportsDetailedProgressSnapshot`

### 5.6 服务端故障后保留恢复现场

#### T-E2E-006 `P0`

目标：

- 下载过程中如果服务端直接消失，任务失败后必须保留可恢复现场

步骤：

1. 启动本地 Range 服务器
2. 下载一个较大的文件
3. 等 metadata 生成
4. 直接杀掉服务器
5. 等待下载返回

期望：

- `download()` 返回失败
- 正式输出文件不存在
- `.part` 存在
- `.config.json` 存在

当前参考：

- `DownloadIntegrationTest.PreservesResumeArtifactsAfterServerFailure`

## 6. CLI 和展示层验收

### 6.1 CLI 空参数行为

#### T-CLI-001 `P2`

目标：

- 空参数运行 CLI 时，输出 usage 并返回失败

当前人工验证：

- `build\src\Debug\AsyncDownload.exe`

期望：

- 输出 `Usage: AsyncDownload <url> <output> [connections]`
- 退出码非 0

### 6.2 进度输出字段

#### T-CLI-002 `P2`

目标：

- CLI 进度输出至少包含这些观测维度

期望字段：

- `downloaded`
- `persisted`
- `vdl`
- `inflight`
- `queued`
- `active`
- `paused`
- `net`
- `disk`
- `memory`
- `progress`

说明：

- 其中 `net` 和 `disk` 当前展示单位为 `MB/s`

### 6.3 配置文件加载

#### T-CLI-003 `P1`

目标：

- CLI 必须能从 `--config` 读取 `DownloadOptions`，并影响实际下载行为

步骤：

1. 构造一个 JSON 配置文件
2. 在其中设置较小的 `scheduler_window_bytes`
3. 通过 CLI 传入 `--config`
4. 同时传入 `--summary-file`

期望：

- 下载成功
- summary 文件存在
- summary 中能反映配置生效后的 `windows_total` / `ranges_total`

当前参考：

- `DownloadIntegrationTest.LoadsDownloadOptionsFromConfigFile`
- 文件：[download_resume_integration_test.cpp](D:/git_repository/coding_with_agents/AsyncDownload/tests/download/download_resume_integration_test.cpp)

## 7. 推荐的最小验收组合

如果你重写后时间有限，至少先跑这 8 条：

- `T-REQ-001`
- `T-BITMAP-002`
- `T-MEM-002`
- `T-PERSIST-001`
- `T-FILE-001`
- `T-E2E-001`
- `T-E2E-002`
- `T-E2E-006`

这 8 条通过后，说明你的版本已经具备：

- 基本下载主链
- 恢复链
- 持久化链
- 背压/异常退出链

## 8. 当前仓库中的直接对应测试

当前自动化测试入口：

```bat
build\tests\Debug\AsyncDownload_tests.exe
```

列出测试：

```bat
build\tests\Debug\AsyncDownload_tests.exe --gtest_list_tests
```

当前测试名清单：

- `BlockBitmapTest.MarksRealDownloadPartitionAsFinished`
- `BlockBitmapTest.MarksFullyCoveredBlocksFinished`
- `BlockBitmapTest.ContiguousFinishedBytesStopsAtFirstHole`
- `BlockBitmapTest.MarksTouchedBlocksAsDownloadingBeforeTheyFinish`
- `BlockBitmapTest.DownloadingMarkDoesNotOverwriteFinishedBlocks`
- `BlockBitmapTest.ResetTransientStatesClearsDownloadingOnly`
- `MemoryAccountingTest.AllowsFirstPacketAboveHighWatermark`
- `MemoryAccountingTest.PausesWhenQueuedBytesAlreadyExistAndThresholdWouldBeExceeded`
- `MemoryAccountingTest.IgnoresEmptyIncomingPacket`
- `RangeSchedulerTest.BuildsAlignedInitialRanges`
- `RangeSchedulerTest.StealsLargestUndispatchedTail`
- `DownloadClientTest.RejectsEmptyRequest`
- `PersistenceThreadTest.PausesRangeWhenGapExceedsThreshold`
- `PersistenceThreadTest.MarksPartiallyPersistedBlocksAsDownloading`
- `PersistenceThreadTest.ClearsGapPauseAfterMissingDataArrives`
- `FileWriterTest.OpenPreallocatesTargetSize`
- `FileWriterTest.OpenFailsWhenOverwriteIsDisabledAndFileExists`
- `MetadataStoreTest.SavesAndLoadsState`
- `DownloadIntegrationTest.ResumeAfterInterruptedCliDownload`
- `DownloadIntegrationTest.LoadsDownloadOptionsFromConfigFile`
- `DownloadIntegrationTest.ResumesAfterCrcRollbackPastVdl`
- `DownloadIntegrationTest.ResetsDownloadingBlocksToEmptyOnResume`
- `DownloadIntegrationTest.UsesDistinctClientPortsAcrossConcurrentRanges`
- `DownloadIntegrationTest.ReportsDetailedProgressSnapshot`
- `DownloadIntegrationTest.PreservesResumeArtifactsAfterServerFailure`

## 9. 用这份规范重写时的建议

如果你打算完全重写，不建议一上来就追所有优化点。更稳妥的顺序是：

1. 先做 `T-REQ / T-FILE / T-META`
2. 再做 `T-BITMAP / T-SCHED / T-PERSIST`
3. 然后打通 `T-E2E-001`
4. 再补 `T-E2E-002 / T-E2E-003 / T-E2E-006`
5. 最后补可观测性和连接隔离

换句话说，先还原“可恢复正确性”，再还原“并发和观测增强”。

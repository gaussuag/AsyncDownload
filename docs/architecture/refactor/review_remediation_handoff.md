# AsyncDownload 渐进式重构审查修复交接

## 1. 结论与优先级

本轮审查发现的 6 项问题不需要按同一优先级处理。

合并或宣布完整重构完成前，必须解决以下 3 个修复包：

1. Recovery metadata 结构校验缺失；
2. PersistenceThread 在异常清理路径可能永久阻塞；
3. PreparedCheckpoint 的 generation reservation 生命周期不完整。

其中第 1 个修复包包含两条独立的数据完整性漏洞：超界 VDL 和畸形 CRC sample。
这两条必须分别有回归测试，不能只修其中一种输入。

以下问题可以延后到核心正确性修复之后，不应与上述修复混在同一提交：

- test-only fault seam 通过 `throw` 注入失败；
- 新增 header 的 include 顺序不符合仓库规则；
- 两处新增代码注释不符合仓库规则。

## 2. 必须修复：Recovery metadata 结构校验

### 2.1 问题是什么

RecoveryCheckpoint 在决定一个已有 `.part` 和 metadata 是否可信前，没有完整执行阶段 4
规定的 structural validation。

当前至少存在两条独立绕过路径：

1. `vdl_offset > total_size` 会被当作“已经覆盖完整文件”；后续 CRC 校验因为所有 block
   offset 都小于该 VDL 而被全部跳过。
2. CRC sample 的 offset、length 和唯一性没有先验证。`length = 0` 时，FileWriter 会成功
   返回空数据；如果 sample 保存的是空数据 CRC，该 block 会继续保持 `finished`。

相关位置：

- `src/recovery/recovery_checkpoint.cpp:125-146`
- `src/recovery/recovery_checkpoint.cpp:167-240`
- `src/recovery/recovery_checkpoint.cpp:319-447`
- `src/storage/file_writer.cpp:249-255`
- `docs/architecture/refactor/phases/04_recovery_checkpoint.md:526-547`

### 2.2 为什么必须修复

这不是单纯的输入容错问题。metadata 与 `.part` 是恢复流程的信任边界。

当畸形 metadata 绕过校验时，未下载、未读取或未通过 CRC 的预分配文件区域可以被视为
可信内容，最终提升为正式输出。用户可能得到返回成功但内容已经损坏的文件，而且现有测试
不会暴露这种静默损坏。

这是数据完整性问题，优先级为 P1。

### 2.3 应该得到什么结果

修复后，RecoveryCheckpoint 必须在修改、删除、重建或打开恢复 artifacts 前，对 metadata
执行一次集中、确定性的结构校验。

期望结果：

- `total_size` 必须为正数并与 remote size 一致；
- `vdl_offset` 必须满足 `0 <= vdl_offset <= total_size`；
- VDL 必须是 block 边界或恰好等于 `total_size`；
- CRC offset 必须是合法 block 起点；
- CRC length 必须等于该 block 的实际 expected length，包括最后一个短 block；
- 同一 block offset 最多只能有一个 CRC sample；
- 所有 offset、length 和 block 计算必须使用 checked arithmetic；
- 任何结构矛盾都返回 `metadata_parse_failed`；
- 被拒绝的 candidate metadata、`.part` 和正式输出保持不变；
- 只有真实读取并通过 CRC 的 VDL 后 finished block 才能继续被信任；
- 合法 legacy fixture 继续保持兼容。

### 2.4 最低验收条件

至少新增并通过以下测试：

- 负 VDL；
- VDL 大于 total size；
- 非 block 边界 VDL；
- CRC length 为 0；
- CRC length 小于或大于 expected length；
- 最后一个短 block 使用错误 length；
- CRC offset 未对齐或越界；
- 相同 offset 出现重复 sample；
- 每个拒绝场景都验证 artifacts 未被删除、改写或提升；
- 合法 legacy metadata 和合法完整 checkpoint 仍能恢复或 finalize。

## 3. 必须修复：PersistenceThread 停止协议

### 3.1 问题是什么

`PersistenceThread::stop()` 只把 `stopping_` 设置为 true，但 `process_loop()` 从不读取该
字段。worker 只能在 Packet Flow 返回 `closed` 或 `failed` 时退出。

线程启动后，如果 orchestrator 在关闭 Packet Flow 前遇到分配异常或其它异常，栈展开会先
进入 `PersistenceThread` 析构。析构调用 `stop()` 和 `join()`，但 worker 只会不断收到
timed dequeue timeout，`join()` 因而可能永久等待。

相关位置：

- `src/persistence/persistence_thread.cpp:43-46`
- `src/persistence/persistence_thread.cpp:104-161`
- `src/download/download_engine.cpp:748-775`
- `src/download/download_engine.cpp:1282-1285`
- `libs/concurrentqueue/include/concurrentqueue/blockingconcurrentqueue.h:412-428`

### 3.2 为什么必须修复

该问题把本应映射为 `std::error_code` 的可恢复失败变成永久挂起。它可能发生在内存压力、
容器扩容失败或其它异常展开路径中，并阻止 `DownloadEngine::run() noexcept` 返回。

这属于核心 shutdown、lifetime 和错误处理正确性问题，优先级为 P1。

### 3.3 应该得到什么结果

修复后，PersistenceThread 必须拥有独立、明确且可唤醒的停止协议。

期望结果：

- open Packet Flow 下调用 stop 能让 worker 在有界时间内退出；
- stop 能唤醒正在等待 packet 的 worker，不能依赖下一次 100 ms timeout 才碰运气退出；
- normal close 仍然 drain 已接纳 packet，并执行规定的 final checkpoint；
- abort 与 normal close 的语义分离，异常清理不能伪装成成功 close；
- stop、Packet Flow close/fail 与 worker error 并发时不会死锁；
- stop 和析构保持幂等；
- first error 不被停止过程的连带错误覆盖；
- `DownloadEngine::run()` 在分配失败时返回约定错误码，而不是挂起。

### 3.4 最低验收条件

至少新增并通过以下测试：

- `start -> stop -> join`，Packet Flow 保持 open；
- open Flow 下直接析构 PersistenceThread；
- worker 正在 timed receive 时 stop；
- worker 正在处理 geometry command 时 stop；
- PersistenceThread 启动后、initial geometry vector 分配失败时，run 有界返回；
- stop 与 producer close 并发重复执行；
- normal close 仍完成 drain、final checkpoint 和 accounting 归零；
- Debug 与 Release 下均执行超时保护，证明没有隐藏 hang。

## 4. 推荐同批修复：PreparedCheckpoint token 生命周期

### 4.1 问题是什么

`RecoveryCheckpoint::prepare()` 会设置 `prepared_generation` reservation，但
`PreparedCheckpoint` 的默认析构和默认 move-assignment 不会释放 reservation。

如果成功返回的 token 被 reset、离开作用域，或被 move-assignment 覆盖，后续所有
`prepare()` 都会因为已有 `prepared_generation` 而返回 `internal_error`。

相关位置：

- `src/recovery/recovery_checkpoint.cpp:23-49`
- `src/recovery/recovery_checkpoint.cpp:488-619`
- `src/recovery/recovery_checkpoint.cpp:631-658`

### 4.2 为什么应该修复

当前生产路径通常会立即把 token 交给 `commit()`，因此触发概率低于前两个 P1；但类型公开
声明了析构和 move-assignment，表面上允许安全的 RAII 移动与放弃。实际行为却会永久锁死
checkpoint 状态机。

这是明确的 ownership/lifetime 缺陷，优先级为 P2。建议与 RecoveryCheckpoint 修复同批完成，
但使用独立提交和独立测试。

### 4.3 应该得到什么结果

期望结果：

- 任意时刻最多存在一个有效 prepared token；
- token 交给 commit 后，reservation 恰好转移一次；
- token 未 commit 就销毁时，reservation 被安全释放；
- move construction 只转移 ownership，不重复释放；
- move-assignment 要么安全释放目标原有 reservation 后再接管，要么从接口中删除；
- abandoned token 之后可以再次 prepare；
- stale、foreign 或重复提交继续返回确定的错误，不破坏当前 owner；
- RecoveryCheckpoint 与 token 的销毁顺序不会产生悬空回调或 use-after-free。

### 4.4 最低验收条件

至少新增并通过以下测试：

- prepare 成功后直接销毁 token，再次 prepare 成功；
- move-construct 后只有新 token 可以 commit；
- move-assign 覆盖已有 token 的明确合同；
- foreign token、stale token 和 duplicate commit；
- RecoveryCheckpoint 先于 abandoned token 销毁时行为安全；
- allocation failure 不遗留 reservation；
- successful commit、failed commit 和 abandoned token 后 generation 仍严格单调。

## 5. 可延后的规范清理

以下问题应修，但不应阻塞前三个修复包的定位与验证：

1. 把 test-only fault seam 的显式 `throw std::bad_alloc()` 改为错误结果注入，恢复
   no-exception toolchain 兼容；
2. 按 standard、third-party、project 的顺序整理新增 header includes；
3. 删除 `src/download/range_scheduler.cpp:188,206` 的新增注释。

这些清理应使用独立提交，不能和恢复或停止协议的行为修复混合。

## 6. 建议交付顺序

1. 为两条 Recovery metadata 绕过路径增加 expected-red 测试；
2. 实现集中 structural validator，使两组测试转绿；
3. 增加 PersistenceThread open-flow stop 和异常展开 expected-red 测试；
4. 实现可唤醒、幂等的停止协议；
5. 增加 PreparedCheckpoint abandon/move expected-red 测试；
6. 修复 token reservation 生命周期；
7. 分别运行各模块 fault、property、resume 和真实 HTTP 测试；
8. 最后单独处理规范清理。

每个行为修复应保持独立提交，不做无关重命名、格式化、性能调参或接口扩张。

## 7. 完成定义

Code Agent 只有在满足以下条件后才能声明修复完成：

- 每个缺陷先有能稳定复现旧行为的 expected-red 测试；
- 修复后新增测试在 Debug 和 Release 均通过；
- Debug/Release build 成功；
- main tests、四个 fault binaries、Range property、Telemetry concurrency 和 resume/HTTP
  integration tests 通过；
- Windows `error=740` 仍按阶段 0 基线单独分类，library-level 测试必须全绿；
- malformed recovery metadata 被拒绝时 artifacts 保持逐字节不变；
- 正常 resume、checkpoint、finalize 与旧 metadata 兼容不回退；
- stop/error 路径使用超时保护验证无死锁；
- 没有新增公开 API、自动 retry、第二套状态 owner 或性能参数变化；
- 所有修复证据记录实际命令、退出码和测试数量，不以已有 evidence 声明代替复跑。

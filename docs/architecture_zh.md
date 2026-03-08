# AsyncDownload 架构设计说明

## 1. 文档目的

这份文档面向当前仓库中的实际实现，目标是帮助读代码的人先建立一张完整的脑图，再进入各个 `.hpp/.cpp` 文件。

它回答的核心问题是：

- 这个下载器由哪些模块组成
- 一次下载请求从开始到结束会经过哪些阶段
- 为什么要区分 `downloaded_bytes`、`persisted_bytes`、`VDL`
- 为什么网络层、持久化层、元数据层要拆开
- 断点续传时，哪些状态是可信的，哪些状态必须回滚

## 2. 总体设计目标

项目当前实现的目标不是“最少代码做一个能下文件的工具”，而是围绕以下几条约束构建一个可恢复、可观察、可扩展的下载引擎：

- 网络接收和磁盘写入解耦
- 多连接下载时，回调线程不阻塞
- 持久化写入串行化，避免相邻范围写盘竞争
- 断点恢复有明确的可信度语义
- 进度信息不只反映“下载了多少”，还反映“写盘是否跟得上”

## 3. 核心模块

### 3.1 对外接口层

文件：

- `include/asyncdownload/types.hpp`
- `include/asyncdownload/client.hpp`
- `src/client.cpp`
- `src/main.cpp`

职责：

- 定义 `DownloadRequest`、`DownloadResult`、`ProgressSnapshot`
- 暴露 `DownloadClient::download(...)`
- CLI 将这些接口包装成可执行工具

这里要注意两点：

1. `ProgressSnapshot` 是“下载任务快照”，不是底层线程的原始状态镜像。
2. `DownloadResult` 表达的是最终结果，而不是中途瞬时状态。

### 3.2 下载引擎层

文件：

- `src/download/download_engine.hpp`
- `src/download/download_engine.cpp`

职责：

- 远端探测
- 恢复判定
- 构建初始 range
- 驱动 libcurl multi 事件循环
- 把网络数据封装成 `DataPacket` 丢给持久化线程
- 执行主动背压
- 控制整个任务的收尾顺序

这是整个项目的“编排层”。它自己不直接写磁盘，也不负责乱序重排。

### 3.3 调度层

文件：

- `src/download/range_scheduler.hpp`
- `src/download/range_scheduler.cpp`

职责：

- 根据位图找出未完成区域
- 把未完成区域切成多个 `RangeContext`
- 在运行期做安全简化版 work stealing
- 为每个逻辑 range 派发一个个 window 请求

这里的关键概念是：

- `range`：逻辑上的下载区间
- `window`：单次 HTTP 请求的覆盖区间

一个 range 可以由多个 window 顺序完成。

### 3.4 持久化层

文件：

- `src/persistence/persistence_thread.hpp`
- `src/persistence/persistence_thread.cpp`

职责：

- 独占管理 `RangeContext::persisted_offset`
- 维护乱序包缓存 `out_of_order_queue`
- 执行 4KB 对齐写入
- 更新 block bitmap
- 异步触发 flush 和 metadata 保存
- 推进 `VDL`

这是当前实现中最关键的一层。它把网络层送来的离散 `DataPacket` 收敛成有序的磁盘状态。

### 3.5 存储层

文件：

- `src/storage/file_writer.hpp`
- `src/storage/file_writer.cpp`

职责：

- 打开 `.part` 文件
- 预分配目标大小
- 按偏移读写
- flush
- 把 `.part` rename 成正式文件

`FileWriter` 不理解 range、bitmap、CRC，它只负责“偏移读写 + 平台文件语义”。

### 3.6 元数据层

文件：

- `src/metadata/metadata_store.hpp`
- `src/metadata/metadata_store.cpp`

职责：

- 把恢复所需状态写成 `config.json`
- 从 `config.json` 读回 `MetadataState`

当前实现是 `json + 原子替换`，不是 mmap 元数据。

### 3.7 核心状态与工具层

文件：

- `src/core/models.hpp`
- `src/core/block_bitmap.hpp`
- `src/core/block_bitmap.cpp`
- `src/core/memory_accounting.hpp`
- `src/core/crc32.hpp`
- `src/core/path_utils.hpp`

职责：

- 定义共享状态模型
- 维护 block bitmap
- 做内存会计
- 做 CRC32
- 生成 `.part`、`.config.json` 路径

## 4. 一次下载的完整生命周期

### 4.1 入口校验

`DownloadEngine::run(...)` 先检查：

- URL 是否为空
- 输出路径是否为空
- libcurl 全局环境是否可用

如果这一步失败，任务直接结束，不会创建额外资源。

### 4.2 远端探测

下载前先 probe：

1. 优先发 `HEAD`
2. 如果 `HEAD` 失败，或者拿不到有效大小，则回退到 `Range: 0-0` 的最小 GET

探测结果会给出：

- 资源总大小
- 是否支持 `Range`
- `ETag`
- `Last-Modified`

这些信息会直接决定：

- 是否允许多连接
- 是否允许断点续传
- 恢复文件是否还能继续使用

### 4.3 恢复判定

本地若同时存在：

- `.part`
- `.config.json`

则尝试恢复。但不是只要文件存在就恢复，而是要满足以下条件：

- URL 一致
- 输出路径一致
- 临时文件路径一致
- 总大小一致
- `block_size` / `io_alignment` 一致
- 若远端返回 `ETag` / `Last-Modified`，则身份也要一致

任何一个条件不满足，都退回“全新下载”。

### 4.4 恢复状态清洗

恢复时不会盲目信任 metadata。

当前实现的处理顺序是：

1. 恢复 bitmap 快照
2. 把 `DOWNLOADING` 回滚成 `EMPTY`
3. 根据 `RangeStateSnapshot.persisted_offset` 重建 finished 区域
4. 对 `VDL` 之后仍标记为 `FINISHED` 的块做 CRC 复核
5. CRC 不通过的块回滚为 `EMPTY`

这样做的核心思想是：

- `VDL` 之前：可信
- `VDL` 之后且 `FINISHED + CRC 通过`：可信
- `DOWNLOADING`：不可信，必须重下

## 5. 调度模型

### 5.1 block 与 range

位图按 `block_size` 粒度管理，默认是 `64KB`。

block 状态只有三种：

- `EMPTY`
- `DOWNLOADING`
- `FINISHED`

调度器并不直接看字节流，而是先把所有“不是 FINISHED 的块”拼成若干连续 span，再生成 `RangeContext`。

### 5.2 初始切分

初始 range 会按块边界切分，直到尽量填满 `max_connections`。

这样做有两个好处：

- 并发度足够
- 后续 steal 时边界仍然和位图对齐，恢复逻辑更稳定

### 5.3 window 化请求

即使一个 range 很大，网络层也不是一次请求拉到底，而是按 `scheduler_window_bytes` 分成多个 window。

这样做的目的：

- 避免一个长请求独占连接太久
- 给运行期 work stealing 留空间
- 简化失败回滚：回滚的是“当前 window 租约”，不是整段 range

## 6. 网络层与持久化层为什么分离

### 6.1 网络层做什么

libcurl 回调只做这些事：

- 校验当前 window 是否还能接收数据
- 做内存会计
- 构造 `DataPacket`
- 非阻塞尝试入队
- 必要时 pause 当前 easy handle

它不做：

- 直接写磁盘
- 修改 `persisted_offset`
- 修改位图
- 更新 VDL

### 6.2 持久化层做什么

Persistence 线程负责：

- 识别是否命中 `persisted_offset`
- 乱序包暂存到 `std::map`
- 对齐写盘
- flush tail
- 提升 block 状态
- 生成 metadata 快照

分层之后，磁盘状态就只有一个线程能推进，避免多线程写盘时的边界竞争。

## 7. 为什么需要 TailBuffer

磁盘写入希望按 `io_alignment` 对齐，默认 `4KB`。

但网络包到达时不保证：

- 从 4KB 边界开始
- 以 4KB 整倍数结束

所以 Persistence 线程采用三段式写法：

1. 前对齐：先把前面的零散字节存入 `tail_buffer`
2. 主体写：完整对齐块直接写盘
3. 尾部挂起：最后不足 4KB 的尾巴继续留在 `tail_buffer`

range 完成时，如果尾巴还没凑满，也必须强制刷掉。

## 8. Block Bitmap 的语义

### 8.1 `DOWNLOADING`

表示这个 block 已经被触达并部分落盘，但还不能证明：

- 它完整
- 它可恢复
- 它能推进 VDL

所以 `DOWNLOADING` 只是一种中间态。

### 8.2 `FINISHED`

表示这个 block 的全部有效字节都已经被覆盖并落盘。

只有 `FINISHED` 才能：

- 参与恢复
- 参与最终 finished 统计
- 参与 VDL 推进候选

### 8.3 VDL 为什么不是 finished 总和

`VDL` 的定义是：

从文件头开始，最长连续安全落盘的长度。

所以：

- 如果第 0 块 finished，第 1 块 empty，第 2 块 finished
- 那么 finished 总量是两块
- 但 VDL 只能推进到第 0 块结束

这是恢复语义里最重要的区别之一。

## 9. 背压与 gap 熔断

### 9.1 内存背压

项目维护全局内存会计，统计：

- `DataPacket` payload
- 固定结构开销
- 乱序 map 节点开销

超过高水位后：

- 事件循环会优先暂停当前最快的一批连接

只有回落到低水位以下：

- 才恢复这些连接

这样做是为了避免 pause/resume 抖动。

### 9.2 gap 熔断

某个 range 如果出现很大的乱序洞，Persistence 会把 `pause_for_gap` 置为真。

事件循环看到后会暂停对应 easy handle。

原因是：

- 前面的缺口一直补不上
- 后面的数据继续接收只会无限堆积内存

等缺口补齐、乱序 map 被 drain 掉后，再恢复。

## 10. flush、metadata、VDL 的关系

Persistence 线程会在两种情况下异步发起 flush：

- 达到字节阈值
- 达到时间阈值

异步任务内部顺序是：

1. `file_writer.flush()`
2. 计算新的 `vdl_offset`
3. 生成 CRC 样本
4. 保存 metadata

只有这整套操作成功后，新的 `VDL` 才对外可见。

这条顺序保证了：

- metadata 不会宣称某段数据可恢复，但磁盘实际还没 flush

## 11. 收尾顺序

当前实现收尾遵守这条顺序：

1. 停止网络生产
2. 等 Persistence drain 队列并完成最终 flush
3. 清理 easy/multi 资源
4. 若成功则 finalize 正式文件
5. 若失败则保留 `.part + .config.json`

这意味着：

- 成功任务最终只留下正式文件
- 失败任务尽量留下可恢复现场

## 12. 关键状态的区别

### 12.1 `downloaded_bytes`

表示已经从网络层收到并成功交给持久化链路处理的数据量。

### 12.2 `persisted_bytes`

表示已经物理写入 `.part` 文件的数据量。

### 12.3 `inflight_bytes`

表示：

`downloaded_bytes - persisted_bytes`

它能直接反映：

- 网络写入速度是否高于磁盘落盘速度
- 当前积压是否在变大

### 12.4 `vdl_offset`

表示：

从文件头起，最长连续、安全、已经进入恢复元数据的前沿

## 13. 当前实现状态

当前仓库已经具备：

- 真实 HTTP 下载
- 多连接 range 下载
- 中断后续传
- `VDL + CRC` 恢复
- gap 熔断
- 主动背压
- `.part + .config.json` 恢复现场
- CLI 进度展示

当前仍然是“工程化可用实现”，而不是“所有理想规格都做到极限”的终态。比较明显的差异有：

- 元数据仍是 `json`，不是 mmap
- work stealing 是安全简化版
- 某些网络策略更偏保守正确，而不是极致性能

## 14. 建议阅读顺序

如果你准备继续深入代码，建议按这个顺序读：

1. `include/asyncdownload/types.hpp`
2. `src/core/models.hpp`
3. `src/download/download_engine.hpp`
4. `src/download/download_engine.cpp`
5. `src/persistence/persistence_thread.hpp`
6. `src/persistence/persistence_thread.cpp`
7. `src/download/range_scheduler.hpp`
8. `src/download/range_scheduler.cpp`
9. `src/core/block_bitmap.hpp`
10. `src/core/block_bitmap.cpp`
11. `src/storage/file_writer.hpp`
12. `src/storage/file_writer.cpp`
13. `src/download/http_probe.cpp`
14. `src/metadata/metadata_store.cpp`

这样阅读时，会先看“任务如何被编排”，再看“状态如何被落地和恢复”。

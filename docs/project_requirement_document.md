# AsyncDownload 项目说明文档

## 项目目标

`AsyncDownload` 的目标不是做一个最小可用的单线程下载脚本，而是实现一个面向工程使用的高性能 HTTP 异步下载引擎，并同时提供：

- 一个可复用的 C++ 库接口
- 一个可直接运行的 CLI 程序
- 一套围绕恢复、持久化、调度和性能可观测性的测试与文档体系

项目的核心设计追求可以概括为三点：

- 可恢复：下载中断后，能基于 `.part + .config.json + VDL + CRC32` 恢复任务
- 可观察：不仅看到“下载了多少”，还要能观察“写盘是否跟上”“是否发生暂停”“内存是否积压”
- 可扩展：网络、调度、持久化、元数据和 CLI 解耦，方便继续优化和替换模块

## 原始开发规格中的硬约束

### 技术栈与依赖

- 语言标准：`C++20`
- 构建系统：`CMake + vcpkg`
- 网络层：`libcurl 7.80+`
- 并发队列：`moodycamel::BlockingConcurrentQueue`
- 元数据序列化：`nlohmann/json 3.10+`
- 后勤线程池：`BS::thread_pool`
- 测试框架：`GoogleTest`

特别强调：

- 必须使用 `libcurl multi interface`
- 偏向 `HTTP/1.1` 多连接模型，以实现物理 TCP 隔离
- Windows 平台网络实现需兼容 Schannel 路径
- 元数据层放弃 SQLite 强依赖，采用 `config.json` 方案

### 线程模型

架构是 `1 + 1 + N`：

- `Orchestrator`
  负责 `curl_multi_wait` 事件循环、HTTP 请求编排、主动背压和任务收尾
- `Persistence`
  单线程独占管理落盘状态、乱序重排、4KB 对齐写入、位图推进和 VDL 推进
- `Workers`
  线程池只负责 `Flush`、`CRC32`、metadata IO 等后勤任务

特别强调：

- `CURLOPT_WRITEFUNCTION` 只能把收到的数据封装成 `DataPacket` 入队
- 严禁在回调里直接改 `RangeContext` 的持久化状态

### 主动背压与非阻塞原则

参数与规则如下：

- 高水位：`256MB`
- 恢复水位：`128MB`
- 超过高水位后，暂停速度 Top 20% 的连接
- 回落到低水位后，批量恢复
- 禁止通过阻塞回调来“硬顶住”积压
- 若队列入队失败，也必须走非阻塞 pause 路线


### 存储与对齐规则

要求：

- `IO_ALIGNMENT = 4KB`
- 所有 work stealing 产生的新分片起点都必须按 4KB 对齐
- 持久化写入要采用“三段式”逻辑：
- 前对齐补齐
- 中间连续 4KB 对齐直写
- 尾部不足 4KB 的碎片先挂起，结束或切换时再补齐写盘
- 持久化线程独占 `out_of_order_queue`
- 单个 range 的 gap 超过 `32MB` 时必须触发 gap pause

### 位图、调度与窃取规则

调度基础规则为：

- `BLOCK_SIZE = 64KB`
- block 状态是 `EMPTY / DOWNLOADING / FINISHED`
- 位图只有持久化线程可以在落盘后更新
- 调度线程只读位图，不直接宣称“写盘完成”

安全窃取规则包括：

- 以剩余区间中点为基础做切分
- 切分点必须对齐到 block 边界
- `endOffset` 的发布要用 `memory_order_release`
- 写回调读取 `endOffset` 时要用 `memory_order_acquire`

### 内存会计

全局内存计数不能只算 payload，必须覆盖：

- `Packet.PayloadSize`
- `sizeof(DataPacket)`
- `MapNodeOverhead`

特别强调：

- 64 位系统下 `MapNodeOverhead` 统一按 `48 bytes`

### 恢复语义

恢复模型的关键点：

- 文件旁会保存一份 `config.json`
- 用 `VDL` 表示“从文件头开始连续安全落盘的长度”
- `Offset < VDL` 的部分可以直接信任
- `Offset >= VDL` 且位图标记为完成的块，需要做 `CRC32` 采样校验
- 校验失败的块必须回滚

退出顺序红线：

- 停止网络
- 消费乱序碎片
- 最终 flush
- 更新 VDL 元数据
- 关闭文件句柄

## 反向恢复出的系统设计

项目设计逻辑图：

1. CLI 或库调用方构造 `DownloadRequest`
2. `DownloadEngine` 先做远端 probe，确定大小、Range 能力和资源身份
3. 若发现已有 `.part + .config.json`，进入恢复判定
4. `RangeScheduler` 基于位图和剩余区间生成逻辑 range 与 window
5. `libcurl multi` 驱动多个 HTTP 请求并发拉取数据
6. 写回调把收到的字节封装成 `DataPacket` 投递到队列
7. `PersistenceThread` 串行处理 packet，完成乱序重排、对齐写盘和位图推进
8. 满足阈值或时间条件时，异步 flush 并保存 metadata
9. 全部完成后，将 `.part` 提升为正式文件，清理 metadata

这个设计的核心思想是：

- 网络层可以快，但不能越权宣称“已经持久化”
- 持久化层才是真正的状态真相来源
- 恢复语义不是“看文件大小猜进度”，而是建立在 `bitmap + VDL + CRC32` 上

## 仓库的实现

### 对外接口层

- `include/asyncdownload/types.hpp`
- `include/asyncdownload/client.hpp`
- `src/client.cpp`
- `src/main.cpp`

当前已经存在这些公开模型：

- `DownloadOptions`
- `DownloadRequest`
- `ProgressSnapshot`
- `PerformanceSummary`
- `DownloadResult`
- `DownloadClient`


### 下载引擎层

- `src/download/download_engine.hpp`
- `src/download/download_engine.cpp`

当前 `DownloadEngine` 已承担：

- probe
- 恢复判定
- 调度初始化
- `libcurl multi` 事件循环
- 数据 packet 投递
- 背压
- 最终收尾


### 调度层

- `src/download/range_scheduler.hpp`
- `src/download/range_scheduler.cpp`

当前调度层负责：

- 初始 range 切分
- window 派发
- 运行期 stealing
- 与 block bitmap 的配合

### 持久化层

- `src/persistence/persistence_thread.hpp`
- `src/persistence/persistence_thread.cpp`

当前持久化线程覆盖：

- 乱序包缓存
- 按 offset 串行推进
- 4KB 对齐写盘
- tail buffer 处理
- flush 与 metadata 保存调度
- `VDL` 推进

### 存储与元数据层

- `src/storage/file_writer.*`
- `src/metadata/metadata_store.*`

元数据实现是：

- `JSON + 临时文件 + rename` 的原子替换


## 当前版本对外行为

### CLI 行为

当前可执行程序支持：

```bat
build\src\Debug\AsyncDownload.exe <url> <output> [connections] [--config <path>] [--pause-on-exit] [--summary-file <path>]
```

CLI 具备这些能力：

- 从 `--config` 加载 `DownloadOptions`
- 输出实时进度
- 输出任务级 `Summary`
- 把 `Summary` 额外写入文件

### 下载产物与恢复产物

下载过程中会生成：

- `xxx.part`
- `xxx.config.json`

预期行为为：

- 成功时把 `.part` 提升为正式文件，并删除 metadata
- 失败时保留 `.part + .config.json`，用于下次恢复

### 可配置的下载参数

当前 `DownloadOptions` 已暴露的主要参数包括：

- `max_connections`
- `queue_capacity_packets`
- `scheduler_window_bytes`
- `backpressure_high_bytes`
- `backpressure_low_bytes`
- `block_size`
- `io_alignment`
- `max_gap_bytes`
- `flush_threshold_bytes`
- `flush_interval`
- `overwrite_existing`


## 测试与验证资产

测试覆盖面包括：

- block bitmap
- memory accounting
- range scheduler
- persistence thread
- file writer
- metadata store
- 下载恢复集成测试

验证方向：

- 配置文件加载
- 中断后恢复
- 基于本地 `range_server.py` 的端到端测试
- 参数调优与 benchmark 分析


## 已完成的实现

下面这些内容可以视为“已明确落地”：

- `libcurl multi` 事件循环
- packet 队列解耦
- 持久化线程独占推进落盘状态
- 4KB 对齐写盘
- `block bitmap + VDL + CRC32` 恢复语义
- CLI 进度与 summary 输出
- 配置文件驱动参数
- 单元测试与集成测试

下面这些内容更像“已有实现基础，但仍需持续核实是否完全符合最早红线”：

- 内存高水位暂停是否严格等价于最早说明书里的“Top 20% 最快连接 pause”
- gap pause、queue full pause、window boundary pause 的细节是否完全契合早期设计
- 恢复时 CRC32 样本校验链路在所有异常场景下是否足够完整
- 退出时的最终 flush、metadata 更新与句柄关闭顺序是否在所有失败分支都严格成立
- “metadata 未来可 mmap 优化”目前仍停留在规划层，而不是当前默认实现

## 开发哲学

- 正确性优先于极限吞吐
- 恢复语义优先于“表面上的下载完成”
- 网络回调必须轻量，不能把写盘复杂性带回 `WRITEFUNCTION`
- 持久化状态必须有单一真相源，避免多线程抢写
- 参数必须可测、可解释，而不是拍脑袋固定死


## 建议把这份文档当成什么来使用

这份文档更适合作为以下三类材料的结合体：

- 项目背景说明
- 开发约束总览
- 新线程接手时的总入口

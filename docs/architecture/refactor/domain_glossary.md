# AsyncDownload Ubiquitous Language

## Core terms

### Download Request

一次由 URL、目标路径和调用方提供的 `DownloadOptions` 组成的下载意图。Download
Request 在校验前不保证可执行。

### Raw Download Options

公开接口 `DownloadOptions` 中由调用方提供的原始字段。Raw Download Options 是兼容
输入，不代表运行时已经接受的策略。

### Effective Download Policy

Raw Download Options 与远端探测事实经过校验、归一化和降级后形成的不可变策略。
调度、传输、背压和持久化只能读取 Effective Download Policy。

### Remote Object Facts

HTTP 探测得到的远端对象事实，包括对象长度、Range 支持情况以及恢复身份信息。

### Transfer Window

一次 HTTP 请求负责传输的连续字节区间。Transfer Window 是 Range Lease 的网络执行
单元，不等同于完整 Range。

### Range

下载对象中按 block 边界管理的连续逻辑区间。Range 的完成以持久化事实为准。

### Range Lease

Range Lifecycle 授予传输侧的一次有限执行权，包含 window 边界和提交身份。失败或取消
时必须归还未持久化部分。

### Range Lifecycle

拥有 Range 状态及合法转换的领域模块。它接收网络事实和持久化事实，但不暴露可由调用方
任意修改的状态字段。

### Data Packet

已经发布到有序持久化数据流中的一段有位置身份的对象数据。

### Control Packet

与对象数据一同有序移交、但本身不承载对象字节的控制事实。Control Packet 不得因数据
通道暂时无法接纳而丢失。

### Packet Flow

拥有 Data Packet、Control Packet、接纳、内存计费和背压语义的进程内边界。

### Queue Admission

Packet Flow 对一次 packet 提交作出的确定性决策。它区分逻辑预算不足、内存预算不足、
底层暂时无法接纳和永久关闭。

### Accounted Bytes

为内存背压计费的字节数。它可以大于 payload bytes，但同一所有权转移只能增加和减少
各一次。

### Inflight Bytes

已经作为 Data Packet 成功发布、但尚未被 Persistence Writer 确认的对象字节数。尚未
发布的 producer-pending bytes，以及恢复时已经可信的字节，都不是 Inflight Bytes。

### Backpressure

Packet Flow 为保护逻辑队列预算或内存预算而暂停生产者，并在明确条件满足后恢复生产者
的机制。

### Queue Pause

Packet Flow 因逻辑 packet 预算暂不可用，或数据通道暂时不能接纳而暂停生产者。Queue
Pause 不等于、也不证明某个物理队列已满。

### Memory Pause

Packet Flow 为避免超过内存预算而暂停生产者。

### Gap Pause

持久化侧无法继续顺序推进 Range 时触发的暂停。Gap Pause 不属于 Packet Flow 容量背压。

### Persistence Writer

唯一拥有目标临时文件写入顺序的执行者。只有 Persistence Writer 能把网络完成转化为
持久化完成。

### Durable Checkpoint

满足既定落盘顺序并可用于恢复的一致性提交。Durable Checkpoint 同时描述 bitmap、VDL、
CRC samples 和 metadata。

### Recovery Snapshot

从已存在的临时文件与 metadata 读取到的候选恢复状态。Recovery Snapshot 只有通过身份、
结构和 CRC 校验后才可信。

### Valid Data Length

简称 VDL。临时文件中已经按顺序持久化并可直接信任的连续前缀长度。

### CRC Sample

对 VDL 之后已完成 block 保存的校验样本，用于恢复时识别内容回退。

### HTTP Probe

发现 Remote Object Facts 的 HTTP 操作。HTTP Probe 不负责下载调度。

### HTTP Transfer

拥有 HTTP 请求执行、响应验证、暂停恢复和传输错误分类的边界。

### Telemetry Session

一次 Download Request 的唯一遥测入口和状态所有者。调用方不直接依赖其内部聚合实现。

### Performance Summary

用于评价一次 Download Request 的正式固定指标集合。Performance Summary 不承载下载
业务结果或临时诊断字段。

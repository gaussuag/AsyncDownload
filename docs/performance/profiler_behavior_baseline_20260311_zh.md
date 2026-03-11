# AsyncDownload Profiler 行为基线（2026-03-11）

## 1. 目的

这份文档用于固化当前版本在 `WPR + WPA Exporter` 下的代码行为基线。

它的用途是：

- 记录当前热点路径长什么样
- 记录当前主要 pause 机制是哪一类
- 记录后续代码优化后，热点是否发生了有意义的迁移

它的用途不是：

- 作为正式性能数值基线
- 直接拿 `MB/s` 和 benchmark 结果做绝对对比

原因：

- profiler 会显著改变运行时开销
- profiler 结果更适合做“行为对比”，不适合做“吞吐基准”

## 2. 基线来源

当前第一版 profiler 行为基线来自：

- `build/profiles/20260311_160858_triage-profile-compare`

补充参考：

- `build/profiles/20260311_154656_triage-profile`

其中：

- `20260311_154656_triage-profile` 主要用于确认符号导出链已经基本可用
- `20260311_160858_triage-profile-compare` 主要用于对比 `throughput_candidate` 和 `scheduler_stress`

## 3. 测试方式

- profiler 入口:
  - `python scripts/performance/profiler.py`
- CLI:
  - `build/src/Release/AsyncDownload.exe`
- URL:
  - `http://127.0.0.1:4287/1gb_files.zip`
- 采集方式:
  - `WPR + WPA Exporter`
- WPR Profiles:
  - `GeneralProfile, CPU, DiskIO, FileIO`
- Symbol Path:
  - `build/src/Release`

说明：

- profiler 行为基线同样必须使用 `Release` 可执行文件。
- profiler 当前只加载本地 `Release` 目录下的项目符号，不依赖微软符号服务器。

## 4. 当前基线 case

当前第一版 profiler 基线主要使用两个 case：

1. `throughput_candidate`
   - `max_connections=16, scheduler_window_bytes=4MiB`
   - 用来观察高并发路径的热点结构

2. `scheduler_stress`
   - `max_connections=8, scheduler_window_bytes=16MiB`
   - 用来观察已知高风险路径的热点结构

补充参考 case：

3. `baseline_default`
   - 用来确认默认路径在 profiler 下的整体行为形态

## 5. 这份基线关注什么

后续代码优化前后，profiler 基线主要看以下问题是否变化：

1. `queue_full_pause` 是否仍然是主 pause 路径
2. `gap_pause` 是否仍然不是主问题
3. 网络回调中的 `packet` 分配与拷贝热点是否仍然明显
4. `PersistenceThread -> handle_data_packet -> append_bytes -> FileWriter::write` 是否仍然是主要消费链热点
5. `scheduler_stress` 是否仍然比 `throughput_candidate` 具有更重的 pause / enqueue / churn 特征

## 6. 当前行为基线摘要

以下数据来自 `20260311_160858_triage-profile-compare/raw_runs.csv`。

| Case | Avg Net MB/s | Queue Full Pauses | Gap Pauses | Max Memory Bytes | 主要意义 |
| --- | ---: | ---: | ---: | ---: | --- |
| `throughput_candidate` | 205.79 | 900,348 | 0 | 73,561,156 | 高并发路径在 profiler 下明显受额外开销影响，但仍可用于看热点结构 |
| `scheduler_stress` | 209.02 | 1,214,248 | 0 | 44,000,599 | 高风险路径比 throughput 具有更高的 queue-full pause 压力 |

补充参考：

| Case | Avg Net MB/s | Queue Full Pauses | Gap Pauses | Max Memory Bytes | 来源 |
| --- | ---: | ---: | ---: | ---: | --- |
| `baseline_default` | 327.68 | 757,616 | 0 | 58,588,800 | `20260311_154656_triage-profile` |

说明：

- 这些吞吐值仅用于说明 profiler 开启时的相对行为，不用于和 benchmark 基线做绝对数值比较。
- profiler 结果说明趋势时，优先看热点结构和 pause 类型，不优先看 `MB/s`。

## 7. 已确认的热点方向

### 7.1 小 packet 分配与拷贝是真实热点

在导出的 sampled CPU 栈中，已经能稳定看到：

- `AsyncDownload.exe!asyncdownload::download::DownloadEngine::run`
- `cw_out_cb_write`
- 网络写回调 lambda
- `std::vector<unsigned char>::_Assign_counted_range`
- `std::_Allocate`
- `operator new`

这说明：

- 当前每次 libcurl callback 到来后，都会产生明显的 payload 拷贝与分配成本
- `packet` 过碎带来的 per-packet 固定开销，已经不只是推测

### 7.2 持久化消费链是真实热点

在导出的 sampled CPU 栈中，也已经能稳定看到：

- `PersistenceThread::process_loop`
- `PersistenceThread::handle_packet`
- `PersistenceThread::handle_data_packet`
- `PersistenceThread::append_bytes`
- `FileWriter::write`

这说明：

- 队列出队之后的处理链本身并不轻
- 当前瓶颈不只是网络层，也不只是纯磁盘带宽

### 7.3 gap 不是当前主问题

当前 profiler 基线里：

- `throughput_candidate.gap_pause_count = 0`
- `scheduler_stress.gap_pause_count = 0`
- `baseline_default.gap_pause_count = 0`

但 `queue_full_pause_count` 都很高，而且 `scheduler_stress` 更高。

这说明：

- 当前主要不是被 `gap pause` 卡住
- 更像是系统在靠 `queue_full_pause` 做主背压

### 7.4 `scheduler_stress` 更像高 churn 风险路径

和 `throughput_candidate` 相比，`scheduler_stress` 当前更明显地表现为：

- 更高的 `queue_full_pause_count`
- 更重的对象创建/释放与回调 churn
- 更粗粒度窗口下的收敛压力

当前更合理的理解是：

- `scheduler_stress` 不是单纯“更慢”
- 它是更容易进入高 pause、高 churn 的不稳定路径

## 8. 当前不应过度解读的内容

以下内容当前不建议直接作为 profiler 基线结论：

- 单次 `MB/s` 的绝对高低
- 单次函数百分比占比的精确差值
- 系统库、内核栈的绝对时间占比

原因：

- profiler 本身会引入明显额外开销
- 本机文件过滤和安全软件会影响文件 I/O 路径
- 当前导出的系统符号并不完整，重点应放在本项目自己的热点路径

## 9. 后续如何使用这份基线

后续代码优化后，建议重新跑：

```powershell
python scripts/performance/profiler.py --url "http://127.0.0.1:4287/1gb_files.zip" --benchmark-suite regression --case-list throughput_candidate,scheduler_stress --label "post-change-profile-compare"
```

对比时重点看：

1. `queue_full_pause_count` 是否下降
2. `packet` 分配/拷贝栈是否收敛
3. `PersistenceThread -> append_bytes -> FileWriter::write` 路径是否变轻
4. `scheduler_stress` 是否不再比 `throughput_candidate` 表现出更高的 churn

## 10. 当前版本结论

截至 `2026-03-11`：

- `20260311_160858_triage-profile-compare` 可以作为第一版 profiler 行为基线存档
- 这份 profiler 基线用于行为与热点结构对比，不用于正式性能数值回归
- 当前已确认的两个主要热点方向是：
  - 小 packet 分配/拷贝
  - 持久化线程 per-packet 消费链
- 当前主要 pause 类型仍然是 `queue_full_pause`，不是 `gap_pause`
- 后续代码优化时，优先观察这两条热点路径是否变轻，而不是优先比较 profiler 下的绝对吞吐

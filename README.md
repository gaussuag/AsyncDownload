# AsyncDownload

一个基于 C++20、`libcurl multi`、`BlockingConcurrentQueue` 和自定义持久化线程模型实现的高性能 HTTP 下载器。

当前仓库已经具备这些核心能力：

- 多连接 Range 下载
- `.part + config.json` 断点续传
- `VDL + CRC32` 恢复校验
- 主动背压
- gap 熔断
- 4KB 对齐写盘
- CLI 进度输出
- `--config` 配置文件加载
- summary 性能汇总输出
- 单元测试与集成测试

## 项目定位

这个项目不是一个通用 C++ 模板仓库，而是一个围绕“可恢复、可观察、可扩展”目标实现的下载引擎。

它的设计重点是：

- 把网络接收和磁盘写入彻底解耦
- 让 libcurl 回调保持非阻塞
- 让持久化线程独占推进落盘状态
- 把恢复语义建立在 `bitmap + VDL + CRC` 之上

## 快速开始

### 环境要求

- Windows
- Visual Studio 2022
- `vcpkg`
- 已设置环境变量 `VCPKG_ROOT`
- Python 3

说明：

- 构建依赖由 `vcpkg.json` 管理，当前使用 `curl` 和 `nlohmann-json`
- Windows 集成测试会启动 `tests/support/range_server.py`

### 构建

```bat
scripts\build.bat
```

### 构建 Release

```bat
scripts\build.bat release
```

### 清理构建目录

```bat
scripts\build.bat clean
```

### 运行测试

```bat
build\tests\Debug\AsyncDownload_tests.exe
```

### 运行 CLI

```bat
build\src\Debug\AsyncDownload.exe <url> <output> [connections] [--config <path>] [--pause-on-exit] [--summary-file <path>]
```

示例：

```bat
build\src\Debug\AsyncDownload.exe "https://example.com/file.bin" "build\src\Debug\file.bin" 4
build\src\Debug\AsyncDownload.exe "https://example.com/file.bin" "build\src\Debug\file.bin" --config configs\download_options.template.json --summary-file build\src\Debug\summary.txt
```

### 配置文件模板

仓库提供了一个可直接复制调整的配置模板：

```text
configs/download_options.template.json
```

CLI 会优先从 `--config` 加载 `DownloadOptions`，然后再应用命令行里的 `connections`
覆盖。

## CLI 输出说明

CLI 当前会周期性输出这些信息：

- `downloaded`
  从网络层收到并成功进入持久化链路的数据量
- `persisted`
  已经物理写入 `.part` 文件的数据量
- `vdl`
  当前最长连续安全落盘前沿
- `inflight`
  已下载但尚未落盘的数据量
- `queued`
  队列里等待持久化的 packet 数量
- `active`
  当前活跃请求数量
- `paused`
  当前暂停中的 range 数量
- `net`
  网络接收速度，单位 `MB/s`
- `disk`
  磁盘写入速度，单位 `MB/s`
- `memory`
  当前下载链路统计到的内存占用
- `progress`
  基于 `persisted_bytes / total_bytes` 计算出的整体进度百分比

CLI 结束后还会输出一段 `Summary`，包含：

- 总耗时、首包时间、首落盘时间
- 平均/峰值网络与磁盘速度
- 恢复复用字节数
- 内存、inflight、队列峰值
- pause 次数、window/range 数量
- flush 和 metadata 保存统计

## 恢复文件说明

下载过程中会在目标文件旁生成：

- `xxx.part`
- `xxx.config.json`

行为规则：

- 下载成功时，会把 `.part` 提升成正式文件，并删除 `.config.json`
- 下载失败时，会保留 `.part + .config.json` 供下次恢复
- 重新执行同一条命令时，若远端资源身份和本地 metadata 一致，则会自动尝试续传

## 目录说明

```text
AsyncDownload/
├── include/asyncdownload/        # 对外公开接口
├── src/
│   ├── download/                 # 下载引擎、调度器、HTTP probe
│   ├── persistence/              # 持久化线程
│   ├── storage/                  # 文件读写与预分配
│   ├── metadata/                 # config.json 读写
│   └── core/                     # 共享状态、位图、CRC、内存会计
├── tests/                        # 单元测试与集成测试
├── docs/                         # 中文设计文档与 Mermaid 流程图
└── scripts/                      # 构建脚本
```

## 关键模块

- `DownloadEngine`
  负责编排一次完整下载任务，包括 probe、恢复判定、调度、事件循环和收尾
- `RangeScheduler`
  负责初始切分、window 派发和安全简化版 work stealing
- `PersistenceThread`
  负责乱序重排、对齐写盘、位图推进、flush、metadata 和 VDL
- `FileWriter`
  负责偏移读写、预分配、flush 和最终 rename
- `MetadataStore`
  负责 `config.json` 的保存与恢复

## 文档入口

如果你是第一次看这个项目，建议按下面顺序阅读：

1. [架构设计说明](docs/architecture_zh.md)
2. [Mermaid 流程图](docs/flowcharts_zh.md)
3. [Benchmark 使用说明](docs/benchmark_guide_zh.md)
4. [重写验收测试规范](docs/reimplementation_test_spec_zh.md)
5. `include/asyncdownload/types.hpp`
6. `src/download/download_engine.cpp`
7. `src/persistence/persistence_thread.cpp`

补充文档：

- [vcpkg 依赖管理问题报告](docs/vcpkg-dependency-issues.md)

## 当前状态

当前实现已经打通真实下载、续传、恢复和自动化测试链路，但仍然不是“所有理想规格都做到极限”的终态。当前更准确的定位是：

- 工程上可运行、可验证、可继续扩展
- 恢复语义和持久化语义已经比较完整
- 某些性能路径仍然偏保守正确，而不是极限优化

## 开发与验证

常用命令：

```bat
scripts\build.bat
build\tests\Debug\AsyncDownload_tests.exe
build\tests\Debug\AsyncDownload_tests.exe --gtest_list_tests
build\tests\Debug\AsyncDownload_tests.exe --gtest_filter=DownloadIntegrationTest.LoadsDownloadOptionsFromConfigFile
build\tests\Debug\AsyncDownload_tests.exe --gtest_filter=DownloadIntegrationTest.ResumeAfterInterruptedCliDownload
build\src\Debug\AsyncDownload.exe "https://example.com/file.bin" "out.bin" --config configs\download_options.template.json --summary-file summary.txt
```

如果你准备继续改代码，建议优先关注这些文件：

- `src/download/download_engine.cpp`
- `src/persistence/persistence_thread.cpp`
- `src/download/range_scheduler.cpp`
- `src/core/block_bitmap.cpp`
- `src/storage/file_writer.cpp`


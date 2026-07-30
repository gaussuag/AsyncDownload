# WF-001：Validated Download Policy

- Type: `task`
- Status: `resolved`
- Depends on: `WF-000`
- Produces: `phases/01_validated_download_policy.md`

## Question

如何保持 `DownloadOptions` 源码兼容，同时让所有跨字段不变量只在一个 module 中校验和
归一化？

## Investigation boundary

- 覆盖 alignment、watermark、连接数、window、queue 与远端 Range 能力。
- 不改变当前合法配置的有效行为。
- 不增加公开配置字段。

## Close criteria

- 给出推荐 interface、错误模型、不可变性和迁移顺序。
- 给出非法组合与非 Range 降级测试矩阵。
- 每一小步有回滚方法。

## Resolution

采用两阶段、不可变的内部 policy interface：

- `validate_download_options(DownloadOptions)` 在任何 I/O 前生成
  `ValidatedDownloadPolicy`，集中验证所有单字段和跨字段不变量；
- `bind_remote_facts(...)` 在 probe 后生成 `EffectiveDownloadPolicy`，用独立类型表达
  Range 与非 Range 的有效连接、window、steal 和 sparse-resume 能力；
- public `DownloadOptions` 保持原样，只作为 raw compatibility input；
- 非 Range 模式固定为单连接、整对象 window、禁用 steal 和 sparse resume，但不修改 raw
  options；
- `io_alignment` 必须是 2 的幂、不超过固定 4096-byte TailBuffer，且整除 `block_size`；
- scheduler、bitmap、memory admission 和 queue count使用 checked arithmetic；
- 所有非法 options 对 public caller 统一映射到现有 `invalid_request`，内部保留精确 reason。

完整 interface、错误表、迁移 slices、测试矩阵、验收和逐步回滚见
[阶段 1：Validated Download Policy](../phases/01_validated_download_policy.md)。

关闭证据：已核实 public options、CLI 合并、engine non-Range 原地修改、scheduler arithmetic、
固定 TailBuffer、persistence copy、metadata identity、vendored queue constructor 和现有测试。
本票只产出实现契约，未修改生产代码。

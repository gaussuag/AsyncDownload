# AsyncDownload 渐进式架构重构

本目录是交付给 Code Agent 的实现合同。目标不是一次性改写整个项目，而是在保持公开
接口、CLI、恢复文件与性能口径兼容的前提下，依次建立 6 个更深的模块。

## 阅读顺序

1. [Wayfinder 路线图](wayfinder_map.md)
2. [阶段 0：特征化与兼容基线](phases/00_characterization_baseline.md)
3. [阶段 1：Validated Download Policy](phases/01_validated_download_policy.md)
4. [阶段 2：Packet Flow / Backpressure](phases/02_packet_flow_backpressure.md)
5. [阶段 3：Range Lifecycle](phases/03_range_lifecycle.md)
6. [阶段 4：Recovery Checkpoint](phases/04_recovery_checkpoint.md)
7. [阶段 5：HTTP Transfer](phases/05_http_transfer.md)
8. [阶段 6：Telemetry Session](phases/06_telemetry_session.md)
9. [Code Agent 交接说明](code_agent_handoff.md)

领域术语以 [domain_glossary.md](domain_glossary.md) 为准。每个阶段必须独立交付、
验证，并在进入下一阶段前形成独立回滚点；不得为了让当前阶段通过而预先依赖下一阶段。
frontier 已推进后，回滚早期阶段必须先按逆序回滚所有 dependents。

## 规范词

- **MUST**：合并前必须满足。
- **MUST NOT**：明确禁止。
- **SHOULD**：默认选择；偏离时必须在阶段变更说明中给出证据。
- **MAY**：不影响合同的实现选择。

## 总体兼容合同

- `include/asyncdownload/client.hpp` 与 `include/asyncdownload/types.hpp` 的现有公开调用方式
  保持源码兼容。
- CLI 参数、配置字段、退出码及当前正式 10 项性能 summary key 保持兼容。
- `.part`、`.config.json`、bitmap、VDL 和 CRC sample 的现有恢复语义保持兼容。
- 继续使用 C++20、libcurl、单 persistence writer 和无异常错误处理。
- 不把历史上已拒绝的性能实验混入结构重构。

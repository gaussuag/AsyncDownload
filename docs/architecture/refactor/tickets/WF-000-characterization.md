# WF-000：特征化与兼容基线

- Type: `research`
- Status: `resolved`
- Depends on: none
- Produces: `phases/00_characterization_baseline.md`

## Question

在改变 module seam 之前，哪些公开、恢复、线程、第三方和性能行为必须由可重复证据锁定？

## Investigation boundary

- 只新增或整理测试与基线，不改变生产行为。
- 明确当前 Windows 测试环境中 `error=740` 的已知基础设施限制。
- 固化正式 10 项 benchmark schema 和历史 reject 清单。

## Close criteria

- 有逐阶段复用的测试矩阵。
- 有兼容合同、基准提交和回滚锚点。
- 明确哪些结果属于现有失败，哪些属于重构回归。

## Resolution

已在 `phases/00_characterization_baseline.md` 固化基线提交、公开/CLI/恢复/HTTP/线程/性能
兼容合同、40 个现有测试的环境差异、逐阶段特征化矩阵、Release benchmark gate、证据包和
回滚规则。本 ticket 只定义观察面，不授权生产行为变化。

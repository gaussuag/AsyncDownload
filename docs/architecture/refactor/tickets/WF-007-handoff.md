# WF-007：Code Agent 交接

- Type: `task`
- Status: `resolved`
- Depends on: `WF-006`
- Produces: `code_agent_handoff.md`

## Question

Code Agent 如何在不丢失约束、不扩大范围的情况下，逐阶段完成、验证和回滚重构？

## Investigation boundary

- 汇总阶段依赖、提交粒度、验证命令、停止条件和证据模板。
- 不重复阶段文档中的完整设计。

## Close criteria

- Code Agent 可以从单一入口开始工作。
- 每个阶段有明确输入、输出、禁止项和完成定义。
- 失败时可以定位到最近阶段并安全回滚。

## Resolution

已完成 [`../code_agent_handoff.md`](../code_agent_handoff.md)，将全部阶段合同收敛为一个
Code Agent 执行入口：

- 固定 0 → 1 → 2 → 3 → 4 → 5 → 6 串行顺序和每阶段解锁条件；
- 定义 tests-first、interface migration、legacy deletion、完整 gate 与 evidence package；
- 用 S/C/P/F/M 分类隔离结构迁移、correctness 修复、性能实验、功能与格式变化；
- 给出逐 commit 和逐 stage 验证、停止条件、历史禁止项及可直接复用的执行 prompt；
- 定义 rollback frontier：当前 tip stage 可独立回滚，已有 dependents 时按逆阶段顺序
  回滚；
- 明确 public/CLI/recovery/HTTP/10-key performance compatibility 与已知 Windows
  `error=740` 分类。

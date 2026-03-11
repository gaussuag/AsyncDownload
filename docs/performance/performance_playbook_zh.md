# AsyncDownload 性能优化寻路指南

## 1. 目的

这份文档是 `docs/performance` 目录的总入口。

它的目标是让任何一个新接手性能优化任务的 agent，都能快速理解：

- 这个目录下每份文档分别是做什么的
- 哪份文档是当前正式基线
- 哪份文档是历史基线
- 哪份文档记录优化历史
- 哪份文档指导日常回归
- 哪份文档记录 profiler 行为基线
- 做完新的性能优化后，应该更新哪些文档

如果你是第一次接手这个项目的性能优化工作，**先读这份文档，再去读其他文档**。

## 2. 目录结构与文档职责

当前 `docs/performance` 下的文档有：

### 2.1 正式 benchmark 基线

- [performance_baseline_20260311_regression_v2_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/performance_baseline_20260311_regression_v2_zh.md)

作用：

- 这是当前**正式主 benchmark 基线**
- 代表聚合缓冲优化落地之后，后续继续优化时应该优先参考的基准
- 当前建议后续优化都基于 `regression_v2` 做 before/after 对比

你应该在这些情况下优先看它：

- 想知道当前正式回归套件是什么
- 想知道后续主要对比哪些 case
- 想知道当前最重要的性能结论是什么

### 2.2 聚合前历史 benchmark 基线

- [performance_baseline_20260310_regression_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/performance_baseline_20260310_regression_zh.md)

作用：

- 这是**聚合缓冲优化前**的 `Release regression` 历史基线
- 不再是后续继续优化时的主基线
- 它的主要价值是帮助理解“第一次性能优化前，系统长什么样”

你应该在这些情况下看它：

- 想回顾第一次性能优化前的系统状态
- 想对比“第一次性能优化到底改变了什么”
- 想理解旧 `regression` 为什么被降级为历史对照套件

### 2.3 性能优化历史

- [performance_optimization_history_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/performance_optimization_history_zh.md)

作用：

- 这是**优化迭代日志**
- 记录每一次性能优化做了什么、为什么这么做、前后指标如何变化
- 当前已经收录了第一次性能优化：`TransferHandle` 聚合缓冲

你应该在这些情况下看它：

- 想知道某个优化为什么被引入
- 想知道优化前后结论是如何变化的
- 想避免后续 agent 重复做已经证明不值得的方向

### 2.4 回归与防劣化指南

- [optimization_regression_guide_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/optimization_regression_guide_zh.md)

作用：

- 这是**日常性能回归操作指南**
- 说明改代码前后怎么跑 benchmark、看哪些指标、怎么判定是否退化
- 当前已经切换到以 `regression_v2` 为主

你应该在这些情况下看它：

- 想做一次正规的 before/after 回归
- 想知道哪些 case 是主观察点
- 想知道什么变化该视为退化，什么变化可以接受

### 2.5 profiler 行为基线

- [profiler_behavior_baseline_20260311_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/profiler_behavior_baseline_20260311_zh.md)

作用：

- 这是**代码行为基线**，不是性能数值基线
- 记录的是热点路径、pause 类型、函数栈方向
- 主要用于对比“优化后热点是否迁移”

你应该在这些情况下看它：

- 想知道当前 profiler 观察到了哪些热点
- 想知道下一轮 profiler 该重点对比哪些函数路径
- 想知道为什么 profiler 不适合作为正式吞吐基线

### 2.6 优化方案说明

- [transfer_handle_packet_aggregation_plan_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/transfer_handle_packet_aggregation_plan_zh.md)

作用：

- 这是某一个具体优化方向的技术设计说明
- 当前记录的是第一次优化的方案文档：`TransferHandle` 聚合缓冲

你应该在这些情况下看它：

- 想理解这个优化方案的设计边界
- 想回顾这个优化原本的目标和约束
- 想在后续做同类优化时复用类似的说明结构

## 3. 当前文档体系的角色划分

可以把这些文档理解成 4 类：

### 3.1 基线类

- [performance_baseline_20260311_regression_v2_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/performance_baseline_20260311_regression_v2_zh.md)
- [performance_baseline_20260310_regression_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/performance_baseline_20260310_regression_zh.md)

职责：

- 保存性能数字
- 保存 case 的解释
- 保存某个阶段的正式结论

### 3.2 流程类

- [optimization_regression_guide_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/optimization_regression_guide_zh.md)

职责：

- 告诉你怎么跑
- 告诉你怎么看
- 告诉你什么叫退化

### 3.3 历史类

- [performance_optimization_history_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/performance_optimization_history_zh.md)

职责：

- 记录“做过什么”
- 记录“为什么这么做”
- 记录“效果如何”

### 3.4 行为与设计类

- [profiler_behavior_baseline_20260311_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/profiler_behavior_baseline_20260311_zh.md)
- [transfer_handle_packet_aggregation_plan_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/transfer_handle_packet_aggregation_plan_zh.md)

职责：

- 一个描述“当前热点与行为”
- 一个描述“某次优化的技术方案”

## 4. 新 agent 的建议阅读顺序

如果一个新的 agent 要接手性能优化任务，建议按这个顺序看：

1. [performance_playbook_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/performance_playbook_zh.md)
   先建立整体地图

2. [performance_baseline_20260311_regression_v2_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/performance_baseline_20260311_regression_v2_zh.md)
   理解当前正式 benchmark 基线和主要 case

3. [optimization_regression_guide_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/optimization_regression_guide_zh.md)
   理解日常回归流程和判定标准

4. [performance_optimization_history_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/performance_optimization_history_zh.md)
   理解已经做过的优化、收益和教训

5. [profiler_behavior_baseline_20260311_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/profiler_behavior_baseline_20260311_zh.md)
   理解热点与行为基线

6. [transfer_handle_packet_aggregation_plan_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/transfer_handle_packet_aggregation_plan_zh.md)
   理解第一次优化的原始技术设计

7. [performance_baseline_20260310_regression_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/performance_baseline_20260310_regression_zh.md)
   如果需要，再回头补看聚合前历史基线

## 5. 当前应以哪份文档为准

### 5.1 想知道“现在该用哪套 benchmark”

看：

- [performance_baseline_20260311_regression_v2_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/performance_baseline_20260311_regression_v2_zh.md)
- [optimization_regression_guide_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/optimization_regression_guide_zh.md)

当前答案是：

- 主套件：`regression_v2`
- 历史对照：`regression`
- 必须使用 `Release`

### 5.2 想知道“以前为什么这么改”

看：

- [performance_optimization_history_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/performance_optimization_history_zh.md)

### 5.3 想知道“当前热点是什么”

看：

- [profiler_behavior_baseline_20260311_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/profiler_behavior_baseline_20260311_zh.md)

### 5.4 想知道“第一次优化前系统是什么样”

看：

- [performance_baseline_20260310_regression_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/performance_baseline_20260310_regression_zh.md)

## 6. 什么时候更新哪些文档

### 6.1 做完一次新的正式 benchmark 基线复测

如果新的 benchmark 结果会成为后续正式比较依据，应更新：

- [performance_baseline_20260311_regression_v2_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/performance_baseline_20260311_regression_v2_zh.md)

如果结论变化影响回归策略，也要同步更新：

- [optimization_regression_guide_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/optimization_regression_guide_zh.md)

### 6.2 做完一次新的性能优化

如果代码已经产生实际性能变化，应该更新：

- [performance_optimization_history_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/performance_optimization_history_zh.md)

如果新的优化结果改变了正式 benchmark 结论，也要更新：

- [performance_baseline_20260311_regression_v2_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/performance_baseline_20260311_regression_v2_zh.md)
- [optimization_regression_guide_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/optimization_regression_guide_zh.md)

### 6.3 做完一次新的 profiler 基线采集

如果新的 profiler 结果要作为后续行为对照基线，应更新：

- [profiler_behavior_baseline_20260311_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/profiler_behavior_baseline_20260311_zh.md)

### 6.4 设计了新的优化方案，但还没动代码

如果只是提出新的技术方案，应新增或更新方案文档，例如：

- 某个新的 `*_plan_zh.md`

如果方案后来真正落地并产生结果，再把它补进：

- [performance_optimization_history_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/performance_optimization_history_zh.md)

## 7. 文档维护规则

后续维护这个目录时，建议遵守下面几条规则：

### 7.1 不要覆盖历史

- 旧基线不要直接删
- 旧优化记录不要直接改写成“当前状态”
- 新阶段基线应新增文档，或在历史文档中明确降级为“历史基线”

### 7.2 明确文档角色

如果新增文档，要明确它属于哪一类：

- benchmark 基线
- profiler 行为基线
- 回归指南
- 优化历史
- 技术方案

不要把它们混成一份“大杂烩”文档。

### 7.3 基线文档优先保存整理后的结论

不要依赖 `build/` 目录长期存在。

每次确认一个新的正式基线后，应把关键数据写进文档，包括：

- 来源目录名
- suite 名
- repeats
- 关键 case 指标
- 当前结论

### 7.4 优化历史文档优先保存“前后变化”

每次记录一次优化时，至少写清楚：

- 为什么改
- 改了什么
- 用哪组 benchmark/profiler 验证
- 结果怎么变化
- 哪些理解被证实了
- 哪些理解被修正了

### 7.5 回归指南必须和当前正式基线一致

如果主 benchmark 套件变了，或者主 case 的优先级变了，必须同步更新：

- [optimization_regression_guide_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/optimization_regression_guide_zh.md)

否则后面的 agent 很容易用错套件。

## 8. 当前接手建议

截至目前，如果一个新的 agent 要继续做性能优化，最推荐的出发点是：

1. 先读 [performance_baseline_20260311_regression_v2_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/performance_baseline_20260311_regression_v2_zh.md)
2. 再读 [performance_optimization_history_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/performance_optimization_history_zh.md)
3. 用 [optimization_regression_guide_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/optimization_regression_guide_zh.md) 约束新的 before/after 验证
4. 如果要做热点定位，再看 [profiler_behavior_baseline_20260311_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/profiler_behavior_baseline_20260311_zh.md)

当前阶段，后续优化的主目标已经比较清楚：

- 降低高 pause churn
- 保住 `balanced_candidate` 的收益
- 保住 `memory_guard` 的低内存优势
- 继续观察 `scheduler_stress` 的稳定性

## 9. 当前版本结论

这份“寻路指南”本身不定义性能结论，它定义的是：

- 这个目录的结构
- 文档之间的关系
- 后续 agent 应该如何阅读、更新和维护这些文档

如果目录结构或文档职责发生变化，应优先更新这份文档。

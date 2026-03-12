# AsyncDownload 性能优化 Playbook

## 1. 目的

这份文档是 `docs/performance` 的入口文档。

它只承担 3 类职责：

- 告诉接手者这个目录里每份文档各自负责什么
- 给出当前仍然有效、且足够稳定的性能工作上下文
- 说明后续性能优化时，应该如何阅读、更新和维护这些文档

它**不是**正式 benchmark 基线，也**不是** profiler 结论文档。  
具体数字、当前主回归套件、当前主 case、热点判断，应以对应的基线文档和指南文档为准。

## 2. 当前稳定上下文

截至当前版本，下面这些上下文是稳定且必须知道的：

- 性能优化已经从“临时试验”进入“持续迭代”阶段，不能只依赖 `build/` 下的测试结果目录保存上下文。
- 当前性能工作同时依赖两类基线：
  - benchmark 基线：用于正式性能回归
  - profiler 行为基线：用于热点路径和行为结构对比
- 当前项目已经完成至少一轮真实性能优化，并且已经把“优化前后变化”沉淀进文档。
- 当前性能优化流程应以 benchmark 为主驱动，以 profiler 为辅助确认。
  - benchmark 先回答收益与退化。
  - profiler 再回答热点与行为路径。
  - 二者应串行执行，不应在 benchmark 执行期间同时开启 profiler。
- 性能优化工作默认应按“单主题单线程”推进。
  - 一轮优化闭环结束后，应优先把结论写入文档，再考虑新开 thread 进入下一个主瓶颈。
- 新 agent 接手时，应先从 `docs/performance` 恢复上下文，而不是依赖旧 thread 的长历史。

这些规则如果发生变化，应优先更新这份 Playbook。

## 3. 文档地图

### 3.1 正式 benchmark 基线

- [performance_baseline_20260311_regression_v2_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/performance_baseline_20260311_regression_v2_zh.md)

职责：

- 记录当前正式 benchmark 基线
- 记录当前正式对比套件与关键 case
- 记录当前应以什么 benchmark 结论为准

何时看它：

- 想知道当前正式基线是什么
- 想知道后续 before/after 应该基于什么比较
- 想知道当前最重要的 benchmark 结论

### 3.2 历史 benchmark 基线

- [performance_baseline_20260310_regression_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/performance_baseline_20260310_regression_zh.md)

职责：

- 保存聚合前的历史基线
- 解释为什么旧套件只作为历史对照

何时看它：

- 想回顾第一次性能优化前系统是什么样
- 想比较“第一次优化到底改变了什么”

### 3.3 回归与防劣化指南

- [optimization_regression_guide_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/optimization_regression_guide_zh.md)

职责：

- 说明后续优化前后应该如何跑 benchmark
- 说明主要观察哪些 case、哪些指标
- 说明什么情况应视为退化、什么情况可以接受
- 说明 benchmark 与 profiler 的主次关系和执行顺序

何时看它：

- 想做一次正式 before/after 回归
- 想知道当前回归套件和判定规则

### 3.4 性能优化历史

- [performance_optimization_history_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/performance_optimization_history_zh.md)

职责：

- 记录每次性能优化做了什么
- 记录为什么这么做
- 记录前后 benchmark/profiler 结果如何变化

何时看它：

- 想知道某个方向以前做过没有
- 想知道以前的理解后来有没有被修正

### 3.5 profiler 行为基线

- [profiler_behavior_baseline_20260311_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/profiler_behavior_baseline_20260311_zh.md)

职责：

- 记录热点路径和行为结构
- 记录 profiler 应该拿什么做后续对照
- 不负责单独定义性能收益结论

何时看它：

- 想知道当前热点路径是什么
- 想知道下一轮 profiler 应该重点盯哪里

### 3.6 thread 初始化文档

- [performance_thread_initialization_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/performance_thread_initialization_zh.md)

职责：

- 固定当前性能开发 thread 的正式起点
- 统一当前 thread 默认沿用的基线、关注 case、验证入口和边界
- 避免后续讨论混用旧 thread 的临时结论

何时看它：

- 想接手当前正在进行的性能开发 thread
- 想确认这个 thread 默认从哪份基线继续
- 想确认本 thread 已约定的首阶段目标和验证顺序

### 3.7 具体优化方案文档

- [transfer_handle_packet_aggregation_plan_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/transfer_handle_packet_aggregation_plan_zh.md)

职责：

- 保存某个具体优化方向的设计说明
- 解释该方案的边界、约束和验证思路

何时看它：

- 想理解某个具体优化方案为什么这样设计
- 想复用同类方案文档的写法

## 4. 新 agent 的推荐阅读顺序

如果一个新的 agent 要接手性能优化任务，建议按下面顺序阅读：

1. [performance_playbook_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/performance_playbook_zh.md)
2. [performance_thread_initialization_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/performance_thread_initialization_zh.md)
3. [performance_baseline_20260311_regression_v2_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/performance_baseline_20260311_regression_v2_zh.md)
4. [optimization_regression_guide_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/optimization_regression_guide_zh.md)
5. [performance_optimization_history_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/performance_optimization_history_zh.md)
6. [profiler_behavior_baseline_20260311_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/profiler_behavior_baseline_20260311_zh.md)

如果当前任务与某个已存在方案直接相关，再补读对应方案文档。

## 5. 当前目录的表述逻辑

这个目录里的文档应保持下面的逻辑分工：

- 基线文档：给结论和数字
- 回归指南：给验证流程和判定规则
- 优化历史：给演进过程
- profiler 基线：给热点行为结构
- 线程初始化文档：给当前 thread 的起点和默认边界
- 方案文档：给某个具体优化的设计说明
- Playbook：给地图、上下文和维护规则

如果某份文档开始同时承担多个职责，应优先收敛，而不是继续叠加内容。

## 6. 文档更新规则

### 6.1 做完一次新的正式 benchmark 验证

如果新的 benchmark 结果会成为后续正式比较依据，应更新：

- 正式 benchmark 基线文档

如果新的结果改变了回归判定方式或主要观察点，还要更新：

- 回归与防劣化指南

### 6.2 做完一次新的性能优化

无论优化幅度大小，只要代码和性能结论发生了有效变化，都应更新：

- [performance_optimization_history_zh.md](/D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/performance_optimization_history_zh.md)

如果这次优化改变了正式 benchmark 结论，还要同步更新：

- 正式 benchmark 基线文档
- 回归与防劣化指南

### 6.3 做完一次新的 profiler 基线采集

如果新的 profiler 结果要作为后续热点对照基线，应更新：

- profiler 行为基线文档

### 6.4 新增一个长期存在的性能文档

如果新增一份长期保留的性能文档，应同步更新这份 Playbook，补上：

- 文档职责
- 推荐阅读顺序中的位置
- 何时使用它

### 6.5 自主检查关联文档

文档维护应按“全局一致性”处理，而不是按“只改当前这份文档”处理。

每次更新 `docs/performance` 中任意一份文档时，都应主动判断这次变化是否会影响：

- 正式 benchmark 基线文档
- 历史 benchmark 基线文档
- 回归与防劣化指南
- 优化历史文档
- profiler 行为基线文档
- Playbook 本身

至少要问自己这几个问题：

- 当前结论是否改变了正式基线的解释
- 当前变化是否改变了回归套件、主观察 case 或判定方式
- 当前变化是否应该写进优化历史，而不是只留在基线文档里
- 当前变化是否让 Playbook 中的文档职责、阅读顺序或维护规则失效

如果答案可能是“会影响”，就应在同一轮工作里同步更新关联文档，而不是把不一致留到以后。

## 7. 维护约束

### 7.1 不要依赖 `build/`

`build/` 目录中的 benchmark 和 profiler 结果可以随时被清理。  
关键数字、关键判断、关键演进过程必须进入 `docs/performance`。

### 7.2 不要把当前结论复制到多个地方

当前正式 suite、当前主要 case、当前热点判断，只应在“最合适的那份文档”里维护。

Playbook 可以告诉你“去哪看”，但不应该成为这些结论的第二份拷贝。

### 7.3 文档维护是全局工作

文档更新不是局部补丁动作，而是全局维护动作。

一次性能优化、一次基线变更、一次 profiler 结论修正，往往会同时影响多份文档。

因此，维护者应主动检查关联文档，而不是等待别人之后再补。

### 7.4 不要覆盖历史

- 旧基线保留为历史
- 旧优化记录保留为历史
- 如果进入新阶段，应新增正式基线或明确降级旧文档的角色

### 7.5 一轮优化闭环后再切线程

一轮性能优化如果已经完成下面这些步骤：

- 目标明确
- 代码改动完成
- benchmark 回归完成
- profiler 对照完成（如有必要）
- 文档更新完成

则应优先视为一个闭环。  
如果后续主瓶颈已经变化，建议新开 thread 继续，而不是在旧 thread 里无限滚动。

## 8. 当前接手建议

如果今天有一个新的 agent 接手性能优化任务，最稳的起手方式是：

1. 先读这份 Playbook
2. 再读当前正式 benchmark 基线
3. 再读回归与防劣化指南
4. 再读优化历史
5. 如果涉及热点判断，再读 profiler 行为基线
6. 在开始改代码前，先和用户确认这轮优化目标

这份文档的目标不是替代其他文档，而是保证任何接手者都知道：

- 当前上下文该从哪里恢复
- 当前规则该从哪里确认
- 做完工作后该更新什么

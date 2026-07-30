# WF-004：Recovery Checkpoint

- Type: `task`
- Status: `resolved`
- Depends on: `WF-003`
- Produces: `phases/04_recovery_checkpoint.md`

## Question

如何让 Durable Checkpoint 的提交顺序、恢复可信度和清理错误只存在于一个 module？

## Investigation boundary

- 保持 `.part + .config.json + bitmap + VDL + CRC32` 格式兼容。
- 保持 persistence 为完成事实的唯一来源。
- 不调优 threshold/interval 或引入已拒绝的 CRC cache 实验；丢失 forced request 的修复
  必须独立分类和验证。

## Close criteria

- 定义 checkpoint 保存、加载、验证、完成和清理 interface。
- 定义崩溃点矩阵与恢复结果。
- 覆盖 metadata/file/CRC 错误传播与回滚。

## Resolution

选择“owning Recovery Checkpoint”深模块：`DownloadEngine` unique-own，
`PersistenceThread` bounded borrow，module private-own `FileWriter` 与 `MetadataStore`。
详细实现合同见
[阶段 4：Recovery Checkpoint](../phases/04_recovery_checkpoint.md)。

结论：

- valid legacy `.part + .config.json + bitmap + VDL + CRC32` schema 保持不变；
- Phase 3 `RangeWriteState` 是 range persisted snapshot 的唯一事实来源；
- checkpoint image 在 Persistence thread 冻结，worker 不读取 live bitmap/Lifecycle；
- commit 顺序固定为 flush → frozen-image CRC → metadata replace → exact VDL publish；
- serialized VDL 前存在 bitmap/range projection hole 时按 malformed metadata 拒绝，不能继续
  无 CRC 信任 hole 后 block；
- pending commit 期间的 range-complete/shutdown force 使用 sticky intent，在当前 future
  get 后提交 mandatory successor image；
- normal 与 resume-complete 共用同一 finalize interface；
- resume-complete 的 `completed_ranges` 显式保持 required block count；
- non-Range incomplete/holey state clean restart，complete state direct finalize；
- metadata remove 结果区分 removed/not-found/failed，promotion 后 cleanup failure 不覆盖成功；
- VDL-prefix hole、live VDL、fresh restart stale-pair、CRC read skip、replace gap、lost
  forced checkpoint 和 overwrite name gap 都作为独立 C 类 correctness commits，不能隐藏
  在结构搬迁中；
- threshold/interval 数值、incremental CRC、compact JSON 和 handle splitting 明确禁止改变；
- 迁移拆为 15 个 tests-first slices，包含真实 temp filesystem、fault/crash matrix、
  integration、Release benchmark 和逐 slice rollback。

核实证据来自 `DownloadEngine` recovery helpers、`PersistenceThread` checkpoint worker、
`MetadataStore`、`FileWriter`、`AtomicBlockBitmap`、Phase 1–3 contracts、现有 resume tests
和性能历史中被拒绝的 flush/CRC experiments。

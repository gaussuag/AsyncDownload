# WF-005：HTTP Transfer

- Type: `task`
- Status: `resolved`
- Depends on: `WF-004`
- Produces: `phases/05_http_transfer.md`

## Question

如何隐藏 libcurl probe、Range 请求、暂停重放、响应验证和错误映射，同时不把调度领域规则
搬进 HTTP module？

## Investigation boundary

- 依据本地 libcurl source/docs 锁定 callback 与 pause 契约。
- 保留 HEAD → Range fallback、HTTP/1.1 和独立物理连接语义。
- 不增加 retry、HTTP/2、自动解压或认证功能。

## Close criteria

- 定义 probe 与 transfer interface、事件和错误分类。
- 定义 callback 数据所有权、pause/unpause 与 replay 去重责任。
- 给出生产 adapter、测试 fake 和真实服务器合同测试。

## Resolution

详见 [`../phases/05_http_transfer.md`](../phases/05_http_transfer.md)。

结论：

- 采用高层 command/event port：`HttpTransferPort` 负责 probe 和创建 session，
  `HttpTransferSession` 负责 transfer 生命周期；生产环境只有 `CurlHttpTransferPort`，
  测试环境只有最小 `DeterministicHttpTransferPort`。
- 依据本地 vcpkg 的 libcurl 8.18.0 source/docs 锁定 write callback 的 pause/replay、
  unpause 同步重入、header 多响应块、multi/easy 清理顺序和 connection option 合同。
- 所有 libcurl handle、callback 和 `curl_easy_pause()` 只在 Orchestrator owner thread 使用；
  HTTP 只按值接收 `RangeLease`，不持有 `RangeLifecycle` 或调度规则。
- 最终 status、精确 `Content-Range`、`Content-Length`、identity
  `Content-Encoding` 和 body 长度在 admission/success 前验证；short/long body 都产生确定性失败。
- 自动解码修正作为独立 C 类提交：显式发送 `Accept-Encoding: identity`，
  `CURLOPT_ACCEPT_ENCODING = nullptr`，并关闭 `CURLOPT_HTTP_CONTENT_DECODING`；
  non-identity 响应拒绝。
- 保留 HTTP/1.1、`FRESH_CONNECT`、`FORBID_REUSE`、HEAD → `Range: 0-0`
  fallback、distinct physical connections、无自动 retry，以及 public/CLI/recovery 兼容性。
- 方案按可独立验证和回滚的 S/C 微提交切片交付，覆盖 deterministic fake、真实 Range
  server 合同测试、静态删除检查、Debug/Release 测试和固定 Release benchmark gate。

主要证据：

- `src/download/http_probe.cpp`
- `src/download/download_engine.cpp`
- `tests/support/range_server.py`
- `tests/download/download_resume_integration_test.cpp`
- 本地 vcpkg libcurl 8.18.0 的 `docs/libcurl/opts`、`docs/libcurl` 与
  `lib/cw-out.c`

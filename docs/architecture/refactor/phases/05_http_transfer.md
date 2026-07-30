# 阶段 5：HTTP Transfer

## 1. Outcome

本阶段把 `HttpProbe`、`DownloadEngine` 中的 libcurl multi/easy 生命周期、write/header
callback、pause/replay、HTTP 响应校验和错误折叠收进一个深模块：
`asyncdownload::http::HttpTransfer`。

阶段完成后：

- `DownloadEngine` 不再 include `<curl/curl.h>`，也不持有 `CURL*`、`CURLM*`、
  `CURLcode` 或 `CURLMcode`；
- probe、HEAD → `Range: 0-0` fallback 和最终响应头解析只有一个实现来源；
- 每个 Transfer Window 按值携带 Phase 3 `RangeLease`，HTTP module 不持有
  `RangeLifecycle`；
- write callback 只把本批字节交给 Phase 2 唯一的 `PacketProducer`，不写文件、不修改
  Range 生命周期；
- 正式 network EMA、downloaded bytes 与 packet summary 仍只由 Packet Flow 在 Data
  Packet successful publish 后通过 `record_download_delta()` 驱动，HTTP 不重复提交；
- `CURL_WRITEFUNC_PAUSE` 的“本批零消费、恢复后重放”语义由 module 内部状态机保证；
- Queue/Memory Pause 仍由 Packet Flow 拥有，Gap Pause 的实际 HTTP episode 由
  `HttpTransferSession` 拥有；
- `curl_easy_pause()`、multi/easy handle 和所有 callback 只在 Orchestrator owner thread
  使用；
- status、最终 `Content-Range`、`Content-Length`、`Content-Encoding` 与实际 body
  字节数在产生 `LeaseSucceeded` 前完成一致性校验；
- Range 响应在 header 合同无效时不会先把 body 交给 Packet Flow；
- 每个 window 继续使用 HTTP/1.1、新物理连接、完成后禁止复用；
- 不增加 HTTP retry、自动降级重试、连接复用、HTTP/2、HTTP/3、认证或代理；
- 公开 C++ interface、CLI、恢复文件、正式 10 项 Performance Summary 保持兼容。

本阶段是结构迁移与若干明确的 HTTP correctness 修复，不是吞吐优化。结构提交与 correctness
提交必须分开，分别保存测试、benchmark 和 rollback evidence。

## 2. 前置条件与阶段边界

Code Agent MUST 先完成：

1. [阶段 0：特征化与兼容基线](00_characterization_baseline.md)；
2. [阶段 1：Validated Download Policy](01_validated_download_policy.md)；
3. [阶段 2：Packet Flow / Backpressure](02_packet_flow_backpressure.md)；
4. [阶段 3：Range Lifecycle](03_range_lifecycle.md)；
5. [阶段 4：Recovery Checkpoint](04_recovery_checkpoint.md)。

执行顺序固定为 Phase 4 → Phase 5。本阶段只消费 Phase 4 已提供的 validated recovery
result、finalize/close seam 与错误结果，不修改 Recovery Checkpoint 内部，也不得把 HTTP
规则移入 Recovery Checkpoint。

本阶段 MUST 保持：

- `DownloadOptions`、`DownloadRequest`、`DownloadResult` 和 progress callback 的公开
  shape；
- CLI 参数、配置、退出码和 stderr 通道；
- HEAD 优先，信息不足或失败时再做 one-byte Range GET；
- Range 模式下的并行连续 Transfer Window；
- 非 Range 模式下单连接完整对象 GET；
- HTTP/1.1；
- `CURLOPT_FRESH_CONNECT = 1L` 与 `CURLOPT_FORBID_REUSE = 1L` 表达的独立物理连接意图；
- redirects 的现有 follow 行为；
- callback 非阻塞写盘；
- 64 KiB Packet Flow aggregation；
- HTTP/Range failure 停止其它 active transfers 时，Packet Flow 仍 open 的 lane draft 继续按
  Phase 2 final-flush 合同发布；只有 upstream Packet Flow/Persistence 已失败时才 discard；
- Queue/Memory 的 high/low、Top 20% 与 episode 计数语义；
- 单 Persistence Writer；
- 无异常 public contract；
- 当前“不自动 retry”的任务失败语义。

本阶段 MUST NOT：

- 让 HTTP module 决定 Range 切分、steal、Lease 签发或任务是否 retry；
- 让 HTTP module 修改 bitmap、VDL、CRC、metadata 或文件；
- 把 libcurl 类型放进 `include/asyncdownload/`、Range Lifecycle 或 Packet Flow interface；
- 引入线程池式 blocking easy transfer；
- 把一个 easy handle 同时交给多个线程；
- 引入 HTTP/2 multiplex、HTTP/3、connection reuse、DNS cache policy 调优；
- 增加 auth、cookie、proxy、certificate policy 或 public cancellation；
- 增加 `If-Range`、条件 GET、镜像或 validator retry；
- 改变 scheduler window、connection count、queue budget、flush cadence；
- 用 profiler 结果豁免正式 benchmark 回归；
- 把 libcurl pause buffer 纳入 Packet Flow 的 Accounted Bytes；
- 依赖 libcurl 内部 pause buffer 的具体容量作为业务预算。

## 3. 已核实的当前实现

### 3.1 当前项目路径

以下是基线源码的具体事实，不是从函数名推断：

| 位置 | 当前行为 | 影响 |
| --- | --- | --- |
| `src/download/http_probe.cpp` | HEAD 成功且得到正 Content-Length 时直接返回 | `Accept-Ranges` 缺失被保守视为不支持，不再 fallback |
| 同文件 `fallback_probe()` | 发送 `Range: 0-0`，discard 整个 body | server 忽略 Range 时可能下载完整对象用于 probe |
| 同文件 `capture_header()` | 所有响应块共用一份 header state | redirect/intermediate response 可能污染最终 facts |
| 同函数 | 用 substring 判断 `Accept-Ranges` 包含 `bytes` | 非 token 值可能被误判 |
| 同函数 | `std::stoll()` 解析 `Content-Range` 总长度并吞异常 | 没有完整 grammar、边界或 start/end 校验 |
| probe 全路径 | 所有 `curl_easy_setopt/getinfo` 返回值被忽略 | setup/getinfo 失败会被后续 symptom 覆盖 |
| `DownloadEngine::TransferHandle` | 同时保存 CURL、RangeContext、queue、buffer、pause、status | libcurl、领域状态与 packet ownership 混在一个数据袋 |
| `arm_transfer()` | 每次 `curl_easy_reset()` 后重新配置 easy | reset 后仍保留 connection/DNS cache，但后续强制 fresh connection |
| 同函数 | `CURLOPT_HTTP_VERSION = CURL_HTTP_VERSION_1_1` | 当前明确请求 HTTP/1.1 |
| 同函数 | `CURLOPT_FRESH_CONNECT = 1L` | 下一次 transfer 强制新连接 |
| 同函数 | `CURLOPT_FORBID_REUSE = 1L` | transfer 完成后连接关闭，不供后续 reuse |
| 同函数 | `CURLOPT_ACCEPT_ENCODING = ""` | 请求全部内建编码并启用自动解码 |
| write callback | 先做 queue/memory admission，成功后推进 `next_offset` | pause 时 offset 通常不推进 |
| write callback | window 已满而仍收到 body 时返回 pause | 可能留下永远等不到 `CURLMSG_DONE` 的 paused transfer |
| transfer path | 没有 header callback | 不校验实际 `Content-Range` 或 `Content-Encoding` |
| `response_is_valid()` | partial Range 只检查 status 206 | 206 的区间可能错位仍被接受 |
| 同函数 | whole-object Range 和 non-Range 接受 200 或 206 | 未证明 206 覆盖完整对象 |
| `finalize_completed_request()` | 先 flush local draft，再看 CURL/status/body length | 无效响应的字节可能已经进入 persistence |
| 同函数 | short body 映射 `http_transfer_failed` | HTTP 合同错误与传输中断未区分 |
| `resume_paused_transfers()` | 清本地 bit 后调用 `curl_easy_pause(CONT)` | 当前未检查返回值，也未显式处理同步重入 |
| `apply_gap_pauses()` | Gap `0→1` 记录 telemetry 并 pause | gap episode owner 目前在 engine helper |
| `curl_multi_perform/wait` | 100 ms wait loop | event loop 与 progress/persistence polling 共用 |
| `curl_multi_info_read()` | DONE 后 get private/status、remove、finalize | getinfo/remove 返回值未检查 |
| cleanup | easy cleanup 后 multi cleanup | 正常顺序接近官方要求，但错误未结构化 |

### 3.2 当前测试面

`tests/support/range_server.py` 当前：

- HEAD 与 GET 都返回正 `Content-Length`；
- 有 Range 时返回 206 和匹配 `Content-Range`；
- 总是声明 `Accept-Ranges: bytes`；
- 可固定 chunk size 与 delay；
- 可记录 method、client port 和 Range header；
- 不能生成 malformed range、ignored range、short/long body、redirect 或 encoding。

现有 integration 已证明：

- interrupted resume、CRC rollback、transient block reset；
- 至少 4 个 concurrent GET 使用 distinct client ports；
- progress 与正式 packet metrics 可观察；
- server failure 保留 `.part` 与 metadata。

现有测试没有证明：

- HEAD fallback 的最终响应块隔离；
- malformed/mismatched `Content-Range` 被拒绝；
- paused callback batch 恰好重放一次；
- 非 identity `Content-Encoding` 不进入 Range offset；
- long body 不会永久 pause；
- 任一 setopt/getinfo/pause/multi 错误被检查；
- failed Lease 没有自动发出第二次 HTTP 请求。

## 4. 已核实的 libcurl 8.18.0 合同

### 4.1 核实版本与来源

当前 build 实际安装：

```text
build/vcpkg_installed/vcpkg/status: curl 8.18.0, port-version 1
build/vcpkg_installed/x64-windows/include/curl/curlver.h: 8.18.0
```

本阶段结论优先依据对应 vcpkg source：

```text
D:/git_repository/ThirdParty/vcpkg/buildtrees/curl/src/
    url-8_18_0-c0c48377da.clean/
```

另外核对了 `D:/git_repository/curl` 的 8.19.0-DEV source/docs；本文使用的 pause、
callback、encoding 和 multi/easy 合同在两者一致。实现和 tests 必须以实际链接的 8.18.0
为准，不能只依据 8.19.0-DEV 新行为。

### 4.2 write callback 与 pause

已核实：

- `CURLOPT_WRITEFUNCTION` 文档规定 body callback 的 `size` 当前总是 1，但应用仍应 checked
  计算 `size * nmemb`；
- callback 可以收到 0 bytes；
- callback 返回值必须等于完整 batch size，否则 transfer 以 `CURLE_WRITE_ERROR` 终止；
- 返回 `CURL_WRITEFUNC_PAUSE` 表示本批一个字节都没有消费；
- `curl_easy_pause(..., CURLPAUSE_CONT)` 后，本批会再次交给 callback；
- `curl_easy_pause()` 不能从另一个 thread 调用；
- unpause receiving 时，write callback 可能在 `curl_easy_pause()` 返回前同步执行；
- paused 数据由 libcurl 自己保存，不属于 Packet Flow ledger。

8.18.0 `lib/cw-out.c` 的具体路径证明：

1. `cw_out_cb_write()` 在调用 client callback 前把 `*pnwritten = 0`；
2. callback 返回 `CURL_WRITEFUNC_PAUSE` 时设置 `ctx->paused = TRUE` 并返回内部
   `CURLE_AGAIN`；
3. `cw_out_do_write()` 看到 `consumed < blen` 后，把未消费部分追加到 pause buffer；
4. `Curl_cw_out_unpause()` 先清 paused，再同步 flush pause writer 和 client writer；
5. client callback 出错后 `ctx->errored`，buffer 被释放，callback 不会再次被调用；
6. 8.18.0 的 `DYN_PAUSE_BUFFER` 为 64 MiB。

64 MiB 是该版本实现细节，不是稳定业务 interface。Code Agent MUST NOT：

- 把它写入 DownloadOptions；
- 把它当作每 transfer 可安全额外占用的内存；
- 用它解释 Packet Flow high watermark；
- 依赖“buffer 足够大，所以可长期 pause”。

### 4.3 automatic decoding

8.18.0 `CURLOPT_ACCEPT_ENCODING` 文档明确：

- 传空字符串 `""` 会发送当前 build 支持的全部 encoding；
- 任意 non-null `CURLOPT_ACCEPT_ENCODING` 会启用 response decoding；
- server 的 compressed `Content-Length` 可以与 write callback 解码后字节总和不同；
- 小 wire body 可以解码成极大 callback body；
- server 可以返回没有请求或不同于请求的 `Content-Encoding`；
- 传 null 会关闭该 option，不发送由它生成的 header，也不自动解码。

当前 vcpkg curl 依赖 zlib，因此 `CURLOPT_ACCEPT_ENCODING = ""` 不是理论风险；它至少可以请求
并解码 gzip/deflate。Range offset、`Content-Range` 与 recovery total size 必须描述同一
identity representation，自动解码会破坏这条前提。

`CURLOPT_HTTPHEADER` 文档进一步规定：

- custom list 不被 libcurl 整体复制；
- list 必须保持有效直到 handle 不再使用；
- 同名 header 会替换 libcurl 自己生成的 header；
- custom header 会跟随 redirect 发送。

最终 target 固定为：

```text
CURLOPT_ACCEPT_ENCODING = nullptr
CURLOPT_HTTP_CONTENT_DECODING = 0L
CURLOPT_HTTPHEADER includes exactly "Accept-Encoding: identity"
```

即：

- 不使用 `CURLOPT_ACCEPT_ENCODING` 做协商；
- 通过显式 immutable header 请求 identity representation；
- 即使未来 option 组合变化，也明确关闭 content decoder；
- final response 出现非空且非 `identity` 的 `Content-Encoding` 时按
  `http_invalid_response` 拒绝；
- probe 和 transfer 使用完全相同的 representation policy。

这是一项独立 C 类 correctness commit，不得藏在 setopt 搬迁中。

### 4.4 header callback

`CURLOPT_HEADERFUNCTION` 文档明确：

- 每次调用只给一条完整 header line，buffer 不以 null 结尾；
- status line 与 header block 的空行都会进入 callback；
- callback 会收到 redirect、authentication 和 informational 等所有 response 的 headers；
- final response 不能通过“最后一次同名 header 覆盖”猜测，必须用 status line 划分 block；
- trailer 也会进入 header callback；
- 返回长度不等于输入长度会导致 `CURLE_WRITE_ERROR`。

因此 target parser 必须：

- 识别每个 `HTTP/... <status>` status line并开始新 block；
- 每个新 block 清空 status-specific headers；
- 只在该 block 的空行后把它标记为 headers complete；
- body 开始后把后续 header lines当 trailer，不覆盖 response contract；
- final completion 时以最后一个 non-informational response block为准；
- intermediate 1xx/3xx body若被 libcurl交付，只消费并丢弃，不进入 Packet Flow。

### 4.5 multi/easy 生命周期

官方 local docs 已核实：

- `curl_multi_add_handle()` 不启动 transfer；`curl_multi_perform()` 才驱动；
- easy 加入 multi 后不得调用 `curl_easy_perform()`；
- DONE 后 easy 仍在 multi，必须显式 remove；
- easy 要复用时先 remove，设置 options，再 add；
- `curl_multi_perform()` 的 `running_handles == 0` 不代表成功，必须 drain
  `curl_multi_info_read()`；
- 单 transfer failure 可以发生而 multi call 仍返回 `CURLM_OK`；
- `curl_multi_perform()` 自身返回 error 后，全部 transfer state 不确定；同一 multi
  不能直接继续 perform；
- `CURLMsg*` 在 remove/cleanup 后失效，必须先复制 `msg->data.result` 和 easy pointer；
- active transfer 可在 callback 外随时 remove，remove 会停止它；
- 不得从 libcurl callback 内 remove easy；
- cleanup 顺序为 remove easy → easy cleanup → multi cleanup；
- `curl_easy_reset()` 清 options，但保留 live connections、DNS cache 等 handle state；
- 同一个 easy/multi handle 在任一时刻不能被多个 thread 使用；
- `curl_multi_wait()` 在没有 curl fd 且没有 extra fd 时可能立即返回。

当前 100 ms `curl_multi_wait()` 形状先保持。切换 `curl_multi_poll()` 或 socket interface
必须作为后续独立性能实验。

### 4.6 connection options

local docs 的精确含义：

- `CURLOPT_HTTP_VERSION = CURL_HTTP_VERSION_1_1` 要求 HTTP/1.1；
- `CURLOPT_FRESH_CONNECT = 1L` 强制下一次 transfer 使用新连接；
- `CURLOPT_FORBID_REUSE = 1L` 要求 transfer 完成后关闭连接；
- `CURLMOPT_MAX_TOTAL_CONNECTIONS` 与 `MAX_HOST_CONNECTIONS` 是同时打开连接上限；
- 超出 multi connection 上限的 easy 会在 libcurl 内部 pending；
- `CURLOPT_RANGE` 只是请求，HTTP server 被标准允许忽略。

因此“设置 Range 就一定得到对应 bytes”和“max connections 就一定有 N 条 active socket”
都不是合同。前者必须用 response validator证明，后者必须用真实 server 的 client-port
overlap test证明。

### 4.7 HEAD 与 fallback GET 方法状态

本地 8.18.0 `CURLOPT_NOBODY`、`CURLOPT_HTTPGET` 文档和 `lib/setopt.c` 已核实：

- `CURLOPT_NOBODY = 1L` 对 HTTP 把 method 设为 HEAD；
- 仅把 `CURLOPT_NOBODY` 设回 0 不是官方推荐的通用 method reset；
- `CURLOPT_HTTPGET = 1L` 明确把 method 设回 GET，并同时清除 no-body/upload 状态；
- `CURLOPT_RANGE = nullptr` 明确禁用 handle 上一次留下的 Range；
- `curl_easy_reset()` 会恢复默认 method，但 target adapter 仍必须显式设置当前 probe
  operation 的方法，不能依赖前一次状态或默认值。

因此 HEAD 与 fallback 即使复用同一个 easy，也必须把 method 当作每次 operation 的完整
配置。fallback 不得只增加 `Range: 0-0` 而沿用 HEAD method。

## 5. 具体安全缺口与变更分类

| Gap | Current concrete condition | Target | Class |
| --- | --- | --- | --- |
| Range 错位 | 只检查 status 206，不解析 header | body admission 前验证 exact `Content-Range` | C |
| invalid body 先入队 | completion 时才校验 status | headers complete 后才允许 Packet Flow admission | C |
| decoded offset mismatch | transfer 设置 `ACCEPT_ENCODING=""` | identity header + decoder disabled + response reject | C |
| redirect header 污染 | probe header state不按 status block reset | final-block accumulator | C |
| HEAD partial/encoded total 污染 | 非200 HEAD或压缩表示的 Content-Length 可能被绑定为对象总长 | 只有 final 200 identity HEAD 成功；其他状态 fallback，非identity直接失败 | C |
| probe method state 泄漏 | HEAD/GET options 会改变 easy method state | 每个 probe operation 显式设置 NOBODY 或 HTTPGET | S/C guard |
| long body永久 pause | remaining 0 后返回 pause | 额外 body立即 `body_too_long` terminal error | C |
| ignored setopt error | 所有 return value丢弃 | first setup error短路并映射 | C |
| pause reentry隐式 | unpause 后继续修改 state | 调用前完成 state transition，返回后不覆盖 | S/C guard |
| curl/领域混合 | TransferHandle同时拥有五类状态 | deep command/event interface | S |
| engine重复错误折叠 | 多 helper各自映射 | module内分类，engine只保留 first primary error | S |

所有 C 类项必须：

1. 先增加在旧实现上 red 或风险特征化测试；
2. 使用独立 `fix:` commit；
3. 记录 success/failure behavior diff；
4. 可独立回滚；
5. 不与 interface move 或删除提交 squash。

## 6. Module responsibilities

### 6.1 HTTP Transfer owns

`HttpTransfer` MUST 独占：

- libcurl process initialization result；
- probe easy handle；
- HEAD 与 one-byte Range fallback；
- final response header block parser；
- HTTP status、Content-Range、Content-Length、Content-Encoding 校验；
- multi handle 与固定 easy slot pool；
- every `curl_easy_setopt/getinfo/pause` return check；
- every `curl_multi_setopt/add/remove/perform/wait/cleanup` return check；
- stable callback userdata；
- Transfer Token 与 slot generation；
- Range header inclusive endpoint conversion；
- callback accepted frontier 与 actual body bytes；
- callback error cause；
- Queue/Memory/Gap composite receive-pause application；
- synchronous unpause reentry guard；
- `CURLMSG_DONE` draining；
- curl error到 project error的映射；
- active transfer cancellation和网络资源 shutdown；
- HTTP first-byte telemetry的唯一发送点；
- 仅供 `PacketProducer::reconcile()` 使用的 per-slot raw speed observation；
- HTTP session snapshot；
- production Curl adapter与 deterministic fake共享的高层 interface。

删除该 module 后，上述复杂性会重新散回 engine、probe、callback、Range adapter和 tests，符合
deletion test。

### 6.2 HTTP Transfer does not own

HTTP Transfer MUST NOT：

- 解释 Raw Download Options；
- 构造 Effective Download Policy；
- 计划 initial ranges、next window或 steal；
- 签发、重试、完成或回滚 Range Lease；
- 修改 Range Lifecycle；
- 决定任务是否开始第二次 HTTP attempt；
- 解释 queue capacity或 memory budget；
- 持有 moodycamel queue；
- 写 part file或 metadata；
- 发布 `PersistenceCommitted`；
- 生成 recovery identity以外的状态；
- 提交 network rate、downloaded bytes、packet count或packet size telemetry；
- 调用 `TelemetrySession::record_download_delta()`；
- 聚合最终 Performance Summary；
- 把 progress snapshot变成业务真相源；
- 为测试暴露 CURL handles；
- 支持 auth、proxy、cookie或 arbitrary headers。

## 7. 备选 interface designs

### 7.1 Design A：薄 `CurlEasy` wrapper

形状：

```cpp
class CurlEasy {
public:
    std::error_code set_url(std::string_view url) noexcept;
    std::error_code set_range(std::string_view value) noexcept;
    std::error_code add_to(CURLM* multi) noexcept;
};
```

拒绝原因：

- callers仍要知道 option组合、callback lifetime、response validator与 pause replay；
- `CURLM*` 仍泄漏；
- probe和transfer仍会分别配置相同 options；
- fake只能模拟函数返回值，不能表达高层 Lease/event合同；
- 删除 wrapper后复杂性几乎不增加，是 shallow module。

### 7.2 Design B：高层 command/event port + session，选定

形状：

```text
HttpTransferPort
    ├── probe(request) -> HttpObjectFacts
    └── open_session(config, PacketProducer, TelemetrySession)
            -> HttpTransferSession
                  start(RangeLease)
                  set_gap_paused(token, bool)
                  poll(timeout) -> Lease event
                  cancel(cause)
                  close()
```

production `CurlHttpTransferPort` 与 test `DeterministicHttpTransferPort` 满足同一个 interface。

选择原因：

- Orchestrator只学习 probe、start、fact feed、poll、cancel、close；
- libcurl和response parser全部在 production adapter后；
- fake表达同一高层命令和事件，不伪造 CURL surface；
- Packet Flow与Range Lifecycle各自保持既有 ownership；
- callback replay与body validation可集中；
- production真实 server tests验证Curl adapter，engine tests用fake验证编排；
- 删除 module会重新暴露大量libcurl知识，具有高 depth、leverage和locality。

### 7.3 Design C：每个 window 一个 blocking worker

形状：

```text
RangeLease -> worker thread -> curl_easy_perform -> result future
```

拒绝原因：

- 违反项目必须使用 libcurl multi 的硬约束；
- easy handles跨线程与PacketProducer owner-thread发生冲突；
- pause/unpause需要跨线程协调；
- N workers改变 `1 + 1 + N` 中 Workers只做后勤的语义；
- cancellation、connection limit和callback ordering更复杂；
- 性能和线程数变化无法作为结构中性迁移。

## 8. Dependency category与方向

libcurl 是 **true external** dependency。目标 seam存在两个真实 adapter：

- production：`CurlHttpTransferPort`；
- test：`DeterministicHttpTransferPort`。

允许的 dependency direction：

```text
download::ValidatedDownloadPolicy
        │
        ▼
http::HttpTransferPort::probe
        │
        ├──► download::RemoteObjectFacts
        └──► recovery::RemoteRecoveryIdentity

download::EffectiveDownloadPolicy
range::RangeLease
flow::PacketProducer
telemetry::TelemetrySession
        │
        ▼
http::HttpTransferSession
        │
        ├── production adapter ──► libcurl
        └── test adapter ────────► deterministic script

HttpTransferEvent
        │
        ▼
DownloadEngine maps by value
        └──► RangeLifecycle::apply(LeaseSucceeded / LeaseFailed)
```

禁止：

- `RangeLifecycle` include HTTP header；
- `PacketFlow` include CURL header；
- `RecoveryCheckpoint`调用probe；
- production engine根据 `CURLcode` 分支；
- public `include/asyncdownload/` include本module；
- fake链接libcurl；
- Curl adapter调用RangeLifecycle。

Telemetry是 in-process dependency。Phase 5继续使用 concrete `TelemetrySession`，但HTTP
只能提交first-byte和Gap Pause episode；不得提交network rate、downloaded bytes或packet
summary。正式network telemetry由Packet Flow successful publish的唯一
`record_download_delta()`调用驱动。Phase 6可在不改变HTTP高层事件语义的前提下深化该
facade。

## 9. 精确 C++ interface

目标 internal header为 `src/http/http_transfer.hpp`。名称若与已合并阶段有机械冲突可以调整，
但方法数量、结果分类、所有权和事件语义 MUST 保持。

```cpp
#pragma once

#include "range/range_lifecycle.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <variant>

namespace asyncdownload::flow {
class PacketProducer;
}

namespace asyncdownload::telemetry {
class TelemetrySession;
}

namespace asyncdownload::http {

using TransferSlotId = std::uint32_t;

enum class HttpFailureReason : std::uint8_t {
    none = 0,
    global_init_failed,
    easy_init_failed,
    multi_init_failed,
    easy_option_failed,
    multi_option_failed,
    easy_info_failed,
    add_handle_failed,
    remove_handle_failed,
    multi_perform_failed,
    multi_wait_failed,
    pause_failed,
    transport_failed,
    callback_sink_failed,
    response_status_invalid,
    content_range_missing,
    content_range_malformed,
    content_range_mismatch,
    content_length_malformed,
    content_length_mismatch,
    content_encoding_invalid,
    body_too_short,
    body_too_long,
    header_callback_failed,
    cancelled,
    upstream_failed,
    protocol_order_invalid,
    allocation_failed
};

struct HttpFailure {
    HttpFailureReason reason = HttpFailureReason::none;
    std::error_code error{};
};

struct HttpProbeRequest {
    std::string url;
};

struct HttpObjectFacts {
    std::int64_t total_size = 0;
    bool accept_ranges = false;
    std::string etag;
    std::string last_modified;
};

struct HttpProbeResult {
    std::optional<HttpObjectFacts> facts;
    HttpFailure failure{};
    long response_code = 0;

    [[nodiscard]] bool ok() const noexcept;
};

enum class HttpSessionState : std::uint8_t {
    open = 0,
    cancelling,
    failed,
    closed
};

struct HttpSessionConfig {
    std::string url;
    std::int64_t total_size = 0;
    std::size_t max_active_transfers = 0;
};

struct TransferToken {
    TransferSlotId slot = 0;
    std::uint64_t slot_generation = 0;
    range::LeaseId lease{};

    friend bool operator==(const TransferToken&, const TransferToken&) = default;
};

enum class HttpStartCode : std::uint8_t {
    started = 0,
    no_capacity,
    closed,
    failed
};

struct HttpStartResult {
    HttpStartCode code = HttpStartCode::failed;
    std::optional<TransferToken> token;
    HttpFailure failure{};
};

struct HttpLeaseSucceeded {
    TransferToken token{};
    range::LeaseId lease{};
    range::ByteOffset received_through = 0;
    long response_code = 0;
};

struct HttpLeaseFailed {
    TransferToken token{};
    range::LeaseId lease{};
    range::ByteOffset accepted_through = 0;
    HttpFailure failure{};
};

using HttpTransferEvent = std::variant<
    HttpLeaseSucceeded,
    HttpLeaseFailed>;

enum class HttpPollCode : std::uint8_t {
    event = 0,
    idle,
    timed_out,
    failed,
    closed
};

struct HttpPollResult {
    HttpPollCode code = HttpPollCode::failed;
    std::optional<HttpTransferEvent> event;
    std::error_code error{};
};

enum class HttpCancelKind : std::uint8_t {
    task_cancelled = 0,
    upstream_failed
};

struct HttpCancelRequest {
    HttpCancelKind kind = HttpCancelKind::task_cancelled;
    std::error_code cause{};
};

struct HttpSessionSnapshot {
    HttpSessionState state = HttpSessionState::open;
    std::size_t active_transfers = 0;
    std::size_t available_slots = 0;
    std::size_t pending_events = 0;
    std::size_t paused_transfers = 0;
    std::error_code error{};
};

class HttpTransferSession {
public:
    virtual ~HttpTransferSession() = default;

    [[nodiscard]] virtual HttpStartResult start(
        const range::RangeLease& lease) noexcept = 0;

    [[nodiscard]] virtual std::error_code set_gap_paused(
        const TransferToken& token,
        bool active) noexcept = 0;

    [[nodiscard]] virtual HttpPollResult poll(
        std::chrono::milliseconds timeout) noexcept = 0;

    [[nodiscard]] virtual std::error_code cancel(
        const HttpCancelRequest& request) noexcept = 0;

    [[nodiscard]] virtual HttpSessionSnapshot snapshot() const noexcept = 0;

    [[nodiscard]] virtual std::error_code close() noexcept = 0;
};

struct HttpSessionOpenResult {
    std::unique_ptr<HttpTransferSession> session;
    HttpFailure failure{};
};

class HttpTransferPort {
public:
    virtual ~HttpTransferPort() = default;

    [[nodiscard]] virtual HttpProbeResult probe(
        const HttpProbeRequest& request) noexcept = 0;

    [[nodiscard]] virtual HttpSessionOpenResult open_session(
        const HttpSessionConfig& config,
        flow::PacketProducer& packet_producer,
        telemetry::TelemetrySession& telemetry) noexcept = 0;
};

[[nodiscard]] std::error_code create_curl_http_transfer_port(
    std::unique_ptr<HttpTransferPort>& result) noexcept;

}
```

### 9.1 Interface rules

- `HttpTransferPort` 和 `HttpTransferSession` 是 internal interface，不进入 public headers。
- factory 捕获 allocation和libcurl init failure，不抛异常。
- `probe()` 在任何 file、Packet Flow、Range Lifecycle或worker创建前执行。
- engine把 `HttpObjectFacts` 映射为 Phase 1 `RemoteObjectFacts` 与 Phase 4
  `RemoteRecoveryIdentity`；HTTP module不依赖这两个consumer类型。
- `open_session()` 只接受已经bind的 total size和connection limit。
- session创建时为每个fixed slot从同一个`PacketProducer`打开一个`ProducerLane`。
- `start()` 只接受Phase 3签发的immutable `RangeLease`，且调用前 Orchestrator 必须已经
  收到该 Lease 全部 Register/Resize geometry commands 的 matching Persistence success
  ACK；HTTP module 不替代这条 Phase 3 barrier。
- `start()` 不修改Lifecycle；ACK 后的 HTTP setup/start failure统一映射为
  `LeaseFailed{accepted_through = lease.bytes.begin}`。`HttpStartResult::failed` 必须携带
  精确 `HttpFailure`，不创建 token、不排 pending event；engine立即 apply failure，清理后的
  slot仍可正常close。只有 geometry submit/ACK 或 completion effect publish 失败才映射
  `EffectApplicationFailed`，不得混淆根因。
- `no_capacity` 不是error；engine不得在没有slot时先继续acquire无限Lease。尚未收到
  geometry ACK 的 pending arm 也预占一个 slot capacity；可 acquire 数量等于
  `max_active_transfers - active_transfers - pending_arms`。
- `TransferToken.slot_generation` 每次slot reuse严格递增，0不签发。
- stale token的`set_gap_paused()`操作返回internal order error且没有side effect；
  session-level `cancel()`不接收token。
- `poll()` 每次最多返回一个event；其余completed slots保留pending event供下一次0ms poll。
- 只要任一 `pending_event` 尚未交付，`start()` 返回 `no_capacity`，
  `snapshot.available_slots == 0`，并由 `snapshot.pending_events` 给出数量；防止已知结果
  之前启动新 Lease。
- multi message drain 一旦构造任一 failed event，立即保存 session first error并禁止新
  start，即使按 slot-id 顺序先向 caller 交付的是 success event。
- `poll()` 不为每个callback batch分配event。
- `HttpLeaseSucceeded.received_through` 必须等于Lease span exclusive end。
- `HttpLeaseFailed.accepted_through` 只表示Packet Flow已经接受的exclusive frontier，不表示
  persisted。
- session snapshot只用于orchestration/telemetry observation，不是Range、downloaded或
  persisted truth。
- `close()` 幂等；active/pending event未处理时返回invalid-order error，不静默discard。

## 10. Selected implementation state

production adapter内部固定slot，地址在session生命周期内稳定：

```cpp
struct CurlTransferSlot {
    CURL* easy = nullptr;
    flow::ProducerLane lane;
    TransferSlotId id = 0;
    std::uint64_t generation = 0;
    std::optional<range::RangeLease> lease;
    std::optional<HttpTransferEvent> pending_event;
    HttpResponseAccumulator response;
    range::ByteOffset accepted_through = 0;
    std::uint64_t body_bytes = 0;
    double bytes_per_second = 0.0;
    std::uint8_t packet_pause_mask = 0;
    bool gap_paused = false;
    bool curl_receive_paused = false;
    bool in_multi = false;
    bool callback_active = false;
    bool cancelling = false;
    HttpFailure callback_failure;
};
```

规则：

- implementation可用`std::vector<std::unique_ptr<CurlTransferSlot>>`或固定array；
- 禁止把可移动`std::vector<CurlTransferSlot>`元素地址直接交给`CURLOPT_PRIVATE/WRITEDATA`
  后继续resize；
- `CURLOPT_PRIVATE`与callback userdata都指向同一个stable slot；
- slot按值保存完整`RangeLease`，不保存`RangeContext*`；
- `accepted_through`初始为`lease.bytes.begin`；
- `body_bytes`只在PacketAdmission consumed后增加；
- `response`每次start清空；
- `pending_event`存在时slot不可复用；
- 每个slot只有一个Phase 2 lane，lane跟slot生命周期，不跟Lease生命周期；
- easy handle可复用，但每次必须remove后reset/reconfigure/add。

session implementation另外拥有：

- `CURLM*`；
- immutable `curl_slist*`，只含`Accept-Encoding: identity`；
- `PacketProducer&`；
- `TelemetrySession&`；
- owner thread id；
- first session error；
- fixed slots；
- state。

header list必须在所有easy remove/reset之后才`curl_slist_free_all()`。

### 10.1 每次 Lease 的 exact easy configuration

slot reuse 时先确认 easy 已从 multi remove，再 `curl_easy_reset()`，随后按顺序设置并检查：

```text
URL = session URL
FOLLOWLOCATION = 1L
NOSIGNAL = 1L
HTTP_VERSION = HTTP/1.1
WRITEFUNCTION / WRITEDATA = module callback / stable slot
HEADERFUNCTION / HEADERDATA = final-block accumulator / stable slot
PRIVATE = stable slot
TCP_KEEPALIVE = 1L
ACCEPT_ENCODING = nullptr
HTTP_CONTENT_DECODING = 0L
HTTPHEADER = immutable ["Accept-Encoding: identity"]
FRESH_CONNECT = 1L
FORBID_REUSE = 1L
PIPEWAIT = 0L
RANGE = "begin-(end-1)" only when lease.use_http_range
```

`RANGE` string必须由 non-empty half-open span checked转换，并由slot持有到easy remove；
non-Range Lease不得留下前一代`CURLOPT_RANGE`。任一 option failure在 add-to-multi 前停止，
清理半配置 easy并通过 `HttpStartResult{failed, no token, HttpFailure}` 同步返回
`easy_option_failed`；不得排 pending event、要求 caller 再 poll，或带着半配置 easy继续。
`curl_multi_add_handle()` failure采用同一同步 start-failure合同。

## 11. Probe contract

### 11.1 通用配置

HEAD与fallback GET都使用：

```text
URL = request.url
FOLLOWLOCATION = 1L
NOSIGNAL = 1L
HTTP_VERSION = HTTP/1.1
HEADERFUNCTION = final-block accumulator
WRITEFUNCTION = discard probe body
ACCEPT_ENCODING = nullptr
HTTP_CONTENT_DECODING = 0L
HTTPHEADER = ["Accept-Encoding: identity"]
```

method-specific 配置固定为：

```text
HEAD:
    NOBODY = 1L
    RANGE = nullptr

fallback:
    HTTPGET = 1L
    RANGE = "0-0"
```

每次 operation 前清空 response accumulator。若复用 easy，必须先
`curl_easy_reset()`，再完整应用通用和 method-specific options；若使用新 easy，也仍显式
设置方法，不依赖默认 GET。禁止用 `CUSTOMREQUEST = "HEAD"`/`"GET"` 绕过
`NOBODY`/`HTTPGET` 的 body-state 合同。

每个setopt MUST 顺序检查。第一个failure立即停止setup、cleanup easy/header list，并返回
`http_probe_failed`；internal reason保留`easy_option_failed`。

### 11.2 HEAD success与fallback条件

HEAD目标规则：

| Final condition | Action |
| --- | --- |
| transport success、final 200、identity representation、正且可表示的 Content-Length | HEAD success |
| CURL transport failure | fallback |
| final status不是200（包括unsolicited 206） | fallback；不得把partial Content-Length绑定为total |
| Content-Length缺失、malformed、冲突或`<= 0` | fallback |
| final `Content-Encoding`存在non-identity token | probe failure，不fallback掩盖representation mismatch |
| header parser allocation/error | probe failure，不fallback掩盖 |

保持当前保守策略：

- HEAD成功但没有`Accept-Ranges: bytes`时，`accept_ranges = false`；
- 不为“可能支持Range”额外fallback；
- unsolicited HEAD 206 即使带`Content-Range: bytes 0-0/N`也不走HEAD direct-success；只有
  随后的显式`Range: 0-0` fallback按§11.3解析total；
- ETag/Last-Modified取最终response block；
- HEAD与fallback使用同一identity representation合同；server忽略identity返回compressed HEAD
  length时不得绑定为object total；
- zero-byte对象仍按Phase 1合同失败，不在本阶段新增空文件下载。

`Accept-Ranges`只接受case-insensitive token `bytes`。`none`、空、substring如`notbytes`
都不表示支持。

### 11.3 `Range: 0-0` fallback

fallback固定发送单区间`0-0`，不自动进行第二次fallback：

| Final response | Required headers/body | Result |
| --- | --- | --- |
| 206 | exact `Content-Range: bytes 0-0/N`，`N > 0`；Content-Length若有必须为1 | size=N, Range=true |
| 200 | 不得有Content-Range；正Content-Length=N | size=N, Range=false |
| 206 missing/malformed/mismatch | — | `http_probe_failed` |
| 200 missing/nonpositive length | — | `http_probe_failed` |
| 416 | — | `http_probe_failed` |
| 1xx/3xx/4xx/5xx final | — | `http_probe_failed` |
| non-identity Content-Encoding | — | `http_probe_failed` |
| transport/callback error | — | `http_probe_failed` |

server忽略Range并返回200时，保持当前会discard完整body的行为。本阶段不通过提前abort、
`MAXFILESIZE`或另一个HEAD改写该路径。

### 11.4 Probe facts mapping

`HttpProbeResult`只返回：

- positive total size；
- conservative Range capability；
- final ETag；
- final Last-Modified；
- final response code。

不得返回：

- CURL handles/codes；
- Content-Length pointer；
- redirect target作为recovery identity；
- decoded size；
- retry hint。

Recovery继续以调用方原始URL作为identity，保持Phase 4合同。

## 12. Response accumulator

内部`HttpResponseAccumulator`至少保存：

```text
status
headers_complete
body_started
content_range: absent / one parsed value / duplicate-invalid
content_length: absent / one parsed value / conflicting-invalid
content_encoding: absent / identity / other / duplicate-invalid
etag
last_modified
parser_failure
```

parser规则：

- checked计算`size * nitems`；
- status line开始新response block；
- header name ASCII case-insensitive；
- header value只trim optional whitespace与CRLF；
- numeric grammar只接受十进制digit，不接受符号、hex、locale separator；
- 所有decimal parse使用checked arithmetic到`int64_t`；
- duplicate `Content-Range` 一律invalid；
- duplicate identical Content-Length可接受，conflicting invalid；
- multiple `Content-Encoding`按逗号token解析；只有全部identity才接受；
- response header empty line关闭block；
- body开始后header callback输入视为trailer，不改变已验证合同；
- callback C trampoline捕获所有异常，保存`allocation_failed/header_callback_failed`并返回
  `CURL_WRITEFUNC_ERROR`；
- parser不作为production header公开，也不让engine读取中间字段。

### 12.1 `Content-Range` exact grammar

只接受单byte range：

```text
bytes <first>-<last>/<complete-length>
```

不接受：

- `bytes */N`；
- `complete-length = *`；
- multiple ranges或multipart；
- first/last/total溢出；
- first > last；
- last >= total；
- trailing非OWS字符；
- duplicate field。

内部统一转成半开：

```text
[first, last + 1)
```

`last + 1`必须checked。HTTP inclusive endpoint只在该parser和request formatter出现。

## 13. Transfer response/body validation table

每个Lease在body admission前验证headers，在CURLMSG_DONE后验证最终body：

| Lease/request | Status | Header contract | Body contract | Result |
| --- | ---: | --- | --- | --- |
| Range partial `[a,b)` | 206 | CR exact `[a,b)`, total exact session total | exactly `b-a` | success |
| Range whole `[0,total)` | 206 | CR exact whole object | exactly total | success |
| Range whole `[0,total)` | 200 | no CR；server安全忽略Range | exactly total | success |
| Range partial | 200 | any | body不得admit | invalid response |
| Any Range | 206 | CR missing/malformed/mismatch | body不得admit | invalid response |
| non-Range whole GET | 200 | no CR | exactly total | success |
| non-Range whole GET | 206 | any | body不得admit | invalid response |
| any | 204/304/416/other final | any | body不得admit | invalid response |
| any accepted status | any | non-identity encoding | body不得admit | invalid response |
| accepted | any | Content-Length present但不等于expected | body不得admit | invalid response |
| accepted, chunked | any | no Content-Length | exact callback total | success |
| accepted | any | body ends early | `< expected` | `body_too_short` |
| accepted | any | callback crosses end | `> expected` | `body_too_long` |

`body_too_short`和`body_too_long`最终都映射public `http_invalid_response`。transport在body未完成
前断开且libcurl返回非OK时，primary internal reason为`transport_failed`，public仍为
`http_transfer_failed`；tests必须同时检查internal分类，避免把协议错误和socket错误重新混同。

### 13.1 Header-before-admission rule

write callback只有在当前response block满足本Lease的status/header合同后，才可以调用
`PacketProducer::accept()`。

若header无效：

- 设置slot callback failure；
- 返回`CURL_WRITEFUNC_ERROR`；
- `accepted_through`不推进；
- 本batch不进入Packet Flow；
- CURLMSG_DONE的`CURLE_WRITE_ERROR`不能覆盖已保存的response reason。

redirect/1xx response body若出现：

- callback返回完整输入长度；
- 不记录first byte；
- 不推进Lease frontier；
- 不进入Packet Flow；
- 后续新status line重置response block。

### 13.2 Body end rule

若`accepted_through == lease.bytes.end`后又收到非空body：

- 不返回pause；
- 不截断并报告success；
- 设置`body_too_long`；
- 返回`CURL_WRITEFUNC_ERROR`。

旧`paused_by_window_boundary`和“等server自己结束”的路径最终删除。这个C类修复避免恶意或错误
server让任务永久paused。

## 14. Callback ownership与pause/replay

### 14.1 Callback byte ownership

libcurl传入的`char*`只在本次callback期间可读。唯一合法路径：

```text
libcurl callback span
    │ PacketProducer::accept
    ├── accepted ──► Packet Flow lane draft
    ├── Queue/Memory Pause ──► zero consumed, libcurl retains/replays
    └── failed/closed ──► callback error
```

HTTP module不得：

- 保存callback pointer/span；
- 复制到第二个HTTP buffer；
- 在pause前推进accepted frontier；
- 在replay时根据pointer identity去重；
- 把libcurl pause buffer计入Packet Flow Accounted Bytes；
- callback中等待Persistence。

### 14.2 Write callback固定算法

对每个非空batch：

1. checked计算batch bytes；
2. 校验slot、Lease、session state与owner thread；
3. 若cancel/upstream failure，保存对应callback failure并返回error；
4. 检查当前final candidate headers；
5. checked验证`accepted_through + bytes <= lease.end`；
6. 构造`flow::DataChunk{lease.id, lease.bytes, accepted_through, span}`；
7. 调`PacketProducer::accept(lane, chunk)`；
8. `accepted`时要求`consumed_bytes == bytes`；
9. 只有第8步后：
   - first accepted nonzero batch记录`record_first_byte_received()`；
   - `accepted_through += bytes`；
   - `body_bytes += bytes`；
10. Queue/Memory admission结果时：
    - 要求`consumed_bytes == 0`；
    - 保存Packet Flow pause mask；
    - 在同一owner thread、返回前设置`curl_receive_paused = true`；
    - 返回`CURL_WRITEFUNC_PAUSE`；
11. closed/failed时保存first cause并返回`CURL_WRITEFUNC_ERROR`。

`PacketAdmission.published_bytes`可以非0且`consumed_bytes == 0`：表示本调用先成功发布旧draft，
但当前incoming未消费。HTTP只按`consumed_bytes`推进frontier，replay不会重复旧draft。

`curl_receive_paused = true` 是 callback 对即将生效实际状态的提交：libcurl 接受
`CURL_WRITEFUNC_PAUSE` 后立即把该 writer 置为 paused，callback 返回与 libcurl 接受之间
没有任何可执行 reconcile edge。若不先提交该位，后续 mask 清零会把
`should_pause == false` 与错误保留的 `curl_receive_paused == false` 判为无边沿，导致永久
停住。

### 14.3 Pause reason ownership

| Reason | State owner | Telemetry episode owner | CURL action owner |
| --- | --- | --- | --- |
| Queue | Packet Flow lane | Packet Flow | HTTP session |
| Memory | Packet Flow lane | Packet Flow | HTTP session |
| Gap | HTTP slot fed by Lifecycle fact | HTTP session | HTTP session |
| Window overflow | terminal response error | no pause metric | HTTP callback abort |

`set_gap_paused(token, true)`：

- stale/unknown token返回error且不记telemetry；
- bit `0→1`时唯一调用
  `TelemetrySession::record_pause(TelemetryPauseReason::gap, false)`；
- duplicate true不重复计；
- bit `1→0`不新增计数；
- Orchestrator投递fact前后都不得另记gap telemetry。

### 14.4 Reconcile与actual CURL state

`poll()`每轮先：

1. 用active slot的raw `bytes_per_second`生成`PacketLaneObservation`；
2. 提供 `actions.size() >= observations.size()` 的预分配 buffer，调唯一
   `PacketProducer::reconcile()`；
3. 检查 `PacketReconcileResult.error`；失败保存 first cause并取消 Session，成功时只读取
   `actions.first(action_count)`并映射到slot；
4. 计算
   `should_pause = packet_pause_mask != 0 || gap_paused`；
5. 与`curl_receive_paused`比较，只在edge调用`curl_easy_pause()`。

`pause_receive`调用`CURLPAUSE_RECV`，`resume`调用`CURLPAUSE_CONT`。每个return必须检查。
callback 返回 `CURL_WRITEFUNC_PAUSE` 的路径已经在返回前把 actual-state bit 置为 true；
因此 Queue/Memory mask 后续清零必然形成恰好一个 true→false resume edge。

slot rate 的唯一消费者是本轮 `PacketProducer::reconcile()`。HTTP 不把它写入
`TelemetrySession`、progress 或 Performance Summary，也不得调用
`record_download_delta()`；正式 network EMA、downloaded bytes、packet count 和 packet
size 只随 Packet Flow 的 successful Data Packet publish 更新。

### 14.5 Unpause同步重入

unpause顺序固定：

```text
apply Packet Flow/gap state
set curl_receive_paused = false
mark slot callback-ready
call curl_easy_pause(easy, CURLPAUSE_CONT)
do not overwrite slot state after return
```

原因是libcurl可能在函数返回前同步重入write callback。若重入callback再次Queue/Memory pause：

- callback更新Packet Flow bit；
- callback返回`CURL_WRITEFUNC_PAUSE`；
- slot最终仍是paused；
- outer unpause返回后不得强制写`false`覆盖。

Debug build记录`callback_active`防止非法remove/reset/close。Release错误路径保存
`protocol_order_invalid`，不throw/terminate。

## 15. Completion与Range Lifecycle关系

CURLMSG_DONE不是Range完成。对一个slot：

1. 复制message result和easy pointer；
2. checked get private/status/speed；speed只更新最终lane observation，不提交telemetry；
3. checked remove easy；
4. flush Packet Flow lane draft；
5. 若draft因当前兼容admission暂不可发布，保留Phase 2的1ms wait语义；
6. 优先读取slot保存的callback failure；
7. 再检查CURLcode；
8. 再检查final response/status/body；
9. 生成一个`HttpLeaseSucceeded`或`HttpLeaseFailed`；
10. event被poll返回后slot才可reuse。

结构阶段保持Phase 2已有的final-draft 1ms compatibility wait。不得在本阶段改为yield、
deferred completion、backoff或独立completion queue。若它成为benchmark热点，另开性能实验。

Engine映射：

```text
HttpLeaseSucceeded
    -> RangeLifecycle.apply(LeaseSucceeded{lease, received_through})

HttpLeaseFailed
    -> RangeLifecycle.apply(LeaseFailed{lease, accepted_through, cause})
```

HTTP module不调用Lifecycle，不决定failed Range是否重新调度。Phase 3当前no-auto-retry合同使
matching failure终止本Session。

成功event只有在：

- Packet Flow final draft已发布；
- CURLcode为OK；
- final status/header合同正确；
- actual body exact；
- accepted frontier exact；
- no callback failure；
- easy已从multi remove

时产生。

## 16. Poll/multi event loop contract

`poll(timeout)`固定：

1. wrong-thread、negative timeout、closed state确定性失败；
2. 若有pending slot event，立即返回最小slot id的event；
3. 应用Packet Flow reconcile与gap pause edge；
4. 调`curl_multi_perform()`；
5. 若multi error：
   - session进入failed；
   - 不再对同一multi调用perform；
   - active handles进入cancel/remove；
6. drain全部`curl_multi_info_read()` DONE messages；
7. 若任一 DONE 产生failed event，立即锁定session first error并禁止start；pending event仍按
   slot id逐个交付，不被通用`poll failed`吞掉；
8. 若产生event，返回一个，其余保留；
9. 若无active，返回idle；
10. 若无event且有active，调`curl_multi_wait(..., timeout)`；
11. wait后再perform/drain一次；
12. 无event返回timed_out。

保持Orchestrator调用timeout为100ms。`num_fds == 0`不是错误，也不代表transfer完成。

Orchestrator 每轮调度前必须反复调用 `poll(0ms)`，直到返回 `idle`/`timed_out`，并按顺序
apply 每个 event/effect。遇到 Lease failure、effect failure 或 session error 后立即禁止
acquire/start。Session 自身也以 pending-event gate 拒绝 start，所以同时 DONE 的
success+failure 不会出现“先交付 success、下一轮先启动新 Lease、再看到已知 failure”。

`running_handles`只作内部诊断，不用于：

- 判断每个Lease成功；
- 跳过info queue；
- 释放slot；
- 产生Range completion。

multi perform/wait错误后，不允许“再试一次看看”。这不是HTTP retry，而是libcurl明确规定
state uncertain；module必须停止同一multi。

## 17. Error mapping与first-error rule

### 17.1 Probe

| Internal reason | Public error |
| --- | --- |
| global/easy init | `http_init_failed` |
| option/getinfo/transport/status/header/body | `http_probe_failed` |
| allocation/protocol impossible state | `internal_error`，除非发生在curl setup则保留probe failed |

保持现有public probe error粗粒度；细分只用于internal tests/evidence。

### 17.2 Transfer

| Condition | Internal reason | Public error |
| --- | --- | --- |
| easy/multi creation before session | init reason | `http_init_failed` |
| setopt/multi option/add/pause | corresponding setup reason | `http_transfer_failed` |
| DNS/connect/TLS/timeout/recv/send/partial | transport | `http_transfer_failed` |
| status/CR/length/encoding/body mismatch | response reason | `http_invalid_response` |
| Packet Flow failed/closed due Persistence | callback_sink/upstream | preserve upstream primary error |
| explicit internal cancel | cancelled | `cancelled` |
| private pointer/wrong generation/order | protocol order | `internal_error` |
| allocation | allocation | `internal_error` |

若CURLMSG_DONE为`CURLE_WRITE_ERROR`：

- slot已有callback failure时，使用保存的cause；
- 无cause才映射generic `http_transfer_failed`；
- 不用CURLcode覆盖`content_range_mismatch`、Packet Flow failure或cancel。

常见CURLcode分组：

- `CURLE_COULDNT_RESOLVE_*`、`COULDNT_CONNECT`、`OPERATION_TIMEDOUT`、`RECV_ERROR`、
  `SEND_ERROR`、`PARTIAL_FILE`、`GOT_NOTHING` → transport；
- `CURLE_OUT_OF_MEMORY`、pause buffer `CURLE_TOO_LARGE` → allocation/resource failure，
  public `internal_error`或当前first primary；
- `CURLE_BAD_FUNCTION_ARGUMENT`、`UNKNOWN_OPTION`、`NOT_BUILT_IN`在setup时 →
  option failure；
- `CURLE_ABORTED_BY_CALLBACK`按已保存callback cause；无cause为transfer failure。

first-error顺序：

```text
upstream Persistence/Packet Flow primary
    > explicit cancel
    > saved callback response/sink cause
    > individual CURLcode
    > multi/cleanup symptom
```

cleanup error记录evidence但不覆盖已有primary。

## 18. Cancel、shutdown与lifetime

### 18.1 `cancel()`

`task_cancelled`要求cause为空；`upstream_failed`要求cause非空。

调用后：

1. session state → cancelling；
2. 禁止新`start()`；
3. 对每个active easy在callback外checked remove；
4. 根据 cancel kind 处理每个 lane draft：
   - `task_cancelled` 表示 HTTP/Range/task stop 但 Packet Flow 仍 open；使用 Phase 2 同一
     1 ms compatibility wait 反复 `PacketProducer::flush(lane)`，直到 accepted 或 flow
     closed/failed，不得直接 discard 已接受字节；
   - `upstream_failed` 表示 Packet Flow/Persistence 已不能接收；调用
     `PacketProducer::discard(lane)` 并检查返回值；
5. `task_cancelled` flush 中若 flow 转为 closed/failed，保存该 error，然后 discard
   remaining draft以平衡 accounting；它不能覆盖触发 task stop 的更早 HTTP primary；
6. 已发布packets由Persistence按Phase 2 drain；
7. 为每个active Lease生成一次failed event；
8. 不重新发起HTTP；
9. events drain后可`close()`。

若任务正常完成，不调用cancel；所有Lease先产生success event。

一条 Lease 的 HTTP failure 使整个 no-retry Session 停止时，engine先保留该 failure为
primary，再用`task_cancelled`取消其它slot。这样 peer slot 已进入lane draft的字节仍可进入
恢复产物，保持当前`stop_network_phase`语义。只有 Persistence/Packet Flow 已先失败时使用
`upstream_failed`并discard。

### 18.2 `close()`

合法前置：

- no active/in-multi handle；
- no pending event；
- no callback active；
- Packet Flow由outer orchestration按Phase 2顺序收尾。

顺序：

```text
remove all easy if needed
curl_easy_cleanup each
curl_multi_cleanup
free immutable header list
detach/destroy ProducerLane
state = closed
```

`curl_multi_cleanup`不能替代easy cleanup。

### 18.3 Global runtime

production factory在创建任何easy前显式调用一次`curl_global_init(CURL_GLOBAL_DEFAULT)`并保存
结果。必须发生在Persistence/workers启动前。

保持当前process-lifetime策略，不在每个Download Request调用`curl_global_cleanup()`，避免
并发request和static destruction ordering风险。若未来要显式global cleanup，需要单独process
runtime设计。

## 19. Thread、ownership与memory order

| Object/state | Owner/writer | Reader | Rule |
| --- | --- | --- | --- |
| Curl port/probe easy | Orchestrator | none | synchronous |
| multi/easy slots | Orchestrator | libcurl callbacks on same call stack | same thread |
| slot Lease/token/frontier | Orchestrator/callback same thread | poll | plain values |
| callback data | libcurl then Packet Flow | no retained borrow | callback lifetime |
| ProducerLane | HTTP slot | PacketFlow implementation | orchestrator confined |
| PacketLease/payload after admission | Packet Flow/Persistence | HTTP none | move ownership |
| RangeLifecycle | Orchestrator outside callback | HTTP event by value | no pointer |
| Gap fact | Lifecycle snapshot/fact drain | `set_gap_paused` by value | same owner thread |
| TelemetrySession | current facade | HTTP仅first-byte/Gap；Packet Flow独占download delta | existing synchronization |
| header list | HTTP session | libcurl | immutable until cleanup |

libcurl handle不能用mutex包装后跨线程调用；同handle多线程在官方contract下非法。wrong-thread
call：

- Debug assert；
- Release返回internal error；
- 无CURL side effect。

Packet Flow state跨Persistence的memory order由Phase 2负责；Range facts由Phase 3负责。HTTP
module不新增atomic mirror，不用relaxed load绕过它们。

销毁顺序：

```text
stop Range acquire
HTTP cancel/finish active easy
drain HTTP Lease events into Lifecycle
HTTP close
PacketProducer close
Persistence drain/join
final Range fact drain
Recovery finalize/close
destroy Lifecycle, PacketFlow, HTTP port
```

若Phase 4最终要求Packet Flow先close再HTTP object销毁，HTTP session仍必须先停止CURL生产；
精确对象析构可机械调整，但“no callback after PacketProducer destruction”不可改变。

## 20. Deterministic fake contract

test adapter位于tests，不链接libcurl：

```text
tests/http/deterministic_http_transfer.hpp
tests/http/deterministic_http_transfer.cpp
```

最小script值：

```cpp
struct FakeProbeStep {
    HttpProbeRequest expected;
    HttpProbeResult result;
};

struct FakeDelivery {
    range::ByteOffset offset = 0;
    std::vector<std::uint8_t> bytes;
};

struct FakeTransferScript {
    range::RangeLease expected_lease;
    std::vector<FakeDelivery> deliveries;
    std::optional<HttpFailure> terminal_failure;
    long response_code = 206;
};
```

fake规则：

- probe request必须exact match；
- start必须按script顺序和完整Lease value match；
- poll不sleep，每次最多推进一个script step；
- delivery走真实`PacketProducer::accept()`，不绕过Packet Flow；
- admission pause时保留同一FakeDelivery，下一次poll重放相同bytes；
- accepted前offset不推进；
- gap active时不delivery；
- cancel后每个active Lease只产生一个failure event；
- script耗尽、额外start、错Lease或错token返回internal error；
- fake不模拟CURLcode、header parser或socket；
- production response correctness由真实server合同测试负责。

这样fake用来测试engine orchestration、Range event mapping、cancel和no-retry；它不是伪造
libcurl implementation的巨型mock。

## 21. Real HTTP server合同测试

扩展`tests/support/range_server.py`，保留现有默认行为，并增加组合开关。server log至少记录：

```text
request ordinal
method
request HTTP version
client port
Range
Accept-Encoding
response status
response Content-Range
response Content-Encoding
body bytes sent
```

必须可确定性生成：

- HEAD 200；
- HEAD 405；
- HEAD缺Content-Length；
- HEAD 200但强制返回gzip encoding；
- HEAD unsolicited 206并带partial Content-Length与Content-Range；
- one-byte Range 206；
- ignored Range 200；
- disabled Range；
- shifted start/end/total；
- malformed/duplicate Content-Range；
- short/long body；
- omitted/conflicting Content-Length；
- chunked exact body；
- redirect chain；
- gzip only whenclient允许；
- forced gzip despiteidentity；
- connection close afterN bytes；
- fixed chunk与barrier delay。

server默认模式必须让现有tests不改命令仍工作。

### 21.1 Probe cases

- `HttpTransferContractTest.ProbesHeadSuccess`
- `HttpTransferContractTest.FallsBackFromRejectedHeadToZeroZeroRange`
- `HttpTransferContractTest.FallsBackWhenHeadLengthIsMissing`
- `HttpTransferContractTest.ClassifiesIgnoredFallbackRangeAsNonRange`
- `HttpTransferContractTest.UsesOnlyFinalRedirectHeaderBlock`
- `HttpTransferContractTest.RejectsMalformedFallbackContentRange`
- `HttpTransferContractTest.RejectsMismatchedFallbackTotal`
- `HttpTransferContractTest.RejectsZeroByteObjectByCurrentPolicy`
- `HttpTransferContractTest.RequestsIdentityForHeadAndFallback`
- `HttpTransferContractTest.UsesHeadThenExplicitGetForFallback`
- `HttpTransferContractTest.RejectsForcedGzipHeadWithoutFallback`
- `HttpTransferContractTest.FallsBackFromUnsolicitedHead206AndIgnoresItsPartialLength`

### 21.2 Transfer response cases

- exact partial 206；
- exact full 206；
- whole Range ignored to exact 200；
- partial Range ignored to 200 rejected beforepacket；
- missing/malformed/shifted/duplicate CR；
- CR total differs fromprobe；
- non-Range unsolicited 206；
- final 204/304/416/500；
- Content-Length mismatch；
- chunked exact；
- short body；
- long body terminal rather thanpermanent pause；
- final redirect block；
- forced gzip rejected beforeadmission；
- identity body exact。

每个invalid response同时断言：

- public error；
- internal reason；
- accepted/persisted frontier；
- request count；
- no automatic retry；
- recovery artifacts按Phase 4合同保留；
- no false Range finished。

### 21.3 Pause/replay

使用real curl + real Packet Flow + controlled Persistence barrier：

1. packet budget设为1；
2. server固定一个callback可见chunk；
3. consumer暂不release credit；
4. callback返回pause；
5. 记录accepted frontier不变；
6. release consumer；
7. poll触发unpause；
8. 同一batch重放；
9. output checksum与source一致；
10. 每个offset只进入Packet Flow一次。

测试名：

- `ReplaysPausedWriteBatchExactlyOnce`
- `DoesNotAdvanceAcceptedFrontierBeforeReplayAcceptance`
- `SupportsSynchronousReplayDuringUnpause`
- `DoesNotDoubleCountFirstByteOnReplay`
- `DoesNotDoubleCountQueueOrMemoryPauseEpisode`

### 21.4 Physical connection与protocol

保留并强化`UsesDistinctClientPortsAcrossConcurrentRanges`：

- 至少4个overlapping Range windows；
- log显示HTTP/1.1；
- 每个active window使用不同client port；
- 后续window不reuse已完成port；
- no HTTP/2/3；
- request count等于签发Lease数；
- failure Lease没有第二次attempt。

## 22. Migration mapping

### 22.1 Probe

替换：

| Old | New |
| --- | --- |
| `download::HttpProbe` | `HttpTransferPort::probe()` |
| `core::RemoteProbeResult` | `http::HttpObjectFacts` + explicit mappings |
| probe-local `ProbeHeaders` | private final-block accumulator |
| engine `CurlGlobal` | Curl production adapter runtime |

旧`http_probe.hpp/.cpp`在production caller归零后删除，不保留wrapper转发。

### 22.2 `TransferHandle`

| Old field/helper | New owner |
| --- | --- |
| `CURL* easy` | Curl slot |
| `RangeContext* range` | immutable `RangeLease` |
| queue/token pointers | Phase 2 `ProducerLane` |
| request start/end/next | Lease span + accepted frontier |
| range header | Curl request formatter |
| response code | response accumulator |
| buffer/accounted fields | Packet Flow lane |
| queue/memory bits | Packet Flow |
| gap bit | HTTP slot |
| window-boundary bit | delete; overflow is terminal |
| speed | HTTP slot raw observation → `PacketLaneObservation` only |
| curl result | Curl slot internal |

### 22.3 Engine loop

目标Orchestrator只做：

```text
validate policy
probe
bind policy
open recovery/PacketFlow/Lifecycle/Persistence
open HttpTransferSession
while work remains:
    while true:
        event = HTTP.poll(0ms)
        if event:
            map event to Lifecycle.apply()
            apply/publish effects; completion requires PacketPublishCode::published
            stop immediately on any effect failure
            continue
        break
    drain Lifecycle facts
    feed gap changes
    drain matching Persistence geometry ACKs
    while HTTP slots available and no pending event/error:
        Lifecycle.acquire()
        submit Register/Resize effects
        store Lease + required tickets as pending arm
    for each pending arm whose tickets all succeeded:
        HTTP.start(lease)
        on synchronous start failure apply LeaseFailed immediately
    if active transfers and no immediate work:
        event = HTTP.poll(100ms)
        if event:
            map/apply it immediately, then continue so poll(0ms) drains the rest
    emit progress
stop/close in documented order
```

engine不得：

- setcurl option；
- parse header；
- callpause；
- mapCURLcode；
- inspectresponse code；
- flush HTTP aggregation；
- storeRange pointer innetwork slot。

### 22.4 Telemetry

- first byte：HTTP callback在Packet Flow成功消费首个nonzero batch后唯一记录；
- queue/memory pause：Packet Flow唯一记录；
- gap pause：HTTP `set_gap_paused()` bit `0→1`唯一记录；
- lane rate：HTTP slot只采样raw rate并形成`PacketLaneObservation`，唯一用于
  `PacketProducer::reconcile()`；
- network EMA、downloaded bytes、packet count和packet size：Packet Flow在successful
  Data Packet publish后唯一调用`TelemetrySession::record_download_delta(bytes)`；
- HTTP、Orchestrator和Engine都不得因slot sample、callback accepted bytes或
  `PacketAdmission::published_bytes`再次调用`record_download_delta()`；
- active requests：`HttpSessionSnapshot.active_transfers`；
- paused transfers：snapshot观测，不成为Range state；
- Phase 6可吸收collector，但不得改变这些producer ownership。

## 23. Expected file layout

```text
src/http/http_transfer.hpp
src/http/curl_http_transfer.hpp
src/http/curl_http_transfer.cpp
src/http/http_response_accumulator.hpp
src/http/http_response_accumulator.cpp
src/download/download_engine.hpp
src/download/download_engine.cpp
src/download/http_probe.hpp
src/download/http_probe.cpp
tests/http/http_transfer_fake_test.cpp
tests/http/http_transfer_contract_test.cpp
tests/http/http_transfer_pause_test.cpp
tests/http/deterministic_http_transfer.hpp
tests/http/deterministic_http_transfer.cpp
tests/support/range_server.py
tests/download/download_resume_integration_test.cpp
```

最终删除`src/download/http_probe.*`。若production factory implementation足够小，
`http_transfer.cpp`可合并到`curl_http_transfer.cpp`；interface和Curl implementation仍须保持
seam清晰。

## 24. Tests-first tiny commits

| Slice | Class | Commit intent | Verification | Rollback |
| ---: | --- | --- | --- | --- |
| 05.1 | test | characterize probe、status、pause replay、distinct ports与known gaps | existing green + explicit red risks | only tests/server modes |
| 05.2 | S | add high-level port/session values and deterministic fake | fake command/event tests | remove new interface/fake |
| 05.3 | S | add production probe adapter and final-header accumulator | HEAD/fallback real server | restore old HttpProbe caller |
| 05.4 | C | isolate final response blocks and strictly validate HEAD/fallback status and total | redirect/malformed/unsolicited HEAD 206 red→green | revert parser behavior only |
| 05.5 | S | add fixed Curl session/slots with checked easy/multi lifecycle, not wired | create/start/cancel/close tests | remove session implementation |
| 05.6 | S | migrate one then all network producers to PacketProducer lanes | pause replay/ownership tests | restore engine callback adapter |
| 05.7 | S | map RangeLease/start and HTTP events to Lifecycle | Lease identity/stale/no-retry | restore compatibility event adapter |
| 05.8 | C | validate status/CR/length before body admission | ignored/shifted/malformed response red→green | revert validation activation |
| 05.9 | C | request identity and disable/reject content decoding for probe and transfer | gzip HEAD/Range red→green | revert encoding options only |
| 05.10 | C | make long body terminal and classify short body | long no-hang, exact reasons | restore old behavior only for evidence |
| 05.11 | C | check all setopt/getinfo/pause/multi returns and preserve first cause | fault/invalid-option tests | revert checking activation |
| 05.12 | S | centralize cancel, DONE drain, cleanup and remove engine curl helpers | shutdown/error matrix | restore old cleanup adapter |
| 05.13 | S | delete HttpProbe/TransferHandle curl state and compatibility paths | static checks + full tests | revert deletion commit |
| 05.14 | perf | Release pre/post benchmark then profiler if needed | Section 27 gates | revert smallest regressing slice |
| 05.15 | docs | record source versions, tests, benchmark, rollback ids | exit checklist | docs only |

05.6迁移期可有compile-time compatibility adapter，最多跨相邻slices。禁止old/new runtime flag。

05.8–05.11都是C类，即使它们使代码更整洁，也不能写成`refactor:`。

## 25. Exact unit/interface tests

### 25.1 Fake/interface

- `HttpTransferFakeTest.MatchesProbeRequestExactly`
- `HttpTransferFakeTest.StartsOnlyExpectedLease`
- `HttpTransferFakeTest.ReturnsNoCapacityWithoutConsumingScript`
- `HttpTransferFakeTest.DeliversAtMostOneEventPerPoll`
- `HttpTransferFakeTest.FeedsBytesThroughRealPacketProducer`
- `HttpTransferFakeTest.ReplaysSameDeliveryAfterPacketPause`
- `HttpTransferFakeTest.GapPauseBlocksDelivery`
- `HttpTransferFakeTest.CountsGapOnlyOnZeroToOne`
- `HttpTransferFakeTest.RejectsStaleTransferToken`
- `HttpTransferFakeTest.CancelProducesOneFailurePerActiveLease`
- `HttpTransferFakeTest.NeverRetriesFailedLease`
- `HttpTransferFakeTest.CloseRejectsPendingEvent`

### 25.2 Session lifecycle

- `CurlHttpTransferTest.CreatesFixedStableSlots`
- `CurlHttpTransferTest.IncrementsSlotGenerationOnReuse`
- `CurlHttpTransferTest.RejectsWrongThreadWithoutCurlSideEffect`
- `CurlHttpTransferTest.RemovesEasyBeforeResetAndReuse`
- `CurlHttpTransferTest.DrainsDoneMessageEvenWhenRunningCountIsZero`
- `CurlHttpTransferTest.DrainsSimultaneousSuccessAndFailureBeforeNewStart`
- `CurlHttpTransferTest.PendingEventMakesAvailableSlotsZero`
- `CurlHttpTransferTest.StopsMultiAfterPerformError`
- `CurlHttpTransferTest.PreservesCallbackCauseOverWriteError`
- `CurlHttpTransferTest.PreservesUpstreamCauseOverCleanupError`
- `CurlHttpTransferTest.ClosesEasyBeforeMulti`
- `CurlHttpTransferTest.KeepsHeaderListAliveUntilAllEasyCleanup`
- `CurlHttpTransferTest.StartOptionFailureReturnsSynchronouslyWithoutPendingEvent`
- `CurlHttpTransferTest.StartAddHandleFailureCanCloseWithoutPoll`
- `CurlHttpTransferTest.FlushesPeerDraftWhenHttpFailureStopsOpenFlow`
- `CurlHttpTransferTest.DiscardsDraftWhenUpstreamFlowAlreadyFailed`
- `CurlHttpTransferTest.PreservesHttpPrimaryIfCancelFlushObservesFlowFailure`
- `CurlHttpTransferTest.ResumesExactlyOnceAfterCallbackQueuePauseClears`
- `CurlHttpTransferTest.ResumesExactlyOnceAfterCallbackMemoryPauseClears`

### 25.3 Response parser through production interface

测试主要经real server使用`probe/start/poll` interface。若纯parser test用于快速覆盖grammar，
它必须放implementation-private test target，不得公开parser interface；真实server tests仍是
合并gate。

至少覆盖：

- decimal overflow；
- duplicate CR；
- identical/conflicting Content-Length；
- OWS；
- lower/upper-case names/unit；
- star total；
- inclusive last overflow；
- redirect block reset；
- trailer ignored；
- non-identity token list。

### 25.4 Cross-module

- dynamic Range geometry必须收到Persistence success ACK后才调用`start()`；
- ACK pending arm计入slot capacity；ACK failure产生`EffectApplicationFailed`且不arm；
- HTTP callback只调用Phase 2 producer；
- callback不调用Lifecycle；
- success event后Lifecycle仍等待Persistence；
- failure event返回unpersisted suffix且no retry；
- Gap fact只由HTTP实际episode计一次；
- Queue/Memory telemetry不被HTTP重复记；
- slot rate只进入`PacketLaneObservation`，HTTP不提交download/rate/packet telemetry；
- 每个successful Data Packet publish只由Packet Flow调用一次`record_download_delta()`；
- reconcile actions buffer覆盖全部observations，reconcile error不产生部分CURL pause edge；
- completion effect只有`PacketPublishCode::published`才算成功，`closed/failed`停止任务；
- Recovery identity仍用probe facts和原始URL；
- server failure保留artifacts；
- formal summary exact 10 keys。

## 26. Static/deletion checks

阶段完成后：

```powershell
rg -n "#include <curl/curl.h>|\\bCURLM?\\b|CURLcode|CURLMcode|curl_" `
  src -g "*.cpp" -g "*.hpp"

rg -n "TransferHandle|response_is_valid|arm_transfer|resume_paused_transfers|apply_gap_pauses" `
  src/download

rg -n "paused_by_window_boundary|range_header|request_start|request_end|next_offset" `
  src/download src/http

rg -n "CURLOPT_ACCEPT_ENCODING.*\"\"" src

rg -n "HttpProbe|RemoteProbeResult" src tests

rg -n "HttpTransfer|curl" include/asyncdownload
```

期望：

- libcurl symbols只存在于`src/http/curl_http_transfer.cpp`及必要private Curl runtime file；
- engine无CURL；
- old helpers/fields为0；
- empty Accept-Encoding为0；
- old probe production type为0；
- public include无internal HTTP/Curl dependency；
- docs与test fixture中的历史文字不在删除范围。

额外检查：

```powershell
rg -n "record_first_byte_received|record_download_delta|TelemetryPauseReason::gap" src
rg -n "TelemetryPauseReason::queue_full|TelemetryPauseReason::memory_pressure" src
```

期望：

- first-byte production call site只有HTTP accepted callback路径；
- `record_download_delta` production call site只有Packet Flow successful publish路径；
- gap production call site只有`set_gap_paused` 0→1；
- queue/memory episode仍只有Packet Flow。

## 27. Performance-neutral gate

### 27.1 固定行为

MUST 保持：

- HTTP/1.1；
- fresh + forbid reuse；
- max total/host connection数值；
- current scheduler Lease/window数；
- 64 KiB Packet Flow aggregation；
- callback到PacketProducer不新增per-batch heap allocation、mutex或virtual call beyond
  high-level session dispatch；
- 100ms multi wait；
- final draft 1ms compatibility wait；
- Queue/Memory/Gap episode定义；
- network EMA、downloaded bytes和packet summary仍由Packet Flow successful publish唯一驱动；
- Packet Flow high/low与Top 20%；
- persistence/flush/recovery；
- 正式10 keys。

high-level session virtual dispatch只发生在start/poll/gap/cancel等低频路径。write callback
必须直接落到concrete Curl slot/internal function，不能每batch经过`std::function`或virtual
HTTP port。

identity correction可能改变server representation，但它是correctness，不得包装成吞吐收益。
正式benchmark server应返回identity，保证pre/post比较同一对象。

### 27.2 Release benchmark

在actual base上同机、同URL、同server、同repeats：

```powershell
scripts\build.bat release

python scripts\performance\benchmark.py `
  --url "http://127.0.0.1:4287/1gb_files.zip" `
  --benchmark-suite regression_v2 `
  --case-list baseline_default,balanced_candidate,memory_guard,scheduler_stress `
  --repeats 20 `
  --label "phase-5-pre"

python scripts\performance\benchmark.py `
  --url "http://127.0.0.1:4287/1gb_files.zip" `
  --benchmark-suite regression_v2 `
  --case-list baseline_default,balanced_candidate,memory_guard,scheduler_stress `
  --repeats 20 `
  --label "phase-5-post"
```

风险探针各20次：

- `deep_buffer_candidate`；
- `queue_backpressure_stress`；
- `gap_tolerance_probe`。

接近gate或双峰时提升到40。

| Signal | Gate |
| --- | --- |
| baseline_default network/disk median | decline <= 5% |
| balanced_candidate network/disk median | decline <= 5% |
| TTFB | no systematic regression；first-byte语义不变 |
| memory_guard max memory | 保持实际pre低内存形态，历史约4.2 MiB |
| max inflight | 多case无无收益显著上升 |
| total/queue pause | 多case约15%以上回归需调查 |
| packet shape | avg/max仍约64 KiB口径 |
| active connections | distinct physical connection合同不变 |
| schema | exact 10 keys |

### 27.3 Profiler

只有benchmark显示变化或需要解释hot path时再单独跑：

```powershell
python scripts\performance\profiler.py `
  --url "http://127.0.0.1:4287/1gb_files.zip" `
  --benchmark-suite regression `
  --case-list throughput_candidate,scheduler_stress `
  --label "phase-5-profile"
```

只回答：

- response parser是否进入per-body hot path；
- callback是否新增allocation/copy；
- high-level port是否被误用到per-batch；
- pause replay/curl buffer路径是否变化；
- final-draft wait是否成为新热点。

禁止借机实施：

- connection reuse；
- HTTP/2/3；
- 改`curl_multi_wait`为poll/socket；
- 128 KiB aggregation；
- 更大queue/window；
- retry/backoff；
- 每轮只resume一个handle；
- 新diagnostic summary fields。

## 28. Rollback strategy

### 28.1 Per-slice

- unwired interface/fake可直接回滚；
- probe adapter失败只恢复old HttpProbe caller；
- session skeleton失败不影响old engine path；
- callback producer migration失败恢复相邻compatibility adapter；
- Range event mapping失败不回滚Phase 3；
- encoding/header/body correctness失败只回滚对应C commit并保持stage未完成；
- cleanup migration失败恢复old cleanup adapter，但保留contract tests；
- benchmark regression回滚最小触发slice，不调defaults掩盖。

### 28.2 Whole stage

反向：

1. 恢复engine cleanup/event loop；
2. 恢复TransferHandle callback；
3. 恢复old HttpProbe；
4. 移除Curl production adapter；
5. 保留旧实现也能通过的characterization fixtures/tests与风险文档；
6. 同步回滚或显式禁用只有新correctness行为才能通过的Content-Range、encoding、long-body
   和failure-injection green tests，把其失败输出保留在evidence，不让baseline停在red。

本阶段无public或metadata格式迁移，不需要数据迁移。

### 28.3 禁止runtime dual path

不得长期保留：

- `use_new_http_transfer`；
- old/new callback toggle；
- `allow_legacy_content_range`；
- encoding compatibility flag；
- retry fallback；
- fake选择production的runtime enum。

回滚依赖小commit，不依赖双轨。

## 29. Stop conditions

出现以下任一情况，停止当前slice并回到Wayfinder，不自行扩scope：

- 实际链接libcurl版本不满足本文已核实的pause/replay合同；
- callback不在Orchestrator call stack执行；
- unpause只能从Persistence thread完成；
- same batch在real test出现duplicate或missing；
- body必须在header validation前进入Packet Flow才能工作；
- identity response无法在不自动解码的前提下取得；
- server必须通过retry才能从ignored Range恢复；
- distinct physical connections无法保持；
- HTTP module需要读取/修改RangeLifecycle internal state；
- PacketProducer不能由唯一owner thread使用；
- pending DONE event尚未交付时仍可能start新Lease；
- HTTP failure取消peer transfer时只能discard仍可flush的lane draft；
- start failure既同步返回failed又遗留必须poll的pending event；
- error mapping必须覆盖Persistence primary才能收尾；
- long body仍只能通过无限pause处理；
- setopt/getinfo/multi错误无法确定性停止；
- callback hot path需要新增mutex/per-batch allocation；
- 正式benchmark超过gate且无法归因到可回滚slice；
- 必须改变public headers、CLI、metadata或recovery格式；
- 必须引入auth/proxy/HTTP2/connection reuse。

独立slice可继续；冲突slice保存具体symbol、libcurl branch、server trace和test evidence。

## 30. Acceptance checklist

- [ ] actual base、vcpkg curl version和local source路径已记录。
- [ ] HEAD与0-0 fallback合同通过real server。
- [ ] HEAD显式NOBODY；fallback显式HTTPGET且Range为0-0，method state不从前一 operation
      泄漏。
- [ ] HEAD forced-gzip直接失败且不fallback；unsolicited HEAD 206必须显式fallback且不采用
      其partial Content-Length。
- [ ] final response block不受redirect/intermediate headers污染。
- [ ] Probe facts映射到Policy与Recovery，无重复HTTP parser。
- [ ] HttpTransferPort只有production Curl adapter与deterministic fake。
- [ ] fake不链接libcurl，production interface不暴露CURL。
- [ ] RangeLease按值进入HTTP，无RangeContext pointer。
- [ ] Register/Resize success ACK先于对应HTTP start，pending arm计入slot capacity。
- [ ] 每轮acquire前已用poll(0) drain pending events；同时success+failure不会启动新Lease。
- [ ] synchronous start failure携带HttpFailure且不排event，caller无需poll即可close。
- [ ] callback只调用Phase 2唯一PacketProducer。
- [ ] pause result时当前batch consumed=0。
- [ ] callback返回pause前设置actual paused state；Queue/Memory mask清零后各恰好一次CONT。
- [ ] replay后每个offset只admit一次。
- [ ] unpause前state完整，支持同步callback重入。
- [ ] curl pause只由Orchestrator owner thread调用。
- [ ] Queue/Memory telemetry只由Packet Flow记录。
- [ ] Gap telemetry只由set_gap_paused 0→1记录。
- [ ] first byte只在首个accepted nonzero body后记录。
- [ ] slot rate只进入PacketLaneObservation，不写TelemetrySession/progress/summary。
- [ ] reconcile检查result/error/action_count；discard与completion publish结果均不被忽略。
- [ ] record_download_delta只由Packet Flow successful Data Packet publish调用一次。
- [ ] invalid status/CR/length/encoding在body admission前拒绝。
- [ ] CR start/end/total exact匹配Lease与probe。
- [ ] short/long body有确定性error，long不永久pause。
- [ ] HTTP/task stop在Packet Flow open时flush peer drafts；upstream flow failure才discard。
- [ ] probe与transfer显式请求identity且decoder关闭。
- [ ] non-identity Content-Encoding拒绝。
- [ ] every setopt/getinfo/pause/multi return checked。
- [ ] callback/upstream first cause不被CURLE_WRITE_ERROR覆盖。
- [ ] DONE message先复制，再remove，slot后续才reuse。
- [ ] multi error后不继续perform同一multi。
- [ ] remove → easy cleanup → multi cleanup顺序正确。
- [ ] header list生命周期覆盖全部easy use。
- [ ] HTTP/1.1与fresh/forbid reuse保持。
- [ ] concurrent Range使用distinct client ports。
- [ ] failed Lease request count证明no auto retry。
- [ ] public/CLI/recovery/10-key summary兼容。
- [ ] old HttpProbe、TransferHandle curl字段、window pause flag删除。
- [ ] Debug/Release/full/integration tests通过；known 740不扩散。
- [ ] Release pre/post benchmark通过gate。
- [ ] profiler仅在需要时解释hot path。
- [ ] 每个S/C slice有独立evidence和rollback id。

## 31. Code Agent start order

1. 读取阶段0–5、`docs/architecture/refactor/domain_glossary.md`、性能playbook和本文件列出的
   libcurl 8.18 source/docs；
2. 记录actual base、dirty worktree、curl version、build features和known Windows 740；
3. 只实现05.1 tests/server modes；
4. 实现unwired port + fake；
5. 单独迁移probe并锁定final header block；
6. 实现unwiredCurl session和checked lifecycle；
7. 迁移PacketProducer callback与RangeLease events；
8. 每个C类缺口先red再fix；
9. 每个slice后跑static/deletion checks；
10. real server覆盖pause/replay、encoding、response matrix和distinct ports；
11. Debug/Release全绿后跑正式Release benchmark；
12. 保存rollback/evidence，全部checklist完成后才标记Phase 5完成。

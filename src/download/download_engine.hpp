#pragma once

#include "asyncdownload/types.hpp"

namespace asyncdownload::download {

class DownloadEngine {
public:
    // 负责把 probe、恢复判定、调度、网络事件循环、持久化线程和最终收尾串成
    // 一次完整下载任务。对外保证不抛异常，错误统一折叠到 DownloadResult.error。
    [[nodiscard]] DownloadResult run(const DownloadRequest& request) noexcept;
};

} // namespace asyncdownload::download

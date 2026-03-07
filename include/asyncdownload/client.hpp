#pragma once

#include "asyncdownload/types.hpp"

namespace asyncdownload {

class DownloadClient {
public:
    DownloadClient();
    ~DownloadClient();

    DownloadClient(const DownloadClient&) = delete;
    DownloadClient& operator=(const DownloadClient&) = delete;
    DownloadClient(DownloadClient&&) = delete;
    DownloadClient& operator=(DownloadClient&&) = delete;

    [[nodiscard]] DownloadResult download(const DownloadRequest& request) noexcept;
};

} // namespace asyncdownload

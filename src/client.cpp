#include "asyncdownload/client.hpp"

#include "download/download_engine.hpp"

namespace asyncdownload {

DownloadClient::DownloadClient() = default;

DownloadClient::~DownloadClient() = default;

DownloadResult DownloadClient::download(const DownloadRequest& request) noexcept {
    download::DownloadEngine engine;
    return engine.run(request);
}

} // namespace asyncdownload

#pragma once

#include <memory>
#include <system_error>

#include "asyncdownload/types.hpp"

namespace asyncdownload::http {
class HttpTransferPort;
}

namespace asyncdownload::download::detail {

using HttpTransferPortFactory = std::error_code (*)(
    std::unique_ptr<http::HttpTransferPort>&) noexcept;
using PostPersistenceStartCheck = std::error_code (*)() noexcept;

struct DownloadEngineDependencies {
    HttpTransferPortFactory create_http_transfer_port = nullptr;
    PostPersistenceStartCheck post_persistence_start_check = nullptr;
};

[[nodiscard]] DownloadResult run_download(
    const DownloadRequest& request,
    const DownloadEngineDependencies& dependencies) noexcept;

}

#include "asyncdownload/error.hpp"

#include <string>

namespace asyncdownload {
namespace {

class DownloadErrorCategory final : public std::error_category {
public:
    [[nodiscard]] const char* name() const noexcept override {
        return "asyncdownload";
    }

    [[nodiscard]] std::string message(const int condition) const override {
        switch (static_cast<DownloadErrc>(condition)) {
        case DownloadErrc::ok:
            return "ok";
        case DownloadErrc::invalid_request:
            return "invalid request";
        case DownloadErrc::create_directory_failed:
            return "create directory failed";
        case DownloadErrc::open_file_failed:
            return "open file failed";
        case DownloadErrc::preallocate_failed:
            return "preallocate file failed";
        case DownloadErrc::file_write_failed:
            return "file write failed";
        case DownloadErrc::file_flush_failed:
            return "file flush failed";
        case DownloadErrc::file_read_failed:
            return "file read failed";
        case DownloadErrc::metadata_parse_failed:
            return "metadata parse failed";
        case DownloadErrc::metadata_save_failed:
            return "metadata save failed";
        case DownloadErrc::metadata_mismatch:
            return "metadata mismatch";
        case DownloadErrc::http_init_failed:
            return "http init failed";
        case DownloadErrc::http_probe_failed:
            return "http probe failed";
        case DownloadErrc::http_invalid_response:
            return "http invalid response";
        case DownloadErrc::http_range_unsupported:
            return "http range unsupported";
        case DownloadErrc::http_transfer_failed:
            return "http transfer failed";
        case DownloadErrc::queue_paused:
            return "transfer paused by queue backpressure";
        case DownloadErrc::cancelled:
            return "operation cancelled";
        case DownloadErrc::internal_error:
            return "internal error";
        }

        return "unknown asyncdownload error";
    }
};

} // namespace

const std::error_category& download_error_category() noexcept {
    static DownloadErrorCategory category;
    return category;
}

std::error_code make_error_code(const DownloadErrc errc) noexcept {
    return {static_cast<int>(errc), download_error_category()};
}

} // namespace asyncdownload

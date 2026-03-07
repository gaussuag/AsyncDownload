#pragma once

#include <system_error>

namespace asyncdownload {

enum class DownloadErrc {
    ok = 0,
    invalid_request,
    create_directory_failed,
    open_file_failed,
    preallocate_failed,
    file_write_failed,
    file_flush_failed,
    file_read_failed,
    metadata_parse_failed,
    metadata_save_failed,
    metadata_mismatch,
    http_init_failed,
    http_probe_failed,
    http_invalid_response,
    http_range_unsupported,
    http_transfer_failed,
    queue_paused,
    cancelled,
    internal_error
};

[[nodiscard]] const std::error_category& download_error_category() noexcept;
[[nodiscard]] std::error_code make_error_code(DownloadErrc errc) noexcept;

} // namespace asyncdownload

namespace std {

template <>
struct is_error_code_enum<asyncdownload::DownloadErrc> : true_type {};

} // namespace std

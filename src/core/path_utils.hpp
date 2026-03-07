#pragma once

#include <filesystem>

namespace asyncdownload::core {

[[nodiscard]] std::filesystem::path make_temporary_path(
    const std::filesystem::path& output_path);

[[nodiscard]] std::filesystem::path make_metadata_path(
    const std::filesystem::path& output_path);

} // namespace asyncdownload::core

#pragma once

#include "core/models.hpp"

#include <filesystem>
#include <optional>
#include <system_error>

namespace asyncdownload::metadata {

class MetadataStore {
public:
    explicit MetadataStore(std::filesystem::path path);

    [[nodiscard]] const std::filesystem::path& path() const noexcept;
    [[nodiscard]] std::error_code save(const core::MetadataState& state) noexcept;
    [[nodiscard]] std::pair<std::error_code, std::optional<core::MetadataState>> load() const noexcept;
    [[nodiscard]] std::error_code remove() noexcept;

private:
    std::filesystem::path path_;
};

} // namespace asyncdownload::metadata


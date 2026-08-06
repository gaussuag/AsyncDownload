#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace asyncdownload::core {

[[nodiscard]] std::optional<std::size_t> required_block_count(
    std::int64_t total_size,
    std::size_t block_size) noexcept;

}

#include <limits>

#include "core/block_geometry.hpp"

namespace asyncdownload::core {

std::optional<std::size_t> required_block_count(
    const std::int64_t total_size,
    const std::size_t block_size) noexcept {
    if (total_size <= 0 || block_size == 0) {
        return std::nullopt;
    }
    if (block_size > static_cast<std::size_t>(
            std::numeric_limits<std::int64_t>::max())) {
        return std::size_t{1};
    }

    const auto divisor = static_cast<std::int64_t>(block_size);
    const auto quotient = total_size / divisor;
    const auto remainder = total_size % divisor;
    const auto count = static_cast<std::uint64_t>(quotient) +
        (remainder == 0 ? 0U : 1U);
    if (count > std::numeric_limits<std::size_t>::max()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(count);
}

}

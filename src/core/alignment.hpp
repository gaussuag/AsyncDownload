#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace asyncdownload::core {

[[nodiscard]] inline std::int64_t align_up(const std::int64_t value,
                                           const std::size_t alignment) noexcept {
    const auto mask = static_cast<std::int64_t>(alignment - 1);
    return (value + mask) & ~mask;
}

[[nodiscard]] inline std::int64_t align_down(const std::int64_t value,
                                             const std::size_t alignment) noexcept {
    const auto mask = static_cast<std::int64_t>(alignment - 1);
    return value & ~mask;
}

[[nodiscard]] inline std::size_t bytes_to_alignment(const std::int64_t value,
                                                    const std::size_t alignment) noexcept {
    const auto remainder = static_cast<std::size_t>(value %
        static_cast<std::int64_t>(alignment));
    return remainder == 0 ? 0 : alignment - remainder;
}

[[nodiscard]] inline std::size_t full_aligned_prefix(const std::size_t size,
                                                     const std::size_t alignment) noexcept {
    return size - (size % alignment);
}

[[nodiscard]] inline std::int64_t clamp_offset(const std::int64_t value,
                                               const std::int64_t minimum,
                                               const std::int64_t maximum) noexcept {
    return std::min(std::max(value, minimum), maximum);
}

} // namespace asyncdownload::core

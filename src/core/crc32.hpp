#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace asyncdownload::core {

[[nodiscard]] std::uint32_t crc32(std::span<const std::byte> bytes) noexcept;

} // namespace asyncdownload::core

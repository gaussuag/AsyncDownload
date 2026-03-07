#include "crc32.hpp"

#include <array>

namespace asyncdownload::core {
namespace {

[[nodiscard]] constexpr std::array<std::uint32_t, 256> make_table() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t index = 0; index < table.size(); ++index) {
        std::uint32_t value = index;
        for (int bit = 0; bit < 8; ++bit) {
            value = (value & 1U) != 0U ? (0xEDB88320U ^ (value >> 1U)) : (value >> 1U);
        }
        table[index] = value;
    }
    return table;
}

} // namespace

std::uint32_t crc32(const std::span<const std::byte> bytes) noexcept {
    static constexpr auto table = make_table();

    std::uint32_t value = 0xFFFFFFFFU;
    for (const auto byte : bytes) {
        const auto lookup = static_cast<std::uint8_t>(value ^
            static_cast<std::uint8_t>(byte));
        value = table[lookup] ^ (value >> 8U);
    }

    return value ^ 0xFFFFFFFFU;
}

} // namespace asyncdownload::core

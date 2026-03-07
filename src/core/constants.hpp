#pragma once

#include <cstddef>

namespace asyncdownload::core {

inline constexpr std::size_t kDefaultBlockSize = 64 * 1024;
inline constexpr std::size_t kDefaultIoAlignment = 4 * 1024;
inline constexpr std::size_t kDefaultBackpressureHighBytes = 256 * 1024 * 1024;
inline constexpr std::size_t kDefaultBackpressureLowBytes = 128 * 1024 * 1024;
inline constexpr std::size_t kDefaultMaxGapBytes = 32 * 1024 * 1024;
inline constexpr std::size_t kDefaultFlushThresholdBytes = 16 * 1024 * 1024;
inline constexpr std::size_t kMapNodeOverheadBytes = 48;

} // namespace asyncdownload::core

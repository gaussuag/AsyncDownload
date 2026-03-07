#pragma once

#include <atomic>
#include <cstddef>

namespace asyncdownload::core {

class MemoryAccounting {
public:
    [[nodiscard]] std::size_t current_bytes() const noexcept;
    [[nodiscard]] std::size_t add(std::size_t bytes) noexcept;
    [[nodiscard]] std::size_t subtract(std::size_t bytes) noexcept;
    void reset() noexcept;

private:
    std::atomic<std::size_t> current_bytes_{0};
};

[[nodiscard]] std::size_t global_packet_overhead(std::size_t payload_size,
                                                 bool include_map_overhead) noexcept;

[[nodiscard]] bool should_pause_for_backpressure(std::size_t current_bytes,
                                                 std::size_t incoming_bytes,
                                                 std::size_t high_watermark) noexcept;

[[nodiscard]] MemoryAccounting& global_memory_accounting() noexcept;

} // namespace asyncdownload::core

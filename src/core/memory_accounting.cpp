#include "memory_accounting.hpp"

#include "constants.hpp"
#include "models.hpp"

namespace asyncdownload::core {

std::size_t MemoryAccounting::current_bytes() const noexcept {
    return current_bytes_.load(std::memory_order_relaxed);
}

std::size_t MemoryAccounting::add(const std::size_t bytes) noexcept {
    return current_bytes_.fetch_add(bytes, std::memory_order_relaxed) + bytes;
}

std::size_t MemoryAccounting::subtract(const std::size_t bytes) noexcept {
    return current_bytes_.fetch_sub(bytes, std::memory_order_relaxed) - bytes;
}

void MemoryAccounting::reset() noexcept {
    current_bytes_.store(0, std::memory_order_relaxed);
}

std::size_t global_packet_overhead(const std::size_t payload_size,
                                   const bool include_map_overhead) noexcept {
    return payload_size + sizeof(DataPacket) +
        (include_map_overhead ? kMapNodeOverheadBytes : 0);
}

bool should_pause_for_backpressure(const std::size_t current_bytes,
                                   const std::size_t incoming_bytes,
                                   const std::size_t high_watermark) noexcept {
    if (incoming_bytes == 0) {
        return false;
    }

    if (current_bytes == 0) {
        return false;
    }

    return current_bytes + incoming_bytes > high_watermark;
}

MemoryAccounting& global_memory_accounting() noexcept {
    static MemoryAccounting accounting;
    return accounting;
}

} // namespace asyncdownload::core


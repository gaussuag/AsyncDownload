#pragma once

#if defined(ASYNCDOWNLOAD_RECOVERY_FAULT_TEST)

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace asyncdownload::recovery::detail {

struct RecoveryFaultPlan {
    std::atomic<bool>
        fail_next_metadata_invalidate{false};
    std::atomic<bool>
        stop_after_metadata_invalidation{false};
    std::atomic<bool>
        stop_after_part_reset{false};
    std::atomic<std::uint64_t>
        pause_after_part_flush_generation{0};
    std::atomic<std::uint64_t>
        part_flush_completed_generation{0};
    std::atomic<bool>
        stop_after_part_flush{false};
    std::atomic<std::size_t>
        fail_crc_read_number{0};
    std::atomic<std::size_t>
        crc_read_count{0};

    void reset() noexcept {
        fail_next_metadata_invalidate.store(
            false,
            std::memory_order_release);
        stop_after_metadata_invalidation.store(
            false,
            std::memory_order_release);
        stop_after_part_reset.store(
            false,
            std::memory_order_release);
        pause_after_part_flush_generation.store(
            0,
            std::memory_order_release);
        part_flush_completed_generation.store(
            0,
            std::memory_order_release);
        stop_after_part_flush.store(
            false,
            std::memory_order_release);
        fail_crc_read_number.store(
            0,
            std::memory_order_release);
        crc_read_count.store(
            0,
            std::memory_order_release);
    }
};

[[nodiscard]] inline RecoveryFaultPlan&
recovery_fault_plan() noexcept {
    static RecoveryFaultPlan plan;
    return plan;
}

}

#endif

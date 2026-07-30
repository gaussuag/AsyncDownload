#pragma once

#if defined(ASYNCDOWNLOAD_RECOVERY_FAULT_TEST)

#include <atomic>

namespace asyncdownload::recovery::detail {

struct RecoveryFaultPlan {
    std::atomic<bool>
        fail_next_metadata_invalidate{false};
    std::atomic<bool>
        stop_after_metadata_invalidation{false};
    std::atomic<bool>
        stop_after_part_reset{false};

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
    }
};

[[nodiscard]] inline RecoveryFaultPlan&
recovery_fault_plan() noexcept {
    static RecoveryFaultPlan plan;
    return plan;
}

}

#endif

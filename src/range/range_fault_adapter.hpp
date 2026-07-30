#pragma once

#if defined(ASYNCDOWNLOAD_RANGE_LIFECYCLE_FAULT_TEST)

#include <atomic>
#include <new>

namespace asyncdownload::range::detail {

struct RangeFaultPlan {
    std::atomic<bool> fail_next_create_allocation{false};
    std::atomic<bool> fail_next_snapshot_allocation{false};
    std::atomic<bool> fail_next_geometry_submit_allocation{false};
    std::atomic<bool> fail_next_write_state_allocation{false};
    std::atomic<bool> fail_next_reorder_allocation{false};

    void reset() noexcept {
        fail_next_create_allocation.store(
            false,
            std::memory_order_release);
        fail_next_snapshot_allocation.store(
            false,
            std::memory_order_release);
        fail_next_geometry_submit_allocation.store(
            false,
            std::memory_order_release);
        fail_next_write_state_allocation.store(
            false,
            std::memory_order_release);
        fail_next_reorder_allocation.store(
            false,
            std::memory_order_release);
    }
};

inline RangeFaultPlan& range_fault_plan() noexcept {
    static RangeFaultPlan plan;
    return plan;
}

inline void fail_if_requested(
    std::atomic<bool>& point) {
    if (point.exchange(
            false,
            std::memory_order_acq_rel)) {
        throw std::bad_alloc();
    }
}

}

#endif

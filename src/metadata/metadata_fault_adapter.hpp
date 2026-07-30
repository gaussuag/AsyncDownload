#pragma once

#if defined(ASYNCDOWNLOAD_RECOVERY_FAULT_TEST)

#include <atomic>

namespace asyncdownload::metadata::detail {

struct MetadataFaultPlan {
    std::atomic<bool> fail_tmp_close{false};
    std::atomic<bool> stop_before_replace{false};
    std::atomic<bool> stop_after_replace{false};

    void reset() noexcept {
        fail_tmp_close.store(
            false,
            std::memory_order_release);
        stop_before_replace.store(
            false,
            std::memory_order_release);
        stop_after_replace.store(
            false,
            std::memory_order_release);
    }
};

[[nodiscard]] inline MetadataFaultPlan&
metadata_fault_plan() noexcept {
    static MetadataFaultPlan plan;
    return plan;
}

}

#endif

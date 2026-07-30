#pragma once

#if defined(ASYNCDOWNLOAD_RECOVERY_FAULT_TEST)

#include <atomic>

namespace asyncdownload::storage::detail {

struct FileWriterFaultPlan {
    std::atomic<bool> fail_before_output_replace{false};
    std::atomic<bool> stop_after_output_replace{false};

    void reset() noexcept {
        fail_before_output_replace.store(
            false,
            std::memory_order_release);
        stop_after_output_replace.store(
            false,
            std::memory_order_release);
    }
};

[[nodiscard]] inline FileWriterFaultPlan&
file_writer_fault_plan() noexcept {
    static FileWriterFaultPlan plan;
    return plan;
}

}

#endif

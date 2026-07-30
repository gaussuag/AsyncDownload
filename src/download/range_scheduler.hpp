#pragma once

#include "core/block_bitmap.hpp"
#include "download/download_policy.hpp"
#include "range/range_types.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

namespace asyncdownload::download {

struct RangeCandidate {
    range::RangeId id{};
    range::ByteSpan bytes{};
    range::ByteOffset dispatch_cursor = 0;
    range::RangePhase phase = range::RangePhase::ready;
};

struct StealPlan {
    range::RangeId donor{};
    range::ByteOffset split = 0;
};

struct RangePlanResult {
    std::vector<range::ByteSpan> ranges;
    std::error_code error;
};

class RangeScheduler {
public:
    RangeScheduler(SchedulingPolicy policy,
                   std::int64_t total_size) noexcept;

    [[nodiscard]] RangePlanResult
    plan_initial(const core::AtomicBlockBitmap& bitmap) const noexcept;

    [[nodiscard]] range::ByteSpan
    next_window(const RangeCandidate& candidate) const noexcept;

    [[nodiscard]] std::optional<StealPlan>
    choose_steal(std::span<const RangeCandidate> candidates) const noexcept;

private:
    // 先把 bitmap 上所有非 finished 的块拼成连续逻辑洞。
    [[nodiscard]] std::vector<std::pair<std::int64_t, std::int64_t>>
    build_unfinished_spans(const core::AtomicBlockBitmap& bitmap) const noexcept;

    // 再把这些大洞按 block 对齐切分成适合并发下载的初始区间。
    [[nodiscard]] std::vector<std::pair<std::int64_t, std::int64_t>>
    split_spans(const std::vector<std::pair<std::int64_t, std::int64_t>>& spans) const noexcept;

    SchedulingPolicy policy_{};
    std::int64_t total_size_ = 0;
};

} // namespace asyncdownload::download


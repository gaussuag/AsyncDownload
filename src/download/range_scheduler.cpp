#include "range_scheduler.hpp"

#include "asyncdownload/error.hpp"

#include <algorithm>
#include <cstdint>
#include <new>
#include <utility>

namespace asyncdownload::download {
namespace {

[[nodiscard]] std::int64_t align_down(
    const std::int64_t value,
    const std::int64_t alignment) noexcept {
    return value - (value % alignment);
}

[[nodiscard]] bool align_up_within(
    const std::int64_t value,
    const std::int64_t alignment,
    const std::int64_t limit,
    std::int64_t& result) noexcept {
    const auto remainder = value % alignment;
    const auto adjustment = remainder == 0 ? 0 : alignment - remainder;
    if (adjustment > limit - value) {
        return false;
    }

    result = value + adjustment;
    return true;
}

[[nodiscard]] bool has_two_units(
    const std::int64_t value,
    const std::int64_t unit) noexcept {
    return value / unit >= 2;
}

}

RangeScheduler::RangeScheduler(SchedulingPolicy policy,
                               const std::int64_t total_size) noexcept
    : policy_(std::move(policy)),
      total_size_(total_size) {}

RangePlanResult RangeScheduler::plan_initial(
    const core::AtomicBlockBitmap& bitmap) const noexcept {
    RangePlanResult result;
    try {
        if (!policy_.issue_range_requests) {
            if (total_size_ > 0) {
                result.ranges.push_back({0, total_size_});
            }
            return result;
        }
        const auto spans =
            split_spans(build_unfinished_spans(bitmap));
        result.ranges.reserve(spans.size());
        for (const auto& [begin, inclusive_end] : spans) {
            if (begin <= inclusive_end) {
                result.ranges.push_back({
                    begin,
                    inclusive_end + 1
                });
            }
        }
        return result;
    } catch (const std::bad_alloc&) {
        result.ranges.clear();
        result.error =
            std::make_error_code(std::errc::not_enough_memory);
        return result;
    } catch (...) {
        result.ranges.clear();
        result.error = make_error_code(DownloadErrc::internal_error);
        return result;
    }
}

range::ByteSpan RangeScheduler::next_window(
    const RangeCandidate& candidate) const noexcept {
    if (candidate.dispatch_cursor < candidate.bytes.begin ||
        candidate.dispatch_cursor >= candidate.bytes.end) {
        return {};
    }
    if (!policy_.issue_range_requests) {
        return {0, total_size_};
    }
    const auto remaining =
        candidate.bytes.end - candidate.dispatch_cursor;
    const auto extent =
        std::min(remaining, policy_.transfer_window_bytes);
    return {
        candidate.dispatch_cursor,
        candidate.dispatch_cursor + extent
    };
}

std::optional<StealPlan> RangeScheduler::choose_steal(
    const std::span<const RangeCandidate> candidates) const noexcept {
    if (!policy_.allow_work_stealing) {
        return std::nullopt;
    }

    const RangeCandidate* donor = nullptr;
    range::ByteOffset donor_remaining = 0;
    for (const auto& candidate : candidates) {
        if ((candidate.phase != range::RangePhase::ready &&
             candidate.phase != range::RangePhase::leased) ||
            candidate.dispatch_cursor < candidate.bytes.begin ||
            candidate.dispatch_cursor >= candidate.bytes.end) {
            continue;
        }
        const auto remaining =
            candidate.bytes.end - candidate.dispatch_cursor;
        if (remaining > donor_remaining) {
            donor = &candidate;
            donor_remaining = remaining;
        }
    }

    if (donor == nullptr ||
        !has_two_units(donor_remaining, policy_.block_bytes) ||
        (policy_.connection_limit >= 16 &&
         !has_two_units(
             donor_remaining,
             policy_.transfer_window_bytes))) {
        return std::nullopt;
    }

    const auto split = align_down(
        donor->dispatch_cursor + donor_remaining / 2,
        policy_.block_bytes);
    if (split <= donor->dispatch_cursor ||
        split >= donor->bytes.end) {
        return std::nullopt;
    }
    return StealPlan{donor->id, split};
}

std::vector<std::pair<std::int64_t, std::int64_t>>
RangeScheduler::build_unfinished_spans(const core::AtomicBlockBitmap& bitmap) const noexcept {
    std::vector<std::pair<std::int64_t, std::int64_t>> spans;
    const auto block_size = policy_.block_bytes;

    std::int64_t current_start = -1;
    for (std::size_t block = 0; block < bitmap.block_count(); ++block) {
        const auto state = bitmap.load(block);
        if (state == core::BlockState::finished) {
            if (current_start >= 0) {
                // 一旦碰到 finished block，就意味着前面的 unfinished span 已经闭合，
                // 可以作为一个待调度区间输出。
                const auto start = current_start;
                const auto end = std::min(static_cast<std::int64_t>(block) * block_size,
                    total_size_) - 1;
                spans.emplace_back(start, end);
                current_start = -1;
            }
            continue;
        }

        if (current_start < 0) {
            // unfinished span 总是从第一个非 finished block 的起点开始。
            current_start = static_cast<std::int64_t>(block) * block_size;
        }
    }

    if (current_start >= 0) {
        spans.emplace_back(current_start, total_size_ - 1);
    }

    return spans;
}

std::vector<std::pair<std::int64_t, std::int64_t>>
RangeScheduler::split_spans(
    const std::vector<std::pair<std::int64_t, std::int64_t>>& spans) const noexcept {
    if (!policy_.issue_range_requests ||
        policy_.connection_limit <= 1 ||
        spans.empty()) {
        return spans;
    }

    std::vector<std::pair<std::int64_t, std::int64_t>> result = spans;

    // 初始切分尽量把最大洞不断对半拆开，直到足够喂满可用连接数。
    // 后续运行期如果仍有负载不均，再通过纯提案做动态修正。
    while (result.size() < policy_.connection_limit) {
        auto largest_it = std::max_element(result.begin(), result.end(),
            [](const auto& lhs, const auto& rhs) {
                return (lhs.second - lhs.first) < (rhs.second - rhs.first);
            });
        if (largest_it == result.end()) {
            break;
        }

        const auto start = largest_it->first;
        const auto end = largest_it->second;
        const auto remaining = end - start + 1;
        if (!has_two_units(remaining, policy_.block_bytes)) {
            break;
        }

        // 初始切分使用 align_up，让右半段总是从块边界开始，便于后续把它直接作为
        // 一个独立 range 派发出去。
        std::int64_t midpoint = 0;
        if (!align_up_within(
                start + (remaining / 2),
                policy_.block_bytes,
                end,
                midpoint) ||
            midpoint <= start) {
            break;
        }

        largest_it->second = midpoint - 1;
        result.emplace_back(midpoint, end);
    }

    std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.first < rhs.first;
    });
    return result;
}

} // namespace asyncdownload::download


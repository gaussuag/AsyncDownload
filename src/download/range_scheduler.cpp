#include "range_scheduler.hpp"

#include "core/alignment.hpp"

#include <algorithm>

namespace asyncdownload::download {

RangeScheduler::RangeScheduler(const asyncdownload::DownloadOptions& options,
                               const std::int64_t total_size,
                               const bool accept_ranges) noexcept
    : options_(options),
      total_size_(total_size),
      accept_ranges_(accept_ranges) {}

std::vector<std::unique_ptr<core::RangeContext>>
RangeScheduler::build_initial_ranges(const core::AtomicBlockBitmap& bitmap) noexcept {
    std::vector<std::unique_ptr<core::RangeContext>> ranges;
    // 调度器看到的输入不是“整个文件”，而是“当前 bitmap 上还没 finished 的洞”。
    // 对新任务来说这些洞覆盖整文件；对恢复任务来说只覆盖未完成区域。
    const auto spans = split_spans(build_unfinished_spans(bitmap));

    for (const auto& [start, end] : spans) {
        if (start > end) {
            continue;
        }

        ranges.push_back(std::make_unique<core::RangeContext>(next_range_id_++, start, end));
    }

    return ranges;
}

std::unique_ptr<core::RangeContext>
RangeScheduler::steal_largest_range(
    const std::vector<std::unique_ptr<core::RangeContext>>& ranges) noexcept {
    if (!accept_ranges_) {
        // 非 Range 服务端无法安全把一段下载任务拆给第二个请求，所以直接禁用 steal。
        return nullptr;
    }

    core::RangeContext* donor = nullptr;
    std::int64_t donor_remaining = 0;

    // 这里偷的不是“当前正在飞行的 HTTP 响应体”，而是 donor 还没派发出去的尾部区间。
    // 这样可以避免在一个正在进行中的请求中途硬缩短 range 带来的复杂竞态。
    for (const auto& range_ptr : ranges) {
        auto* candidate = range_ptr.get();
        if (candidate->marked_finished.load(std::memory_order_acquire)) {
            continue;
        }

        const auto current = candidate->current_offset.load(std::memory_order_acquire);
        const auto end = candidate->end_offset.load(std::memory_order_acquire);
        if (current >= end) {
            continue;
        }

        const auto remaining = end - current + 1;
        if (remaining > donor_remaining) {
            donor = candidate;
            donor_remaining = remaining;
        }
    }

    if (donor == nullptr || donor_remaining <
        static_cast<std::int64_t>(options_.block_size * 2)) {
        return nullptr;
    }

    const auto current = donor->current_offset.load(std::memory_order_acquire);
    const auto old_end = donor->end_offset.load(std::memory_order_acquire);
    // midpoint 必须按 block_size 对齐，这样新旧 range 的分界线就和位图颗粒度一致，
    // 后续 finished 判定和恢复逻辑都会更稳定。
    const auto midpoint = core::align_down(current + (donor_remaining / 2), options_.block_size);
    if (midpoint <= current || midpoint > old_end) {
        return nullptr;
    }

    donor->end_offset.store(midpoint - 1, std::memory_order_release);
    return std::make_unique<core::RangeContext>(next_range_id_++, midpoint, old_end);
}

std::pair<std::int64_t, std::int64_t>
RangeScheduler::next_window(const core::RangeContext& range) const noexcept {
    const auto start = range.current_offset.load(std::memory_order_acquire);
    const auto end = range.end_offset.load(std::memory_order_acquire);
    if (start > end) {
        return {0, -1};
    }

    if (!accept_ranges_) {
        // 不能做 Range 时，一个请求必须覆盖整文件；这里返回完整区间，
        // 让上层走最保守的单请求路径。
        return {0, total_size_ - 1};
    }

    // window 是“单次 HTTP 请求的租约大小”，不是整个逻辑 range 的大小。
    // 这样同一个 range 可以被拆成多个顺序请求，中间还能给 work stealing 留空间。
    const auto window = static_cast<std::int64_t>(options_.scheduler_window_bytes);
    return {start, std::min(end, start + window - 1)};
}

std::vector<std::pair<std::int64_t, std::int64_t>>
RangeScheduler::build_unfinished_spans(const core::AtomicBlockBitmap& bitmap) const noexcept {
    std::vector<std::pair<std::int64_t, std::int64_t>> spans;
    const auto block_size = static_cast<std::int64_t>(options_.block_size);

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

    if (spans.empty() && total_size_ > 0) {
        // bitmap 为空洞的极端情况一般意味着全新任务或恢复信息缺失；
        // 这里保守退回整文件 span，避免调度器什么也不派发。
        spans.emplace_back(0, total_size_ - 1);
    }

    return spans;
}

std::vector<std::pair<std::int64_t, std::int64_t>>
RangeScheduler::split_spans(
    const std::vector<std::pair<std::int64_t, std::int64_t>>& spans) const noexcept {
    if (!accept_ranges_ || options_.max_connections <= 1 || spans.empty()) {
        return spans;
    }

    std::vector<std::pair<std::int64_t, std::int64_t>> result = spans;

    // 初始切分尽量把最大洞不断对半拆开，直到足够喂满可用连接数。
    // 后续运行期如果仍有负载不均，再交给 steal_largest_range 做动态修正。
    while (result.size() < options_.max_connections) {
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
        if (remaining < static_cast<std::int64_t>(options_.block_size * 2)) {
            break;
        }

        // 初始切分使用 align_up，让右半段总是从块边界开始，便于后续把它直接作为
        // 一个独立 RangeContext 派发出去。
        const auto midpoint = core::align_up(start + (remaining / 2), options_.block_size);
        if (midpoint <= start || midpoint > end) {
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


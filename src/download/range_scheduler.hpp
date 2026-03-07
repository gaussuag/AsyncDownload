#pragma once

#include "core/block_bitmap.hpp"
#include "core/models.hpp"

#include <memory>
#include <vector>

namespace asyncdownload::download {

class RangeScheduler {
public:
    // total_size 和 accept_ranges 在一次任务内固定不变；options 决定切分粒度和并发策略。
    RangeScheduler(const asyncdownload::DownloadOptions& options,
                   std::int64_t total_size,
                   bool accept_ranges) noexcept;

    // 根据当前 bitmap 生成初始 RangeContext 列表，只覆盖未完成区域。
    [[nodiscard]] std::vector<std::unique_ptr<core::RangeContext>>
    build_initial_ranges(const core::AtomicBlockBitmap& bitmap) noexcept;

    // 从现有 range 集合里找出最大未派发尾部并切出一个新 range。
    // 这是运行期负载均衡的核心入口。
    [[nodiscard]] std::unique_ptr<core::RangeContext>
    steal_largest_range(const std::vector<std::unique_ptr<core::RangeContext>>& ranges) noexcept;

    // 为某个逻辑 range 生成下一次 HTTP 请求应该覆盖的 window。
    [[nodiscard]] std::pair<std::int64_t, std::int64_t>
    next_window(const core::RangeContext& range) const noexcept;

private:
    // 先把 bitmap 上所有非 finished 的块拼成连续逻辑洞。
    [[nodiscard]] std::vector<std::pair<std::int64_t, std::int64_t>>
    build_unfinished_spans(const core::AtomicBlockBitmap& bitmap) const noexcept;

    // 再把这些大洞按 block 对齐切分成适合并发下载的初始区间。
    [[nodiscard]] std::vector<std::pair<std::int64_t, std::int64_t>>
    split_spans(const std::vector<std::pair<std::int64_t, std::int64_t>>& spans) const noexcept;

    asyncdownload::DownloadOptions options_;
    std::int64_t total_size_ = 0;
    bool accept_ranges_ = false;
    // range_id 由调度器统一分配，保证后续 metadata 和控制消息都能稳定索引。
    std::size_t next_range_id_ = 0;
};

} // namespace asyncdownload::download


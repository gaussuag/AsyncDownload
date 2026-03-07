#include "download/range_scheduler.hpp"
#include "core/block_bitmap.hpp"

#include <gtest/gtest.h>

TEST(RangeSchedulerTest, BuildsAlignedInitialRanges) {
    asyncdownload::DownloadOptions options{};
    options.max_connections = 4;
    options.block_size = 64 * 1024;

    asyncdownload::core::AtomicBlockBitmap bitmap(4);
    bitmap.store(1, asyncdownload::core::BlockState::finished);

    asyncdownload::download::RangeScheduler scheduler(options, 4LL * 64 * 1024, true);
    const auto ranges = scheduler.build_initial_ranges(bitmap);

    ASSERT_EQ(ranges.size(), 3);
    EXPECT_EQ(ranges[0]->start_offset, 0);
    EXPECT_EQ(ranges[0]->end_offset.load(), 64 * 1024 - 1);
    EXPECT_EQ(ranges[1]->start_offset % static_cast<std::int64_t>(options.block_size), 0);
    EXPECT_EQ(ranges[2]->start_offset % static_cast<std::int64_t>(options.block_size), 0);
}

TEST(RangeSchedulerTest, StealsLargestUndispatchedTail) {
    asyncdownload::DownloadOptions options{};
    options.max_connections = 4;
    options.block_size = 64 * 1024;

    asyncdownload::download::RangeScheduler scheduler(options, 8LL * 64 * 1024, true);
    std::vector<std::unique_ptr<asyncdownload::core::RangeContext>> ranges;
    ranges.push_back(std::make_unique<asyncdownload::core::RangeContext>(0, 0, 8LL * 64 * 1024 - 1));
    ranges.front()->current_offset.store(2LL * 64 * 1024, std::memory_order_release);

    auto stolen = scheduler.steal_largest_range(ranges);

    ASSERT_NE(stolen, nullptr);
    EXPECT_EQ(stolen->start_offset % static_cast<std::int64_t>(options.block_size), 0);
    EXPECT_GT(stolen->start_offset, ranges.front()->current_offset.load(std::memory_order_acquire));
    EXPECT_LT(ranges.front()->end_offset.load(std::memory_order_acquire), stolen->end_offset.load(std::memory_order_acquire));
}

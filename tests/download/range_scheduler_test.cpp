#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "core/block_bitmap.hpp"
#include "core/models.hpp"
#include "download/download_policy.hpp"
#include "download/range_scheduler.hpp"

namespace {

asyncdownload::download::SchedulingPolicy range_policy() {
    return {
        4,
        4 * 1024 * 1024,
        64 * 1024,
        true,
        true
    };
}

}

TEST(RangeSchedulerTest, BuildsAlignedInitialRanges) {
    const auto policy = range_policy();
    asyncdownload::core::AtomicBlockBitmap bitmap(4);
    bitmap.store(1, asyncdownload::core::BlockState::finished);

    asyncdownload::download::RangeScheduler scheduler(
        policy,
        4LL * policy.block_bytes);
    const auto ranges = scheduler.build_initial_ranges(bitmap);

    ASSERT_EQ(ranges.size(), 3);
    EXPECT_EQ(ranges[0]->start_offset, 0);
    EXPECT_EQ(ranges[0]->end_offset.load(), policy.block_bytes - 1);
    EXPECT_EQ(ranges[1]->start_offset % policy.block_bytes, 0);
    EXPECT_EQ(ranges[2]->start_offset % policy.block_bytes, 0);
}

TEST(RangeSchedulerTest, StealsLargestUndispatchedTail) {
    const auto policy = range_policy();
    asyncdownload::download::RangeScheduler scheduler(
        policy,
        8LL * policy.block_bytes);
    std::vector<std::unique_ptr<asyncdownload::core::RangeContext>> ranges;
    ranges.push_back(std::make_unique<asyncdownload::core::RangeContext>(
        0,
        0,
        8LL * policy.block_bytes - 1));
    ranges.front()->current_offset.store(
        2LL * policy.block_bytes,
        std::memory_order_release);

    auto stolen = scheduler.steal_largest_range(ranges);

    ASSERT_NE(stolen, nullptr);
    EXPECT_EQ(stolen->start_offset % policy.block_bytes, 0);
    EXPECT_GT(
        stolen->start_offset,
        ranges.front()->current_offset.load(std::memory_order_acquire));
    EXPECT_LT(
        ranges.front()->end_offset.load(std::memory_order_acquire),
        stolen->end_offset.load(std::memory_order_acquire));
}

TEST(RangeSchedulerTest, UsesEffectiveWindowWithoutOverflowAtLargeOffsets) {
    auto policy = range_policy();
    policy.transfer_window_bytes = 64;
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    asyncdownload::download::RangeScheduler scheduler(policy, maximum);
    const asyncdownload::core::RangeContext range(
        0,
        maximum - 10,
        maximum - 1);

    const auto window = scheduler.next_window(range);

    EXPECT_EQ(window.first, maximum - 10);
    EXPECT_EQ(window.second, maximum - 1);
}

TEST(RangeSchedulerTest, DoesNotOverflowWhenBlockIsLargerThanHalfRemaining) {
    auto policy = range_policy();
    policy.block_bytes = std::int64_t{1} << 62;
    asyncdownload::download::RangeScheduler scheduler(
        policy,
        policy.block_bytes + 100);
    std::vector<std::unique_ptr<asyncdownload::core::RangeContext>> ranges;
    ranges.push_back(std::make_unique<asyncdownload::core::RangeContext>(
        0,
        0,
        policy.block_bytes + 99));

    const auto stolen = scheduler.steal_largest_range(ranges);

    EXPECT_EQ(stolen, nullptr);
}

TEST(RangeSchedulerTest, DoesNotStealWhenEffectivePolicyDisablesStealing) {
    auto policy = range_policy();
    policy.allow_work_stealing = false;
    asyncdownload::download::RangeScheduler scheduler(
        policy,
        8LL * policy.block_bytes);
    std::vector<std::unique_ptr<asyncdownload::core::RangeContext>> ranges;
    ranges.push_back(std::make_unique<asyncdownload::core::RangeContext>(
        0,
        0,
        8LL * policy.block_bytes - 1));

    const auto stolen = scheduler.steal_largest_range(ranges);

    EXPECT_EQ(stolen, nullptr);
}

TEST(RangeSchedulerTest, NonRangePolicyReturnsSingleFullWindow) {
    auto policy = range_policy();
    policy.connection_limit = 1;
    policy.transfer_window_bytes = 12345;
    policy.issue_range_requests = false;
    policy.allow_work_stealing = false;
    asyncdownload::download::RangeScheduler scheduler(policy, 12345);
    const asyncdownload::core::RangeContext range(0, 4000, 8000);

    const auto window = scheduler.next_window(range);

    EXPECT_EQ(window.first, 0);
    EXPECT_EQ(window.second, 12344);
}

TEST(RangeSchedulerTest, FinalShortWindowEndsAtObjectBoundary) {
    auto policy = range_policy();
    policy.transfer_window_bytes = 4096;
    asyncdownload::download::RangeScheduler scheduler(policy, 10000);
    const asyncdownload::core::RangeContext range(0, 9000, 9999);

    const auto window = scheduler.next_window(range);

    EXPECT_EQ(window.first, 9000);
    EXPECT_EQ(window.second, 9999);
}

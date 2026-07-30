#include <array>
#include <cstdint>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

#include "core/block_bitmap.hpp"
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

TEST(RangeSchedulerProposalTest, PlansBitmapHolesAsHalfOpenSpans) {
    const auto policy = range_policy();
    asyncdownload::core::AtomicBlockBitmap bitmap(4);
    bitmap.store(1, asyncdownload::core::BlockState::finished);
    asyncdownload::download::RangeScheduler scheduler(
        policy, 4LL * policy.block_bytes);

    const auto plan = scheduler.plan_initial(bitmap);

    ASSERT_FALSE(plan.error);
    ASSERT_EQ(plan.ranges.size(), 3U);
    EXPECT_EQ(
        plan.ranges[0],
        (asyncdownload::range::ByteSpan{0, policy.block_bytes}));
    EXPECT_EQ(
        plan.ranges[1],
        (asyncdownload::range::ByteSpan{
            2 * policy.block_bytes,
            3 * policy.block_bytes
        }));
    EXPECT_EQ(
        plan.ranges[2],
        (asyncdownload::range::ByteSpan{
            3 * policy.block_bytes,
            4 * policy.block_bytes
        }));
}

TEST(RangeSchedulerProposalTest, PlansShortObjectExactly) {
    const auto policy = range_policy();
    asyncdownload::core::AtomicBlockBitmap bitmap(1);
    asyncdownload::download::RangeScheduler scheduler(policy, 123);

    const auto plan = scheduler.plan_initial(bitmap);

    ASSERT_FALSE(plan.error);
    ASSERT_EQ(plan.ranges.size(), 1U);
    EXPECT_EQ(
        plan.ranges.front(),
        (asyncdownload::range::ByteSpan{0, 123}));
}

TEST(RangeSchedulerProposalTest, UsesExactHalfOpenFinalWindow) {
    auto policy = range_policy();
    policy.transfer_window_bytes = 4096;
    asyncdownload::download::RangeScheduler scheduler(policy, 10000);
    const asyncdownload::download::RangeCandidate candidate{
        {4},
        {0, 10000},
        9000,
        asyncdownload::range::RangePhase::ready
    };

    const auto window = scheduler.next_window(candidate);

    EXPECT_EQ(
        window,
        (asyncdownload::range::ByteSpan{9000, 10000}));
}

TEST(RangeSchedulerProposalTest, ChoosesLargestTailWithoutMutation) {
    const auto policy = range_policy();
    asyncdownload::download::RangeScheduler scheduler(
        policy, 16LL * policy.block_bytes);
    const std::array<asyncdownload::download::RangeCandidate, 2>
        candidates{{
            {
                {5},
                {0, 4LL * policy.block_bytes},
                0,
                asyncdownload::range::RangePhase::ready
            },
            {
                {7},
                {
                    4LL * policy.block_bytes,
                    16LL * policy.block_bytes
                },
                6LL * policy.block_bytes,
                asyncdownload::range::RangePhase::leased
            }
        }};

    const auto plan = scheduler.choose_steal(candidates);

    ASSERT_TRUE(plan.has_value());
    EXPECT_EQ(plan->donor, (asyncdownload::range::RangeId{7}));
    EXPECT_GT(plan->split, candidates[1].dispatch_cursor);
    EXPECT_EQ(
        candidates[1].bytes.end,
        16LL * policy.block_bytes);
}

TEST(RangeSchedulerProposalTest, ChangesThresholdAtConnectionSixteen) {
    auto fifteen_policy = range_policy();
    fifteen_policy.connection_limit = 15;
    fifteen_policy.transfer_window_bytes =
        4 * fifteen_policy.block_bytes;
    auto sixteen_policy = fifteen_policy;
    sixteen_policy.connection_limit = 16;
    const asyncdownload::download::RangeCandidate candidate{
        {1},
        {0, 4LL * fifteen_policy.block_bytes},
        0,
        asyncdownload::range::RangePhase::ready
    };
    asyncdownload::download::RangeScheduler fifteen(
        fifteen_policy, candidate.bytes.end);
    asyncdownload::download::RangeScheduler sixteen(
        sixteen_policy, candidate.bytes.end);

    EXPECT_TRUE(fifteen.choose_steal(
        std::span<const asyncdownload::download::RangeCandidate>(
            &candidate, 1)).has_value());
    EXPECT_FALSE(sixteen.choose_steal(
        std::span<const asyncdownload::download::RangeCandidate>(
            &candidate, 1)).has_value());
}

TEST(RangeSchedulerProposalTest, BreaksEqualTailTieByInputOrder) {
    const auto policy = range_policy();
    const auto extent = 4LL * policy.block_bytes;
    asyncdownload::download::RangeScheduler scheduler(
        policy, 2 * extent);
    const std::array<asyncdownload::download::RangeCandidate, 2>
        candidates{{
            {
                {9},
                {0, extent},
                0,
                asyncdownload::range::RangePhase::ready
            },
            {
                {3},
                {extent, 2 * extent},
                extent,
                asyncdownload::range::RangePhase::ready
            }
        }};

    const auto plan = scheduler.choose_steal(candidates);

    ASSERT_TRUE(plan.has_value());
    EXPECT_EQ(plan->donor, (asyncdownload::range::RangeId{9}));
}

TEST(RangeSchedulerProposalTest, RejectsSplitAtDispatchedFrontier) {
    auto policy = range_policy();
    policy.block_bytes = 8;
    asyncdownload::download::RangeScheduler scheduler(policy, 24);
    const asyncdownload::download::RangeCandidate candidate{
        {1},
        {0, 24},
        15,
        asyncdownload::range::RangePhase::leased
    };

    const auto plan = scheduler.choose_steal(
        std::span<const asyncdownload::download::RangeCandidate>(
            &candidate, 1));

    EXPECT_FALSE(plan.has_value());
}

TEST(RangeSchedulerProposalTest, NonRangePlansOneFullObject) {
    auto policy = range_policy();
    policy.connection_limit = 1;
    policy.issue_range_requests = false;
    policy.allow_work_stealing = false;
    const auto total_size = 4LL * policy.block_bytes;
    asyncdownload::core::AtomicBlockBitmap bitmap(4);
    bitmap.store(1, asyncdownload::core::BlockState::finished);
    asyncdownload::download::RangeScheduler scheduler(
        policy, total_size);

    const auto plan = scheduler.plan_initial(bitmap);

    ASSERT_FALSE(plan.error);
    ASSERT_EQ(plan.ranges.size(), 1U);
    EXPECT_EQ(
        plan.ranges.front(),
        (asyncdownload::range::ByteSpan{0, total_size}));
    EXPECT_FALSE(scheduler.choose_steal({}).has_value());
}

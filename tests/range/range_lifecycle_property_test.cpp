#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "core/block_bitmap.hpp"
#include "download/range_scheduler.hpp"
#include "range/range_lifecycle.hpp"

namespace {

constexpr std::uint32_t PROPERTY_SEED = 0xA51D03U;

asyncdownload::download::SchedulingPolicy property_policy(
    const std::size_t connections,
    const std::int64_t window) {
    return {
        connections,
        window,
        64,
        true,
        connections > 1
    };
}

}

TEST(
    RangeLifecyclePropertyTest,
    InitialPlanExactlyCoversRandomUnfinishedBytes) {
    std::mt19937 generator(PROPERTY_SEED);
    SCOPED_TRACE(
        ::testing::Message() << "seed=" << PROPERTY_SEED);

    for (std::size_t trial = 0; trial < 256; ++trial) {
        const auto block_count =
            std::size_t{1} + generator() % 64;
        const auto final_extent =
            std::int64_t{1} + generator() % 64;
        const auto total_size =
            static_cast<std::int64_t>(
                (block_count - 1) * 64) +
            final_extent;
        asyncdownload::core::AtomicBlockBitmap bitmap(
            block_count);
        std::vector<bool> expected(
            static_cast<std::size_t>(total_size),
            false);
        for (std::size_t block = 0;
             block < block_count;
             ++block) {
            const auto finished =
                (generator() % 3) == 0 ||
                trial == 0;
            if (finished) {
                bitmap.store(
                    block,
                    asyncdownload::core::BlockState::finished);
                continue;
            }
            const auto begin = block * 64;
            const auto end = std::min<std::size_t>(
                begin + 64,
                expected.size());
            std::fill(
                expected.begin() +
                    static_cast<std::ptrdiff_t>(begin),
                expected.begin() +
                    static_cast<std::ptrdiff_t>(end),
                true);
        }
        asyncdownload::download::RangeScheduler scheduler(
            property_policy(
                std::size_t{1} + generator() % 16,
                64 * (std::int64_t{1} + generator() % 8)),
            total_size);

        const auto planned = scheduler.plan_initial(bitmap);

        ASSERT_FALSE(planned.error) << "trial=" << trial;
        std::vector<bool> actual(expected.size(), false);
        for (const auto span : planned.ranges) {
            ASSERT_GE(span.begin, 0) << "trial=" << trial;
            ASSERT_LT(span.begin, span.end)
                << "trial=" << trial;
            ASSERT_LE(span.end, total_size)
                << "trial=" << trial;
            for (auto offset = span.begin;
                 offset < span.end;
                 ++offset) {
                const auto index =
                    static_cast<std::size_t>(offset);
                EXPECT_FALSE(actual[index])
                    << "trial=" << trial;
                actual[index] = true;
            }
        }
        EXPECT_EQ(actual, expected) << "trial=" << trial;
    }
}

TEST(
    RangeLifecyclePropertyTest,
    RepeatedStealsPreserveUnionAlignmentAndDisjointness) {
    std::mt19937 generator(PROPERTY_SEED);
    SCOPED_TRACE(
        ::testing::Message() << "seed=" << PROPERTY_SEED);

    for (std::size_t trial = 0; trial < 256; ++trial) {
        const auto block_count =
            std::int64_t{8} + generator() % 57;
        const auto total_size = block_count * 64;
        const auto connections =
            std::size_t{2} + generator() % 15;
        asyncdownload::download::RangeScheduler scheduler(
            property_policy(
                connections,
                64 * (std::int64_t{1} + generator() % 4)),
            total_size);
        std::vector<asyncdownload::download::RangeCandidate>
            candidates{{
                {0},
                {0, total_size},
                0,
                asyncdownload::range::RangePhase::ready
            }};
        std::uint64_t next_id = 1;

        while (candidates.size() < connections) {
            const auto proposal =
                scheduler.choose_steal(candidates);
            if (!proposal.has_value()) {
                break;
            }
            const auto donor = std::find_if(
                candidates.begin(),
                candidates.end(),
                [&proposal](const auto& candidate) {
                    return candidate.id == proposal->donor;
                });
            ASSERT_NE(donor, candidates.end())
                << "trial=" << trial;
            ASSERT_GT(proposal->split, donor->dispatch_cursor)
                << "trial=" << trial;
            ASSERT_EQ(proposal->split % 64, 0)
                << "trial=" << trial;
            const auto old_end = donor->bytes.end;
            donor->bytes.end = proposal->split;
            candidates.push_back({
                {next_id++},
                {proposal->split, old_end},
                proposal->split,
                asyncdownload::range::RangePhase::ready
            });
        }

        std::sort(
            candidates.begin(),
            candidates.end(),
            [](const auto& left, const auto& right) {
                return left.bytes.begin < right.bytes.begin;
            });
        std::int64_t cursor = 0;
        std::int64_t total = 0;
        for (const auto& candidate : candidates) {
            EXPECT_EQ(candidate.bytes.begin, cursor)
                << "trial=" << trial;
            EXPECT_LT(
                candidate.bytes.begin,
                candidate.bytes.end)
                << "trial=" << trial;
            if (candidate.bytes.end != total_size) {
                EXPECT_EQ(candidate.bytes.end % 64, 0)
                    << "trial=" << trial;
            }
            cursor = candidate.bytes.end;
            total += candidate.bytes.end -
                candidate.bytes.begin;
        }
        EXPECT_EQ(cursor, total_size) << "trial=" << trial;
        EXPECT_EQ(total, total_size) << "trial=" << trial;
    }
}

TEST(
    RangeLifecyclePropertyTest,
    LeaseAndPersistenceEventsPreserveCoverageAndTerminalCount) {
    std::mt19937 generator(PROPERTY_SEED);
    SCOPED_TRACE(
        ::testing::Message() << "seed=" << PROPERTY_SEED);

    for (std::size_t trial = 0; trial < 256; ++trial) {
        const auto total_size =
            std::int64_t{1} + generator() % 2048;
        const auto window =
            std::int64_t{1} + generator() % 256;
        const std::array<asyncdownload::range::ByteSpan, 1>
            initial{{{0, total_size}}};
        auto creation =
            asyncdownload::range::RangeLifecycle::create(
                total_size,
                property_policy(1, window),
                initial);
        ASSERT_FALSE(creation.error) << "trial=" << trial;
        ASSERT_NE(creation.value, nullptr)
            << "trial=" << trial;
        auto lifecycle = std::move(creation.value);
        std::int64_t cursor = 0;
        std::uint64_t previous_generation = 0;
        std::optional<asyncdownload::range::LeaseSucceeded>
            previous_success;
        asyncdownload::range::CompletionId completion{};

        while (cursor < total_size) {
            const auto acquired = lifecycle->acquire();
            ASSERT_FALSE(acquired.error)
                << "trial=" << trial;
            ASSERT_TRUE(acquired.lease.has_value())
                << "trial=" << trial;
            const auto lease = *acquired.lease;
            EXPECT_EQ(lease.bytes.begin, cursor)
                << "trial=" << trial;
            EXPECT_GT(
                lease.id.generation,
                previous_generation)
                << "trial=" << trial;
            if (previous_success.has_value()) {
                const auto before = lifecycle->snapshot();
                const auto stale =
                    lifecycle->apply(*previous_success);
                const auto after = lifecycle->snapshot();
                ASSERT_FALSE(before.error);
                ASSERT_FALSE(after.error);
                EXPECT_TRUE(
                    stale.disposition ==
                        asyncdownload::range::
                            EventDisposition::stale ||
                    stale.disposition ==
                        asyncdownload::range::
                            EventDisposition::duplicate);
                EXPECT_EQ(
                    before.value.ranges[0].dispatch_cursor,
                    after.value.ranges[0].dispatch_cursor);
                EXPECT_EQ(
                    before.value.ranges[0].persisted_through,
                    after.value.ranges[0].persisted_through);
            }
            const asyncdownload::range::LeaseSucceeded
                succeeded{lease.id, lease.bytes.end};
            const auto applied =
                lifecycle->apply(succeeded);
            ASSERT_FALSE(applied.error)
                << "trial=" << trial;
            EXPECT_EQ(
                lifecycle->apply(succeeded).disposition,
                asyncdownload::range::
                    EventDisposition::duplicate);
            ASSERT_FALSE(lifecycle->apply(
                asyncdownload::range::PersistedThrough{
                    lease.id.range,
                    lease.bytes.end
                }).error);
            if (lease.bytes.begin > 0) {
                EXPECT_EQ(
                    lifecycle->apply(
                        asyncdownload::range::
                            PersistedThrough{
                                lease.id.range,
                                lease.bytes.begin
                            }).disposition,
                    asyncdownload::range::
                        EventDisposition::stale);
            }
            const auto snapshot = lifecycle->snapshot();
            ASSERT_FALSE(snapshot.error);
            EXPECT_EQ(
                snapshot.value.ranges[0].persisted_through,
                lease.bytes.end);
            EXPECT_FALSE(snapshot.value.all_finished);
            EXPECT_EQ(snapshot.value.finished_ranges, 0U);
            if (applied.effects.size == 1) {
                completion = std::get<
                    asyncdownload::range::
                        PublishRangeCompleteEffect>(
                            applied.effects.values[0])
                                 .completion;
            }
            cursor = lease.bytes.end;
            previous_generation = lease.id.generation;
            previous_success = succeeded;
        }

        const auto committed = lifecycle->apply(
            asyncdownload::range::PersistenceCommitted{
                completion,
                total_size
            });
        ASSERT_FALSE(committed.error)
            << "trial=" << trial;
        EXPECT_EQ(
            lifecycle->apply(
                asyncdownload::range::PersistenceCommitted{
                    completion,
                    total_size
                }).disposition,
            asyncdownload::range::
                EventDisposition::duplicate);
        const auto final_snapshot = lifecycle->snapshot();
        ASSERT_FALSE(final_snapshot.error);
        EXPECT_TRUE(final_snapshot.value.all_finished);
        EXPECT_EQ(final_snapshot.value.finished_ranges, 1U);
    }
}

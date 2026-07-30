#include <atomic>
#include <cstdint>
#include <optional>
#include <thread>

#include <gtest/gtest.h>

#include "range/range_fact_slot.hpp"

TEST(RangeFactSlotTest, CoalescesPersistedAndGapFacts) {
    asyncdownload::range::RangeFactSlot slot({3}, 64);
    auto publisher = slot.publisher();

    publisher.publish_persisted_through(128);
    publisher.publish_persisted_through(96);
    publisher.publish_gap_pause(true);
    publisher.publish_gap_pause(false);

    const auto snapshot = slot.read_since(0);
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->persisted_through, 128);
    EXPECT_FALSE(snapshot->gap_paused);
    EXPECT_EQ(snapshot->committed_generation, 0U);
    EXPECT_EQ(snapshot->revision, 4U);
    EXPECT_FALSE(slot.read_since(snapshot->revision).has_value());
}

TEST(RangeFactSlotTest, RejectsCompletionForAnotherRange) {
    asyncdownload::range::RangeFactSlot slot({3}, 64);
    auto publisher = slot.publisher();

    const auto error = publisher.publish_committed(
        {{4}, 1},
        128);

    EXPECT_TRUE(error);
    EXPECT_FALSE(slot.read_since(0).has_value());
}

TEST(RangeFactSlotTest, KeepsCompletionSticky) {
    asyncdownload::range::RangeFactSlot slot({3}, 64);
    auto publisher = slot.publisher();
    ASSERT_FALSE(publisher.publish_committed(
        {{3}, 2},
        256));

    const auto conflicting = publisher.publish_committed(
        {{3}, 3},
        320);
    const auto snapshot = slot.read_since(0);

    EXPECT_TRUE(conflicting);
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->persisted_through, 256);
    EXPECT_EQ(snapshot->committed_generation, 2U);
}

TEST(
    RangeFactSlotTest,
    AcquireRevisionObservesCompletionFrontier) {
    asyncdownload::range::RangeFactSlot slot({9}, 0);
    auto publisher = slot.publisher();
    std::atomic<bool> start{false};
    std::atomic<bool> publish_failed{false};
    std::thread writer(
        [&publisher, &start, &publish_failed]() {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            const auto error = publisher.publish_committed(
                {{9}, 7},
                4096);
            publish_failed.store(
                static_cast<bool>(error),
                std::memory_order_release);
        });

    start.store(true, std::memory_order_release);
    std::optional<asyncdownload::range::RangeFactSnapshot>
        observed;
    while (!observed.has_value()) {
        observed = slot.read_since(0);
        std::this_thread::yield();
    }
    writer.join();

    EXPECT_FALSE(
        publish_failed.load(std::memory_order_acquire));
    EXPECT_EQ(observed->committed_generation, 7U);
    EXPECT_EQ(observed->persisted_through, 4096);
}

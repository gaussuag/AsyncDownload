#include <concurrentqueue/blockingconcurrentqueue.h>
#include <gtest/gtest.h>

#include "core/memory_accounting.hpp"
#include "core/models.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace {

using PacketQueue =
    moodycamel::BlockingConcurrentQueue<asyncdownload::core::DataPacket>;

TEST(PacketFlowCharacterizationTest, ExplicitProducerOrdersDataBeforeCompletion) {
    PacketQueue queue(1, 1, 0);
    PacketQueue::producer_token_t producer(queue);

    asyncdownload::core::DataPacket data{};
    data.kind = asyncdownload::core::PacketKind::data;
    data.offset = 4096;
    data.payload = {1, 2, 3, 4};

    asyncdownload::core::DataPacket completion{};
    completion.kind = asyncdownload::core::PacketKind::range_complete;

    ASSERT_TRUE(queue.enqueue(producer, std::move(data)));
    ASSERT_TRUE(queue.enqueue(producer, std::move(completion)));

    asyncdownload::core::DataPacket received{};
    ASSERT_TRUE(queue.try_dequeue(received));
    EXPECT_EQ(received.kind, asyncdownload::core::PacketKind::data);
    ASSERT_TRUE(queue.try_dequeue(received));
    EXPECT_EQ(received.kind, asyncdownload::core::PacketKind::range_complete);
}

TEST(PacketFlowCharacterizationTest, ConstructorCapacityIsNotALogicalHardBudget) {
    PacketQueue queue(1, 1, 0);
    PacketQueue::producer_token_t producer(queue);

    EXPECT_TRUE(queue.try_enqueue(producer, asyncdownload::core::DataPacket{}));
    EXPECT_TRUE(queue.try_enqueue(producer, asyncdownload::core::DataPacket{}));
}

TEST(PacketFlowCharacterizationTest, SeparateProducerStreamsOnlyPreserveLocalOrder) {
    PacketQueue queue(4, 1, 1);
    PacketQueue::producer_token_t network(queue);

    asyncdownload::core::DataPacket first{};
    first.kind = asyncdownload::core::PacketKind::data;
    first.offset = 1;
    asyncdownload::core::DataPacket second{};
    second.kind = asyncdownload::core::PacketKind::range_complete;
    second.offset = 2;
    asyncdownload::core::DataPacket shutdown{};
    shutdown.kind = asyncdownload::core::PacketKind::shutdown;

    ASSERT_TRUE(queue.enqueue(network, std::move(first)));
    ASSERT_TRUE(queue.enqueue(std::move(shutdown)));
    ASSERT_TRUE(queue.enqueue(network, std::move(second)));

    std::array<asyncdownload::core::DataPacket, 3> received{};
    for (auto& packet : received) {
        ASSERT_TRUE(queue.try_dequeue(packet));
    }

    std::size_t first_index = received.size();
    std::size_t second_index = received.size();
    std::size_t shutdown_count = 0;
    for (std::size_t index = 0; index < received.size(); ++index) {
        if (received[index].kind == asyncdownload::core::PacketKind::data) {
            first_index = index;
        } else if (received[index].kind ==
                   asyncdownload::core::PacketKind::range_complete) {
            second_index = index;
        } else if (received[index].kind ==
                   asyncdownload::core::PacketKind::shutdown) {
            ++shutdown_count;
        }
    }

    EXPECT_LT(first_index, second_index);
    EXPECT_EQ(shutdown_count, 1U);
}

TEST(PacketFlowCharacterizationTest, MemoryAdmissionAllowsAnOversizedFirstPacket) {
    EXPECT_FALSE(asyncdownload::core::should_pause_for_backpressure(0, 129, 128));
    EXPECT_TRUE(asyncdownload::core::should_pause_for_backpressure(1, 128, 128));
}

}

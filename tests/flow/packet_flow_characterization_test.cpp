#include <concurrentqueue/blockingconcurrentqueue.h>
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace {

enum class LegacyEnvelopeType : std::uint8_t {
    data = 0,
    range_complete,
    shutdown
};

struct LegacyPacket {
    LegacyEnvelopeType kind = LegacyEnvelopeType::data;
    std::int64_t offset = 0;
    std::array<std::uint8_t, 4> payload{};
};

using PacketQueue =
    moodycamel::BlockingConcurrentQueue<LegacyPacket>;

TEST(PacketFlowCharacterizationTest, ExplicitProducerOrdersDataBeforeCompletion) {
    PacketQueue queue(1, 1, 0);
    PacketQueue::producer_token_t producer(queue);

    LegacyPacket data{};
    data.kind = LegacyEnvelopeType::data;
    data.offset = 4096;
    data.payload = {1, 2, 3, 4};

    LegacyPacket completion{};
    completion.kind = LegacyEnvelopeType::range_complete;

    ASSERT_TRUE(queue.enqueue(producer, std::move(data)));
    ASSERT_TRUE(queue.enqueue(producer, std::move(completion)));

    LegacyPacket received{};
    ASSERT_TRUE(queue.try_dequeue(received));
    EXPECT_EQ(received.kind, LegacyEnvelopeType::data);
    ASSERT_TRUE(queue.try_dequeue(received));
    EXPECT_EQ(received.kind, LegacyEnvelopeType::range_complete);
}

TEST(PacketFlowCharacterizationTest, ConstructorCapacityIsNotALogicalHardBudget) {
    PacketQueue queue(1, 1, 0);
    PacketQueue::producer_token_t producer(queue);

    EXPECT_TRUE(queue.try_enqueue(producer, LegacyPacket{}));
    EXPECT_TRUE(queue.try_enqueue(producer, LegacyPacket{}));
}

TEST(PacketFlowCharacterizationTest, SeparateProducerStreamsOnlyPreserveLocalOrder) {
    PacketQueue queue(4, 1, 1);
    PacketQueue::producer_token_t network(queue);

    LegacyPacket first{};
    first.kind = LegacyEnvelopeType::data;
    first.offset = 1;
    LegacyPacket second{};
    second.kind = LegacyEnvelopeType::range_complete;
    second.offset = 2;
    LegacyPacket shutdown{};
    shutdown.kind = LegacyEnvelopeType::shutdown;

    ASSERT_TRUE(queue.enqueue(network, std::move(first)));
    ASSERT_TRUE(queue.enqueue(std::move(shutdown)));
    ASSERT_TRUE(queue.enqueue(network, std::move(second)));

    std::array<LegacyPacket, 3> received{};
    for (auto& packet : received) {
        ASSERT_TRUE(queue.try_dequeue(packet));
    }

    std::size_t first_index = received.size();
    std::size_t second_index = received.size();
    std::size_t shutdown_count = 0;
    for (std::size_t index = 0; index < received.size(); ++index) {
        if (received[index].kind == LegacyEnvelopeType::data) {
            first_index = index;
        } else if (received[index].kind ==
                   LegacyEnvelopeType::range_complete) {
            second_index = index;
        } else if (received[index].kind ==
                   LegacyEnvelopeType::shutdown) {
            ++shutdown_count;
        }
    }

    EXPECT_LT(first_index, second_index);
    EXPECT_EQ(shutdown_count, 1U);
}

}

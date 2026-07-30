#pragma once

#include "flow/packet_flow.hpp"

#include <concurrentqueue/blockingconcurrentqueue.h>

#include <chrono>
#include <cstdint>
#include <utility>

namespace asyncdownload::flow::detail {

enum class PacketEnvelopeKind : std::uint8_t {
    data = 0,
    control,
    close
};

struct PacketEnvelope {
    PacketEnvelopeKind kind = PacketEnvelopeKind::data;
    PacketSequence sequence = 0;
    DataPacket data{};
    ControlPacket control{};
    std::size_t accounted_bytes = 0;
};

struct PacketQueueTraits final : moodycamel::ConcurrentQueueDefaultTraits {
    static constexpr std::size_t INITIAL_IMPLICIT_PRODUCER_HASH_SIZE = 0;
};

class MoodycamelPacketQueueAdapter {
public:
    explicit MoodycamelPacketQueueAdapter(const std::size_t minimum_capacity)
        : queue_(minimum_capacity, 1, 0),
          producer_(queue_),
          consumer_(queue_) {}

    MoodycamelPacketQueueAdapter(const MoodycamelPacketQueueAdapter&) = delete;
    MoodycamelPacketQueueAdapter& operator=(const MoodycamelPacketQueueAdapter&) = delete;

    [[nodiscard]] bool try_publish(PacketEnvelope& envelope) {
        return queue_.try_enqueue(producer_, std::move(envelope));
    }

    [[nodiscard]] bool publish(PacketEnvelope& envelope) {
        return queue_.enqueue(producer_, std::move(envelope));
    }

    [[nodiscard]] bool receive(PacketEnvelope& envelope,
                               const std::chrono::microseconds timeout) {
        return queue_.wait_dequeue_timed(consumer_, envelope, timeout);
    }

private:
    using Queue = moodycamel::BlockingConcurrentQueue<PacketEnvelope, PacketQueueTraits>;

    Queue queue_;
    Queue::producer_token_t producer_;
    Queue::consumer_token_t consumer_;
};

}

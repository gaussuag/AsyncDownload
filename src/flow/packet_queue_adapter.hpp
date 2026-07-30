#pragma once

#include "flow/packet_flow.hpp"

#include <chrono>
#include <cstdint>
#include <utility>

#if defined(ASYNCDOWNLOAD_PACKET_FLOW_FAULT_TEST)
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <new>
#else
#include <concurrentqueue/blockingconcurrentqueue.h>
#endif

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

#if defined(ASYNCDOWNLOAD_PACKET_FLOW_FAULT_TEST)

struct PacketQueueFaultPlan {
    std::atomic<bool> fail_next_data_publish{false};
    std::atomic<bool> fail_next_control_publish{false};
    std::atomic<bool> fail_next_close_publish{false};
    std::atomic<bool> fail_next_constructor{false};
    std::atomic<bool> fail_next_payload_allocation{false};
    std::atomic<bool> force_next_receive_timeout{false};
    std::atomic<bool> block_next_data_publish{false};
    std::atomic<bool> data_publish_blocked{false};
    std::atomic<bool> release_data_publish{false};
    std::atomic<bool> consumer_fail_started{false};

    void reset() noexcept {
        fail_next_data_publish.store(false, std::memory_order_release);
        fail_next_control_publish.store(false, std::memory_order_release);
        fail_next_close_publish.store(false, std::memory_order_release);
        fail_next_constructor.store(false, std::memory_order_release);
        fail_next_payload_allocation.store(false, std::memory_order_release);
        force_next_receive_timeout.store(false, std::memory_order_release);
        block_next_data_publish.store(false, std::memory_order_release);
        data_publish_blocked.store(false, std::memory_order_release);
        release_data_publish.store(false, std::memory_order_release);
        consumer_fail_started.store(false, std::memory_order_release);
    }
};

inline PacketQueueFaultPlan& packet_queue_fault_plan() noexcept {
    static PacketQueueFaultPlan plan;
    return plan;
}

class FaultInjectingPacketQueueAdapter {
public:
    explicit FaultInjectingPacketQueueAdapter(std::size_t) {
        if (packet_queue_fault_plan().fail_next_constructor.exchange(
                false, std::memory_order_acq_rel)) {
            throw std::bad_alloc();
        }
    }

    FaultInjectingPacketQueueAdapter(
        const FaultInjectingPacketQueueAdapter&) = delete;
    FaultInjectingPacketQueueAdapter& operator=(
        const FaultInjectingPacketQueueAdapter&) = delete;

    [[nodiscard]] bool try_publish(PacketEnvelope& envelope) {
        auto& plan = packet_queue_fault_plan();
        if (plan.block_next_data_publish.exchange(
                false, std::memory_order_acq_rel)) {
            plan.data_publish_blocked.store(true, std::memory_order_release);
            plan.data_publish_blocked.notify_all();
            while (!plan.release_data_publish.load(
                std::memory_order_acquire)) {
                plan.release_data_publish.wait(
                    false, std::memory_order_acquire);
            }
        }
        if (plan.fail_next_data_publish.exchange(
                false, std::memory_order_acq_rel)) {
            return false;
        }
        return enqueue(envelope);
    }

    [[nodiscard]] bool publish(PacketEnvelope& envelope) {
        auto& plan = packet_queue_fault_plan();
        if (envelope.kind == PacketEnvelopeKind::control &&
            plan.fail_next_control_publish.exchange(
                false, std::memory_order_acq_rel)) {
            return false;
        }
        if (envelope.kind == PacketEnvelopeKind::close &&
            plan.fail_next_close_publish.exchange(
                false, std::memory_order_acq_rel)) {
            return false;
        }
        return enqueue(envelope);
    }

    [[nodiscard]] bool receive(
        PacketEnvelope& envelope,
        const std::chrono::microseconds timeout) {
        if (packet_queue_fault_plan().force_next_receive_timeout.exchange(
                false, std::memory_order_acq_rel)) {
            return false;
        }
        std::unique_lock lock(mutex_);
        if (!available_.wait_for(lock, timeout, [this]() {
                return !queue_.empty();
            })) {
            return false;
        }
        envelope = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

private:
    [[nodiscard]] bool enqueue(PacketEnvelope& envelope) noexcept {
        try {
            {
                std::scoped_lock lock(mutex_);
                queue_.push_back(std::move(envelope));
            }
            available_.notify_one();
            return true;
        } catch (...) {
            return false;
        }
    }

    std::mutex mutex_;
    std::condition_variable available_;
    std::deque<PacketEnvelope> queue_;
};

using PacketQueueAdapter = FaultInjectingPacketQueueAdapter;

#else

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

using PacketQueueAdapter = MoodycamelPacketQueueAdapter;

#endif

}

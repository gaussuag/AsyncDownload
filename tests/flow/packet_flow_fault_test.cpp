#include <gtest/gtest.h>

#include "asyncdownload/telemetry/telemetry_session.hpp"
#include "flow/packet_flow.hpp"
#include "flow/packet_queue_adapter.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <system_error>
#include <utility>

namespace {

asyncdownload::download::FlowControlPolicy fault_policy() {
    return {
        2,
        1024 * 1024,
        512 * 1024
    };
}

class PacketFlowFaultTest : public testing::Test {
protected:
    void SetUp() override {
        asyncdownload::flow::detail::packet_queue_fault_plan().reset();
    }

    void TearDown() override {
        auto& plan =
            asyncdownload::flow::detail::packet_queue_fault_plan();
        plan.release_data_publish.store(true, std::memory_order_release);
        plan.release_data_publish.notify_all();
        plan.reset();
    }
};

TEST_F(PacketFlowFaultTest, QueueConstructorAllocationFailureIsMapped) {
    auto& plan =
        asyncdownload::flow::detail::packet_queue_fault_plan();
    plan.fail_next_constructor.store(true, std::memory_order_release);
    asyncdownload::telemetry::TelemetrySession telemetry;
    std::unique_ptr<asyncdownload::flow::PacketFlow> flow;

    const auto error = asyncdownload::flow::PacketFlow::create(
        fault_policy(), telemetry, flow);

    EXPECT_EQ(error, std::make_error_code(std::errc::not_enough_memory));
    EXPECT_EQ(flow, nullptr);
}

TEST_F(PacketFlowFaultTest, DataBackendFailureRetainsDraftAndCredit) {
    asyncdownload::telemetry::TelemetrySession telemetry;
    std::unique_ptr<asyncdownload::flow::PacketFlow> flow;
    ASSERT_FALSE(asyncdownload::flow::PacketFlow::create(
        fault_policy(), telemetry, flow));
    asyncdownload::flow::ProducerLane lane;
    ASSERT_FALSE(flow->producer().open_lane(lane));
    const std::array<std::uint8_t, 4> bytes{1, 2, 3, 4};
    ASSERT_TRUE(flow->producer().accept(
        lane, {{{1}, 1}, {0, 4}, 0, bytes}).accepted());
    auto& plan =
        asyncdownload::flow::detail::packet_queue_fault_plan();
    plan.fail_next_data_publish.store(true, std::memory_order_release);

    const auto failed = flow->producer().flush(lane);

    EXPECT_EQ(
        failed.code,
        asyncdownload::flow::PacketAdmissionCode::
            backend_temporarily_unavailable);
    EXPECT_EQ(failed.consumed_bytes, 0U);
    EXPECT_EQ(flow->producer().snapshot().queued_packets, 0U);
    EXPECT_GT(flow->producer().snapshot().accounted_bytes, 0U);
    const auto retried = flow->producer().flush(lane);
    EXPECT_TRUE(retried.accepted());
    EXPECT_EQ(retried.published_bytes, bytes.size());
    ASSERT_FALSE(flow->producer().close());
    asyncdownload::flow::PacketLease lease;
    ASSERT_EQ(
        flow->consumer().receive(
            lease, std::chrono::milliseconds(1)).code,
        asyncdownload::flow::PacketReceiveCode::packet);
    lease.complete();
    EXPECT_EQ(
        flow->consumer().receive(
            lease, std::chrono::milliseconds(1)).code,
        asyncdownload::flow::PacketReceiveCode::closed);
    EXPECT_EQ(flow->producer().snapshot().queued_packets, 0U);
    EXPECT_EQ(flow->producer().snapshot().accounted_bytes, 0U);
}

TEST_F(PacketFlowFaultTest, ControlAllocationFailureBecomesTerminalFailure) {
    asyncdownload::telemetry::TelemetrySession telemetry;
    std::unique_ptr<asyncdownload::flow::PacketFlow> flow;
    ASSERT_FALSE(asyncdownload::flow::PacketFlow::create(
        fault_policy(), telemetry, flow));
    auto& plan =
        asyncdownload::flow::detail::packet_queue_fault_plan();
    plan.fail_next_control_publish.store(true, std::memory_order_release);

    const auto result = flow->producer().publish({
        asyncdownload::flow::ControlPacketKind::range_complete,
        {{1}, 1},
        4
    });

    EXPECT_EQ(
        result.code,
        asyncdownload::flow::PacketPublishCode::failed);
    EXPECT_TRUE(result.error);
    EXPECT_EQ(
        flow->producer().snapshot().state,
        asyncdownload::flow::PacketFlowState::failed);
    EXPECT_EQ(flow->producer().snapshot().queued_packets, 0U);
    EXPECT_EQ(flow->producer().snapshot().accounted_bytes, 0U);
}

TEST_F(PacketFlowFaultTest, CloseAllocationFailureBecomesTerminalFailure) {
    asyncdownload::telemetry::TelemetrySession telemetry;
    std::unique_ptr<asyncdownload::flow::PacketFlow> flow;
    ASSERT_FALSE(asyncdownload::flow::PacketFlow::create(
        fault_policy(), telemetry, flow));
    auto& plan =
        asyncdownload::flow::detail::packet_queue_fault_plan();
    plan.fail_next_close_publish.store(true, std::memory_order_release);

    const auto error = flow->producer().close();

    EXPECT_TRUE(error);
    EXPECT_EQ(
        flow->producer().snapshot().state,
        asyncdownload::flow::PacketFlowState::failed);
    EXPECT_EQ(flow->producer().snapshot().queued_packets, 0U);
    EXPECT_EQ(flow->producer().snapshot().accounted_bytes, 0U);
}

TEST_F(PacketFlowFaultTest, EnvelopeAllocationFailureRollsBackAccounting) {
    asyncdownload::telemetry::TelemetrySession telemetry;
    std::unique_ptr<asyncdownload::flow::PacketFlow> flow;
    ASSERT_FALSE(asyncdownload::flow::PacketFlow::create(
        fault_policy(), telemetry, flow));
    asyncdownload::flow::ProducerLane lane;
    ASSERT_FALSE(flow->producer().open_lane(lane));
    const std::array<std::uint8_t, 4> bytes{1, 2, 3, 4};
    auto& plan =
        asyncdownload::flow::detail::packet_queue_fault_plan();
    plan.fail_next_payload_allocation.store(
        true, std::memory_order_release);

    const auto result = flow->producer().accept(
        lane, {{{1}, 1}, {0, 4}, 0, bytes});

    EXPECT_EQ(
        result.code,
        asyncdownload::flow::PacketAdmissionCode::failed);
    EXPECT_EQ(
        result.error,
        std::make_error_code(std::errc::not_enough_memory));
    EXPECT_EQ(flow->producer().snapshot().queued_packets, 0U);
    EXPECT_EQ(flow->producer().snapshot().accounted_bytes, 0U);
    EXPECT_FALSE(flow->consumer().fail(
        std::make_error_code(std::errc::io_error)));
    EXPECT_EQ(
        flow->producer().snapshot().error,
        std::make_error_code(std::errc::not_enough_memory));
}

TEST_F(PacketFlowFaultTest, TimedReceiveTimeoutDoesNotConsumePacket) {
    asyncdownload::telemetry::TelemetrySession telemetry;
    std::unique_ptr<asyncdownload::flow::PacketFlow> flow;
    ASSERT_FALSE(asyncdownload::flow::PacketFlow::create(
        fault_policy(), telemetry, flow));
    asyncdownload::flow::ProducerLane lane;
    ASSERT_FALSE(flow->producer().open_lane(lane));
    const std::array<std::uint8_t, 4> bytes{1, 2, 3, 4};
    ASSERT_TRUE(flow->producer().accept(
        lane, {{{1}, 1}, {0, 4}, 0, bytes}).accepted());
    ASSERT_TRUE(flow->producer().flush(lane).accepted());
    auto& plan =
        asyncdownload::flow::detail::packet_queue_fault_plan();
    plan.force_next_receive_timeout.store(
        true, std::memory_order_release);
    asyncdownload::flow::PacketLease lease;

    EXPECT_EQ(
        flow->consumer().receive(
            lease, std::chrono::milliseconds(1)).code,
        asyncdownload::flow::PacketReceiveCode::timeout);
    EXPECT_EQ(flow->producer().snapshot().queued_packets, 1U);
    ASSERT_EQ(
        flow->consumer().receive(
            lease, std::chrono::milliseconds(1)).code,
        asyncdownload::flow::PacketReceiveCode::packet);
    lease.complete();
    ASSERT_FALSE(flow->producer().close());
    EXPECT_EQ(
        flow->consumer().receive(
            lease, std::chrono::milliseconds(1)).code,
        asyncdownload::flow::PacketReceiveCode::closed);
}

TEST_F(PacketFlowFaultTest, ConsumerFailWaitsForAndDrainsActiveProducer) {
    auto& plan =
        asyncdownload::flow::detail::packet_queue_fault_plan();
    plan.block_next_data_publish.store(true, std::memory_order_release);
    std::promise<asyncdownload::flow::PacketFlow*> ready;
    std::promise<void> producer_done;
    asyncdownload::telemetry::TelemetrySession telemetry;
    auto producer = std::async(std::launch::async, [&]() {
        std::unique_ptr<asyncdownload::flow::PacketFlow> flow;
        EXPECT_FALSE(asyncdownload::flow::PacketFlow::create(
            fault_policy(), telemetry, flow));
        asyncdownload::flow::ProducerLane lane;
        EXPECT_FALSE(flow->producer().open_lane(lane));
        ready.set_value(flow.get());
        std::array<std::uint8_t, 64 * 1024> bytes{};
        const auto result = flow->producer().accept(
            lane,
            {{{1}, 1}, {0, 64 * 1024}, 0, bytes});
        producer_done.get_future().wait();
        return std::pair{
            result,
            flow->producer().snapshot()
        };
    });
    auto* flow = ready.get_future().get();
    while (!plan.data_publish_blocked.load(std::memory_order_acquire)) {
        plan.data_publish_blocked.wait(false, std::memory_order_acquire);
    }
    auto failed = std::async(std::launch::async, [flow]() {
        return flow->consumer().fail(
            std::make_error_code(std::errc::io_error));
    });
    while (!plan.consumer_fail_started.load(std::memory_order_acquire)) {
        plan.consumer_fail_started.wait(
            false, std::memory_order_acquire);
    }
    plan.release_data_publish.store(true, std::memory_order_release);
    plan.release_data_publish.notify_all();

    EXPECT_FALSE(failed.get());
    producer_done.set_value();
    const auto [admission, snapshot] = producer.get();
    EXPECT_TRUE(admission.accepted());
    EXPECT_EQ(
        snapshot.state,
        asyncdownload::flow::PacketFlowState::failed);
    EXPECT_EQ(snapshot.queued_packets, 0U);
    EXPECT_EQ(snapshot.accounted_bytes, 0U);
}

}

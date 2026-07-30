#include <gtest/gtest.h>

#include "asyncdownload/telemetry/telemetry_session.hpp"
#include "core/constants.hpp"
#include "flow/packet_flow.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <latch>
#include <limits>
#include <map>
#include <memory>
#include <system_error>
#include <utility>
#include <vector>

namespace {

asyncdownload::download::FlowControlPolicy test_policy() {
    return {
        4,
        1024 * 1024,
        512 * 1024
    };
}

std::unique_ptr<asyncdownload::flow::PacketFlow> make_flow(
    asyncdownload::telemetry::TelemetrySession& telemetry,
    const std::size_t packet_budget = 4) {
    std::unique_ptr<asyncdownload::flow::PacketFlow> flow;
    auto policy = test_policy();
    policy.packet_budget = packet_budget;
    EXPECT_FALSE(asyncdownload::flow::PacketFlow::create(
        policy, telemetry, flow));
    return flow;
}

TEST(PacketFlowTest, CreatesOneProducerAndOneConsumerFromValidatedPolicy) {
    asyncdownload::telemetry::TelemetrySession telemetry;
    std::unique_ptr<asyncdownload::flow::PacketFlow> flow;

    ASSERT_FALSE(asyncdownload::flow::PacketFlow::create(
        test_policy(), telemetry, flow));
    ASSERT_NE(flow, nullptr);
    EXPECT_EQ(&flow->producer(), &flow->producer());
    EXPECT_EQ(&flow->consumer(), &flow->consumer());

    ASSERT_FALSE(flow->producer().close());
    asyncdownload::flow::PacketLease lease;
    const auto received = flow->consumer().receive(
        lease, std::chrono::milliseconds(1));
    EXPECT_EQ(received.code, asyncdownload::flow::PacketReceiveCode::closed);
    EXPECT_FALSE(received.error);
}

TEST(PacketFlowTest, RejectsWrongThreadColdMutationWithoutSideEffect) {
    asyncdownload::telemetry::TelemetrySession telemetry;
    auto flow = make_flow(telemetry);
    auto wrong_thread_error = std::async(std::launch::async, [&]() {
        asyncdownload::flow::ProducerLane lane;
        return flow->producer().open_lane(lane);
    });

    EXPECT_TRUE(wrong_thread_error.get());
    const auto before = flow->producer().snapshot();
    EXPECT_EQ(before.state, asyncdownload::flow::PacketFlowState::open);
    EXPECT_EQ(before.queued_packets, 0U);
    EXPECT_EQ(before.accounted_bytes, 0U);

    asyncdownload::flow::PacketLease packet;
    EXPECT_EQ(
        flow->consumer().receive(
            packet, std::chrono::microseconds(0)).code,
        asyncdownload::flow::PacketReceiveCode::timeout);
    auto wrong_consumer_error =
        std::async(std::launch::async, [&]() {
            return flow->consumer().fail(
                std::make_error_code(std::errc::io_error));
        });
    EXPECT_TRUE(wrong_consumer_error.get());
    EXPECT_EQ(
        flow->producer().snapshot().state,
        asyncdownload::flow::PacketFlowState::open);
    ASSERT_FALSE(flow->producer().close());
    EXPECT_EQ(
        flow->consumer().receive(
            packet, std::chrono::milliseconds(1)).code,
        asyncdownload::flow::PacketReceiveCode::closed);
}

TEST(PacketFlowTest, EnforcesLogicalHardBudgetAtOneTwoAndThirtyThree) {
    for (const auto budget : {1U, 2U, 33U}) {
        asyncdownload::telemetry::TelemetrySession telemetry;
        auto flow = make_flow(telemetry, budget);
        asyncdownload::flow::ProducerLane lane;
        ASSERT_FALSE(flow->producer().open_lane(lane));
        const std::array<std::uint8_t, 4> bytes{1, 2, 3, 4};
        const asyncdownload::range::LeaseId lease{{6}, 1};
        const asyncdownload::range::ByteSpan span{
            0,
            static_cast<std::int64_t>((budget + 1) * bytes.size())
        };

        for (std::size_t index = 0; index < budget; ++index) {
            ASSERT_TRUE(flow->producer().accept(
                lane,
                {
                    lease,
                    span,
                    static_cast<std::int64_t>(index * bytes.size()),
                    bytes
                }).accepted());
            ASSERT_TRUE(flow->producer().flush(lane).accepted());
        }
        ASSERT_TRUE(flow->producer().accept(
            lane,
            {
                lease,
                span,
                static_cast<std::int64_t>(budget * bytes.size()),
                bytes
            }).accepted());

        const auto exhausted = flow->producer().flush(lane);
        EXPECT_EQ(
            exhausted.code,
            asyncdownload::flow::PacketAdmissionCode::packet_budget_exhausted);
        EXPECT_EQ(exhausted.consumed_bytes, 0U);
        EXPECT_EQ(exhausted.published_bytes, 0U);
        EXPECT_EQ(flow->producer().snapshot().queued_packets, budget);

        asyncdownload::flow::PacketLease packet;
        ASSERT_EQ(
            flow->consumer().receive(
                packet, std::chrono::milliseconds(1)).code,
            asyncdownload::flow::PacketReceiveCode::packet);
        packet.complete();
        const auto resumed = flow->producer().flush(lane);
        EXPECT_TRUE(resumed.accepted());
        EXPECT_EQ(resumed.published_bytes, bytes.size());
        ASSERT_FALSE(flow->producer().close());

        while (flow->consumer().receive(
                   packet,
                   std::chrono::milliseconds(1)).code ==
               asyncdownload::flow::PacketReceiveCode::packet) {
            packet.complete();
        }
        const auto snapshot = flow->producer().snapshot();
        EXPECT_EQ(snapshot.state, asyncdownload::flow::PacketFlowState::closed);
        EXPECT_EQ(snapshot.queued_packets, 0U);
        EXPECT_EQ(snapshot.accounted_bytes, 0U);
    }
}

TEST(PacketFlowTest, ControlBypassesDataAdmissionBudgetWithoutBeingDropped) {
    asyncdownload::telemetry::TelemetrySession telemetry;
    auto flow = make_flow(telemetry, 1);
    asyncdownload::flow::ProducerLane lane;
    ASSERT_FALSE(flow->producer().open_lane(lane));
    const std::array<std::uint8_t, 4> bytes{1, 2, 3, 4};
    ASSERT_TRUE(flow->producer().accept(
        lane, {{{9}, 1}, {0, 4}, 0, bytes}).accepted());
    ASSERT_TRUE(flow->producer().flush(lane).accepted());

    const auto control = flow->producer().publish({
        asyncdownload::flow::ControlPacketKind::range_complete,
        {{9}, 1},
        4
    });

    EXPECT_EQ(
        control.code,
        asyncdownload::flow::PacketPublishCode::published);
    EXPECT_EQ(flow->producer().snapshot().queued_packets, 2U);
    ASSERT_FALSE(flow->producer().close());
    asyncdownload::flow::PacketLease packet;
    ASSERT_EQ(
        flow->consumer().receive(packet, std::chrono::milliseconds(1)).code,
        asyncdownload::flow::PacketReceiveCode::packet);
    EXPECT_EQ(packet.kind(), asyncdownload::flow::PacketKind::data);
    packet.complete();
    ASSERT_EQ(
        flow->consumer().receive(packet, std::chrono::milliseconds(1)).code,
        asyncdownload::flow::PacketReceiveCode::packet);
    EXPECT_EQ(packet.kind(), asyncdownload::flow::PacketKind::control);
    packet.complete();
    EXPECT_EQ(
        flow->consumer().receive(packet, std::chrono::milliseconds(1)).code,
        asyncdownload::flow::PacketReceiveCode::closed);
}

TEST(PacketFlowTest, ConcurrentDownloadInstancesDoNotShareAccounting) {
    asyncdownload::telemetry::TelemetrySession first_telemetry;
    asyncdownload::telemetry::TelemetrySession second_telemetry;
    auto policy = test_policy();
    policy.memory_high_bytes = 128;
    policy.memory_low_bytes = 64;
    std::unique_ptr<asyncdownload::flow::PacketFlow> first;
    std::unique_ptr<asyncdownload::flow::PacketFlow> second;
    ASSERT_FALSE(asyncdownload::flow::PacketFlow::create(
        policy, first_telemetry, first));
    ASSERT_FALSE(asyncdownload::flow::PacketFlow::create(
        policy, second_telemetry, second));
    asyncdownload::flow::ProducerLane first_lane;
    asyncdownload::flow::ProducerLane second_lane;
    ASSERT_FALSE(first->producer().open_lane(first_lane));
    ASSERT_FALSE(second->producer().open_lane(second_lane));
    std::vector<std::uint8_t> bytes(129, 5);

    const auto first_admission = first->producer().accept(
        first_lane,
        {
            {{1}, 1},
            {0, static_cast<std::int64_t>(bytes.size())},
            0,
            bytes
        });
    const auto second_admission = second->producer().accept(
        second_lane,
        {
            {{2}, 1},
            {0, static_cast<std::int64_t>(bytes.size())},
            0,
            bytes
        });

    EXPECT_TRUE(first_admission.accepted());
    EXPECT_TRUE(second_admission.accepted());
    EXPECT_EQ(
        first->producer().snapshot().accounted_bytes,
        sizeof(asyncdownload::flow::DataPacket) + bytes.size());
    EXPECT_EQ(
        second->producer().snapshot().accounted_bytes,
        sizeof(asyncdownload::flow::DataPacket) + bytes.size());
    ASSERT_FALSE(first->producer().discard(first_lane));
    ASSERT_FALSE(second->producer().discard(second_lane));
    EXPECT_EQ(first->producer().snapshot().accounted_bytes, 0U);
    EXPECT_EQ(second->producer().snapshot().accounted_bytes, 0U);
}

TEST(PacketFlowTest, ConsumerFailureDrainsQueuedAccounting) {
    asyncdownload::telemetry::TelemetrySession telemetry;
    auto flow = make_flow(telemetry);
    asyncdownload::flow::ProducerLane lane;
    ASSERT_FALSE(flow->producer().open_lane(lane));
    const std::array<std::uint8_t, 4> bytes{1, 2, 3, 4};
    ASSERT_TRUE(flow->producer().accept(
        lane, {{{4}, 1}, {0, 4}, 0, bytes}).accepted());
    ASSERT_TRUE(flow->producer().flush(lane).accepted());
    ASSERT_EQ(
        flow->producer().publish({
            asyncdownload::flow::ControlPacketKind::range_complete,
            {{4}, 1},
            4
        }).code,
        asyncdownload::flow::PacketPublishCode::published);
    ASSERT_EQ(flow->producer().snapshot().queued_packets, 2U);
    ASSERT_GT(flow->producer().snapshot().accounted_bytes, 0U);

    const auto failure = std::make_error_code(std::errc::io_error);
    ASSERT_FALSE(flow->consumer().fail(failure));
    const auto snapshot = flow->producer().snapshot();
    EXPECT_EQ(snapshot.state, asyncdownload::flow::PacketFlowState::failed);
    EXPECT_EQ(snapshot.error, failure);
    EXPECT_EQ(snapshot.queued_packets, 0U);
    EXPECT_EQ(snapshot.accounted_bytes, 0U);

    EXPECT_FALSE(flow->consumer().fail(
        std::make_error_code(std::errc::no_space_on_device)));
    EXPECT_EQ(flow->producer().snapshot().error, failure);
}

TEST(PacketFlowTest, QueuePauseCountsOnlyOnEpisodeEntry) {
    asyncdownload::telemetry::TelemetrySession telemetry;
    telemetry.record_task_started();
    auto flow = make_flow(telemetry, 1);
    asyncdownload::flow::ProducerLane lane;
    ASSERT_FALSE(flow->producer().open_lane(lane));
    const std::array<std::uint8_t, 4> bytes{1, 2, 3, 4};
    ASSERT_TRUE(flow->producer().accept(
        lane, {{{1}, 1}, {0, 8}, 0, bytes}).accepted());
    ASSERT_TRUE(flow->producer().flush(lane).accepted());
    ASSERT_TRUE(flow->producer().accept(
        lane, {{{1}, 1}, {0, 8}, 4, bytes}).accepted());

    const auto first = flow->producer().flush(lane);
    const auto repeated = flow->producer().flush(lane);

    EXPECT_EQ(
        first.code,
        asyncdownload::flow::PacketAdmissionCode::packet_budget_exhausted);
    EXPECT_EQ(
        repeated.code,
        asyncdownload::flow::PacketAdmissionCode::packet_budget_exhausted);
    EXPECT_EQ(first.consumed_bytes, 0U);
    EXPECT_EQ(repeated.consumed_bytes, 0U);
    auto summary = telemetry.final_summary();
    EXPECT_EQ(summary.total_pause_count, 1U);
    EXPECT_EQ(summary.queue_full_pause_count, 1U);

    asyncdownload::flow::PacketLease packet;
    ASSERT_EQ(
        flow->consumer().receive(packet, std::chrono::milliseconds(1)).code,
        asyncdownload::flow::PacketReceiveCode::packet);
    packet.complete();
    const std::array<asyncdownload::flow::PacketLaneObservation, 1>
        observations{{{lane.id(), 1.0, true}}};
    std::array<asyncdownload::flow::PacketPauseAction, 1> actions{};
    const auto reconciled =
        flow->producer().reconcile(observations, actions);
    ASSERT_FALSE(reconciled.error);
    ASSERT_EQ(reconciled.action_count, 1U);
    EXPECT_EQ(
        actions[0].kind,
        asyncdownload::flow::PacketPauseActionKind::resume_candidate);
    EXPECT_FALSE(flow->producer().paused(lane));
    ASSERT_TRUE(flow->producer().flush(lane).accepted());
    ASSERT_FALSE(flow->producer().close());
    while (flow->consumer().receive(
               packet,
               std::chrono::milliseconds(1)).code ==
           asyncdownload::flow::PacketReceiveCode::packet) {
        packet.complete();
    }
}

TEST(PacketFlowTest, MemoryPauseCountsOnlyOnEpisodeEntry) {
    asyncdownload::telemetry::TelemetrySession telemetry;
    telemetry.record_task_started();
    auto policy = test_policy();
    policy.memory_high_bytes = 128;
    policy.memory_low_bytes = 64;
    std::unique_ptr<asyncdownload::flow::PacketFlow> flow;
    ASSERT_FALSE(asyncdownload::flow::PacketFlow::create(
        policy, telemetry, flow));
    asyncdownload::flow::ProducerLane lane;
    ASSERT_FALSE(flow->producer().open_lane(lane));
    const std::array<std::uint8_t, 40> first{};
    const std::array<std::uint8_t, 20> second{};
    ASSERT_TRUE(flow->producer().accept(
        lane, {{{2}, 1}, {0, 60}, 0, first}).accepted());

    const auto paused = flow->producer().accept(
        lane, {{{2}, 1}, {0, 60}, 40, second});
    const auto repeated = flow->producer().accept(
        lane, {{{2}, 1}, {0, 60}, 40, second});

    EXPECT_EQ(
        paused.code,
        asyncdownload::flow::PacketAdmissionCode::memory_budget_exhausted);
    EXPECT_EQ(
        repeated.code,
        asyncdownload::flow::PacketAdmissionCode::memory_budget_exhausted);
    EXPECT_EQ(paused.consumed_bytes, 0U);
    EXPECT_EQ(repeated.consumed_bytes, 0U);
    const auto summary = telemetry.final_summary();
    EXPECT_EQ(summary.total_pause_count, 1U);
    EXPECT_EQ(summary.queue_full_pause_count, 0U);
    ASSERT_FALSE(flow->producer().discard(lane));
}

TEST(PacketFlowTest, SelectsFastestTwentyPercentWithLaneIdTieBreaker) {
    asyncdownload::telemetry::TelemetrySession telemetry;
    auto policy = test_policy();
    policy.memory_high_bytes = 128;
    policy.memory_low_bytes = 64;
    std::unique_ptr<asyncdownload::flow::PacketFlow> flow;
    ASSERT_FALSE(asyncdownload::flow::PacketFlow::create(
        policy, telemetry, flow));
    std::array<asyncdownload::flow::ProducerLane, 5> lanes;
    for (auto& lane : lanes) {
        ASSERT_FALSE(flow->producer().open_lane(lane));
    }
    std::vector<std::uint8_t> bytes(129, 1);
    ASSERT_TRUE(flow->producer().accept(
        lanes[0],
        {
            {{4}, 1},
            {0, static_cast<std::int64_t>(bytes.size())},
            0,
            bytes
        }).accepted());
    std::array<asyncdownload::flow::PacketLaneObservation, 5>
        observations{};
    for (std::size_t index = 0; index < lanes.size(); ++index) {
        observations[index] = {
            lanes[index].id(),
            10.0,
            true
        };
    }
    std::array<asyncdownload::flow::PacketPauseAction, 5> actions{};

    const auto reconciled =
        flow->producer().reconcile(observations, actions);

    ASSERT_FALSE(reconciled.error);
    ASSERT_EQ(reconciled.action_count, 1U);
    EXPECT_EQ(actions[0].lane_id, lanes[0].id());
    EXPECT_EQ(
        actions[0].kind,
        asyncdownload::flow::PacketPauseActionKind::pause_receive);
    for (auto& lane : lanes) {
        ASSERT_FALSE(flow->producer().discard(lane));
    }
}

TEST(PacketFlowTest, AcceptsContiguousChunksIntoOneLaneDraft) {
    asyncdownload::telemetry::TelemetrySession telemetry;
    auto flow = make_flow(telemetry);
    asyncdownload::flow::ProducerLane lane;
    ASSERT_FALSE(flow->producer().open_lane(lane));
    const std::array<std::uint8_t, 4> first{1, 2, 3, 4};
    const std::array<std::uint8_t, 3> second{5, 6, 7};
    const asyncdownload::range::LeaseId lease{
        asyncdownload::range::RangeId{3}, 4};
    const asyncdownload::range::ByteSpan span{100, 200};

    const auto first_result = flow->producer().accept(
        lane, {lease, span, 100, first});
    const auto second_result = flow->producer().accept(
        lane, {lease, span, 104, second});

    EXPECT_TRUE(first_result.accepted());
    EXPECT_EQ(first_result.consumed_bytes, first.size());
    EXPECT_TRUE(second_result.accepted());
    EXPECT_EQ(second_result.consumed_bytes, second.size());
    EXPECT_EQ(flow->producer().snapshot().queued_packets, 0U);
    EXPECT_EQ(
        flow->producer().snapshot().accounted_bytes,
        sizeof(asyncdownload::flow::DataPacket) + 7U);

    const auto flushed = flow->producer().flush(lane);
    ASSERT_TRUE(flushed.accepted());
    EXPECT_EQ(flushed.published_bytes, 7U);

    asyncdownload::flow::PacketLease packet;
    ASSERT_EQ(
        flow->consumer().receive(packet, std::chrono::milliseconds(1)).code,
        asyncdownload::flow::PacketReceiveCode::packet);
    ASSERT_NE(packet.data(), nullptr);
    EXPECT_EQ(packet.data()->lease, lease);
    EXPECT_EQ(packet.data()->lease_span, span);
    EXPECT_EQ(packet.data()->offset, 100);
    EXPECT_EQ(packet.data()->payload,
        (std::vector<std::uint8_t>{1, 2, 3, 4, 5, 6, 7}));
    EXPECT_EQ(flow->producer().snapshot().queued_packets, 0U);
    EXPECT_GT(flow->producer().snapshot().accounted_bytes, 0U);
    packet.complete();
    EXPECT_EQ(flow->producer().snapshot().accounted_bytes, 0U);
    ASSERT_FALSE(flow->producer().close());
    EXPECT_EQ(
        flow->consumer().receive(packet, std::chrono::milliseconds(1)).code,
        asyncdownload::flow::PacketReceiveCode::closed);
}

TEST(PacketFlowTest, RejectsNonContiguousChunkWithoutMutatingDraft) {
    asyncdownload::telemetry::TelemetrySession telemetry;
    auto flow = make_flow(telemetry);
    asyncdownload::flow::ProducerLane lane;
    ASSERT_FALSE(flow->producer().open_lane(lane));
    const std::array<std::uint8_t, 4> bytes{1, 2, 3, 4};
    const asyncdownload::range::LeaseId lease{
        asyncdownload::range::RangeId{1}, 1};
    const asyncdownload::range::ByteSpan span{0, 32};

    ASSERT_TRUE(flow->producer().accept(
        lane, {lease, span, 0, bytes}).accepted());
    const auto before = flow->producer().snapshot();
    const auto rejected = flow->producer().accept(
        lane, {lease, span, 8, bytes});

    EXPECT_EQ(rejected.code, asyncdownload::flow::PacketAdmissionCode::failed);
    EXPECT_EQ(rejected.error,
        std::make_error_code(std::errc::invalid_argument));
    EXPECT_EQ(
        flow->producer().snapshot().accounted_bytes,
        before.accounted_bytes);
    EXPECT_EQ(
        flow->producer().snapshot().state,
        asyncdownload::flow::PacketFlowState::failed);
    ASSERT_FALSE(flow->producer().discard(lane));
}

TEST(PacketFlowTest, RejectsChunkOutsideLeaseSpanWithoutMutation) {
    asyncdownload::telemetry::TelemetrySession telemetry;
    auto flow = make_flow(telemetry);
    asyncdownload::flow::ProducerLane lane;
    ASSERT_FALSE(flow->producer().open_lane(lane));
    const std::array<std::uint8_t, 4> bytes{1, 2, 3, 4};

    const auto rejected = flow->producer().accept(
        lane,
        {{{4}, 1}, {10, 20}, 18, bytes});

    EXPECT_EQ(rejected.code, asyncdownload::flow::PacketAdmissionCode::failed);
    EXPECT_EQ(rejected.error,
        std::make_error_code(std::errc::invalid_argument));
    const auto snapshot = flow->producer().snapshot();
    EXPECT_EQ(snapshot.state, asyncdownload::flow::PacketFlowState::failed);
    EXPECT_EQ(snapshot.queued_packets, 0U);
    EXPECT_EQ(snapshot.accounted_bytes, 0U);
    EXPECT_EQ(snapshot.error, rejected.error);
    ASSERT_FALSE(flow->producer().discard(lane));
}

TEST(PacketFlowTest, RejectsOffsetSizeOverflowWithoutMutation) {
    asyncdownload::telemetry::TelemetrySession telemetry;
    auto flow = make_flow(telemetry);
    asyncdownload::flow::ProducerLane lane;
    ASSERT_FALSE(flow->producer().open_lane(lane));
    const std::array<std::uint8_t, 2> bytes{1, 2};
    const auto maximum = std::numeric_limits<std::int64_t>::max();

    const auto rejected = flow->producer().accept(
        lane,
        {{{4}, 1}, {0, maximum}, maximum, bytes});

    EXPECT_EQ(rejected.code, asyncdownload::flow::PacketAdmissionCode::failed);
    EXPECT_EQ(rejected.error,
        std::make_error_code(std::errc::invalid_argument));
    const auto snapshot = flow->producer().snapshot();
    EXPECT_EQ(snapshot.state, asyncdownload::flow::PacketFlowState::failed);
    EXPECT_EQ(snapshot.queued_packets, 0U);
    EXPECT_EQ(snapshot.accounted_bytes, 0U);
    ASSERT_FALSE(flow->producer().discard(lane));
}

TEST(PacketFlowTest, RejectsChunkLargerThanAggregationTargetWithoutMutation) {
    asyncdownload::telemetry::TelemetrySession telemetry;
    auto flow = make_flow(telemetry);
    asyncdownload::flow::ProducerLane lane;
    ASSERT_FALSE(flow->producer().open_lane(lane));
    std::vector<std::uint8_t> bytes(64 * 1024 + 1, 7);

    const auto rejected = flow->producer().accept(
        lane,
        {
            {{4}, 1},
            {0, static_cast<std::int64_t>(bytes.size())},
            0,
            bytes
        });

    EXPECT_EQ(rejected.code, asyncdownload::flow::PacketAdmissionCode::failed);
    EXPECT_EQ(rejected.error,
        std::make_error_code(std::errc::invalid_argument));
    const auto snapshot = flow->producer().snapshot();
    EXPECT_EQ(snapshot.state, asyncdownload::flow::PacketFlowState::failed);
    EXPECT_EQ(snapshot.queued_packets, 0U);
    EXPECT_EQ(snapshot.accounted_bytes, 0U);
    ASSERT_FALSE(flow->producer().discard(lane));
}

TEST(PacketFlowTest, RejectsNonPositiveCompletionExpectedEnd) {
    asyncdownload::telemetry::TelemetrySession telemetry;
    auto flow = make_flow(telemetry);

    const auto rejected = flow->producer().publish({
        asyncdownload::flow::ControlPacketKind::range_complete,
        {{1}, 2},
        0
    });

    EXPECT_EQ(rejected.code, asyncdownload::flow::PacketPublishCode::failed);
    EXPECT_EQ(rejected.error,
        std::make_error_code(std::errc::invalid_argument));
    const auto snapshot = flow->producer().snapshot();
    EXPECT_EQ(snapshot.state, asyncdownload::flow::PacketFlowState::failed);
    EXPECT_EQ(snapshot.queued_packets, 0U);
    EXPECT_EQ(snapshot.accounted_bytes, 0U);
    EXPECT_EQ(snapshot.error, rejected.error);
}

TEST(PacketFlowTest, PublishesAtCurrentSixtyFourKibAggregationShape) {
    asyncdownload::telemetry::TelemetrySession telemetry;
    auto flow = make_flow(telemetry);
    asyncdownload::flow::ProducerLane lane;
    ASSERT_FALSE(flow->producer().open_lane(lane));
    std::vector<std::uint8_t> bytes(64 * 1024, 11);

    const auto admission = flow->producer().accept(
        lane,
        {
            {asyncdownload::range::RangeId{2}, 1},
            {0, static_cast<std::int64_t>(bytes.size())},
            0,
            bytes
        });

    EXPECT_TRUE(admission.accepted());
    EXPECT_EQ(admission.published_bytes, bytes.size());
    EXPECT_EQ(flow->producer().snapshot().queued_packets, 1U);
    asyncdownload::flow::PacketLease packet;
    ASSERT_EQ(
        flow->consumer().receive(packet, std::chrono::milliseconds(1)).code,
        asyncdownload::flow::PacketReceiveCode::packet);
    ASSERT_NE(packet.data(), nullptr);
    EXPECT_EQ(packet.data()->payload.size(), bytes.size());
    packet.complete();
    ASSERT_FALSE(flow->producer().close());
    EXPECT_EQ(
        flow->consumer().receive(packet, std::chrono::milliseconds(1)).code,
        asyncdownload::flow::PacketReceiveCode::closed);
}

TEST(PacketFlowTest, ReorderNodeAddsExactlyFortyEightBytesOnce) {
    asyncdownload::telemetry::TelemetrySession telemetry;
    auto flow = make_flow(telemetry);
    asyncdownload::flow::ProducerLane lane;
    ASSERT_FALSE(flow->producer().open_lane(lane));
    const std::array<std::uint8_t, 4> bytes{1, 2, 3, 4};
    ASSERT_TRUE(flow->producer().accept(
        lane,
        {{{5}, 6}, {0, 4}, 0, bytes}).accepted());
    ASSERT_TRUE(flow->producer().flush(lane).accepted());

    asyncdownload::flow::PacketLease packet;
    ASSERT_EQ(
        flow->consumer().receive(packet, std::chrono::milliseconds(1)).code,
        asyncdownload::flow::PacketReceiveCode::packet);
    const auto before =
        flow->producer().snapshot().accounted_bytes;
    EXPECT_FALSE(packet.account_reorder_node());
    EXPECT_EQ(
        flow->producer().snapshot().accounted_bytes,
        before + asyncdownload::core::kMapNodeOverheadBytes);
    EXPECT_TRUE(packet.account_reorder_node());

    asyncdownload::flow::PacketLease moved(std::move(packet));
    EXPECT_FALSE(packet.has_value());
    EXPECT_TRUE(moved.has_value());
    moved.complete();
    EXPECT_EQ(flow->producer().snapshot().accounted_bytes, 0U);
    EXPECT_EQ(
        flow->producer().snapshot().state,
        asyncdownload::flow::PacketFlowState::failed);
}

TEST(PacketFlowTest, PublishesRangeCompleteAfterAllRangeData) {
    asyncdownload::telemetry::TelemetrySession telemetry;
    auto flow = make_flow(telemetry);
    asyncdownload::flow::ProducerLane lane;
    ASSERT_FALSE(flow->producer().open_lane(lane));
    const std::array<std::uint8_t, 4> bytes{1, 2, 3, 4};
    ASSERT_TRUE(flow->producer().accept(
        lane,
        {{{8}, 3}, {0, 4}, 0, bytes}).accepted());
    ASSERT_TRUE(flow->producer().flush(lane).accepted());
    ASSERT_EQ(
        flow->producer().publish({
            asyncdownload::flow::ControlPacketKind::range_complete,
            {{8}, 3},
            4
        }).code,
        asyncdownload::flow::PacketPublishCode::published);
    ASSERT_FALSE(flow->producer().close());

    asyncdownload::flow::PacketLease packet;
    ASSERT_EQ(
        flow->consumer().receive(packet, std::chrono::milliseconds(1)).code,
        asyncdownload::flow::PacketReceiveCode::packet);
    const auto data_sequence = packet.sequence();
    EXPECT_EQ(packet.kind(), asyncdownload::flow::PacketKind::data);
    packet.complete();
    ASSERT_EQ(
        flow->consumer().receive(packet, std::chrono::milliseconds(1)).code,
        asyncdownload::flow::PacketReceiveCode::packet);
    EXPECT_EQ(packet.kind(), asyncdownload::flow::PacketKind::control);
    EXPECT_GT(packet.sequence(), data_sequence);
    ASSERT_NE(packet.control(), nullptr);
    EXPECT_EQ(packet.control()->completion,
        (asyncdownload::range::CompletionId{{8}, 3}));
    EXPECT_EQ(packet.control()->expected_end, 4);
    packet.complete();
    EXPECT_EQ(
        flow->consumer().receive(packet, std::chrono::milliseconds(1)).code,
        asyncdownload::flow::PacketReceiveCode::closed);
}

TEST(PacketFlowTest, CloseMarkerFollowsAllRegularPackets) {
    asyncdownload::telemetry::TelemetrySession telemetry;
    auto flow = make_flow(telemetry);
    asyncdownload::flow::ProducerLane lane;
    ASSERT_FALSE(flow->producer().open_lane(lane));
    const std::array<std::uint8_t, 4> bytes{1, 2, 3, 4};
    ASSERT_TRUE(flow->producer().accept(
        lane, {{{11}, 2}, {0, 4}, 0, bytes}).accepted());
    ASSERT_TRUE(flow->producer().flush(lane).accepted());
    ASSERT_EQ(
        flow->producer().publish({
            asyncdownload::flow::ControlPacketKind::range_complete,
            {{11}, 2},
            4
        }).code,
        asyncdownload::flow::PacketPublishCode::published);
    ASSERT_FALSE(flow->producer().close());

    asyncdownload::flow::PacketLease packet;
    ASSERT_EQ(
        flow->consumer().receive(packet, std::chrono::milliseconds(1)).code,
        asyncdownload::flow::PacketReceiveCode::packet);
    const auto data_sequence = packet.sequence();
    packet.complete();
    ASSERT_EQ(
        flow->consumer().receive(packet, std::chrono::milliseconds(1)).code,
        asyncdownload::flow::PacketReceiveCode::packet);
    const auto control_sequence = packet.sequence();
    packet.complete();
    EXPECT_GT(control_sequence, data_sequence);
    EXPECT_EQ(
        flow->consumer().receive(packet, std::chrono::milliseconds(1)).code,
        asyncdownload::flow::PacketReceiveCode::closed);
}

TEST(PacketFlowTest, CloseWakesTimedConsumer) {
    asyncdownload::telemetry::TelemetrySession telemetry;
    auto flow = make_flow(telemetry);
    std::latch ready(2);
    auto received = std::async(std::launch::async, [&]() {
        asyncdownload::flow::PacketLease packet;
        ready.arrive_and_wait();
        return flow->consumer().receive(
            packet, std::chrono::seconds(5));
    });
    ready.arrive_and_wait();

    ASSERT_FALSE(flow->producer().close());

    ASSERT_EQ(
        received.wait_for(std::chrono::seconds(1)),
        std::future_status::ready);
    const auto result = received.get();
    EXPECT_EQ(result.code, asyncdownload::flow::PacketReceiveCode::closed);
    EXPECT_FALSE(result.error);
}

TEST(PacketFlowTest, RejectsPublicationAfterClose) {
    asyncdownload::telemetry::TelemetrySession telemetry;
    auto flow = make_flow(telemetry);
    ASSERT_FALSE(flow->producer().close());

    const auto published = flow->producer().publish({
        asyncdownload::flow::ControlPacketKind::range_complete,
        {{3}, 1},
        4
    });

    EXPECT_EQ(published.code, asyncdownload::flow::PacketPublishCode::closed);
    EXPECT_FALSE(published.error);
    asyncdownload::flow::PacketLease packet;
    EXPECT_EQ(
        flow->consumer().receive(packet, std::chrono::milliseconds(1)).code,
        asyncdownload::flow::PacketReceiveCode::closed);
}

TEST(PacketFlowTest, RejectsRangeCompleteWhileRangeDraftRemains) {
    asyncdownload::telemetry::TelemetrySession telemetry;
    auto flow = make_flow(telemetry);
    asyncdownload::flow::ProducerLane lane;
    ASSERT_FALSE(flow->producer().open_lane(lane));
    const std::array<std::uint8_t, 4> bytes{1, 2, 3, 4};
    ASSERT_TRUE(flow->producer().accept(
        lane, {{{3}, 4}, {0, 8}, 0, bytes}).accepted());

    const auto published = flow->producer().publish({
        asyncdownload::flow::ControlPacketKind::range_complete,
        {{3}, 4},
        8
    });

    EXPECT_EQ(published.code, asyncdownload::flow::PacketPublishCode::failed);
    EXPECT_TRUE(published.error);
    EXPECT_EQ(
        flow->producer().snapshot().state,
        asyncdownload::flow::PacketFlowState::failed);
    ASSERT_FALSE(flow->producer().discard(lane));
}

TEST(PacketFlowTest, PreservesDependencyNeutralRangeValuesByValue) {
    const asyncdownload::range::LeaseId lease{
        asyncdownload::range::RangeId{7},
        9
    };
    const asyncdownload::range::ByteSpan span{1024, 2048};
    const asyncdownload::range::CompletionId completion{
        asyncdownload::range::RangeId{7},
        9
    };

    EXPECT_EQ(lease, (asyncdownload::range::LeaseId{
        asyncdownload::range::RangeId{7}, 9}));
    EXPECT_EQ(span, (asyncdownload::range::ByteSpan{1024, 2048}));
    EXPECT_EQ(completion, (asyncdownload::range::CompletionId{
        asyncdownload::range::RangeId{7}, 9}));
}

TEST(PacketFlowTest, RejectsZeroPacketBudgetWithoutCreatingFlow) {
    asyncdownload::telemetry::TelemetrySession telemetry;
    std::unique_ptr<asyncdownload::flow::PacketFlow> flow;
    auto policy = test_policy();
    policy.packet_budget = 0;

    const auto error = asyncdownload::flow::PacketFlow::create(
        policy, telemetry, flow);

    EXPECT_EQ(error, std::make_error_code(std::errc::invalid_argument));
    EXPECT_EQ(flow, nullptr);
}

TEST(
    PacketFlowTest,
    ConcurrentProducerConsumerPreservesOneMillionSequencesAcrossBudgets) {
    constexpr std::size_t packet_count = 333'334;
    for (const auto budget : {1U, 2U, 33U}) {
        asyncdownload::telemetry::TelemetrySession telemetry;
        std::promise<asyncdownload::flow::PacketFlow*> ready;
        std::latch consumer_done(1);
        std::atomic<std::size_t> released_credits{0};
        auto producer = std::async(std::launch::async, [&]() {
            std::unique_ptr<asyncdownload::flow::PacketFlow> flow;
            auto policy = test_policy();
            policy.packet_budget = budget;
            auto error = asyncdownload::flow::PacketFlow::create(
                policy, telemetry, flow);
            if (error) {
                ready.set_value(nullptr);
                return std::pair{false,
                                 asyncdownload::flow::PacketFlowSnapshot{}};
            }
            asyncdownload::flow::ProducerLane lane;
            error = flow->producer().open_lane(lane);
            if (error) {
                ready.set_value(nullptr);
                return std::pair{false,
                                 flow->producer().snapshot()};
            }
            ready.set_value(flow.get());
            bool valid = true;
            for (std::size_t index = 0;
                 index < packet_count && valid;
                 ++index) {
                const std::array<std::uint8_t, 1> payload{
                    static_cast<std::uint8_t>(index % 251)
                };
                const auto admission = flow->producer().accept(
                    lane,
                    {
                        {{8}, 1},
                        {0, static_cast<std::int64_t>(packet_count)},
                        static_cast<std::int64_t>(index),
                        payload
                    });
                valid = admission.accepted() &&
                    admission.consumed_bytes == payload.size();
                while (valid) {
                    const auto observed =
                        released_credits.load(std::memory_order_acquire);
                    const auto flushed = flow->producer().flush(lane);
                    if (flushed.accepted()) {
                        break;
                    }
                    if (flushed.code !=
                        asyncdownload::flow::PacketAdmissionCode::
                            packet_budget_exhausted) {
                        valid = false;
                        break;
                    }
                    released_credits.wait(
                        observed, std::memory_order_acquire);
                }
            }
            if (valid) {
                error = flow->producer().close();
                valid = !error;
            }
            consumer_done.wait();
            return std::pair{
                valid,
                flow->producer().snapshot()
            };
        });

        auto* flow = ready.get_future().get();
        ASSERT_NE(flow, nullptr);
        asyncdownload::flow::PacketLease lease;
        std::map<
            asyncdownload::flow::PacketSequence,
            asyncdownload::flow::PacketLease> held;
        std::size_t observed_packets = 0;
        std::uint64_t expected_checksum = 0;
        std::uint64_t actual_checksum = 0;
        std::uint32_t random_state = 0x9E3779B9U;
        bool valid = true;
        std::size_t consecutive_timeouts = 0;
        while (valid) {
            const auto received = flow->consumer().receive(
                lease, std::chrono::milliseconds(1));
            if (received.code ==
                asyncdownload::flow::PacketReceiveCode::timeout) {
                ++consecutive_timeouts;
                valid = consecutive_timeouts < 10'000;
                continue;
            }
            consecutive_timeouts = 0;
            if (received.code ==
                asyncdownload::flow::PacketReceiveCode::closed) {
                break;
            }
            if (received.code !=
                    asyncdownload::flow::PacketReceiveCode::packet ||
                lease.kind() !=
                    asyncdownload::flow::PacketKind::data ||
                lease.data() == nullptr) {
                valid = false;
                break;
            }
            const auto* data = lease.data();
            const auto expected_value = static_cast<std::uint8_t>(
                observed_packets % 251);
            valid =
                lease.sequence() == observed_packets + 1 &&
                data->offset ==
                    static_cast<std::int64_t>(observed_packets) &&
                data->payload.size() == 1 &&
                data->payload.front() == expected_value;
            expected_checksum += expected_value;
            actual_checksum += data->payload.front();
            ++observed_packets;
            released_credits.fetch_add(1, std::memory_order_acq_rel);
            released_credits.notify_one();

            random_state ^= random_state << 13;
            random_state ^= random_state >> 17;
            random_state ^= random_state << 5;
            if ((random_state & 7U) == 0U) {
                valid = !lease.account_reorder_node();
                held.emplace(lease.sequence(), std::move(lease));
                if (held.size() > 32) {
                    held.begin()->second.complete();
                    held.erase(held.begin());
                }
            } else {
                lease.complete();
            }
        }
        for (auto& [sequence, packet] : held) {
            static_cast<void>(sequence);
            packet.complete();
        }
        consumer_done.count_down();
        const auto [producer_valid, snapshot] = producer.get();

        EXPECT_TRUE(valid);
        EXPECT_TRUE(producer_valid);
        EXPECT_EQ(observed_packets, packet_count);
        EXPECT_EQ(actual_checksum, expected_checksum);
        EXPECT_EQ(
            snapshot.state,
            asyncdownload::flow::PacketFlowState::closed);
        EXPECT_EQ(snapshot.queued_packets, 0U);
        EXPECT_EQ(snapshot.accounted_bytes, 0U);
    }
}

}

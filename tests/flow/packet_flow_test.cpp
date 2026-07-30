#include <gtest/gtest.h>

#include "asyncdownload/telemetry/telemetry_session.hpp"
#include "core/constants.hpp"
#include "core/memory_accounting.hpp"
#include "flow/packet_flow.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
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
    asyncdownload::telemetry::TelemetrySession& telemetry) {
    std::unique_ptr<asyncdownload::flow::PacketFlow> flow;
    EXPECT_FALSE(asyncdownload::flow::PacketFlow::create(
        test_policy(), telemetry, flow));
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

TEST(PacketFlowTest, AcceptsContiguousChunksIntoOneLaneDraft) {
    asyncdownload::core::global_memory_accounting().reset();
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
    asyncdownload::core::global_memory_accounting().reset();
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
    ASSERT_FALSE(flow->producer().discard(lane));
    ASSERT_FALSE(flow->producer().close());
    asyncdownload::flow::PacketLease packet;
    EXPECT_EQ(
        flow->consumer().receive(packet, std::chrono::milliseconds(1)).code,
        asyncdownload::flow::PacketReceiveCode::closed);
}

TEST(PacketFlowTest, PublishesAtCurrentSixtyFourKibAggregationShape) {
    asyncdownload::core::global_memory_accounting().reset();
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
    asyncdownload::core::global_memory_accounting().reset();
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
    ASSERT_FALSE(flow->producer().close());
    EXPECT_EQ(
        flow->consumer().receive(packet, std::chrono::milliseconds(1)).code,
        asyncdownload::flow::PacketReceiveCode::closed);
}

TEST(PacketFlowTest, PublishesRangeCompleteAfterAllRangeData) {
    asyncdownload::core::global_memory_accounting().reset();
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

}

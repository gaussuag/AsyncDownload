#include <gtest/gtest.h>

#include "asyncdownload/telemetry/telemetry_session.hpp"
#include "flow/packet_flow.hpp"

#include <chrono>
#include <memory>

namespace {

asyncdownload::download::FlowControlPolicy test_policy() {
    return {
        4,
        1024 * 1024,
        512 * 1024
    };
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

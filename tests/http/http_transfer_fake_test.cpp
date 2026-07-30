#include "deterministic_http_transfer.hpp"

#include "asyncdownload/error.hpp"
#include "asyncdownload/telemetry/telemetry_session.hpp"
#include "flow/packet_flow.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace {

using asyncdownload::http::HttpCancelKind;
using asyncdownload::http::HttpCancelRequest;
using asyncdownload::http::HttpFailure;
using asyncdownload::http::HttpFailureReason;
using asyncdownload::http::HttpLeaseFailed;
using asyncdownload::http::HttpLeaseSucceeded;
using asyncdownload::http::HttpObjectFacts;
using asyncdownload::http::HttpPollCode;
using asyncdownload::http::HttpProbeRequest;
using asyncdownload::http::HttpProbeResult;
using asyncdownload::http::HttpSessionConfig;
using asyncdownload::http::HttpStartCode;
using asyncdownload::http::testing::DeterministicHttpTransferPort;
using asyncdownload::http::testing::FakeDelivery;
using asyncdownload::http::testing::FakeProbeStep;
using asyncdownload::http::testing::FakeTransferScript;

asyncdownload::range::RangeLease make_lease(
    const std::uint64_t range,
    const std::uint64_t generation,
    const std::int64_t begin,
    const std::int64_t end) {
    return {
        {{range}, generation},
        {begin, end},
        true
    };
}

FakeTransferScript make_script(
    const asyncdownload::range::RangeLease lease,
    std::vector<std::uint8_t> bytes,
    const std::optional<HttpFailure> failure =
        std::nullopt) {
    return {
        lease,
        {
            FakeDelivery{
                lease.bytes.begin,
                std::move(bytes)
            }
        },
        failure,
        206
    };
}

class HttpTransferFakeTest : public ::testing::Test {
protected:
    void SetUp() override {
        recreate_flow(4);
    }

    void recreate_flow(
        const std::size_t packet_budget) {
        flow_.reset();
        telemetry_.record_task_started();
        const asyncdownload::download::FlowControlPolicy policy{
            packet_budget,
            1024 * 1024,
            512 * 1024
        };
        ASSERT_FALSE(
            asyncdownload::flow::PacketFlow::create(
                policy,
                telemetry_,
                flow_));
    }

    std::unique_ptr<
        asyncdownload::http::HttpTransferSession>
    open_session(
        DeterministicHttpTransferPort& port,
        const std::size_t capacity = 2) {
        auto opened = port.open_session(
            {"https://example.test/object", 131072, capacity},
            flow_->producer(),
            telemetry_);
        EXPECT_FALSE(opened.failure.error);
        EXPECT_NE(opened.session, nullptr);
        return std::move(opened.session);
    }

    std::vector<std::uint8_t> receive_data() {
        asyncdownload::flow::PacketLease packet;
        const auto received =
            flow_->consumer().receive(
                packet,
                std::chrono::microseconds(0));
        EXPECT_EQ(
            received.code,
            asyncdownload::flow::PacketReceiveCode::packet);
        EXPECT_NE(packet.data(), nullptr);
        std::vector<std::uint8_t> bytes;
        if (packet.data() != nullptr) {
            bytes = packet.data()->payload;
        }
        packet.complete();
        return bytes;
    }

    asyncdownload::telemetry::TelemetrySession telemetry_;
    std::unique_ptr<asyncdownload::flow::PacketFlow> flow_;
};

TEST_F(HttpTransferFakeTest, MatchesProbeRequestExactly) {
    const HttpProbeResult expected{
        HttpObjectFacts{
            128,
            true,
            "\"etag\"",
            "date"
        },
        {},
        200
    };
    DeterministicHttpTransferPort port(
        {
            FakeProbeStep{
                {"https://example.test/object"},
                expected
            }
        },
        {});

    const auto mismatch =
        port.probe({"https://example.test/other"});
    EXPECT_FALSE(mismatch.ok());
    EXPECT_EQ(
        mismatch.failure.reason,
        HttpFailureReason::protocol_order_invalid);
    EXPECT_EQ(port.consumed_probe_steps(), 0U);

    const auto matched =
        port.probe({"https://example.test/object"});
    ASSERT_TRUE(matched.ok());
    EXPECT_EQ(matched.facts, expected.facts);
    EXPECT_EQ(port.consumed_probe_steps(), 1U);
}

TEST_F(HttpTransferFakeTest, StartsOnlyExpectedLease) {
    const auto expected = make_lease(1, 1, 0, 4);
    DeterministicHttpTransferPort port(
        {},
        {make_script(expected, {1, 2, 3, 4})});
    auto session = open_session(port);

    const auto wrong =
        session->start(make_lease(2, 1, 0, 4));
    EXPECT_EQ(wrong.code, HttpStartCode::failed);
    EXPECT_FALSE(wrong.token.has_value());
    EXPECT_EQ(port.consumed_transfer_scripts(), 0U);
}

TEST_F(HttpTransferFakeTest, ReturnsNoCapacityWithoutConsumingScript) {
    const auto first = make_lease(1, 1, 0, 4);
    const auto second = make_lease(2, 1, 4, 8);
    DeterministicHttpTransferPort port(
        {},
        {
            make_script(first, {1, 2, 3, 4}),
            make_script(second, {5, 6, 7, 8})
        });
    auto session = open_session(port, 1);

    EXPECT_EQ(
        session->start(first).code,
        HttpStartCode::started);
    EXPECT_EQ(
        session->start(second).code,
        HttpStartCode::no_capacity);
    EXPECT_EQ(port.consumed_transfer_scripts(), 1U);
}

TEST_F(HttpTransferFakeTest, DeliversAtMostOneEventPerPoll) {
    const auto first = make_lease(1, 1, 0, 1);
    const auto second = make_lease(2, 1, 1, 2);
    DeterministicHttpTransferPort port(
        {},
        {
            make_script(first, {1}),
            make_script(second, {2})
        });
    auto session = open_session(port, 2);
    ASSERT_EQ(
        session->start(first).code,
        HttpStartCode::started);
    ASSERT_EQ(
        session->start(second).code,
        HttpStartCode::started);

    EXPECT_EQ(
        session->poll(std::chrono::milliseconds(0)).code,
        HttpPollCode::timed_out);
    const auto first_event =
        session->poll(std::chrono::milliseconds(0));
    ASSERT_EQ(first_event.code, HttpPollCode::event);
    ASSERT_TRUE(first_event.event.has_value());
    EXPECT_TRUE(std::holds_alternative<HttpLeaseSucceeded>(
        *first_event.event));
    EXPECT_EQ(
        session->poll(std::chrono::milliseconds(0)).code,
        HttpPollCode::timed_out);
    const auto second_event =
        session->poll(std::chrono::milliseconds(0));
    EXPECT_EQ(second_event.code, HttpPollCode::event);
}

TEST_F(HttpTransferFakeTest, FeedsBytesThroughRealPacketProducer) {
    const auto lease = make_lease(1, 1, 0, 4);
    DeterministicHttpTransferPort port(
        {},
        {make_script(lease, {1, 2, 3, 4})});
    auto session = open_session(port);
    ASSERT_EQ(
        session->start(lease).code,
        HttpStartCode::started);

    EXPECT_EQ(
        session->poll(std::chrono::milliseconds(0)).code,
        HttpPollCode::timed_out);
    EXPECT_EQ(
        session->poll(std::chrono::milliseconds(0)).code,
        HttpPollCode::event);
    EXPECT_EQ(
        receive_data(),
        (std::vector<std::uint8_t>{1, 2, 3, 4}));
}

TEST_F(HttpTransferFakeTest, ReplaysSameDeliveryAfterPacketPause) {
    recreate_flow(1);
    const auto lease =
        make_lease(1, 1, 0, 160 * 1024);
    std::vector<std::uint8_t> first(32 * 1024, 0x11);
    std::vector<std::uint8_t> second(32 * 1024, 0x22);
    std::vector<std::uint8_t> third(32 * 1024, 0x33);
    std::vector<std::uint8_t> fourth(32 * 1024, 0x44);
    std::vector<std::uint8_t> fifth(32 * 1024, 0x55);
    DeterministicHttpTransferPort port(
        {},
        {
            {
                lease,
                {
                    {0, first},
                    {32 * 1024, second},
                    {64 * 1024, third},
                    {96 * 1024, fourth},
                    {128 * 1024, fifth}
                },
                std::nullopt,
                206
            }
        });
    auto session = open_session(port, 1);
    ASSERT_EQ(
        session->start(lease).code,
        HttpStartCode::started);

    EXPECT_EQ(
        session->poll(std::chrono::milliseconds(0)).code,
        HttpPollCode::timed_out);
    EXPECT_EQ(
        session->poll(std::chrono::milliseconds(0)).code,
        HttpPollCode::timed_out);
    EXPECT_EQ(
        session->poll(std::chrono::milliseconds(0)).code,
        HttpPollCode::timed_out);
    EXPECT_EQ(
        session->poll(std::chrono::milliseconds(0)).code,
        HttpPollCode::timed_out);
    EXPECT_EQ(
        session->poll(std::chrono::milliseconds(0)).code,
        HttpPollCode::timed_out);
    EXPECT_EQ(
        session->snapshot().active_transfers,
        1U);
    auto first_packet = first;
    first_packet.insert(
        first_packet.end(),
        second.begin(),
        second.end());
    EXPECT_EQ(receive_data(), first_packet);
    EXPECT_EQ(
        session->poll(std::chrono::milliseconds(0)).code,
        HttpPollCode::timed_out);
    auto second_packet = third;
    second_packet.insert(
        second_packet.end(),
        fourth.begin(),
        fourth.end());
    EXPECT_EQ(receive_data(), second_packet);
    EXPECT_EQ(
        session->poll(std::chrono::milliseconds(0)).code,
        HttpPollCode::event);
    EXPECT_EQ(receive_data(), fifth);
}

TEST_F(HttpTransferFakeTest, GapPauseBlocksDelivery) {
    const auto lease = make_lease(1, 1, 0, 4);
    DeterministicHttpTransferPort port(
        {},
        {make_script(lease, {1, 2, 3, 4})});
    auto session = open_session(port);
    const auto started = session->start(lease);
    ASSERT_EQ(started.code, HttpStartCode::started);
    ASSERT_TRUE(started.token.has_value());

    EXPECT_FALSE(
        session->set_gap_paused(*started.token, true));
    EXPECT_EQ(
        session->poll(std::chrono::milliseconds(0)).code,
        HttpPollCode::timed_out);
    EXPECT_EQ(
        flow_->producer().snapshot().queued_packets,
        0U);
    EXPECT_FALSE(
        session->set_gap_paused(*started.token, false));
    EXPECT_EQ(
        session->poll(std::chrono::milliseconds(0)).code,
        HttpPollCode::timed_out);
}

TEST_F(HttpTransferFakeTest, CountsGapOnlyOnZeroToOne) {
    const auto lease = make_lease(1, 1, 0, 4);
    DeterministicHttpTransferPort port(
        {},
        {make_script(lease, {1, 2, 3, 4})});
    auto session = open_session(port);
    const auto started = session->start(lease);
    ASSERT_TRUE(started.token.has_value());

    EXPECT_FALSE(
        session->set_gap_paused(*started.token, true));
    EXPECT_FALSE(
        session->set_gap_paused(*started.token, true));
    EXPECT_FALSE(
        session->set_gap_paused(*started.token, false));
    EXPECT_FALSE(
        session->set_gap_paused(*started.token, true));
    EXPECT_EQ(
        telemetry_.final_summary().total_pause_count,
        2U);
}

TEST_F(HttpTransferFakeTest, RejectsStaleTransferToken) {
    const auto lease = make_lease(1, 1, 0, 1);
    DeterministicHttpTransferPort port(
        {},
        {make_script(lease, {1})});
    auto session = open_session(port);
    const auto started = session->start(lease);
    ASSERT_TRUE(started.token.has_value());
    auto stale = *started.token;
    ++stale.slot_generation;

    EXPECT_TRUE(session->set_gap_paused(stale, true));
    EXPECT_EQ(
        telemetry_.final_summary().total_pause_count,
        0U);
}

TEST_F(HttpTransferFakeTest, CancelProducesOneFailurePerActiveLease) {
    const auto first = make_lease(1, 1, 0, 1);
    const auto second = make_lease(2, 1, 1, 2);
    DeterministicHttpTransferPort port(
        {},
        {
            make_script(first, {1}),
            make_script(second, {2})
        });
    auto session = open_session(port, 2);
    ASSERT_EQ(
        session->start(first).code,
        HttpStartCode::started);
    ASSERT_EQ(
        session->start(second).code,
        HttpStartCode::started);
    ASSERT_FALSE(
        session->cancel(
            {HttpCancelKind::task_cancelled, {}}));

    const auto first_result =
        session->poll(std::chrono::milliseconds(0));
    const auto second_result =
        session->poll(std::chrono::milliseconds(0));
    ASSERT_EQ(first_result.code, HttpPollCode::event);
    ASSERT_EQ(second_result.code, HttpPollCode::event);
    EXPECT_TRUE(std::holds_alternative<HttpLeaseFailed>(
        *first_result.event));
    EXPECT_TRUE(std::holds_alternative<HttpLeaseFailed>(
        *second_result.event));
    EXPECT_EQ(
        session->poll(std::chrono::milliseconds(0)).code,
        HttpPollCode::idle);
}

TEST_F(HttpTransferFakeTest, NeverRetriesFailedLease) {
    const auto lease = make_lease(1, 1, 0, 1);
    const HttpFailure failure{
        HttpFailureReason::transport_failed,
        asyncdownload::make_error_code(
            asyncdownload::DownloadErrc::http_transfer_failed)
    };
    DeterministicHttpTransferPort port(
        {},
        {make_script(lease, {}, failure)});
    auto session = open_session(port);
    ASSERT_EQ(
        session->start(lease).code,
        HttpStartCode::started);

    const auto event =
        session->poll(std::chrono::milliseconds(0));
    ASSERT_EQ(event.code, HttpPollCode::event);
    ASSERT_TRUE(event.event.has_value());
    EXPECT_TRUE(std::holds_alternative<HttpLeaseFailed>(
        *event.event));
    EXPECT_EQ(port.consumed_transfer_scripts(), 1U);
    EXPECT_EQ(
        session->start(lease).code,
        HttpStartCode::failed);
}

TEST_F(HttpTransferFakeTest, CloseRejectsPendingEvent) {
    const auto lease = make_lease(1, 1, 0, 1);
    DeterministicHttpTransferPort port(
        {},
        {make_script(lease, {1})});
    auto session = open_session(port);
    ASSERT_EQ(
        session->start(lease).code,
        HttpStartCode::started);
    ASSERT_FALSE(
        session->cancel(
            HttpCancelRequest{
                HttpCancelKind::task_cancelled,
                {}
            }));

    EXPECT_TRUE(session->close());
    EXPECT_EQ(
        session->snapshot().pending_events,
        1U);
    EXPECT_EQ(
        session->poll(std::chrono::milliseconds(0)).code,
        HttpPollCode::event);
    EXPECT_FALSE(session->close());
}

}

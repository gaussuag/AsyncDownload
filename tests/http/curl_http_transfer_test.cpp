#include "http/http_transfer.hpp"

#include "asyncdownload/telemetry/telemetry_session.hpp"
#include "flow/packet_flow.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <memory>

namespace {

class CurlHttpTransferTest : public ::testing::Test {
protected:
    void SetUp() override {
        telemetry_.record_task_started();
        const asyncdownload::download::FlowControlPolicy policy{
            8,
            1024 * 1024,
            512 * 1024
        };
        ASSERT_FALSE(
            asyncdownload::flow::PacketFlow::create(
                policy,
                telemetry_,
                flow_));
        ASSERT_FALSE(
            asyncdownload::http::
                create_curl_http_transfer_port(port_));
        ASSERT_NE(port_, nullptr);
    }

    std::unique_ptr<
        asyncdownload::http::HttpTransferSession>
    open_session(
        const std::size_t capacity = 3) {
        auto opened = port_->open_session(
            {
                "http://127.0.0.1:1/object",
                1024,
                capacity
            },
            flow_->producer(),
            telemetry_);
        EXPECT_FALSE(opened.failure.error);
        EXPECT_NE(opened.session, nullptr);
        return std::move(opened.session);
    }

    asyncdownload::range::RangeLease lease(
        const std::uint64_t range = 1,
        const std::uint64_t generation = 1) {
        return {
            {{range}, generation},
            {0, 1024},
            true
        };
    }

    asyncdownload::telemetry::TelemetrySession
        telemetry_;
    std::unique_ptr<asyncdownload::flow::PacketFlow>
        flow_;
    std::unique_ptr<
        asyncdownload::http::HttpTransferPort>
        port_;
};

TEST_F(CurlHttpTransferTest, CreatesFixedStableSlots) {
    auto session = open_session(3);
    const auto snapshot = session->snapshot();
    EXPECT_EQ(
        snapshot.state,
        asyncdownload::http::HttpSessionState::open);
    EXPECT_EQ(snapshot.active_transfers, 0U);
    EXPECT_EQ(snapshot.available_slots, 3U);
    EXPECT_EQ(snapshot.pending_events, 0U);
    EXPECT_FALSE(session->close());
}

TEST_F(
    CurlHttpTransferTest,
    RejectsWrongThreadWithoutCurlSideEffect) {
    auto session = open_session(2);
    auto result = std::async(
        std::launch::async,
        [&session, this]() {
            return session->start(lease());
        });

    const auto wrong_thread = result.get();
    EXPECT_EQ(
        wrong_thread.code,
        asyncdownload::http::HttpStartCode::failed);
    EXPECT_EQ(
        wrong_thread.failure.reason,
        asyncdownload::http::
            HttpFailureReason::protocol_order_invalid);
    const auto snapshot = session->snapshot();
    EXPECT_EQ(snapshot.active_transfers, 0U);
    EXPECT_EQ(snapshot.available_slots, 2U);
    EXPECT_FALSE(session->close());
}

TEST_F(
    CurlHttpTransferTest,
    PendingEventMakesAvailableSlotsZero) {
    auto session = open_session(2);
    const auto started =
        session->start(lease());
    ASSERT_EQ(
        started.code,
        asyncdownload::http::HttpStartCode::started);
    ASSERT_FALSE(
        session->cancel(
            {
                asyncdownload::http::
                    HttpCancelKind::task_cancelled,
                {}
            }));

    const auto pending = session->snapshot();
    EXPECT_EQ(pending.active_transfers, 0U);
    EXPECT_EQ(pending.pending_events, 1U);
    EXPECT_EQ(pending.available_slots, 0U);
    EXPECT_EQ(
        session->start(lease(2, 1)).code,
        asyncdownload::http::
            HttpStartCode::no_capacity);
    EXPECT_TRUE(session->close());

    const auto event =
        session->poll(std::chrono::milliseconds(0));
    EXPECT_EQ(
        event.code,
        asyncdownload::http::HttpPollCode::event);
    EXPECT_FALSE(session->close());
}

TEST_F(
    CurlHttpTransferTest,
    StartFailureReturnsSynchronouslyWithoutPendingEvent) {
    auto session = open_session(2);
    auto invalid = lease();
    invalid.bytes.end = invalid.bytes.begin;

    const auto start = session->start(invalid);
    EXPECT_EQ(
        start.code,
        asyncdownload::http::HttpStartCode::failed);
    EXPECT_FALSE(start.token.has_value());
    EXPECT_EQ(
        start.failure.reason,
        asyncdownload::http::
            HttpFailureReason::protocol_order_invalid);
    const auto snapshot = session->snapshot();
    EXPECT_EQ(snapshot.active_transfers, 0U);
    EXPECT_EQ(snapshot.pending_events, 0U);
    EXPECT_EQ(snapshot.available_slots, 2U);
    EXPECT_FALSE(session->close());
}

}

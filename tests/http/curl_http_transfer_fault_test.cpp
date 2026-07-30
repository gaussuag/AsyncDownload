#include "http/curl_runtime_adapter.hpp"
#include "http/http_transfer.hpp"

#include "asyncdownload/error.hpp"
#include "asyncdownload/telemetry/telemetry_session.hpp"
#include "flow/packet_flow.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>

namespace {

class CurlHttpTransferFaultTest :
    public ::testing::Test {
protected:
    void SetUp() override {
        fault_plan().reset();
        telemetry_.record_task_started();
        const asyncdownload::download::
            FlowControlPolicy policy{
                8,
                1024 * 1024,
                512 * 1024
            };
        ASSERT_FALSE(
            asyncdownload::flow::
                PacketFlow::create(
                    policy,
                    telemetry_,
                    flow_));
        ASSERT_FALSE(
            asyncdownload::http::
                create_curl_http_transfer_port(
                    port_));
        ASSERT_NE(port_, nullptr);
    }

    void TearDown() override {
        fault_plan().reset();
    }

    asyncdownload::http::detail::
        CurlRuntimeFaultPlan& fault_plan() {
        return asyncdownload::http::detail::
            curl_runtime_fault_plan();
    }

    std::unique_ptr<
        asyncdownload::http::
            HttpTransferSession>
    open_session(
        const std::size_t capacity = 1) {
        auto opened = port_->open_session(
            {
                "http://127.0.0.1:1/object",
                4,
                capacity
            },
            flow_->producer(),
            telemetry_);
        EXPECT_FALSE(opened.failure.error);
        EXPECT_NE(opened.session, nullptr);
        return std::move(opened.session);
    }

    asyncdownload::range::RangeLease lease() {
        return {
            {{1}, 1},
            {0, 4},
            true
        };
    }

    asyncdownload::http::HttpPollResult
    poll_until_event(
        asyncdownload::http::HttpTransferSession& session) {
        const auto deadline =
            std::chrono::steady_clock::now() +
            std::chrono::seconds(2);
        asyncdownload::http::HttpPollResult result{};
        while (std::chrono::steady_clock::now() <
               deadline) {
            result = session.poll(
                std::chrono::milliseconds(20));
            if (result.code ==
                    asyncdownload::http::
                        HttpPollCode::event ||
                result.code ==
                    asyncdownload::http::
                        HttpPollCode::failed) {
                return result;
            }
        }
        return result;
    }

    void cleanup(
        asyncdownload::http::
            HttpTransferSession& session) {
        fault_plan().fail_next_easy_pause.store(
            false);
        fault_plan().fail_next_remove_handle.store(
            false);
        fault_plan().fail_next_multi_cleanup.store(
            false);
        const auto snapshot = session.snapshot();
        if (snapshot.active_transfers != 0) {
            static_cast<void>(
                session.cancel({
                    asyncdownload::http::
                        HttpCancelKind::task_cancelled,
                    {}
                }));
        }
        while (session.snapshot().
                   pending_events != 0) {
            static_cast<void>(
                session.poll(
                    std::chrono::milliseconds(0)));
        }
        static_cast<void>(session.close());
    }

    asyncdownload::telemetry::
        TelemetrySession telemetry_;
    std::unique_ptr<
        asyncdownload::flow::PacketFlow> flow_;
    std::unique_ptr<
        asyncdownload::http::HttpTransferPort> port_;
};

TEST_F(
    CurlHttpTransferFaultTest,
    ProbeStopsAtFirstOptionFailure) {
    fault_plan().fail_next_easy_option.store(true);

    const auto result =
        port_->probe({
            "http://127.0.0.1:1/object"
        });

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(
        result.failure.reason,
        asyncdownload::http::
            HttpFailureReason::easy_option_failed);
    EXPECT_EQ(
        result.failure.error,
        asyncdownload::make_error_code(
            asyncdownload::DownloadErrc::
                http_probe_failed));
}

TEST_F(
    CurlHttpTransferFaultTest,
    ProbeReportsEasyInitFailure) {
    fault_plan().
        fail_next_easy_init.store(true);

    const auto result =
        port_->probe({
            "http://127.0.0.1:1/object"
        });

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(
        result.failure.reason,
        asyncdownload::http::
            HttpFailureReason::easy_init_failed);
    EXPECT_EQ(
        result.failure.error,
        asyncdownload::make_error_code(
            asyncdownload::DownloadErrc::
                http_init_failed));
}

TEST_F(
    CurlHttpTransferFaultTest,
    ProbeReportsHeaderAllocationFailure) {
    fault_plan().
        fail_next_slist_append.store(true);

    const auto result =
        port_->probe({
            "http://127.0.0.1:1/object"
        });

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(
        result.failure.reason,
        asyncdownload::http::
            HttpFailureReason::allocation_failed);
}

TEST_F(
    CurlHttpTransferFaultTest,
    ProbeReportsGetInfoFailure) {
    fault_plan().
        skip_next_easy_perform.store(true);
    fault_plan().fail_next_easy_info.store(true);

    const auto result =
        port_->probe({
            "http://127.0.0.1:1/object"
        });

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(
        result.failure.reason,
        asyncdownload::http::
            HttpFailureReason::easy_info_failed);
    EXPECT_EQ(
        result.failure.error,
        asyncdownload::make_error_code(
            asyncdownload::DownloadErrc::
                http_probe_failed));
}

TEST_F(
    CurlHttpTransferFaultTest,
    OpenStopsAtFirstMultiOptionFailure) {
    fault_plan().
        fail_next_multi_option.store(true);

    const auto opened = port_->open_session(
        {
            "http://127.0.0.1:1/object",
            4,
            1
        },
        flow_->producer(),
        telemetry_);

    EXPECT_EQ(opened.session, nullptr);
    EXPECT_EQ(
        opened.failure.reason,
        asyncdownload::http::
            HttpFailureReason::multi_option_failed);
}

TEST_F(
    CurlHttpTransferFaultTest,
    OpenReportsMultiInitFailure) {
    fault_plan().
        fail_next_multi_init.store(true);

    const auto opened = port_->open_session(
        {
            "http://127.0.0.1:1/object",
            4,
            1
        },
        flow_->producer(),
        telemetry_);

    EXPECT_EQ(opened.session, nullptr);
    EXPECT_EQ(
        opened.failure.reason,
        asyncdownload::http::
            HttpFailureReason::multi_init_failed);
}

TEST_F(
    CurlHttpTransferFaultTest,
    OpenReportsHeaderAllocationFailure) {
    fault_plan().
        fail_next_slist_append.store(true);

    const auto opened = port_->open_session(
        {
            "http://127.0.0.1:1/object",
            4,
            1
        },
        flow_->producer(),
        telemetry_);

    EXPECT_EQ(opened.session, nullptr);
    EXPECT_EQ(
        opened.failure.reason,
        asyncdownload::http::
            HttpFailureReason::allocation_failed);
}

TEST_F(
    CurlHttpTransferFaultTest,
    OpenReportsEasySlotInitFailure) {
    fault_plan().
        fail_next_easy_init.store(true);

    const auto opened = port_->open_session(
        {
            "http://127.0.0.1:1/object",
            4,
            1
        },
        flow_->producer(),
        telemetry_);

    EXPECT_EQ(opened.session, nullptr);
    EXPECT_EQ(
        opened.failure.reason,
        asyncdownload::http::
            HttpFailureReason::easy_init_failed);
}

TEST_F(
    CurlHttpTransferFaultTest,
    StartStopsAtFirstEasyOptionFailure) {
    auto session = open_session();
    ASSERT_NE(session, nullptr);
    fault_plan().
        fail_next_easy_option.store(true);

    const auto result =
        session->start(lease());

    EXPECT_EQ(
        result.code,
        asyncdownload::http::
            HttpStartCode::failed);
    EXPECT_EQ(
        result.failure.reason,
        asyncdownload::http::
            HttpFailureReason::easy_option_failed);
    EXPECT_EQ(
        session->snapshot().active_transfers,
        0U);
    cleanup(*session);
}

TEST_F(
    CurlHttpTransferFaultTest,
    StartReportsAddHandleFailureSynchronously) {
    auto session = open_session();
    ASSERT_NE(session, nullptr);
    fault_plan().
        fail_next_add_handle.store(true);

    const auto result =
        session->start(lease());

    EXPECT_EQ(
        result.code,
        asyncdownload::http::
            HttpStartCode::failed);
    EXPECT_EQ(
        result.failure.reason,
        asyncdownload::http::
            HttpFailureReason::add_handle_failed);
    EXPECT_EQ(
        session->snapshot().pending_events,
        0U);
    cleanup(*session);
}

TEST_F(
    CurlHttpTransferFaultTest,
    PauseFailureStopsSession) {
    auto session = open_session();
    ASSERT_NE(session, nullptr);
    const auto started =
        session->start(lease());
    ASSERT_TRUE(started.token.has_value());
    ASSERT_FALSE(
        session->set_gap_paused(
            *started.token,
            true));
    fault_plan().
        fail_next_easy_pause.store(true);

    const auto result =
        session->poll(
            std::chrono::milliseconds(0));

    EXPECT_EQ(
        result.code,
        asyncdownload::http::
            HttpPollCode::failed);
    EXPECT_EQ(
        result.error,
        asyncdownload::make_error_code(
            asyncdownload::DownloadErrc::
                http_transfer_failed));
    cleanup(*session);
}

TEST_F(
    CurlHttpTransferFaultTest,
    MultiPerformFailureIsNeverRetried) {
    auto session = open_session();
    ASSERT_NE(session, nullptr);
    ASSERT_EQ(
        session->start(lease()).code,
        asyncdownload::http::
            HttpStartCode::started);
    fault_plan().
        fail_next_multi_perform.store(true);

    const auto first =
        session->poll(
            std::chrono::milliseconds(0));
    const auto second =
        session->poll(
            std::chrono::milliseconds(0));

    EXPECT_EQ(
        first.code,
        asyncdownload::http::
            HttpPollCode::failed);
    EXPECT_EQ(
        second.code,
        asyncdownload::http::
            HttpPollCode::failed);
    EXPECT_EQ(
        fault_plan().
            multi_perform_calls.load(),
        1U);
    cleanup(*session);
}

TEST_F(
    CurlHttpTransferFaultTest,
    MultiWaitFailureStopsSession) {
    auto session = open_session();
    ASSERT_NE(session, nullptr);
    ASSERT_EQ(
        session->start(lease()).code,
        asyncdownload::http::
            HttpStartCode::started);
    fault_plan().
        skip_next_multi_perform.store(true);
    fault_plan().
        fail_next_multi_wait.store(true);

    const auto result =
        session->poll(
            std::chrono::milliseconds(0));

    EXPECT_EQ(
        result.code,
        asyncdownload::http::
            HttpPollCode::failed);
    EXPECT_EQ(
        fault_plan().multi_wait_calls.load(),
        1U);
    cleanup(*session);
}

TEST_F(
    CurlHttpTransferFaultTest,
    CompletionReportsGetInfoFailureBeforeTransport) {
    auto session = open_session();
    ASSERT_NE(session, nullptr);
    ASSERT_EQ(
        session->start(lease()).code,
        asyncdownload::http::
            HttpStartCode::started);
    fault_plan().
        fail_next_easy_info.store(true);
    fault_plan().
        skip_next_multi_perform.store(true);
    fault_plan().
        emit_next_done.store(true);

    const auto result =
        poll_until_event(*session);

    ASSERT_EQ(
        result.code,
        asyncdownload::http::
            HttpPollCode::event);
    ASSERT_TRUE(result.event.has_value());
    const auto* failure =
        std::get_if<
            asyncdownload::http::
                HttpLeaseFailed>(
            &*result.event);
    ASSERT_NE(failure, nullptr);
    EXPECT_EQ(
        failure->failure.reason,
        asyncdownload::http::
            HttpFailureReason::easy_info_failed);
    cleanup(*session);
}

TEST_F(
    CurlHttpTransferFaultTest,
    CompletionReportsRemoveFailure) {
    auto session = open_session();
    ASSERT_NE(session, nullptr);
    ASSERT_EQ(
        session->start(lease()).code,
        asyncdownload::http::
            HttpStartCode::started);
    fault_plan().
        fail_next_remove_handle.store(true);
    fault_plan().
        skip_next_multi_perform.store(true);
    fault_plan().
        emit_next_done.store(true);

    const auto result =
        poll_until_event(*session);

    ASSERT_EQ(
        result.code,
        asyncdownload::http::
            HttpPollCode::event);
    ASSERT_TRUE(result.event.has_value());
    const auto* failure =
        std::get_if<
            asyncdownload::http::
                HttpLeaseFailed>(
            &*result.event);
    ASSERT_NE(failure, nullptr);
    EXPECT_EQ(
        failure->failure.reason,
        asyncdownload::http::
            HttpFailureReason::remove_handle_failed);
    cleanup(*session);
}

TEST_F(
    CurlHttpTransferFaultTest,
    CloseReportsMultiCleanupFailure) {
    auto session = open_session();
    ASSERT_NE(session, nullptr);
    fault_plan().
        fail_next_multi_cleanup.store(true);

    const auto error = session->close();

    EXPECT_EQ(
        error,
        asyncdownload::make_error_code(
            asyncdownload::DownloadErrc::
                http_transfer_failed));
    EXPECT_EQ(
        fault_plan().
            multi_cleanup_calls.load(),
        1U);
}

}

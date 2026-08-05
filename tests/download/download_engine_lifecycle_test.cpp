#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <system_error>

#include <gtest/gtest.h>

#include "asyncdownload/error.hpp"
#include "download/download_engine_internal.hpp"
#include "flow/packet_flow.hpp"
#include "http/http_transfer.hpp"

namespace {

std::unique_ptr<asyncdownload::http::HttpTransferPort> next_port;
std::optional<asyncdownload::flow::PacketFlowState>
    session_destructor_flow_state;

[[nodiscard]] std::error_code take_port(
    std::unique_ptr<asyncdownload::http::HttpTransferPort>& result) noexcept {
    result = std::move(next_port);
    return result == nullptr
        ? asyncdownload::make_error_code(
            asyncdownload::DownloadErrc::internal_error)
        : std::error_code{};
}

[[nodiscard]] std::error_code fail_after_persistence_start() noexcept {
    return std::make_error_code(std::errc::not_enough_memory);
}

class ProbePort : public asyncdownload::http::HttpTransferPort {
public:
    [[nodiscard]] asyncdownload::http::HttpProbeResult probe(
        const asyncdownload::http::HttpProbeRequest&) noexcept override {
        return {
            asyncdownload::http::HttpObjectFacts{
                8192,
                true,
                {},
                {}
            },
            {},
            200
        };
    }

    [[nodiscard]] asyncdownload::http::HttpSessionOpenResult open_session(
        const asyncdownload::http::HttpSessionConfig&,
        asyncdownload::flow::PacketProducer&,
        asyncdownload::telemetry::TelemetrySession&) noexcept override {
        return {
            nullptr,
            {
                asyncdownload::http::HttpFailureReason::allocation_failed,
                std::make_error_code(std::errc::not_enough_memory)
            }
        };
    }
};

class FailingSession : public asyncdownload::http::HttpTransferSession {
public:
    explicit FailingSession(
        asyncdownload::flow::PacketProducer& producer) noexcept
        : producer_(producer) {}

    ~FailingSession() override {
        session_destructor_flow_state =
            producer_.snapshot().state;
    }

    [[nodiscard]] asyncdownload::http::HttpStartResult start(
        const asyncdownload::range::RangeLease&) noexcept override {
        return {
            asyncdownload::http::HttpStartCode::failed,
            {},
            {
                asyncdownload::http::HttpFailureReason::allocation_failed,
                std::make_error_code(std::errc::not_enough_memory)
            }
        };
    }

    [[nodiscard]] std::error_code set_gap_paused(
        const asyncdownload::http::TransferToken&,
        bool) noexcept override {
        return {};
    }

    [[nodiscard]] asyncdownload::http::HttpPollResult poll(
        std::chrono::milliseconds) noexcept override {
        return {asyncdownload::http::HttpPollCode::idle, {}, {}};
    }

    [[nodiscard]] std::error_code cancel(
        const asyncdownload::http::HttpCancelRequest&) noexcept override {
        state_ = asyncdownload::http::HttpSessionState::cancelling;
        return {};
    }

    [[nodiscard]] asyncdownload::http::HttpSessionSnapshot snapshot()
        const noexcept override {
        return {
            state_,
            0,
            state_ == asyncdownload::http::HttpSessionState::open ? 1U : 0U,
            0,
            0,
            {}
        };
    }

    [[nodiscard]] std::error_code close() noexcept override {
        state_ = asyncdownload::http::HttpSessionState::closed;
        return {};
    }

private:
    asyncdownload::flow::PacketProducer& producer_;
    asyncdownload::http::HttpSessionState state_ =
        asyncdownload::http::HttpSessionState::open;
};

class FailingSessionPort final : public ProbePort {
public:
    [[nodiscard]] asyncdownload::http::HttpSessionOpenResult open_session(
        const asyncdownload::http::HttpSessionConfig&,
        asyncdownload::flow::PacketProducer& producer,
        asyncdownload::telemetry::TelemetrySession&) noexcept override {
        return {
            std::make_unique<FailingSession>(producer),
            {}
        };
    }
};

class DownloadEngineLifecycleTest : public ::testing::Test {
protected:
    void SetUp() override {
        static std::atomic<std::uint64_t> next_id{0};
        root_ = std::filesystem::temp_directory_path() /
            ("asyncdownload_engine_lifecycle_" +
             std::to_string(next_id.fetch_add(
                 1,
                 std::memory_order_relaxed)));
        std::error_code error;
        std::filesystem::create_directories(root_, error);
        ASSERT_FALSE(error);
        session_destructor_flow_state.reset();
    }

    void TearDown() override {
        next_port.reset();
        std::error_code error;
        const auto removed =
            std::filesystem::remove_all(root_, error);
        static_cast<void>(removed);
    }

    [[nodiscard]] asyncdownload::DownloadRequest request() const {
        asyncdownload::DownloadRequest value{};
        value.url = "https://example.com/artifact.bin";
        value.output_path = root_ / "artifact.bin";
        value.options.max_connections = 1;
        return value;
    }

    std::filesystem::path root_;
};

}

TEST_F(
    DownloadEngineLifecycleTest,
    PostStartResourceFailureReturnsWithoutHanging) {
    next_port = std::make_unique<ProbePort>();
    const auto started = std::chrono::steady_clock::now();

    const auto result = asyncdownload::download::detail::run_download(
        request(),
        {take_port, fail_after_persistence_start});

    EXPECT_EQ(
        result.error,
        std::make_error_code(std::errc::not_enough_memory));
    EXPECT_LT(
        std::chrono::steady_clock::now() - started,
        std::chrono::seconds(2));
    EXPECT_TRUE(std::filesystem::exists(result.temporary_path));
}

TEST_F(
    DownloadEngineLifecycleTest,
    HttpSessionIsDestroyedBeforePacketFlowCloses) {
    next_port = std::make_unique<FailingSessionPort>();
    const auto started = std::chrono::steady_clock::now();

    const auto result = asyncdownload::download::detail::run_download(
        request(),
        {take_port, nullptr});

    EXPECT_EQ(
        result.error,
        std::make_error_code(std::errc::not_enough_memory));
    ASSERT_TRUE(session_destructor_flow_state.has_value());
    EXPECT_EQ(
        *session_destructor_flow_state,
        asyncdownload::flow::PacketFlowState::open);
    EXPECT_LT(
        std::chrono::steady_clock::now() - started,
        std::chrono::seconds(2));
}

#include <atomic>
#include <array>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <system_error>

#include <gtest/gtest.h>

#include "asyncdownload/error.hpp"
#include "core/block_bitmap.hpp"
#include "core/block_geometry.hpp"
#include "core/models.hpp"
#include "download/download_policy.hpp"
#include "download/download_engine_internal.hpp"
#include "flow/packet_flow.hpp"
#include "flow/packet_queue_adapter.hpp"
#include "http/http_transfer.hpp"
#include "persistence/persistence_thread.hpp"
#include "range/range_fact_slot.hpp"
#include "range/range_fault_adapter.hpp"
#include "recovery/recovery_checkpoint.hpp"
#include <thread-pool/BS_thread_pool.hpp>
#include "download/persistence_phase.hpp"

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
        asyncdownload::flow::detail::
            packet_queue_fault_plan().reset();
        asyncdownload::range::detail::
            range_fault_plan().reset();
    }

    void TearDown() override {
        next_port.reset();
        asyncdownload::flow::detail::
            packet_queue_fault_plan().reset();
        asyncdownload::range::detail::
            range_fault_plan().reset();
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

TEST_F(
    DownloadEngineLifecycleTest,
    InitialGeometryFailureDrainsAndJoins) {
    asyncdownload::range::detail::range_fault_plan().
        fail_next_geometry_submit_allocation.store(
            true,
            std::memory_order_release);
    next_port = std::make_unique<ProbePort>();
    const auto started = std::chrono::steady_clock::now();

    const auto result = asyncdownload::download::detail::run_download(
        request(),
        {take_port, nullptr});

    EXPECT_TRUE(result.error);
    EXPECT_TRUE(std::filesystem::exists(result.temporary_path));
    EXPECT_LT(
        std::chrono::steady_clock::now() - started,
        std::chrono::seconds(2));
}

TEST_F(
    DownloadEngineLifecycleTest,
    CloseMarkerFailurePreservesFirstError) {
    asyncdownload::flow::detail::packet_queue_fault_plan().
        fail_next_close_publish.store(
            true,
            std::memory_order_release);
    next_port = std::make_unique<ProbePort>();

    const auto result = asyncdownload::download::detail::run_download(
        request(),
        {take_port, fail_after_persistence_start});

    EXPECT_EQ(
        result.error,
        std::make_error_code(std::errc::not_enough_memory));
    EXPECT_FALSE(
        asyncdownload::flow::detail::packet_queue_fault_plan().
            fail_next_close_publish.load(
                std::memory_order_acquire));
}

TEST_F(
    DownloadEngineLifecycleTest,
    PersistenceFailureUsesConsumerOwnerToDrainFlow) {
    asyncdownload::range::detail::range_fault_plan().
        fail_next_write_state_allocation.store(
            true,
            std::memory_order_release);
    next_port = std::make_unique<ProbePort>();

    const auto result = asyncdownload::download::detail::run_download(
        request(),
        {take_port, nullptr});

    EXPECT_TRUE(result.error);
    EXPECT_TRUE(
        asyncdownload::flow::detail::packet_queue_fault_plan().
            consumer_fail_started.load(
                std::memory_order_acquire));
}

TEST_F(
    DownloadEngineLifecycleTest,
    FinishIsIdempotentAndPreservesFirstError) {
    asyncdownload::DownloadOptions options{};
    options.max_connections = 1;
    options.block_size = 4096;
    options.io_alignment = 4096;
    options.max_gap_bytes = 4096;
    options.flush_threshold_bytes = 4096;
    const auto validated =
        asyncdownload::download::validate_download_options(
            options);
    ASSERT_TRUE(validated.ok());
    const auto effective =
        asyncdownload::download::bind_remote_facts(
            *validated.value,
            {8192, true});
    ASSERT_TRUE(effective.ok());
    asyncdownload::core::SessionState session(
        *effective.value);
    session.url = "https://example.com/phase.bin";
    session.paths.output_path = root_ / "phase.bin";
    session.paths.temporary_path =
        root_ / "phase.bin.part";
    session.paths.metadata_path =
        root_ / "phase.bin.config.json";
    std::unique_ptr<asyncdownload::flow::PacketFlow>
        packet_flow;
    ASSERT_FALSE(asyncdownload::flow::PacketFlow::create(
        session.effective_policy.flow_control(),
        session.telemetry_session_,
        packet_flow));
    asyncdownload::recovery::RecoveryOpenRequest open_request{};
    open_request.paths = session.paths;
    open_request.remote.url = session.url;
    open_request.remote.total_size = 8192;
    open_request.remote.accept_ranges = true;
    open_request.policy =
        session.effective_policy.recovery_identity();
    auto opened =
        asyncdownload::recovery::RecoveryCheckpoint::open(
            open_request);
    ASSERT_FALSE(opened.error);
    const auto block_count = asyncdownload::core::required_block_count(
        8192,
        session.effective_policy.persistence().block_bytes);
    ASSERT_TRUE(block_count.has_value());
    asyncdownload::core::AtomicBlockBitmap bitmap(*block_count);
    BS::thread_pool<> workers(1);
    asyncdownload::persistence::PersistenceThread persistence(
        session,
        session.effective_policy.persistence(),
        packet_flow->consumer(),
        bitmap,
        *opened.checkpoint,
        workers);
    asyncdownload::download::PersistencePhase phase(
        packet_flow->producer(),
        persistence);
    phase.start();

    const auto first = std::make_error_code(
        std::errc::not_enough_memory);
    const auto second = asyncdownload::make_error_code(
        asyncdownload::DownloadErrc::internal_error);
    EXPECT_EQ(phase.finish(first), first);
    EXPECT_EQ(phase.finish(second), second);
    EXPECT_EQ(
        packet_flow->producer().snapshot().state,
        asyncdownload::flow::PacketFlowState::closed);
    opened.checkpoint->close_preserving_artifacts();
}

TEST_F(
    DownloadEngineLifecycleTest,
    CloseMarkerFailureReturnsAfterConsumerPoll) {
    asyncdownload::DownloadOptions options{};
    options.max_connections = 1;
    options.block_size = 4096;
    options.io_alignment = 4096;
    options.max_gap_bytes = 4096;
    options.flush_threshold_bytes = 4096;
    const auto validated =
        asyncdownload::download::validate_download_options(options);
    ASSERT_TRUE(validated.ok());
    const auto effective =
        asyncdownload::download::bind_remote_facts(
            *validated.value,
            {8192, true});
    ASSERT_TRUE(effective.ok());
    asyncdownload::core::SessionState session(*effective.value);
    session.url = "https://example.com/close.bin";
    session.paths.output_path = root_ / "close.bin";
    session.paths.temporary_path = root_ / "close.bin.part";
    session.paths.metadata_path = root_ / "close.bin.config.json";
    std::unique_ptr<asyncdownload::flow::PacketFlow> packet_flow;
    ASSERT_FALSE(asyncdownload::flow::PacketFlow::create(
        session.effective_policy.flow_control(),
        session.telemetry_session_,
        packet_flow));
    asyncdownload::recovery::RecoveryOpenRequest open_request{};
    open_request.paths = session.paths;
    open_request.remote.url = session.url;
    open_request.remote.total_size = 8192;
    open_request.remote.accept_ranges = true;
    open_request.policy =
        session.effective_policy.recovery_identity();
    auto opened =
        asyncdownload::recovery::RecoveryCheckpoint::open(
            open_request);
    ASSERT_FALSE(opened.error);
    const auto block_count = asyncdownload::core::required_block_count(
        8192,
        session.effective_policy.persistence().block_bytes);
    ASSERT_TRUE(block_count.has_value());
    asyncdownload::core::AtomicBlockBitmap bitmap(*block_count);
    BS::thread_pool<> workers(1);
    asyncdownload::persistence::PersistenceThread persistence(
        session,
        session.effective_policy.persistence(),
        packet_flow->consumer(),
        bitmap,
        *opened.checkpoint,
        workers);
    asyncdownload::flow::detail::packet_queue_fault_plan().
        fail_next_close_publish.store(
            true,
            std::memory_order_release);
    asyncdownload::download::PersistencePhase phase(
        packet_flow->producer(),
        persistence);
    phase.start();
    const auto started = std::chrono::steady_clock::now();

    const auto result = phase.finish({});

    EXPECT_EQ(
        result,
        asyncdownload::make_error_code(
            asyncdownload::DownloadErrc::internal_error));
    EXPECT_LT(
        std::chrono::steady_clock::now() - started,
        std::chrono::seconds(2));
    EXPECT_TRUE(
        asyncdownload::flow::detail::packet_queue_fault_plan().
            consumer_fail_started.load(
                std::memory_order_acquire));
    opened.checkpoint->close_preserving_artifacts();
}

TEST_F(
    DownloadEngineLifecycleTest,
    NormalCloseDrainsPacketsAndFinalizesCheckpoint) {
    asyncdownload::DownloadOptions options{};
    options.max_connections = 1;
    options.block_size = 4096;
    options.io_alignment = 4096;
    options.max_gap_bytes = 4096;
    options.flush_threshold_bytes = 4096;
    const auto validated =
        asyncdownload::download::validate_download_options(options);
    ASSERT_TRUE(validated.ok());
    const auto effective =
        asyncdownload::download::bind_remote_facts(
            *validated.value,
            {4096, true});
    ASSERT_TRUE(effective.ok());
    asyncdownload::core::SessionState session(*effective.value);
    session.url = "https://example.com/normal.bin";
    session.paths.output_path = root_ / "normal.bin";
    session.paths.temporary_path = root_ / "normal.bin.part";
    session.paths.metadata_path = root_ / "normal.bin.config.json";
    std::unique_ptr<asyncdownload::flow::PacketFlow> packet_flow;
    ASSERT_FALSE(asyncdownload::flow::PacketFlow::create(
        session.effective_policy.flow_control(),
        session.telemetry_session_,
        packet_flow));
    asyncdownload::flow::ProducerLane lane;
    ASSERT_FALSE(packet_flow->producer().open_lane(lane));
    asyncdownload::recovery::RecoveryOpenRequest open_request{};
    open_request.paths = session.paths;
    open_request.remote.url = session.url;
    open_request.remote.total_size = 4096;
    open_request.remote.accept_ranges = true;
    open_request.policy =
        session.effective_policy.recovery_identity();
    auto opened =
        asyncdownload::recovery::RecoveryCheckpoint::open(
            open_request);
    ASSERT_FALSE(opened.error);
    const auto block_count = asyncdownload::core::required_block_count(
        4096,
        session.effective_policy.persistence().block_bytes);
    ASSERT_TRUE(block_count.has_value());
    asyncdownload::core::AtomicBlockBitmap bitmap(*block_count);
    BS::thread_pool<> workers(1);
    asyncdownload::persistence::PersistenceThread persistence(
        session,
        session.effective_policy.persistence(),
        packet_flow->consumer(),
        bitmap,
        *opened.checkpoint,
        workers);
    asyncdownload::range::RangeFactSlot facts({0}, 0);
    const auto registration = persistence.submit_range_geometry(
        asyncdownload::range::RegisterRangeEffect{
            {0},
            {0, 4096},
            0,
            facts.publisher()});
    ASSERT_FALSE(registration.error);
    asyncdownload::download::PersistencePhase phase(
        packet_flow->producer(),
        persistence);
    phase.start();
    const std::array<std::uint8_t, 4096> payload{};
    const auto accepted = packet_flow->producer().accept(
        lane,
        {
            {{0}, 1},
            {0, 4096},
            0,
            payload
        });
    ASSERT_TRUE(accepted.accepted());
    ASSERT_TRUE(packet_flow->producer().flush(lane).accepted());
    const auto published = packet_flow->producer().publish({
        asyncdownload::flow::ControlPacketKind::range_complete,
        {{0}, 1},
        4096});
    ASSERT_EQ(
        published.code,
        asyncdownload::flow::PacketPublishCode::published);

    std::optional<asyncdownload::range::RangeFactSnapshot> committed;
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        committed = facts.read_since(0);
        if (committed.has_value() &&
            committed->committed_generation == 1) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(committed.has_value());
    ASSERT_EQ(committed->committed_generation, 1U);
    const auto phase_result = phase.finish({});
    EXPECT_FALSE(phase_result);
    EXPECT_EQ(
        packet_flow->producer().snapshot().queued_packets,
        0U);
    EXPECT_EQ(
        packet_flow->producer().snapshot().accounted_bytes,
        0U);
    const auto finalized = opened.checkpoint->finalize();
    EXPECT_FALSE(finalized.error);
    EXPECT_TRUE(finalized.output_available);
    EXPECT_TRUE(std::filesystem::exists(session.paths.output_path));
}

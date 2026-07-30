#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <system_error>
#include <utility>

#include <gtest/gtest.h>
#include <thread-pool/BS_thread_pool.hpp>

#include "asyncdownload/error.hpp"
#include "core/block_bitmap.hpp"
#include "core/models.hpp"
#include "download/download_policy.hpp"
#include "flow/packet_flow.hpp"
#include "persistence/persistence_thread.hpp"
#include "range/range_fault_adapter.hpp"
#include "range/range_lifecycle.hpp"
#include "recovery/recovery_checkpoint.hpp"

namespace {

asyncdownload::download::SchedulingPolicy fault_policy() {
    return {4, 128, 64, true, true};
}

asyncdownload::download::EffectiveDownloadPolicy
effective_fault_policy() {
    asyncdownload::DownloadOptions options{};
    const auto validated =
        asyncdownload::download::validate_download_options(
            options);
    if (!validated.ok()) {
        std::abort();
    }
    auto effective =
        asyncdownload::download::bind_remote_facts(
            *validated.value,
            {4096, true});
    if (!effective.ok()) {
        std::abort();
    }
    return std::move(*effective.value);
}

class RangeLifecycleFaultTest : public ::testing::Test {
protected:
    void SetUp() override {
        static std::atomic<std::uint64_t> next_id{0};
        root_ = std::filesystem::temp_directory_path() /
            ("asyncdownload_range_fault_" +
             std::to_string(next_id.fetch_add(
                 1,
                 std::memory_order_relaxed)));
        std::error_code ec;
        std::filesystem::create_directories(root_, ec);
        ASSERT_FALSE(ec);
        asyncdownload::range::detail::range_fault_plan().reset();
    }

    void TearDown() override {
        asyncdownload::range::detail::range_fault_plan().reset();
        std::error_code ec;
        const auto removed =
            std::filesystem::remove_all(root_, ec);
        static_cast<void>(removed);
    }

    std::unique_ptr<
        asyncdownload::recovery::RecoveryCheckpoint>
    open_checkpoint(
        asyncdownload::core::SessionState& session) {
        if (session.paths.temporary_path.empty()) {
            session.paths.output_path =
                root_ / "output.bin";
            session.paths.temporary_path =
                root_ / "output.bin.part";
            session.paths.metadata_path =
                root_ / "output.bin.config.json";
            session.url =
                "http://127.0.0.1/fault.bin";
        }
        asyncdownload::recovery::RecoveryOpenRequest
            request{};
        request.paths = session.paths;
        request.remote.url = session.url;
        request.remote.total_size = session.total_size;
        request.remote.accept_ranges =
            session.effective_policy.remote_facts().
                accept_ranges;
        request.policy =
            session.effective_policy.recovery_identity();
        request.overwrite_existing = true;
        auto opened =
            asyncdownload::recovery::RecoveryCheckpoint::
                open(request);
        EXPECT_FALSE(opened.error);
        EXPECT_NE(opened.checkpoint, nullptr);
        return std::move(opened.checkpoint);
    }

    std::filesystem::path root_;
};

}

TEST_F(
    RangeLifecycleFaultTest,
    CreateAllocationFailureIsMappedWithoutValue) {
    auto& plan =
        asyncdownload::range::detail::range_fault_plan();
    plan.fail_next_create_allocation.store(
        true,
        std::memory_order_release);
    const std::array<asyncdownload::range::ByteSpan, 1>
        ranges{{{0, 256}}};

    const auto creation =
        asyncdownload::range::RangeLifecycle::create(
            256,
            fault_policy(),
            ranges);

    EXPECT_EQ(
        creation.error,
        std::make_error_code(std::errc::not_enough_memory));
    EXPECT_EQ(creation.value, nullptr);
    EXPECT_TRUE(creation.effects.values.empty());
}

TEST_F(
    RangeLifecycleFaultTest,
    SnapshotAllocationFailureDoesNotMutateLifecycle) {
    const std::array<asyncdownload::range::ByteSpan, 1>
        ranges{{{0, 256}}};
    auto creation =
        asyncdownload::range::RangeLifecycle::create(
            256,
            fault_policy(),
            ranges);
    ASSERT_FALSE(creation.error);
    ASSERT_NE(creation.value, nullptr);
    const auto before = creation.value->snapshot();
    ASSERT_FALSE(before.error);
    auto& plan =
        asyncdownload::range::detail::range_fault_plan();
    plan.fail_next_snapshot_allocation.store(
        true,
        std::memory_order_release);

    const auto failed = creation.value->snapshot();
    const auto after = creation.value->snapshot();

    EXPECT_EQ(
        failed.error,
        std::make_error_code(std::errc::not_enough_memory));
    EXPECT_TRUE(failed.value.ranges.empty());
    ASSERT_FALSE(after.error);
    ASSERT_EQ(after.value.ranges.size(), 1U);
    EXPECT_EQ(after.value.ranges[0].bytes, (ranges[0]));
    EXPECT_EQ(
        after.value.ranges[0].phase,
        asyncdownload::range::RangePhase::ready);
}

TEST_F(
    RangeLifecycleFaultTest,
    GenerationOverflowFailsWithoutAdvancingCursor) {
    const std::array<asyncdownload::range::ByteSpan, 1>
        ranges{{{0, 256}}};
    auto creation =
        asyncdownload::range::RangeLifecycle::create(
            256,
            fault_policy(),
            ranges);
    ASSERT_FALSE(creation.error);
    ASSERT_NE(creation.value, nullptr);
    auto& plan =
        asyncdownload::range::detail::range_fault_plan();
    plan.force_next_lease_generation.store(
        std::numeric_limits<std::uint64_t>::max(),
        std::memory_order_release);

    const auto acquired = creation.value->acquire();
    const auto snapshot = creation.value->snapshot();

    EXPECT_TRUE(acquired.error);
    EXPECT_FALSE(acquired.lease.has_value());
    ASSERT_FALSE(snapshot.error);
    EXPECT_EQ(snapshot.value.ranges[0].dispatch_cursor, 0);
    EXPECT_EQ(
        snapshot.value.ranges[0].phase,
        asyncdownload::range::RangePhase::failed);
}

TEST_F(
    RangeLifecycleFaultTest,
    GeometrySubmitAllocationFailureDoesNotConsumeTicket) {
    asyncdownload::core::SessionState session(
        effective_fault_policy());
    std::unique_ptr<asyncdownload::flow::PacketFlow>
        packet_flow;
    ASSERT_FALSE(asyncdownload::flow::PacketFlow::create(
        session.effective_policy.flow_control(),
        session.telemetry_session_,
        packet_flow));
    asyncdownload::core::AtomicBlockBitmap bitmap(1);
    auto checkpoint = open_checkpoint(session);
    ASSERT_NE(checkpoint, nullptr);
    BS::thread_pool<> workers(1);
    asyncdownload::persistence::PersistenceThread persistence(
        session,
        session.effective_policy.persistence(),
        packet_flow->consumer(),
        bitmap,
        *checkpoint,
        workers,
        1);
    auto& plan =
        asyncdownload::range::detail::range_fault_plan();
    plan.fail_next_geometry_submit_allocation.store(
        true,
        std::memory_order_release);
    const auto command =
        asyncdownload::persistence::RangeGeometryCommand{
            asyncdownload::range::RegisterRangeEffect{
                {0},
                {0, 4096},
                0,
                {}
            }
        };

    const auto failed =
        persistence.submit_range_geometry(command);
    const auto succeeded =
        persistence.submit_range_geometry(command);

    EXPECT_EQ(
        failed.error,
        std::make_error_code(std::errc::not_enough_memory));
    EXPECT_EQ(failed.ticket, 0U);
    EXPECT_FALSE(succeeded.error);
    EXPECT_EQ(succeeded.ticket, 1U);
}

TEST_F(
    RangeLifecycleFaultTest,
    WriteStateAllocationFailureStopsPersistenceWithoutAck) {
    asyncdownload::core::SessionState session(
        effective_fault_policy());
    std::unique_ptr<asyncdownload::flow::PacketFlow>
        packet_flow;
    ASSERT_FALSE(asyncdownload::flow::PacketFlow::create(
        session.effective_policy.flow_control(),
        session.telemetry_session_,
        packet_flow));
    asyncdownload::core::AtomicBlockBitmap bitmap(1);
    auto checkpoint = open_checkpoint(session);
    ASSERT_NE(checkpoint, nullptr);
    BS::thread_pool<> workers(1);
    asyncdownload::persistence::PersistenceThread persistence(
        session,
        session.effective_policy.persistence(),
        packet_flow->consumer(),
        bitmap,
        *checkpoint,
        workers,
        1);
    auto& plan =
        asyncdownload::range::detail::range_fault_plan();
    plan.fail_next_write_state_allocation.store(
        true,
        std::memory_order_release);
    const auto submitted =
        persistence.submit_range_geometry(
            asyncdownload::range::RegisterRangeEffect{
                {0},
                {0, 4096},
                0,
                {}
            });
    ASSERT_FALSE(submitted.error);
    persistence.start();
    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::seconds(1);
    while (!persistence.error() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(1));
    }
    const auto close_error =
        packet_flow->producer().close();
    persistence.stop();
    persistence.join();

    EXPECT_EQ(
        persistence.error(),
        std::make_error_code(std::errc::not_enough_memory));
    EXPECT_EQ(close_error, persistence.error());
    EXPECT_FALSE(
        persistence.poll_range_geometry_ack().ack.has_value());
    EXPECT_EQ(
        packet_flow->producer().snapshot().accounted_bytes,
        0U);
}

TEST_F(
    RangeLifecycleFaultTest,
    ReorderAllocationFailureBecomesPersistenceError) {
    asyncdownload::core::SessionState session(
        effective_fault_policy());
    const auto temp_root =
        std::filesystem::temp_directory_path() /
        "asyncdownload_range_fault_test";
    std::error_code filesystem_error;
    std::filesystem::create_directories(
        temp_root,
        filesystem_error);
    ASSERT_FALSE(filesystem_error);
    session.paths.temporary_path =
        temp_root / "output.bin.part";
    session.paths.metadata_path =
        temp_root / "output.bin.config.json";
    session.paths.output_path =
        temp_root / "output.bin";
    session.url = "http://127.0.0.1/fault.bin";
    std::unique_ptr<asyncdownload::flow::PacketFlow>
        packet_flow;
    ASSERT_FALSE(asyncdownload::flow::PacketFlow::create(
        session.effective_policy.flow_control(),
        session.telemetry_session_,
        packet_flow));
    asyncdownload::flow::ProducerLane lane;
    ASSERT_FALSE(
        packet_flow->producer().open_lane(lane));
    asyncdownload::core::AtomicBlockBitmap bitmap(1);
    auto checkpoint = open_checkpoint(session);
    ASSERT_NE(checkpoint, nullptr);
    BS::thread_pool<> workers(1);
    asyncdownload::persistence::PersistenceThread persistence(
        session,
        session.effective_policy.persistence(),
        packet_flow->consumer(),
        bitmap,
        *checkpoint,
        workers,
        1);
    asyncdownload::range::RangeFactSlot facts({0}, 0);
    ASSERT_FALSE(
        persistence.submit_range_geometry(
            asyncdownload::range::RegisterRangeEffect{
                {0},
                {0, 4096},
                0,
                facts.publisher()
            }).error);
    persistence.start();
    auto& plan =
        asyncdownload::range::detail::range_fault_plan();
    plan.fail_next_reorder_allocation.store(
        true,
        std::memory_order_release);
    const std::array<std::uint8_t, 512> payload{};
    const auto accepted = packet_flow->producer().accept(
        lane,
        {
            {{0}, 1},
            {0, 4096},
            2048,
            payload
        });
    ASSERT_TRUE(accepted.accepted());
    ASSERT_TRUE(
        packet_flow->producer().flush(lane).accepted());

    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::seconds(1);
    while (!persistence.error() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(1));
    }
    const auto observed_error = persistence.error();
    const auto close_error =
        packet_flow->producer().close();
    persistence.stop();
    persistence.join();
    const auto flow_snapshot =
        packet_flow->producer().snapshot();
    checkpoint->close_preserving_artifacts();
    const auto removed = std::filesystem::remove_all(
        temp_root,
        filesystem_error);
    static_cast<void>(removed);

    EXPECT_EQ(
        observed_error,
        std::make_error_code(std::errc::not_enough_memory));
    EXPECT_EQ(close_error, observed_error);
    EXPECT_EQ(flow_snapshot.queued_packets, 0U);
    EXPECT_EQ(flow_snapshot.accounted_bytes, 0U);
}

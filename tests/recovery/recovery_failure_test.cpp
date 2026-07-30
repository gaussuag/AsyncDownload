#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <thread-pool/BS_thread_pool.hpp>

#include "asyncdownload/error.hpp"
#include "core/block_bitmap.hpp"
#include "core/models.hpp"
#include "download/download_policy.hpp"
#include "flow/packet_flow.hpp"
#include "metadata/metadata_store.hpp"
#include "persistence/persistence_thread.hpp"
#include "range/range_fact_slot.hpp"
#include "recovery/recovery_checkpoint.hpp"
#include "recovery/recovery_fault_adapter.hpp"

namespace {

asyncdownload::download::EffectiveDownloadPolicy
effective_policy(const std::int64_t total_size) {
    asyncdownload::DownloadOptions options{};
    options.block_size = 4096;
    options.io_alignment = 4096;
    options.max_gap_bytes = 4096;
    options.flush_threshold_bytes = 4096;
    options.flush_interval =
        std::chrono::seconds(60);
    const auto validated =
        asyncdownload::download::
            validate_download_options(options);
    if (!validated.ok()) {
        std::abort();
    }
    auto effective =
        asyncdownload::download::bind_remote_facts(
            *validated.value,
            {total_size, true});
    if (!effective.ok()) {
        std::abort();
    }
    return std::move(*effective.value);
}

bool wait_for(
    const std::function<bool()>& predicate,
    const std::chrono::milliseconds timeout) {
    const auto deadline =
        std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() <
           deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::yield();
    }
    return predicate();
}

class RecoveryFailureTest : public ::testing::Test {
protected:
    void SetUp() override {
        static std::atomic<std::uint64_t> next_id{0};
        root_ = std::filesystem::temp_directory_path() /
            ("asyncdownload_recovery_failure_" +
             std::to_string(next_id.fetch_add(
                 1,
                 std::memory_order_relaxed)));
        std::error_code ec;
        const auto removed =
            std::filesystem::remove_all(root_, ec);
        static_cast<void>(removed);
        ec.clear();
        std::filesystem::create_directories(root_, ec);
        ASSERT_FALSE(ec);
        asyncdownload::recovery::detail::
            recovery_fault_plan().reset();
    }

    void TearDown() override {
        asyncdownload::recovery::detail::
            recovery_fault_plan().reset();
        std::error_code ec;
        const auto removed =
            std::filesystem::remove_all(root_, ec);
        static_cast<void>(removed);
    }

    [[nodiscard]] asyncdownload::recovery::
    RecoveryOpenRequest request() const {
        asyncdownload::recovery::RecoveryOpenRequest
            value{};
        value.paths.output_path =
            root_ / "artifact.bin";
        value.paths.temporary_path =
            root_ / "artifact.bin.part";
        value.paths.metadata_path =
            root_ / "artifact.bin.config.json";
        value.remote.url =
            "https://example.com/artifact.bin";
        value.remote.total_size = 8192;
        value.remote.accept_ranges = true;
        value.policy.block_bytes = 4096;
        value.policy.io_alignment_bytes = 4096;
        value.policy.allow_sparse_resume = true;
        value.overwrite_existing = true;
        return value;
    }

    [[nodiscard]] std::vector<std::uint8_t>
    write_mismatched_pair(
        const asyncdownload::recovery::
            RecoveryOpenRequest& request) const {
        std::vector<std::uint8_t> bytes(8192);
        for (std::size_t index = 0;
             index < bytes.size();
             ++index) {
            bytes[index] =
                static_cast<std::uint8_t>(
                    index % 251);
        }
        std::ofstream part(
            request.paths.temporary_path,
            std::ios::binary | std::ios::trunc);
        EXPECT_TRUE(part.is_open());
        part.write(
            reinterpret_cast<const char*>(
                bytes.data()),
            static_cast<std::streamsize>(
                bytes.size()));
        part.close();
        EXPECT_TRUE(part.good());

        asyncdownload::core::MetadataState state{};
        state.url =
            "https://example.com/other.bin";
        state.output_path = request.paths.output_path;
        state.temporary_path =
            request.paths.temporary_path;
        state.total_size = request.remote.total_size;
        state.vdl_offset = 4096;
        state.accept_ranges = true;
        state.block_size = request.policy.block_bytes;
        state.io_alignment =
            request.policy.io_alignment_bytes;
        state.bitmap_states = {2, 0};
        asyncdownload::metadata::MetadataStore store(
            request.paths.metadata_path);
        EXPECT_FALSE(store.save(state));
        return bytes;
    }

    [[nodiscard]] static std::vector<std::uint8_t>
    read_file(const std::filesystem::path& path) {
        std::ifstream stream(path, std::ios::binary);
        EXPECT_TRUE(stream.is_open());
        return {
            std::istreambuf_iterator<char>(stream),
            std::istreambuf_iterator<char>()
        };
    }

    std::filesystem::path root_;
};

}

TEST_F(
    RecoveryFailureTest,
    InvalidationFailurePreservesOriginalPair) {
    const auto open_request = request();
    const auto part_before =
        write_mismatched_pair(open_request);
    const auto metadata_before =
        read_file(open_request.paths.metadata_path);
    asyncdownload::recovery::detail::
        recovery_fault_plan().
            fail_next_metadata_invalidate.store(
                true,
                std::memory_order_release);

    auto opened =
        asyncdownload::recovery::RecoveryCheckpoint::open(
            open_request);

    EXPECT_EQ(
        opened.error,
        asyncdownload::make_error_code(
            asyncdownload::DownloadErrc::
                metadata_save_failed));
    EXPECT_EQ(opened.checkpoint, nullptr);
    EXPECT_EQ(
        read_file(open_request.paths.temporary_path),
        part_before);
    EXPECT_EQ(
        read_file(open_request.paths.metadata_path),
        metadata_before);
}

TEST_F(
    RecoveryFailureTest,
    CrashAfterPartResetCannotLeaveStalePair) {
    const auto open_request = request();
    const auto part_before =
        write_mismatched_pair(open_request);
    asyncdownload::recovery::detail::
        recovery_fault_plan().
            stop_after_part_reset.store(
                true,
                std::memory_order_release);

    auto opened =
        asyncdownload::recovery::RecoveryCheckpoint::open(
            open_request);

    EXPECT_EQ(
        opened.error,
        asyncdownload::make_error_code(
            asyncdownload::DownloadErrc::
                internal_error));
    EXPECT_EQ(opened.checkpoint, nullptr);
    EXPECT_FALSE(std::filesystem::exists(
        open_request.paths.metadata_path));
    EXPECT_NE(
        read_file(open_request.paths.temporary_path),
        part_before);
}

TEST_F(
    RecoveryFailureTest,
    CrashAfterInvalidationKeepsOldPartUntrusted) {
    const auto open_request = request();
    const auto part_before =
        write_mismatched_pair(open_request);
    asyncdownload::recovery::detail::
        recovery_fault_plan().
            stop_after_metadata_invalidation.store(
                true,
                std::memory_order_release);

    auto opened =
        asyncdownload::recovery::RecoveryCheckpoint::open(
            open_request);

    EXPECT_EQ(
        opened.error,
        asyncdownload::make_error_code(
            asyncdownload::DownloadErrc::
                internal_error));
    EXPECT_EQ(opened.checkpoint, nullptr);
    EXPECT_FALSE(std::filesystem::exists(
        open_request.paths.metadata_path));
    EXPECT_EQ(
        read_file(open_request.paths.temporary_path),
        part_before);
}

TEST_F(
    RecoveryFailureTest,
    FrozenImageExcludesWriteAfterPartFlush) {
    const auto open_request = request();
    auto opened =
        asyncdownload::recovery::RecoveryCheckpoint::open(
            open_request);
    ASSERT_FALSE(opened.error);
    ASSERT_NE(opened.checkpoint, nullptr);
    std::vector<std::uint8_t> first(4096, 0x31);
    ASSERT_FALSE(opened.checkpoint->write(0, first));
    const std::vector<std::uint8_t> bitmap{
        static_cast<std::uint8_t>(
            asyncdownload::core::BlockState::finished),
        static_cast<std::uint8_t>(
            asyncdownload::core::BlockState::empty)
    };
    const std::vector<
        asyncdownload::recovery::RecoveryRangeFact>
        ranges{{
            {0},
            {0, 8192},
            4096,
            4096,
            1
        }};
    auto prepared =
        opened.checkpoint->prepare(bitmap, ranges);
    ASSERT_FALSE(prepared.error);
    ASSERT_NE(prepared.checkpoint, nullptr);
    auto& fault_plan =
        asyncdownload::recovery::detail::
            recovery_fault_plan();
    fault_plan.
        pause_after_part_flush_generation.store(
            1,
            std::memory_order_release);
    auto committed = std::async(
        std::launch::async,
        [&opened, checkpoint =
             std::move(prepared.checkpoint)]() mutable {
            return opened.checkpoint->commit(
                std::move(checkpoint));
        });
    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::seconds(2);
    while (fault_plan.
               part_flush_completed_generation.load(
                   std::memory_order_acquire) != 1 &&
           std::chrono::steady_clock::now() <
               deadline) {
        std::this_thread::yield();
    }
    const auto reached_barrier =
        fault_plan.
            part_flush_completed_generation.load(
                std::memory_order_acquire) == 1;
    const std::vector<std::uint8_t> second(
        4096,
        0x52);
    const auto second_write_error =
        opened.checkpoint->write(4096, second);
    fault_plan.
        pause_after_part_flush_generation.store(
            0,
            std::memory_order_release);
    const auto result = committed.get();

    EXPECT_TRUE(reached_barrier);
    EXPECT_FALSE(second_write_error);
    ASSERT_FALSE(result.error);
    EXPECT_EQ(result.generation, 1U);
    EXPECT_EQ(result.committed_vdl, 4096);
    asyncdownload::metadata::MetadataStore store(
        open_request.paths.metadata_path);
    const auto [load_error, state] = store.load();
    ASSERT_FALSE(load_error);
    ASSERT_TRUE(state.has_value());
    EXPECT_EQ(state->vdl_offset, 4096);
    EXPECT_EQ(state->bitmap_states, bitmap);
    EXPECT_TRUE(state->crc_samples.empty());
}

TEST_F(
    RecoveryFailureTest,
    CrashAfterPartFlushPreservesLastMetadata) {
    const auto open_request = request();
    auto opened =
        asyncdownload::recovery::RecoveryCheckpoint::open(
            open_request);
    ASSERT_FALSE(opened.error);
    ASSERT_NE(opened.checkpoint, nullptr);
    const std::vector<std::uint8_t> first(
        4096,
        0x61);
    ASSERT_FALSE(opened.checkpoint->write(0, first));
    const auto finished =
        static_cast<std::uint8_t>(
            asyncdownload::core::BlockState::finished);
    const auto empty =
        static_cast<std::uint8_t>(
            asyncdownload::core::BlockState::empty);
    std::vector<
        asyncdownload::recovery::RecoveryRangeFact>
        ranges{{
            {0},
            {0, 8192},
            4096,
            4096,
            1
        }};
    auto first_prepared =
        opened.checkpoint->prepare(
            std::vector<std::uint8_t>{
                finished,
                empty
            },
            ranges);
    ASSERT_FALSE(first_prepared.error);
    auto first_committed =
        opened.checkpoint->commit(
            std::move(first_prepared.checkpoint));
    ASSERT_FALSE(first_committed.error);
    ASSERT_EQ(first_committed.generation, 1U);
    const auto metadata_before =
        read_file(open_request.paths.metadata_path);

    const std::vector<std::uint8_t> second(
        4096,
        0x72);
    ASSERT_FALSE(
        opened.checkpoint->write(4096, second));
    ranges[0].dispatch_cursor = 8192;
    ranges[0].persisted_through = 8192;
    ranges[0].legacy_status = 2;
    auto second_prepared =
        opened.checkpoint->prepare(
            std::vector<std::uint8_t>{
                finished,
                finished
            },
            ranges);
    ASSERT_FALSE(second_prepared.error);
    auto& fault_plan =
        asyncdownload::recovery::detail::
            recovery_fault_plan();
    fault_plan.stop_after_part_flush.store(
        true,
        std::memory_order_release);

    const auto second_committed =
        opened.checkpoint->commit(
            std::move(second_prepared.checkpoint));

    EXPECT_EQ(
        second_committed.error,
        asyncdownload::make_error_code(
            asyncdownload::DownloadErrc::
                internal_error));
    EXPECT_EQ(second_committed.generation, 2U);
    EXPECT_EQ(second_committed.committed_vdl, 0);
    EXPECT_EQ(
        read_file(open_request.paths.metadata_path),
        metadata_before);
}

TEST_F(
    RecoveryFailureTest,
    PersistencePublishesFrozenCommitVdl) {
    asyncdownload::core::SessionState session(
        effective_policy(8192));
    const auto open_request = request();
    session.paths = open_request.paths;
    session.url = open_request.remote.url;
    auto opened =
        asyncdownload::recovery::RecoveryCheckpoint::open(
            open_request);
    ASSERT_FALSE(opened.error);
    ASSERT_NE(opened.checkpoint, nullptr);
    std::unique_ptr<asyncdownload::flow::PacketFlow>
        packet_flow;
    ASSERT_FALSE(asyncdownload::flow::PacketFlow::create(
        session.effective_policy.flow_control(),
        session.telemetry_session_,
        packet_flow));
    asyncdownload::flow::ProducerLane lane;
    ASSERT_FALSE(
        packet_flow->producer().open_lane(lane));
    asyncdownload::core::AtomicBlockBitmap bitmap(2);
    BS::thread_pool<> workers(1);
    asyncdownload::persistence::PersistenceThread
        persistence(
            session,
            session.effective_policy.persistence(),
            packet_flow->consumer(),
            bitmap,
            *opened.checkpoint,
            workers,
            1);
    asyncdownload::range::RangeFactSlot facts({0}, 0);
    const auto registered =
        persistence.submit_range_geometry(
            asyncdownload::range::RegisterRangeEffect{
                {0},
                {0, 8192},
                0,
                facts.publisher()
            });
    ASSERT_FALSE(registered.error);
    auto& fault_plan =
        asyncdownload::recovery::detail::
            recovery_fault_plan();
    fault_plan.
        pause_after_part_flush_generation.store(
            1,
            std::memory_order_release);
    persistence.start();
    const auto first_accepted =
        packet_flow->producer().accept(
            lane,
            {
                {{0}, 1},
                {0, 8192},
                0,
                std::vector<std::uint8_t>(
                    4096,
                    0x41)
            });
    const auto first_flushed =
        packet_flow->producer().flush(lane);
    const auto first_barrier = wait_for(
        [&fault_plan]() {
            return fault_plan.
                part_flush_completed_generation.load(
                    std::memory_order_acquire) == 1;
        },
        std::chrono::seconds(2));
    auto second_accepted =
        asyncdownload::flow::PacketAdmission{};
    auto second_flushed =
        asyncdownload::flow::PacketAdmission{};
    if (first_barrier) {
        second_accepted =
            packet_flow->producer().accept(
                lane,
                {
                    {{0}, 1},
                    {0, 8192},
                    4096,
                    std::vector<std::uint8_t>(
                        4096,
                        0x52)
                });
        second_flushed =
            packet_flow->producer().flush(lane);
    }
    const auto second_persisted = wait_for(
        [&session]() {
            return session.persisted_bytes.load(
                       std::memory_order_acquire) ==
                8192;
        },
        std::chrono::seconds(2));
    fault_plan.
        pause_after_part_flush_generation.store(
            2,
            std::memory_order_release);
    const auto second_barrier = wait_for(
        [&fault_plan, &session]() {
            return fault_plan.
                       part_flush_completed_generation.load(
                           std::memory_order_acquire) ==
                       2 &&
                session.vdl_offset.load(
                    std::memory_order_acquire) ==
                    4096;
        },
        std::chrono::seconds(2));
    asyncdownload::metadata::MetadataStore store(
        open_request.paths.metadata_path);
    const auto [load_error, state] = store.load();
    const auto published_vdl =
        session.vdl_offset.load(
            std::memory_order_acquire);
    fault_plan.
        pause_after_part_flush_generation.store(
            0,
            std::memory_order_release);
    const auto close_error =
        packet_flow->producer().close();
    persistence.stop();
    persistence.join();

    EXPECT_TRUE(first_accepted.accepted());
    EXPECT_TRUE(first_flushed.accepted());
    EXPECT_TRUE(first_barrier);
    EXPECT_TRUE(second_accepted.accepted());
    EXPECT_TRUE(second_flushed.accepted());
    EXPECT_TRUE(second_persisted);
    EXPECT_TRUE(second_barrier);
    EXPECT_FALSE(load_error);
    ASSERT_TRUE(state.has_value());
    EXPECT_EQ(state->vdl_offset, 4096);
    EXPECT_EQ(published_vdl, state->vdl_offset);
    EXPECT_FALSE(close_error);
    EXPECT_FALSE(persistence.error());
}

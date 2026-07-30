#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <thread>
#include <utility>
#include <vector>

#include <concurrentqueue/blockingconcurrentqueue.h>
#include <gtest/gtest.h>
#include <thread-pool/BS_thread_pool.hpp>

#include "core/block_bitmap.hpp"
#include "core/memory_accounting.hpp"
#include "core/models.hpp"
#include "metadata/metadata_store.hpp"
#include "persistence/persistence_thread.hpp"
#include "storage/file_writer.hpp"

namespace {

void enqueue_data_packet(
    moodycamel::BlockingConcurrentQueue<asyncdownload::core::DataPacket>& queue,
    asyncdownload::core::SessionState& session,
    asyncdownload::core::DataPacket packet) {
    const auto accounted = asyncdownload::core::global_packet_overhead(
        packet.payload.size(),
        false);
    packet.accounted_bytes = accounted;
    const auto current_bytes = asyncdownload::core::global_memory_accounting().add(accounted);
    static_cast<void>(current_bytes);
    queue.enqueue(std::move(packet));
    session.queued_packets.fetch_add(1, std::memory_order_relaxed);
}

bool wait_for_condition(const std::function<bool()>& predicate,
                        const std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return predicate();
}

asyncdownload::download::EffectiveDownloadPolicy make_effective_policy(
    const asyncdownload::download::PersistencePolicy& policy,
    const std::int64_t total_size) {
    asyncdownload::DownloadOptions options{};
    options.block_size = policy.block_bytes;
    options.io_alignment = policy.io_alignment_bytes;
    options.max_gap_bytes =
        static_cast<std::size_t>(policy.max_gap_bytes);
    options.flush_threshold_bytes = policy.flush_threshold_bytes;
    options.flush_interval = policy.flush_interval;
    options.overwrite_existing = policy.overwrite_existing;

    const auto validated =
        asyncdownload::download::validate_download_options(options);
    if (!validated.ok()) {
        std::abort();
    }
    auto effective = asyncdownload::download::bind_remote_facts(
        *validated.value,
        {total_size, true});
    if (!effective.ok()) {
        std::abort();
    }
    return std::move(*effective.value);
}

void persist_single_range_at_tail_capacity(
    const std::filesystem::path& temp_root,
    const std::int64_t total_size) {
    asyncdownload::core::global_memory_accounting().reset();
    std::error_code ec;
    std::filesystem::create_directories(temp_root, ec);
    ASSERT_FALSE(ec);

    asyncdownload::download::PersistencePolicy policy{};
    policy.block_bytes = asyncdownload::core::TAIL_BUFFER_CAPACITY_BYTES;
    policy.io_alignment_bytes =
        asyncdownload::core::TAIL_BUFFER_CAPACITY_BYTES;
    policy.max_gap_bytes =
        asyncdownload::core::TAIL_BUFFER_CAPACITY_BYTES;
    policy.flush_threshold_bytes =
        asyncdownload::core::TAIL_BUFFER_CAPACITY_BYTES;
    policy.flush_interval = std::chrono::milliseconds(10);

    asyncdownload::core::SessionState session(
        make_effective_policy(policy, total_size));
    session.paths.output_path = temp_root / "output.bin";
    session.paths.temporary_path = temp_root / "output.bin.part";
    session.paths.metadata_path = temp_root / "output.bin.config.json";
    session.url = "http://127.0.0.1/test.bin";
    session.telemetry_session_.record_task_started();

    moodycamel::BlockingConcurrentQueue<
        asyncdownload::core::DataPacket> queue(16);
    asyncdownload::core::AtomicBlockBitmap bitmap(
        asyncdownload::core::required_block_count(
            session.total_size,
            policy.block_bytes));
    asyncdownload::storage::FileWriter writer;
    ASSERT_FALSE(writer.open(
        session.paths.temporary_path,
        session.total_size,
        false,
        true));
    asyncdownload::metadata::MetadataStore store(session.paths.metadata_path);
    BS::thread_pool<> workers(1);
    asyncdownload::persistence::PersistenceThread persistence(
        session,
        policy,
        queue,
        bitmap,
        writer,
        store,
        workers);

    asyncdownload::core::RangeContext range(0, 0, total_size - 1);
    persistence.register_range(&range);
    persistence.start();

    asyncdownload::core::DataPacket packet{};
    packet.kind = asyncdownload::core::PacketKind::data;
    packet.range_id = 0;
    packet.offset = 0;
    packet.payload.assign(static_cast<std::size_t>(total_size), 0x5A);
    enqueue_data_packet(queue, session, std::move(packet));

    asyncdownload::core::DataPacket complete{};
    complete.kind = asyncdownload::core::PacketKind::range_complete;
    complete.range_id = 0;
    queue.enqueue(std::move(complete));
    session.queued_packets.fetch_add(1, std::memory_order_relaxed);

    ASSERT_TRUE(wait_for_condition([&range]() {
        return range.marked_finished.load(std::memory_order_acquire);
    }, std::chrono::milliseconds(1000)));

    persistence.stop();
    persistence.join();
    EXPECT_FALSE(persistence.error());
    EXPECT_EQ(
        session.persisted_bytes.load(std::memory_order_relaxed),
        total_size);

    std::vector<std::byte> stored;
    EXPECT_FALSE(writer.read(
        0,
        static_cast<std::size_t>(total_size),
        stored));
    ASSERT_EQ(stored.size(), static_cast<std::size_t>(total_size));
    for (const auto byte : stored) {
        EXPECT_EQ(byte, std::byte{0x5A});
    }

    writer.close();
    EXPECT_EQ(
        std::filesystem::file_size(session.paths.temporary_path, ec),
        static_cast<std::uintmax_t>(total_size));
    EXPECT_FALSE(ec);

    const auto removed = std::filesystem::remove_all(temp_root, ec);
    static_cast<void>(removed);
}

TEST(PersistenceThreadTest, AcceptsValidatedAlignmentAtTailCapacity) {
    const auto temp_root =
        std::filesystem::temp_directory_path() /
        "asyncdownload_tail_capacity_test";
    persist_single_range_at_tail_capacity(
        temp_root,
        asyncdownload::core::TAIL_BUFFER_CAPACITY_BYTES);
}

TEST(PersistenceThreadTest, FlushesFinalTailWithoutWritingPastObjectEnd) {
    const auto temp_root =
        std::filesystem::temp_directory_path() /
        "asyncdownload_final_tail_test";
    persist_single_range_at_tail_capacity(
        temp_root,
        asyncdownload::core::TAIL_BUFFER_CAPACITY_BYTES + 907);
}

TEST(PersistenceThreadTest, PausesRangeWhenGapExceedsThreshold) {
    asyncdownload::core::global_memory_accounting().reset();

    const auto temp_root = std::filesystem::temp_directory_path() / "asyncdownload_gap_pause_test";
    std::error_code ec;
    std::filesystem::create_directories(temp_root, ec);
    ASSERT_FALSE(ec);

    asyncdownload::download::PersistencePolicy policy{};
    policy.block_bytes = 4096;
    policy.io_alignment_bytes = 4096;
    policy.max_gap_bytes = 4096;
    policy.flush_threshold_bytes = 4096;
    policy.flush_interval = std::chrono::milliseconds(10);

    asyncdownload::core::SessionState session(
        make_effective_policy(policy, 12 * 1024));
    session.paths.output_path = temp_root / "output.bin";
    session.paths.temporary_path = temp_root / "output.bin.part";
    session.paths.metadata_path = temp_root / "output.bin.config.json";
    session.url = "http://127.0.0.1/test.bin";
    session.telemetry_session_.record_task_started();

    moodycamel::BlockingConcurrentQueue<asyncdownload::core::DataPacket> queue(16);
    asyncdownload::core::AtomicBlockBitmap bitmap(
        asyncdownload::core::required_block_count(session.total_size, policy.block_bytes));
    asyncdownload::storage::FileWriter writer;
    ASSERT_FALSE(writer.open(session.paths.temporary_path, session.total_size, false, true));
    asyncdownload::metadata::MetadataStore store(session.paths.metadata_path);
    BS::thread_pool<> workers(1);
    asyncdownload::persistence::PersistenceThread persistence(
        session, policy, queue, bitmap, writer, store, workers);

    asyncdownload::core::RangeContext range(0, 0, session.total_size - 1);
    persistence.register_range(&range);
    persistence.start();

    asyncdownload::core::DataPacket packet{};
    packet.kind = asyncdownload::core::PacketKind::data;
    packet.range_id = 0;
    packet.offset = 8 * 1024;
    packet.payload.assign(4096, 0x33);
    enqueue_data_packet(queue, session, std::move(packet));

    EXPECT_TRUE(wait_for_condition([&range]() {
        return range.pause_for_gap.load(std::memory_order_acquire);
    }, std::chrono::milliseconds(1000)));

    persistence.stop();
    persistence.join();
    writer.close();

    const auto removed = std::filesystem::remove_all(temp_root, ec);
    static_cast<void>(removed);
}

TEST(PersistenceThreadTest, MarksPartiallyPersistedBlocksAsDownloading) {
    asyncdownload::core::global_memory_accounting().reset();

    const auto temp_root =
        std::filesystem::temp_directory_path() / "asyncdownload_downloading_bitmap_test";
    std::error_code ec;
    std::filesystem::create_directories(temp_root, ec);
    ASSERT_FALSE(ec);

    asyncdownload::download::PersistencePolicy policy{};
    policy.block_bytes = 64 * 1024;
    policy.io_alignment_bytes = 4096;
    policy.max_gap_bytes = 64 * 1024;
    policy.flush_threshold_bytes = 4096;
    policy.flush_interval = std::chrono::milliseconds(10);

    asyncdownload::core::SessionState session(
        make_effective_policy(policy, 12 * 1024));
    session.paths.output_path = temp_root / "output.bin";
    session.paths.temporary_path = temp_root / "output.bin.part";
    session.paths.metadata_path = temp_root / "output.bin.config.json";
    session.url = "http://127.0.0.1/test.bin";
    session.telemetry_session_.record_task_started();

    moodycamel::BlockingConcurrentQueue<asyncdownload::core::DataPacket> queue(16);
    asyncdownload::core::AtomicBlockBitmap bitmap(
        asyncdownload::core::required_block_count(session.total_size, policy.block_bytes));
    asyncdownload::storage::FileWriter writer;
    ASSERT_FALSE(writer.open(session.paths.temporary_path, session.total_size, false, true));
    asyncdownload::metadata::MetadataStore store(session.paths.metadata_path);
    BS::thread_pool<> workers(1);
    asyncdownload::persistence::PersistenceThread persistence(
        session, policy, queue, bitmap, writer, store, workers);

    asyncdownload::core::RangeContext range(0, 0, session.total_size - 1);
    persistence.register_range(&range);
    persistence.start();

    asyncdownload::core::DataPacket packet{};
    packet.kind = asyncdownload::core::PacketKind::data;
    packet.range_id = 0;
    packet.offset = 0;
    packet.payload.assign(4096, 0x11);
    enqueue_data_packet(queue, session, std::move(packet));

    EXPECT_TRUE(wait_for_condition([&bitmap]() {
        return bitmap.load(0) == asyncdownload::core::BlockState::downloading;
    }, std::chrono::milliseconds(1000)));

    persistence.stop();
    persistence.join();
    writer.close();

    EXPECT_FALSE(persistence.error());
    EXPECT_EQ(bitmap.load(0), asyncdownload::core::BlockState::downloading);

    const auto removed = std::filesystem::remove_all(temp_root, ec);
    static_cast<void>(removed);
}

TEST(PersistenceThreadTest, DrainsQueuedPacketsAfterPersistence) {
    asyncdownload::core::global_memory_accounting().reset();

    const auto temp_root =
        std::filesystem::temp_directory_path() / "asyncdownload_queue_bytes_tracking_test";
    std::error_code ec;
    std::filesystem::create_directories(temp_root, ec);
    ASSERT_FALSE(ec);

    asyncdownload::download::PersistencePolicy policy{};
    policy.block_bytes = 4096;
    policy.io_alignment_bytes = 4096;
    policy.max_gap_bytes = 4096;
    policy.flush_threshold_bytes = 4096;
    policy.flush_interval = std::chrono::milliseconds(10);

    asyncdownload::core::SessionState session(
        make_effective_policy(policy, 4096));
    session.paths.output_path = temp_root / "output.bin";
    session.paths.temporary_path = temp_root / "output.bin.part";
    session.paths.metadata_path = temp_root / "output.bin.config.json";
    session.url = "http://127.0.0.1/test.bin";
    session.telemetry_session_.record_task_started();

    moodycamel::BlockingConcurrentQueue<asyncdownload::core::DataPacket> queue(16);
    asyncdownload::core::AtomicBlockBitmap bitmap(
        asyncdownload::core::required_block_count(session.total_size, policy.block_bytes));
    asyncdownload::storage::FileWriter writer;
    ASSERT_FALSE(writer.open(session.paths.temporary_path, session.total_size, false, true));
    asyncdownload::metadata::MetadataStore store(session.paths.metadata_path);
    BS::thread_pool<> workers(1);
    asyncdownload::persistence::PersistenceThread persistence(
        session, policy, queue, bitmap, writer, store, workers);

    asyncdownload::core::RangeContext range(0, 0, session.total_size - 1);
    persistence.register_range(&range);
    persistence.start();

    asyncdownload::core::DataPacket packet{};
    packet.kind = asyncdownload::core::PacketKind::data;
    packet.range_id = 0;
    packet.offset = 0;
    packet.payload.assign(4096, 0x7A);
    enqueue_data_packet(queue, session, std::move(packet));

    EXPECT_EQ(session.queued_packets.load(std::memory_order_relaxed), 1U);
    EXPECT_TRUE(wait_for_condition([&session]() {
        return session.persisted_bytes.load(std::memory_order_acquire) == 4096 &&
            session.queued_packets.load(std::memory_order_acquire) == 0U;
    }, std::chrono::milliseconds(1000)));

    persistence.stop();
    persistence.join();
    writer.close();

    EXPECT_FALSE(persistence.error());
    EXPECT_EQ(session.queued_packets.load(std::memory_order_relaxed), 0U);

    const auto removed = std::filesystem::remove_all(temp_root, ec);
    static_cast<void>(removed);
}

TEST(PersistenceThreadTest, CollectsSampledPacketLatencyStats) {
    asyncdownload::core::global_memory_accounting().reset();

    const auto temp_root =
        std::filesystem::temp_directory_path() / "asyncdownload_packet_latency_sampling_test";
    std::error_code ec;
    std::filesystem::create_directories(temp_root, ec);
    ASSERT_FALSE(ec);

    asyncdownload::download::PersistencePolicy policy{};
    policy.block_bytes = 4096;
    policy.io_alignment_bytes = 4096;
    policy.max_gap_bytes = 4096;
    policy.flush_threshold_bytes = 4096;
    policy.flush_interval = std::chrono::milliseconds(10);

    asyncdownload::core::SessionState session(
        make_effective_policy(policy, 4096));
    session.paths.output_path = temp_root / "output.bin";
    session.paths.temporary_path = temp_root / "output.bin.part";
    session.paths.metadata_path = temp_root / "output.bin.config.json";
    session.url = "http://127.0.0.1/test.bin";
    session.telemetry_session_.record_task_started();

    moodycamel::BlockingConcurrentQueue<asyncdownload::core::DataPacket> queue(16);
    asyncdownload::core::AtomicBlockBitmap bitmap(
        asyncdownload::core::required_block_count(session.total_size, policy.block_bytes));
    asyncdownload::storage::FileWriter writer;
    ASSERT_FALSE(writer.open(session.paths.temporary_path, session.total_size, false, true));
    asyncdownload::metadata::MetadataStore store(session.paths.metadata_path);
    BS::thread_pool<> workers(1);
    asyncdownload::persistence::PersistenceThread persistence(
        session, policy, queue, bitmap, writer, store, workers);

    asyncdownload::core::RangeContext range(0, 0, session.total_size - 1);
    persistence.register_range(&range);
    persistence.start();

    asyncdownload::core::DataPacket packet{};
    packet.kind = asyncdownload::core::PacketKind::data;
    packet.range_id = 0;
    packet.offset = 0;
    packet.payload.assign(4096, 0x55);
    enqueue_data_packet(queue, session, std::move(packet));

    EXPECT_TRUE(wait_for_condition([&session]() {
        return session.persisted_bytes.load(std::memory_order_acquire) == 4096;
    }, std::chrono::milliseconds(1000)));

    persistence.stop();
    persistence.join();
    writer.close();

    EXPECT_FALSE(persistence.error());
    EXPECT_EQ(session.persisted_bytes.load(std::memory_order_relaxed), 4096);
    const auto summary = session.telemetry_session_.final_summary();
    EXPECT_EQ(summary.max_inflight_bytes, 0);

    const auto removed = std::filesystem::remove_all(temp_root, ec);
    static_cast<void>(removed);
}

TEST(PersistenceThreadTest, ClearsGapPauseAfterMissingDataArrives) {
    asyncdownload::core::global_memory_accounting().reset();

    const auto temp_root = std::filesystem::temp_directory_path() / "asyncdownload_gap_resume_test";
    std::error_code ec;
    std::filesystem::create_directories(temp_root, ec);
    ASSERT_FALSE(ec);

    asyncdownload::download::PersistencePolicy policy{};
    policy.block_bytes = 4096;
    policy.io_alignment_bytes = 4096;
    policy.max_gap_bytes = 4096;
    policy.flush_threshold_bytes = 4096;
    policy.flush_interval = std::chrono::milliseconds(10);

    asyncdownload::core::SessionState session(
        make_effective_policy(policy, 12 * 1024));
    session.paths.output_path = temp_root / "output.bin";
    session.paths.temporary_path = temp_root / "output.bin.part";
    session.paths.metadata_path = temp_root / "output.bin.config.json";
    session.url = "http://127.0.0.1/test.bin";
    session.telemetry_session_.record_task_started();

    moodycamel::BlockingConcurrentQueue<asyncdownload::core::DataPacket> queue(16);
    asyncdownload::core::AtomicBlockBitmap bitmap(
        asyncdownload::core::required_block_count(session.total_size, policy.block_bytes));
    asyncdownload::storage::FileWriter writer;
    ASSERT_FALSE(writer.open(session.paths.temporary_path, session.total_size, false, true));
    asyncdownload::metadata::MetadataStore store(session.paths.metadata_path);
    BS::thread_pool<> workers(1);
    asyncdownload::persistence::PersistenceThread persistence(
        session, policy, queue, bitmap, writer, store, workers);

    asyncdownload::core::RangeContext range(0, 0, session.total_size - 1);
    persistence.register_range(&range);
    persistence.start();

    asyncdownload::core::DataPacket tail_packet{};
    tail_packet.kind = asyncdownload::core::PacketKind::data;
    tail_packet.range_id = 0;
    tail_packet.offset = 8 * 1024;
    tail_packet.payload.assign(4096, 0x44);
    enqueue_data_packet(queue, session, std::move(tail_packet));

    ASSERT_TRUE(wait_for_condition([&range]() {
        return range.pause_for_gap.load(std::memory_order_acquire);
    }, std::chrono::milliseconds(1000)));

    asyncdownload::core::DataPacket head_packet{};
    head_packet.kind = asyncdownload::core::PacketKind::data;
    head_packet.range_id = 0;
    head_packet.offset = 0;
    head_packet.payload.assign(8 * 1024, 0x22);
    enqueue_data_packet(queue, session, std::move(head_packet));

    EXPECT_TRUE(wait_for_condition([&range, &session]() {
        return !range.pause_for_gap.load(std::memory_order_acquire) &&
            session.persisted_bytes.load(std::memory_order_acquire) == 12 * 1024;
    }, std::chrono::milliseconds(1000)));

    asyncdownload::core::DataPacket complete_packet{};
    complete_packet.kind = asyncdownload::core::PacketKind::range_complete;
    complete_packet.range_id = 0;
    queue.enqueue(std::move(complete_packet));
    session.queued_packets.fetch_add(1, std::memory_order_relaxed);

    EXPECT_TRUE(wait_for_condition([&range]() {
        return range.marked_finished.load(std::memory_order_acquire);
    }, std::chrono::milliseconds(1000)));

    persistence.stop();
    persistence.join();
    writer.close();

    EXPECT_FALSE(persistence.error());
    EXPECT_EQ(bitmap.load(0), asyncdownload::core::BlockState::finished);
    EXPECT_EQ(bitmap.load(1), asyncdownload::core::BlockState::finished);
    EXPECT_EQ(bitmap.load(2), asyncdownload::core::BlockState::finished);
    EXPECT_EQ(session.persisted_bytes.load(std::memory_order_relaxed), 3 * 4096);
    const auto summary = session.telemetry_session_.final_summary();
    EXPECT_GT(summary.max_memory_bytes, 0U);

    const auto removed = std::filesystem::remove_all(temp_root, ec);
    static_cast<void>(removed);
}

} // namespace

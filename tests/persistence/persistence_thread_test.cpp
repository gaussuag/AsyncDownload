#include "asyncdownload/types.hpp"
#include "core/block_bitmap.hpp"
#include "core/memory_accounting.hpp"
#include "core/models.hpp"
#include "metadata/metadata_store.hpp"
#include "persistence/persistence_thread.hpp"
#include "storage/file_writer.hpp"

#include <gtest/gtest.h>

#include <concurrentqueue/blockingconcurrentqueue.h>
#include <thread-pool/BS_thread_pool.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <thread>
#include <vector>

namespace {

void enqueue_data_packet(
    moodycamel::BlockingConcurrentQueue<asyncdownload::core::DataPacket>& queue,
    asyncdownload::core::SessionState& session,
    asyncdownload::core::DataPacket packet) {
    const auto accounted = asyncdownload::core::global_packet_overhead(packet.payload.size(), false);
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

TEST(PersistenceThreadTest, PausesRangeWhenGapExceedsThreshold) {
    asyncdownload::core::global_memory_accounting().reset();

    const auto temp_root = std::filesystem::temp_directory_path() / "asyncdownload_gap_pause_test";
    std::error_code ec;
    std::filesystem::create_directories(temp_root, ec);
    ASSERT_FALSE(ec);

    asyncdownload::DownloadOptions options{};
    options.block_size = 4096;
    options.io_alignment = 4096;
    options.max_gap_bytes = 4096;
    options.flush_threshold_bytes = 4096;
    options.flush_interval = std::chrono::milliseconds(10);

    asyncdownload::core::SessionState session{};
    session.paths.output_path = temp_root / "output.bin";
    session.paths.temporary_path = temp_root / "output.bin.part";
    session.paths.metadata_path = temp_root / "output.bin.config.json";
    session.url = "http://127.0.0.1/test.bin";
    session.options = options;
    session.total_size = 12 * 1024;
    session.accept_ranges = true;

    moodycamel::BlockingConcurrentQueue<asyncdownload::core::DataPacket> queue(16);
    asyncdownload::core::AtomicBlockBitmap bitmap(
        asyncdownload::core::required_block_count(session.total_size, options.block_size));
    asyncdownload::storage::FileWriter writer;
    ASSERT_FALSE(writer.open(session.paths.temporary_path, session.total_size, false, true));
    asyncdownload::metadata::MetadataStore store(session.paths.metadata_path);
    BS::thread_pool<> workers(1);
    asyncdownload::persistence::PersistenceThread persistence(
        session, queue, bitmap, writer, store, workers);

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

    asyncdownload::DownloadOptions options{};
    options.block_size = 64 * 1024;
    options.io_alignment = 4096;
    options.max_gap_bytes = 64 * 1024;
    options.flush_threshold_bytes = 4096;
    options.flush_interval = std::chrono::milliseconds(10);

    asyncdownload::core::SessionState session{};
    session.paths.output_path = temp_root / "output.bin";
    session.paths.temporary_path = temp_root / "output.bin.part";
    session.paths.metadata_path = temp_root / "output.bin.config.json";
    session.url = "http://127.0.0.1/test.bin";
    session.options = options;
    session.total_size = 12 * 1024;
    session.accept_ranges = true;

    moodycamel::BlockingConcurrentQueue<asyncdownload::core::DataPacket> queue(16);
    asyncdownload::core::AtomicBlockBitmap bitmap(
        asyncdownload::core::required_block_count(session.total_size, options.block_size));
    asyncdownload::storage::FileWriter writer;
    ASSERT_FALSE(writer.open(session.paths.temporary_path, session.total_size, false, true));
    asyncdownload::metadata::MetadataStore store(session.paths.metadata_path);
    BS::thread_pool<> workers(1);
    asyncdownload::persistence::PersistenceThread persistence(
        session, queue, bitmap, writer, store, workers);

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

TEST(PersistenceThreadTest, ClearsGapPauseAfterMissingDataArrives) {
    asyncdownload::core::global_memory_accounting().reset();

    const auto temp_root = std::filesystem::temp_directory_path() / "asyncdownload_gap_resume_test";
    std::error_code ec;
    std::filesystem::create_directories(temp_root, ec);
    ASSERT_FALSE(ec);

    asyncdownload::DownloadOptions options{};
    options.block_size = 4096;
    options.io_alignment = 4096;
    options.max_gap_bytes = 4096;
    options.flush_threshold_bytes = 4096;
    options.flush_interval = std::chrono::milliseconds(10);

    asyncdownload::core::SessionState session{};
    session.paths.output_path = temp_root / "output.bin";
    session.paths.temporary_path = temp_root / "output.bin.part";
    session.paths.metadata_path = temp_root / "output.bin.config.json";
    session.url = "http://127.0.0.1/test.bin";
    session.options = options;
    session.total_size = 12 * 1024;
    session.accept_ranges = true;

    moodycamel::BlockingConcurrentQueue<asyncdownload::core::DataPacket> queue(16);
    asyncdownload::core::AtomicBlockBitmap bitmap(
        asyncdownload::core::required_block_count(session.total_size, options.block_size));
    asyncdownload::storage::FileWriter writer;
    ASSERT_FALSE(writer.open(session.paths.temporary_path, session.total_size, false, true));
    asyncdownload::metadata::MetadataStore store(session.paths.metadata_path);
    BS::thread_pool<> workers(1);
    asyncdownload::persistence::PersistenceThread persistence(
        session, queue, bitmap, writer, store, workers);

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

    const auto removed = std::filesystem::remove_all(temp_root, ec);
    static_cast<void>(removed);
}

} // namespace

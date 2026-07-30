#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <thread-pool/BS_thread_pool.hpp>

#include "core/block_bitmap.hpp"
#include "core/models.hpp"
#include "flow/packet_flow.hpp"
#include "metadata/metadata_store.hpp"
#include "persistence/persistence_thread.hpp"
#include "storage/file_writer.hpp"

namespace {

struct TestPacket {
    std::size_t range_id = 0;
    std::int64_t offset = 0;
    std::vector<std::uint8_t> payload;
};

void enqueue_data_packet(
    asyncdownload::flow::PacketProducer& producer,
    asyncdownload::flow::ProducerLane& lane,
    asyncdownload::core::SessionState& session,
    const TestPacket& packet) {
    const auto accepted = producer.accept(
        lane,
        {
            {
                {
                    static_cast<std::uint64_t>(packet.range_id)
                },
                0
            },
            {0, session.total_size},
            packet.offset,
            packet.payload
        });
    ASSERT_TRUE(accepted.accepted());
    const auto flushed = producer.flush(lane);
    ASSERT_TRUE(flushed.accepted());
}

void enqueue_range_complete(
    asyncdownload::flow::PacketProducer& producer,
    asyncdownload::core::SessionState& session,
    const std::size_t range_id) {
    const auto published = producer.publish({
        asyncdownload::flow::ControlPacketKind::range_complete,
        {
            {
                static_cast<std::uint64_t>(range_id)
            },
            0
        },
        session.total_size
    });
    ASSERT_EQ(
        published.code,
        asyncdownload::flow::PacketPublishCode::published);
}

std::unique_ptr<asyncdownload::flow::PacketFlow> make_packet_flow(
    asyncdownload::core::SessionState& session) {
    std::unique_ptr<asyncdownload::flow::PacketFlow> packet_flow;
    EXPECT_FALSE(asyncdownload::flow::PacketFlow::create(
        session.effective_policy.flow_control(),
        session.telemetry_session_,
        packet_flow));
    return packet_flow;
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

    auto packet_flow = make_packet_flow(session);
    asyncdownload::flow::ProducerLane packet_lane;
    ASSERT_FALSE(packet_flow->producer().open_lane(packet_lane));
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
        packet_flow->consumer(),
        bitmap,
        writer,
        store,
        workers);

    asyncdownload::core::RangeContext range(0, 0, total_size - 1);
    persistence.register_range(&range);
    persistence.start();

    TestPacket packet{};
    packet.range_id = 0;
    packet.offset = 0;
    packet.payload.assign(static_cast<std::size_t>(total_size), 0x5A);
    enqueue_data_packet(
        packet_flow->producer(), packet_lane, session, packet);
    enqueue_range_complete(packet_flow->producer(), session, 0);

    ASSERT_TRUE(wait_for_condition([&range]() {
        return range.marked_finished.load(std::memory_order_acquire);
    }, std::chrono::milliseconds(1000)));

    ASSERT_FALSE(packet_flow->producer().close());
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

    auto packet_flow = make_packet_flow(session);
    asyncdownload::flow::ProducerLane packet_lane;
    ASSERT_FALSE(packet_flow->producer().open_lane(packet_lane));
    asyncdownload::core::AtomicBlockBitmap bitmap(
        asyncdownload::core::required_block_count(session.total_size, policy.block_bytes));
    asyncdownload::storage::FileWriter writer;
    ASSERT_FALSE(writer.open(session.paths.temporary_path, session.total_size, false, true));
    asyncdownload::metadata::MetadataStore store(session.paths.metadata_path);
    BS::thread_pool<> workers(1);
    asyncdownload::persistence::PersistenceThread persistence(
        session, policy, packet_flow->consumer(), bitmap, writer, store, workers);

    asyncdownload::core::RangeContext range(0, 0, session.total_size - 1);
    persistence.register_range(&range);
    persistence.start();

    TestPacket packet{};
    packet.range_id = 0;
    packet.offset = 8 * 1024;
    packet.payload.assign(4096, 0x33);
    enqueue_data_packet(
        packet_flow->producer(), packet_lane, session, packet);

    EXPECT_TRUE(wait_for_condition([&range]() {
        return range.pause_for_gap.load(std::memory_order_acquire);
    }, std::chrono::milliseconds(1000)));

    ASSERT_FALSE(packet_flow->producer().close());
    persistence.stop();
    persistence.join();
    writer.close();

    const auto removed = std::filesystem::remove_all(temp_root, ec);
    static_cast<void>(removed);
}

TEST(PersistenceThreadTest, MarksPartiallyPersistedBlocksAsDownloading) {

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

    auto packet_flow = make_packet_flow(session);
    asyncdownload::flow::ProducerLane packet_lane;
    ASSERT_FALSE(packet_flow->producer().open_lane(packet_lane));
    asyncdownload::core::AtomicBlockBitmap bitmap(
        asyncdownload::core::required_block_count(session.total_size, policy.block_bytes));
    asyncdownload::storage::FileWriter writer;
    ASSERT_FALSE(writer.open(session.paths.temporary_path, session.total_size, false, true));
    asyncdownload::metadata::MetadataStore store(session.paths.metadata_path);
    BS::thread_pool<> workers(1);
    asyncdownload::persistence::PersistenceThread persistence(
        session, policy, packet_flow->consumer(), bitmap, writer, store, workers);

    asyncdownload::core::RangeContext range(0, 0, session.total_size - 1);
    persistence.register_range(&range);
    persistence.start();

    TestPacket packet{};
    packet.range_id = 0;
    packet.offset = 0;
    packet.payload.assign(4096, 0x11);
    enqueue_data_packet(
        packet_flow->producer(), packet_lane, session, packet);

    EXPECT_TRUE(wait_for_condition([&bitmap]() {
        return bitmap.load(0) == asyncdownload::core::BlockState::downloading;
    }, std::chrono::milliseconds(1000)));

    ASSERT_FALSE(packet_flow->producer().close());
    persistence.stop();
    persistence.join();
    writer.close();

    EXPECT_FALSE(persistence.error());
    EXPECT_EQ(bitmap.load(0), asyncdownload::core::BlockState::downloading);

    const auto removed = std::filesystem::remove_all(temp_root, ec);
    static_cast<void>(removed);
}

TEST(PersistenceThreadTest, DrainsQueuedPacketsAfterPersistence) {

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

    auto packet_flow = make_packet_flow(session);
    asyncdownload::flow::ProducerLane packet_lane;
    ASSERT_FALSE(packet_flow->producer().open_lane(packet_lane));
    asyncdownload::core::AtomicBlockBitmap bitmap(
        asyncdownload::core::required_block_count(session.total_size, policy.block_bytes));
    asyncdownload::storage::FileWriter writer;
    ASSERT_FALSE(writer.open(session.paths.temporary_path, session.total_size, false, true));
    asyncdownload::metadata::MetadataStore store(session.paths.metadata_path);
    BS::thread_pool<> workers(1);
    asyncdownload::persistence::PersistenceThread persistence(
        session, policy, packet_flow->consumer(), bitmap, writer, store, workers);

    asyncdownload::core::RangeContext range(0, 0, session.total_size - 1);
    persistence.register_range(&range);
    persistence.start();

    TestPacket packet{};
    packet.range_id = 0;
    packet.offset = 0;
    packet.payload.assign(4096, 0x7A);
    enqueue_data_packet(
        packet_flow->producer(), packet_lane, session, packet);

    EXPECT_EQ(packet_flow->producer().snapshot().queued_packets, 1U);
    EXPECT_TRUE(wait_for_condition([&session, &packet_flow]() {
        return session.persisted_bytes.load(std::memory_order_acquire) == 4096 &&
            packet_flow->producer().snapshot().queued_packets == 0U;
    }, std::chrono::milliseconds(1000)));

    ASSERT_FALSE(packet_flow->producer().close());
    persistence.stop();
    persistence.join();
    writer.close();

    EXPECT_FALSE(persistence.error());
    EXPECT_EQ(packet_flow->producer().snapshot().queued_packets, 0U);

    const auto removed = std::filesystem::remove_all(temp_root, ec);
    static_cast<void>(removed);
}

TEST(PersistenceThreadTest, CollectsSampledPacketLatencyStats) {

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

    auto packet_flow = make_packet_flow(session);
    asyncdownload::flow::ProducerLane packet_lane;
    ASSERT_FALSE(packet_flow->producer().open_lane(packet_lane));
    asyncdownload::core::AtomicBlockBitmap bitmap(
        asyncdownload::core::required_block_count(session.total_size, policy.block_bytes));
    asyncdownload::storage::FileWriter writer;
    ASSERT_FALSE(writer.open(session.paths.temporary_path, session.total_size, false, true));
    asyncdownload::metadata::MetadataStore store(session.paths.metadata_path);
    BS::thread_pool<> workers(1);
    asyncdownload::persistence::PersistenceThread persistence(
        session, policy, packet_flow->consumer(), bitmap, writer, store, workers);

    asyncdownload::core::RangeContext range(0, 0, session.total_size - 1);
    persistence.register_range(&range);
    persistence.start();

    TestPacket packet{};
    packet.range_id = 0;
    packet.offset = 0;
    packet.payload.assign(4096, 0x55);
    enqueue_data_packet(
        packet_flow->producer(), packet_lane, session, packet);

    EXPECT_TRUE(wait_for_condition([&session]() {
        return session.persisted_bytes.load(std::memory_order_acquire) == 4096;
    }, std::chrono::milliseconds(1000)));

    ASSERT_FALSE(packet_flow->producer().close());
    persistence.stop();
    persistence.join();
    writer.close();

    EXPECT_FALSE(persistence.error());
    EXPECT_EQ(session.persisted_bytes.load(std::memory_order_relaxed), 4096);
    const auto summary = session.telemetry_session_.final_summary();
    EXPECT_EQ(summary.max_inflight_bytes, 4096);

    const auto removed = std::filesystem::remove_all(temp_root, ec);
    static_cast<void>(removed);
}

TEST(PersistenceThreadTest, ClearsGapPauseAfterMissingDataArrives) {

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

    auto packet_flow = make_packet_flow(session);
    asyncdownload::flow::ProducerLane packet_lane;
    ASSERT_FALSE(packet_flow->producer().open_lane(packet_lane));
    asyncdownload::core::AtomicBlockBitmap bitmap(
        asyncdownload::core::required_block_count(session.total_size, policy.block_bytes));
    asyncdownload::storage::FileWriter writer;
    ASSERT_FALSE(writer.open(session.paths.temporary_path, session.total_size, false, true));
    asyncdownload::metadata::MetadataStore store(session.paths.metadata_path);
    BS::thread_pool<> workers(1);
    asyncdownload::persistence::PersistenceThread persistence(
        session, policy, packet_flow->consumer(), bitmap, writer, store, workers);

    asyncdownload::core::RangeContext range(0, 0, session.total_size - 1);
    persistence.register_range(&range);
    persistence.start();

    TestPacket tail_packet{};
    tail_packet.range_id = 0;
    tail_packet.offset = 8 * 1024;
    tail_packet.payload.assign(4096, 0x44);
    enqueue_data_packet(
        packet_flow->producer(), packet_lane, session, tail_packet);

    ASSERT_TRUE(wait_for_condition([&range]() {
        return range.pause_for_gap.load(std::memory_order_acquire);
    }, std::chrono::milliseconds(1000)));

    TestPacket head_packet{};
    head_packet.range_id = 0;
    head_packet.offset = 0;
    head_packet.payload.assign(8 * 1024, 0x22);
    enqueue_data_packet(
        packet_flow->producer(), packet_lane, session, head_packet);

    EXPECT_TRUE(wait_for_condition([&range, &session]() {
        return !range.pause_for_gap.load(std::memory_order_acquire) &&
            session.persisted_bytes.load(std::memory_order_acquire) == 12 * 1024;
    }, std::chrono::milliseconds(1000)));

    enqueue_range_complete(packet_flow->producer(), session, 0);

    EXPECT_TRUE(wait_for_condition([&range]() {
        return range.marked_finished.load(std::memory_order_acquire);
    }, std::chrono::milliseconds(1000)));

    ASSERT_FALSE(packet_flow->producer().close());
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

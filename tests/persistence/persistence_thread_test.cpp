#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
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
    std::uint64_t generation = 1;
    asyncdownload::range::ByteSpan lease_span{};
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
                packet.generation
            },
            packet.lease_span.begin <
                    packet.lease_span.end ?
                packet.lease_span :
                asyncdownload::range::ByteSpan{
                    0,
                    session.total_size
                },
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
            1
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
    ASSERT_FALSE(persistence.register_range(&range));
    persistence.start();

    TestPacket packet{};
    packet.range_id = 0;
    packet.offset = 0;
    packet.payload.assign(static_cast<std::size_t>(total_size), 0x5A);
    enqueue_data_packet(
        packet_flow->producer(), packet_lane, session, packet);
    EXPECT_FALSE(range.marked_finished.load(
        std::memory_order_acquire));
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

struct PersistenceScenarioResult {
    std::error_code error;
    asyncdownload::flow::PacketFlowSnapshot flow;
    std::int64_t persisted_through = 0;
    bool marked_finished = false;
};

PersistenceScenarioResult run_persistence_scenario(
    const std::vector<TestPacket>& packets,
    const std::vector<
        asyncdownload::flow::ControlPacket>& controls = {}) {
    static std::atomic<std::uint64_t> sequence{0};
    const auto temp_root =
        std::filesystem::temp_directory_path() /
        ("asyncdownload_persistence_scenario_" +
         std::to_string(sequence.fetch_add(
             1,
             std::memory_order_relaxed)));
    std::error_code filesystem_error;
    std::filesystem::create_directories(
        temp_root,
        filesystem_error);
    EXPECT_FALSE(filesystem_error);

    asyncdownload::download::PersistencePolicy policy{};
    policy.block_bytes = 4096;
    policy.io_alignment_bytes = 4096;
    policy.max_gap_bytes = 4096;
    policy.flush_threshold_bytes = 4096;
    policy.flush_interval = std::chrono::milliseconds(10);
    asyncdownload::core::SessionState session(
        make_effective_policy(policy, 4096));
    session.paths.output_path = temp_root / "output.bin";
    session.paths.temporary_path =
        temp_root / "output.bin.part";
    session.paths.metadata_path =
        temp_root / "output.bin.config.json";
    session.url = "http://127.0.0.1/test.bin";
    auto packet_flow = make_packet_flow(session);
    asyncdownload::flow::ProducerLane lane;
    EXPECT_FALSE(
        packet_flow->producer().open_lane(lane));
    asyncdownload::core::AtomicBlockBitmap bitmap(1);
    asyncdownload::storage::FileWriter writer;
    EXPECT_FALSE(writer.open(
        session.paths.temporary_path,
        session.total_size,
        false,
        true));
    asyncdownload::metadata::MetadataStore store(
        session.paths.metadata_path);
    BS::thread_pool<> workers(1);
    asyncdownload::persistence::PersistenceThread persistence(
        session,
        policy,
        packet_flow->consumer(),
        bitmap,
        writer,
        store,
        workers,
        1);
    asyncdownload::core::RangeContext range(0, 0, 4095);
    EXPECT_FALSE(persistence.register_range(&range));
    persistence.start();

    for (const auto& packet : packets) {
        enqueue_data_packet(
            packet_flow->producer(),
            lane,
            session,
            packet);
        if (persistence.error()) {
            break;
        }
    }
    for (const auto& control : controls) {
        const auto published =
            packet_flow->producer().publish(control);
        EXPECT_EQ(
            published.code,
            asyncdownload::flow::
                PacketPublishCode::published);
        if (published.code !=
            asyncdownload::flow::
                PacketPublishCode::published) {
            break;
        }
    }
    const auto close_error =
        packet_flow->producer().close();
    static_cast<void>(close_error);
    persistence.stop();
    persistence.join();
    PersistenceScenarioResult result{
        persistence.error(),
        packet_flow->producer().snapshot(),
        range.persisted_offset,
        range.marked_finished.load(
            std::memory_order_acquire)
    };
    writer.close();
    const auto removed = std::filesystem::remove_all(
        temp_root,
        filesystem_error);
    static_cast<void>(removed);
    return result;
}

TEST(PersistenceThreadTest, AcceptsValidatedAlignmentAtTailCapacity) {
    const auto temp_root =
        std::filesystem::temp_directory_path() /
        "asyncdownload_tail_capacity_test";
    persist_single_range_at_tail_capacity(
        temp_root,
        asyncdownload::core::TAIL_BUFFER_CAPACITY_BYTES);
}

TEST(
    PersistenceThreadTest,
    RejectsFirstLeaseGenerationOtherThanOne) {
    TestPacket packet{};
    packet.generation = 2;
    packet.offset = 0;
    packet.payload.assign(512, 0x31);

    const auto result =
        run_persistence_scenario({packet});

    EXPECT_TRUE(result.error);
}

TEST(
    PersistenceThreadTest,
    RejectsSpanChangeWithinLeaseGeneration) {
    TestPacket first{};
    first.generation = 1;
    first.lease_span = {0, 2048};
    first.offset = 0;
    first.payload.assign(512, 0x32);
    TestPacket second{};
    second.generation = 1;
    second.lease_span = {0, 4096};
    second.offset = 512;
    second.payload.assign(512, 0x33);

    const auto result =
        run_persistence_scenario({first, second});

    EXPECT_TRUE(result.error);
}

TEST(
    PersistenceThreadTest,
    RejectsLeaseGenerationJump) {
    TestPacket first{};
    first.generation = 1;
    first.lease_span = {0, 512};
    first.offset = 0;
    first.payload.assign(512, 0x34);
    TestPacket jumped{};
    jumped.generation = 3;
    jumped.lease_span = {512, 1024};
    jumped.offset = 512;
    jumped.payload.assign(512, 0x35);

    const auto result =
        run_persistence_scenario({first, jumped});

    EXPECT_TRUE(result.error);
}

TEST(
    PersistenceThreadTest,
    RejectsOverlapBetweenConsecutiveLeaseSpans) {
    TestPacket first{};
    first.generation = 1;
    first.lease_span = {0, 512};
    first.offset = 0;
    first.payload.assign(512, 0x36);
    TestPacket overlapping{};
    overlapping.generation = 2;
    overlapping.lease_span = {256, 768};
    overlapping.offset = 512;
    overlapping.payload.assign(256, 0x37);

    const auto result =
        run_persistence_scenario({first, overlapping});

    EXPECT_TRUE(result.error);
}

TEST(
    PersistenceThreadTest,
    DiscardsFullyPersistedDuplicateData) {
    TestPacket packet{};
    packet.generation = 1;
    packet.lease_span = {0, 512};
    packet.offset = 0;
    packet.payload.assign(512, 0x38);

    const auto result =
        run_persistence_scenario({packet, packet});

    EXPECT_FALSE(result.error);
    EXPECT_EQ(result.flow.queued_packets, 0U);
    EXPECT_EQ(result.flow.accounted_bytes, 0U);
}

TEST(
    PersistenceThreadTest,
    DiscardsExactDuplicateBufferedData) {
    TestPacket buffered{};
    buffered.generation = 1;
    buffered.lease_span = {0, 4096};
    buffered.offset = 2048;
    buffered.payload.assign(512, 0x39);
    TestPacket head{};
    head.generation = 1;
    head.lease_span = {0, 4096};
    head.offset = 0;
    head.payload.assign(2048, 0x3A);

    const auto result =
        run_persistence_scenario({buffered, buffered, head});

    EXPECT_FALSE(result.error);
    EXPECT_EQ(result.flow.queued_packets, 0U);
    EXPECT_EQ(result.flow.accounted_bytes, 0U);
}

TEST(
    PersistenceThreadTest,
    RejectsPartiallyPersistedOverlap) {
    TestPacket first{};
    first.generation = 1;
    first.lease_span = {0, 4096};
    first.offset = 0;
    first.payload.assign(1024, 0x3B);
    TestPacket overlapping{};
    overlapping.generation = 1;
    overlapping.lease_span = {0, 4096};
    overlapping.offset = 512;
    overlapping.payload.assign(1024, 0x3C);

    const auto result =
        run_persistence_scenario({first, overlapping});

    EXPECT_TRUE(result.error);
    EXPECT_EQ(result.flow.queued_packets, 0U);
    EXPECT_EQ(result.flow.accounted_bytes, 0U);
}

TEST(
    PersistenceThreadTest,
    DiscardsStaleGenerationAlreadyPersisted) {
    TestPacket first{};
    first.generation = 1;
    first.lease_span = {0, 512};
    first.offset = 0;
    first.payload.assign(512, 0x3D);
    TestPacket second{};
    second.generation = 2;
    second.lease_span = {512, 1024};
    second.offset = 512;
    second.payload.assign(512, 0x3E);

    const auto result =
        run_persistence_scenario({first, second, first});

    EXPECT_FALSE(result.error);
    EXPECT_EQ(result.flow.queued_packets, 0U);
    EXPECT_EQ(result.flow.accounted_bytes, 0U);
}

TEST(
    PersistenceThreadTest,
    RejectsLeaseOutsideRegisteredGeometry) {
    TestPacket packet{};
    packet.generation = 1;
    packet.lease_span = {4096, 4608};
    packet.offset = 4096;
    packet.payload.assign(512, 0x3F);

    const auto result =
        run_persistence_scenario({packet});

    EXPECT_TRUE(result.error);
    EXPECT_EQ(result.flow.queued_packets, 0U);
    EXPECT_EQ(result.flow.accounted_bytes, 0U);
}

TEST(
    PersistenceThreadTest,
    RejectsCompletionGenerationMismatch) {
    TestPacket packet{};
    packet.generation = 1;
    packet.lease_span = {0, 4096};
    packet.offset = 0;
    packet.payload.assign(4096, 0x40);
    const asyncdownload::flow::ControlPacket completion{
        asyncdownload::flow::ControlPacketKind::range_complete,
        {{0}, 2},
        4096
    };

    const auto result =
        run_persistence_scenario({packet}, {completion});

    EXPECT_TRUE(result.error);
    EXPECT_FALSE(result.marked_finished);
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
    ASSERT_FALSE(persistence.register_range(&range));
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
    ASSERT_FALSE(persistence.register_range(&range));
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
    ASSERT_FALSE(persistence.register_range(&range));
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
    ASSERT_FALSE(persistence.register_range(&range));
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
    ASSERT_FALSE(persistence.register_range(&range));
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

TEST(
    PersistenceThreadTest,
    AcknowledgesRegisteredAndResizedGeometryInTicketOrder) {
    asyncdownload::download::PersistencePolicy policy{};
    policy.block_bytes = 4096;
    policy.io_alignment_bytes = 4096;
    policy.max_gap_bytes = 4096;
    policy.flush_threshold_bytes = 4096;
    policy.flush_interval = std::chrono::milliseconds(10);

    asyncdownload::core::SessionState session(
        make_effective_policy(policy, 4096));
    const auto temp_root =
        std::filesystem::temp_directory_path() /
        "asyncdownload_geometry_ack_test";
    std::error_code filesystem_error;
    std::filesystem::create_directories(
        temp_root,
        filesystem_error);
    ASSERT_FALSE(filesystem_error);
    session.paths.output_path = temp_root / "output.bin";
    session.paths.temporary_path =
        temp_root / "output.bin.part";
    session.paths.metadata_path =
        temp_root / "output.bin.config.json";
    session.url = "http://127.0.0.1/test.bin";
    auto packet_flow = make_packet_flow(session);
    asyncdownload::core::AtomicBlockBitmap bitmap(1);
    asyncdownload::storage::FileWriter writer;
    ASSERT_FALSE(writer.open(
        session.paths.temporary_path,
        session.total_size,
        false,
        true));
    asyncdownload::metadata::MetadataStore store(
        session.paths.metadata_path);
    BS::thread_pool<> workers(1);
    asyncdownload::persistence::PersistenceThread persistence(
        session,
        policy,
        packet_flow->consumer(),
        bitmap,
        writer,
        store,
        workers,
        1);
    asyncdownload::core::RangeContext range(0, 0, 4095);
    ASSERT_FALSE(persistence.register_range(&range));
    persistence.start();

    const auto registered = persistence.submit_range_geometry(
        asyncdownload::persistence::RangeGeometryCommand{
            asyncdownload::range::RegisterRangeEffect{
                {0},
                {0, 4096},
                0
            }
        });
    std::optional<
        asyncdownload::persistence::RangeRegistrationAck>
        registration_ack;
    const auto observed_registration = wait_for_condition(
        [&persistence, &registration_ack]() {
            const auto polled =
                persistence.poll_range_geometry_ack();
            if (polled.error || !polled.ack.has_value()) {
                return false;
            }
            registration_ack = *polled.ack;
            return true;
        },
        std::chrono::milliseconds(1000));

    range.end_offset.store(2047, std::memory_order_release);
    const auto resized = persistence.submit_range_geometry(
        asyncdownload::persistence::RangeGeometryCommand{
            asyncdownload::range::ResizeRangeEffect{
                {0},
                2048,
                1
            }
        });
    std::optional<
        asyncdownload::persistence::RangeRegistrationAck>
        resize_ack;
    const auto observed_resize = wait_for_condition(
        [&persistence, &resize_ack]() {
            const auto polled =
                persistence.poll_range_geometry_ack();
            if (polled.error || !polled.ack.has_value()) {
                return false;
            }
            resize_ack = *polled.ack;
            return true;
        },
        std::chrono::milliseconds(1000));

    const auto close_error = packet_flow->producer().close();
    persistence.stop();
    persistence.join();
    writer.close();

    EXPECT_FALSE(close_error);
    ASSERT_FALSE(registered.error);
    ASSERT_FALSE(resized.error);
    EXPECT_EQ(registered.ticket, 1U);
    EXPECT_EQ(resized.ticket, 2U);
    ASSERT_TRUE(observed_registration);
    ASSERT_TRUE(registration_ack.has_value());
    EXPECT_EQ(registration_ack->ticket, registered.ticket);
    EXPECT_EQ(registration_ack->range, (asyncdownload::range::RangeId{0}));
    EXPECT_EQ(registration_ack->geometry_revision, 0U);
    EXPECT_FALSE(registration_ack->error);
    ASSERT_TRUE(observed_resize);
    ASSERT_TRUE(resize_ack.has_value());
    EXPECT_EQ(resize_ack->ticket, resized.ticket);
    EXPECT_EQ(resize_ack->range, (asyncdownload::range::RangeId{0}));
    EXPECT_EQ(resize_ack->geometry_revision, 1U);
    EXPECT_FALSE(resize_ack->error);
    EXPECT_FALSE(persistence.error());
    const auto removed = std::filesystem::remove_all(
        temp_root,
        filesystem_error);
    static_cast<void>(removed);
}

} // namespace

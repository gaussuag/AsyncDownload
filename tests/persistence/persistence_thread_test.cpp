#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <thread-pool/BS_thread_pool.hpp>

#include "asyncdownload/error.hpp"
#include "core/block_bitmap.hpp"
#include "core/models.hpp"
#include "flow/packet_flow.hpp"
#include "persistence/persistence_thread.hpp"
#include "recovery/recovery_checkpoint.hpp"

namespace {

static_assert(std::is_constructible_v<
    asyncdownload::persistence::PersistenceThread,
    asyncdownload::core::SessionState&,
    asyncdownload::download::PersistencePolicy,
    asyncdownload::flow::PacketConsumer&,
    asyncdownload::core::AtomicBlockBitmap&,
    asyncdownload::recovery::RecoveryCheckpoint&,
    BS::thread_pool<>&,
    std::size_t>);

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

void submit_range_registration(
    asyncdownload::persistence::PersistenceThread& persistence,
    asyncdownload::range::RangeFactSlot& facts,
    const std::int64_t total_size,
    const std::uint64_t range_id = 0) {
    const auto submitted = persistence.submit_range_geometry(
        asyncdownload::range::RegisterRangeEffect{
            {range_id},
            {0, total_size},
            0,
            facts.publisher()
        });
    ASSERT_FALSE(submitted.error);
    ASSERT_NE(submitted.ticket, 0U);
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

std::unique_ptr<
    asyncdownload::recovery::RecoveryCheckpoint>
open_checkpoint(
    const asyncdownload::core::SessionState& session) {
    asyncdownload::recovery::RecoveryOpenRequest request{};
    request.paths = session.paths;
    request.remote.url = session.url;
    request.remote.total_size = session.total_size;
    request.remote.accept_ranges =
        session.effective_policy.remote_facts().
            accept_ranges;
    request.remote.etag = session.etag;
    request.remote.last_modified =
        session.last_modified;
    request.policy =
        session.effective_policy.recovery_identity();
    request.overwrite_existing =
        session.effective_policy.persistence().
            overwrite_existing;
    auto opened =
        asyncdownload::recovery::RecoveryCheckpoint::open(
            request);
    EXPECT_FALSE(opened.error);
    EXPECT_NE(opened.checkpoint, nullptr);
    return std::move(opened.checkpoint);
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
    auto checkpoint = open_checkpoint(session);
    ASSERT_NE(checkpoint, nullptr);
    BS::thread_pool<> workers(1);
    asyncdownload::persistence::PersistenceThread persistence(
        session,
        policy,
        packet_flow->consumer(),
        bitmap,
        *checkpoint,
        workers);

    asyncdownload::range::RangeFactSlot facts({0}, 0);
    submit_range_registration(persistence, facts, total_size);
    persistence.start();

    TestPacket packet{};
    packet.range_id = 0;
    packet.offset = 0;
    packet.payload.assign(static_cast<std::size_t>(total_size), 0x5A);
    enqueue_data_packet(
        packet_flow->producer(), packet_lane, session, packet);
    EXPECT_FALSE(facts.read_since(0).has_value());
    enqueue_range_complete(packet_flow->producer(), session, 0);

    ASSERT_TRUE(wait_for_condition([&facts]() {
        const auto snapshot = facts.read_since(0);
        return snapshot.has_value() &&
            snapshot->committed_generation == 1;
    }, std::chrono::milliseconds(1000)));

    ASSERT_FALSE(packet_flow->producer().close());
    persistence.stop();
    persistence.join();
    EXPECT_FALSE(persistence.error());
    EXPECT_EQ(
        session.persisted_bytes.load(std::memory_order_relaxed),
        total_size);
    EXPECT_EQ(
        session.telemetry_session_.
            current_snapshot().persisted_bytes,
        total_size);

    checkpoint->close_preserving_artifacts();
    std::ifstream stored_file(
        session.paths.temporary_path,
        std::ios::binary);
    ASSERT_TRUE(stored_file.is_open());
    const std::vector<char> stored{
        std::istreambuf_iterator<char>(stored_file),
        std::istreambuf_iterator<char>()
    };
    ASSERT_EQ(stored.size(), static_cast<std::size_t>(total_size));
    for (const auto byte : stored) {
        EXPECT_EQ(
            static_cast<unsigned char>(byte),
            0x5A);
    }

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
    asyncdownload::ProgressSnapshot telemetry;
    std::int64_t absolute_persisted = 0;
    std::int64_t persisted_through = 0;
    bool committed = false;
};

PersistenceScenarioResult run_persistence_scenario(
    const std::vector<TestPacket>& packets,
    const std::vector<
        asyncdownload::flow::ControlPacket>& controls = {},
    const bool close_checkpoint_before_start = false,
    const std::int64_t total_size = 4096,
    const std::int64_t initial_persisted = 0) {
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
        make_effective_policy(policy, total_size));
    session.paths.output_path = temp_root / "output.bin";
    session.paths.temporary_path =
        temp_root / "output.bin.part";
    session.paths.metadata_path =
        temp_root / "output.bin.config.json";
    session.url = "http://127.0.0.1/test.bin";
    session.persisted_bytes.store(
        initial_persisted,
        std::memory_order_relaxed);
    session.telemetry_session_.record_task_started();
    auto packet_flow = make_packet_flow(session);
    asyncdownload::flow::ProducerLane lane;
    EXPECT_FALSE(
        packet_flow->producer().open_lane(lane));
    asyncdownload::core::AtomicBlockBitmap bitmap(
        asyncdownload::core::required_block_count(
            total_size,
            policy.block_bytes));
    auto checkpoint = open_checkpoint(session);
    EXPECT_NE(checkpoint, nullptr);
    BS::thread_pool<> workers(1);
    asyncdownload::persistence::PersistenceThread persistence(
        session,
        policy,
        packet_flow->consumer(),
        bitmap,
        *checkpoint,
        workers,
        1);
    asyncdownload::range::RangeFactSlot facts({0}, 0);
    submit_range_registration(
        persistence,
        facts,
        total_size);
    if (!close_checkpoint_before_start) {
        persistence.start();
    }

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
    if (close_checkpoint_before_start) {
        checkpoint->close_preserving_artifacts();
        persistence.start();
    }
    persistence.stop();
    persistence.join();
    const auto fact_snapshot = facts.read_since(0);
    PersistenceScenarioResult result{
        persistence.error(),
        packet_flow->producer().snapshot(),
        session.telemetry_session_.current_snapshot(),
        session.persisted_bytes.load(
            std::memory_order_relaxed),
        fact_snapshot.has_value() ?
            fact_snapshot->persisted_through :
            0,
        fact_snapshot.has_value() &&
            fact_snapshot->committed_generation != 0
    };
    checkpoint->close_preserving_artifacts();
    const auto removed = std::filesystem::remove_all(
        temp_root,
        filesystem_error);
    static_cast<void>(removed);
    return result;
}

TEST(PersistenceThreadTest, RecordsOnlyNewSuccessfulWritesAfterRestoredBase) {
    TestPacket packet{};
    packet.offset = 0;
    packet.payload.assign(4096, 0x2A);

    const auto result = run_persistence_scenario(
        {packet},
        {},
        false,
        12 * 1024,
        8 * 1024);

    EXPECT_FALSE(result.error);
    EXPECT_EQ(result.absolute_persisted, 12 * 1024);
    EXPECT_EQ(result.telemetry.persisted_bytes, 4096);
}

TEST(PersistenceThreadTest, RejectsAbsolutePersistedCounterOverflow) {
    TestPacket packet{};
    packet.offset = 0;
    packet.payload.assign(4096, 0x2B);
    const auto initial =
        std::numeric_limits<std::int64_t>::max() - 2048;

    const auto result = run_persistence_scenario(
        {packet},
        {},
        false,
        4096,
        initial);

    EXPECT_EQ(
        result.error,
        asyncdownload::make_error_code(
            asyncdownload::DownloadErrc::internal_error));
    EXPECT_EQ(result.absolute_persisted, initial);
    EXPECT_EQ(result.telemetry.persisted_bytes, 0);
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
    EXPECT_FALSE(result.committed);
    EXPECT_EQ(result.absolute_persisted, 0);
    EXPECT_EQ(result.telemetry.persisted_bytes, 0);
}

TEST(
    PersistenceThreadTest,
    DrainsBufferedAccountingAfterTerminalError) {
    TestPacket buffered{};
    buffered.generation = 1;
    buffered.lease_span = {0, 4096};
    buffered.offset = 2048;
    buffered.payload.assign(512, 0x41);
    TestPacket jumped{};
    jumped.generation = 3;
    jumped.lease_span = {0, 4096};
    jumped.offset = 0;
    jumped.payload.assign(512, 0x42);

    const auto result =
        run_persistence_scenario({buffered, jumped});

    EXPECT_TRUE(result.error);
    EXPECT_EQ(result.flow.queued_packets, 0U);
    EXPECT_EQ(result.flow.accounted_bytes, 0U);
}

TEST(
    PersistenceThreadTest,
    DrainsOriginalPacketAfterConflictingMapKey) {
    TestPacket buffered{};
    buffered.generation = 1;
    buffered.lease_span = {0, 4096};
    buffered.offset = 2048;
    buffered.payload.assign(512, 0x43);
    TestPacket conflicting = buffered;
    conflicting.payload.assign(256, 0x44);

    const auto result =
        run_persistence_scenario({buffered, conflicting});

    EXPECT_TRUE(result.error);
    EXPECT_EQ(result.flow.queued_packets, 0U);
    EXPECT_EQ(result.flow.accounted_bytes, 0U);
}

TEST(
    PersistenceThreadTest,
    RejectsCompletionWhileGapRemains) {
    TestPacket buffered{};
    buffered.generation = 1;
    buffered.lease_span = {0, 4096};
    buffered.offset = 2048;
    buffered.payload.assign(512, 0x45);
    const asyncdownload::flow::ControlPacket completion{
        asyncdownload::flow::ControlPacketKind::range_complete,
        {{0}, 1},
        4096
    };

    const auto result =
        run_persistence_scenario({buffered}, {completion});

    EXPECT_TRUE(result.error);
    EXPECT_FALSE(result.committed);
    EXPECT_EQ(result.flow.accounted_bytes, 0U);
}

TEST(
    PersistenceThreadTest,
    RejectsCompletionBeforeFinalPersistedFrontier) {
    TestPacket packet{};
    packet.generation = 1;
    packet.lease_span = {0, 4096};
    packet.offset = 0;
    packet.payload.assign(512, 0x46);
    const asyncdownload::flow::ControlPacket completion{
        asyncdownload::flow::ControlPacketKind::range_complete,
        {{0}, 1},
        4096
    };

    const auto result =
        run_persistence_scenario({packet}, {completion});

    EXPECT_TRUE(result.error);
    EXPECT_FALSE(result.committed);
    EXPECT_EQ(result.persisted_through, 512);
}

TEST(
    PersistenceThreadTest,
    RejectsCompletionExpectedEndMismatch) {
    TestPacket packet{};
    packet.generation = 1;
    packet.lease_span = {0, 4096};
    packet.offset = 0;
    packet.payload.assign(4096, 0x47);
    const asyncdownload::flow::ControlPacket completion{
        asyncdownload::flow::ControlPacketKind::range_complete,
        {{0}, 1},
        2048
    };

    const auto result =
        run_persistence_scenario({packet}, {completion});

    EXPECT_TRUE(result.error);
    EXPECT_FALSE(result.committed);
}

TEST(
    PersistenceThreadTest,
    DoesNotCommitWhenFinalTailWriteFails) {
    TestPacket packet{};
    packet.generation = 1;
    packet.lease_span = {0, 4095};
    packet.offset = 0;
    packet.payload.assign(4095, 0x48);
    const asyncdownload::flow::ControlPacket completion{
        asyncdownload::flow::ControlPacketKind::range_complete,
        {{0}, 1},
        4095
    };

    const auto result = run_persistence_scenario(
        {packet},
        {completion},
        true,
        4095);

    EXPECT_TRUE(result.error);
    EXPECT_FALSE(result.committed);
}

TEST(
    PersistenceThreadTest,
    TreatsDuplicateCompletionAsIdempotent) {
    TestPacket packet{};
    packet.generation = 1;
    packet.lease_span = {0, 4096};
    packet.offset = 0;
    packet.payload.assign(4096, 0x49);
    const asyncdownload::flow::ControlPacket completion{
        asyncdownload::flow::ControlPacketKind::range_complete,
        {{0}, 1},
        4096
    };

    const auto result = run_persistence_scenario(
        {packet},
        {completion, completion});

    EXPECT_FALSE(result.error);
    EXPECT_TRUE(result.committed);
    EXPECT_EQ(result.persisted_through, 4096);
}

TEST(
    PersistenceThreadTest,
    IgnoresStaleCompletionForOlderGeneration) {
    TestPacket first{};
    first.generation = 1;
    first.lease_span = {0, 512};
    first.offset = 0;
    first.payload.assign(512, 0x4A);
    TestPacket second{};
    second.generation = 2;
    second.lease_span = {512, 1024};
    second.offset = 512;
    second.payload.assign(512, 0x4B);
    const asyncdownload::flow::ControlPacket stale{
        asyncdownload::flow::ControlPacketKind::range_complete,
        {{0}, 1},
        4096
    };

    const auto result =
        run_persistence_scenario({first, second}, {stale});

    EXPECT_FALSE(result.error);
    EXPECT_FALSE(result.committed);
    EXPECT_EQ(result.persisted_through, 1024);
}

TEST(
    PersistenceThreadTest,
    RejectsConflictingDuplicateCompletion) {
    TestPacket packet{};
    packet.generation = 1;
    packet.lease_span = {0, 4096};
    packet.offset = 0;
    packet.payload.assign(4096, 0x4C);
    const asyncdownload::flow::ControlPacket completion{
        asyncdownload::flow::ControlPacketKind::range_complete,
        {{0}, 1},
        4096
    };
    const asyncdownload::flow::ControlPacket conflicting{
        asyncdownload::flow::ControlPacketKind::range_complete,
        {{0}, 1},
        2048
    };

    const auto result = run_persistence_scenario(
        {packet},
        {completion, conflicting});

    EXPECT_TRUE(result.error);
    EXPECT_TRUE(result.committed);
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
    auto checkpoint = open_checkpoint(session);
    ASSERT_NE(checkpoint, nullptr);
    BS::thread_pool<> workers(1);
    asyncdownload::persistence::PersistenceThread persistence(
        session,
        policy,
        packet_flow->consumer(),
        bitmap,
        *checkpoint,
        workers);

    asyncdownload::range::RangeFactSlot facts({0}, 0);
    submit_range_registration(
        persistence,
        facts,
        session.total_size);
    persistence.start();

    TestPacket packet{};
    packet.range_id = 0;
    packet.offset = 8 * 1024;
    packet.payload.assign(4096, 0x33);
    enqueue_data_packet(
        packet_flow->producer(), packet_lane, session, packet);

    EXPECT_TRUE(wait_for_condition([&facts]() {
        const auto snapshot = facts.read_since(0);
        return snapshot.has_value() &&
            snapshot->gap_paused;
    }, std::chrono::milliseconds(1000)));

    ASSERT_FALSE(packet_flow->producer().close());
    persistence.stop();
    persistence.join();
    checkpoint->close_preserving_artifacts();

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
    auto checkpoint = open_checkpoint(session);
    ASSERT_NE(checkpoint, nullptr);
    BS::thread_pool<> workers(1);
    asyncdownload::persistence::PersistenceThread persistence(
        session,
        policy,
        packet_flow->consumer(),
        bitmap,
        *checkpoint,
        workers);

    asyncdownload::range::RangeFactSlot facts({0}, 0);
    submit_range_registration(
        persistence,
        facts,
        session.total_size);
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
    checkpoint->close_preserving_artifacts();

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
    auto checkpoint = open_checkpoint(session);
    ASSERT_NE(checkpoint, nullptr);
    BS::thread_pool<> workers(1);
    asyncdownload::persistence::PersistenceThread persistence(
        session,
        policy,
        packet_flow->consumer(),
        bitmap,
        *checkpoint,
        workers);

    asyncdownload::range::RangeFactSlot facts({0}, 0);
    submit_range_registration(
        persistence,
        facts,
        session.total_size);
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
    checkpoint->close_preserving_artifacts();

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
    auto checkpoint = open_checkpoint(session);
    ASSERT_NE(checkpoint, nullptr);
    BS::thread_pool<> workers(1);
    asyncdownload::persistence::PersistenceThread persistence(
        session,
        policy,
        packet_flow->consumer(),
        bitmap,
        *checkpoint,
        workers);

    asyncdownload::range::RangeFactSlot facts({0}, 0);
    submit_range_registration(
        persistence,
        facts,
        session.total_size);
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
    checkpoint->close_preserving_artifacts();

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
    auto checkpoint = open_checkpoint(session);
    ASSERT_NE(checkpoint, nullptr);
    BS::thread_pool<> workers(1);
    asyncdownload::persistence::PersistenceThread persistence(
        session,
        policy,
        packet_flow->consumer(),
        bitmap,
        *checkpoint,
        workers);

    asyncdownload::range::RangeFactSlot facts({0}, 0);
    submit_range_registration(
        persistence,
        facts,
        session.total_size);
    persistence.start();

    TestPacket tail_packet{};
    tail_packet.range_id = 0;
    tail_packet.offset = 8 * 1024;
    tail_packet.payload.assign(4096, 0x44);
    enqueue_data_packet(
        packet_flow->producer(), packet_lane, session, tail_packet);

    ASSERT_TRUE(wait_for_condition([&facts]() {
        const auto snapshot = facts.read_since(0);
        return snapshot.has_value() &&
            snapshot->gap_paused;
    }, std::chrono::milliseconds(1000)));

    TestPacket head_packet{};
    head_packet.range_id = 0;
    head_packet.offset = 0;
    head_packet.payload.assign(8 * 1024, 0x22);
    enqueue_data_packet(
        packet_flow->producer(), packet_lane, session, head_packet);

    EXPECT_TRUE(wait_for_condition([&facts, &session]() {
        const auto snapshot = facts.read_since(0);
        return snapshot.has_value() &&
            !snapshot->gap_paused &&
            session.persisted_bytes.load(std::memory_order_acquire) == 12 * 1024;
    }, std::chrono::milliseconds(1000)));

    enqueue_range_complete(packet_flow->producer(), session, 0);

    EXPECT_TRUE(wait_for_condition([&facts]() {
        const auto snapshot = facts.read_since(0);
        return snapshot.has_value() &&
            snapshot->committed_generation == 1;
    }, std::chrono::milliseconds(1000)));

    ASSERT_FALSE(packet_flow->producer().close());
    persistence.stop();
    persistence.join();
    checkpoint->close_preserving_artifacts();

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
    AcknowledgesGeometryInOrderBeforeFirstData) {
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
    asyncdownload::flow::ProducerLane packet_lane;
    ASSERT_FALSE(
        packet_flow->producer().open_lane(packet_lane));
    asyncdownload::core::AtomicBlockBitmap bitmap(1);
    auto checkpoint = open_checkpoint(session);
    ASSERT_NE(checkpoint, nullptr);
    BS::thread_pool<> workers(1);
    asyncdownload::persistence::PersistenceThread persistence(
        session,
        policy,
        packet_flow->consumer(),
        bitmap,
        *checkpoint,
        workers,
        1);
    asyncdownload::range::RangeFactSlot facts({0}, 0);
    persistence.start();

    const auto registered = persistence.submit_range_geometry(
        asyncdownload::persistence::RangeGeometryCommand{
            asyncdownload::range::RegisterRangeEffect{
                {0},
                {0, 4096},
                0,
                facts.publisher()
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
    ASSERT_FALSE(registered.error);
    ASSERT_TRUE(observed_registration);
    ASSERT_TRUE(registration_ack.has_value());
    ASSERT_FALSE(registration_ack->error);

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
    ASSERT_FALSE(resized.error);
    ASSERT_TRUE(observed_resize);
    ASSERT_TRUE(resize_ack.has_value());
    ASSERT_FALSE(resize_ack->error);

    TestPacket packet{};
    packet.range_id = 0;
    packet.lease_span = {0, 2048};
    packet.offset = 0;
    packet.payload.assign(512, 0x4A);
    enqueue_data_packet(
        packet_flow->producer(),
        packet_lane,
        session,
        packet);
    ASSERT_TRUE(wait_for_condition([&facts]() {
        const auto snapshot = facts.read_since(0);
        return snapshot.has_value() &&
            snapshot->persisted_through == 512;
    }, std::chrono::milliseconds(1000)));

    const auto close_error = packet_flow->producer().close();
    persistence.stop();
    persistence.join();
    checkpoint->close_preserving_artifacts();

    EXPECT_FALSE(close_error);
    EXPECT_EQ(registered.ticket, 1U);
    EXPECT_EQ(resized.ticket, 2U);
    EXPECT_EQ(registration_ack->ticket, registered.ticket);
    EXPECT_EQ(registration_ack->range, (asyncdownload::range::RangeId{0}));
    EXPECT_EQ(registration_ack->geometry_revision, 0U);
    EXPECT_EQ(resize_ack->ticket, resized.ticket);
    EXPECT_EQ(resize_ack->range, (asyncdownload::range::RangeId{0}));
    EXPECT_EQ(resize_ack->geometry_revision, 1U);
    EXPECT_FALSE(persistence.error());
    const auto removed = std::filesystem::remove_all(
        temp_root,
        filesystem_error);
    static_cast<void>(removed);
}

} // namespace

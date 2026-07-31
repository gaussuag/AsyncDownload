#include <cstdint>
#include <limits>

#include <gtest/gtest.h>

#include "asyncdownload/error.hpp"
#include "download/progress_snapshot_builder.hpp"

namespace asyncdownload::download {
namespace {

TEST(ProgressSnapshotBuilderTest, UsesEachAuthoritativeSourceExactlyOnce) {
    ProgressSnapshotSources sources{};
    sources.total_bytes = 1'000;
    sources.recovery_trusted_bytes = 100;
    sources.persisted_bytes = 250;
    sources.vdl_offset = 200;
    sources.packet_flow.queued_packets = 7;
    sources.packet_flow.accounted_bytes = 800;
    sources.packet_flow.published_data_bytes = 300;
    sources.http.active_transfers = 3;
    sources.http.paused_transfers = 2;
    sources.telemetry.total_bytes = 91;
    sources.telemetry.downloaded_bytes = 92;
    sources.telemetry.persisted_bytes = 93;
    sources.telemetry.vdl_offset = 94;
    sources.telemetry.inflight_bytes = 95;
    sources.telemetry.queued_packets = 96;
    sources.telemetry.active_requests = 97;
    sources.telemetry.paused_ranges = 98;
    sources.telemetry.memory_bytes = 99;
    sources.telemetry.network_bytes_per_second = 12.5;
    sources.telemetry.disk_bytes_per_second = 8.25;
    sources.telemetry.resumed = false;
    sources.resumed = true;

    const auto result = build_progress_snapshot(sources);

    ASSERT_FALSE(result.error);
    EXPECT_EQ(result.snapshot.total_bytes, 1'000);
    EXPECT_EQ(result.snapshot.downloaded_bytes, 400);
    EXPECT_EQ(result.snapshot.persisted_bytes, 250);
    EXPECT_EQ(result.snapshot.vdl_offset, 200);
    EXPECT_EQ(result.snapshot.inflight_bytes, 150);
    EXPECT_EQ(result.snapshot.queued_packets, 7U);
    EXPECT_EQ(result.snapshot.active_requests, 3U);
    EXPECT_EQ(result.snapshot.paused_ranges, 2U);
    EXPECT_EQ(result.snapshot.memory_bytes, 800U);
    EXPECT_DOUBLE_EQ(result.snapshot.network_bytes_per_second, 12.5);
    EXPECT_DOUBLE_EQ(result.snapshot.disk_bytes_per_second, 8.25);
    EXPECT_TRUE(result.snapshot.resumed);
}

TEST(ProgressSnapshotBuilderTest, RestoredBaseDoesNotCreateFalseInflight) {
    ProgressSnapshotSources sources{};
    sources.total_bytes = 512;
    sources.recovery_trusted_bytes = 128;
    sources.persisted_bytes = 128;
    sources.vdl_offset = 128;
    sources.resumed = true;

    const auto result = build_progress_snapshot(sources);

    ASSERT_FALSE(result.error);
    EXPECT_EQ(result.snapshot.downloaded_bytes, 128);
    EXPECT_EQ(result.snapshot.persisted_bytes, 128);
    EXPECT_EQ(result.snapshot.inflight_bytes, 0);
}

TEST(ProgressSnapshotBuilderTest, ClampsTransientPersistedLeadToZeroInflight) {
    ProgressSnapshotSources sources{};
    sources.total_bytes = 512;
    sources.recovery_trusted_bytes = 128;
    sources.persisted_bytes = 256;
    sources.packet_flow.published_data_bytes = 64;

    const auto result = build_progress_snapshot(sources);

    ASSERT_FALSE(result.error);
    EXPECT_EQ(result.snapshot.downloaded_bytes, 192);
    EXPECT_EQ(result.snapshot.persisted_bytes, 256);
    EXPECT_EQ(result.snapshot.inflight_bytes, 0);
}

TEST(ProgressSnapshotBuilderTest, RejectsDownloadedByteOverflow) {
    ProgressSnapshotSources sources{};
    sources.total_bytes = std::numeric_limits<std::int64_t>::max();
    sources.recovery_trusted_bytes =
        std::numeric_limits<std::int64_t>::max();
    sources.packet_flow.published_data_bytes = 1;

    const auto result = build_progress_snapshot(sources);

    EXPECT_EQ(result.error,
        make_error_code(DownloadErrc::internal_error));
}

}
}

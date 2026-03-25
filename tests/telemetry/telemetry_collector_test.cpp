#include "asyncdownload/telemetry/telemetry_collector.hpp"

#include <chrono>

#include <gtest/gtest.h>

namespace {

using asyncdownload::telemetry::TelemetryCollector;
using asyncdownload::telemetry::TelemetryClock;
using asyncdownload::telemetry::TelemetryPauseReason;

class TelemetryCollectorTest : public ::testing::Test {
protected:
    TelemetryCollector collector_{};
};

TEST_F(TelemetryCollectorTest, ComputesTtfbAndIgnoresDuplicates) {
    collector_.record_task_started(TelemetryClock::time_point{std::chrono::seconds(1)});
    collector_.record_first_byte_received(TelemetryClock::time_point{std::chrono::milliseconds(1250)});
    collector_.record_first_byte_received(TelemetryClock::time_point{std::chrono::milliseconds(1400)});

    const auto summary = collector_.final_summary();
    EXPECT_EQ(summary.time_to_first_byte_ms, 250);
}

TEST_F(TelemetryCollectorTest, TracksPacketStatisticsAndAveragePacketSize) {
    collector_.record_task_started(TelemetryClock::time_point{std::chrono::seconds(1)});
    collector_.record_download_delta(100U, TelemetryClock::time_point{std::chrono::milliseconds(1100)});
    collector_.record_download_delta(200U, TelemetryClock::time_point{std::chrono::milliseconds(1200)});
    collector_.record_download_delta(150U, TelemetryClock::time_point{std::chrono::milliseconds(1300)});

    const auto summary = collector_.final_summary();
    EXPECT_EQ(summary.packets_enqueued_total, 3U);
    EXPECT_EQ(summary.max_packet_size_bytes, 200U);
    EXPECT_DOUBLE_EQ(summary.average_packet_size_bytes, 150.0);
}

TEST_F(TelemetryCollectorTest, ComputesEmaSpeedsForCurrentSnapshotAndAveragesForSummary) {
    collector_.record_task_started(TelemetryClock::time_point{std::chrono::seconds(1)});
    collector_.record_download_delta(100U, TelemetryClock::time_point{std::chrono::seconds(2)});
    collector_.record_download_delta(200U, TelemetryClock::time_point{std::chrono::seconds(3)});
    collector_.record_persist_delta(80U, TelemetryClock::time_point{std::chrono::milliseconds(2500)});
    collector_.record_persist_delta(120U, TelemetryClock::time_point{std::chrono::seconds(4)});
    collector_.record_task_completed(TelemetryClock::time_point{std::chrono::seconds(5)});

    const auto snapshot = collector_.current_snapshot();
    EXPECT_DOUBLE_EQ(snapshot.network_bytes_per_second, 200.0);
    EXPECT_DOUBLE_EQ(snapshot.disk_bytes_per_second, 80.0);
    EXPECT_GT(snapshot.watermark_timestamp_ns, 0);

    const auto summary = collector_.final_summary();
    EXPECT_DOUBLE_EQ(summary.average_network_bytes_per_second, 75.0);
    EXPECT_DOUBLE_EQ(summary.average_disk_bytes_per_second, 50.0);
}

TEST_F(TelemetryCollectorTest, TracksPeakMemoryAndInflightBytes) {
    collector_.record_task_started(TelemetryClock::time_point{std::chrono::seconds(1)});
    collector_.record_download_delta(300U, TelemetryClock::time_point{std::chrono::milliseconds(1100)});
    collector_.record_memory_sample(1024U, TelemetryClock::time_point{std::chrono::milliseconds(1150)});
    collector_.record_persist_delta(100U, TelemetryClock::time_point{std::chrono::milliseconds(1200)});
    collector_.record_download_delta(200U, TelemetryClock::time_point{std::chrono::milliseconds(1300)});
    collector_.record_memory_sample(2048U, TelemetryClock::time_point{std::chrono::milliseconds(1350)});

    const auto snapshot = collector_.current_snapshot();
    EXPECT_EQ(snapshot.downloaded_bytes, 500);
    EXPECT_EQ(snapshot.persisted_bytes, 100);
    EXPECT_EQ(snapshot.inflight_bytes, 400);
    EXPECT_EQ(snapshot.memory_bytes, 2048U);

    const auto summary = collector_.final_summary();
    EXPECT_EQ(summary.max_memory_bytes, 2048U);
    EXPECT_EQ(summary.max_inflight_bytes, 400);
}

TEST_F(TelemetryCollectorTest, TracksPauseCounts) {
    collector_.record_task_started(TelemetryClock::time_point{std::chrono::seconds(1)});
    collector_.record_pause(TelemetryPauseReason::queue_full,
        true,
        TelemetryClock::time_point{std::chrono::milliseconds(1100)});
    collector_.record_pause(TelemetryPauseReason::gap,
        false,
        TelemetryClock::time_point{std::chrono::milliseconds(1200)});
    collector_.record_pause(TelemetryPauseReason::queue_full,
        false,
        TelemetryClock::time_point{std::chrono::milliseconds(1300)});

    const auto summary = collector_.final_summary();
    EXPECT_EQ(summary.total_pause_count, 3U);
    EXPECT_EQ(summary.queue_full_pause_count, 2U);
}

TEST_F(TelemetryCollectorTest, IgnoresUpdatesAfterTaskCompleted) {
    collector_.record_task_started(TelemetryClock::time_point{std::chrono::seconds(1)});
    collector_.record_download_delta(120U, TelemetryClock::time_point{std::chrono::milliseconds(1100)});
    collector_.record_task_completed(TelemetryClock::time_point{std::chrono::seconds(2)});
    collector_.record_download_delta(999U, TelemetryClock::time_point{std::chrono::seconds(3)});

    const auto summary = collector_.final_summary();
    EXPECT_EQ(summary.packets_enqueued_total, 1U);
    EXPECT_EQ(summary.max_packet_size_bytes, 120U);
}

TEST_F(TelemetryCollectorTest, UsesProvidedNowForIncompleteTaskSummary) {
    collector_.record_task_started(TelemetryClock::time_point{std::chrono::seconds(1)});
    collector_.record_download_delta(100U, TelemetryClock::time_point{std::chrono::seconds(2)});

    const auto summary = collector_.final_summary(TelemetryClock::time_point{std::chrono::seconds(5)});
    EXPECT_DOUBLE_EQ(summary.average_network_bytes_per_second, 25.0);
    EXPECT_DOUBLE_EQ(summary.average_disk_bytes_per_second, 0.0);
}

} // namespace

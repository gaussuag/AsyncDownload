#include "asyncdownload/telemetry/telemetry_collector.hpp"
#include "asyncdownload/telemetry/telemetry_sink.hpp"

#include <gtest/gtest.h>

namespace {

using asyncdownload::telemetry::TelemetryCollector;
using asyncdownload::telemetry::TelemetryCompletionStatus;
using asyncdownload::telemetry::TelemetryEvent;
using asyncdownload::telemetry::TelemetryEventType;
using asyncdownload::telemetry::TelemetryPauseReason;
using asyncdownload::telemetry::TelemetryPayload;
using asyncdownload::telemetry::TelemetrySink;

class TelemetryCollectorTest : public ::testing::Test {
protected:
    TelemetrySink sink_{};
    TelemetryCollector collector_{sink_};

    void SetUp() override {
        collector_.start_consuming();
    }

    void TearDown() override {
        collector_.wait_until_drained();
        collector_.stop_consuming();
    }

    void enqueue(const TelemetryEvent& event) {
        ASSERT_TRUE(sink_.enqueue(event));
    }

    void drain() {
        collector_.wait_until_drained();
    }
};

TEST_F(TelemetryCollectorTest, ComputesTtfbAndIgnoresDuplicates) {
    enqueue(TelemetryEvent{
        TelemetryEventType::task_started,
        1'000'000'000U,
        TelemetryPayload::task_started_payload(),
    });
    enqueue(TelemetryEvent{
        TelemetryEventType::first_byte_received,
        1'250'000'000U,
        TelemetryPayload::first_byte_received_payload(),
    });
    enqueue(TelemetryEvent{
        TelemetryEventType::first_byte_received,
        1'400'000'000U,
        TelemetryPayload::first_byte_received_payload(),
    });

    drain();

    const auto summary = collector_.final_summary();
    EXPECT_EQ(summary.time_to_first_byte_ms, 250);
}

TEST_F(TelemetryCollectorTest, TracksPacketStatisticsAndAveragePacketSize) {
    enqueue(TelemetryEvent{
        TelemetryEventType::task_started,
        1'000'000'000U,
        TelemetryPayload::task_started_payload(),
    });
    enqueue(TelemetryEvent{
        TelemetryEventType::download_delta,
        1'100'000'000U,
        TelemetryPayload::download_delta_payload(100U),
    });
    enqueue(TelemetryEvent{
        TelemetryEventType::download_delta,
        1'200'000'000U,
        TelemetryPayload::download_delta_payload(200U),
    });
    enqueue(TelemetryEvent{
        TelemetryEventType::download_delta,
        1'300'000'000U,
        TelemetryPayload::download_delta_payload(150U),
    });

    drain();

    const auto summary = collector_.final_summary();
    EXPECT_EQ(summary.packets_enqueued_total, 3U);
    EXPECT_EQ(summary.max_packet_size_bytes, 200U);
    EXPECT_DOUBLE_EQ(summary.average_packet_size_bytes, 150.0);
}

TEST_F(TelemetryCollectorTest, ComputesEmaSpeedsForCurrentSnapshotAndAveragesForSummary) {
    enqueue(TelemetryEvent{
        TelemetryEventType::task_started,
        1'000'000'000U,
        TelemetryPayload::task_started_payload(),
    });
    enqueue(TelemetryEvent{
        TelemetryEventType::download_delta,
        2'000'000'000U,
        TelemetryPayload::download_delta_payload(100U),
    });
    enqueue(TelemetryEvent{
        TelemetryEventType::download_delta,
        3'000'000'000U,
        TelemetryPayload::download_delta_payload(200U),
    });
    enqueue(TelemetryEvent{
        TelemetryEventType::persist_delta,
        2'500'000'000U,
        TelemetryPayload::persist_delta_payload(80U),
    });
    enqueue(TelemetryEvent{
        TelemetryEventType::persist_delta,
        4'000'000'000U,
        TelemetryPayload::persist_delta_payload(120U),
    });
    enqueue(TelemetryEvent{
        TelemetryEventType::task_completed,
        5'000'000'000U,
        TelemetryPayload::task_completed_payload(TelemetryCompletionStatus::success),
    });

    drain();

    const auto snapshot = collector_.current_snapshot();
    EXPECT_DOUBLE_EQ(snapshot.network_bytes_per_second, 200.0);
    EXPECT_DOUBLE_EQ(snapshot.disk_bytes_per_second, 80.0);
    EXPECT_GT(snapshot.watermark_timestamp_ns, 0);

    const auto summary = collector_.final_summary();
    EXPECT_DOUBLE_EQ(summary.average_network_bytes_per_second, 75.0);
    EXPECT_DOUBLE_EQ(summary.average_disk_bytes_per_second, 50.0);
}

TEST_F(TelemetryCollectorTest, TracksPeakMemoryAndInflightBytes) {
    enqueue(TelemetryEvent{
        TelemetryEventType::task_started,
        1'000'000'000U,
        TelemetryPayload::task_started_payload(),
    });
    enqueue(TelemetryEvent{
        TelemetryEventType::download_delta,
        1'100'000'000U,
        TelemetryPayload::download_delta_payload(300U),
    });
    enqueue(TelemetryEvent{
        TelemetryEventType::memory_sample,
        1'150'000'000U,
        TelemetryPayload::memory_sample_payload(1024U),
    });
    enqueue(TelemetryEvent{
        TelemetryEventType::persist_delta,
        1'200'000'000U,
        TelemetryPayload::persist_delta_payload(100U),
    });
    enqueue(TelemetryEvent{
        TelemetryEventType::download_delta,
        1'300'000'000U,
        TelemetryPayload::download_delta_payload(200U),
    });
    enqueue(TelemetryEvent{
        TelemetryEventType::memory_sample,
        1'350'000'000U,
        TelemetryPayload::memory_sample_payload(2048U),
    });

    drain();

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
    enqueue(TelemetryEvent{
        TelemetryEventType::task_started,
        1'000'000'000U,
        TelemetryPayload::task_started_payload(),
    });
    enqueue(TelemetryEvent{
        TelemetryEventType::queue_paused,
        1'100'000'000U,
        TelemetryPayload::queue_paused_payload(TelemetryPauseReason::queue_full, true),
    });
    enqueue(TelemetryEvent{
        TelemetryEventType::queue_paused,
        1'200'000'000U,
        TelemetryPayload::queue_paused_payload(TelemetryPauseReason::gap, false),
    });
    enqueue(TelemetryEvent{
        TelemetryEventType::queue_paused,
        1'300'000'000U,
        TelemetryPayload::queue_paused_payload(TelemetryPauseReason::queue_full, false),
    });

    drain();

    const auto summary = collector_.final_summary();
    EXPECT_EQ(summary.total_pause_count, 3U);
    EXPECT_EQ(summary.queue_full_pause_count, 2U);
}

TEST_F(TelemetryCollectorTest, IgnoresEventsAfterTaskCompleted) {
    enqueue(TelemetryEvent{
        TelemetryEventType::task_started,
        1'000'000'000U,
        TelemetryPayload::task_started_payload(),
    });
    enqueue(TelemetryEvent{
        TelemetryEventType::download_delta,
        1'100'000'000U,
        TelemetryPayload::download_delta_payload(120U),
    });
    enqueue(TelemetryEvent{
        TelemetryEventType::task_completed,
        2'000'000'000U,
        TelemetryPayload::task_completed_payload(TelemetryCompletionStatus::success),
    });
    enqueue(TelemetryEvent{
        TelemetryEventType::download_delta,
        3'000'000'000U,
        TelemetryPayload::download_delta_payload(999U),
    });

    drain();

    const auto summary = collector_.final_summary();
    EXPECT_EQ(summary.packets_enqueued_total, 1U);
    EXPECT_EQ(summary.max_packet_size_bytes, 120U);
}

} // namespace

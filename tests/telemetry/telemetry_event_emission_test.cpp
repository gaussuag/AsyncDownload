#include "core/models.hpp"

#include <gtest/gtest.h>

namespace {

TEST(TelemetryEventEmissionTest, SessionTelemetryProducesSummaryWithoutMutatingRuntimeMetrics) {
    asyncdownload::core::SessionState session{};

    session.telemetry_session_.record_task_started();
    session.telemetry_session_.record_first_byte_received();
    session.telemetry_session_.record_download_delta(1024U);
    session.telemetry_session_.record_download_delta(2048U);
    session.telemetry_session_.record_persist_delta(1024U);
    session.telemetry_session_.record_pause(
        asyncdownload::telemetry::TelemetryPauseReason::queue_full, true);
    session.telemetry_session_.record_pause(
        asyncdownload::telemetry::TelemetryPauseReason::memory_pressure, false);
    session.telemetry_session_.record_memory_sample(8192U);
    session.telemetry_session_.record_task_completed();

    const auto summary = session.telemetry_session_.final_summary();

    EXPECT_EQ(session.performance_metrics.total_pause_count.load(std::memory_order_relaxed), 0U);
    EXPECT_EQ(session.performance_metrics.queue_full_pause_count.load(std::memory_order_relaxed), 0U);
    EXPECT_EQ(session.performance_metrics.max_memory_bytes.load(std::memory_order_relaxed), 0U);
    EXPECT_EQ(session.performance_metrics.max_inflight_bytes.load(std::memory_order_relaxed), 0);
    EXPECT_EQ(session.performance_metrics.packets_enqueued_total.load(std::memory_order_relaxed), 0U);
    EXPECT_EQ(session.performance_metrics.max_packet_size_bytes.load(std::memory_order_relaxed), 0U);

    EXPECT_EQ(summary.total_pause_count, 2U);
    EXPECT_EQ(summary.queue_full_pause_count, 1U);
    EXPECT_EQ(summary.max_memory_bytes, 8192U);
    EXPECT_EQ(summary.packets_enqueued_total, 2U);
    EXPECT_EQ(summary.max_packet_size_bytes, 2048U);
    EXPECT_DOUBLE_EQ(summary.average_packet_size_bytes, 1536.0);
}

TEST(TelemetryEventEmissionTest, SessionTelemetrySnapshotAdvancesFromEvents) {
    asyncdownload::core::SessionState session{};

    session.telemetry_session_.record_task_started();
    session.telemetry_session_.record_download_delta(4096U);
    session.telemetry_session_.record_persist_delta(1024U);
    session.telemetry_session_.record_memory_sample(16384U);
    const auto summary = session.telemetry_session_.final_summary();
    EXPECT_EQ(summary.packets_enqueued_total, 1U);

    const auto snapshot = session.telemetry_session_.current_snapshot();

    EXPECT_EQ(snapshot.downloaded_bytes, 4096);
    EXPECT_EQ(snapshot.persisted_bytes, 1024);
    EXPECT_EQ(snapshot.inflight_bytes, 3072);
    EXPECT_EQ(snapshot.memory_bytes, 16384U);
    EXPECT_GT(snapshot.watermark_timestamp_ns, 0);
}

} // namespace

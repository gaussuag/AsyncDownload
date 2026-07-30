#include <chrono>

#include <gtest/gtest.h>

#include "asyncdownload/telemetry/telemetry_session.hpp"

namespace {

using asyncdownload::telemetry::TelemetryClock;
using asyncdownload::telemetry::TelemetryPauseReason;
using asyncdownload::telemetry::TelemetrySession;

TEST(TelemetrySessionTest, DefaultsEveryFormalSummaryFieldToZero) {
    const TelemetrySession session;
    const auto summary = session.final_summary();

    EXPECT_DOUBLE_EQ(summary.average_network_bytes_per_second, 0.0);
    EXPECT_DOUBLE_EQ(summary.average_disk_bytes_per_second, 0.0);
    EXPECT_EQ(summary.time_to_first_byte_ms, 0);
    EXPECT_EQ(summary.max_memory_bytes, 0U);
    EXPECT_EQ(summary.max_inflight_bytes, 0);
    EXPECT_EQ(summary.total_pause_count, 0U);
    EXPECT_EQ(summary.queue_full_pause_count, 0U);
    EXPECT_EQ(summary.packets_enqueued_total, 0U);
    EXPECT_DOUBLE_EQ(summary.average_packet_size_bytes, 0.0);
    EXPECT_EQ(summary.max_packet_size_bytes, 0U);
}

TEST(TelemetrySessionTest, IgnoresUpdatesBeforeTaskStart) {
    TelemetrySession session;

    session.record_first_byte_received(TelemetryClock::time_point{});
    session.record_download_delta(100U);
    session.record_persist_delta(80U);
    session.record_pause(TelemetryPauseReason::queue_full, true);
    session.record_memory_sample(4096U);
    session.record_task_completed(TelemetryClock::time_point{});

    const auto snapshot = session.current_snapshot();
    const auto summary = session.final_summary();
    EXPECT_EQ(snapshot.downloaded_bytes, 0);
    EXPECT_EQ(snapshot.persisted_bytes, 0);
    EXPECT_EQ(snapshot.memory_bytes, 0U);
    EXPECT_EQ(summary.packets_enqueued_total, 0U);
    EXPECT_EQ(summary.total_pause_count, 0U);
}

TEST(TelemetrySessionTest, KeepsEarliestFirstByteAndClampsCompletion) {
    TelemetrySession session;
    const auto start = TelemetryClock::time_point{std::chrono::seconds(1)};

    session.record_task_started(start);
    session.record_first_byte_received(start + std::chrono::seconds(4));
    session.record_first_byte_received(start + std::chrono::seconds(6));
    session.record_first_byte_received(start + std::chrono::seconds(2));
    session.record_task_completed(start + std::chrono::seconds(3));

    const auto summary =
        session.final_summary(start + std::chrono::seconds(100));
    EXPECT_EQ(summary.time_to_first_byte_ms, 2000);
}

TEST(TelemetrySessionTest, UsesSuppliedNowForIncompleteTaskAverage) {
    TelemetrySession session;
    const auto start = TelemetryClock::now();

    session.record_task_started(start);
    session.record_download_delta(100U);
    session.record_persist_delta(80U);

    const auto summary =
        session.final_summary(start + std::chrono::seconds(4));
    EXPECT_DOUBLE_EQ(summary.average_network_bytes_per_second, 25.0);
    EXPECT_DOUBLE_EQ(summary.average_disk_bytes_per_second, 20.0);
}

TEST(TelemetrySessionTest, CompletedSummaryIgnoresLaterQueryTime) {
    TelemetrySession session;
    const auto start = TelemetryClock::now();

    session.record_task_started(start);
    session.record_download_delta(300U);
    session.record_persist_delta(200U);
    session.record_task_completed(start + std::chrono::seconds(4));

    const auto first =
        session.final_summary(start + std::chrono::seconds(10));
    const auto second =
        session.final_summary(start + std::chrono::seconds(100));
    EXPECT_DOUBLE_EQ(first.average_network_bytes_per_second, 75.0);
    EXPECT_DOUBLE_EQ(first.average_disk_bytes_per_second, 50.0);
    EXPECT_DOUBLE_EQ(
        second.average_network_bytes_per_second,
        first.average_network_bytes_per_second);
    EXPECT_DOUBLE_EQ(
        second.average_disk_bytes_per_second,
        first.average_disk_bytes_per_second);
}

TEST(TelemetrySessionTest, TracksObservedSnapshotPeaksAndPacketShape) {
    TelemetrySession session;
    session.record_task_started();

    session.record_download_delta(100U);
    session.record_download_delta(200U);
    session.record_persist_delta(80U);
    session.record_memory_sample(1024U);
    session.record_memory_sample(512U);
    session.record_pause(TelemetryPauseReason::queue_full, false);
    session.record_pause(TelemetryPauseReason::gap, true);
    session.record_pause(TelemetryPauseReason::memory_pressure, false);

    const auto snapshot = session.current_snapshot();
    const auto summary = session.final_summary();
    EXPECT_EQ(snapshot.downloaded_bytes, 300);
    EXPECT_EQ(snapshot.persisted_bytes, 80);
    EXPECT_EQ(snapshot.inflight_bytes, 220);
    EXPECT_EQ(snapshot.memory_bytes, 512U);
    EXPECT_EQ(summary.max_memory_bytes, 1024U);
    EXPECT_EQ(summary.max_inflight_bytes, 300);
    EXPECT_EQ(summary.total_pause_count, 3U);
    EXPECT_EQ(summary.queue_full_pause_count, 2U);
    EXPECT_EQ(summary.packets_enqueued_total, 2U);
    EXPECT_DOUBLE_EQ(summary.average_packet_size_bytes, 150.0);
    EXPECT_EQ(summary.max_packet_size_bytes, 200U);
}

TEST(TelemetrySessionTest, CountsZeroBytePacketForCompatibility) {
    TelemetrySession session;
    session.record_task_started();

    session.record_download_delta(0U);

    const auto summary = session.final_summary();
    EXPECT_EQ(summary.packets_enqueued_total, 1U);
    EXPECT_DOUBLE_EQ(summary.average_packet_size_bytes, 0.0);
    EXPECT_EQ(summary.max_packet_size_bytes, 0U);
}

TEST(TelemetrySessionTest, IgnoresUpdatesAndDuplicateCompletionAfterCompletion) {
    TelemetrySession session;
    const auto start = TelemetryClock::now();

    session.record_task_started(start);
    session.record_download_delta(100U);
    session.record_task_completed(start + std::chrono::seconds(2));
    session.record_task_completed(start + std::chrono::seconds(20));
    session.record_download_delta(900U);
    session.record_persist_delta(900U);
    session.record_pause(TelemetryPauseReason::gap, false);
    session.record_memory_sample(8192U);

    const auto summary =
        session.final_summary(start + std::chrono::seconds(100));
    EXPECT_EQ(summary.packets_enqueued_total, 1U);
    EXPECT_EQ(summary.max_packet_size_bytes, 100U);
    EXPECT_EQ(summary.total_pause_count, 0U);
    EXPECT_EQ(summary.max_memory_bytes, 0U);
    EXPECT_DOUBLE_EQ(summary.average_network_bytes_per_second, 50.0);
}

TEST(TelemetrySessionTest, SecondStartBeginsCleanObservationEpoch) {
    TelemetrySession session;
    session.record_task_started();
    session.record_first_byte_received();
    session.record_download_delta(100U);
    session.record_persist_delta(50U);
    session.record_pause(TelemetryPauseReason::gap, false);
    session.record_memory_sample(4096U);

    session.record_task_started();

    const auto snapshot = session.current_snapshot();
    const auto summary = session.final_summary();
    EXPECT_EQ(snapshot.downloaded_bytes, 0);
    EXPECT_EQ(snapshot.persisted_bytes, 0);
    EXPECT_EQ(snapshot.inflight_bytes, 0);
    EXPECT_EQ(snapshot.memory_bytes, 0U);
    EXPECT_EQ(summary.time_to_first_byte_ms, 0);
    EXPECT_EQ(summary.max_memory_bytes, 0U);
    EXPECT_EQ(summary.max_inflight_bytes, 0);
    EXPECT_EQ(summary.total_pause_count, 0U);
    EXPECT_EQ(summary.packets_enqueued_total, 0U);
}

}

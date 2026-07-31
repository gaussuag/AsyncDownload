#include <chrono>

#include <gtest/gtest.h>

#include "asyncdownload/telemetry/telemetry_collector.hpp"
#include "asyncdownload/telemetry/telemetry_session.hpp"

namespace {

using asyncdownload::PerformanceSummary;
using asyncdownload::ProgressSnapshot;
using asyncdownload::telemetry::TelemetryClock;
using asyncdownload::telemetry::TelemetryCollector;
using asyncdownload::telemetry::TelemetryPauseReason;
using asyncdownload::telemetry::TelemetrySession;

void expect_equal(
    const ProgressSnapshot& lhs,
    const ProgressSnapshot& rhs) {
    EXPECT_EQ(lhs.total_bytes, rhs.total_bytes);
    EXPECT_EQ(lhs.downloaded_bytes, rhs.downloaded_bytes);
    EXPECT_EQ(lhs.persisted_bytes, rhs.persisted_bytes);
    EXPECT_EQ(lhs.vdl_offset, rhs.vdl_offset);
    EXPECT_EQ(lhs.inflight_bytes, rhs.inflight_bytes);
    EXPECT_EQ(lhs.queued_packets, rhs.queued_packets);
    EXPECT_EQ(lhs.active_requests, rhs.active_requests);
    EXPECT_EQ(lhs.paused_ranges, rhs.paused_ranges);
    EXPECT_EQ(lhs.memory_bytes, rhs.memory_bytes);
    EXPECT_DOUBLE_EQ(
        lhs.network_bytes_per_second,
        rhs.network_bytes_per_second);
    EXPECT_DOUBLE_EQ(
        lhs.disk_bytes_per_second,
        rhs.disk_bytes_per_second);
    EXPECT_EQ(lhs.resumed, rhs.resumed);
}

void expect_equal(
    const PerformanceSummary& lhs,
    const PerformanceSummary& rhs) {
    EXPECT_DOUBLE_EQ(
        lhs.average_network_bytes_per_second,
        rhs.average_network_bytes_per_second);
    EXPECT_DOUBLE_EQ(
        lhs.average_disk_bytes_per_second,
        rhs.average_disk_bytes_per_second);
    EXPECT_EQ(
        lhs.time_to_first_byte_ms,
        rhs.time_to_first_byte_ms);
    EXPECT_EQ(lhs.max_memory_bytes, rhs.max_memory_bytes);
    EXPECT_EQ(lhs.max_inflight_bytes, rhs.max_inflight_bytes);
    EXPECT_EQ(lhs.total_pause_count, rhs.total_pause_count);
    EXPECT_EQ(
        lhs.queue_full_pause_count,
        rhs.queue_full_pause_count);
    EXPECT_EQ(
        lhs.packets_enqueued_total,
        rhs.packets_enqueued_total);
    EXPECT_DOUBLE_EQ(
        lhs.average_packet_size_bytes,
        rhs.average_packet_size_bytes);
    EXPECT_EQ(
        lhs.max_packet_size_bytes,
        rhs.max_packet_size_bytes);
}

TEST(
    TelemetryCollectorCompatibilityTest,
    ForwardsEveryExplicitTimestampMethodLikeSession) {
    TelemetryCollector collector;
    TelemetrySession session;
    const auto start =
        TelemetryClock::time_point{std::chrono::seconds(1)};
    const auto first_byte =
        start + std::chrono::milliseconds(250);
    const auto download_at =
        start + std::chrono::milliseconds(500);
    const auto persist_at =
        start + std::chrono::milliseconds(750);
    const auto pause_at =
        start + std::chrono::seconds(1);
    const auto memory_at =
        start + std::chrono::milliseconds(1250);
    const auto completed_at =
        start + std::chrono::seconds(2);

    collector.record_task_started(start);
    session.record_task_started(start);
    collector.record_first_byte_received(first_byte);
    session.record_first_byte_received(first_byte);
    collector.record_download_delta(300, download_at);
    session.record_download_delta_at(300, download_at);
    collector.record_persist_delta(100, persist_at);
    session.record_persist_delta_at(100, persist_at);
    collector.record_pause(
        TelemetryPauseReason::queue_full,
        true,
        pause_at);
    session.record_pause_at(
        TelemetryPauseReason::queue_full,
        true,
        pause_at);
    collector.record_memory_sample(2048, memory_at);
    session.record_memory_sample_at(2048, memory_at);
    collector.record_task_completed(completed_at);
    session.record_task_completed(completed_at);

    expect_equal(
        collector.current_snapshot(),
        session.current_snapshot());
    expect_equal(
        collector.final_summary(completed_at),
        session.final_summary(completed_at));
}

TEST(
    TelemetryCollectorCompatibilityTest,
    ForwardsCompletionIgnoreAndStartReset) {
    TelemetryCollector collector;
    const auto start =
        TelemetryClock::time_point{std::chrono::seconds(1)};
    collector.record_task_started(start);
    collector.record_download_delta(
        100,
        start + std::chrono::seconds(1));
    collector.record_task_completed(
        start + std::chrono::seconds(2));
    collector.record_download_delta(
        900,
        start + std::chrono::seconds(3));

    EXPECT_EQ(
        collector.final_summary().packets_enqueued_total,
        1U);

    collector.record_task_started(
        start + std::chrono::seconds(4));
    const auto snapshot = collector.current_snapshot();
    const auto summary = collector.final_summary(
        start + std::chrono::seconds(5));
    EXPECT_EQ(snapshot.downloaded_bytes, 0);
    EXPECT_EQ(snapshot.persisted_bytes, 0);
    EXPECT_EQ(summary.packets_enqueued_total, 0U);
    EXPECT_EQ(summary.total_pause_count, 0U);
}

TEST(
    TelemetryCollectorCompatibilityTest,
    DoesNotShareStateWithIndependentSession) {
    TelemetryCollector collector;
    TelemetrySession session;
    const auto start =
        TelemetryClock::time_point{std::chrono::seconds(1)};
    collector.record_task_started(start);
    session.record_task_started(start);
    collector.record_download_delta(
        128,
        start + std::chrono::seconds(1));
    collector.record_pause(
        TelemetryPauseReason::gap,
        false,
        start + std::chrono::seconds(1));

    EXPECT_EQ(
        collector.final_summary().packets_enqueued_total,
        1U);
    EXPECT_EQ(
        collector.final_summary().total_pause_count,
        1U);
    EXPECT_EQ(
        session.final_summary().packets_enqueued_total,
        0U);
    EXPECT_EQ(
        session.final_summary().total_pause_count,
        0U);
}

}

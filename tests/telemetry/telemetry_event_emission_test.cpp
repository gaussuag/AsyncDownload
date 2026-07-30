#include <chrono>
#include <cstdlib>
#include <utility>

#include <gtest/gtest.h>

#include "download/download_policy.hpp"
#include "core/models.hpp"

namespace {

asyncdownload::download::EffectiveDownloadPolicy make_effective_policy() {
    const auto validated =
        asyncdownload::download::validate_download_options(
            asyncdownload::DownloadOptions{});
    if (!validated.ok()) {
        std::abort();
    }
    auto effective = asyncdownload::download::bind_remote_facts(
        *validated.value,
        {1, true});
    if (!effective.ok()) {
        std::abort();
    }
    return std::move(*effective.value);
}

TEST(TelemetryEventEmissionTest, SessionTelemetryProducesSummaryFromEventStream) {
    asyncdownload::core::SessionState session(make_effective_policy());

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
    const auto snapshot = session.telemetry_session_.current_snapshot();

    EXPECT_EQ(snapshot.downloaded_bytes, 3072);
    EXPECT_EQ(snapshot.persisted_bytes, 1024);
    EXPECT_EQ(snapshot.inflight_bytes, 2048);
    EXPECT_EQ(snapshot.memory_bytes, 8192U);
    EXPECT_EQ(summary.total_pause_count, 2U);
    EXPECT_EQ(summary.queue_full_pause_count, 1U);
    EXPECT_EQ(summary.max_memory_bytes, 8192U);
    EXPECT_EQ(summary.packets_enqueued_total, 2U);
    EXPECT_EQ(summary.max_packet_size_bytes, 2048U);
    EXPECT_DOUBLE_EQ(summary.average_packet_size_bytes, 1536.0);
}

TEST(TelemetryEventEmissionTest, SessionTelemetryUsesExplicitTaskTimestampsForAverages) {
    asyncdownload::core::SessionState session(make_effective_policy());

    const auto started_at = asyncdownload::telemetry::TelemetryClock::now();
    const auto first_byte_at = started_at + std::chrono::milliseconds(250);
    const auto completed_at = started_at + std::chrono::seconds(4);

    session.telemetry_session_.record_task_started(started_at);
    session.telemetry_session_.record_first_byte_received(first_byte_at);
    session.telemetry_session_.record_download_delta(300U);
    session.telemetry_session_.record_persist_delta(300U);
    session.telemetry_session_.record_task_completed(completed_at);

    const auto summary = session.telemetry_session_.final_summary();

    EXPECT_EQ(summary.time_to_first_byte_ms, 250);
    EXPECT_DOUBLE_EQ(summary.average_network_bytes_per_second, 75.0);
    EXPECT_DOUBLE_EQ(summary.average_disk_bytes_per_second, 75.0);
}

TEST(TelemetryEventEmissionTest, SessionTelemetrySnapshotAdvancesFromEvents) {
    asyncdownload::core::SessionState session(make_effective_policy());

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
}

} // namespace
